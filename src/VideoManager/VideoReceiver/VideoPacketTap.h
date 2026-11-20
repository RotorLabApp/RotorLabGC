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
#include <QtCore/QMutex>
#include <QtCore/QRecursiveMutex>
#include <QtCore/QString>
#include <QtCore/QWaitCondition>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <variant>

/// G88 packet tap: the video
/// receiver publishes every complete access unit as received (after RTP depacketizing or the
/// libavformat demux, before decoding) to any number of consumers, such as local recording
/// (G95) and the Fleet Server relay (G109). Consumers build against this header only.
///
/// Rules:
/// - A slow or blocked consumer never stalls decoding or display. Each consumer has a bounded
///   queue, bounded both in packets and in bytes (G88 M3: a hostile sender can make every unit
///   up to 8 MiB); when either bound is reached, units are dropped and counted, and that consumer
///   gets nothing more until the next keyframe, which then carries discontinuity and
///   droppedBefore > 0.
/// - Consumers never see the receiver's buffers: VideoTapPacket::data and parameterSets are
///   QByteArrays of their own (implicitly shared and never written again by the receiver; a
///   consumer that writes to its copy detaches it).
/// - With no consumer attached, publishing costs one atomic load.
/// - All items one consumer sees are in publish order: events and packets share one queue.
///   A new generation is always announced by an event before its first unit.
/// - Every function is thread safe. The receiver publishes from its worker thread (and from the
///   GUI thread for SourceChanged); a consumer pops on any thread of its own.
///
/// Generations: every StreamStarted, SourceChanged and SsrcChanged event starts a new
/// generation (a new source, a reopen, a new codec, a new RTP sender, or the stream coming
/// back after a timeout). Within one generation the codec, the sender and the clocks are
/// continuous, apart from units marked discontinuity.

enum class VideoTapCodec : quint8 { H264, H265 };

/// One access unit, Annex B (00 00 00 01 start codes), exactly as the camera sent it.
struct VideoTapPacket
{
    VideoTapCodec codec = VideoTapCodec::H264;
    QByteArray data;                ///< the access unit (in-band parameter sets stay in it)
    /// Latest SPS and PPS (H.265: VPS, SPS, PPS) seen in this generation, Annex B, current for
    /// this unit; may be empty before the first keyframe. VideoParameterSets::split() gives the
    /// NAL units one by one.
    QByteArray parameterSets;
    bool keyFrame = false;          ///< IDR (H.264) or IRAP (H.265): a decoder or a new file can start here
    /// True on the first unit of a generation and on the first unit after a gap: units lost on
    /// the network (RTP sequence gap), dropped by the receiver (incomplete or damaged), or
    /// dropped for this consumer (queue full). Such a unit is always a keyframe.
    bool discontinuity = false;
    bool hasRtpTimestamp = false;   ///< true for RTP sources received by GC's own depacketizer (UDP)
    quint32 rtpTimestamp = 0;       ///< RTP timestamp (90 kHz, wraps at 2^32) as received, when hasRtpTimestamp
    /// Presentation and decode time in 90 kHz units; -1 unknown. UDP RTP (no B-frames on that
    /// path): both are the RTP timestamp. RTSP and MPEG-TS: the demuxer's pts and dts, which
    /// differ when the stream has B-frames.
    qint64 pts90k = -1;
    qint64 dts90k = -1;
    qint64 receiveSteadyNs = 0;     ///< monotonic (steady) clock: first packet of the unit at the socket, or demux time
    /// Wall clock (UTC, ms since 1970) for receiveSteadyNs, from this generation's anchor
    /// (VideoTapEvent::utcAnchorMs) plus elapsed steady time: never jumps within a generation.
    qint64 receiveUtcMs = 0;
    quint32 ssrc = 0;               ///< RTP sender (UDP sources), 0 otherwise
    quint64 generation = 0;         ///< see "Generations" above
    quint64 index = 0;              ///< tap-wide publish counter: consecutive for units published while consumers exist
    quint64 droppedBefore = 0;      ///< units this consumer lost from its queue just before this one
};

