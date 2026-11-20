/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "VideoManager.h"
#include "AppSettings.h"
#include "MultiVehicleManager.h"
#include "QGCApplication.h"
#include "QGCCameraManager.h"
#include "QGCCorePlugin.h"
#include "QGCLoggingCategory.h"
#include "SettingsManager.h"
#include "SubtitleWriter.h"
#include "Vehicle.h"
#include "VideoReceiver.h"
#include "VideoSettings.h"
#ifdef QGC_GST_STREAMING
#include "GStreamer.h"
#else
#include "VideoItemStub.h"
#endif
#include "QtMultimediaReceiver.h"
#include "UVCReceiver.h"
#ifdef QGC_FFMPEG_STREAMING
#include "FFmpegReceiver.h"
#endif

#include <QtCore/qapplicationstatic.h>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QPointer>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtCore/QTimer>

QGC_LOGGING_CATEGORY(VideoManagerLog, "qgc.videomanager.videomanager")

static constexpr const char *kFileExtension[VideoReceiver::FILE_FORMAT_MAX + 1] = {
    "mkv",
    "mov",
    "mp4"
};

Q_APPLICATION_STATIC(VideoManager, _videoManagerInstance);

VideoManager::VideoManager(QObject *parent)
    : QObject(parent)
    , _subtitleWriter(new SubtitleWriter(this))
    , _videoSettings(SettingsManager::instance()->videoSettings())
{
    // qCDebug(VideoManagerLog) << this;

    (void) qRegisterMetaType<VideoReceiver::STATUS>("STATUS");

#ifdef QGC_GST_STREAMING
    if (!GStreamer::initialize()) {
        qCCritical(VideoManagerLog) << "Failed To Initialize GStreamer";
    }
#endif
}

VideoManager::~VideoManager()
{
    // qCDebug(VideoManagerLog) << this;
}

VideoManager *VideoManager::instance()
{
    return _videoManagerInstance();
}

void VideoManager::registerQmlTypes()
{
    (void) qmlRegisterUncreatableType<VideoManager>("QGroundControl.VideoManager", 1, 0, "VideoManager", "Reference only");
    (void) qmlRegisterUncreatableType<VideoReceiver>("QGroundControl", 1, 0, "VideoReceiver","Reference only");
#ifndef QGC_GST_STREAMING
    (void) qmlRegisterType<VideoItemStub>("org.freedesktop.gstreamer.Qt6GLVideoItem", 1, 0, "GstGLQt6VideoItem");
#endif
}

void VideoManager::init(QQuickWindow *window)
{
    if (_initialized) {
        return;
    }

    if (!window) {
        qCCritical(VideoManagerLog) << "Failed To Init Video Manager - window is NULL";
        return;
    }

    // TODO: VideoSettings _configChanged/streamConfiguredChanged
    // G67: a change the user makes in the video settings may get a dialog; vehicle changes never do
    (void) connect(_videoSettings->videoSource(), &Fact::rawValueChanged, this, &VideoManager::_videoSettingsChanged);
    (void) connect(_videoSettings->udpUrl(), &Fact::rawValueChanged, this, &VideoManager::_videoSettingsChanged);
    (void) connect(_videoSettings->rtspUrl(), &Fact::rawValueChanged, this, &VideoManager::_videoSettingsChanged);
    (void) connect(_videoSettings->tcpUrl(), &Fact::rawValueChanged, this, &VideoManager::_videoSettingsChanged);
    (void) connect(_videoSettings->aspectRatio(), &Fact::rawValueChanged, this, &VideoManager::aspectRatioChanged);
    (void) connect(_videoSettings->lowLatencyMode(), &Fact::rawValueChanged, this, [this](const QVariant &value) { Q_UNUSED(value); _restartAllVideos(); });
    (void) connect(MultiVehicleManager::instance(), &MultiVehicleManager::activeVehicleChanged, this, &VideoManager::_setActiveVehicle);

    (void) connect(this, &VideoManager::autoStreamConfiguredChanged, this, &VideoManager::_videoSourceChanged);

    static const QStringList videoStreamList = {
        "videoContent",
        "thermalVideo"
    };
    for (const QString &streamName : videoStreamList) {
        VideoReceiver *receiver = QGCCorePlugin::instance()->createVideoReceiver(this);
        if (!receiver) {
            continue;
        }
        receiver->setName(streamName);

        _initVideoReceiver(receiver, window);
    }

    if (_videoSinkFailed) {
        // G83: a stream source cannot be shown this run, so tell QML video is off and tell the user
        emit hasVideoChanged();
        _reportVideoOff(VideoOffTrigger::Start);
    }

    window->scheduleRenderJob(new FinishVideoInitialization(), QQuickWindow::BeforeSynchronizingStage);

    _initialized = true;
}

