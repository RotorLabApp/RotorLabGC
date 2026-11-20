#include "VideoSinkTest.h"

#include "MAVLinkLib.h"
#include "MockLink.h"
#include "QGCCameraManager.h"
#include "QGCCorePlugin.h"
#include "QGCVideoStreamInfo.h"
#include "QtMultimediaReceiver.h"
#include "SettingsManager.h"
#include "VideoManager.h"
#include "VideoReceiver.h"
#include "VideoSettings.h"
#include "Vehicle.h"
#ifdef QGC_FFMPEG_STREAMING
#include "FFmpegReceiver.h"
#endif

#include <QtCore/QFileInfo>
#include <QtCore/QLoggingCategory>
#include <QtCore/QMutex>
#include <QtCore/QPointer>
#include <QtCore/QTemporaryDir>
#include <QtMultimediaQuick/private/qquickvideooutput_p.h>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

namespace {

const QString kVideoOffText = QStringLiteral("This version of RotorLab GC cannot show network video streams (UDP, RTSP, TCP or MPEG-TS), so video is off. Your video settings are not changed. To stop this message, open Application Settings, Video, and set Source to Video Stream Disabled.");
const QString kVideoOffAircraftText = QStringLiteral("The connected aircraft offers a video stream, but this version of RotorLab GC cannot show network video streams, so video is off. Your video settings are not changed. This message appears once each time the aircraft connects.");

// Test values only: documentation addresses (RFC 5737) and a made-up password
const QString kSavedUdpUrl = QStringLiteral("127.0.0.1:56999");
const QString kSavedRtspUrl = QStringLiteral("rtsp://192.0.2.1:554/saved");
const QString kSecret = QStringLiteral("s3cretPw");
const QString kVehicleRtspUrl = QStringLiteral("rtsp://admin:s3cretPw@192.0.2.10:8554/live");
const QString kVehicleRtspUrlRedacted = QStringLiteral("rtsp://192.0.2.10:8554/live");

void resetVideoSettings()
{
    VideoSettings *const vs = SettingsManager::instance()->videoSettings();
    vs->videoSource()->setRawValue(vs->videoSource()->rawDefaultValue());
    vs->udpUrl()->setRawValue(vs->udpUrl()->rawDefaultValue());
    vs->rtspUrl()->setRawValue(vs->rtspUrl()->rawDefaultValue());
    vs->streamEnabled()->setRawValue(vs->streamEnabled()->rawDefaultValue());
    vs->lowLatencyMode()->setRawValue(vs->lowLatencyMode()->rawDefaultValue());
}

void setSavedSource(const char *source)
{
    VideoSettings *const vs = SettingsManager::instance()->videoSettings();
    vs->udpUrl()->setRawValue(kSavedUdpUrl);
    vs->rtspUrl()->setRawValue(kSavedRtspUrl);
    vs->streamEnabled()->setRawValue(true);
    vs->videoSource()->setRawValue(QString::fromLatin1(source));
}

QString savedSource()
{
    return SettingsManager::instance()->videoSettings()->videoSource()->rawValue().toString();
}

QString savedRtspUrl()
{
    return SettingsManager::instance()->videoSettings()->rtspUrl()->rawValue().toString();
}

/// The VIDEO_STREAM_INFORMATION a vehicle sends, packed and decoded like MockLink does
QGCVideoStreamInfo *makeStreamInfo(uint8_t type, const QString &uri, uint8_t encoding, QObject *parent)
{
    QByteArray nameBA = QByteArrayLiteral("STREAM 1");
    nameBA.resize(MAVLINK_MSG_VIDEO_STREAM_INFORMATION_FIELD_NAME_LEN);
    QByteArray uriBA = uri.toUtf8();
    uriBA.resize(MAVLINK_MSG_VIDEO_STREAM_INFORMATION_FIELD_URI_LEN);

    mavlink_message_t msg{};
    (void) mavlink_msg_video_stream_information_pack_chan(
        1,
        MAV_COMP_ID_CAMERA,
        0,
        &msg,
        1,
        1,
        type,
        VIDEO_STREAM_STATUS_FLAGS_RUNNING,
        30,
        1920,
        1080,
        1000,
        0,
        70,
        nameBA.constData(),
        uriBA.constData(),
        encoding,
        0);

    mavlink_video_stream_information_t info{};
    mavlink_msg_video_stream_information_decode(&msg, &info);
    return new QGCVideoStreamInfo(info, parent);
}

/// Collects every log line while it lives, with video debug logging turned on.
class LogCapture
{
public:
    LogCapture()
    {
        {
            QMutexLocker lock(&s_mutex);
            s_lines.clear();
        }
        _categories = { &const_cast<QLoggingCategory&>(VideoManagerLog()), &const_cast<QLoggingCategory&>(QtMultimediaReceiverLog()) };
        for (QLoggingCategory *category : std::as_const(_categories)) {
            _debugWas.append(category->isDebugEnabled());
            category->setEnabled(QtDebugMsg, true);
        }
        s_previous = qInstallMessageHandler(&LogCapture::_handler);
    }

    ~LogCapture()
    {
        (void) qInstallMessageHandler(s_previous);
        s_previous = nullptr;
        for (qsizetype i = 0; i < _categories.size(); i++) {
            _categories[i]->setEnabled(QtDebugMsg, _debugWas[i]);
        }
    }

    QStringList lines() const
    {
        QMutexLocker lock(&s_mutex);
        return s_lines;
    }

    bool any(const QString &text) const
    {
        for (const QString &line : lines()) {
            if (line.contains(text)) {
                return true;
            }
        }
        return false;
    }

private:
    static void _handler(QtMsgType type, const QMessageLogContext &context, const QString &message)
    {
        {
            QMutexLocker lock(&s_mutex);
            s_lines.append(message);
        }
        if (s_previous) {
            s_previous(type, context, message);
        }
    }

