/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "VideoPacketTap.h"

#include <QtCore/QDeadlineTimer>

#include <cstring>
#include <utility>

namespace {

constexpr char kStartCode[4] = { 0, 0, 0, 1 };

int nalType(VideoTapCodec codec, const char *nal)
{
    const quint8 b = static_cast<quint8>(nal[0]);
    return (codec == VideoTapCodec::H264) ? (b & 0x1f) : ((b >> 1) & 0x3f);
}

} // namespace

// VideoParameterSets -----------------------------------------------------------------

void VideoParameterSets::setCodec(VideoTapCodec codec)
{
    if (codec != _codec) {
        _codec = codec;
        clear();
    }
}

void VideoParameterSets::clear()
{
    _vps.clear();
    _sps.clear();
    _pps.clear();
    _combined.clear();
}

bool VideoParameterSets::isParameterSet(VideoTapCodec codec, const char *nal, int size)
{
    if (!nal || (size < 2) || (size > kMaxNalBytes) || (static_cast<quint8>(nal[0]) & 0x80)) {
        return false;
    }
    const int type = nalType(codec, nal);
    return (codec == VideoTapCodec::H264) ? ((type == 7) || (type == 8)) : ((type >= 32) && (type <= 34));
}

bool VideoParameterSets::addNal(const char *nal, int size)
{
    if (!isParameterSet(_codec, nal, size)) {
        return false;
    }
    const int type = nalType(_codec, nal);
    QByteArray *slot = nullptr;
    if (_codec == VideoTapCodec::H264) {
        slot = (type == 7) ? &_sps : &_pps;
    } else {
        slot = (type == 32) ? &_vps : ((type == 33) ? &_sps : &_pps);
    }
    if ((slot->size() == size) && (std::memcmp(slot->constData(), nal, static_cast<size_t>(size)) == 0)) {
        return false;
    }
    *slot = QByteArray(nal, size);
    _rebuild();
    return true;
}

QList<QByteArray> VideoParameterSets::split(const QByteArray &annexB)
{
    QList<QByteArray> nals;
    const char *const p = annexB.constData();
    const int n = static_cast<int>(annexB.size());
    int start = -1;
    int i = 0;
    auto take = [&](int end) {
        while ((end > start) && (p[end - 1] == 0)) {
            --end;
        }
        if (end > start) {
            nals.append(QByteArray(p + start, end - start));
        }
    };
    while (i + 3 <= n) {
        if ((p[i] == 0) && (p[i + 1] == 0) && (p[i + 2] == 1)) {
            if (start >= 0) {
                take(i);
            }
            i += 3;
            start = i;
        } else {
            ++i;
        }
    }
    if ((start >= 0) && (start < n)) {
        take(n);
    }
    return nals;
}

bool VideoParameterSets::scanAnnexB(const QByteArray &annexB)
{
    const char *const p = annexB.constData();
    const int n = static_cast<int>(annexB.size());
    bool changed = false;
    int start = -1;     // first byte of the current NAL unit
    int i = 0;
    while (i + 3 <= n) {
        if ((p[i] == 0) && (p[i + 1] == 0) && (p[i + 2] == 1)) {
            if (start >= 0) {
                int end = i;
                while ((end > start) && (p[end - 1] == 0)) {
                    --end;      // trailing zero of a 4-byte start code
                }
                changed |= addNal(p + start, end - start);
            }
            i += 3;
            start = i;
        } else {
            ++i;
        }
    }
    if ((start >= 0) && (start < n)) {
        changed |= addNal(p + start, n - start);
    }
    return changed;
}

void VideoParameterSets::_rebuild()
{
    QByteArray combined;
    for (const QByteArray *nal : { &_vps, &_sps, &_pps }) {
        if (!nal->isEmpty()) {
            combined.append(kStartCode, 4);
            combined.append(*nal);
        }
    }
    _combined = combined;
}

// VideoPacketConsumer ----------------------------------------------------------------