void VideoManager::cleanup()
{
    for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
        QGCCorePlugin::instance()->releaseVideoSink(receiver->sink());
    }
}

void VideoManager::_cleanupOldVideos()
{
    if (!SettingsManager::instance()->videoSettings()->enableStorageLimit()->rawValue().toBool()) {
        return;
    }

    const QString savePath = SettingsManager::instance()->appSettings()->videoSavePath();
    QDir videoDir = QDir(savePath);
    videoDir.setFilter(QDir::Files | QDir::Readable | QDir::NoSymLinks | QDir::Writable);
    videoDir.setSorting(QDir::Time);

    QStringList nameFilters;
    for (size_t i = 0; i < std::size(kFileExtension); i++) {
        nameFilters << QStringLiteral("*.") + kFileExtension[i];
    }

    videoDir.setNameFilters(nameFilters);
    QFileInfoList vidList = videoDir.entryInfoList();
    if (vidList.isEmpty()) {
        return;
    }

    uint64_t total = 0;
    for (const QFileInfo &video : std::as_const(vidList)) {
        total += video.size();
    }

    const uint64_t maxSize = SettingsManager::instance()->videoSettings()->maxVideoSize()->rawValue().toUInt() * qPow(1024, 2);
    while ((total >= maxSize) && !vidList.isEmpty()) {
        const QFileInfo info = vidList.takeLast();
        total -= info.size();
        const QString path = info.filePath();
        qCDebug(VideoManagerLog) << "Removing old video file:" << path;
        (void) QFile::remove(path);
    }
}

void VideoManager::startRecording(const QString &videoFile)
{
    const VideoReceiver::FILE_FORMAT fileFormat = static_cast<VideoReceiver::FILE_FORMAT>(_videoSettings->recordingFormat()->rawValue().toInt());
    if (!VideoReceiver::isValidFileFormat(fileFormat)) {
        qgcApp()->showAppMessage(tr("Invalid video format defined."));
        return;
    }

    _cleanupOldVideos();

    const QString savePath = SettingsManager::instance()->appSettings()->videoSavePath();
    if (savePath.isEmpty()) {
        qgcApp()->showAppMessage(tr("Unabled to record video. Video save path must be specified in Settings."));
        return;
    }

    const QString videoFileUrl = videoFile.isEmpty() ? QDateTime::currentDateTime().toString("yyyy-MM-dd_hh.mm.ss") : videoFile;
    const QString ext = kFileExtension[fileFormat];

    const QString videoFileNameTemplate = savePath + "/" + videoFileUrl + ".%1" + ext;

    for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
        if (!receiver->started()) {
            qCDebug(VideoManagerLog) << "Video receiver is not ready.";
            continue;
        }
        const QString streamName = (receiver->name() == QStringLiteral("videoContent")) ? "" : (receiver->name() + ".");
        const QString videoFileName = videoFileNameTemplate.arg(streamName);
        receiver->startRecording(videoFileName, fileFormat);
    }
}

void VideoManager::stopRecording()
{
    for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
        receiver->stopRecording();
    }
}

void VideoManager::grabImage(const QString &imageFile)
{
    if (imageFile.isEmpty()) {
        _imageFile = SettingsManager::instance()->appSettings()->photoSavePath();
        _imageFile += QStringLiteral("/") + QDateTime::currentDateTime().toString("yyyy-MM-dd_hh.mm.ss.zzz") + QStringLiteral(".jpg");
    } else {
        _imageFile = imageFile;
    }

    emit imageFileChanged(_imageFile);

    for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
        receiver->takeScreenshot(_imageFile);
        // QSharedPointer<QQuickItemGrabResult> result = receiver->widget()->grabToImage(const QSize &targetSize = QSize())
    }
}

double VideoManager::aspectRatio() const
{
    for (VideoReceiver *receiver : _videoReceivers) {
        QGCVideoStreamInfo *pInfo = receiver->videoStreamInfo();
        if (!receiver->isThermal() && pInfo && !pInfo->isThermal()) {
            return pInfo->aspectRatio();
        }
    }

    // FIXME: use _videoReceiver->videoSize() to calculate AR (if AR is not specified in the settings?)
    return _videoSettings->aspectRatio()->rawValue().toDouble();
}

double VideoManager::thermalAspectRatio() const
{
    for (VideoReceiver *receiver : _videoReceivers) {
        QGCVideoStreamInfo *pInfo = receiver->videoStreamInfo();
        if (receiver->isThermal() && pInfo && pInfo->isThermal()) {
            return pInfo->aspectRatio();
        }
    }

    return 1.0;
}

double VideoManager::hfov() const
{
    for (VideoReceiver *receiver : _videoReceivers) {
        QGCVideoStreamInfo *pInfo = receiver->videoStreamInfo();
        if (!receiver->isThermal() && pInfo && !pInfo->isThermal()) {
            return pInfo->hfov();
        }
    }

    return 1.0;
}

