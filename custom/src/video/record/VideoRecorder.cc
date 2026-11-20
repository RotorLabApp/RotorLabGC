/****************************************************************************
 *
 * RotorLab GC - local video recording, the module's front (G95).
 *
 ****************************************************************************/

#include "VideoRecorder.h"
#include "RecordingRecovery.h"
#include "RecordingStore.h"
#include "VehicleClock.h"
#include "VideoUploader.h"
#include "VideoTapRegistry.h"

#include <QtCore/QApplicationStatic>
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonObject>
#include <QtCore/QSettings>
#include <QtCore/QSysInfo>
#include <QtCore/QUrl>
#include <QtCore/QVariantMap>
#include <QtGui/QDesktopServices>

#include "AppSettings.h"
#include "MultiVehicleManager.h"
#include "QmlObjectListModel.h"
#include "SettingsManager.h"
#include "Vehicle.h"
#include "VideoPacketTap.h"
#include "fleet/FleetClient.h"
#include "fleet/FleetConnector.h"
#include "fleet/FleetStore.h"

#ifdef QGC_FFMPEG_STREAMING
#include "Fmp4Muxer.h"
#include "RecordingWriter.h"
#endif

#include "QGCLoggingCategory.h"

Q_DECLARE_LOGGING_CATEGORY(VideoRecordLog)

Q_APPLICATION_STATIC(VideoRecorder, _videoRecorderInstance);

namespace {

constexpr int kDisarmGraceMs = 10000;
constexpr int kTapCapacity = 300;          ///< about 10 s at 30 fps: room for a slow disk
constexpr const char *kGroup = "VideoRecord";

#ifndef RLGC_GIT_COMMIT
#define RLGC_GIT_COMMIT ""
#endif

} // namespace

VideoRecorder *VideoRecorder::instance()
{
    return _videoRecorderInstance();
}

VideoRecorder::VideoRecorder(QObject *parent)
    : QObject(parent)
    , _store(new RecordingStore())
    , _clock(new VehicleClock(this))
    , _client(new FleetClient(this))
{
    _loadSettings();

    _uploader = new VideoUploader(_store, _client,
                                  VideoUploader::fleetEnvironment([this]() { return _uploadToFleet; }), this);
    connect(_uploader, &VideoUploader::changed, this, &VideoRecorder::queueChanged);

    if (FleetConnector *fc = FleetConnector::instance()) {
        connect(fc, &FleetConnector::stateChanged, this, [this, fc]() {
            switch (fc->state()) {
            case FleetConnector::Revoked:
            case FleetConnector::NotConnected:
                // Abort what is on the wire; keep every file and entry.
                _uploader->stopAll();
                break;
            case FleetConnector::Connected:
                _uploader->resume();
                break;
            default:
                break;
            }
            if (fc->suspended()) {
                _uploader->stopAll();
            }
            emit queueChanged();
        });
    }

    if (MultiVehicleManager *mvm = MultiVehicleManager::instance()) {
        connect(mvm, &MultiVehicleManager::vehicleAdded, this, &VideoRecorder::_watchVehicle);
        connect(mvm, &MultiVehicleManager::vehicleRemoved, this, [this](Vehicle *v) {
            if (v) {
                _onArmed(v->id(), false);
            }
        });
        if (QmlObjectListModel *vehicles = mvm->vehicles()) {
            for (int i = 0; i < vehicles->count(); ++i) {
                _watchVehicle(qobject_cast<Vehicle *>((*vehicles)[i]));
            }
        }
    }

    _disarmTimer.setSingleShot(true);
    _disarmTimer.setInterval(kDisarmGraceMs);
    connect(&_disarmTimer, &QTimer::timeout, this, [this]() {
        if (_armed.isEmpty() && _mode == ModeWhileArmed) {
            _setWanted(false);
        }
    });
    _refTimer.setInterval(2000);
    connect(&_refTimer, &QTimer::timeout, this, &VideoRecorder::_refreshRefs);
    _tapTimer.setInterval(2000);
    connect(&_tapTimer, &QTimer::timeout, this, &VideoRecorder::_attachTap);
    _tick.setInterval(1000);
    connect(&_tick, &QTimer::timeout, this, &VideoRecorder::statusChanged);

    connect(qApp, &QCoreApplication::aboutToQuit, this, &VideoRecorder::_shutdown);

    _startRecovery();
}

VideoRecorder::~VideoRecorder()
{
    _shutdown();
    delete _store;
}

