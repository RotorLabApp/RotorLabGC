/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "FFmpegReceiver.h"
#include "FFmpegFrameBridge.h"
#include "FFmpegSource.h"
#include "RtpDepacketizer.h"
#include "QGCLoggingCategory.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QThread>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QUdpSocket>
#include <QtGui/QImage>
#include <QtMultimediaQuick/private/qquickvideooutput_p.h>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
}

QGC_LOGGING_CATEGORY(FFmpegReceiverLog, "qgc.videomanager.videoreceiver.ffmpeg")

namespace {

// Diagnostics lists are bounded: a hidden window draws nothing and nothing is swapped
constexpr qsizetype kMaxAwaitingSwap = 64;
constexpr qsizetype kMaxDoneRows = 4096;

qint64 nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

double ms(qint64 fromNs, qint64 toNs)
{
    return ((fromNs < 0) || (toNs < 0)) ? -1.0 : (toNs - fromNs) / 1.0e6;
}

QString avError(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    (void) av_strerror(err, buf, sizeof(buf));
    return QString::fromUtf8(buf);
}

// FFmpeg logs through one global callback. GC forwards warnings and errors to its own log,
// with credentials removed (an RTSP URL may appear in FFmpeg's text).
void avLogCallback(void *avcl, int level, const char *fmt, va_list vl)
{
    if (level > AV_LOG_WARNING) {
        return;
    }
    char line[1024];
    int prefix = 1;
    va_list copy;
    va_copy(copy, vl);
    (void) av_log_format_line2(avcl, level, fmt, copy, line, sizeof(line), &prefix);
    va_end(copy);
    line[sizeof(line) - 1] = '\0';
    const QString text = QString::fromUtf8(line).trimmed();
    qCDebug(FFmpegReceiverLog) << "ffmpeg:" << VideoReceiver::redactedUri(text);
}

void installAvLog()
{
    static std::once_flag once;
    std::call_once(once, []() { av_log_set_callback(avLogCallback); });
}

int interruptCallback(void *opaque)
{
    return static_cast<std::atomic_bool*>(opaque)->load() ? 1 : 0;
}

bool supportedCodec(int codecId)
{
    // Drone video is H.264 or H.265. Other decoders are never opened on network input.
    return (codecId == AV_CODEC_ID_H264) || (codecId == AV_CODEC_ID_HEVC);
}

} // namespace

FFmpegReceiver::FFmpegReceiver(QObject *parent)
    : VideoReceiver(parent)
{
    _statsTimer.setInterval(1000);
    (void) connect(&_statsTimer, &QTimer::timeout, this, [this]() { _flushStats(false); });

    _watchdog.setInterval(250);
    (void) connect(&_watchdog, &QTimer::timeout, this, [this]() {
        const qint64 now = nowNs();
        const qint64 limitNs = static_cast<qint64>(qMax<uint32_t>(_timeout, 1)) * 1000000000LL;
        const qint64 lastPacket = _lastPacketNs.load();
        const bool packets = (lastPacket > 0) && ((now - lastPacket) < limitNs);
        if (packets && !_streaming) {
            _setStreaming(true);
        } else if (!packets && _streaming) {
            qCDebug(FFmpegReceiverLog) << "No packets for" << _timeout << "s" << _name;
            _setStreaming(false);
            emit timeout();
        }
        // No new picture for the timeout: never leave a frozen picture up as if it were live
        const qint64 lastFrame = _lastFrameNs.load();
        if (_decoding && ((lastFrame <= 0) || ((now - lastFrame) >= limitNs))) {
            qCInfo(FFmpegReceiverLog) << "No new picture for" << _timeout << "s, video area back to waiting" << _name;
            if (_videoSink) {
                _videoSink->setVideoFrame(QVideoFrame());
            }
            _setDecoding(false);
        }
    });
}

FFmpegReceiver::~FFmpegReceiver()
{
    if (_thread) {
        _stopRequested = true;
        _thread->wait();
        delete _thread;
        _thread = nullptr;
    }
    _flushStats(true);
    _closeStatsFiles();
}

bool FFmpegReceiver::enabled()
{
    return true;
}

void *FFmpegReceiver::createVideoSink(QQuickItem *widget, QObject *parent)
{
    Q_UNUSED(parent);

    if (!widget) {
        return nullptr;
    }

    // G83: only a real VideoOutput has a video sink
    QQuickVideoOutput *const videoOutput = qobject_cast<QQuickVideoOutput*>(widget);
    if (!videoOutput) {
        qCCritical(FFmpegReceiverLog) << "Video widget" << widget->objectName() << "is a" << widget->metaObject()->className() << "and not a VideoOutput: no video sink";
        return nullptr;
    }

    if (!librariesMatchHeaders()) {
        qCCritical(FFmpegReceiverLog) << "FFmpeg libraries do not match the headers GC was built with:" << libraryVersions();
        return nullptr;
    }

    return videoOutput->videoSink();
}

void FFmpegReceiver::releaseVideoSink(void *sink)
{
    Q_UNUSED(sink);   // owned by the QML VideoOutput
}

VideoReceiver *FFmpegReceiver::createVideoReceiver(QObject *parent)
{
    Q_UNUSED(parent);
    return new FFmpegReceiver(nullptr);
}

QString FFmpegReceiver::libraryVersions()
{
    auto v = [](unsigned x) { return QStringLiteral("%1.%2.%3").arg(x >> 16).arg((x >> 8) & 0xff).arg(x & 0xff); };
    return QStringLiteral("avcodec %1 (headers %2), avformat %3 (headers %4), avutil %5 (headers %6)")
        .arg(v(avcodec_version()), v(LIBAVCODEC_VERSION_INT), v(avformat_version()), v(LIBAVFORMAT_VERSION_INT),
             v(avutil_version()), v(LIBAVUTIL_VERSION_INT));
}