double VideoManager::thermalHfov() const
{
    for (VideoReceiver *receiver : _videoReceivers) {
        QGCVideoStreamInfo *pInfo = receiver->videoStreamInfo();
        if (receiver->isThermal() && pInfo && pInfo->isThermal()) {
            return pInfo->hfov();
        }
    }

    return _videoSettings->aspectRatio()->rawValue().toDouble();
}

bool VideoManager::hasThermal() const
{
    for (VideoReceiver *receiver : _videoReceivers) {
        QGCVideoStreamInfo *pInfo = receiver->videoStreamInfo();
        if (receiver->isThermal() && pInfo && pInfo->isThermal()) {
            return true;
        }
    }

    return false;
}

bool VideoManager::hasVideo() const
{
    if (_videoUnavailable()) {
        return false;
    }
    return _videoWanted();
}

bool VideoManager::_videoWanted() const
{
    // G84: a stream from the vehicle counts for this instance (the saved settings are not used for it)
    return (_videoSettings->streamEnabled()->rawValue().toBool() && (autoStreamConfigured() || _videoSettings->streamConfigured()));
}

QString VideoManager::_effectiveVideoSource() const
{
    // G84: the source from the vehicle applies for this session only; otherwise the saved source
    return _autoStreamSource.isEmpty() ? _videoSettings->videoSource()->rawValue().toString() : _autoStreamSource;
}

bool VideoManager::_videoUnavailable() const
{
    // G83: without a video sink a stream source cannot be decoded, so it falls back to no video.
    // G88: the same when the receiver of this build cannot play the source.
    // UVC does not use the sink and is not affected.
    return ((_videoSinkFailed || _receiverCannotPlay) && isStreamSource());
}

void VideoManager::_reportVideoOff(VideoOffTrigger trigger)
{
    if (!_videoWanted() || !_videoUnavailable()) {
        _videoOffSource.clear();
        return;
    }

    // G67: the stream offered by the connected aircraft overrides the saved source (_updateSettings).
    // Setting the saved source to Video Stream Disabled does not stop it, so that advice is not given.
    const bool fromAircraft = autoStreamConfigured();
    const QString source = _effectiveVideoSource();
    QString message;
    if (fromAircraft) {
        // Once for each vehicle connection: a Vehicle lives from connect to disconnect
        if (!_activeVehicle) {
            return;
        }
        _videoOffVehicles.removeAll(QPointer<Vehicle>());
        for (const QPointer<Vehicle> &shown : std::as_const(_videoOffVehicles)) {
            if (shown == _activeVehicle) {
                return;
            }
        }
        _videoOffVehicles.append(QPointer<Vehicle>(_activeVehicle));
        message = tr("The connected aircraft offers a video stream, but this version of RotorLab GC cannot show network video streams, so video is off. Your video settings are not changed. This message appears once each time the aircraft connects.");
    } else {
        if (source == _videoOffSource) {
            return;
        }
        _videoOffSource = source;
        message = tr("This version of RotorLab GC cannot show network video streams (UDP, RTSP, TCP or MPEG-TS), so video is off. Your video settings are not changed. To stop this message, open Application Settings, Video, and set Source to Video Stream Disabled.");
    }

    // The saved setting is not changed: video is off for this run only.
    qCWarning(VideoManagerLog) << "Video is off for this run:" << (_videoSinkFailed ? "no video sink" : "the receiver cannot play") << "source" << source << (fromAircraft ? "(from the vehicle)" : "(saved)");

    // Only a source the user just chose in the settings gets a dialog. At start and when an
    // aircraft connects, never block the Fly view: show the non-modal "Video" banner.
    const bool modal = (trigger == VideoOffTrigger::Settings) && !fromAircraft;
    emit videoOff(message, modal);
    if (modal) {
        qgcApp()->showAppMessage(message);
    } else {
        // Queued so that at start the banner opens once the main window is up
        const QString title = tr("Video");
        QTimer::singleShot(0, qgcApp(), [message, title]() { qgcApp()->showCriticalVehicleMessage(message, title); });
    }
}

bool VideoManager::isUvc() const
{
    return (!_uvcVideoSourceID.isEmpty() && uvcEnabled() && hasVideo());
}

bool VideoManager::gstreamerEnabled()
{
#ifdef QGC_GST_STREAMING
    return true;
#else
    return false;
#endif
}

bool VideoManager::ffmpegEnabled()
{
#ifdef QGC_FFMPEG_STREAMING
    return FFmpegReceiver::enabled();
#else
    return false;
#endif
}

