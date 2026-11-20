/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QtTypes>

#include <vector>

#include "VideoPacketTap.h"

/// G88 (design C1): GC's own RTP depacketizer for H.264 (RFC 6184) and H.265 (RFC 7798).
/// It turns RTP packets into Annex B access units and closes an access unit on the RTP
/// marker bit, so a frame goes to the decoder as soon as its last packet arrives
/// (libavformat's RTP path waits for the next frame's packets, spike Day 1 finding).
///
/// H.264: single NAL units (1 to 23), STAP-A (24), FU-A (28). STAP-B, MTAP and FU-B
/// (25, 26, 27, 29: interleaved mode) are refused.
/// H.265: single NAL units (0 to 47), AP (48), FU (49); PACI (50) and the unspecified
/// types 51 to 63 are skipped; no DONL.
/// Reorder window (G88 receiver): packets that arrive ahead of a missing one are held, in
/// sequence order, for at most reorderPackets sequence numbers and reorderHoldNs of time.
/// The missing packet usually comes a few packets later (Wi-Fi and cellular reorder), and
/// then everything is released in order with nothing lost. In-order packets never wait:
/// they are parsed straight from the caller's buffer. When the window is exceeded (a packet
/// further ahead than reorderPackets, or a held packet that waited reorderHoldNs) the
/// missing packets are a real loss: the access unit being built and everything after it
/// is dropped until the next key frame (H.264 IDR, H.265 IRAP). A packet that comes after
/// its place was given up is late and refused; a second copy of a packet is a duplicate
/// and refused. The window's memory is bounded (reorderPackets packets of at most
/// kMaxPacketBytes) and so is its wait; a jump of more than kRestartJump either way is a
/// new sender and resets it. Held packets are released by push and by poll: the caller
/// calls poll by nextDeadlineNs when no packet comes.
///
/// Stream start: the first packet to arrive sets the expected sequence, but it need not be
/// the first one sent (Wi-Fi reorders the first packets as well as later ones). Until the
/// first access unit closes, the packets parsed into it are kept (fewer than reorderPackets),
/// so a packet of that access unit sent before the first one to arrive rewinds the start:
/// the access unit is built again from the earlier packet, in sequence order. Without this
/// the earlier packet (typically the SPS/PPS in front of the first IDR) was refused as late
/// and the IDR went to the decoder without its parameter sets, which then failed every
/// frame until the next key frame. When the rewind is not possible (no window, or the
/// access unit is already as large as the window) that access unit is dropped instead.
/// In-order packets still never wait.
///
/// Every packet is network input and treated as hostile: each length is checked against
/// the bytes actually there before it is used, nothing is read past the packet, a packet
/// that does not parse leaves no partial data in the access unit, and an access unit is
/// never larger than maxAccessUnitBytes (it is dropped instead). One sender (SSRC) at a
/// time: another SSRC takes over only after the current one has been silent for
/// kSsrcTakeoverNs.
///
/// Pure (no sockets, no FFmpeg): unit tested in RtpDepacketizerTest.
class RtpDepacketizer
{
public:
    enum class Codec { H264, H265 };

    /// Larger than any UDP payload (65,507 bytes over IPv4); anything bigger is refused.
    static constexpr int kMaxPacketBytes = 65536;
    /// Default bound of one access unit: a 4K intra frame at a high bit rate is about 2 MB.
    static constexpr int kDefaultMaxAccessUnitBytes = 8 * 1024 * 1024;
    /// A packet from another SSRC is accepted only after this much silence from the current one.
    static constexpr qint64 kSsrcTakeoverNs = 500LL * 1000 * 1000;
    /// A sequence jump larger than this, forward or back, is a new sender (restart), not a loss.
    static constexpr int kRestartJump = 1000;
    /// Default reorder window: 64 sequence numbers and 50 ms. Only packets behind a gap wait;
    /// in-order packets never do. 50 ms is 1.5 frames at 30 fps, far less than the freeze a
    /// loss costs (up to one GOP, about 1 s, until the next key frame), and well above the few
    /// milliseconds Wi-Fi and cellular retries move a packet. 64 packets of 1200 bytes are
    /// 150 ms at 4 Mb/s (720p30) and 77 ms at 8 Mb/s (1080p30), so the time bound normally ends
    /// a gap first; the count bound only acts in a key frame burst or above about 12 Mb/s.
    /// Memory: 64 x 1.2 KB typical, at most 64 x kMaxPacketBytes (4 MB).
    static constexpr int kDefaultReorderPackets = 64;
    static constexpr qint64 kDefaultReorderHoldNs = 50LL * 1000 * 1000;
    /// Largest window accepted (well under kRestartJump); 0 or 1 means no window.
    static constexpr int kMaxReorderPackets = 512;
    /// Longest hold accepted.
    static constexpr qint64 kMaxReorderHoldNs = 500LL * 1000 * 1000;