bool FFmpegReceiver::librariesMatchHeaders()
{
    return ((avcodec_version() >> 16) == LIBAVCODEC_VERSION_MAJOR) &&
           ((avformat_version() >> 16) == LIBAVFORMAT_VERSION_MAJOR) &&
           ((avutil_version() >> 16) == LIBAVUTIL_VERSION_MAJOR);
}

QString FFmpegReceiver::portInUseText(quint16 port)
{
    return tr("Video is not received: UDP port %1 is in use by another program on this computer. Close it or choose another port in Video settings.").arg(port);
}

FFmpegReceiver::Counters FFmpegReceiver::counters() const
{
    QMutexLocker locker(&_mutex);
    return _counters;
}

void FFmpegReceiver::start(uint32_t timeout)
{
    qCDebug(FFmpegReceiverLog) << Q_FUNC_INFO << _name;

    if (_thread) {
        qCDebug(FFmpegReceiverLog) << "Already running!";
        emit onStartComplete(STATUS_INVALID_STATE);
        return;
    }

    const FFmpegSource source = FFmpegSource::fromUri(_uri, _lowLatency);
    if (!source.isValid()) {
        qCWarning(FFmpegReceiverLog) << "Cannot open" << FFmpegSource::logText(_uri) << ":" << source.error;
        emit onStartComplete(STATUS_INVALID_URL);
        return;
    }

    // This build cannot play the source: G83 tells the user (VideoManager)
    if (!librariesMatchHeaders()) {
        qCCritical(FFmpegReceiverLog) << "FFmpeg version mismatch:" << libraryVersions();
        emit onStartComplete(STATUS_NOT_IMPLEMENTED);
        return;
    }
    if ((source.kind == FFmpegSource::Kind::Rtp) &&
        !avcodec_find_decoder((source.codecName == QLatin1String("hevc")) ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264)) {
        qCCritical(FFmpegReceiverLog) << "No" << source.codecName << "decoder in" << libraryVersions();
        emit onStartComplete(STATUS_NOT_IMPLEMENTED);
        return;
    }

    installAvLog();
    qCInfo(FFmpegReceiverLog) << "Starting" << _name << FFmpegSource::logText(_uri) << "|" << libraryVersions();

    // Packet tap: a start on another URI is a source change for the consumers
    const QString tapSource = FFmpegSource::logText(_uri);
    if (!_tapSource.isEmpty() && (tapSource != _tapSource)) {
        _publishTapEvent(VideoTapEvent::Type::SourceChanged, tapSource, false, VideoTapCodec::H264, 0);
    }
    _tapSource = tapSource;

    _timeout = timeout;
    _stopRequested = false;
    const quint64 startGeneration = ++_startGeneration;
    setReceiveStatus(RECEIVE_STATUS_OK, QString());
    _lastPacketNs = 0;
    _lastFrameNs = 0;
    {
        QMutexLocker locker(&_mutex);
        _counters = Counters();
        _pendingSet = false;
        _pendingFrame = QVideoFrame();
        _awaitingSwap.clear();
        _syncedForSwap.clear();
        _done.clear();
        _timesDropped = 0;
    }
    _lastSecond = Counters();
    _lastSecondNs = nowNs();
    _openStatsFiles();

    // G88 M1: the workers get everything they need from the URI here, on the GUI thread.
    // No worker code may read _uri, _name or any other VideoReceiver member that has a setter:
    // VideoManager can call setUri while the receiver runs (and a worker may still be in its
    // bind loop for seconds), so such a read races the assignment.
    if (source.kind == FFmpegSource::Kind::Rtp) {
        _thread = QThread::create([this, source, startGeneration, tapSource]() { _workerRtp(source, startGeneration, tapSource); });
    } else {
        _thread = QThread::create([this, source, tapSource]() { _workerAvformat(source, tapSource); });
    }
    _thread->setObjectName(QStringLiteral("FFmpegReceiver-%1").arg(_name));
    _thread->start(QThread::HighPriority);

    _statsTimer.start();
    _watchdog.start();

    emit onStartComplete(STATUS_OK);
}

void FFmpegReceiver::stop()
{
    qCDebug(FFmpegReceiverLog) << Q_FUNC_INFO << _name;

    if (!_thread) {
        emit onStopComplete(STATUS_INVALID_STATE);
        return;
    }

    _stopRequested = true;
    _thread->wait();
    delete _thread;
    _thread = nullptr;
    ++_startGeneration;
    setReceiveStatus(RECEIVE_STATUS_OK, QString());

    _statsTimer.stop();
    _watchdog.stop();
    _flushStats(true);
    _closeStatsFiles();

    _setStreaming(false);
    _setDecoding(false);

    emit onStopComplete(STATUS_OK);
}

void FFmpegReceiver::startDecoding(void *sink)
{
    qCDebug(FFmpegReceiverLog) << Q_FUNC_INFO << _name;

    if (!sink) {
        qCCritical(FFmpegReceiverLog) << "VideoSink is NULL";
        emit onStartDecodingComplete(STATUS_FAIL);
        return;
    }

    _videoSink = static_cast<QVideoSink*>(sink);
    _videoSink->setSubtitleText(QString());

    if (_swapConnection) {
        (void) disconnect(_swapConnection);
    }
    if (_syncConnection) {
        (void) disconnect(_syncConnection);
    }
    _window = (_diagnostics && _widget) ? _widget->window() : nullptr;
    if (_window) {
        // Diagnostics only. Both are emitted on the render thread: record there, directly. A frame
        // counts as shown by a swap only if it was on the sink when that frame was synchronized.
        _syncConnection = connect(_window, &QQuickWindow::beforeSynchronizing, this, &FFmpegReceiver::_onBeforeSynchronizing, Qt::DirectConnection);
        _swapConnection = connect(_window, &QQuickWindow::frameSwapped, this, &FFmpegReceiver::_onFrameSwapped, Qt::DirectConnection);
    }

    emit onStartDecodingComplete(STATUS_OK);
}