QString VideoManager::recordStatusChipSource()
{
    // Built only with G95 (local recording); without it the Fly view loads nothing
    static const QString source = QFile::exists(QStringLiteral(":/custom/video/VideoRecordStatus.qml"))
                                      ? QStringLiteral("qrc:/custom/video/VideoRecordStatus.qml") : QString();
    return source;
}

bool VideoManager::videoOutputBackend()
{
    return !gstreamerEnabled() && (ffmpegEnabled() || qtmultimediaEnabled());
}

bool VideoManager::uvcEnabled()
{
    return UVCReceiver::enabled();
}

bool VideoManager::qtmultimediaEnabled()
{
    return QtMultimediaReceiver::enabled();
}

void VideoManager::setfullScreen(bool on)
{
    if (on) {
        if (!_activeVehicle || _activeVehicle->vehicleLinkManager()->communicationLost()) {
            on = false;
        }
    }

    if (on != _fullScreen) {
        _fullScreen = on;
        emit fullScreenChanged();
    }
}

bool VideoManager::isStreamSource() const
{
    static const QStringList videoSourceList = {
        VideoSettings::videoSourceUDPH264,
        VideoSettings::videoSourceUDPH265,
        VideoSettings::videoSourceRTSP,
        VideoSettings::videoSourceTCP,
        VideoSettings::videoSourceMPEGTS,
        VideoSettings::videoSource3DRSolo,
        VideoSettings::videoSourceParrotDiscovery,
        VideoSettings::videoSourceYuneecMantisG,
        VideoSettings::videoSourceHerelinkAirUnit,
        VideoSettings::videoSourceHerelinkHotspot,
    };
    const QString videoSource = _videoSettings->videoSource()->rawValue().toString();
    return (videoSourceList.contains(videoSource) || autoStreamConfigured());
}

void VideoManager::_videoSourceChanged()
{
    _applyVideoSource(VideoOffTrigger::Vehicle);
}

void VideoManager::_videoSettingsChanged()
{
    _applyVideoSource(VideoOffTrigger::Settings);
}

void VideoManager::_applyVideoSource(VideoOffTrigger trigger)
{
    _lastTrigger = trigger;
    bool changed = false;
    if (_activeVehicle) {
        QGCCameraManager* camMgr = _activeVehicle->cameraManager();
        for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
            QGCVideoStreamInfo* info = nullptr;
            if (receiver->isThermal()) {
                info = camMgr ? camMgr->thermalStreamInstance() : nullptr;
            } else {
                info = camMgr ? camMgr->currentStreamInstance() : nullptr;
            }
            // Assign stream info
            receiver->setVideoStreamInfo(info);
            changed |= _updateSettings(receiver);
        }
    } else {
        for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
            receiver->setVideoStreamInfo(nullptr);
            changed |= _updateSettings(receiver);
        }
    }

    if (changed) {
        // G88: another source gets its own try
        _receiverCannotPlay = false;
        emit hasVideoChanged();
        emit isStreamSourceChanged();
        emit isAutoStreamChanged();

        if (hasVideo()) {
            _restartAllVideos();
        } else {
            stopVideo();
        }

        qCDebug(VideoManagerLog) << "New Video Source:" << _effectiveVideoSource();
    }

    _reportVideoOff(trigger);
}

bool VideoManager::_updateUVC(VideoReceiver *receiver)
{
    bool result = false;

    const QString oldUvcVideoSrcID = _uvcVideoSourceID;

    if (!uvcEnabled() || !hasVideo() || isStreamSource()) {
        _uvcVideoSourceID = QString();
    } else {
        _uvcVideoSourceID = UVCReceiver::getSourceId();
    }

    if (oldUvcVideoSrcID != _uvcVideoSourceID) {
        qCDebug(VideoManagerLog) << "UVC changed from [" << oldUvcVideoSrcID << "] to [" << _uvcVideoSourceID << "]";
        if (!_uvcVideoSourceID.isEmpty()) {
            UVCReceiver::checkPermission();
        }
        result = true;
        emit uvcVideoSourceIDChanged();
        emit isUvcChanged();
    }

    return result;
}

bool VideoManager::autoStreamConfigured() const
{
    // G84: set from the vehicle's VIDEO_STREAM_INFORMATION for this session only
    return !_autoStreamSource.isEmpty();
}