    struct AccessUnit
    {
        QByteArray data;            ///< Annex B (start codes), ready for avcodec_send_packet
        quint32 rtpTimestamp = 0;
        qint64 firstPacketNs = 0;   ///< arrival time of its first RTP packet
        bool keyFrame = false;
        QByteArray parameterSets;   ///< latest SPS/PPS (H.265 VPS/SPS/PPS) seen so far, Annex B (shared, not copied)
        quint32 ssrc = 0;
        bool afterLoss = false;     ///< packets were given up or access units dropped since the previous unit out
    };

    struct Stats
    {
        quint64 packets = 0;            ///< RTP packets of the current sender taken in
        quint64 accessUnits = 0;
        quint64 sequenceGaps = 0;       ///< gap events
        quint64 packetsMissing = 0;     ///< packets missing in those gaps
        quint64 droppedAccessUnits = 0; ///< incomplete, malformed, too large or waiting for a key frame
        quint64 refused = 0;            ///< not RTP, wrong payload type, late, duplicate, other sender, unsupported packetization
        quint64 malformed = 0;          ///< RTP or payload structure that does not hold together
        quint64 oversized = 0;          ///< packets over kMaxPacketBytes and access units over the bound
        quint64 senderChanges = 0;      ///< restarts: new SSRC or a sequence jump
        quint64 reordered = 0;          ///< packets that came after a later one and were still used (recovered)
        quint64 late = 0;               ///< packets that came after their place was given up as lost (also in refused)
        quint64 duplicates = 0;         ///< second copies of a packet (also in refused)
        quint64 held = 0;               ///< packets that waited in the reorder window
        qint64 holdNsTotal = 0;         ///< their total wait
        qint64 holdNsMax = 0;           ///< the longest wait of one packet
    };

    explicit RtpDepacketizer(Codec codec, int payloadType = 96, int maxAccessUnitBytes = kDefaultMaxAccessUnitBytes);

    /// One RTP packet as received. Complete access units are appended to out: usually at most
    /// two (the one a timestamp change closed and the one the marker bit closed), more when
    /// the packet releases packets held in the reorder window. arrivalNs is a steady clock
    /// that never goes back; poll uses the same clock.
    void push(const QByteArray &packet, qint64 arrivalNs, QList<AccessUnit> &out);

    /// Releases what waited in the reorder window for reorderHoldNs or more by nowNs (the
    /// missing packets in front of it are a loss). Call it when nextDeadlineNs has passed.
    void poll(qint64 nowNs, QList<AccessUnit> &out);
    /// When poll has work to do (steady ns), or 0 when nothing is held.
    qint64 nextDeadlineNs() const;
    /// Packets held in the reorder window now.
    int heldPackets() const { return _heldCount; }

    /// Reorder window size in sequence numbers (0 or 1: none) and the longest hold. Values are
    /// clamped to kMaxReorderPackets and kMaxReorderHoldNs. Drops anything held.
    void setReorderWindow(int packets, qint64 holdNs);
    int reorderPackets() const { return _capacity; }
    qint64 reorderHoldNs() const { return _holdNs; }