void FFmpegReceiver::stopDecoding()
{
    qCDebug(FFmpegReceiverLog) << Q_FUNC_INFO << _name;

    if (_swapConnection) {
        (void) disconnect(_swapConnection);
    }
    if (_syncConnection) {
        (void) disconnect(_syncConnection);
    }
    _window = nullptr;
    _videoSink = nullptr;
    _setDecoding(false);

    emit onStopDecodingComplete(STATUS_OK);
}

void FFmpegReceiver::startRecording(const QString &videoFile, FILE_FORMAT format)
{
    Q_UNUSED(videoFile);
    Q_UNUSED(format);
    // G88b: recording remuxes the packets with libavformat; not in G88
    emit onStartRecordingComplete(STATUS_NOT_IMPLEMENTED);
}

void FFmpegReceiver::stopRecording()
{
    emit onStopRecordingComplete(STATUS_NOT_IMPLEMENTED);
}

void FFmpegReceiver::takeScreenshot(const QString &imageFile)
{
    if (!_lastFrame.isValid()) {
        emit onTakeScreenshotComplete(STATUS_INVALID_STATE);
        return;
    }
    const QImage image = _lastFrame.toImage();
    emit onTakeScreenshotComplete((!image.isNull() && image.save(imageFile)) ? STATUS_OK : STATUS_FAIL);
}

// Worker thread ---------------------------------------------------------------------

bool FFmpegReceiver::_sleepUnlessStopped(int ms)
{
    for (int waited = 0; (waited < ms) && !_stopRequested; waited += 50) {
        QThread::msleep(50);
    }
    return !_stopRequested;
}

void FFmpegReceiver::_postReceiveStatus(quint64 startGeneration, RECEIVE_STATUS status, const QString &text)
{
    // To the GUI thread; a status from a run that was stopped or restarted meanwhile is dropped
    (void) QMetaObject::invokeMethod(this, [this, startGeneration, status, text]() {
        if (startGeneration == _startGeneration.load()) {
            setReceiveStatus(status, text);
        }
    }, Qt::QueuedConnection);
}

AVCodecContext *FFmpegReceiver::_openDecoder(int codecId, const AVCodecParameters *par)
{
    if (!supportedCodec(codecId)) {
        qCWarning(FFmpegReceiverLog) << "Not an H.264 or H.265 stream, codec id" << codecId;
        return nullptr;
    }
    const AVCodec *codec = avcodec_find_decoder(static_cast<AVCodecID>(codecId));
    if (!codec) {
        qCWarning(FFmpegReceiverLog) << "No decoder for codec id" << codecId;
        return nullptr;
    }
    AVCodecContext *codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) {
        return nullptr;
    }
    if (par && (avcodec_parameters_to_context(codecCtx, par) < 0)) {
        avcodec_free_context(&codecCtx);
        return nullptr;
    }
    // Design C2: no frame threading (it delays output by a frame per thread), low-delay
    // output, and the packet's read time carried to its frame. The picture size is bounded:
    // a stream that declares a larger one is refused by the decoder.
    codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY | AV_CODEC_FLAG_COPY_OPAQUE;
    codecCtx->thread_type = FF_THREAD_SLICE;
    codecCtx->thread_count = 0;
    codecCtx->max_pixels = kMaxPixels;
    const int ret = avcodec_open2(codecCtx, codec, nullptr);
    if (ret < 0) {
        qCWarning(FFmpegReceiverLog) << "Cannot open decoder" << codec->name << avError(ret);
        avcodec_free_context(&codecCtx);
        return nullptr;
    }
    qCInfo(FFmpegReceiverLog) << "Opened decoder" << codec->name;
    return codecCtx;
}

void FFmpegReceiver::_decode(AVCodecContext *codecCtx, AVPacket *packet, AVFrame *frame, bool &loggedFormat)
{
    const int sent = avcodec_send_packet(codecCtx, packet);
    av_packet_unref(packet);
    if ((sent < 0) && (sent != AVERROR(EAGAIN))) {
        QMutexLocker locker(&_mutex);
        _counters.decodeErrors++;
    }

    while (avcodec_receive_frame(codecCtx, frame) == 0) {
        FrameTimes times;
        times.decodedNs = nowNs();
        times.readNs = static_cast<qint64>(reinterpret_cast<intptr_t>(frame->opaque));
        times.width = frame->width;
        times.height = frame->height;
        if (frame->decode_error_flags || (frame->flags & AV_FRAME_FLAG_CORRUPT)) {
            QMutexLocker locker(&_mutex);
            _counters.corruptFrames++;
        }
        if (!loggedFormat) {
            qCInfo(FFmpegReceiverLog) << "First frame" << frame->width << "x" << frame->height << FFmpegFrameBridge::pixelFormatName(frame);
            loggedFormat = true;
        }
        const QVideoFrame videoFrame = FFmpegFrameBridge::toVideoFrame(frame);
        times.bridgedNs = nowNs();
        if (!videoFrame.isValid()) {
            QMutexLocker locker(&_mutex);
            _counters.convertFailed++;
        } else {
            _offerFrame(videoFrame, times);
        }
        av_frame_unref(frame);
    }
}