bool VideoManager::_updateAutoStream(VideoReceiver *receiver)
{
    // G84: a vehicle's VIDEO_STREAM_INFORMATION configures the stream for this session only.
    // It never writes the user's saved video source or URLs; those come back when the
    // vehicle's stream goes away.
    const QGCVideoStreamInfo *pInfo = receiver->videoStreamInfo();

    QString source, url;
    if (pInfo && !pInfo->uri().isEmpty()) {
        qCDebug(VideoManagerLog) << QString("Configure stream (%1):").arg(receiver->name()) << VideoReceiver::redactedUri(pInfo->uri());

        switch (pInfo->type()) {
        case VIDEO_STREAM_TYPE_RTSP:
            source = VideoSettings::videoSourceRTSP;
            url = pInfo->uri();
            break;
        case VIDEO_STREAM_TYPE_TCP_MPEG:
            source = VideoSettings::videoSourceTCP;
            url = pInfo->uri();
            break;
        case VIDEO_STREAM_TYPE_RTPUDP:
            if (pInfo->encoding() == VIDEO_STREAM_ENCODING_H265) {
                source = VideoSettings::videoSourceUDPH265;
                url = pInfo->uri().contains("udp265://") ? pInfo->uri() : QStringLiteral("udp265://0.0.0.0:%1").arg(pInfo->uri());
            } else {
                source = VideoSettings::videoSourceUDPH264;
                url = pInfo->uri().contains("udp://") ? pInfo->uri() : QStringLiteral("udp://0.0.0.0:%1").arg(pInfo->uri());
            }
            break;
        case VIDEO_STREAM_TYPE_MPEG_TS:
            source = VideoSettings::videoSourceMPEGTS;
            url = pInfo->uri().contains("mpegts://") ? pInfo->uri() : QStringLiteral("mpegts://0.0.0.0:%1").arg(pInfo->uri());
            break;
        default:
            // Not used: the saved video settings stay in effect
            qCWarning(VideoManagerLog) << "Unknown VIDEO_STREAM_TYPE" << pInfo->type() << "from vehicle, using the saved video settings";
            break;
        }
    }

    if (receiver->isThermal()) {
        return source.isEmpty() ? false : _updateVideoUri(receiver, url);
    }

    if (source.isEmpty()) {
        if (_autoStreamSource.isEmpty()) {
            return false;
        }
        // The vehicle's stream went away: back to the saved settings
        _autoStreamSource.clear();
        emit autoStreamConfiguredChanged();
        return true;
    }

    bool changed = _updateVideoUri(receiver, url);
    if (source != _autoStreamSource) {
        _autoStreamSource = source;
        changed = true;
    }
    if (changed) {
        emit autoStreamConfiguredChanged();
    }

    return changed;
}

bool VideoManager::_updateVideoUri(VideoReceiver *receiver, const QString &uri)
{
    if (!receiver) {
        qCDebug(VideoManagerLog) << "VideoReceiver is NULL";
        return false;
    }

    if ((uri == receiver->uri()) && !receiver->uri().isNull()) {
        return false;
    }

    qCDebug(VideoManagerLog) << "New Video URI" << VideoReceiver::redactedUri(uri);

    receiver->setUri(uri);

    return true;
}

bool VideoManager::_updateSettings(VideoReceiver *receiver)
{
    if (!receiver) {
        qCDebug(VideoManagerLog) << "VideoReceiver is NULL";
        return false;
    }

    bool settingsChanged = false;

    const bool lowLatency = _videoSettings->lowLatencyMode()->rawValue().toBool();
    if (lowLatency != receiver->lowLatency()) {
        receiver->setLowLatency(lowLatency);
        settingsChanged = true;
    }

    if (receiver->isThermal()) {
        return settingsChanged;
    }

    settingsChanged |= _updateUVC(receiver);
    settingsChanged |= _updateAutoStream(receiver);
    if (!_autoStreamSource.isEmpty()) {
        // G84: the vehicle's stream is in use for this session, the saved source does not apply
        return settingsChanged;
    }

    const QString source = _videoSettings->videoSource()->rawValue().toString();
    if (source == VideoSettings::videoSourceUDPH264) {
        settingsChanged |= _updateVideoUri(receiver, QStringLiteral("udp://%1").arg(_videoSettings->udpUrl()->rawValue().toString()));
    } else if (source == VideoSettings::videoSourceUDPH265) {
        settingsChanged |= _updateVideoUri(receiver, QStringLiteral("udp265://%1").arg(_videoSettings->udpUrl()->rawValue().toString()));
    } else if (source == VideoSettings::videoSourceMPEGTS) {
        settingsChanged |= _updateVideoUri(receiver, QStringLiteral("mpegts://%1").arg(_videoSettings->udpUrl()->rawValue().toString()));
    } else if (source == VideoSettings::videoSourceRTSP) {
        settingsChanged |= _updateVideoUri(receiver, _videoSettings->rtspUrl()->rawValue().toString());
    } else if (source == VideoSettings::videoSourceTCP) {
        settingsChanged |= _updateVideoUri(receiver, QStringLiteral("tcp://%1").arg(_videoSettings->tcpUrl()->rawValue().toString()));
    } else if (source == VideoSettings::videoSource3DRSolo) {
        settingsChanged |= _updateVideoUri(receiver, QStringLiteral("udp://0.0.0.0:5600"));
    } else if (source == VideoSettings::videoSourceParrotDiscovery) {
        settingsChanged |= _updateVideoUri(receiver, QStringLiteral("udp://0.0.0.0:8888"));
    } else if (source == VideoSettings::videoSourceYuneecMantisG) {
        settingsChanged |= _updateVideoUri(receiver, QStringLiteral("rtsp://192.168.42.1:554/live"));
    } else if (source == VideoSettings::videoSourceHerelinkAirUnit) {
        settingsChanged |= _updateVideoUri(receiver, QStringLiteral("rtsp://192.168.0.10:8554/H264Video"));
    } else if (source == VideoSettings::videoSourceHerelinkHotspot) {
        settingsChanged |= _updateVideoUri(receiver, QStringLiteral("rtsp://192.168.43.1:8554/fpv_stream"));
    } else if ((source == VideoSettings::videoDisabled) || (source == VideoSettings::videoSourceNoVideo)) {
        settingsChanged |= _updateVideoUri(receiver, QString());
    } else {
        settingsChanged |= _updateVideoUri(receiver, QString());
        if (!isUvc()) {
            qCCritical(VideoManagerLog) << "Video source URI \"" << source << "\" is not supported. Please add support!";
        }
    }

    return settingsChanged;
}

