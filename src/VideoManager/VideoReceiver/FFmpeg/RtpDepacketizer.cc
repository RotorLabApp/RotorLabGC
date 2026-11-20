/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "RtpDepacketizer.h"

#include <QtCore/QVarLengthArray>

#include <algorithm>
#include <utility>

namespace {

constexpr char kStartCode[4] = { 0, 0, 0, 1 };
constexpr int kStartCodeBytes = 4;
constexpr int kRtpHeaderBytes = 12;

// Access unit bounds accepted from callers (tests use small ones)
constexpr int kMinAccessUnitBound = 64;
constexpr int kMaxAccessUnitBound = 256 * 1024 * 1024;

inline quint8 u8(const char *p, int i)
{
    return static_cast<quint8>(p[i]);
}

inline quint16 be16(const char *p)
{
    return static_cast<quint16>((u8(p, 0) << 8) | u8(p, 1));
}

inline quint32 be32(const char *p)
{
    return (static_cast<quint32>(u8(p, 0)) << 24) | (static_cast<quint32>(u8(p, 1)) << 16) |
           (static_cast<quint32>(u8(p, 2)) << 8) | static_cast<quint32>(u8(p, 3));
}

} // namespace

RtpDepacketizer::RtpDepacketizer(Codec codec, int payloadType, int maxAccessUnitBytes)
    : _codec(codec)
    , _payloadType(payloadType & 0x7f)
    , _maxAccessUnitBytes(qBound(kMinAccessUnitBound, maxAccessUnitBytes, kMaxAccessUnitBound))
    , _parameterSets((codec == Codec::H265) ? VideoTapCodec::H265 : VideoTapCodec::H264)
{
    setReorderWindow(kDefaultReorderPackets, kDefaultReorderHoldNs);
}

void RtpDepacketizer::setReorderWindow(int packets, qint64 holdNs)
{
    _capacity = qBound(1, packets, kMaxReorderPackets);
    _holdNs = std::clamp(holdNs, qint64(0), kMaxReorderHoldNs);
    _window.assign(static_cast<size_t>(_capacity), Held());
    _history.assign(static_cast<size_t>(_capacity), -1);
    _heldCount = 0;
    _highestHeld = 0;
    _endStartLog();     // its copies assumed the old window
}

void RtpDepacketizer::_clearWindow()
{
    if (_heldCount > 0) {
        for (Held &held : _window) {
            held = Held();
        }
    }
    std::fill(_history.begin(), _history.end(), -1);
    _heldCount = 0;
    _highestHeld = 0;
}

void RtpDepacketizer::reset()
{
    _haveSender = false;
    _ssrc = 0;
    _lastArrivalNs = 0;
    _haveSequence = false;
    _expected = 0;
    _clearWindow();
    _firstUnit = false;
    _endStartLog();
    _waitKey = true;
    _inAccessUnit = false;
    _auBroken = false;
    _auOversized = false;
    _inFragment = false;
    _fragmentType = -1;
    _au = AccessUnit();
}

// A new stream: the first packet to arrive may not be the first one sent
void RtpDepacketizer::_beginStart(quint16 sequence)
{
    _firstUnit = true;
    _startLog.clear();
    _startLogValid = (_capacity > 1);
    _firstSequence = sequence;
}

void RtpDepacketizer::_endStartLog()
{
    _startLogValid = false;
    _startLog.clear();
}

// A packet sent before the first one parsed, of the same (first, still open) access unit:
// what was parsed goes back into the window and the access unit is built again from this one.
bool RtpDepacketizer::_rewindStart(quint16 sequence, quint32 timestamp)
{
    if (!_firstUnit || !_startLogValid || !_inAccessUnit || (timestamp != _au.rtpTimestamp) || _startLog.empty()) {
        return false;
    }
    const quint16 beforeFirst = static_cast<quint16>(_firstSequence - sequence);
    if ((beforeFirst == 0) || (beforeFirst >= _capacity)) {
        return false;
    }
    // Everything parsed or held must fit in the window counted from this packet
    const quint16 top = (_heldCount > 0) ? _highestHeld : static_cast<quint16>(_expected - 1);
    if (static_cast<quint16>(top - sequence) >= _capacity) {
        return false;
    }
    for (const Held &logged : _startLog) {
        if (_window[_index(logged.sequence)].used) {
            return false;   // cannot happen inside the span checked above
        }
    }
    for (Held &logged : _startLog) {
        _window[_index(logged.sequence)] = std::move(logged);
        _heldCount++;
    }
    _stats.packets -= _startLog.size();    // counted again when parsed again
    _startLog.clear();
    _highestHeld = top;
    std::fill(_history.begin(), _history.end(), -1);
    _au = AccessUnit();
    _inAccessUnit = false;
    _auBroken = false;
    _auOversized = false;
    _inFragment = false;
    _fragmentType = -1;
    _expected = sequence;
    _firstSequence = sequence;
    return true;
}