    QList<QLoggingCategory*> _categories;
    QList<bool> _debugWas;
    static inline QMutex s_mutex;
    static inline QStringList s_lines;
    static inline QtMessageHandler s_previous = nullptr;
};

/// A receiver that only counts what VideoManager asks of it, so the restart paths can be
/// driven without opening a network stream.
class FakeVideoReceiver : public VideoReceiver
{
public:
    explicit FakeVideoReceiver(const QString &name) { setName(name); }

    void start(uint32_t timeout) override { Q_UNUSED(timeout); ++starts; }
    void stop() override { ++stops; }
    void startDecoding(void *sink) override { Q_UNUSED(sink); ++decodes; }
    void stopDecoding() override {}
    void startRecording(const QString &videoFile, FILE_FORMAT format) override { Q_UNUSED(videoFile); Q_UNUSED(format); }
    void stopRecording() override {}
    void takeScreenshot(const QString &imageFile) override { Q_UNUSED(imageFile); }

    int starts = 0;
    int stops = 0;
    int decodes = 0;
};

/// A window with a real VideoOutput and a fake main receiver. The VideoManager is given
/// the receiver directly, so no QMediaPlayer and no network are used.
struct FakeRig
{
    FakeRig()
    {
        output = new QQuickVideoOutput(window.contentItem());
        output->setObjectName(QStringLiteral("videoContent"));
        receiver = new FakeVideoReceiver(QStringLiteral("videoContent"));
    }
    ~FakeRig() { delete receiver; }

    QQuickWindow window;
    QPointer<QQuickVideoOutput> output;
    QPointer<FakeVideoReceiver> receiver;
};

// G88: the stream backends that decode into a VideoOutput (no GStreamer): GC's FFmpeg
// receiver, or Qt Multimedia
bool qtVideoOnly()
{
    return VideoManager::videoOutputBackend();
}

} // namespace

void VideoSinkTest::init()
{
    UnitTest::init();
    resetVideoSettings();
}

void VideoSinkTest::cleanup()
{
    resetVideoSettings();
    UnitTest::cleanup();
}

// A plain item (what the QML video widget is with GStreamer off) gives no sink and
// does not crash; a real VideoOutput still gives its sink.
void VideoSinkTest::_nonVideoWidgetRefused()
{
    QCOMPARE(QtMultimediaReceiver::createVideoSink(nullptr), nullptr);

    QQuickItem plainItem;
    plainItem.setObjectName(QStringLiteral("videoContent"));
    QCOMPARE(QtMultimediaReceiver::createVideoSink(&plainItem), nullptr);

    QQuickVideoOutput videoOutput;
    QVERIFY(videoOutput.videoSink() != nullptr);
    QCOMPARE(QtMultimediaReceiver::createVideoSink(&videoOutput), static_cast<void*>(videoOutput.videoSink()));

    // G101: the sink of the backend this build uses (GStreamer, Qt Multimedia or none) refuses a
    // plain item too. GStreamer used to hand it to qml6glsink, which crashed.
    QCOMPARE(QGCCorePlugin::instance()->createVideoSink(&plainItem, nullptr), nullptr);
}

// The saved source is UDP h.264 and the window has no real video output. GC must
// not crash, video is off for this run, the user is told once, and the saved
// setting is kept. At start the message is non-modal; a source chosen at run
// time gets the dialog.
void VideoSinkTest::_savedSourceFallsBackToNoVideo()
{
    if (!qtVideoOnly()) {
        QSKIP("Needs a VideoOutput video backend (GStreamer off, FFmpeg receiver or Qt Multimedia)");
    }

    VideoSettings *const vs = SettingsManager::instance()->videoSettings();
    vs->udpUrl()->setRawValue(QStringLiteral("0.0.0.0:5600"));
    vs->streamEnabled()->setRawValue(true);
    vs->videoSource()->setRawValue(QString::fromLatin1(VideoSettings::videoSourceUDPH264));

    QQuickWindow window;
    QQuickItem *const videoContent = new QQuickItem(window.contentItem());
    videoContent->setObjectName(QStringLiteral("videoContent"));
    QQuickItem *const thermalVideo = new QQuickItem(window.contentItem());
    thermalVideo->setObjectName(QStringLiteral("thermalVideo"));

    VideoManager videoManager;
    QSignalSpy videoOffSpy(&videoManager, &VideoManager::videoOff);
    QVERIFY(videoOffSpy.isValid());

    videoManager.init(&window);
    QCoreApplication::processEvents();

    QCOMPARE(videoOffSpy.count(), 1);
    QCOMPARE(videoOffSpy.at(0).at(0).toString(), kVideoOffText);
    QCOMPARE(videoOffSpy.at(0).at(1).toBool(), false);  // at start: banner, not a dialog
    QVERIFY(!videoManager.hasVideo());
    QVERIFY(!videoManager.decoding());
    QVERIFY(!videoManager.streaming());
    QCOMPARE(vs->videoSource()->rawValue().toString(), QString::fromLatin1(VideoSettings::videoSourceUDPH264));

    // Starting video again stays off and does not repeat the message.
    videoManager.startVideo();
    QCoreApplication::processEvents();
    QCOMPARE(videoOffSpy.count(), 1);
    QVERIFY(!videoManager.decoding());

    // No video, then the stream source chosen again at run time: told again, no crash.
    vs->videoSource()->setRawValue(QString::fromLatin1(VideoSettings::videoSourceNoVideo));
    QCOMPARE(videoOffSpy.count(), 1);
    vs->videoSource()->setRawValue(QString::fromLatin1(VideoSettings::videoSourceUDPH264));
    QCoreApplication::processEvents();
    QCOMPARE(videoOffSpy.count(), 2);
    QCOMPARE(videoOffSpy.at(1).at(0).toString(), kVideoOffText);
    QCOMPARE(videoOffSpy.at(1).at(1).toBool(), true);   // chosen at run time: dialog
    QVERIFY(!videoManager.hasVideo());
    QVERIFY(!videoManager.decoding());
}