// RTSP, MPEG-TS over UDP and MPEG-TS over TCP: libavformat (design C2). Reopens after an
// error, waiting longer each time up to 4 s.
void FFmpegReceiver::_workerAvformat(FFmpegSource source, QString logText)
{
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!packet || !frame) {
        av_frame_free(&frame);
        av_packet_free(&packet);
        return;
    }
    bool loggedFormat = false;
    bool first = true;
    int backoffMs = 500;
    VideoParameterSets parameterSets;

    while (!_stopRequested) {
        if (!first) {
            {
                QMutexLocker locker(&_mutex);
                _counters.reopens++;
            }
            if (!_sleepUnlessStopped(backoffMs)) {
                break;
            }
            backoffMs = qMin(backoffMs * 2, 4000);
        }
        first = false;

        AVFormatContext *fmt = avformat_alloc_context();
        if (!fmt) {
            continue;
        }
        fmt->interrupt_callback.callback = interruptCallback;
        fmt->interrupt_callback.opaque = &_stopRequested;

        AVDictionary *dict = nullptr;
        for (const auto &kv : std::as_const(source.formatOptions)) {
            (void) av_dict_set(&dict, kv.first.constData(), kv.second.constData(), 0);
        }
        // Only the H.264 and H.265 decoders, also while FFmpeg probes the streams
        (void) av_dict_set(&dict, "codec_whitelist", "h264,hevc", 0);

        const AVInputFormat *inputFormat = source.inputFormat.isEmpty() ? nullptr : av_find_input_format(source.inputFormat.constData());
        int ret = avformat_open_input(&fmt, source.url.constData(), inputFormat, &dict);   // frees fmt on failure
        if (dict) {
            const AVDictionaryEntry *e = nullptr;
            while ((e = av_dict_iterate(dict, e))) {
                qCDebug(FFmpegReceiverLog) << "Option not used by the demuxer:" << e->key;
            }
            av_dict_free(&dict);
        }

        AVCodecContext *codecCtx = nullptr;
        int videoStream = -1;
        bool tapStarted = false;
        bool tapFirstUnit = true;
        bool tapGap = false;            // a unit was refused: tap again from the next keyframe
        bool loggedOversized = false;
        VideoTapCodec tapCodec = VideoTapCodec::H264;
        AVRational timeBase{ 1, 90000 };
        if (ret < 0) {
            if (!_stopRequested) {
                qCWarning(FFmpegReceiverLog) << "Cannot open" << logText << ":" << avError(ret);
            }
        } else {
            videoStream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            if (videoStream < 0) {
                // MPEG-TS: the streams are known once the PMT has been read (bounded by probesize)
                (void) avformat_find_stream_info(fmt, nullptr);
                videoStream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            }
            if (videoStream < 0) {
                qCWarning(FFmpegReceiverLog) << "No video stream in" << logText;
                ret = AVERROR_STREAM_NOT_FOUND;
            } else {
                const AVCodecParameters *par = fmt->streams[videoStream]->codecpar;
                for (unsigned i = 0; i < fmt->nb_streams; ++i) {
                    if (static_cast<int>(i) != videoStream) {
                        fmt->streams[i]->discard = AVDISCARD_ALL;
                    }
                }
                if ((static_cast<qint64>(par->width) * par->height) > kMaxPixels) {
                    qCWarning(FFmpegReceiverLog) << "Picture" << par->width << "x" << par->height << "is larger than GC shows";
                    ret = AVERROR_INVALIDDATA;
                } else {
                    codecCtx = _openDecoder(par->codec_id, par);
                    if (!codecCtx) {
                        ret = AVERROR_DECODER_NOT_FOUND;
                    } else {
                        tapCodec = (par->codec_id == AV_CODEC_ID_HEVC) ? VideoTapCodec::H265 : VideoTapCodec::H264;
                        parameterSets.setCodec(tapCodec);
                        parameterSets.clear();
                        if (par->extradata && (par->extradata_size > 3) && (par->extradata_size < (1 << 20)) && (par->extradata[0] == 0)) {
                            // Annex B extradata (RTSP sprop parameter sets); avcC/hvcC start with 1 and are skipped
                            (void) parameterSets.scanAnnexB(QByteArray::fromRawData(reinterpret_cast<const char*>(par->extradata), par->extradata_size));
                        }
                        timeBase = fmt->streams[videoStream]->time_base;
                        tapStarted = true;
                        _publishTapEvent(VideoTapEvent::Type::StreamStarted, logText, true, tapCodec, 0);
                    }
                }
            }
        }

        while ((ret >= 0) && !_stopRequested) {
            ret = av_read_frame(fmt, packet);
            if (ret == AVERROR(EAGAIN)) {
                ret = 0;
                continue;
            }
            if (ret < 0) {
                if (!_stopRequested) {
                    qCWarning(FFmpegReceiverLog) << "Read failed, reopening:" << avError(ret);
                }
                break;
            }

            const qint64 readNs = nowNs();
            _lastPacketNs = readNs;
            {
                QMutexLocker locker(&_mutex);
                _counters.packets++;
                if (packet->flags & AV_PKT_FLAG_CORRUPT) {
                    _counters.corruptPackets++;
                }
            }

            if (packet->stream_index != videoStream) {
                av_packet_unref(packet);
                continue;
            }

            // G88 M3: the same access unit bound as the UDP path (RtpDepacketizer). A larger
            // packet is neither tapped nor decoded; the tap resumes at the next keyframe.
            if (!avformatPacketAllowed(packet->size)) {
                {
                    QMutexLocker locker(&_mutex);
                    _counters.droppedAccessUnits++;
                }
                if (!loggedOversized) {
                    loggedOversized = true;
                    qCWarning(FFmpegReceiverLog) << "Refused a packet of" << packet->size << "bytes (bound"
                                                 << kMaxAvformatPacketBytes << ") from" << logText;
                }
                tapGap = true;
                av_packet_unref(packet);
                continue;
            }

            const bool keyFrame = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            if (keyFrame && (packet->size > 0)) {
                // In-band parameter sets come with keyframes: scanned once a GOP, consumers or not
                (void) parameterSets.scanAnnexB(QByteArray::fromRawData(reinterpret_cast<const char*>(packet->data), packet->size));
            }
            if (tapGap && keyFrame) {
                tapGap = false;
                tapFirstUnit = true;    // the unit after a refused one is a discontinuity
            }
            if (_tap.hasConsumers() && (packet->size > 0) && !tapGap) {
                VideoTapPacket tapPacket;
                tapPacket.codec = tapCodec;
                tapPacket.data = QByteArray(reinterpret_cast<const char*>(packet->data), packet->size);   // a copy of its own
                tapPacket.parameterSets = parameterSets.annexB();
                tapPacket.keyFrame = keyFrame;
                if (packet->pts != AV_NOPTS_VALUE) {
                    tapPacket.pts90k = av_rescale_q(packet->pts, timeBase, AVRational{ 1, 90000 });
                }
                if (packet->dts != AV_NOPTS_VALUE) {
                    tapPacket.dts90k = av_rescale_q(packet->dts, timeBase, AVRational{ 1, 90000 });
                }
                tapPacket.discontinuity = tapFirstUnit;
                tapPacket.receiveSteadyNs = readNs;
                tapPacket.receiveUtcMs = _tapUtcMs(readNs);
                tapPacket.generation = _tapGeneration.load();
                _tap.publishPacket(std::move(tapPacket));
            }
            tapFirstUnit = false;

            packet->opaque = reinterpret_cast<void*>(static_cast<intptr_t>(readNs));
            _decode(codecCtx, packet, frame, loggedFormat);
            backoffMs = 500;
        }

        if (tapStarted) {
            _publishTapEvent(VideoTapEvent::Type::StreamStopped, logText, true, tapCodec, 0,
                             _stopRequested ? VideoTapEvent::StopReason::Stopped : VideoTapEvent::StopReason::Error);
        }
        if (codecCtx) {
            avcodec_free_context(&codecCtx);
        }
        if (fmt) {
            avformat_close_input(&fmt);
        }
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
}