void RtpDepacketizer::_restart()
{
    _stats.senderChanges++;
    reset();
}

void RtpDepacketizer::push(const QByteArray &packet, qint64 arrivalNs, QList<AccessUnit> &out)
{
    _take(packet, arrivalNs, out);
    // Every packet, used or not, moves the clock: nothing waits past its hold time
    _expire(arrivalNs, out);
}

void RtpDepacketizer::_take(const QByteArray &packet, qint64 arrivalNs, QList<AccessUnit> &out)
{
    const qsizetype packetSize = packet.size();
    if (packetSize > kMaxPacketBytes) {
        _stats.refused++;
        _stats.oversized++;
        return;
    }
    const char *const p = packet.constData();
    int size = static_cast<int>(packetSize);

    // RTP fixed header (RFC 3550 section 5.1)
    if ((size < kRtpHeaderBytes) || ((u8(p, 0) >> 6) != 2)) {
        _stats.refused++;
        return;
    }
    if ((u8(p, 1) & 0x7f) != _payloadType) {
        _stats.refused++;
        return;
    }

    const bool padding = (u8(p, 0) & 0x20) != 0;
    const bool extension = (u8(p, 0) & 0x10) != 0;
    const int csrcCount = u8(p, 0) & 0x0f;
    const bool marker = (u8(p, 1) & 0x80) != 0;
    const quint16 sequence = be16(p + 2);
    const quint32 timestamp = be32(p + 4);
    const quint32 ssrc = be32(p + 8);

    int offset = kRtpHeaderBytes + 4 * csrcCount;   // at most 72
    if (offset > size) {
        _stats.malformed++;
        return;
    }
    if (extension) {
        if (size - offset < 4) {
            _stats.malformed++;
            return;
        }
        const int extensionBytes = 4 * static_cast<int>(be16(p + offset + 2));   // at most 262,140
        if (extensionBytes > size - offset - 4) {
            _stats.malformed++;
            return;
        }
        offset += 4 + extensionBytes;
    }
    if (padding) {
        // The last byte counts the padding, itself included: at least 1, never into the header
        const int padBytes = u8(p, size - 1);
        if ((padBytes == 0) || (padBytes > size - offset)) {
            _stats.malformed++;
            return;
        }
        size -= padBytes;
    }
    if (size - offset < 1) {
        _stats.malformed++;   // no payload
        return;
    }

    // One sender at a time: another SSRC takes over only after the current one went quiet
    if (_haveSender && (ssrc != _ssrc)) {
        if ((arrivalNs - _lastArrivalNs) < kSsrcTakeoverNs) {
            _stats.refused++;
            return;
        }
        _restart();
    }
    if (!_haveSender) {
        _haveSender = true;
        _ssrc = ssrc;
    }

    if (!_haveSequence) {
        _haveSequence = true;
        _expected = sequence;
        _beginStart(sequence);
    }
    _accept(sequence, timestamp, marker, p + offset, size - offset, arrivalNs, out);
}