VideoPacketConsumer::VideoPacketConsumer(int capacity, qint64 maxBytes)
    : _capacity(capacity)
    , _maxBytes(maxBytes)
    , _waitKey(true)    // a consumer starts at a keyframe
{
}

void VideoPacketConsumer::setWakeup(std::function<void()> wakeup)
{
    std::function<void()> old;
    {
        // Waits for a wakeup that is running now (G88 M2); none of the old one starts after this
        QMutexLocker locker(&_wakeMutex);
        old = std::exchange(_wakeup, std::move(wakeup));
    }
    // The old wakeup (and what it captured) is released outside the lock
}

VideoPacketConsumer::Stats VideoPacketConsumer::stats() const
{
    QMutexLocker locker(&_mutex);
    Stats s = _stats;
    s.queued = static_cast<int>(_queue.size());
    s.queuedBytes = _queuedBytes;
    return s;
}

bool VideoPacketConsumer::tryPop(VideoTapItem &item)
{
    QMutexLocker locker(&_mutex);
    if (_queue.empty()) {
        return false;
    }
    item = std::move(_queue.front());
    _queue.pop_front();
    if (const VideoTapPacket *packet = std::get_if<VideoTapPacket>(&item)) {
        --_queuedPackets;
        _queuedBytes -= packetBytes(*packet);
    }
    return true;
}

bool VideoPacketConsumer::waitPop(VideoTapItem &item, int timeoutMs)
{
    QDeadlineTimer deadline(qMax(0, timeoutMs));
    QMutexLocker locker(&_mutex);
    while (_queue.empty()) {
        if (_detached.load() || !_ready.wait(&_mutex, deadline)) {
            if (_queue.empty()) {
                return false;
            }
            break;
        }
    }
    item = std::move(_queue.front());
    _queue.pop_front();
    if (const VideoTapPacket *packet = std::get_if<VideoTapPacket>(&item)) {
        --_queuedPackets;
        _queuedBytes -= packetBytes(*packet);
    }
    return true;
}

void VideoPacketConsumer::_offerPacket(const VideoTapPacket &packet)
{
    {
        QMutexLocker locker(&_mutex);
        if (_detached.load()) {
            return;
        }
        if (_waitKey && !packet.keyFrame) {
            if (_stats.packets > 0) {
                _stats.dropped++;
                _pendingDropped++;
            }
            return;
        }
        const qint64 bytes = packetBytes(packet);
        if ((_queuedPackets >= _capacity) || (bytes > (_maxBytes - _queuedBytes))) {
            // Full (packets or bytes, G88 M3): drop and wait for the next keyframe, so what this
            // consumer gets stays decodable
            _stats.dropped++;
            _pendingDropped++;
            _waitKey = true;
            return;
        }
        VideoTapPacket copy = packet;   // shares the bytes; the receiver never writes them again
        copy.droppedBefore = _pendingDropped;
        if (_pendingDropped > 0) {
            copy.discontinuity = true;
        }
        if (_waitKey && (_stats.packets > 0)) {
            _stats.resyncs++;
        }
        _waitKey = false;
        _pendingDropped = 0;
        _queue.emplace_back(std::move(copy));
        _queuedPackets++;
        _queuedBytes += bytes;
        _stats.packets++;
    }
    _wake();
}

void VideoPacketConsumer::_offerEvent(const VideoTapEvent &event)
{
    {
        QMutexLocker locker(&_mutex);
        if (_detached.load()) {
            return;
        }
        if (static_cast<int>(_queue.size()) >= _capacity + kEventReserve) {
            _stats.droppedEvents++;
            return;
        }
        _queue.emplace_back(event);
        _stats.events++;
        if ((event.type != VideoTapEvent::Type::StreamStopped) && (_stats.packets > 0)) {
            // New stream or sender: its own parameter sets and keyframe come next
            _waitKey = true;
        }
    }
    _wake();
}