void VideoManager::_setActiveVehicle(Vehicle *vehicle)
{
    qCDebug(VideoManagerLog) << Q_FUNC_INFO << "new vehicle" << vehicle << "old active vehicle" << _activeVehicle;

    if (_activeVehicle) {
        (void) disconnect(_activeVehicle->vehicleLinkManager(), &VehicleLinkManager::communicationLostChanged, this, &VideoManager::_communicationLostChanged);
        auto cameraManager = _activeVehicle->cameraManager();
        if (cameraManager) {
            MavlinkCameraControl *pCamera = cameraManager->currentCameraInstance();
            if (pCamera) {
                pCamera->stopStream();
            }
            (void) disconnect(cameraManager, &QGCCameraManager::streamChanged, this, &VideoManager::_videoSourceChanged);
        }

        // G101: the old vehicle is no longer active before its stream info is cleared. Clearing it
        // re-enters _videoSourceChanged (through autoStreamConfiguredChanged), which reads the stream
        // info of _activeVehicle; with the old vehicle still set it put that vehicle's stream info
        // back, and the receivers kept a pointer to it after the vehicle was deleted.
        _activeVehicle = nullptr;
        for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
            // disconnect(receiver->videoStreamInfo(), &QGCVideoStreamInfo::infoChanged, ))
            receiver->setVideoStreamInfo(nullptr);
        }
    }

    _activeVehicle = vehicle;
    if (_activeVehicle) {
        (void) connect(_activeVehicle->vehicleLinkManager(), &VehicleLinkManager::communicationLostChanged, this, &VideoManager::_communicationLostChanged);
        if (_activeVehicle->cameraManager()) {
            (void) connect(_activeVehicle->cameraManager(), &QGCCameraManager::streamChanged, this, &VideoManager::_videoSourceChanged);
            MavlinkCameraControl *pCamera = _activeVehicle->cameraManager()->currentCameraInstance();
            if (pCamera) {
                pCamera->resumeStream();
            }
        }

        for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
            if (_activeVehicle->cameraManager()) {
                if (receiver->isThermal()) {
                    receiver->setVideoStreamInfo(_activeVehicle->cameraManager()->thermalStreamInstance());
                } else {
                    receiver->setVideoStreamInfo(_activeVehicle->cameraManager()->currentStreamInstance());
                }
            } else {
                receiver->setVideoStreamInfo(nullptr);
            }
            // connect(receiver->videoStreamInfo(), &QGCVideoStreamInfo::infoChanged, ))
        }

        // G67: a new connection whose stream is already known still gets its message once
        _reportVideoOff(VideoOffTrigger::Vehicle);
    } else {
        setfullScreen(false);
    }
}

void VideoManager::_communicationLostChanged(bool connectionLost)
{
    if (connectionLost) {
        setfullScreen(false);
    }
}

void VideoManager::_restartAllVideos()
{
    for (VideoReceiver *videoReceiver : std::as_const(_videoReceivers)) {
        _restartVideo(videoReceiver);
    }
}

void VideoManager::_restartVideo(VideoReceiver *receiver)
{
    if (!receiver) {
        qCDebug(VideoManagerLog) << "VideoReceiver is NULL";
        return;
    }

    qCDebug(VideoManagerLog) << "Restart video receiver" << receiver->name();

    if (receiver->started()) {
        _stopReceiver(receiver);
        // onStopComplete Signal Will Restart It
    } else {
        _startReceiver(receiver);
    }
}

