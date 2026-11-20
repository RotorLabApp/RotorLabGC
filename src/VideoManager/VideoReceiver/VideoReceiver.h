/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QRegularExpression>
#include <QtCore/QSize>
#include <QtCore/QTimer>
#include <QtQuick/QQuickItem>

class QGCVideoStreamInfo;
class VideoPacketTap;

class VideoReceiver : public QObject
{
    Q_OBJECT

public:
    explicit VideoReceiver(QObject *parent = nullptr)
        : QObject(parent)
    {}

    bool isThermal() const { return (_name == QStringLiteral("thermalVideo")); }

    /// G84: returns text with the user name and password (user:pass@) removed from every
    /// URL in it, so a video URL can be logged without its credentials. Use it for every
    /// log line that prints a video URI or an error string that may contain one.
    static QString redactedUri(const QString &text)
    {
        // scheme:// then the userinfo up to the last '@' before the path, a space or the end
        static const QRegularExpression userInfo(QStringLiteral(R"(([A-Za-z][A-Za-z0-9+.\-]*://)[^/\s]*@)"));
        QString result = text;
        return result.replace(userInfo, QStringLiteral(R"(\1)"));
    }

    void *sink() { return _sink; }
    QQuickItem *widget() { return _widget.data(); }
    QString name() const { return _name; }
    QString uri() const { return _uri; }
    bool started() const { return _started; }
    bool lowLatency() const { return _lowLatency; }
    QGCVideoStreamInfo *videoStreamInfo() { return _videoStreamInfo; }
    QString recordingOutput() const { return _recordingOutput; }
    /// G88: every received access unit, before decoding, for recording (G95) and relay (G109).
    /// Null for receivers without one (GStreamer, Qt Multimedia). See VideoPacketTap.h.
    virtual VideoPacketTap *packetTap() { return nullptr; }

    virtual void setSink(void *sink) { if (sink != _sink) { _sink = sink; emit sinkChanged(_sink); } }
    virtual void setWidget(QQuickItem *widget) { if (widget != _widget) { _widget = widget; emit widgetChanged(widget); } }
    void setName(const QString &name) { if (name != _name) { _name = name; emit nameChanged(_name); } }
    void setUri(const QString &uri) { if (uri != _uri) { _uri = uri; emit uriChanged(_uri); } }
    void setStarted(bool started) { if (started != _started) { _started = started; emit startedChanged(_started); } }
    void setLowLatency(bool lowLatency) { if (lowLatency != _lowLatency) { _lowLatency = lowLatency; emit lowLatencyChanged(_lowLatency); } }
    void setVideoStreamInfo(QGCVideoStreamInfo *videoStreamInfo) { if (videoStreamInfo != _videoStreamInfo) { _videoStreamInfo = videoStreamInfo; emit videoStreamInfoChanged(); } }

    // QMediaFormat::FileFormat
    enum FILE_FORMAT {
        FILE_FORMAT_MIN = 0,
        FILE_FORMAT_MKV = FILE_FORMAT_MIN,
        FILE_FORMAT_MOV,
        FILE_FORMAT_MP4,
        FILE_FORMAT_MAX = FILE_FORMAT_MP4
    };
    Q_ENUM(FILE_FORMAT)
    static bool isValidFileFormat(FILE_FORMAT format) { return ((format >= FILE_FORMAT_MIN) && (format <= FILE_FORMAT_MAX)); }

    enum STATUS {
        STATUS_MIN = 0,
        STATUS_OK = STATUS_MIN,
        STATUS_FAIL,
        STATUS_INVALID_STATE,
        STATUS_INVALID_URL,
        STATUS_NOT_IMPLEMENTED,
        STATUS_MAX = STATUS_NOT_IMPLEMENTED
    };
    Q_ENUM(STATUS)
    static bool isValidStatus(STATUS status) { return ((status >= STATUS_MIN) && (status <= STATUS_MAX)); }

    /// G88 (m4080): why a started receiver gets no video, for the user. Changed on the GUI
    /// thread only; cleared by start, stop and when the cause is gone.
    enum RECEIVE_STATUS {
        RECEIVE_STATUS_OK = 0,
        RECEIVE_STATUS_PORT_IN_USE,     ///< the UDP video port is held by another program
    };
    Q_ENUM(RECEIVE_STATUS)
    RECEIVE_STATUS receiveStatus() const { return _receiveStatus; }
    /// Text for the user while receiveStatus() is not RECEIVE_STATUS_OK, else empty
    QString receiveStatusText() const { return _receiveStatusText; }

signals:
    void timeout();
    void streamingChanged(bool active);
    void decodingChanged(bool active);
    void recordingChanged(bool active);
    void recordingStarted(const QString &filename);
    void videoSizeChanged(QSize size);
    /// G88 (m4080): emitted once per change, never repeated while the cause stays
    void receiveStatusChanged(VideoReceiver::RECEIVE_STATUS status, const QString &text);

    void sinkChanged(void *sink);
    void nameChanged(const QString &name);
    void uriChanged(const QString &uri);
    void startedChanged(bool started);
    void lowLatencyChanged(bool lowLatency);
    void videoStreamInfoChanged();
    void widgetChanged(QQuickItem *widget);

    void onStartComplete(STATUS status);
    void onStopComplete(STATUS status);
    void onStartDecodingComplete(STATUS status);
    void onStopDecodingComplete(STATUS status);
    void onStartRecordingComplete(STATUS status);
    void onStopRecordingComplete(STATUS status);
    void onTakeScreenshotComplete(STATUS status);

public slots:
    virtual void start(uint32_t timeout) = 0;
    virtual void stop() = 0;
    virtual void startDecoding(void *sink) = 0;
    virtual void stopDecoding() = 0;
    virtual void startRecording(const QString &videoFile, FILE_FORMAT format) = 0;
    virtual void stopRecording() = 0;
    virtual void takeScreenshot(const QString &imageFile) = 0;

protected:
    void setReceiveStatus(RECEIVE_STATUS status, const QString &text)
    {
        if ((status != _receiveStatus) || (text != _receiveStatusText)) {
            _receiveStatus = status;
            _receiveStatusText = text;
            emit receiveStatusChanged(_receiveStatus, _receiveStatusText);
        }
    }

    void *_sink = nullptr;
    QPointer<QQuickItem> _widget;   ///< G84: the QML item can be destroyed (Loader) while the receiver lives
    QGCVideoStreamInfo *_videoStreamInfo = nullptr;
    QString _name;
    QString _uri;
    bool _started = false;
    bool _decoding = false;
    bool _recording = false;
    bool _streaming = false;
    bool _lowLatency = false;
    bool _resetVideoSink = false;
    bool _endOfStream = false;
    bool _removingDecoder = false;
    bool _removingRecorder = false;
    // buffer:
    //      -1 - disable buffer and video sync
    //      0 - default buffer length
    //      N - buffer length, ms
    int _buffer = 0;
    qint64 _lastSourceFrameTime = 0;
    qint64 _lastVideoFrameTime = 0;
    QTimer _watchdogTimer;
    uint32_t _signalDepth = 0;
    uint32_t _timeout = 0;
    QString _recordingOutput;
    RECEIVE_STATUS _receiveStatus = RECEIVE_STATUS_OK;
    QString _receiveStatusText;

    // bool _initialized = false;
    // bool _fullScreen = false;
    // QSize _videoSize;
    // QString _imageFile;
};