// G67: which "video is off" text is shown, and how. The saved source gets the text that says
// how to stop it, as a banner at start and a dialog when the user just chose it. A stream the
// aircraft offers overrides the saved source, so it gets its own text (no advice that cannot
// work), always as the non-modal banner, once for each vehicle connection. Needs no video
// backend: the missing sink is set directly.
void VideoSinkTest::_videoOffMessageChoice()
{
    VideoSettings *const vs = SettingsManager::instance()->videoSettings();
    setSavedSource(VideoSettings::videoSourceUDPH264);

    VideoManager videoManager;   // not init(): driven directly, no settings or vehicle connections
    videoManager._videoSinkFailed = true;
    QSignalSpy videoOffSpy(&videoManager, &VideoManager::videoOff);
    QVERIFY(videoOffSpy.isValid());

    // Saved source at start: banner, once
    videoManager._reportVideoOff(VideoManager::VideoOffTrigger::Start);
    QCOMPARE(videoOffSpy.count(), 1);
    QCOMPARE(videoOffSpy.at(0).at(0).toString(), kVideoOffText);
    QCOMPARE(videoOffSpy.at(0).at(1).toBool(), false);
    videoManager._reportVideoOff(VideoManager::VideoOffTrigger::Vehicle);
    videoManager._reportVideoOff(VideoManager::VideoOffTrigger::Settings);
    QCOMPARE(videoOffSpy.count(), 1);

    // Another stream source chosen in the settings: dialog
    vs->videoSource()->setRawValue(QString::fromLatin1(VideoSettings::videoSourceRTSP));
    videoManager._reportVideoOff(VideoManager::VideoOffTrigger::Settings);
    QCOMPARE(videoOffSpy.count(), 2);
    QCOMPARE(videoOffSpy.at(1).at(0).toString(), kVideoOffText);
    QCOMPARE(videoOffSpy.at(1).at(1).toBool(), true);

    // A vehicle stream with no active vehicle (nothing connected): no aircraft message
    videoManager._autoStreamSource = QString::fromLatin1(VideoSettings::videoSourceRTSP);
    videoManager._reportVideoOff(VideoManager::VideoOffTrigger::Vehicle);
    QCOMPARE(videoOffSpy.count(), 2);

    // The aircraft connects and offers a stream: aircraft text, banner
    _connectMockLink();
    QVERIFY(_vehicle);
    videoManager._setActiveVehicle(_vehicle);
    QCOMPARE(videoOffSpy.count(), 3);
    QCOMPARE(videoOffSpy.at(2).at(0).toString(), kVideoOffAircraftText);
    QCOMPARE(videoOffSpy.at(2).at(1).toBool(), false);
    QVERIFY(!kVideoOffAircraftText.contains(QStringLiteral("Video Stream Disabled")));

    // Same connection: not repeated, and a settings change never turns it into a dialog
    videoManager._reportVideoOff(VideoManager::VideoOffTrigger::Vehicle);
    vs->videoSource()->setRawValue(QString::fromLatin1(VideoSettings::videoSourceUDPH264));
    videoManager._reportVideoOff(VideoManager::VideoOffTrigger::Settings);
    QCOMPARE(videoOffSpy.count(), 3);

    // Video Stream Disabled does not stop the aircraft's stream from being wanted
    vs->videoSource()->setRawValue(QString::fromLatin1(VideoSettings::videoDisabled));
    QVERIFY(videoManager._videoWanted());
    QVERIFY(videoManager._videoUnavailable());
    videoManager._reportVideoOff(VideoManager::VideoOffTrigger::Settings);
    QCOMPARE(videoOffSpy.count(), 3);

    // Disconnect and connect again: shown once more, once
    videoManager._setActiveVehicle(nullptr);
    _disconnectMockLink();
    videoManager._autoStreamSource = QString::fromLatin1(VideoSettings::videoSourceRTSP);
    _connectMockLink();
    QVERIFY(_vehicle);
    videoManager._setActiveVehicle(_vehicle);
    videoManager._reportVideoOff(VideoManager::VideoOffTrigger::Vehicle);
    QCOMPARE(videoOffSpy.count(), 4);
    QCOMPARE(videoOffSpy.at(3).at(0).toString(), kVideoOffAircraftText);
    QCOMPARE(videoOffSpy.at(3).at(1).toBool(), false);

    // US spelling, no em dash, guidance only
    for (const QString &text : {kVideoOffText, kVideoOffAircraftText}) {
        QVERIFY(!text.contains(QChar(0x2014)));
        QVERIFY(!text.contains(QStringLiteral("approv"), Qt::CaseInsensitive));
        QVERIFY(!text.contains(QStringLiteral("clear"), Qt::CaseInsensitive));
    }

    videoManager._setActiveVehicle(nullptr);
    videoManager._autoStreamSource.clear();
    _disconnectMockLink();
    QCoreApplication::processEvents();
}