// Sequence numbers wrap at 65536: all differences are taken modulo 2^16.
void RtpDepacketizer::_accept(quint16 sequence, quint32 timestamp, bool marker, const char *payload, int size, qint64 arrivalNs,
                              QList<AccessUnit> &out)
{
    quint16 ahead = static_cast<quint16>(sequence - _expected);
    const quint16 behind = static_cast<quint16>(_expected - sequence);

    if ((ahead >= _capacity) && (ahead <= kRestartJump)) {
        // Past the window: the window is exceeded, so what is missing in front of the
        // packet's place (less the window) is lost. Held packets in that range go out first.
        _advanceTo(static_cast<quint16>(sequence - (_capacity - 1)), arrivalNs, out);
        _releaseReady(arrivalNs, out);
        ahead = static_cast<quint16>(sequence - _expected);
    }

    if (ahead == 0) {
        // The packet expected next: parsed straight from the caller's buffer, no wait
        if (_heldCount > 0) {
            _stats.reordered++;     // later ones came first and are waiting for it
        }
        _lastArrivalNs = arrivalNs;
        _history[_index(sequence)] = sequence;
        _expected = static_cast<quint16>(sequence + 1);
        _process(timestamp, marker, payload, size, arrivalNs, out);
        _releaseReady(arrivalNs, out);
        return;
    }

    if (ahead < _capacity) {
        // Ahead of a missing packet, inside the window: hold a copy
        Held &held = _window[_index(sequence)];
        if (held.used) {
            _stats.duplicates++;
            _stats.refused++;
            return;
        }
        const quint16 highestAhead = static_cast<quint16>(_highestHeld - _expected);
        if (_heldCount > 0) {
            if (ahead < highestAhead) {
                _stats.reordered++;     // a later one is already waiting
            } else {
                _highestHeld = sequence;
            }
        } else {
            _highestHeld = sequence;
        }
        held.used = true;
        held.sequence = sequence;
        held.marker = marker;
        held.timestamp = timestamp;
        held.arrivalNs = arrivalNs;
        held.payload = QByteArray(payload, size);
        _heldCount++;
        _lastArrivalNs = arrivalNs;
        return;
    }

    if (behind <= kRestartJump) {
        // At or before a place already parsed or given up: a duplicate or late, never used
        const bool seen = (behind <= _capacity) && (_history[_index(sequence)] == static_cast<qint32>(sequence));
        if (!seen && _rewindStart(sequence, timestamp)) {
            // Sent before the first packet to arrive: the start is rebuilt from it (now expected)
            _accept(sequence, timestamp, marker, payload, size, arrivalNs, out);
            return;
        }
        if (seen) {
            _stats.duplicates++;
        } else {
            _stats.late++;
            if (_firstUnit && _inAccessUnit && (timestamp == _au.rtpTimestamp)) {
                // Part of the first access unit, sent before its first packet to arrive, and
                // the start cannot be rebuilt: that access unit is incomplete, never decoded
                _auBroken = true;
                _endStartLog();
            }
        }
        _stats.refused++;
        return;
    }

    // Far outside the window either way: a new sender (random initial sequence number) or
    // hostile input. The window and the partial access unit go, the stream starts again here.
    const quint32 ssrc = _ssrc;
    _restart();
    _haveSender = true;
    _ssrc = ssrc;
    _haveSequence = true;
    _beginStart(sequence);
    _expected = static_cast<quint16>(sequence + 1);
    _history[_index(sequence)] = sequence;
    _lastArrivalNs = arrivalNs;
    _process(timestamp, marker, payload, size, arrivalNs, out);
}

void RtpDepacketizer::_process(quint32 timestamp, bool marker, const char *payload, int size, qint64 arrivalNs, QList<AccessUnit> &out)
{
    _stats.packets++;

    if (_inAccessUnit && (timestamp != _au.rtpTimestamp)) {
        // the previous frame's marker packet was lost or never sent
        _finishAccessUnit(out);
    }
    if (!_inAccessUnit) {
        _startAccessUnit(timestamp, arrivalNs);
    }

    if (!_parsePayload(payload, size)) {
        _auBroken = true;
    }

    if (_firstUnit && _startLogValid) {
        // A copy for a rewind of the start, bounded by the window (more cannot be rewound anyway)
        if (static_cast<int>(_startLog.size()) >= _capacity - 1) {
            _endStartLog();
        } else {
            Held logged;
            logged.used = true;
            logged.sequence = static_cast<quint16>(_expected - 1);   // every caller moved _expected past it
            logged.marker = marker;
            logged.timestamp = timestamp;
            logged.arrivalNs = arrivalNs;
            logged.payload = QByteArray(payload, size);
            _startLog.push_back(std::move(logged));
        }
    }

    if (marker) {
        _finishAccessUnit(out);
    }
}

void RtpDepacketizer::_declareLoss(quint32 count)
{
    _stats.sequenceGaps++;
    _stats.packetsMissing += count;
    _lossSinceUnit = true;
    _endStartLog();     // a start with a loss in it is not rebuilt
    if (_inAccessUnit) {
        _auBroken = true;
    }
    _inFragment = false;
    _waitKey = true;
}