// UDP H.264 and H.265 (design C1): GC's own UDP socket and RTP depacketizer. An access unit
// goes to the decoder when its marker packet arrives. Times are taken at the socket.
void FFmpegReceiver::_workerRtp(FFmpegSource source, quint64 startGeneration, QString tapSource)
{
    const bool h265 = (source.codecName == QLatin1String("hevc"));
    QHostAddress address;
    if (!address.setAddress(source.host)) {
        address = QHostAddress(QHostAddress::AnyIPv4);
    }
    const bool multicast = address.isMulticast();

    QUdpSocket socket;
    const QHostAddress bindAddress = multicast ? ((address.protocol() == QAbstractSocket::IPv6Protocol) ? QHostAddress(QHostAddress::AnyIPv6) : QHostAddress(QHostAddress::AnyIPv4)) : address;
    // m4080: unicast never shares its port. With address reuse a second GC on the same port
    // would bind and then get nothing; without it the bind fails and the user is told why.
    // Multicast is shared on purpose: every listener gets the group's datagrams.
    const QAbstractSocket::BindMode bindMode = multicast ? (QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)
                                                         : QAbstractSocket::BindMode(QUdpSocket::DontShareAddress);
    const quint16 port = static_cast<quint16>(source.port);
    int bindTries = 0;
    int backoffMs = kBindRetryFirstMs;
    bool reportedInUse = false;
    while (!_stopRequested && !socket.bind(bindAddress, port, bindMode)) {
        const QAbstractSocket::SocketError error = socket.error();
        // Windows reports a port held exclusively by another socket as an access error
        bool inUse = (error == QAbstractSocket::AddressInUseError);
#ifdef Q_OS_WIN
        inUse = inUse || (error == QAbstractSocket::SocketAccessError);
#endif
        if ((bindTries++ % 10) == 0) {
            qCWarning(FFmpegReceiverLog) << "Cannot bind UDP" << source.host << port << socket.errorString() << "retrying";
        }
        // Once per start: the status stays until the bind succeeds or the receiver stops
        if (inUse && !reportedInUse) {
            reportedInUse = true;
            _postReceiveStatus(startGeneration, RECEIVE_STATUS_PORT_IN_USE, portInUseText(port));
        }
        socket.abort();
        (void) _sleepUnlessStopped(backoffMs);
        backoffMs = std::min(backoffMs * 2, kBindRetryMaxMs);
    }
    if (_stopRequested) {
        return;
    }
    if (reportedInUse) {
        qCInfo(FFmpegReceiverLog) << "UDP port" << port << "is free again, bound after" << bindTries << "tries";
        _postReceiveStatus(startGeneration, RECEIVE_STATUS_OK, QString());
    }
    if (multicast && !socket.joinMulticastGroup(address)) {
        qCWarning(FFmpegReceiverLog) << "Cannot join multicast group" << source.host << socket.errorString();
    }
    socket.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 4 * 1024 * 1024);
    qCInfo(FFmpegReceiverLog) << "Listening for RTP" << source.codecName << "on" << source.host << source.port
                              << "receive buffer" << socket.socketOption(QAbstractSocket::ReceiveBufferSizeSocketOption).toInt();

    AVCodecContext *codecCtx = _openDecoder(h265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264, nullptr);
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!codecCtx || !packet || !frame) {
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codecCtx);
        return;
    }
    bool loggedFormat = false;
    const VideoTapCodec tapCodec = h265 ? VideoTapCodec::H265 : VideoTapCodec::H264;
    _publishTapEvent(VideoTapEvent::Type::StreamStarted, tapSource, true, tapCodec, 0);
    quint32 tapSsrc = 0;
    bool tapTimedOut = false;       // StreamStopped(Timeout) sent; StreamStarted when packets come back
    bool tapFirstUnit = true;       // the next unit starts a generation
    const qint64 timeoutNs = static_cast<qint64>(qMax<uint32_t>(_timeout, 1)) * 1000000000LL;

    RtpDepacketizer depacketizer(h265 ? RtpDepacketizer::Codec::H265 : RtpDepacketizer::Codec::H264);
    QList<RtpDepacketizer::AccessUnit> units;
    // One fixed buffer: a UDP payload is at most 65,507 bytes, so nothing is ever cut short
    QByteArray buffer(RtpDepacketizer::kMaxPacketBytes, Qt::Uninitialized);
    qint64 lastArrival = 0;

    // Complete access units to the tap and the decoder, in order
    auto deliver = [&]() {
        for (const RtpDepacketizer::AccessUnit &unit : std::as_const(units)) {
            if (unit.ssrc != tapSsrc) {
                if (tapSsrc != 0) {
                    _publishTapEvent(VideoTapEvent::Type::SsrcChanged, tapSource, true, tapCodec, unit.ssrc);
                    tapFirstUnit = true;
                }
                tapSsrc = unit.ssrc;
            }
            // A unit after a real loss (packets given up, or units dropped) is a discontinuity;
            // packets the reorder window put back in order are not. It is a keyframe: the
            // depacketizer waits for one after a loss.
            const bool discontinuity = tapFirstUnit || unit.afterLoss;
            tapFirstUnit = false;
            if (_tap.hasConsumers()) {
                // The access unit's bytes are shared, never copied: the depacketizer starts a
                // new buffer for every unit and never writes this one again
                VideoTapPacket tapPacket;
                tapPacket.codec = tapCodec;
                tapPacket.data = unit.data;
                tapPacket.parameterSets = unit.parameterSets;
                tapPacket.keyFrame = unit.keyFrame;
                tapPacket.hasRtpTimestamp = true;
                tapPacket.rtpTimestamp = unit.rtpTimestamp;
                tapPacket.pts90k = unit.rtpTimestamp;   // no B-frames on this path: dts is pts
                tapPacket.dts90k = unit.rtpTimestamp;
                tapPacket.discontinuity = discontinuity;
                tapPacket.receiveSteadyNs = unit.firstPacketNs;
                tapPacket.receiveUtcMs = _tapUtcMs(unit.firstPacketNs);
                tapPacket.ssrc = unit.ssrc;
                tapPacket.generation = _tapGeneration.load();
                _tap.publishPacket(std::move(tapPacket));
            }
            if (av_new_packet(packet, static_cast<int>(unit.data.size())) < 0) {
                continue;
            }
            std::memcpy(packet->data, unit.data.constData(), static_cast<size_t>(unit.data.size()));
            packet->opaque = reinterpret_cast<void*>(static_cast<intptr_t>(unit.firstPacketNs));
            packet->pts = unit.rtpTimestamp;
            packet->dts = AV_NOPTS_VALUE;
            if (unit.keyFrame) {
                packet->flags |= AV_PKT_FLAG_KEY;
            }
            _decode(codecCtx, packet, frame, loggedFormat);
        }
        units.clear();
    };

    while (!_stopRequested) {
        // Wake for the reorder window's next deadline too: a held packet never waits on the socket
        int waitMs = 100;
        const qint64 deadline = depacketizer.nextDeadlineNs();
        if (deadline > 0) {
            const qint64 untilMs = (deadline - nowNs() + 999999) / 1000000;
            waitMs = (untilMs <= 0) ? 0 : static_cast<int>(std::min<qint64>(untilMs, 100));
        }
        if (!socket.hasPendingDatagrams() && ((waitMs == 0) || !socket.waitForReadyRead(waitMs))) {
            if (depacketizer.heldPackets() > 0) {
                depacketizer.poll(nowNs(), units);
                deliver();
            }
            // Told on this thread, in order with the units: consumers close their part here
            if (!tapTimedOut && (lastArrival > 0) && ((nowNs() - lastArrival) >= timeoutNs)) {
                _publishTapEvent(VideoTapEvent::Type::StreamStopped, tapSource, true, tapCodec, tapSsrc, VideoTapEvent::StopReason::Timeout);
                tapTimedOut = true;
            }
            _updateRtpCounters(depacketizer.stats());
            continue;
        }
        // Bounded work per wake, so a flood cannot keep this loop from seeing a stop
        for (int n = 0; (n < 512) && socket.hasPendingDatagrams() && !_stopRequested; ++n) {
            const qint64 got = socket.readDatagram(buffer.data(), buffer.size());
            const qint64 arrival = nowNs();
            if (got <= 0) {
                continue;
            }
            if ((lastArrival > 0) && ((arrival - lastArrival) > 1000000000LL)) {
                depacketizer.poll(arrival, units);   // anything still held goes first
                deliver();
                depacketizer.reset();   // a pause of a second or more: maybe a new sender
            }
            lastArrival = arrival;
            _lastPacketNs = arrival;
            if (tapTimedOut) {
                _publishTapEvent(VideoTapEvent::Type::StreamStarted, tapSource, true, tapCodec, 0);
                tapTimedOut = false;
                tapFirstUnit = true;
                tapSsrc = 0;
            }

            depacketizer.push(QByteArray::fromRawData(buffer.constData(), static_cast<qsizetype>(got)), arrival, units);
            deliver();
        }

        _updateRtpCounters(depacketizer.stats());
    }

    if (!tapTimedOut) {
        _publishTapEvent(VideoTapEvent::Type::StreamStopped, tapSource, true, tapCodec, tapSsrc, VideoTapEvent::StopReason::Stopped);
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codecCtx);
}