struct VideoTapEvent
{
    enum class Type : quint8 {
        StreamStarted,  ///< input open (UDP socket bound, RTSP/MPEG-TS opened) or packets back after a timeout: new generation
        StreamStopped,  ///< the source stopped (see stopReason): close the current part or live stream
        SourceChanged,  ///< the receiver was started on another URI (source is the new one): new generation
        SsrcChanged,    ///< another RTP sender took over on the same port: new generation
    };
    enum class StopReason : quint8 {
        Stopped,        ///< the receiver was stopped: video source set to Disabled or changed, or GC closing
        Timeout,        ///< no packets for the receiver's timeout; StreamStarted follows if they come back
        Error,          ///< the input failed (RTSP or MPEG-TS read error); the receiver reopens it
    };

    Type type = Type::StreamStarted;
    StopReason stopReason = StopReason::Stopped;   ///< StreamStopped only
    quint64 generation = 0;         ///< the generation this event starts (or, for StreamStopped, ends)
    VideoTapCodec codec = VideoTapCodec::H264;
    bool codecKnown = false;
    QString source;                 ///< video URI without credentials
    quint32 ssrc = 0;
    qint64 steadyNs = 0;            ///< monotonic clock when the event was published
    /// The generation's clock anchor: utcAnchorMs (UTC, ms since 1970) was read at steadyAnchorNs.
    /// A unit's wall time is utcAnchorMs + (receiveSteadyNs - steadyAnchorNs) / 1e6.
    qint64 utcAnchorMs = 0;
    qint64 steadyAnchorNs = 0;
};

using VideoTapItem = std::variant<VideoTapPacket, VideoTapEvent>;

/// The latest parameter sets of a stream, kept per NAL unit type. Useful to consumers too
/// (for example as decoder or muxer extradata).
class VideoParameterSets
{
public:
    static constexpr int kMaxNalBytes = 4096;   ///< larger "parameter sets" are not kept

    explicit VideoParameterSets(VideoTapCodec codec = VideoTapCodec::H264) : _codec(codec) {}

    VideoTapCodec codec() const { return _codec; }
    void setCodec(VideoTapCodec codec);

    /// One NAL unit without start code. Returns true if it was a parameter set and changed
    /// the stored ones.
    bool addNal(const char *nal, int size);
    /// Every NAL unit of an Annex B buffer (3- or 4-byte start codes).
    bool scanAnnexB(const QByteArray &annexB);
    /// Annex B: VPS (H.265), SPS, PPS. Shares data until the next change.
    QByteArray annexB() const { return _combined; }
    /// The NAL units of an Annex B buffer, without start codes (for avcC/hvcC style records).
    static QList<QByteArray> split(const QByteArray &annexB);
    bool isEmpty() const { return _combined.isEmpty(); }
    void clear();

    static bool isParameterSet(VideoTapCodec codec, const char *nal, int size);

private:
    void _rebuild();

    VideoTapCodec _codec;
    QByteArray _vps;
    QByteArray _sps;
    QByteArray _pps;
    QByteArray _combined;
};

/// One consumer's bounded queue. Get it from VideoPacketTap::attach; give it back with
/// VideoPacketTap::detach (or just release it: the tap then drops it at its next publish).
class VideoPacketConsumer
{
public:
    struct Stats
    {
        quint64 packets = 0;        ///< packets queued for this consumer
        quint64 events = 0;         ///< events queued
        quint64 dropped = 0;        ///< packets dropped (queue full, or waiting for a keyframe after that)
        quint64 droppedEvents = 0;  ///< events dropped (event reserve full as well)
        quint64 resyncs = 0;        ///< times this consumer restarted at a keyframe after a drop
        int queued = 0;             ///< items waiting now
        qint64 queuedBytes = 0;     ///< bytes of the packets waiting now (data plus parameterSets)
    };

    /// Items accepted beyond the packet capacity, for events only (stream changes must arrive)
    static constexpr int kEventReserve = 32;

    int capacity() const { return _capacity; }
    qint64 maxBytes() const { return _maxBytes; }
    /// The bytes a packet counts against the byte budget
    static qint64 packetBytes(const VideoTapPacket &packet) { return static_cast<qint64>(packet.data.size()) + static_cast<qint64>(packet.parameterSets.size()); }