void VideoPacketConsumer::_wake()
{
    {
        QMutexLocker locker(&_mutex);
        _ready.wakeAll();
    }
    // G88 M2: the wakeup runs under the wake lock, so setWakeup and _detach wait for it to
    // return and no call starts after they have returned. Safe because a wakeup returns at once
    // and never publishes. A local copy, so that a wakeup that replaces itself runs intact.
    QMutexLocker wakeLocker(&_wakeMutex);
    if (_wakeup && !_detached.load()) {
        const std::function<void()> wakeup = _wakeup;
        wakeup();
    }
}

void VideoPacketConsumer::_detach()
{
    {
        QMutexLocker locker(&_mutex);
        _detached = true;
        _ready.wakeAll();
    }
    std::function<void()> old;
    {
        // Waits for a wakeup that is running now (G88 M2); none starts after this
        QMutexLocker wakeLocker(&_wakeMutex);
        old = std::exchange(_wakeup, nullptr);
    }
}

// VideoPacketTap ---------------------------------------------------------------------

VideoPacketTap::~VideoPacketTap()
{
    QMutexLocker locker(&_mutex);
    for (const std::shared_ptr<VideoPacketConsumer> &consumer : std::as_const(_consumers)) {
        consumer->_detach();
    }
    _consumers.clear();
    _count = 0;
}

std::shared_ptr<VideoPacketConsumer> VideoPacketTap::attach(int capacity, qint64 maxBytes)
{
    QMutexLocker locker(&_mutex);
    if (_consumers.size() >= kMaxConsumers) {
        return nullptr;
    }
    std::shared_ptr<VideoPacketConsumer> consumer(new VideoPacketConsumer(qBound(1, capacity, kMaxCapacity),
                                                                          qBound(kMinMaxBytes, maxBytes, kMaxMaxBytes)));
    _consumers.append(consumer);
    _count = static_cast<int>(_consumers.size());
    return consumer;
}

void VideoPacketTap::detach(const std::shared_ptr<VideoPacketConsumer> &consumer)
{
    if (!consumer) {
        return;
    }
    consumer->_detach();
    QMutexLocker locker(&_mutex);
    _consumers.removeAll(consumer);
    _count = static_cast<int>(_consumers.size());
}

QList<std::shared_ptr<VideoPacketConsumer>> VideoPacketTap::_snapshot()
{
    // Called with _mutex held. Consumers released by their owner (only the tap holds them) go.
    for (qsizetype i = _consumers.size() - 1; i >= 0; --i) {
        if (_consumers.at(i).use_count() == 1) {
            _consumers.at(i)->_detach();
            _consumers.removeAt(i);
        }
    }
    _count = static_cast<int>(_consumers.size());
    return _consumers;
}

void VideoPacketTap::publishPacket(VideoTapPacket packet)
{
    if (!hasConsumers()) {
        return;
    }
    // One publish at a time, so every consumer sees items in publish order
    QMutexLocker publishLocker(&_publishMutex);
    QList<std::shared_ptr<VideoPacketConsumer>> consumers;
    {
        QMutexLocker locker(&_mutex);
        consumers = _snapshot();
        packet.index = ++_index;
    }
    packet.droppedBefore = 0;
    // Each consumer's own lock is held only to queue: a consumer busy with its data never blocks this
    for (const std::shared_ptr<VideoPacketConsumer> &consumer : std::as_const(consumers)) {
        consumer->_offerPacket(packet);
    }
}

void VideoPacketTap::publishEvent(const VideoTapEvent &event)
{
    if (!hasConsumers()) {
        return;
    }
    // One publish at a time, so every consumer sees items in publish order
    QMutexLocker publishLocker(&_publishMutex);
    QList<std::shared_ptr<VideoPacketConsumer>> consumers;
    {
        QMutexLocker locker(&_mutex);
        consumers = _snapshot();
    }
    for (const std::shared_ptr<VideoPacketConsumer> &consumer : std::as_const(consumers)) {
        consumer->_offerEvent(event);
    }
}