void FFmpegReceiver::_updateRtpCounters(const RtpDepacketizer::Stats &s)
{
    QMutexLocker locker(&_mutex);
    _counters.packets = s.packets;
    _counters.rtpMissed = s.packetsMissing;
    _counters.droppedAccessUnits = s.droppedAccessUnits;
    _counters.malformedPackets = s.malformed;
    _counters.refusedPackets = s.refused;
    _counters.reorderRecovered = s.reordered;
    _counters.rtpLate = s.late;
    _counters.rtpDuplicates = s.duplicates;
    _counters.reorderHeld = s.held;
    _counters.reorderHoldNsTotal = s.holdNsTotal;
    _counters.reorderHoldNsMax = s.holdNsMax;
}

void FFmpegReceiver::_publishTapEvent(VideoTapEvent::Type type, const QString &source, bool codecKnown, VideoTapCodec codec, quint32 ssrc,
                                      VideoTapEvent::StopReason reason)
{
    const qint64 steady = nowNs();
    if (type != VideoTapEvent::Type::StreamStopped) {
        // A new generation, with its own wall clock anchor. Published before its first unit.
        _tapAnchorSteadyNs = steady;
        _tapAnchorUtcMs = QDateTime::currentMSecsSinceEpoch();
        _tapGeneration++;
    }
    if (!_tap.hasConsumers()) {
        return;
    }
    VideoTapEvent event;
    event.type = type;
    event.stopReason = reason;
    event.generation = _tapGeneration.load();
    event.codec = codec;
    event.codecKnown = codecKnown;
    event.source = source;
    event.ssrc = ssrc;
    event.steadyNs = steady;
    event.utcAnchorMs = _tapAnchorUtcMs.load();
    event.steadyAnchorNs = _tapAnchorSteadyNs.load();
    _tap.publishEvent(event);
}