    /// Next item, if any. Never blocks for more than the queue's own short lock.
    bool tryPop(VideoTapItem &item);
    /// Waits up to timeoutMs on the consumer's thread for the next item. False on timeout or
    /// once detached.
    bool waitPop(VideoTapItem &item, int timeoutMs);
    /// Called on the publishing thread after an item was queued: it must return at once
    /// (for example QMetaObject::invokeMethod with Qt::QueuedConnection) and must not publish.
    /// Optional.
    /// Guarantee (G88 M2): the wakeup runs under this consumer's wake lock, which setWakeup and
    /// detach also take. Once setWakeup (with any value) or detach has returned, no call to the
    /// previous wakeup is running and none can start, so the wakeup may capture a raw pointer
    /// whose owner replaces or clears it (or detaches) before it is destroyed. So setWakeup and
    /// detach may wait for one wakeup call to return. A wakeup may call setWakeup or the pop
    /// functions on its own consumer (the lock is recursive), but must not call VideoPacketTap
    /// functions and must not wait on a thread that is calling setWakeup or detach.
    void setWakeup(std::function<void()> wakeup);

    Stats stats() const;
    /// False once detached, by the owner or because the tap was destroyed. A consumer
    /// that is no longer attached must not call detach on a VideoPacketTap pointer it kept: the
    /// tap may be gone. Detaching is then not needed.
    bool attached() const { return !_detached.load(); }

private:
    friend class VideoPacketTap;
    VideoPacketConsumer(int capacity, qint64 maxBytes);

    void _offerPacket(const VideoTapPacket &packet);
    void _offerEvent(const VideoTapEvent &event);
    void _detach();
    void _wake();

    const int _capacity;
    const qint64 _maxBytes;
    mutable QMutex _mutex;
    QWaitCondition _ready;
    std::deque<VideoTapItem> _queue;
    int _queuedPackets = 0;
    qint64 _queuedBytes = 0;
    bool _waitKey = false;          ///< dropped something: nothing until the next keyframe
    quint64 _pendingDropped = 0;
    Stats _stats;
    /// Held while the wakeup runs, and by setWakeup and _detach (G88 M2). Never taken with
    /// _mutex held; _mutex may be taken under it. Recursive so that a wakeup may clear itself.
    QRecursiveMutex _wakeMutex;
    std::function<void()> _wakeup;  ///< guarded by _wakeMutex
    std::atomic_bool _detached{false};
};

class VideoPacketTap
{
public:
    static constexpr int kDefaultCapacity = 120;    ///< about 4 s at 30 frames per second
    static constexpr int kMaxCapacity = 3000;
    static constexpr int kMaxConsumers = 8;
    /// Byte budget per consumer (G88 M3): data plus parameterSets of the queued packets
    static constexpr qint64 kDefaultMaxBytes = 64LL * 1024 * 1024;
    /// Lowest budget: one largest access unit (RtpDepacketizer::kDefaultMaxAccessUnitBytes,
    /// 8 MiB, the bound on every receiver path) plus its parameter sets always fits
    static constexpr qint64 kMinMaxBytes = 16LL * 1024 * 1024;
    static constexpr qint64 kMaxMaxBytes = 256LL * 1024 * 1024;  ///< hard ceiling

    VideoPacketTap() = default;
    VideoPacketTap(const VideoPacketTap &) = delete;
    VideoPacketTap &operator=(const VideoPacketTap &) = delete;
    ~VideoPacketTap();

    /// A new consumer with a queue of capacity packets (1 to kMaxCapacity) and at most maxBytes
    /// queued bytes (kMinMaxBytes to kMaxMaxBytes). Its first packet is a keyframe. Null when
    /// kMaxConsumers are attached.
    std::shared_ptr<VideoPacketConsumer> attach(int capacity = kDefaultCapacity, qint64 maxBytes = kDefaultMaxBytes);
    void detach(const std::shared_ptr<VideoPacketConsumer> &consumer);
    int consumerCount() const { return _count.load(std::memory_order_relaxed); }
    bool hasConsumers() const { return consumerCount() > 0; }

    // Publisher side (the video receiver) ------------------------------------------------
    void publishPacket(VideoTapPacket packet);
    void publishEvent(const VideoTapEvent &event);

private:
    QList<std::shared_ptr<VideoPacketConsumer>> _snapshot();

    QMutex _publishMutex;           ///< held across one publish (never by attach or detach)
    mutable QMutex _mutex;
    QList<std::shared_ptr<VideoPacketConsumer>> _consumers;
    std::atomic<int> _count{0};
    quint64 _index = 0;
};