// G101: a vehicle whose camera offers a stream disconnects. The receivers must not keep the
// vehicle's stream info (it is deleted with the vehicle) and the vehicle's stream source is
// cleared. Before G101, clearing the stream info re-entered the source check while the old
// vehicle was still active, which put its stream info back.
void VideoSinkTest::_vehicleStreamClearedOnDisconnect()
{
    setSavedSource(VideoSettings::videoSourceUDPH264);

    // A main receiver set up the normal way, with a plain item: every backend refuses it, so there is
    // no video sink on any build (GStreamer, Qt Multimedia or none). Only the stream info and the
    // source are checked, so the test does not depend on a working sink.
    QQuickWindow window;
    QQuickItem *const videoContent = new QQuickItem(window.contentItem());
    videoContent->setObjectName(QStringLiteral("videoContent"));
    FakeVideoReceiver *const receiver = new FakeVideoReceiver(QStringLiteral("videoContent"));
    VideoManager videoManager;
    // The connection init() makes: the path that re-entered the source check on disconnect
    (void) connect(&videoManager, &VideoManager::autoStreamConfiguredChanged, &videoManager, &VideoManager::_videoSourceChanged);
    videoManager._initVideoReceiver(receiver, &window);
    QCOMPARE(receiver->sink(), nullptr);

    _connectMockLink();
    QVERIFY(_vehicle);
    QGCCameraManager *const cameraManager = _vehicle->cameraManager();
    QVERIFY(cameraManager);
    videoManager._setActiveVehicle(_vehicle);

    // The vehicle's camera announces itself and offers an RTSP stream (as MockLink::_sendVideoInfo would)
    const uint8_t sysid = static_cast<uint8_t>(_vehicle->id());
    const uint8_t compid = MAV_COMP_ID_CAMERA;
    auto offerStream = [&]() {
        mavlink_message_t msg{};
        (void) mavlink_msg_heartbeat_pack(sysid, compid, &msg, MAV_TYPE_CAMERA, MAV_AUTOPILOT_INVALID, 0, 0, MAV_STATE_STANDBY);
        _mockLink->respondWithMavlinkMessage(msg);

        const uint8_t vendor[32] = "VENDOR";
        const uint8_t model[32] = "MODEL 1";
        (void) mavlink_msg_camera_information_pack(sysid, compid, &msg, 0, vendor, model, 0, 0, 0, 0, 1920, 1080, 0,
                                                   CAMERA_CAP_FLAGS_HAS_VIDEO_STREAM, 0, "", 0, 0);
        _mockLink->respondWithMavlinkMessage(msg);

        QByteArray nameBA = QByteArrayLiteral("STREAM 1");
        nameBA.resize(MAVLINK_MSG_VIDEO_STREAM_INFORMATION_FIELD_NAME_LEN);
        QByteArray uriBA = QByteArrayLiteral("rtsp://192.0.2.10:8554/live");   // documentation address (RFC 5737)
        uriBA.resize(MAVLINK_MSG_VIDEO_STREAM_INFORMATION_FIELD_URI_LEN);
        (void) mavlink_msg_video_stream_information_pack(sysid, compid, &msg, 1, 1, VIDEO_STREAM_TYPE_RTSP,
                                                         VIDEO_STREAM_STATUS_FLAGS_RUNNING, 30, 1920, 1080, 1000, 0, 70,
                                                         nameBA.constData(), uriBA.constData(),
                                                         VIDEO_STREAM_ENCODING_H264, 0);
        _mockLink->respondWithMavlinkMessage(msg);
    };
    for (int i = 0; (i < 20) && !cameraManager->currentStreamInstance(); i++) {
        offerStream();
        QTest::qWait(250);
    }
    QPointer<QGCVideoStreamInfo> streamInfo = cameraManager->currentStreamInstance();
    QVERIFY2(streamInfo, "the MockLink camera did not offer its stream");
    QTRY_VERIFY_WITH_TIMEOUT(videoManager.autoStreamConfigured(), 5000);
    QCOMPARE(receiver->videoStreamInfo(), streamInfo.data());
    QCOMPARE(videoManager._effectiveVideoSource(), QString::fromLatin1(VideoSettings::videoSourceRTSP));

    // The vehicle goes away (MultiVehicleManager makes no vehicle active first)
    videoManager._setActiveVehicle(nullptr);
    QCOMPARE(receiver->videoStreamInfo(), nullptr);
    QVERIFY(!videoManager.autoStreamConfigured());
    QCOMPARE(videoManager._effectiveVideoSource(), QString::fromLatin1(VideoSettings::videoSourceUDPH264));
    QVERIFY(videoManager._activeVehicle == nullptr);

    _disconnectMockLink();
    QTRY_VERIFY_WITH_TIMEOUT(streamInfo.isNull(), 5000);   // deleted with the vehicle
    QCOMPARE(receiver->videoStreamInfo(), nullptr);
    QVERIFY(!videoManager.autoStreamConfigured());
    QVERIFY(!videoManager.hasThermal());                 // reads each receiver's stream info

    videoManager._videoReceivers.clear();
    delete receiver;
}

// G84: user name and password are removed from every URL in a text; nothing else changes.
void VideoSinkTest::_redactUri()
{
    QCOMPARE(VideoReceiver::redactedUri(kVehicleRtspUrl), kVehicleRtspUrlRedacted);
    QCOMPARE(VideoReceiver::redactedUri(QStringLiteral("rtsp://admin@192.0.2.10/live")), QStringLiteral("rtsp://192.0.2.10/live"));
    QCOMPARE(VideoReceiver::redactedUri(QStringLiteral("rtsp://user:p@ss@192.0.2.10:554/a@b")), QStringLiteral("rtsp://192.0.2.10:554/a@b"));
    QCOMPARE(VideoReceiver::redactedUri(QStringLiteral("rtsp://user:pw@192.0.2.10")), QStringLiteral("rtsp://192.0.2.10"));
    QCOMPARE(VideoReceiver::redactedUri(QStringLiteral("udp://0.0.0.0:5600")), QStringLiteral("udp://0.0.0.0:5600"));
    QCOMPARE(VideoReceiver::redactedUri(QStringLiteral("rtsp://192.0.2.10/live?token=a@b")), QStringLiteral("rtsp://192.0.2.10/live?token=a@b"));
    QCOMPARE(VideoReceiver::redactedUri(QStringLiteral("0.0.0.0:5600")), QStringLiteral("0.0.0.0:5600"));
    QCOMPARE(VideoReceiver::redactedUri(QString()), QString());

    // An error string with two URLs in it, as a player can report
    const QString error = QStringLiteral("Could not open rtsp://admin:s3cretPw@192.0.2.10/live (fallback tcp://u:s3cretPw@192.0.2.11:5000): 401");
    QCOMPARE(VideoReceiver::redactedUri(error), QStringLiteral("Could not open rtsp://192.0.2.10/live (fallback tcp://192.0.2.11:5000): 401"));
    QVERIFY(!VideoReceiver::redactedUri(error).contains(kSecret));
}