    const Stats &stats() const { return _stats; }
    /// The current sender (0 before the first packet)
    quint32 ssrc() const { return _haveSender ? _ssrc : 0; }
    /// Latest parameter sets seen in single NAL units and aggregation packets (kept across resets)
    QByteArray parameterSets() const { return _parameterSets.annexB(); }
    int maxAccessUnitBytes() const { return _maxAccessUnitBytes; }

    /// Restart (new sender): forget SSRC, sequence, timestamp, the reorder window and the
    /// partial access unit.
    void reset();

private:
    struct Span
    {
        const char *data;
        int size;
    };

    /// A packet in the reorder window: its payload is a copy (the caller's buffer is reused)
    struct Held
    {
        bool used = false;
        quint16 sequence = 0;
        bool marker = false;
        quint32 timestamp = 0;
        qint64 arrivalNs = 0;
        QByteArray payload;
    };

    void _take(const QByteArray &packet, qint64 arrivalNs, QList<AccessUnit> &out);
    void _accept(quint16 sequence, quint32 timestamp, bool marker, const char *payload, int size, qint64 arrivalNs, QList<AccessUnit> &out);
    void _process(quint32 timestamp, bool marker, const char *payload, int size, qint64 arrivalNs, QList<AccessUnit> &out);
    void _release(Held &held, qint64 nowNs, QList<AccessUnit> &out);
    void _releaseReady(qint64 nowNs, QList<AccessUnit> &out);
    void _advanceTo(quint16 target, qint64 nowNs, QList<AccessUnit> &out);
    void _expire(qint64 nowNs, QList<AccessUnit> &out);
    void _declareLoss(quint32 count);
    void _clearWindow();
    void _beginStart(quint16 sequence);
    void _endStartLog();
    bool _rewindStart(quint16 sequence, quint32 timestamp);
    size_t _index(quint16 sequence) const { return static_cast<size_t>(sequence % static_cast<quint32>(_capacity)); }
    bool _parsePayload(const char *payload, int size);
    bool _parseH264(const char *p, int size);
    bool _parseH265(const char *p, int size);
    bool _appendNal(const char *nal, int size);
    bool _append(const char *data, int size);
    bool _endFragmentIfOpen();
    void _startAccessUnit(quint32 timestamp, qint64 arrivalNs);
    void _finishAccessUnit(QList<AccessUnit> &out);
    void _restart();
    bool _isKeyNal(const char *nal) const;

    Codec _codec;
    int _payloadType;
    int _maxAccessUnitBytes;

    bool _haveSender = false;
    quint32 _ssrc = 0;
    qint64 _lastArrivalNs = 0;
    bool _haveSequence = false;
    quint16 _expected = 0;          ///< next sequence number to parse

    int _capacity = kDefaultReorderPackets;
    qint64 _holdNs = kDefaultReorderHoldNs;
    std::vector<Held> _window;      ///< _capacity slots, by sequence modulo _capacity: [_expected, _expected + _capacity)
    std::vector<qint32> _history;   ///< same slots for [_expected - _capacity, _expected): sequence parsed there, or -1 if lost
    int _heldCount = 0;
    quint16 _highestHeld = 0;       ///< valid while _heldCount > 0

    bool _firstUnit = false;        ///< the first access unit since the sender (re)started is still open
    bool _startLogValid = false;    ///< _startLog holds every packet parsed into that access unit
    quint16 _firstSequence = 0;     ///< lowest sequence number parsed since the start
    std::vector<Held> _startLog;    ///< copies of those packets (fewer than _capacity), for a rewind

    bool _waitKey = true;           ///< nothing decodable until a key frame
    bool _lossSinceUnit = false;    ///< a loss or a dropped unit since the last unit out (AccessUnit::afterLoss)
    bool _inAccessUnit = false;
    bool _auBroken = false;         ///< a packet of this access unit was lost or malformed
    bool _auOversized = false;      ///< this access unit went over the bound
    bool _inFragment = false;
    int _fragmentType = -1;         ///< NAL type of the fragmented NAL unit being joined
    AccessUnit _au;
    Stats _stats;
    VideoParameterSets _parameterSets;
};