bool VideoRecorder::available() const
{
#ifdef QGC_FFMPEG_STREAMING
    return true;
#else
    return false;
#endif
}

QString VideoRecorder::unavailableText() const
{
    return available() ? QString()
                       : tr("Video recording needs this version's built-in video receiver, which this build does not include.");
}

// ------------------------------------------------------------- settings

void VideoRecorder::_loadSettings()
{
    QSettings s;
    s.beginGroup(QLatin1String(kGroup));
    const QString mode = s.value(QStringLiteral("mode"), QStringLiteral("armed")).toString();
    _mode = (mode == QLatin1String("off")) ? ModeOff : (mode == QLatin1String("always")) ? ModeAlways : ModeWhileArmed;
    _uploadToFleet = s.value(QStringLiteral("uploadToFleet"), true).toBool();
    _camera = s.value(QStringLiteral("camera"), QStringLiteral("main")).toString();
    if (_camera.trimmed().isEmpty()) {
        _camera = QStringLiteral("main");
    }
    s.endGroup();
}

void VideoRecorder::_saveSettings()
{
    QSettings s;
    s.beginGroup(QLatin1String(kGroup));
    s.setValue(QStringLiteral("mode"), _mode == ModeOff ? QStringLiteral("off")
                                       : _mode == ModeAlways ? QStringLiteral("always") : QStringLiteral("armed"));
    s.setValue(QStringLiteral("uploadToFleet"), _uploadToFleet);
    s.setValue(QStringLiteral("camera"), _camera);
    s.endGroup();
}

void VideoRecorder::setMode(int mode)
{
    if (mode < ModeOff || mode > ModeAlways || mode == _mode) {
        return;
    }
    _mode = mode;
    _saveSettings();
    _stoppedText.clear();
    emit settingsChanged();
    emit statusChanged();
    _apply();
}

void VideoRecorder::setUploadToFleet(bool on)
{
    if (on == _uploadToFleet) {
        return;
    }
    _uploadToFleet = on;
    _saveSettings();
    if (!on) {
        _uploader->stopAll();
    } else {
        _uploader->kick();
    }
    emit settingsChanged();
    emit queueChanged();
}

void VideoRecorder::setCamera(const QString &camera)
{
    // Fleet takes 1 to 40 letters, digits, spaces, _ or -.
    QString c;
    for (const QChar ch : camera.trimmed()) {
        if (ch.isLetterOrNumber() && ch.unicode() < 128) {
            c += ch;
        } else if (ch == QLatin1Char(' ') || ch == QLatin1Char('_') || ch == QLatin1Char('-')) {
            c += ch;
        }
    }
    c = c.trimmed().left(40);
    if (c.isEmpty()) {
        c = QStringLiteral("main");
    }
    if (c == _camera) {
        return;
    }
    _camera = c;
    _saveSettings();
    emit settingsChanged();
}

QString VideoRecorder::folder() const
{
    AppSettings *app = SettingsManager::instance() ? SettingsManager::instance()->appSettings() : nullptr;
    const QString video = app ? app->videoSavePath() : QString();
    return video.isEmpty() ? QString() : QDir(video).filePath(QStringLiteral("Recordings"));
}

QString VideoRecorder::_stationName() const
{
    const QString name = FleetStore::stationName();
    return name.isEmpty() ? QSysInfo::machineHostName() : name;
}

// ------------------------------------------------------------- recovery

void VideoRecorder::_startRecovery()
{
    const QString dir = folder();
    const QSet<QString> known = _store->names();
    auto found = std::make_shared<QList<QJsonObject>>();
    QThread *thread = QThread::create([dir, known, found]() {
        *found = RecordingRecovery::scan(dir, known);
    });
    connect(thread, &QThread::finished, this, [this, thread, found]() {
        thread->deleteLater();
        for (QJsonObject e : std::as_const(*found)) {
            if (e.value(QStringLiteral("state")).toString().isEmpty()) {
                e[QStringLiteral("state")] = QStringLiteral("waiting");
            }
            _store->upsert(e);
        }
        _recoveryDone = true;
        emit queueChanged();
        _uploader->kick();
        _apply();
    });
    thread->start();
}

// ------------------------------------------------------------- arming

void VideoRecorder::_watchVehicle(Vehicle *vehicle)
{
    if (!vehicle) {
        return;
    }
    const int sysid = vehicle->id();
    connect(vehicle, &Vehicle::armedChanged, this, [this, sysid](bool armed) { _onArmed(sysid, armed); });
    if (vehicle->armed()) {
        _onArmed(sysid, true);
    }
}

