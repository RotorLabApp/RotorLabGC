/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QLoggingCategory>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QRunnable>
#include <QtCore/QSize>
// #include <QtQmlIntegration/QtQmlIntegration>

Q_DECLARE_LOGGING_CATEGORY(VideoManagerLog)

class QQuickWindow;
class FinishVideoInitialization;
class SubtitleWriter;
class Vehicle;
class VideoReceiver;
class VideoSettings;

class VideoManager : public QObject
{
    Q_OBJECT
    friend class VideoSinkTest;
    // QML_ELEMENT
    // QML_UNCREATABLE("")
    Q_MOC_INCLUDE("Vehicle.h")
    Q_PROPERTY(bool     gstreamerEnabled        READ gstreamerEnabled                           CONSTANT)
    Q_PROPERTY(bool     qtmultimediaEnabled     READ qtmultimediaEnabled                        CONSTANT)
    Q_PROPERTY(bool     ffmpegEnabled           READ ffmpegEnabled                              CONSTANT)
    Q_PROPERTY(QString  recordStatusChipSource  READ recordStatusChipSource                     CONSTANT)
    Q_PROPERTY(bool     uvcEnabled              READ uvcEnabled                                 CONSTANT)
    Q_PROPERTY(bool     autoStreamConfigured    READ autoStreamConfigured                       NOTIFY autoStreamConfiguredChanged)
    Q_PROPERTY(bool     decoding                READ decoding                                   NOTIFY decodingChanged)
    Q_PROPERTY(bool     fullScreen              READ fullScreen             WRITE setfullScreen NOTIFY fullScreenChanged)
    Q_PROPERTY(bool     hasThermal              READ hasThermal                                 NOTIFY decodingChanged)
    Q_PROPERTY(bool     hasVideo                READ hasVideo                                   NOTIFY hasVideoChanged)
    Q_PROPERTY(bool     isStreamSource          READ isStreamSource                             NOTIFY isStreamSourceChanged)
    Q_PROPERTY(bool     isUvc                   READ isUvc                                      NOTIFY isUvcChanged)
    Q_PROPERTY(bool     recording               READ recording                                  NOTIFY recordingChanged)
    Q_PROPERTY(bool     streaming               READ streaming                                  NOTIFY streamingChanged)
    Q_PROPERTY(double   aspectRatio             READ aspectRatio                                NOTIFY aspectRatioChanged)
    Q_PROPERTY(double   hfov                    READ hfov                                       NOTIFY aspectRatioChanged)
    Q_PROPERTY(double   thermalAspectRatio      READ thermalAspectRatio                         NOTIFY aspectRatioChanged)
    Q_PROPERTY(double   thermalHfov             READ thermalHfov                                NOTIFY aspectRatioChanged)
    Q_PROPERTY(QSize    videoSize               READ videoSize                                  NOTIFY videoSizeChanged)
    Q_PROPERTY(QString  imageFile               READ imageFile                                  NOTIFY imageFileChanged)
    Q_PROPERTY(QString  uvcVideoSourceID        READ uvcVideoSourceID                           NOTIFY uvcVideoSourceIDChanged)
    Q_PROPERTY(QString  receiveStatusText       READ receiveStatusText                          NOTIFY receiveStatusTextChanged)

public:
    explicit VideoManager(QObject *parent = nullptr);
    ~VideoManager();

    static VideoManager *instance();
    static void registerQmlTypes();

    Q_INVOKABLE void grabImage(const QString &imageFile = QString());
    Q_INVOKABLE void startRecording(const QString &videoFile = QString());
    Q_INVOKABLE void startVideo();
    Q_INVOKABLE void stopRecording();
    Q_INVOKABLE void stopVideo();