qint64 FFmpegReceiver::_tapUtcMs(qint64 steadyNs) const
{
    return _tapAnchorUtcMs.load() + (steadyNs - _tapAnchorSteadyNs.load()) / 1000000;
}

void FFmpegReceiver::_offerFrame(const QVideoFrame &frame, const FrameTimes &times)
{
    bool schedule = false;
    {
        QMutexLocker locker(&_mutex);
        _counters.decoded++;
        FrameTimes t = times;
        t.index = _counters.decoded;
        if (_pendingSet) {
            // Live policy: never queue. The older frame was never shown.
            _counters.superseded++;
            _recordDone(_pendingTimes);
        } else {
            schedule = true;
        }
        _pendingFrame = frame;
        _pendingTimes = t;
        _pendingSet = true;
    }
    if (schedule) {
        (void) QMetaObject::invokeMethod(this, &FFmpegReceiver::_deliverPending, Qt::QueuedConnection);
    }
}

// Called with _mutex held
void FFmpegReceiver::_recordDone(const FrameTimes &times)
{
    if (!_diagnostics) {
        return;
    }
    if (_done.size() >= kMaxDoneRows) {
        _timesDropped++;
        return;
    }
    _done.append(times);
}

// Main thread -----------------------------------------------------------------------

void FFmpegReceiver::_deliverPending()
{
    QVideoFrame frame;
    FrameTimes times;
    {
        QMutexLocker locker(&_mutex);
        if (!_pendingSet) {
            return;
        }
        frame = _pendingFrame;
        times = _pendingTimes;
        _pendingFrame = QVideoFrame();
        _pendingSet = false;
    }

    if (!_videoSink || !_thread) {
        QMutexLocker locker(&_mutex);
        _recordDone(times);
        return;
    }

    times.setNs = nowNs();
    _videoSink->setVideoFrame(frame);
    _lastFrame = frame;
    _lastFrameNs = times.setNs;

    {
        QMutexLocker locker(&_mutex);
        _counters.shown++;
        if (_diagnostics && _window) {
            _awaitingSwap.append(times);
            while (_awaitingSwap.size() > kMaxAwaitingSwap) {
                _recordDone(_awaitingSwap.takeFirst());
            }
        } else {
            _recordDone(times);
        }
    }

    const QSize size(times.width, times.height);
    if (size != _videoSize) {
        _videoSize = size;
        emit videoSizeChanged(size);
    }
    _setDecoding(true);
}

void FFmpegReceiver::_onBeforeSynchronizing()
{
    // Render thread, GUI thread blocked: the frames on the sink now go into this frame.
    // Of several, only the last is drawn; the others were superseded on the sink.
    QMutexLocker locker(&_mutex);
    _syncedForSwap.append(_awaitingSwap);
    _awaitingSwap.clear();
    while (_syncedForSwap.size() > kMaxAwaitingSwap) {
        _recordDone(_syncedForSwap.takeFirst());
    }
}

void FFmpegReceiver::_onFrameSwapped()
{
    const qint64 t = nowNs();
    QMutexLocker locker(&_mutex);
    _counters.swaps++;
    for (qsizetype i = 0; i < _syncedForSwap.size(); ++i) {
        FrameTimes times = _syncedForSwap.at(i);
        if (i + 1 == _syncedForSwap.size()) {
            times.swapNs = t;
        } else {
            _counters.replacedOnSink++;
        }
        _recordDone(times);
    }
    _syncedForSwap.clear();
}

void FFmpegReceiver::_setStreaming(bool active)
{
    if (active != _streaming) {
        _streaming = active;
        emit streamingChanged(active);
    }
}

void FFmpegReceiver::_setDecoding(bool active)
{
    if (active != _decoding) {
        _decoding = active;
        emit decodingChanged(active);
    }
}

void FFmpegReceiver::_openStatsFiles()
{
    _closeStatsFiles();
    _statsPrefix.clear();

    // Diagnostics are off unless asked for (the laptop measurement runs and the benches)
    const QString dir = qEnvironmentVariable("QGC_FFMPEG_STATS_DIR");
    _diagnostics = !dir.isEmpty();
    if (!_diagnostics) {
        return;
    }
    if (!QDir().mkpath(dir)) {
        qCWarning(FFmpegReceiverLog) << "Cannot create the stats folder" << dir;
        _diagnostics = false;
        return;
    }
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    _statsPrefix = QDir(dir).filePath(QStringLiteral("gc-video-%1-%2").arg(_name.isEmpty() ? QStringLiteral("video") : _name, stamp));

    _framesCsv.setFileName(_statsPrefix + QStringLiteral("-frames.csv"));
    if (_framesCsv.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        (void) _framesCsv.write("index,read_ns,decoded_ns,bridged_ns,set_ns,swap_ns,width,height,read_to_decoded_ms,read_to_set_ms,read_to_swap_ms\n");
    }
    _secondsCsv.setFileName(_statsPrefix + QStringLiteral("-seconds.csv"));
    if (_secondsCsv.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        (void) _secondsCsv.write("t_s,packets,decoded,shown,superseded,convert_failed,decode_errors,corrupt_packets,rtp_missed,reopens,awaiting_swap,swaps,replaced_on_sink,corrupt_frames,dropped_access_units,malformed_packets,refused_packets,rows_dropped,reordered_recovered,rtp_late,rtp_duplicates,reorder_held,reorder_hold_avg_ms,reorder_hold_max_ms\n");
    }
    qCInfo(FFmpegReceiverLog) << "Frame timing CSV:" << _framesCsv.fileName();
}