QString VideoRecorder::_flightRef(int sysid) const
{
    const QJsonObject state = FleetStore::flightState(sysid);
    return state.value(QStringLiteral("active")).toBool() ? state.value(QStringLiteral("ref")).toString() : QString();
}

void VideoRecorder::_onArmed(int sysid, bool armed)
{
#ifdef QGC_FFMPEG_STREAMING
    if (armed) {
        if (_armed.contains(sysid)) {
            return;
        }
        const bool first = _armed.isEmpty();
        _armed.insert(sysid);
        _disarmTimer.stop();
        const QString ref = _flightRef(sysid);
        _refs.insert(sysid, ref);
        _refTimer.start();
        if (first || _sysid == 0) {
            _sysid = sysid;
        }
        if (_writer) {
            RecordingWriter *w = _writer;
            const int vehicleSysid = _sysid;
            const QString vehicleRef = _refs.value(_sysid);
            QMetaObject::invokeMethod(w, [w, sysid, ref, vehicleSysid, vehicleRef]() {
                w->setVehicle(vehicleSysid, QStringLiteral("Vehicle %1").arg(vehicleSysid), vehicleRef);
                w->mark(QStringLiteral("arm"), sysid, ref);
            }, Qt::QueuedConnection);
        }
        if (first && _mode == ModeWhileArmed) {
            if (_halted) {
                _setWanted(false);      // a new flight clears a disk or write stop
            }
            _setWanted(true);
        }
    } else {
        if (!_armed.remove(sysid)) {
            return;
        }
        const QString ref = _refs.value(sysid);
        if (_writer) {
            RecordingWriter *w = _writer;
            QMetaObject::invokeMethod(w, [w, sysid, ref]() { w->mark(QStringLiteral("disarm"), sysid, ref); },
                                      Qt::QueuedConnection);
        }
        if (_armed.isEmpty()) {
            _refTimer.stop();
            _disarmTimer.start();       // the landing goes in the file
        } else if (_sysid == sysid) {
            _sysid = *_armed.constBegin();
        }
    }
#else
    Q_UNUSED(sysid);
    Q_UNUSED(armed);
#endif
}

void VideoRecorder::_refreshRefs()
{
#ifdef QGC_FFMPEG_STREAMING
    // FleetTelemetry mints the flight ref on its own tick after the arm.
    for (const int sysid : std::as_const(_armed)) {
        const QString ref = _flightRef(sysid);
        if (!ref.isEmpty() && ref != _refs.value(sysid)) {
            _refs.insert(sysid, ref);
            if (_writer && sysid == _sysid) {
                RecordingWriter *w = _writer;
                QMetaObject::invokeMethod(w, [w, sysid, ref]() {
                    w->setVehicle(sysid, QStringLiteral("Vehicle %1").arg(sysid), ref);
                    w->mark(QStringLiteral("flight_ref"), sysid, ref);
                }, Qt::QueuedConnection);
            }
        }
    }
#endif
}

// ------------------------------------------------------------- the writer

void VideoRecorder::_apply()
{
#ifdef QGC_FFMPEG_STREAMING
    if (!_recoveryDone || _shutDown) {
        return;                         // recovery runs before any new recording
    }
    if (_mode == ModeOff) {
        _setWanted(false);
        _tapTimer.stop();
        if (_writer) {
            RecordingWriter *w = _writer;
            QMetaObject::invokeMethod(w, [w]() { (void) w->detachConsumer(); }, Qt::BlockingQueuedConnection);
        }
        // A consumer the tap already let go of (the receiver and its tap were
        // destroyed) must not be handed back to that tap: the pointer may dangle.
        if (_tap && _consumer && _consumer->attached()) {
            _tap->detach(_consumer);
        }
        _consumer.reset();
        _tap = nullptr;
        return;
    }
    _ensureWriter();
    _attachTap();
    _tapTimer.start();
    _setWanted(_mode == ModeAlways || !_armed.isEmpty());
#endif
}

