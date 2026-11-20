/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QFile>
#include <QtCore/QLoggingCategory>
#include <QtCore/QMetaObject>
#include <QtCore/QMutex>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtMultimedia/QVideoFrame>
#include <QtMultimedia/QVideoSink>

#include <atomic>

#include "FFmpegSource.h"
#include "RtpDepacketizer.h"
#include "VideoPacketTap.h"
#include "VideoReceiver.h"

Q_DECLARE_LOGGING_CATEGORY(FFmpegReceiverLog)

class QThread;
struct AVCodecContext;
struct AVCodecParameters;
struct AVFrame;
struct AVPacket;
class QQuickWindow;

/// G88: GC's own video receiver on the FFmpeg 7.1 libraries Qt Multimedia ships.
///
/// One worker thread per stream. Input:
/// - UDP H.264 and H.265 ("udp://", "udp265://"): GC's own UDP socket and RtpDepacketizer
///   (design C1); a picture goes to the decoder when its RTP marker packet arrives.
/// - RTSP, MPEG-TS over UDP and MPEG-TS over TCP: libavformat (design C2), with options set
///   on that input only (FFmpegSource).
/// Decode with libavcodec, low delay, slice threads only, H.264 and H.265 only; only the
/// newest frame goes to the VideoOutput's QVideoSink (no timestamp pacing, no queue).
///
/// Diagnostics, off unless QGC_FFMPEG_STATS_DIR names a folder: per frame the steady-clock
/// times of the first packet at the socket (or av_read_frame), decoded, setVideoFrame and the
/// next QQuickWindow::frameSwapped, and per second the counters, as two CSV files there.
class FFmpegReceiver : public VideoReceiver
{
    Q_OBJECT

public:
    explicit FFmpegReceiver(QObject *parent = nullptr);
    ~FFmpegReceiver() override;

    static bool enabled();
    static void *createVideoSink(QQuickItem *widget, QObject *parent = nullptr);
    static void releaseVideoSink(void *sink);
    static VideoReceiver *createVideoReceiver(QObject *parent);

    /// FFmpeg library versions GC runs with, for the log ("avcodec 61.19.100 ...").
    static QString libraryVersions();
    /// True when the avcodec/avformat/avutil majors at run time match the headers.
    static bool librariesMatchHeaders();

    /// Largest picture the decoder accepts (width times height); bigger streams are refused.
    static constexpr qint64 kMaxPixels = 4096LL * 2304LL;

    /// G88 M3: RTSP, MPEG-TS and TCP share the UDP path's access unit bound. A larger demuxed
    /// packet is refused: not tapped and not decoded.
    static constexpr int kMaxAvformatPacketBytes = RtpDepacketizer::kDefaultMaxAccessUnitBytes;
    static constexpr bool avformatPacketAllowed(int size) { return (size >= 0) && (size <= kMaxAvformatPacketBytes); }

    /// m4080: the text shown when the UDP video port is held by another program
    static QString portInUseText(quint16 port);
    /// m4080: UDP bind retries back off from the first to the last delay (ms)
    static constexpr int kBindRetryFirstMs = 250;
    static constexpr int kBindRetryMaxMs = 2000;

    /// Path prefix of this run's CSV files (empty when diagnostics are off).
    QString statsFilePrefix() const { return _statsPrefix; }

    struct Counters
    {
        quint64 packets = 0;            ///< RTP packets (UDP) or demuxed packets (libavformat)
        quint64 decoded = 0;
        quint64 shown = 0;              ///< setVideoFrame calls
        quint64 superseded = 0;         ///< dropped because a newer frame arrived first
        quint64 convertFailed = 0;      ///< pixel format not handled by the bridge
        quint64 decodeErrors = 0;
        quint64 corruptPackets = 0;
        quint64 rtpMissed = 0;          ///< RTP packets missing (sequence gaps)
        quint64 reopens = 0;
        quint64 corruptFrames = 0;      ///< frames the decoder flagged as damaged
        quint64 droppedAccessUnits = 0; ///< UDP: incomplete, malformed, too large or waiting for a key frame; RTSP/TCP: too large
        quint64 malformedPackets = 0;   ///< UDP: RTP or payload structure that does not hold together
        quint64 refusedPackets = 0;     ///< UDP: not RTP, other payload type or sender, late, duplicate, unsupported
        quint64 swaps = 0;              ///< QQuickWindow::frameSwapped calls (diagnostics only)
        quint64 replacedOnSink = 0;     ///< set on the sink but replaced before a frame was drawn (diagnostics only)
        quint64 reorderRecovered = 0;   ///< UDP: RTP packets that came out of order and were put back (no loss)
        quint64 rtpLate = 0;            ///< UDP: RTP packets that came after the reorder window gave them up (in refused)
        quint64 rtpDuplicates = 0;      ///< UDP: second copies of an RTP packet (in refused)
        quint64 reorderHeld = 0;        ///< UDP: RTP packets that waited in the reorder window
        qint64 reorderHoldNsTotal = 0;  ///< UDP: their total wait
        qint64 reorderHoldNsMax = 0;    ///< UDP: the longest wait of one packet
    };