// G84: an RTSP stream from the vehicle is used for this session only. The saved source and
// RTSP URL are not written, the vehicle's password never reaches the log, and the saved
// settings are back when the vehicle's stream goes away.
void VideoSinkTest::_autoStreamRtspIsSessionOnly()
{
    if (!qtVideoOnly()) {
        QSKIP("Needs a VideoOutput video backend (GStreamer off, FFmpeg receiver or Qt Multimedia)");
    }

    setSavedSource(VideoSettings::videoSourceUDPH264);

    FakeRig rig;
    VideoManager videoManager;
    videoManager._initVideoReceiver(rig.receiver, &rig.window);
    QVERIFY(rig.receiver->sink() != nullptr);
    QCOMPARE(rig.receiver->uri(), QStringLiteral("udp://") + kSavedUdpUrl);
    QCOMPARE(rig.receiver->starts, 1);
    QVERIFY(!videoManager.autoStreamConfigured());

    QSignalSpy autoSpy(&videoManager, &VideoManager::autoStreamConfiguredChanged);
    QVERIFY(autoSpy.isValid());

    QGCVideoStreamInfo *const info = makeStreamInfo(VIDEO_STREAM_TYPE_RTSP, kVehicleRtspUrl, VIDEO_STREAM_ENCODING_H264, &videoManager);
    QCOMPARE(info->uri(), kVehicleRtspUrl);
    {
        LogCapture log;
        rig.receiver->setVideoStreamInfo(info);
        QVERIFY(log.any(kVehicleRtspUrlRedacted));
        QVERIFY2(!log.any(kSecret), qPrintable(log.lines().join(QLatin1Char('\n'))));
    }

    QCOMPARE(autoSpy.count(), 1);
    QVERIFY(videoManager.autoStreamConfigured());
    QVERIFY(videoManager.isStreamSource());
    QVERIFY(videoManager.hasVideo());
    QCOMPARE(rig.receiver->uri(), kVehicleRtspUrl);
    QCOMPARE(videoManager._effectiveVideoSource(), QString::fromLatin1(VideoSettings::videoSourceRTSP));

    // The saved settings are not touched
    QCOMPARE(savedSource(), QString::fromLatin1(VideoSettings::videoSourceUDPH264));
    QCOMPARE(savedRtspUrl(), kSavedRtspUrl);

    // A settings pass keeps the vehicle's stream (the saved UDP URL does not win)
    (void) videoManager._updateSettings(rig.receiver);
    QCOMPARE(rig.receiver->uri(), kVehicleRtspUrl);
    QCOMPARE(savedSource(), QString::fromLatin1(VideoSettings::videoSourceUDPH264));

    // Same info again: nothing changes
    QVERIFY(!videoManager._updateAutoStream(rig.receiver));
    QCOMPARE(autoSpy.count(), 1);

    // The vehicle's stream goes away: back to the saved settings
    rig.receiver->setVideoStreamInfo(nullptr);
    QCOMPARE(autoSpy.count(), 2);
    QVERIFY(!videoManager.autoStreamConfigured());
    (void) videoManager._updateSettings(rig.receiver);
    QCOMPARE(rig.receiver->uri(), QStringLiteral("udp://") + kSavedUdpUrl);
    QCOMPARE(videoManager._effectiveVideoSource(), QString::fromLatin1(VideoSettings::videoSourceUDPH264));
    QCOMPARE(savedSource(), QString::fromLatin1(VideoSettings::videoSourceUDPH264));
    QCOMPARE(savedRtspUrl(), kSavedRtspUrl);
}

// G84: a stream type GC does not know is not used. Before 5.2.0 it wrote "No Video" into
// the user's saved source; now the saved settings stay in effect and are not written.
void VideoSinkTest::_autoStreamUnknownTypeKeepsSavedSettings()
{
    if (!qtVideoOnly()) {
        QSKIP("Needs a VideoOutput video backend (GStreamer off, FFmpeg receiver or Qt Multimedia)");
    }

    setSavedSource(VideoSettings::videoSourceRTSP);

    FakeRig rig;
    VideoManager videoManager;
    videoManager._initVideoReceiver(rig.receiver, &rig.window);
    QCOMPARE(rig.receiver->uri(), kSavedRtspUrl);

    QSignalSpy autoSpy(&videoManager, &VideoManager::autoStreamConfiguredChanged);
    QGCVideoStreamInfo *const info = makeStreamInfo(200, QStringLiteral("rtsp://192.0.2.20:554/unknown"), VIDEO_STREAM_ENCODING_H264, &videoManager);
    QCOMPARE(info->type(), static_cast<quint8>(200));
    {
        LogCapture log;
        rig.receiver->setVideoStreamInfo(info);
        QVERIFY(log.any(QStringLiteral("Unknown VIDEO_STREAM_TYPE")));
    }

    QCOMPARE(autoSpy.count(), 0);
    QVERIFY(!videoManager.autoStreamConfigured());
    QCOMPARE(rig.receiver->uri(), kSavedRtspUrl);
    (void) videoManager._updateSettings(rig.receiver);
    QCOMPARE(rig.receiver->uri(), kSavedRtspUrl);
    QCOMPARE(savedSource(), QString::fromLatin1(VideoSettings::videoSourceRTSP));
    QCOMPARE(savedRtspUrl(), kSavedRtspUrl);
    QVERIFY(videoManager.hasVideo());
}