void VideoRecorder::_ensureWriter()
{
#ifdef QGC_FFMPEG_STREAMING
    if (_writer) {
        return;
    }
    RecordingConfig config;
    config.folder = folder();
    config.camera = _camera;
    config.station = _stationName();
    config.gcVersion = QCoreApplication::applicationVersion();
    config.gcCommit = QStringLiteral(RLGC_GIT_COMMIT);
    VehicleClock *clock = _clock;
    config.vehicle = [clock](int sysid) { return clock->snapshot(sysid); };

    _writer = new RecordingWriter(config);
    _writer->moveToThread(&_thread);
    connect(&_thread, &QThread::finished, _writer, &QObject::deleteLater);
    connect(_writer, &RecordingWriter::partOpened, this, &VideoRecorder::_onPartOpened, Qt::QueuedConnection);
    connect(_writer, &RecordingWriter::progress, this, &VideoRecorder::_onProgress, Qt::QueuedConnection);
    connect(_writer, &RecordingWriter::partClosed, this, &VideoRecorder::_onPartClosed, Qt::QueuedConnection);
    connect(_writer, &RecordingWriter::stopped, this, &VideoRecorder::_onStopped, Qt::QueuedConnection);
    _thread.setObjectName(QStringLiteral("VideoRecord"));
    _thread.start(QThread::LowPriority);
#endif
}

void VideoRecorder::_attachTap()
{
#ifdef QGC_FFMPEG_STREAMING
    if (!_writer || _mode == ModeOff) {
        return;
    }
    VideoPacketTap *tap = VideoTapRegistry::tap();
    // _tap is compared or used only while the consumer is still attached; once it is
    // not, the tap may have been destroyed and its pointer may dangle (or be reused).
    const bool attached = _consumer && _consumer->attached();
    if (attached && (tap == _tap)) {
        return;
    }
    if (_tap && attached) {
        _tap->detach(_consumer);
    }
    _consumer.reset();
    _tap = tap;
    if (!tap) {
        return;                         // no receiver yet, or one without a tap
    }
    _consumer = tap->attach(kTapCapacity);
    if (!_consumer) {
        if (!_tapFullLogged) {
            qCWarning(VideoRecordLog) << "video tap has no room for the recorder; recording waits";
            _tapFullLogged = true;
        }
        _tap = nullptr;
        return;
    }
    RecordingWriter *w = _writer;
    auto consumer = _consumer;
    QMetaObject::invokeMethod(w, [w, consumer]() { w->attachConsumer(consumer); }, Qt::QueuedConnection);
#endif
}

void VideoRecorder::_setWanted(bool wanted)
{
#ifdef QGC_FFMPEG_STREAMING
    if (!_writer) {
        _wanted = false;
        return;
    }
    if (wanted) {
        _halted = false;
        _stoppedText.clear();
        // Bind new parts to the Fleet server and station enrolled now.
        const bool enrolled = FleetStore::enrolled();
        const QString base = enrolled ? FleetStore::baseUrl() : QString();
        const int station = enrolled ? FleetStore::stationId() : 0;
        const QString name = _stationName();
        const int sysid = _sysid;
        const QString ref = _refs.value(_sysid);
        RecordingWriter *w = _writer;
        QMetaObject::invokeMethod(w, [w, base, station, name, sysid, ref]() {
            w->setBinding(base, station, name);
            if (sysid > 0) {
                w->setVehicle(sysid, QStringLiteral("Vehicle %1").arg(sysid), ref);
            }
            w->setWanted(true);
        }, Qt::QueuedConnection);
    } else {
        RecordingWriter *w = _writer;
        QMetaObject::invokeMethod(w, [w]() { w->setWanted(false); }, Qt::QueuedConnection);
    }
    _wanted = wanted;
    emit statusChanged();
#else
    Q_UNUSED(wanted);
#endif
}

void VideoRecorder::_shutdown()
{
    if (_shutDown) {
        return;
    }
    _shutDown = true;
    _tapTimer.stop();
    _tick.stop();
#ifdef QGC_FFMPEG_STREAMING
    if (_writer && _thread.isRunning()) {
        RecordingWriter *w = _writer;
        // Close the open part properly (end line, hash, fsync) before GC exits.
        QMetaObject::invokeMethod(w, [w]() { w->shutdown(); }, Qt::BlockingQueuedConnection);
    }
    // As in _apply, never detach from a tap that already let the consumer go
    if (_tap && _consumer && _consumer->attached()) {
        _tap->detach(_consumer);
    }
    _consumer.reset();
    _tap = nullptr;
#endif
    if (_thread.isRunning()) {
        _thread.quit();
        _thread.wait(10000);
    }
    _writer = nullptr;
}

