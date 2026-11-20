/****************************************************************************
 *
 * RotorLab GC - local video recording, the module's front (G95).
 *
 * One QObject, exposed to QML as `VideoRecorder`: the settings, the start
 * and stop rules, the status chip and the recording queue. It owns the
 * writer's thread, the time source, the start-up recovery and the uploader.
 *
 * Start and stop ("Record video"):
 *   Off
 *   While armed (default): from the first keyframe after any vehicle arms
 *     until 10 s after the last vehicle disarms, so the landing is in it.
 *   Always: from the first keyframe while video is received, until the
 *     source stops.
 *
 * Recording is always the stream as received, never decoded or re-encoded.
 *
 * THE RULE: nothing here may stop, delay or gate a flight, the display or the
 * live relay. A full disk, a failed write or an unreachable Fleet Server stops
 * recording or uploading only, and says so.
 *
 * Built everywhere; records only where GC's own FFmpeg receiver is built
 * (QGC_FFMPEG_STREAMING). Elsewhere `available` is false and the settings
 * section says why.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtCore/QVariantList>

#include <memory>

class FleetClient;
class RecordingStore;
class RecordingWriter;
class VehicleClock;
class Vehicle;
class VideoPacketConsumer;
class VideoPacketTap;
class VideoUploader;

class VideoRecorder : public QObject
{
    Q_OBJECT

public:
    enum Mode { ModeOff = 0, ModeWhileArmed = 1, ModeAlways = 2 };
    Q_ENUM(Mode)

private:
    Q_PROPERTY(bool    available        READ available        CONSTANT)
    Q_PROPERTY(QString unavailableText  READ unavailableText  CONSTANT)
    Q_PROPERTY(int     mode             READ mode             WRITE setMode           NOTIFY settingsChanged)
    Q_PROPERTY(bool    uploadToFleet    READ uploadToFleet    WRITE setUploadToFleet  NOTIFY settingsChanged)
    Q_PROPERTY(QString camera           READ camera           WRITE setCamera         NOTIFY settingsChanged)
    Q_PROPERTY(QString folder           READ folder           NOTIFY settingsChanged)
    Q_PROPERTY(bool    recording        READ recording        NOTIFY statusChanged)
    Q_PROPERTY(QString elapsedText      READ elapsedText      NOTIFY statusChanged)
    Q_PROPERTY(QString partSizeText     READ partSizeText     NOTIFY statusChanged)
    Q_PROPERTY(QString stoppedText      READ stoppedText      NOTIFY statusChanged)
    Q_PROPERTY(bool    chipVisible      READ chipVisible      NOTIFY statusChanged)
    Q_PROPERTY(QVariantList queue       READ queue            NOTIFY queueChanged)
    Q_PROPERTY(QString uploadStatusText READ uploadStatusText NOTIFY queueChanged)

public:
    explicit VideoRecorder(QObject *parent = nullptr);
    ~VideoRecorder() override;

    static VideoRecorder *instance();

    bool    available() const;
    QString unavailableText() const;

    int     mode() const { return _mode; }
    void    setMode(int mode);
    bool    uploadToFleet() const { return _uploadToFleet; }
    void    setUploadToFleet(bool on);
    QString camera() const { return _camera; }
    void    setCamera(const QString &camera);
    QString folder() const;

    bool    recording() const { return _recording; }
    QString elapsedText() const;
    QString partSizeText() const;
    QString stoppedText() const { return _stoppedText; }
    bool    chipVisible() const { return _mode != ModeOff && (_recording || !_stoppedText.isEmpty()); }

    QVariantList queue() const;
    QString uploadStatusText() const;

    Q_INVOKABLE void openFolder();
    Q_INVOKABLE void retry(const QString &id);
    Q_INVOKABLE void sendToThisServer(const QString &id);
    /// Ask the Fleet Server again where a stored recording stands.
    Q_INVOKABLE void checkRecords(const QString &id);
    /// The confirmation sentence for deleting an entry, with its server state.
    Q_INVOKABLE QString deleteQuestion(const QString &id) const;
    /// Delete an entry's video and time map from this computer (after the
    /// operator confirmed). Never called automatically.
    Q_INVOKABLE void deleteRecording(const QString &id);

    static QString sizeText(qint64 bytes);

signals:
    void settingsChanged();
    void statusChanged();
    void queueChanged();

private:
    void _loadSettings();
    void _saveSettings();
    void _startRecovery();
    void _apply();
    void _ensureWriter();
    void _attachTap();
    void _shutdown();
    void _setWanted(bool wanted);
    void _watchVehicle(Vehicle *vehicle);
    void _onArmed(int sysid, bool armed);
    void _refreshRefs();
    QString _flightRef(int sysid) const;
    QString _stationName() const;

    // Writer callbacks (queued to this thread).
    void _onPartOpened(const QString &path, qint64 startedUtcMs);
    void _onProgress(qint64 bytes, qint64 startedUtcMs);
    void _onPartClosed(const QJsonObject &entry);
    void _onStopped(int reason, const QString &text);

    int     _mode = ModeWhileArmed;
    bool    _uploadToFleet = true;
    QString _camera = QStringLiteral("main");

    RecordingStore *_store = nullptr;
    VehicleClock   *_clock = nullptr;
    FleetClient    *_client = nullptr;
    VideoUploader  *_uploader = nullptr;

    QThread          _thread;
    RecordingWriter *_writer = nullptr;
    std::shared_ptr<VideoPacketConsumer> _consumer;
    VideoPacketTap  *_tap = nullptr;
    QTimer           _tapTimer;
    bool             _tapFullLogged = false;

    bool    _recoveryDone = false;
    bool    _wanted = false;
    bool    _halted = false;
    QSet<int> _armed;
    QTimer  _disarmTimer;               ///< 10 s after the last disarm
    QTimer  _refTimer;                  ///< picks up flight refs FleetTelemetry mints after the arm
    QHash<int, QString> _refs;
    int     _sysid = 0;

    bool    _recording = false;
    QString _currentPath;
    qint64  _sessionStartUtcMs = 0;
    qint64  _partBytes = 0;
    QString _stoppedText;
    QTimer  _tick;                      ///< elapsed time on the chip
    bool    _shutDown = false;
};