void VideoManager::_stopReceiver(VideoReceiver *receiver)
{
    if (!receiver) {
        qCDebug(VideoManagerLog) << "VideoReceiver is NULL";
        return;
    }

    if (receiver->started()) {
        receiver->stop();
    }
}

void VideoManager::stopVideo()
{
    for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
        _stopReceiver(receiver);
    }
}

void VideoManager::_startReceiver(VideoReceiver *receiver)
{
    if (!receiver) {
        qCDebug(VideoManagerLog) << "VideoReceiver is NULL";
        return;
    }

    if (receiver->started()) {
        qCDebug(VideoManagerLog) << "VideoReceiver is already started" << receiver->name();
        return;
    }

    if (receiver->uri().isEmpty()) {
        qCDebug(VideoManagerLog) << "VideoUri is NULL" << receiver->name();
        return;
    }

    if (!receiver->sink()) {
        // G83: never start a receiver that has nowhere to decode to
        qCWarning(VideoManagerLog) << "No video sink, not starting" << receiver->name();
        return;
    }

    const QString source = _effectiveVideoSource();
    /* The gstreamer rtsp source will switch to tcp if udp is not available after 5 seconds.
       So we should allow for some negotiation time for rtsp */

    const uint32_t timeout = ((source == VideoSettings::videoSourceRTSP) ? _videoSettings->rtspTimeout()->rawValue().toUInt() : 3);

    receiver->start(timeout);
}