void VideoRecorder::_onPartOpened(const QString &path, qint64 startedUtcMs)
{
    _recording = true;
    _currentPath = path;
    _sessionStartUtcMs = startedUtcMs;
    _partBytes = 0;
    _stoppedText.clear();
    _tick.start();
    emit statusChanged();
    emit queueChanged();
}

void VideoRecorder::_onProgress(qint64 bytes, qint64 startedUtcMs)
{
    _partBytes = bytes;
    _sessionStartUtcMs = startedUtcMs;
    emit statusChanged();
}

void VideoRecorder::_onPartClosed(const QJsonObject &entry)
{
    QJsonObject e = entry;
    e[QStringLiteral("state")] = QStringLiteral("waiting");
    _store->upsert(e);
    _recording = false;
    _currentPath.clear();
    _tick.stop();
    emit statusChanged();
    emit queueChanged();
    _uploader->kick();                  // register as soon as Fleet is reachable
}

void VideoRecorder::_onStopped(int reason, const QString &text)
{
    Q_UNUSED(reason);
    _recording = false;
    _tick.stop();
    _stoppedText = text;
#ifdef QGC_FFMPEG_STREAMING
    if (reason != RecordingWriter::StopSourceStopped) {
        _halted = true;
    }
#endif
    emit statusChanged();
}

// ------------------------------------------------------------- status

QString VideoRecorder::sizeText(qint64 bytes)
{
    if (bytes >= 1024LL * 1024 * 1024) {
        return tr("%1 GB").arg(double(bytes) / (1024.0 * 1024 * 1024), 0, 'f', 2);
    }
    if (bytes >= 1024LL * 1024) {
        return tr("%1 MB").arg(double(bytes) / (1024.0 * 1024), 0, 'f', 1);
    }
    return tr("%1 KB").arg((bytes + 1023) / 1024);
}

QString VideoRecorder::elapsedText() const
{
    if (!_recording || _sessionStartUtcMs <= 0) {
        return QString();
    }
    const qint64 s = qMax(qint64(0), (QDateTime::currentMSecsSinceEpoch() - _sessionStartUtcMs) / 1000);
    return QStringLiteral("%1:%2:%3").arg(s / 3600, 2, 10, QLatin1Char('0'))
                                     .arg((s / 60) % 60, 2, 10, QLatin1Char('0'))
                                     .arg(s % 60, 2, 10, QLatin1Char('0'));
}

QString VideoRecorder::partSizeText() const
{
    return _recording ? sizeText(_partBytes) : QString();
}

QString VideoRecorder::uploadStatusText() const
{
    if (!_uploadToFleet) {
        return tr("Upload is off. Recordings stay on this computer.");
    }
    const QString revoked = _uploader->statusText();
    if (!revoked.isEmpty()) {
        return revoked;
    }
    if (!FleetStore::enrolled()) {
        return tr("This station is not enrolled with a Fleet Server. Recordings stay on this computer.");
    }
    FleetConnector *fc = FleetConnector::instance();
    if (fc && fc->suspended()) {
        return tr("The RotorLab Connector is this station's route. Recordings stay on this computer.");
    }
    return QString();
}