void RtpDepacketizer::_release(Held &held, qint64 nowNs, QList<AccessUnit> &out)
{
    // Moved out of the slot first: parsing never touches the window
    const Held packet = std::move(held);
    held = Held();
    _heldCount--;
    const qint64 wait = std::max(qint64(0), nowNs - packet.arrivalNs);
    _stats.held++;
    _stats.holdNsTotal += wait;
    _stats.holdNsMax = qMax(_stats.holdNsMax, wait);
    _history[_index(packet.sequence)] = packet.sequence;
    _expected = static_cast<quint16>(packet.sequence + 1);
    // The access unit keeps the packet's own arrival time, so the wait shows in its latency
    _process(packet.timestamp, packet.marker, packet.payload.constData(), static_cast<int>(packet.payload.size()), packet.arrivalNs, out);
}

void RtpDepacketizer::_releaseReady(qint64 nowNs, QList<AccessUnit> &out)
{
    while (_heldCount > 0) {
        Held &held = _window[_index(_expected)];
        if (!held.used || (held.sequence != _expected)) {
            return;
        }
        _release(held, nowNs, out);
    }
}

// Everything before target is parsed (held) or lost (missing), in order; _expected becomes target.
void RtpDepacketizer::_advanceTo(quint16 target, qint64 nowNs, QList<AccessUnit> &out)
{
    const quint16 start = _expected;   // _release moves _expected
    const int steps = static_cast<quint16>(target - start);
    const int scan = qMin(steps, _capacity);   // the window holds nothing further
    quint32 missing = 0;
    for (int i = 0; i < scan; ++i) {
        const quint16 sequence = static_cast<quint16>(start + i);
        Held &held = _window[_index(sequence)];
        if (held.used && (held.sequence == sequence)) {
            if (missing > 0) {
                _declareLoss(missing);
                missing = 0;
            }
            _release(held, nowNs, out);
        } else {
            missing++;
            _history[_index(sequence)] = -1;
        }
    }
    // Sequence numbers jumped over beyond the window were never held. Their history slots
    // keep other sequence numbers, so a late copy of one still reads as late.
    missing += static_cast<quint32>(steps - scan);
    if (missing > 0) {
        _declareLoss(missing);
    }
    _expected = target;
}

// A held packet waits at most _holdNs: then the missing ones in front of it are a loss.
void RtpDepacketizer::_expire(qint64 nowNs, QList<AccessUnit> &out)
{
    while ((_heldCount > 0) && (nowNs >= nextDeadlineNs())) {
        // The first held packet in sequence order: everything missing before it is lost
        bool found = false;
        for (int i = 0; i < _capacity; ++i) {
            const quint16 sequence = static_cast<quint16>(_expected + i);
            const Held &held = _window[_index(sequence)];
            if (held.used && (held.sequence == sequence)) {
                _advanceTo(sequence, nowNs, out);
                found = true;
                break;
            }
        }
        if (!found) {
            _clearWindow();     // cannot happen (held packets are always inside the window)
            return;
        }
        _releaseReady(nowNs, out);
    }
}

qint64 RtpDepacketizer::nextDeadlineNs() const
{
    if (_heldCount == 0) {
        return 0;
    }
    qint64 oldest = 0;
    bool found = false;
    for (const Held &held : _window) {
        if (held.used && (!found || (held.arrivalNs < oldest))) {
            oldest = held.arrivalNs;
            found = true;
        }
    }
    return oldest + _holdNs;
}

void RtpDepacketizer::poll(qint64 nowNs, QList<AccessUnit> &out)
{
    _expire(nowNs, out);
}

void RtpDepacketizer::_startAccessUnit(quint32 timestamp, qint64 arrivalNs)
{
    _au = AccessUnit();
    _au.rtpTimestamp = timestamp;
    _au.firstPacketNs = arrivalNs;
    _inAccessUnit = true;
    _auBroken = false;
    _auOversized = false;
    _inFragment = false;
    _fragmentType = -1;
}

void RtpDepacketizer::_finishAccessUnit(QList<AccessUnit> &out)
{
    if (!_inAccessUnit) {
        return;
    }
    _inAccessUnit = false;
    _firstUnit = false;     // the start can no longer be rewound
    _endStartLog();
    if (_inFragment) {
        _auBroken = true;   // its last NAL unit never got its end fragment
    }
    _inFragment = false;
    _fragmentType = -1;

    if (_auOversized || _auBroken) {
        if (_auOversized) {
            _stats.oversized++;
        }
        _stats.droppedAccessUnits++;
        _lossSinceUnit = true;
        _waitKey = true;    // later frames may refer to this one
        _au = AccessUnit();
        return;
    }
    if (_au.data.isEmpty()) {
        _stats.droppedAccessUnits++;
        _lossSinceUnit = true;
        _au = AccessUnit();
        return;
    }
    if (_waitKey) {
        if (!_au.keyFrame) {
            _stats.droppedAccessUnits++;
            _lossSinceUnit = true;
            _au = AccessUnit();
            return;
        }
        _waitKey = false;
    }
    _stats.accessUnits++;
    _au.parameterSets = _parameterSets.annexB();
    _au.ssrc = _ssrc;
    _au.afterLoss = _lossSinceUnit;
    _lossSinceUnit = false;
    out.append(_au);
    _au = AccessUnit();
}