void FFmpegReceiver::_closeStatsFiles()
{
    if (_framesCsv.isOpen()) {
        _framesCsv.close();
    }
    if (_secondsCsv.isOpen()) {
        _secondsCsv.close();
    }
}

void FFmpegReceiver::_flushStats(bool final)
{
    QList<FrameTimes> done;
    Counters now;
    qsizetype awaiting = 0;
    quint64 rowsDropped = 0;
    {
        QMutexLocker locker(&_mutex);
        done.swap(_done);
        if (final) {
            done.append(_syncedForSwap);
            done.append(_awaitingSwap);
            _syncedForSwap.clear();
            _awaitingSwap.clear();
        }
        now = _counters;
        awaiting = _awaitingSwap.size();
        rowsDropped = _timesDropped;
    }

    if (_framesCsv.isOpen()) {
        QByteArray out;
        for (const FrameTimes &t : std::as_const(done)) {
            out += QByteArray::number(t.index) + ',' + QByteArray::number(t.readNs) + ',' + QByteArray::number(t.decodedNs) + ',' +
                   QByteArray::number(t.bridgedNs) + ',' + QByteArray::number(t.setNs) + ',' + QByteArray::number(t.swapNs) + ',' +
                   QByteArray::number(t.width) + ',' + QByteArray::number(t.height) + ',' +
                   QByteArray::number(ms(t.readNs, t.decodedNs), 'f', 3) + ',' + QByteArray::number(ms(t.readNs, t.setNs), 'f', 3) + ',' +
                   QByteArray::number(ms(t.readNs, t.swapNs), 'f', 3) + '\n';
        }
        (void) _framesCsv.write(out);
        (void) _framesCsv.flush();
    }

    const qint64 t = nowNs();
    const double seconds = (t - _lastSecondNs) / 1.0e9;
    if (_secondsCsv.isOpen()) {
        const quint64 heldNow = now.reorderHeld - _lastSecond.reorderHeld;
        const QByteArray line = QByteArray::number(seconds, 'f', 3) + ',' + QByteArray::number(now.packets - _lastSecond.packets) + ',' +
                                QByteArray::number(now.decoded - _lastSecond.decoded) + ',' + QByteArray::number(now.shown - _lastSecond.shown) + ',' +
                                QByteArray::number(now.superseded - _lastSecond.superseded) + ',' + QByteArray::number(now.convertFailed - _lastSecond.convertFailed) + ',' +
                                QByteArray::number(now.decodeErrors - _lastSecond.decodeErrors) + ',' + QByteArray::number(now.corruptPackets - _lastSecond.corruptPackets) + ',' +
                                QByteArray::number(now.rtpMissed - _lastSecond.rtpMissed) + ',' + QByteArray::number(now.reopens - _lastSecond.reopens) + ',' +
                                QByteArray::number(awaiting) + ',' + QByteArray::number(now.swaps - _lastSecond.swaps) + ',' +
                                QByteArray::number(now.replacedOnSink - _lastSecond.replacedOnSink) + ',' +
                                QByteArray::number(now.corruptFrames - _lastSecond.corruptFrames) + ',' +
                                QByteArray::number(now.droppedAccessUnits - _lastSecond.droppedAccessUnits) + ',' +
                                QByteArray::number(now.malformedPackets - _lastSecond.malformedPackets) + ',' +
                                QByteArray::number(now.refusedPackets - _lastSecond.refusedPackets) + ',' +
                                QByteArray::number(rowsDropped) + ',' +
                                QByteArray::number(now.reorderRecovered - _lastSecond.reorderRecovered) + ',' +
                                QByteArray::number(now.rtpLate - _lastSecond.rtpLate) + ',' +
                                QByteArray::number(now.rtpDuplicates - _lastSecond.rtpDuplicates) + ',' +
                                QByteArray::number(heldNow) + ',' +
                                QByteArray::number((heldNow > 0) ? (now.reorderHoldNsTotal - _lastSecond.reorderHoldNsTotal) / 1.0e6 / heldNow : 0.0, 'f', 3) + ',' +
                                QByteArray::number(now.reorderHoldNsMax / 1.0e6, 'f', 3) + '\n';
        (void) _secondsCsv.write(line);
        (void) _secondsCsv.flush();
    }
    if (_thread || final) {
        qCDebug(FFmpegReceiverLog) << _name << "per s: packets" << (now.packets - _lastSecond.packets) << "decoded" << (now.decoded - _lastSecond.decoded)
                                   << "shown" << (now.shown - _lastSecond.shown) << "superseded" << (now.superseded - _lastSecond.superseded)
                                   << "rtp missed" << (now.rtpMissed - _lastSecond.rtpMissed) << "decode errors" << (now.decodeErrors - _lastSecond.decodeErrors)
                                   << "malformed" << (now.malformedPackets - _lastSecond.malformedPackets)
                                   << "reordered" << (now.reorderRecovered - _lastSecond.reorderRecovered) << "late" << (now.rtpLate - _lastSecond.rtpLate);
    }
    _lastSecond = now;
    _lastSecondNs = t;
}