// G84: an RTP/UDP h.265 stream from the vehicle that gives only a port
void VideoSinkTest::_autoStreamUdpH265()
{
    if (!qtVideoOnly()) {
        QSKIP("Needs a VideoOutput video backend (GStreamer off, FFmpeg receiver or Qt Multimedia)");
    }

    setSavedSource(VideoSettings::videoSourceRTSP);

    FakeRig rig;
    VideoManager videoManager;
    videoManager._initVideoReceiver(rig.receiver, &rig.window);

    rig.receiver->setVideoStreamInfo(makeStreamInfo(VIDEO_STREAM_TYPE_RTPUDP, QStringLiteral("5601"), VIDEO_STREAM_ENCODING_H265, &videoManager));
    QVERIFY(videoManager.autoStreamConfigured());
    QCOMPARE(rig.receiver->uri(), QStringLiteral("udp265://0.0.0.0:5601"));
    QCOMPARE(videoManager._effectiveVideoSource(), QString::fromLatin1(VideoSettings::videoSourceUDPH265));
    QCOMPARE(savedSource(), QString::fromLatin1(VideoSettings::videoSourceRTSP));
    QCOMPARE(savedRtspUrl(), kSavedRtspUrl);
}

// Herelink sources map to their fixed RTSP URLs. With no real video output (GStreamer off)
// video is off with the message, nothing is started and the saved source is kept.
void VideoSinkTest::_herelinkSource()
{
    if (!qtVideoOnly()) {
        QSKIP("Needs a VideoOutput video backend (GStreamer off, FFmpeg receiver or Qt Multimedia)");
    }

    setSavedSource(VideoSettings::videoSourceHerelinkAirUnit);

    QQuickWindow window;
    QQuickItem *const videoContent = new QQuickItem(window.contentItem());
    videoContent->setObjectName(QStringLiteral("videoContent"));
    QQuickItem *const thermalVideo = new QQuickItem(window.contentItem());
    thermalVideo->setObjectName(QStringLiteral("thermalVideo"));

    VideoManager videoManager;
    QSignalSpy videoOffSpy(&videoManager, &VideoManager::videoOff);
    videoManager.init(&window);
    QCoreApplication::processEvents();

    QCOMPARE(videoManager._videoReceivers.size(), 2);
    VideoReceiver *const main = videoManager._videoReceivers.at(0);
    QVERIFY(!main->isThermal());
    QVERIFY(videoManager.isStreamSource());
    QCOMPARE(main->uri(), QStringLiteral("rtsp://192.168.0.10:8554/H264Video"));
    QVERIFY(!main->started());
    QVERIFY(!videoManager.hasVideo());
    QCOMPARE(videoOffSpy.count(), 1);
    QCOMPARE(videoOffSpy.at(0).at(0).toString(), kVideoOffText);

    SettingsManager::instance()->videoSettings()->videoSource()->setRawValue(QString::fromLatin1(VideoSettings::videoSourceHerelinkHotspot));
    QCoreApplication::processEvents();
    QCOMPARE(main->uri(), QStringLiteral("rtsp://192.168.43.1:8554/fpv_stream"));
    QVERIFY(!main->started());
    QCOMPARE(savedSource(), QString::fromLatin1(VideoSettings::videoSourceHerelinkHotspot));
}

// The thermal widget is not a video output but the main one is: main video is set up and
// started, thermal has no sink, and video is not turned off.
void VideoSinkTest::_thermalSinkFailsMainWorks()
{
    if (!qtVideoOnly()) {
        QSKIP("Needs a VideoOutput video backend (GStreamer off, FFmpeg receiver or Qt Multimedia)");
    }

    if (!VideoManager::ffmpegEnabled()) {
        QSKIP("UDP video needs GC's FFmpeg receiver (Qt Multimedia refuses it)");
    }
    setSavedSource(VideoSettings::videoSourceUDPH264);   // loopback only

    QQuickWindow window;
    QQuickVideoOutput *const videoContent = new QQuickVideoOutput(window.contentItem());
    videoContent->setObjectName(QStringLiteral("videoContent"));
    QQuickItem *const thermalVideo = new QQuickItem(window.contentItem());
    thermalVideo->setObjectName(QStringLiteral("thermalVideo"));

    VideoManager videoManager;
    QSignalSpy videoOffSpy(&videoManager, &VideoManager::videoOff);
    videoManager.init(&window);
    QCoreApplication::processEvents();

    QCOMPARE(videoManager._videoReceivers.size(), 2);
    VideoReceiver *const main = videoManager._videoReceivers.at(0);
    VideoReceiver *const thermal = videoManager._videoReceivers.at(1);
    QVERIFY(!main->isThermal());
    QVERIFY(thermal->isThermal());
    QCOMPARE(main->sink(), static_cast<void*>(videoContent->videoSink()));
    QCOMPARE(thermal->sink(), nullptr);
    QVERIFY(!videoManager._videoSinkFailed);
    QCOMPARE(videoOffSpy.count(), 0);
    QVERIFY(videoManager.hasVideo());
    QCOMPARE(main->uri(), QStringLiteral("udp://") + kSavedUdpUrl);
    QVERIFY(main->started());
    QVERIFY(!thermal->started());

    videoManager.stopVideo();
    QCoreApplication::processEvents();
}