    Counters counters() const;

    /// G88 packet tap: every access unit as received, before decoding (VideoPacketTap.h)
    VideoPacketTap *packetTap() override { return &_tap; }

public slots:
    void start(uint32_t timeout) override;
    void stop() override;
    void startDecoding(void *sink) override;
    void stopDecoding() override;
    void startRecording(const QString &videoFile, VideoReceiver::FILE_FORMAT format) override;
    void stopRecording() override;
    void takeScreenshot(const QString &imageFile) override;

private:
    struct FrameTimes
    {
        quint64 index = 0;
        qint64 readNs = -1;     ///< first packet at the socket (UDP) or av_read_frame
        qint64 decodedNs = -1;
        qint64 bridgedNs = -1;
        qint64 setNs = -1;
        qint64 swapNs = -1;
        int width = 0;
        int height = 0;
    };

    void _workerAvformat(FFmpegSource source, QString logText);
    void _workerRtp(FFmpegSource source, quint64 startGeneration, QString tapSource);
    void _postReceiveStatus(quint64 startGeneration, RECEIVE_STATUS status, const QString &text);
    void _updateRtpCounters(const RtpDepacketizer::Stats &stats);
    bool _sleepUnlessStopped(int ms);
    AVCodecContext *_openDecoder(int codecId, const AVCodecParameters *par);
    void _decode(AVCodecContext *codecCtx, AVPacket *packet, AVFrame *frame, bool &loggedFormat);
    void _offerFrame(const QVideoFrame &frame, const FrameTimes &times);
    void _deliverPending();
    void _onBeforeSynchronizing();
    void _onFrameSwapped();
    void _flushStats(bool final);
    void _openStatsFiles();
    void _closeStatsFiles();
    void _recordDone(const FrameTimes &times);
    void _setStreaming(bool active);
    void _setDecoding(bool active);
    void _publishTapEvent(VideoTapEvent::Type type, const QString &source, bool codecKnown, VideoTapCodec codec, quint32 ssrc,
                          VideoTapEvent::StopReason reason = VideoTapEvent::StopReason::Stopped);
    qint64 _tapUtcMs(qint64 steadyNs) const;

    QThread *_thread = nullptr;
    std::atomic_bool _stopRequested{false};
    std::atomic<quint64> _startGeneration{0};   ///< m4080: a status posted by an older run is dropped
    std::atomic<qint64> _lastPacketNs{0};
    std::atomic<qint64> _lastFrameNs{0};

    mutable QMutex _mutex;              ///< guards everything below up to _counters
    QVideoFrame _pendingFrame;
    FrameTimes _pendingTimes;
    bool _pendingSet = false;
    QList<FrameTimes> _awaitingSwap;    ///< diagnostics: set on the sink, not yet synchronized
    QList<FrameTimes> _syncedForSwap;   ///< diagnostics: synchronized into the frame being rendered
    QList<FrameTimes> _done;            ///< diagnostics: ready for the CSV
    quint64 _timesDropped = 0;          ///< diagnostics: rows not kept (the lists are bounded)
    Counters _counters;

    bool _diagnostics = false;
    QPointer<QVideoSink> _videoSink;
    QPointer<QQuickWindow> _window;
    QMetaObject::Connection _swapConnection;
    QMetaObject::Connection _syncConnection;
    QVideoFrame _lastFrame;             ///< for takeScreenshot
    QSize _videoSize;
    QTimer _statsTimer;
    QTimer _watchdog;
    QFile _framesCsv;
    QFile _secondsCsv;
    QString _statsPrefix;
    Counters _lastSecond;
    qint64 _lastSecondNs = 0;

    VideoPacketTap _tap;
    std::atomic<quint64> _tapGeneration{0};
    std::atomic<qint64> _tapAnchorSteadyNs{0};    ///< this generation's clock anchor
    std::atomic<qint64> _tapAnchorUtcMs{0};
    QString _tapSource;                 ///< URI (no credentials) of the last start, GUI thread only
};