bool RtpDepacketizer::_isKeyNal(const char *nal) const
{
    if (_codec == Codec::H264) {
        return (u8(nal, 0) & 0x1f) == 5;                    // IDR slice
    }
    const int type = (u8(nal, 0) >> 1) & 0x3f;
    return (type >= 16) && (type <= 21);                     // IRAP
}

bool RtpDepacketizer::_append(const char *data, int size)
{
    if (_auOversized) {
        return false;
    }
    if ((size < 0) || (static_cast<qint64>(_au.data.size()) + size > _maxAccessUnitBytes)) {
        // Never grow past the bound: drop what is there and the rest of this access unit
        _auOversized = true;
        _auBroken = true;
        _au.data = QByteArray();
        return false;
    }
    _au.data.append(data, size);
    return true;
}

bool RtpDepacketizer::_appendNal(const char *nal, int size)
{
    if (!_append(kStartCode, kStartCodeBytes) || !_append(nal, size)) {
        return false;
    }
    if (_isKeyNal(nal)) {
        _au.keyFrame = true;
    }
    (void) _parameterSets.addNal(nal, size);    // a byte compare unless it is a parameter set
    return true;
}

bool RtpDepacketizer::_endFragmentIfOpen()
{
    if (!_inFragment) {
        return false;
    }
    // A new NAL unit while a fragmented one is still open: that one is cut short
    _inFragment = false;
    _fragmentType = -1;
    _auBroken = true;
    return true;
}

bool RtpDepacketizer::_parsePayload(const char *payload, int size)
{
    return (_codec == Codec::H264) ? _parseH264(payload, size) : _parseH265(payload, size);
}

// RFC 6184, non-interleaved mode. Returns false when the packet's data is lost to the
// access unit (malformed or unsupported); the counters say which.
bool RtpDepacketizer::_parseH264(const char *p, int size)
{
    // size >= 1 (checked in push)
    const quint8 header = u8(p, 0);
    if (header & 0x80) {
        _stats.malformed++;     // forbidden_zero_bit
        return false;
    }
    const int type = header & 0x1f;

    if ((type >= 1) && (type <= 23)) {
        (void) _endFragmentIfOpen();
        (void) _appendNal(p, size);
        return true;
    }

    if (type == 24) {   // STAP-A: [header] ([size16] [NAL unit])+
        (void) _endFragmentIfOpen();
        QVarLengthArray<Span, 16> units;
        int offset = 1;
        while (offset < size) {
            if (size - offset < 2) {
                _stats.malformed++;
                return false;
            }
            const int length = be16(p + offset);
            offset += 2;
            if ((length < 1) || (length > size - offset)) {
                _stats.malformed++;
                return false;
            }
            const quint8 nal = u8(p, offset);
            const int nalType = nal & 0x1f;
            if ((nal & 0x80) || (nalType < 1) || (nalType > 23)) {
                _stats.malformed++;     // no aggregation or fragment inside an aggregation
                return false;
            }
            units.append(Span{ p + offset, length });
            offset += length;
        }
        if (units.isEmpty()) {
            _stats.malformed++;
            return false;
        }
        // Checked as a whole first: a bad packet leaves nothing behind in the access unit
        for (const Span &unit : units) {
            (void) _appendNal(unit.data, unit.size);
        }
        return true;
    }

    if (type == 28) {   // FU-A: [indicator] [FU header] [fragment]
        if (size < 3) {
            _stats.malformed++;
            return false;
        }
        const quint8 fu = u8(p, 1);
        const bool start = (fu & 0x80) != 0;
        const bool end = (fu & 0x40) != 0;
        const int nalType = fu & 0x1f;
        if ((start && end) || (nalType < 1) || (nalType > 23)) {
            _stats.malformed++;
            (void) _endFragmentIfOpen();
            return false;
        }
        if (start) {
            (void) _endFragmentIfOpen();
            const char nalHeader = static_cast<char>((header & 0xe0) | nalType);
            if (!_append(kStartCode, kStartCodeBytes) || !_append(&nalHeader, 1)) {
                return true;    // over the bound: counted when the access unit closes
            }
            if (_isKeyNal(&nalHeader)) {
                _au.keyFrame = true;
            }
            _inFragment = true;
            _fragmentType = nalType;
        } else if (_auOversized) {
            return true;        // already dropped for size: nothing more to join
        } else if (!_inFragment || (nalType != _fragmentType)) {
            _stats.malformed++;     // middle or end without its start
            (void) _endFragmentIfOpen();
            return false;
        }
        (void) _append(p + 2, size - 2);
        if (end) {
            _inFragment = false;
            _fragmentType = -1;
        }
        return true;
    }

    // 0 and 30, 31: undefined; 25, 26, 27, 29: interleaved mode (STAP-B, MTAP16, MTAP24, FU-B)
    _stats.refused++;
    (void) _endFragmentIfOpen();
    return false;
}