// The restart paths: a failed start retries, a good start decodes, a restart stops and
// the stop restarts after a second, and an invalid URL is not restarted.
void VideoSinkTest::_restartPaths()
{
    if (!qtVideoOnly()) {
        QSKIP("Needs a VideoOutput video backend (GStreamer off, FFmpeg receiver or Qt Multimedia)");
    }

    setSavedSource(VideoSettings::videoSourceUDPH264);

    FakeRig rig;
    VideoManager videoManager;
    videoManager._initVideoReceiver(rig.receiver, &rig.window);
    FakeVideoReceiver *const receiver = rig.receiver;
    QCOMPARE(receiver->starts, 1);

    emit receiver->onStartComplete(VideoReceiver::STATUS_FAIL);
    QCOMPARE(receiver->starts, 2);
    QVERIFY(!receiver->started());

    emit receiver->onStartComplete(VideoReceiver::STATUS_OK);
    QVERIFY(receiver->started());
    QCOMPARE(receiver->decodes, 1);

    // Low latency mode and source changes restart all videos: a started receiver is stopped
    videoManager._restartAllVideos();
    QCOMPARE(receiver->stops, 1);

    emit receiver->onStopComplete(VideoReceiver::STATUS_OK);
    QVERIFY(!receiver->started());
    QCOMPARE(receiver->starts, 2);
    QTRY_COMPARE_WITH_TIMEOUT(receiver->starts, 3, 5000);

    // Invalid URL: no restart
    emit receiver->onStopComplete(VideoReceiver::STATUS_INVALID_URL);
    QTest::qWait(1500);
    QCOMPARE(receiver->starts, 3);

    // Not started yet: a restart starts it
    videoManager._restartAllVideos();
    QCOMPARE(receiver->starts, 4);
}

// G84: the restart timer must not use a VideoManager that is gone (receivers outlive it).
void VideoSinkTest::_restartAfterVideoManagerDestroyed()
{
    if (!qtVideoOnly()) {
        QSKIP("Needs a VideoOutput video backend (GStreamer off, FFmpeg receiver or Qt Multimedia)");
    }

    setSavedSource(VideoSettings::videoSourceUDPH264);

    FakeRig rig;
    VideoManager *videoManager = new VideoManager();
    videoManager->_initVideoReceiver(rig.receiver, &rig.window);
    QCOMPARE(rig.receiver->starts, 1);

    emit rig.receiver->onStopComplete(VideoReceiver::STATUS_OK);   // restart in 1 s
    delete videoManager;
    videoManager = nullptr;

    QTest::qWait(1500);
    QCOMPARE(rig.receiver->starts, 1);
    QVERIFY(!rig.receiver.isNull());
}

// G84: the restart timer must not use a receiver that is gone.
void VideoSinkTest::_restartAfterReceiverDestroyed()
{
    if (!qtVideoOnly()) {
        QSKIP("Needs a VideoOutput video backend (GStreamer off, FFmpeg receiver or Qt Multimedia)");
    }

    setSavedSource(VideoSettings::videoSourceUDPH264);

    FakeRig rig;
    VideoManager videoManager;
    videoManager._initVideoReceiver(rig.receiver, &rig.window);

    emit rig.receiver->onStopComplete(VideoReceiver::STATUS_OK);   // restart in 1 s
    delete rig.receiver.data();
    videoManager._videoReceivers.clear();   // as if the receiver list was rebuilt
    QVERIFY(rig.receiver.isNull());

    QTest::qWait(1500);   // must not crash
}

// G84: when the QML Loader destroys the video item, the receiver's widget and sink are cleared
// and the receiver is not started into the dead sink.
void VideoSinkTest::_widgetDestroyedClearsSink()
{
    if (!qtVideoOnly()) {
        QSKIP("Needs a VideoOutput video backend (GStreamer off, FFmpeg receiver or Qt Multimedia)");
    }

    setSavedSource(VideoSettings::videoSourceUDPH264);

    FakeRig rig;
    VideoManager videoManager;
    videoManager._initVideoReceiver(rig.receiver, &rig.window);
    emit rig.receiver->onStartComplete(VideoReceiver::STATUS_OK);
    QVERIFY(rig.receiver->started());
    QCOMPARE(rig.receiver->widget(), static_cast<QQuickItem*>(rig.output.data()));
    QVERIFY(rig.receiver->sink() != nullptr);

    delete rig.output.data();
    QVERIFY(rig.output.isNull());
    QCOMPARE(rig.receiver->widget(), nullptr);
    QCOMPARE(rig.receiver->sink(), nullptr);
    QCOMPARE(rig.receiver->stops, 1);

    emit rig.receiver->onStopComplete(VideoReceiver::STATUS_OK);
    const int starts = rig.receiver->starts;
    QTest::qWait(1500);   // the restart finds no sink and does not start
    QCOMPARE(rig.receiver->starts, starts);
    QVERIFY(!rig.receiver->started());
}

// G84: a screenshot with Qt Multimedia video fails cleanly (status, log line, no file), with
// and without a video sink.
void VideoSinkTest::_screenshotFailsCleanly()
{
    if (!VideoManager::qtmultimediaEnabled()) {
        QSKIP("Qt video streaming is off in this build");
    }

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString imageFile = dir.filePath(QStringLiteral("shot.jpg"));

    QtMultimediaReceiver receiver;
    receiver.setName(QStringLiteral("videoContent"));
    QSignalSpy spy(&receiver, &VideoReceiver::onTakeScreenshotComplete);
    QVERIFY(spy.isValid());

    {
        LogCapture log;
        receiver.takeScreenshot(imageFile);
        QVERIFY(log.any(QStringLiteral("Screenshot is not supported")));
    }
    QCOMPARE(spy.count(), 1);
    QCOMPARE(qvariant_cast<VideoReceiver::STATUS>(spy.at(0).at(0)), VideoReceiver::STATUS_NOT_IMPLEMENTED);
    QVERIFY(!QFileInfo::exists(imageFile));

    QQuickVideoOutput videoOutput;
    receiver.startDecoding(videoOutput.videoSink());
    receiver.takeScreenshot(imageFile);
    QCOMPARE(spy.count(), 2);
    QCOMPARE(qvariant_cast<VideoReceiver::STATUS>(spy.at(1).at(0)), VideoReceiver::STATUS_NOT_IMPLEMENTED);
    QVERIFY(!QFileInfo::exists(imageFile));
    receiver.stopDecoding();
}