void VideoManager::_initVideoReceiver(VideoReceiver *receiver, QQuickWindow *window)
{
    if (_videoReceivers.contains(receiver)) {
        qCWarning(VideoManagerLog) << "Receiver already initialized";
    }

    QQuickItem *widget = window->findChild<QQuickItem*>(receiver->name());
    if (!widget) {
        qCCritical(VideoManagerLog) << "stream widget not found" << receiver->name();
    }
    receiver->setWidget(widget);

    void *sink = QGCCorePlugin::instance()->createVideoSink(receiver->widget(), receiver);
    if (!sink) {
        qCCritical(VideoManagerLog) << "createVideoSink() failed" << receiver->name();
        if (!receiver->isThermal()) {
            _videoSinkFailed = true;
        }
    }
    receiver->setSink(sink);

    if (widget) {
        // G84: the QML Loader can destroy the video item; its video sink goes with it, so
        // never decode into it again. The receiver keeps only a QPointer to the item.
        (void) connect(widget, &QObject::destroyed, this, [this, receiver = QPointer<VideoReceiver>(receiver)]() {
            if (!receiver) {
                return;
            }
            qCWarning(VideoManagerLog) << "Video widget destroyed, video sink cleared" << receiver->name();
            _stopReceiver(receiver);
            receiver->setSink(nullptr);
        });
    }

    (void) connect(receiver, &VideoReceiver::onStartComplete, this, [this, receiver](VideoReceiver::STATUS status) {
        if (!receiver) {
            return;
        }

        qCDebug(VideoManagerLog) << "Video" << receiver->name() << "Start complete, status:" << status;
        switch (status) {
        case VideoReceiver::STATUS_OK:
            receiver->setStarted(true);
            if (receiver->sink()) {
                receiver->startDecoding(receiver->sink());
            }
            break;
        case VideoReceiver::STATUS_INVALID_URL:
        case VideoReceiver::STATUS_INVALID_STATE:
            break;
        case VideoReceiver::STATUS_NOT_IMPLEMENTED:
            // G88: this build cannot play the source (FFmpeg library mismatch, no decoder, or a
            // receiver without network streams). Not retried; video is off with the G83 message.
            if (!receiver->isThermal()) {
                qCWarning(VideoManagerLog) << "Video" << receiver->name() << "cannot play" << VideoReceiver::redactedUri(receiver->uri());
                if (!_receiverCannotPlay) {
                    _receiverCannotPlay = true;
                    emit hasVideoChanged();
                }
                _reportVideoOff(_lastTrigger);
            }
            break;
        default:
            _restartVideo(receiver);
            break;
        }
    });

    (void) connect(receiver, &VideoReceiver::onStopComplete, this, [this, receiver](VideoReceiver::STATUS status) {
        qCDebug(VideoManagerLog) << "Stop complete" << receiver->name() << VideoReceiver::redactedUri(receiver->uri())  << ", status:" << status;
        receiver->setStarted(false);
        if (status == VideoReceiver::STATUS_INVALID_URL) {
            qCDebug(VideoManagerLog) << "Invalid video URL. Not restarting";
        } else {
            // G84: receivers have no parent and can outlive this VideoManager. With this as the
            // context the restart is dropped if the VideoManager is destroyed first, and the
            // QPointer drops it if the receiver is destroyed first.
            QTimer::singleShot(1000, this, [this, receiver = QPointer<VideoReceiver>(receiver)]() {
                if (!receiver) {
                    return;
                }
                qCDebug(VideoManagerLog) << "Restarting video receiver" << receiver->name() << VideoReceiver::redactedUri(receiver->uri());
                _startReceiver(receiver);
            });
        }
    });

    (void) connect(receiver, &VideoReceiver::streamingChanged, this, [this, receiver](bool active) {
        qCDebug(VideoManagerLog) << "Video" << receiver->name() << "streaming changed, active:" << (active ? "yes" : "no");
        if (!receiver->isThermal()) {
            _streaming = active;
            emit streamingChanged();
        }
    });

    (void) connect(receiver, &VideoReceiver::decodingChanged, this, [this, receiver](bool active) {
        qCDebug(VideoManagerLog) << "Video" << receiver->name() << "decoding changed, active:" << (active ? "yes" : "no");
        if (!receiver->isThermal()) {
            _decoding = active;
            emit decodingChanged();
        }
    });

    (void) connect(receiver, &VideoReceiver::recordingChanged, this, [this, receiver](bool active) {
        qCDebug(VideoManagerLog) << "Video" << receiver->name() << "recording changed, active:" << (active ? "yes" : "no");
        if (!receiver->isThermal()) {
            _recording = active;
            if (!active) {
                _subtitleWriter->stopCapturingTelemetry();
            }
            emit recordingChanged();
        }
    });

    (void) connect(receiver, &VideoReceiver::recordingStarted, this, [this, receiver](const QString &filename) {
        qCDebug(VideoManagerLog) << "Video" << receiver->name() << "recording started";
        if (!receiver->isThermal()) {
            _subtitleWriter->startCapturingTelemetry(filename, videoSize());
        }
    });

    (void) connect(receiver, &VideoReceiver::videoSizeChanged, this, [this, receiver](QSize size) {
        qCDebug(VideoManagerLog) << "Video" << receiver->name() << "resized. New resolution:" << size.width() << "x" << size.height();
        if (!receiver->isThermal()) {
            _videoSize = size;
            emit videoSizeChanged();
        }
    });

    // G88 (m4080): a started receiver that gets no video says why. The receiver changes its
    // status once per start (and once more when the cause is gone), so the banner shows once
    // per start; the Fly view keeps the text in the video area until it clears.
    (void) connect(receiver, &VideoReceiver::receiveStatusChanged, this, [this, receiver](VideoReceiver::RECEIVE_STATUS status, const QString &text) {
        if (!receiver) {
            return;
        }
        qCDebug(VideoManagerLog) << "Video" << receiver->name() << "receive status:" << status;
        if (!receiver->isThermal() && (text != _receiveStatusText)) {
            _receiveStatusText = text;
            emit receiveStatusTextChanged();
        }
        if ((status != VideoReceiver::RECEIVE_STATUS_OK) && !text.isEmpty()) {
            qCWarning(VideoManagerLog) << "Video" << receiver->name() << text;
            const QString title = tr("Video");
            QTimer::singleShot(0, qgcApp(), [text, title]() { qgcApp()->showCriticalVehicleMessage(text, title); });
        }
    });

    (void) connect(receiver, &VideoReceiver::onTakeScreenshotComplete, this, [receiver](VideoReceiver::STATUS status) {
        if (status == VideoReceiver::STATUS_OK) {
            qCDebug(VideoManagerLog) << "Video" << receiver->name() << "screenshot taken";
        } else {
            qCWarning(VideoManagerLog) << "Video" << receiver->name() << "screenshot failed, status:" << status;
        }
    });

    (void) connect(receiver, &VideoReceiver::videoStreamInfoChanged, this, [this, receiver]() {
        const QGCVideoStreamInfo *videoStreamInfo = receiver->videoStreamInfo();
        qCDebug(VideoManagerLog) << "Video" << receiver->name() << "stream info:" << (videoStreamInfo ? "received" : "lost");

        (void) _updateAutoStream(receiver);
    });

    (void) _updateSettings(receiver);

    _videoReceivers.append(receiver);

    if (hasVideo()) {
        _startReceiver(receiver);
    }
}

void VideoManager::startVideo()
{
    if (!hasVideo()) {
        qCDebug(VideoManagerLog) << "Stream not enabled/configured";
        return;
    }

    _restartAllVideos();
}

/*===========================================================================*/

FinishVideoInitialization::FinishVideoInitialization()
    : QRunnable()
{
    // qCDebug(VideoManagerLog) << this;
}

FinishVideoInitialization::~FinishVideoInitialization()
{
    // qCDebug(VideoManagerLog) << this;
}

void FinishVideoInitialization::run()
{
    VideoManager::instance()->startVideo();
}