// RFC 7798, without DONL (sprop-max-don-diff 0, as drone senders use).
bool RtpDepacketizer::_parseH265(const char *p, int size)
{
    if (size < 3) {
        _stats.malformed++;     // 2-byte payload header and at least one byte
        return false;
    }
    const quint8 h0 = u8(p, 0);
    const quint8 h1 = u8(p, 1);
    if ((h0 & 0x80) || ((h1 & 0x07) == 0)) {
        _stats.malformed++;     // forbidden_zero_bit, or TemporalId 0 (nuh_temporal_id_plus1 is never 0)
        return false;
    }
    const int type = (h0 >> 1) & 0x3f;

    if (type <= 47) {
        (void) _endFragmentIfOpen();
        (void) _appendNal(p, size);
        return true;
    }

    if (type == 48) {   // AP: [header 2] ([size16] [NAL unit])+
        (void) _endFragmentIfOpen();
        QVarLengthArray<Span, 16> units;
        int offset = 2;
        while (offset < size) {
            if (size - offset < 2) {
                _stats.malformed++;
                return false;
            }
            const int length = be16(p + offset);
            offset += 2;
            if ((length < 3) || (length > size - offset)) {
                _stats.malformed++;
                return false;
            }
            const quint8 n0 = u8(p, offset);
            const quint8 n1 = u8(p, offset + 1);
            const int nalType = (n0 >> 1) & 0x3f;
            if ((n0 & 0x80) || ((n1 & 0x07) == 0) || (nalType > 47)) {
                _stats.malformed++;
                return false;
            }
            units.append(Span{ p + offset, length });
            offset += length;
        }
        if (units.isEmpty()) {
            _stats.malformed++;
            return false;
        }
        for (const Span &unit : units) {
            (void) _appendNal(unit.data, unit.size);
        }
        return true;
    }

    if (type == 49) {   // FU: [header 2] [FU header] [fragment]
        if (size < 4) {
            _stats.malformed++;
            return false;
        }
        const quint8 fu = u8(p, 2);
        const bool start = (fu & 0x80) != 0;
        const bool end = (fu & 0x40) != 0;
        const int nalType = fu & 0x3f;
        if ((start && end) || (nalType > 47)) {
            _stats.malformed++;
            (void) _endFragmentIfOpen();
            return false;
        }
        if (start) {
            (void) _endFragmentIfOpen();
            const char nalHeader[2] = { static_cast<char>((h0 & 0x81) | (nalType << 1)), static_cast<char>(h1) };
            if (!_append(kStartCode, kStartCodeBytes) || !_append(nalHeader, 2)) {
                return true;    // over the bound: counted when the access unit closes
            }
            if (_isKeyNal(nalHeader)) {
                _au.keyFrame = true;
            }
            _inFragment = true;
            _fragmentType = nalType;
        } else if (_auOversized) {
            return true;        // already dropped for size: nothing more to join
        } else if (!_inFragment || (nalType != _fragmentType)) {
            _stats.malformed++;
            (void) _endFragmentIfOpen();
            return false;
        }
        (void) _append(p + 3, size - 3);
        if (end) {
            _inFragment = false;
            _fragmentType = -1;
        }
        return true;
    }

    // 50: PACI, 51 to 63: unspecified. Not supported: the data they carry is lost.
    _stats.refused++;
    (void) _endFragmentIfOpen();
    return false;
}