// G88: with GStreamer off and the FFmpeg receiver built, a stream source decodes into the
// Fly view's VideoOutput: GC's own receiver, the VideoOutput's sink, no "video is off", and
// switching the source to Video Stream Disabled and back works.
void VideoSinkTest::_streamSourceUsesVideoOutput()
{
#ifdef QGC_FFMPEG_STREAMING
    if (!qtVideoOnly()) {
        QSKIP("Needs a VideoOutput video backend (GStreamer off)");
    }

    setSavedSource(VideoSettings::videoSourceUDPH264);   // loopback only

    QQuickWindow window;
    QQuickVideoOutput *const videoContent = new QQuickVideoOutput(window.contentItem());
    videoContent->setObjectName(QStringLiteral("videoContent"));
    QQuickVideoOutput *const thermalVideo = new QQuickVideoOutput(window.contentItem());
    thermalVideo->setObjectName(QStringLiteral("thermalVideo"));

    VideoManager videoManager;
    QSignalSpy videoOffSpy(&videoManager, &VideoManager::videoOff);
    videoManager.init(&window);
    QCoreApplication::processEvents();

    QCOMPARE(videoManager._videoReceivers.size(), 2);
    VideoReceiver *const main = videoManager._videoReceivers.at(0);
    QVERIFY(qobject_cast<FFmpegReceiver*>(main) != nullptr);
    QCOMPARE(main->sink(), static_cast<void*>(videoContent->videoSink()));
    QCOMPARE(videoManager._videoReceivers.at(1)->sink(), static_cast<void*>(thermalVideo->videoSink()));
    QVERIFY(!videoManager._videoSinkFailed);
    QVERIFY(!videoManager._receiverCannotPlay);
    QCOMPARE(videoOffSpy.count(), 0);
    QVERIFY(videoManager.hasVideo());
    QVERIFY(main->started());

    VideoSettings *const vs = SettingsManager::instance()->videoSettings();
    for (int i = 0; i < 3; ++i) {
        vs->videoSource()->setRawValue(QString::fromLatin1(VideoSettings::videoDisabled));
        QCoreApplication::processEvents();
        QVERIFY(!videoManager.hasVideo());
        QTRY_VERIFY_WITH_TIMEOUT(!main->started(), 3000);
        vs->videoSource()->setRawValue(QString::fromLatin1(VideoSettings::videoSourceUDPH264));
        QCoreApplication::processEvents();
        QVERIFY(videoManager.hasVideo());
        QTRY_VERIFY_WITH_TIMEOUT(main->started(), 3000);
    }
    QCOMPARE(videoOffSpy.count(), 0);

    videoManager.stopVideo();
    QCoreApplication::processEvents();
#else
    QSKIP("The FFmpeg receiver is not in this build");
#endif
}

// G88: a receiver that cannot play the source in this build (STATUS_NOT_IMPLEMENTED) turns video
// off with the G83 message: a banner at start, a dialog for a source just chosen in the settings,
// no retries, and another source gets a new try.
void VideoSinkTest::_cannotPlayShowsMessage()
{
    if (!qtVideoOnly()) {
        QSKIP("Needs a VideoOutput video backend (GStreamer off, FFmpeg receiver or Qt Multimedia)");
    }

    setSavedSource(VideoSettings::videoSourceUDPH264);

    FakeRig rig;
    VideoManager videoManager;
    QSignalSpy videoOffSpy(&videoManager, &VideoManager::videoOff);
    QVERIFY(videoOffSpy.isValid());
    videoManager._initVideoReceiver(rig.receiver, &rig.window);
    QVERIFY(rig.receiver->sink() != nullptr);
    QCOMPARE(rig.receiver->starts, 1);
    QVERIFY(videoManager.hasVideo());

    emit rig.receiver->onStartComplete(VideoReceiver::STATUS_NOT_IMPLEMENTED);
    QCOMPARE(videoOffSpy.count(), 1);
    QCOMPARE(videoOffSpy.at(0).at(0).toString(), kVideoOffText);
    QCOMPARE(videoOffSpy.at(0).at(1).toBool(), false);   // at start: banner
    QVERIFY(!videoManager.hasVideo());
    QVERIFY(!rig.receiver->started());
    QTest::qWait(1500);
    QCOMPARE(rig.receiver->starts, 1);   // not retried
    QCOMPARE(savedSource(), QString::fromLatin1(VideoSettings::videoSourceUDPH264));

    // Another source chosen in the settings gets a new try; it cannot play either: dialog
    VideoSettings *const vs = SettingsManager::instance()->videoSettings();
    vs->videoSource()->setRawValue(QString::fromLatin1(VideoSettings::videoSourceRTSP));
    videoManager._applyVideoSource(VideoManager::VideoOffTrigger::Settings);
    QVERIFY(!videoManager._receiverCannotPlay);
    QCOMPARE(rig.receiver->starts, 2);
    QCOMPARE(videoOffSpy.count(), 1);
    emit rig.receiver->onStartComplete(VideoReceiver::STATUS_NOT_IMPLEMENTED);
    QCOMPARE(videoOffSpy.count(), 2);
    QCOMPARE(videoOffSpy.at(1).at(0).toString(), kVideoOffText);
    QCOMPARE(videoOffSpy.at(1).at(1).toBool(), true);
    QVERIFY(!videoManager.hasVideo());

    // A source that plays: no message, video on
    vs->videoSource()->setRawValue(QString::fromLatin1(VideoSettings::videoSourceUDPH264));
    videoManager._applyVideoSource(VideoManager::VideoOffTrigger::Settings);
    QCOMPARE(rig.receiver->starts, 3);
    emit rig.receiver->onStartComplete(VideoReceiver::STATUS_OK);
    QVERIFY(videoManager.hasVideo());
    QVERIFY(rig.receiver->started());
    QCOMPARE(videoOffSpy.count(), 2);
}