QVariantList VideoRecorder::queue() const
{
    QVariantList out;
    if (_recording && !_currentPath.isEmpty()) {
        QVariantMap row;
        row[QStringLiteral("id")] = QString();
        row[QStringLiteral("name")] = QFileInfo(_currentPath).fileName();
        row[QStringLiteral("size")] = sizeText(_partBytes);
        row[QStringLiteral("state")] = QStringLiteral("recording");
        row[QStringLiteral("stateText")] = tr("Recording");
        row[QStringLiteral("detail")] = QString();
        row[QStringLiteral("canRetry")] = false;
        row[QStringLiteral("canCheck")] = false;
        row[QStringLiteral("canSend")] = false;
        row[QStringLiteral("canDelete")] = false;
        out.append(row);
    }
    const QList<QJsonObject> entries = _store->entries();
    for (qsizetype i = entries.size() - 1; i >= 0; --i) {     // newest first
        const QJsonObject &e = entries.at(i);
        const QString state = e.value(QStringLiteral("state")).toString();
        QVariantMap row;
        row[QStringLiteral("id")] = e.value(QStringLiteral("id")).toString();
        row[QStringLiteral("name")] = e.value(QStringLiteral("name")).toString();
        row[QStringLiteral("size")] = sizeText(e.value(QStringLiteral("size")).toInteger());
        row[QStringLiteral("state")] = state;
        QString text;
        const bool received = !e.value(QStringLiteral("video_receipt")).toObject().isEmpty();
        if (state == QLatin1String("done")) {
            text = received ? tr("Received by Records") : tr("Stored on Fleet");
        } else if (state == QLatin1String("uploading")) {
            text = tr("Uploading %1%").arg(int(e.value(QStringLiteral("progress")).toDouble() * 100.0));
        } else if (state == QLatin1String("registering")) {
            text = tr("Registering");
        } else if (state == QLatin1String("checking")) {
            text = tr("Checking");
        } else if (state == QLatin1String("parked")) {
            text = tr("Parked");
        } else if (state == QLatin1String("wrong_server") || state == QLatin1String("unbound")) {
            text = tr("Kept on this computer");
        } else {
            text = tr("Waiting");
        }
        if (e.value(QStringLiteral("recovered")).toBool()) {
            text += QLatin1String(", ") + tr("recovered after an unexpected stop");
        }
        row[QStringLiteral("stateText")] = text;
        row[QStringLiteral("detail")] = e.value(QStringLiteral("detail")).toString();
        row[QStringLiteral("canRetry")] = (state == QLatin1String("parked") && e.value(QStringLiteral("retryable")).toBool());
        row[QStringLiteral("canCheck")] = (state == QLatin1String("done")
                                           && e.value(QStringLiteral("records_poll_stopped")).toBool());
        row[QStringLiteral("canSend")] = (state == QLatin1String("wrong_server") || state == QLatin1String("unbound"))
                                         && FleetStore::enrolled();
        row[QStringLiteral("canDelete")] = (e.value(QStringLiteral("id")).toString() != _uploader->currentId());
        out.append(row);
    }
    return out;
}

// ------------------------------------------------------------- actions

void VideoRecorder::openFolder()
{
    const QString dir = folder();
    if (dir.isEmpty()) {
        return;
    }
    QDir().mkpath(dir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void VideoRecorder::retry(const QString &id)
{
    _uploader->retry(id);
    emit queueChanged();
}

void VideoRecorder::checkRecords(const QString &id)
{
    _uploader->checkRecords(id);
    emit queueChanged();
}

void VideoRecorder::sendToThisServer(const QString &id)
{
    _uploader->sendToThisServer(id);
    emit queueChanged();
}

QString VideoRecorder::deleteQuestion(const QString &id) const
{
    const QJsonObject e = _store->entry(id);
    if (e.isEmpty()) {
        return QString();
    }
    const QString name = e.value(QStringLiteral("name")).toString();
    if (e.value(QStringLiteral("state")).toString() == QLatin1String("done")
        && !e.value(QStringLiteral("video_receipt")).toObject().isEmpty()) {
        return tr("Delete %1 and its time map from this computer? The Fleet Server confirmed it received both "
                  "with the same SHA-256 and size, and Records sent a receipt for it at %2. The receipt file "
                  "stays in the folder. This cannot be undone.")
            .arg(name, e.value(QStringLiteral("video_receipt_at")).toString());
    }
    if (e.value(QStringLiteral("state")).toString() == QLatin1String("done")) {
        return tr("Delete %1 and its time map from this computer? The Fleet Server confirmed it received both "
                  "with the same SHA-256 and size. This cannot be undone.").arg(name);
    }
    return tr("Delete %1 and its time map from this computer? The Fleet Server has not confirmed a copy, so this "
              "may be the only one. This cannot be undone.").arg(name);
}

void VideoRecorder::deleteRecording(const QString &id)
{
    const QJsonObject e = _store->entry(id);
    if (e.isEmpty()) {
        return;
    }
    if (_uploader->currentId() == id) {
        _uploader->stopAll();
    }
    const QString video = e.value(QStringLiteral("path")).toString();
    const QString sidecar = e.value(QStringLiteral("sidecar_path")).toString();
    qCInfo(VideoRecordLog) << "operator deleted recording" << e.value(QStringLiteral("name")).toString()
                           << "state" << e.value(QStringLiteral("state")).toString()
                           << "sha256" << e.value(QStringLiteral("sha256")).toString();
    if (!video.isEmpty()) {
        (void) QFile::remove(video);
    }
    if (!sidecar.isEmpty()) {
        (void) QFile::remove(sidecar);
    }
    _store->remove(id);
    emit queueChanged();
    _uploader->kick();
}