    void init(QQuickWindow *rootWindow);
    void cleanup();
    bool autoStreamConfigured() const;
    bool decoding() const { return _decoding; }
    bool fullScreen() const { return _fullScreen; }
    bool hasThermal() const;
    bool hasVideo() const;
    bool isStreamSource() const;
    bool isUvc() const;
    bool recording() const { return _recording; }
    bool streaming() const { return _streaming; }
    double aspectRatio() const;
    double hfov() const;
    double thermalAspectRatio() const;
    double thermalHfov() const;
    QSize videoSize() const { return _videoSize; }
    QString imageFile() const { return _imageFile; }
    QString uvcVideoSourceID() const { return _uvcVideoSourceID; }
    /// G88 (m4080): why the main video receiver gets no video (for example its UDP port is in use), else empty
    QString receiveStatusText() const { return _receiveStatusText; }
    void setfullScreen(bool on);
    static bool gstreamerEnabled();
    static bool qtmultimediaEnabled();
    /// G88: GC's own FFmpeg receiver is built (and, without GStreamer, is the stream receiver)
    static bool ffmpegEnabled();
    /// G88: stream video decodes into the Fly view's VideoOutput (no GStreamer; FFmpeg or Qt Multimedia)
    static bool videoOutputBackend();
    /// G95: the record status chip's QML when that module is in this build, else empty
    static QString recordStatusChipSource();
    static bool uvcEnabled();

signals:
    void aspectRatioChanged();
    void autoStreamConfiguredChanged();
    void decodingChanged();
    void fullScreenChanged();
    void hasVideoChanged();
    void imageFileChanged(const QString &filename);
    void isAutoStreamChanged();
    void isStreamSourceChanged();
    void isUvcChanged();
    void recordingChanged();
    void recordingStarted(const QString &filename);
    void streamingChanged();
    void uvcVideoSourceIDChanged();
    void videoSizeChanged();
    void receiveStatusTextChanged();
    /// G83: video could not start for the selected source, so video is off for this run.
    /// modal is true only when the user just changed the video settings (a dialog); at start
    /// and when an aircraft offers a stream it is false (the non-modal "Video" banner).
    void videoOff(const QString &message, bool modal);

private slots:
    void _communicationLostChanged(bool communicationLost);
    void _setActiveVehicle(Vehicle *vehicle);
    void _videoSourceChanged();
    void _videoSettingsChanged();

private:
    /// G83/G67: what caused a video source check, which decides how "video is off" is shown
    enum class VideoOffTrigger {
        Start,      ///< GC start: banner
        Settings,   ///< the user changed the video settings: dialog (saved source only)
        Vehicle,    ///< the active vehicle or its stream changed: banner
    };

    void _applyVideoSource(VideoOffTrigger trigger);
    void _initVideoReceiver(VideoReceiver *receiver, QQuickWindow *window);
    bool _updateAutoStream(VideoReceiver *receiver);
    bool _updateUVC(VideoReceiver *receiver);
    bool _updateSettings(VideoReceiver *receiver);
    bool _updateVideoUri(VideoReceiver *receiver, const QString &uri);
    void _restartAllVideos();
    void _restartVideo(VideoReceiver *receiver);
    void _startReceiver(VideoReceiver *receiver);
    void _stopReceiver(VideoReceiver *receiver);
    bool _videoUnavailable() const;
    bool _videoWanted() const;
    QString _effectiveVideoSource() const;
    void _reportVideoOff(VideoOffTrigger trigger);
    static void _cleanupOldVideos();

    QList<VideoReceiver*> _videoReceivers;

    SubtitleWriter *_subtitleWriter = nullptr;
    VideoSettings *_videoSettings = nullptr;

    bool _initialized = false;
    bool _fullScreen = false;
    bool _videoSinkFailed = false;  ///< G83: the main video widget gave no video sink this run
    bool _receiverCannotPlay = false;   ///< G88: the main receiver cannot play the current source in this build
    VideoOffTrigger _lastTrigger = VideoOffTrigger::Start;  ///< G88: what started the last source check
    QString _videoOffSource;        ///< G83: saved source the "video is off" message was last shown for
    QList<QPointer<Vehicle>> _videoOffVehicles;  ///< G67: vehicle connections the aircraft stream message was shown for
    QString _autoStreamSource;      ///< G84: source set by the vehicle for this session only, never saved
    QAtomicInteger<bool> _decoding = false;
    QAtomicInteger<bool> _recording = false;
    QAtomicInteger<bool> _streaming = false;
    QSize _videoSize;
    QString _imageFile;
    QString _uvcVideoSourceID;
    QString _receiveStatusText;     ///< G88 (m4080): the main receiver's receiveStatusText
    Vehicle *_activeVehicle = nullptr;
};

/*===========================================================================*/

class FinishVideoInitialization : public QRunnable
{
public:
    FinishVideoInitialization();
    ~FinishVideoInitialization();

    void run() final;
};
