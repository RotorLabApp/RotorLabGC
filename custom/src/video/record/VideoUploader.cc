/****************************************************************************
 *
 * RotorLab GC - recorded video to the Fleet Server (G95, contract S21).
 *
 ****************************************************************************/

#include "VideoUploader.h"
#include "RecordingRecovery.h"
#include "RecordingStore.h"
#include "TimeMapWriter.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonValue>
#include <QtCore/QThread>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkReply>

#include <algorithm>
#include <iterator>
#include <memory>

#include "fleet/FleetConnector.h"
#include "fleet/FleetStore.h"

#include "QGCLoggingCategory.h"

Q_DECLARE_LOGGING_CATEGORY(VideoRecordLog)

namespace {

constexpr int kBackoffSeconds[] = {1, 2, 5, 15, 30, 300};
/// Status checks after the final 200: every few minutes, then hourly.
constexpr int kRecordsSeconds[] = {120, 300, 600, 900, 1800, 3600};
constexpr int kSweepMs = 15000;
constexpr char kHandedOver[] = "handed_over";

QString str(const QJsonObject &o, const QString &key)
{
    const QJsonValue v = o.value(key);
    return v.isString() ? v.toString() : (v.isDouble() ? QString::number(v.toInteger()) : QString());
}

/// SHA-256 (hex) of [offset, offset + length) of a file, read in 1 MB blocks.
QByteArray rangeSha(const QString &path, qint64 offset, qint64 length)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || !f.seek(offset)) {
        return {};
    }
    QCryptographicHash h(QCryptographicHash::Sha256);
    qint64 left = length;
    while (left > 0) {
        const QByteArray block = f.read(qMin(left, qint64(1024 * 1024)));
        if (block.isEmpty()) {
            return {};
        }
        h.addData(block);
        left -= block.size();
    }
    return h.result().toHex();
}

} // namespace

VideoUploader::VideoUploader(RecordingStore *store, HttpClient *client, const Environment &env, QObject *parent)
    : QObject(parent)
    , _store(store)
    , _client(client)
    , _env(env)
{
    _timer.setInterval(kSweepMs);
    connect(&_timer, &QTimer::timeout, this, &VideoUploader::_sweep);
    _timer.start();
    _wake.setSingleShot(true);
    connect(&_wake, &QTimer::timeout, this, &VideoUploader::_sweep);
}

VideoUploader::~VideoUploader()
{
    ++_session;
    if (_reply) {
        _reply->abort();
    }
}

void VideoUploader::setSweepMs(int ms)
{
    _timer.setInterval(ms);
}

VideoUploader::Environment VideoUploader::fleetEnvironment(const std::function<bool()> &uploadEnabled)
{
    Environment env;
    env.canUpload = [uploadEnabled]() {
        if (!uploadEnabled || !uploadEnabled() || !FleetStore::enrolled()) {
            return false;
        }
        FleetConnector *fc = FleetConnector::instance();
        if (!fc || fc->suspended()) {
            return false;               // the RotorLab Connector is the route: keep recordings local
        }
        switch (fc->state()) {
        case FleetConnector::FirstSync:
        case FleetConnector::Connected:
        case FleetConnector::Unreachable:
            return true;
        default:
            return false;
        }
    };
    env.baseUrl = []() { return FleetStore::baseUrl(); };
    env.stationId = []() { return FleetStore::stationId(); };
    env.token = []() { return FleetStore::token(); };
    env.pin = []() { return FleetStore::serverPin(); };
    return env;
}

QString VideoUploader::registerPath(const QString &flightRef)
{
    if (flightRef.isEmpty()) {
        return QStringLiteral("/api/gc/v1/stations/self/videos");
    }
    return QStringLiteral("/api/gc/v1/flights/%1/videos")
        .arg(QString::fromLatin1(QUrl::toPercentEncoding(flightRef)));
}

QString VideoUploader::uploadPath(const QString &uploadId)
{
    return QStringLiteral("/api/gc/v1/uploads/%1").arg(QString::fromLatin1(QUrl::toPercentEncoding(uploadId)));
}

QString VideoUploader::receiptPath(const QString &filePath)
{
    return filePath + QStringLiteral(".records-receipt.json");
}

bool VideoUploader::_kindFinished(const QJsonObject &e, const QString &prefix)
{
    return e.value(prefix + QStringLiteral("records_state")).toString() == QLatin1String(kHandedOver)
           && !e.value(prefix + QStringLiteral("receipt")).toObject().isEmpty();
}

bool VideoUploader::wantsRecordsStatus(const QJsonObject &e)
{
    if (e.value(QStringLiteral("state")).toString() != QLatin1String("done")
        || str(e, QStringLiteral("video_upload_id")).isEmpty()
        || e.value(QStringLiteral("records_poll_stopped")).toBool()) {
        return false;
    }
    const bool sidecarFinished = str(e, QStringLiteral("sidecar_upload_id")).isEmpty()
                                 || _kindFinished(e, QStringLiteral("sidecar_"));
    return !(_kindFinished(e, QStringLiteral("video_")) && sidecarFinished);
}

QString VideoUploader::statusText() const
{
    if (_revoked) {
        return tr("Uploads stopped: this ground station was removed from the Fleet Server. Recordings stay on this computer.");
    }
    return QString();
}

QString VideoUploader::_k(Kind kind, const char *field) const
{
    return (kind == Kind::Video ? QStringLiteral("video_") : QStringLiteral("sidecar_")) + QLatin1String(field);
}

QString VideoUploader::_path(Kind kind) const
{
    return _entry.value(kind == Kind::Video ? QStringLiteral("path") : QStringLiteral("sidecar_path")).toString();
}

qint64 VideoUploader::_size(Kind kind) const
{
    return _entry.value(kind == Kind::Video ? QStringLiteral("size") : QStringLiteral("sidecar_size")).toInteger();
}

QString VideoUploader::_sha(Kind kind) const
{
    return _entry.value(kind == Kind::Video ? QStringLiteral("sha256") : QStringLiteral("sidecar_sha256")).toString();
}

void VideoUploader::_save()
{
    if (!_entry.isEmpty()) {
        _store->upsert(_entry);
    }
}

void VideoUploader::_configureClient()
{
    // Read the credential now, not at startup: it may have changed.
    _client->setBaseUrl(_entry.value(QStringLiteral("base_url")).toString());
    _client->setPinnedSpkiSha256(_env.pin ? _env.pin() : QString());
    _client->setToken(_env.token ? _env.token() : QString());
}

// ------------------------------------------------------------- the queue

void VideoUploader::kick()
{
    QTimer::singleShot(0, this, &VideoUploader::_sweep);
}

void VideoUploader::stopAll()
{
    ++_session;
    if (_client) {
        _client->abortAll();         // stop what is on the wire, drop its answer
    }
    if (_reply) {
        _reply->abort();
        _reply.clear();
    }
    if (_busy && !_entry.isEmpty()) {
        const QString state = _entry.value(QStringLiteral("state")).toString();
        if (state != QLatin1String("done") && state != QLatin1String("parked")) {
            _entry[QStringLiteral("state")] = QStringLiteral("waiting");
        }
        _save();
    }
    _busy = false;
    _currentId.clear();
    _entry = QJsonObject();
    emit changed();
}

void VideoUploader::resume()
{
    if (_revoked) {
        _revoked = false;
        emit changed();
    }
    kick();
}

void VideoUploader::retry(const QString &id)
{
    QJsonObject e = _store->entry(id);
    if (e.isEmpty() || e.value(QStringLiteral("state")).toString() == QLatin1String("done")) {
        return;
    }
    e[QStringLiteral("state")] = QStringLiteral("waiting");
    e[QStringLiteral("detail")] = QString();
    e[QStringLiteral("attempts")] = 0;
    e[QStringLiteral("backoff_index")] = 0;
    e[QStringLiteral("next_try_ms")] = 0;
    e[QStringLiteral("video_restarted")] = false;
    e[QStringLiteral("sidecar_restarted")] = false;
    e.remove(QStringLiteral("changed"));
    e.remove(QStringLiteral("early_close_at"));
    e.remove(QStringLiteral("early_closes"));
    _store->upsert(e);
    emit changed();
    kick();
}

void VideoUploader::sendToThisServer(const QString &id)
{
    QJsonObject e = _store->entry(id);
    if (e.isEmpty() || !_env.baseUrl || _env.baseUrl().isEmpty()) {
        return;
    }
    // A new server knows nothing of earlier upload ids.
    e[QStringLiteral("base_url")] = _env.baseUrl();
    e[QStringLiteral("station_id")] = _env.stationId ? _env.stationId() : 0;
    for (const char *f : {"video_upload_id", "sidecar_upload_id"}) {
        e.remove(QLatin1String(f));
    }
    for (const char *f : {"video_offset", "sidecar_offset"}) {
        e[QLatin1String(f)] = 0;
    }
    e[QStringLiteral("video_done")] = false;
    e[QStringLiteral("sidecar_done")] = false;
    _store->upsert(e);
    qCInfo(VideoRecordLog) << "recording" << e.value(QStringLiteral("name")).toString()
                           << "rebound to the enrolled server by the operator";
    retry(id);
}

void VideoUploader::checkRecords(const QString &id)
{
    QJsonObject e = _store->entry(id);
    if (e.isEmpty() || e.value(QStringLiteral("state")).toString() != QLatin1String("done")) {
        return;
    }
    e[QStringLiteral("records_poll_stopped")] = false;
    e[QStringLiteral("records_polls")] = 0;
    e[QStringLiteral("records_backoff_index")] = 0;
    e[QStringLiteral("records_next_ms")] = 0;
    _store->upsert(e);
    emit changed();
    kick();
}

bool VideoUploader::_eligible(QJsonObject &e, bool *bindingChanged)
{
    *bindingChanged = false;
    const QString state = e.value(QStringLiteral("state")).toString();
    if (state == QLatin1String("done") || state == QLatin1String("parked")) {
        return false;
    }
    const QString base = e.value(QStringLiteral("base_url")).toString();
    const int station = e.value(QStringLiteral("station_id")).toInt();
    QString newState;
    QString detail;
    if (base.isEmpty() || station <= 0) {
        newState = QStringLiteral("unbound");
        detail = tr("Recorded while this station was not enrolled with a Fleet Server; this recording stays on this computer.");
    } else if (base != _env.baseUrl() || station != _env.stationId()) {
        newState = QStringLiteral("wrong_server");
        detail = tr("Enrolled to a different server; this recording stays on this computer.");
    } else if (state == QLatin1String("unbound") || state == QLatin1String("wrong_server") || state.isEmpty()) {
        newState = QStringLiteral("waiting");
    }
    if (!newState.isEmpty() && newState != state) {
        e[QStringLiteral("state")] = newState;
        e[QStringLiteral("detail")] = detail;
        *bindingChanged = true;
    }
    return e.value(QStringLiteral("state")).toString() == QLatin1String("waiting")
           || e.value(QStringLiteral("state")).toString() == QLatin1String("registering")
           || e.value(QStringLiteral("state")).toString() == QLatin1String("uploading")
           || e.value(QStringLiteral("state")).toString() == QLatin1String("checking");
}

void VideoUploader::_sweep()
{
    if (_busy || !_client || _revoked) {
        return;
    }
    if (!_env.canUpload || !_env.canUpload()) {
        return;                         // recordings stay queued; nothing is sent
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    qint64 nextWake = -1;
    bool anyChanged = false;
    QJsonObject statusDue;              ///< a stored recording whose status check is due
    for (QJsonObject e : _store->entries()) {
        if (wantsRecordsStatus(e)) {
            // Only the server and station it was stored on know it.
            if (e.value(QStringLiteral("base_url")).toString() == _env.baseUrl()
                && e.value(QStringLiteral("station_id")).toInt() == _env.stationId()) {
                const qint64 due = e.value(QStringLiteral("records_next_ms")).toInteger();
                if (due > now) {
                    nextWake = (nextWake < 0) ? due : qMin(nextWake, due);
                } else if (statusDue.isEmpty()) {
                    statusDue = e;
                }
            }
            continue;
        }
        bool changedBinding = false;
        const bool ok = _eligible(e, &changedBinding);
        if (changedBinding) {
            _store->upsert(e);
            anyChanged = true;
        }
        if (!ok) {
            continue;
        }
        const qint64 next = e.value(QStringLiteral("next_try_ms")).toInteger();
        if (next > now) {
            nextWake = (nextWake < 0) ? next : qMin(nextWake, next);
            continue;
        }
        _entry = e;
        _currentId = e.value(QStringLiteral("id")).toString();
        _busy = true;
        _verified = false;
        _needHead = true;
        _consecutive400 = 0;
        _answered = false;
        _configureClient();
        emit changed();
        _step();
        return;
    }
    if (!statusDue.isEmpty()) {
        // Uploads first; a status check only when no part is waiting to go.
        _entry = statusDue;
        _currentId = statusDue.value(QStringLiteral("id")).toString();
        _busy = true;
        _configureClient();
        emit changed();
        _startRecordsPolling();
        return;
    }
    if (nextWake > 0) {
        _wake.start(int(qBound(qint64(0), nextWake - now, qint64(3600 * 1000))));
    }
    if (anyChanged) {
        emit changed();
    }
}

void VideoUploader::_finish()
{
    _save();
    _busy = false;
    _currentId.clear();
    _entry = QJsonObject();
    _reply.clear();
    emit changed();
    QTimer::singleShot(0, this, &VideoUploader::_sweep);
}

void VideoUploader::_park(const QString &why, bool retryable)
{
    qCWarning(VideoRecordLog) << "recording upload parked:" << _entry.value(QStringLiteral("name")).toString() << why;
    _entry[QStringLiteral("state")] = QStringLiteral("parked");
    _entry[QStringLiteral("detail")] = why;
    _entry[QStringLiteral("retryable")] = retryable;
    _finish();
}

void VideoUploader::_backoff(const QString &why, int retryAfter)
{
    const int attempts = _entry.value(QStringLiteral("attempts")).toInt() + 1;
    _entry[QStringLiteral("attempts")] = attempts;
    if (attempts >= kMaxAttempts) {
        _park(tr("Could not upload after %1 tries (%2). Try again later.").arg(kMaxAttempts).arg(why), true);
        return;
    }
    int seconds = 0;
    if (retryAfter >= 0) {
        // The server said when to come back (Server 2.0: Retry-After: 30 on
        // 503). Honor it as given, within 1 s to 1 h, and leave our own
        // schedule where it was.
        seconds = std::clamp(retryAfter, 1, 3600);
    } else {
        const int idx = std::clamp(_entry.value(QStringLiteral("backoff_index")).toInt(), 0,
                                   int(std::size(kBackoffSeconds)) - 1);
        seconds = kBackoffSeconds[idx];
        _entry[QStringLiteral("backoff_index")] = idx + 1;
    }
    _entry[QStringLiteral("next_try_ms")] = QDateTime::currentMSecsSinceEpoch() + qint64(seconds) * _unitMs;
    _entry[QStringLiteral("state")] = QStringLiteral("waiting");
    _entry[QStringLiteral("detail")] = tr("Waiting to try again in %1 s: %2").arg(seconds).arg(why);
    qCInfo(VideoRecordLog) << "recording upload waits" << seconds << "s:" << why;
    _finish();
}

bool VideoUploader::_common(const HttpReply &r)
{
    if (r.pinRefused) {
        _park(r.error, true);
        return false;
    }
    if (!r.netOk) {
        qCInfo(VideoRecordLog) << "no HTTP answer: QNetworkReply error" << r.netError << "body bytes sent" << r.bodySent;
        _backoff(r.error.isEmpty() ? tr("no answer from the Fleet Server") : r.error, -1);
        return false;
    }
    if (r.status == 401) {
        // Revoked: stop everything, keep everything.
        qCWarning(VideoRecordLog) << "recording uploads stopped: 401 from the Fleet Server";
        _revoked = true;
        _entry[QStringLiteral("state")] = QStringLiteral("waiting");
        _entry[QStringLiteral("detail")] = statusText();
        _finish();
        return false;
    }
    if (r.status == 413) {
        _park(tr("The Fleet Server refused this file as too large."), true);
        return false;
    }
    if (r.status >= 500) {
        _backoff(r.status == 503 ? tr("the Fleet Server cannot take the file yet")
                                 : tr("the Fleet Server returned an error (%1)").arg(r.status),
                 r.retryAfter);
        return false;
    }
    return true;
}

void VideoUploader::_step()
{
    if (str(_entry, _k(Kind::Video, "upload_id")).isEmpty()) {
        _register(Kind::Video);
    } else if (str(_entry, _k(Kind::Sidecar, "upload_id")).isEmpty()) {
        _register(Kind::Sidecar);
    } else if (!_entry.value(_k(Kind::Video, "done")).toBool()) {
        if (_needHead) {
            _head(Kind::Video);
        } else {
            _sendChunk(Kind::Video);
        }
    } else if (!_entry.value(_k(Kind::Sidecar, "done")).toBool()) {
        if (_needHead) {
            _head(Kind::Sidecar);
        } else {
            _sendChunk(Kind::Sidecar);
        }
    } else {
        _entry[QStringLiteral("state")] = QStringLiteral("done");
        _entry[QStringLiteral("detail")] = tr("Stored on the Fleet Server");
        _entry[QStringLiteral("progress")] = 1.0;
        _entry[QStringLiteral("stored_at")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        // Then ask now and then where it stands: attached to a flight, and
        // received by Records (GET /uploads/<id>).
        _entry[QStringLiteral("records_poll_stopped")] = false;
        _entry[QStringLiteral("records_polls")] = 0;
        _entry[QStringLiteral("records_backoff_index")] = 1;
        _entry[QStringLiteral("records_next_ms")] =
            QDateTime::currentMSecsSinceEpoch() + qint64(kRecordsSeconds[0]) * _unitMs;
        qCInfo(VideoRecordLog) << "recording stored on the Fleet Server:" << _entry.value(QStringLiteral("name")).toString()
                               << "sha256" << _sha(Kind::Video);
        _recordsDetail();
        _finish();
    }
}

void VideoUploader::_register(Kind kind)
{
    QJsonObject body;
    body[QStringLiteral("camera")] = _entry.value(QStringLiteral("camera"));
    body[QStringLiteral("started_at")] = _entry.value(QStringLiteral("started_at"));
    body[QStringLiteral("ended_at")] = _entry.value(QStringLiteral("ended_at"));
    body[QStringLiteral("part")] = _entry.value(QStringLiteral("part"));
    body[QStringLiteral("recovered")] = _entry.value(QStringLiteral("recovered")).toBool();
    if (kind == Kind::Video) {
        body[QStringLiteral("name")] = _entry.value(QStringLiteral("name"));
        body[QStringLiteral("size")] = _size(kind);
        body[QStringLiteral("sha256")] = _sha(kind);
        body[QStringLiteral("codec")] = _entry.value(QStringLiteral("codec"));
        body[QStringLiteral("width")] = _entry.value(QStringLiteral("width"));
        body[QStringLiteral("height")] = _entry.value(QStringLiteral("height"));
        // Fleet takes 1 to 240, or null for unknown. A part too short
        // to measure has no rate of its own: null, never a nominal 30. The
        // time map keeps the real per-fragment times and fps_measured: false.
        const double fps = _entry.value(QStringLiteral("fps")).toDouble();
        body[QStringLiteral("fps")] = TimeMapWriter::fpsKnown(fps) ? QJsonValue(fps) : QJsonValue(QJsonValue::Null);
    } else {
        body[QStringLiteral("name")] = _entry.value(QStringLiteral("sidecar_name"));
        body[QStringLiteral("size")] = _size(kind);
        body[QStringLiteral("sha256")] = _sha(kind);
        body[QStringLiteral("kind")] = QStringLiteral("video-timemap");
        body[QStringLiteral("part_of")] = _entry.value(QStringLiteral("name"));
    }
    _entry[QStringLiteral("state")] = QStringLiteral("registering");
    _entry[QStringLiteral("detail")] = QString();
    _save();
    emit changed();

    const quint64 session = _session;
    const bool stationRoute = _entry.value(QStringLiteral("station_route")).toBool();
    const QString path = registerPath(stationRoute ? QString() : _entry.value(QStringLiteral("flight_ref")).toString());
    qCInfo(VideoRecordLog) << "registering" << body.value(QStringLiteral("name")).toString()
                           << "sha256" << body.value(QStringLiteral("sha256")).toString();
    _client->post(path, body, [this, session, kind](const HttpReply &r) {
        if (session != _session) {
            return;
        }
        _answered |= r.netOk;
        if (r.netOk && r.status == 409) {
            if (r.error.contains(QStringLiteral("licen"), Qt::CaseInsensitive)) {
                _backoff(r.error, r.retryAfter);
            } else {
                _park(r.error.isEmpty()
                      ? tr("The Fleet Server already has a different recording with this name.") : r.error,
                      true);
            }
            return;
        }
        if (r.netOk && r.status == 404 && !_entry.value(QStringLiteral("flight_ref")).toString().isEmpty()
            && !_entry.value(QStringLiteral("station_route")).toBool()) {
            // Fleet has no report naming that flight: register by station and
            // start time instead (Fleet attaches it to the flight it overlaps).
            qCInfo(VideoRecordLog) << "flight not known to Fleet; registering by station instead";
            _entry[QStringLiteral("station_route")] = true;
            _save();
            _register(kind);
            return;
        }
        if (!_common(r)) {
            return;
        }
        if (r.status != 200) {
            _park(r.error.isEmpty() ? tr("The Fleet Server refused this recording (%1).").arg(r.status) : r.error, true);
            return;
        }
        const QString id = str(r.body, QStringLiteral("upload_id"));
        if (id.isEmpty()) {
            _backoff(tr("the Fleet Server's answer had no upload id"), -1);
            return;
        }
        _entry[_k(kind, "upload_id")] = id;
        _entry[_k(kind, "offset")] = qBound(qint64(0), r.body.value(QStringLiteral("offset")).toInteger(), _size(kind));
        _needHead = false;
        _save();
        _step();
    });
}

void VideoUploader::_head(Kind kind)
{
    const quint64 session = _session;
    _client->head(uploadPath(str(_entry, _k(kind, "upload_id"))), [this, session, kind](const HttpReply &r) {
        if (session != _session) {
            return;
        }
        _answered |= r.netOk;
        if (r.netOk && r.status == 404) {
            // The server no longer knows this upload: register again (same name and hash).
            _entry.remove(_k(kind, "upload_id"));
            _entry[_k(kind, "offset")] = 0;
            _needHead = false;
            _step();
            return;
        }
        if (!_common(r)) {
            return;
        }
        bool ok = false;
        const qint64 off = r.header("Upload-Offset").trimmed().toLongLong(&ok);
        if (!ok || off < 0) {
            _backoff(tr("the Fleet Server did not say how much it has"), r.retryAfter);
            return;
        }
        _entry[_k(kind, "offset")] = qMin(off, _size(kind));
        _needHead = false;
        _save();
        if (off > 0 && !_verified) {
            _verifyThen(kind, [this, kind]() { _sendChunk(kind); });
        } else {
            _sendChunk(kind);
        }
    });
}

void VideoUploader::_verifyThen(Kind kind, std::function<void()> next)
{
    if (_verified) {
        next();
        return;
    }
    _entry[QStringLiteral("state")] = QStringLiteral("checking");
    _entry[QStringLiteral("detail")] = tr("Checking the file on this computer");
    emit changed();

    const quint64 session = _session;
    const QString path = _path(kind);
    const QString expected = _sha(kind);
    auto result = std::make_shared<QByteArray>();
    QThread *thread = QThread::create([path, result]() { *result = RecordingRecovery::hashFile(path); });
    connect(thread, &QThread::finished, this, [this, thread, session, result, expected, next = std::move(next)]() {
        thread->deleteLater();
        if (session != _session) {
            return;
        }
        if (QString::fromLatin1(*result) != expected) {
            _entry[QStringLiteral("changed")] = true;
            _park(tr("The file changed on this computer after capture. It is not sent as the same recording."), false);
            return;
        }
        _verified = true;
        next();
    });
    thread->start();
}

void VideoUploader::_sendChunk(Kind kind)
{
    const qint64 size = _size(kind);
    qint64 offset = _entry.value(_k(kind, "offset")).toInteger();
    if (size <= 0) {
        _park(tr("The recording file is empty."), false);
        return;
    }
    if (offset >= size) {
        // The server has every byte but its final answer never reached us. A
        // stored upload answers any PUT with its sha256 and size, so send the
        // last chunk again to hear it.
        offset = qMax(qint64(0), size - _chunk);
        _entry[_k(kind, "offset")] = offset;
    }
    const qint64 length = qMin(_chunk, size - offset);
    const QString path = _path(kind);
    if (QFileInfo(path).size() != size) {
        _entry[QStringLiteral("changed")] = true;
        _park(tr("The file changed on this computer after capture. It is not sent as the same recording."), false);
        return;
    }
    const QByteArray sha = rangeSha(path, offset, length);
    if (sha.isEmpty()) {
        _park(tr("The recording file cannot be read on this computer."), true);
        return;
    }

    const qint64 total = _size(Kind::Video) + _size(Kind::Sidecar);
    const qint64 before = (kind == Kind::Video) ? offset : _size(Kind::Video) + offset;
    _entry[QStringLiteral("state")] = QStringLiteral("uploading");
    _entry[QStringLiteral("detail")] = QString();
    _entry[QStringLiteral("progress")] = total > 0 ? double(before) / double(total) : 0.0;
    emit changed();

    const quint64 session = _session;
    _reply = _client->putRange(uploadPath(str(_entry, _k(kind, "upload_id"))), path, offset, length, sha, size,
        [this, session, kind, offset, length](const HttpReply &r) {
            if (session != _session) {
                return;
            }
            _reply.clear();
            _onChunkReply(kind, offset, length, r);
        },
        [this, session, before, total](qint64 sent, qint64) {
            if (session == _session && total > 0 && _busy) {
                _entry[QStringLiteral("progress")] = double(before + sent) / double(total);
                emit changed();
            }
        });
}

void VideoUploader::_onChunkReply(Kind kind, qint64 offset, qint64 length, const HttpReply &r)
{
    const qint64 size = _size(kind);
    _answered |= r.netOk;
    if (r.netOk) {
        switch (r.status) {
        case 204: {
            bool ok = false;
            qint64 next = r.header("Upload-Offset").trimmed().toLongLong(&ok);
            if (!ok) {
                next = offset + length;
            }
            _entry[_k(kind, "offset")] = qBound(qint64(0), next, size);
            _chunkTaken();
            _save();
            _sendChunk(kind);
            return;
        }
        case 200:
            if (r.body.contains(QStringLiteral("sha256"))) {
                _done(kind, r);         // the last chunk: the server's whole-file check
                return;
            } else {
                // A chunk taken (Server 2.0 answers 200 {"offset": n}, plus Upload-Offset).
                bool ok = false;
                qint64 next = r.header("Upload-Offset").trimmed().toLongLong(&ok);
                if (!ok && r.body.contains(QStringLiteral("offset"))) {
                    next = r.body.value(QStringLiteral("offset")).toInteger(-1);
                    ok = next >= 0;
                }
                if (!ok) {
                    next = offset + length;
                }
                _entry[_k(kind, "offset")] = qBound(qint64(0), next, size);
                _chunkTaken();
                _save();
                _sendChunk(kind);
                return;
            }
        case 400: {
            _consecutive400 += 1;
            qCInfo(VideoRecordLog) << "chunk refused (400):" << offset << length << r.error;
            bool restartOk = false;
            const qint64 restartAt = r.header("Upload-Offset").trimmed().toLongLong(&restartOk);
            if (offset + length == size && restartOk && restartAt == 0) {
                // The last chunk, and the whole file did not match: the server is back at 0.
                _restartOrPark(kind, tr("The Fleet Server's copy did not match this computer's file."));
                return;
            }
            if (_consecutive400 >= 3) {
                _verified = false;
                _entry[QStringLiteral("attempts")] = _entry.value(QStringLiteral("attempts")).toInt() + 1;
                if (_entry.value(QStringLiteral("attempts")).toInt() >= kMaxAttempts) {
                    _park(tr("The Fleet Server kept refusing a part of this file."), true);
                    return;
                }
                _verifyThen(kind, [this, kind]() { _consecutive400 = 0; _sendChunk(kind); });
                return;
            }
            _sendChunk(kind);
            return;
        }
        case 413:
            // The server's body-size cap is below our chunk: guidance, then
            // park (the brief: 413 parks, with "Try again"). The file stays.
            qCWarning(VideoRecordLog) << "chunk refused as too large (413):" << offset << length << r.error;
            _park(_chunkTooLargeText(), true);
            return;
        case 404:
            _entry.remove(_k(kind, "upload_id"));
            _entry[_k(kind, "offset")] = 0;
            _needHead = false;
            _save();
            _step();
            return;
        case 409: {
            bool ok = false;
            const qint64 off = r.header("Upload-Offset").trimmed().toLongLong(&ok);
            if (ok && off >= 0 && off <= size) {
                _entry[_k(kind, "offset")] = off;
                _save();
                if (off > 0 && !_verified) {
                    _verifyThen(kind, [this, kind]() { _sendChunk(kind); });
                } else {
                    _sendChunk(kind);
                }
            } else {
                _needHead = true;
                _step();
            }
            return;
        }
        default:
            break;
        }
    } else if (_closedEarly(r)) {
        _onEarlyClose(kind, offset, r);
        return;
    }
    if (!_common(r)) {
        return;
    }
    _park(r.error.isEmpty() ? tr("The Fleet Server refused this upload (%1).").arg(r.status) : r.error, true);
}

bool VideoUploader::_closedEarly(const HttpReply &r) const
{
    // A chunk that ended with no HTTP status after the request went out:
    // what a server does when its body-size cap is below the chunk size
    // (S20/S21 end-to-end run: "Connection closed", "Unable to write",
    // "Unknown error" on Windows; on Linux "Unknown error" before Qt counted
    // any upload progress). Any error code counts, on every platform, once
    // part of the body was sent, or once the server answered earlier in this
    // pass (each pass starts with a HEAD or a register, so a laptop that is
    // offline fails there first). A link that never came up is not this.
    if (r.netOk || r.pinRefused || r.netError == int(QNetworkReply::NoError)) {
        return false;
    }
    switch (QNetworkReply::NetworkError(r.netError)) {
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::TimeoutError:
    case QNetworkReply::OperationCanceledError:     // our idle timeout, or an abort
    case QNetworkReply::SslHandshakeFailedError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::BackgroundRequestNotAllowedError:
    case QNetworkReply::ProxyConnectionRefusedError:
    case QNetworkReply::ProxyNotFoundError:
    case QNetworkReply::ProxyTimeoutError:
    case QNetworkReply::ProxyAuthenticationRequiredError:
        return false;                               // never connected, or a stall
    default:
        break;
    }
    return r.bodySent > 0 || _answered;
}

void VideoUploader::_chunkTaken()
{
    _entry[QStringLiteral("attempts")] = 0;
    _entry[QStringLiteral("backoff_index")] = 0;
    _entry.remove(QStringLiteral("early_close_at"));
    _entry.remove(QStringLiteral("early_closes"));
    _consecutive400 = 0;
}

QString VideoUploader::_chunkTooLargeText() const
{
    const qint64 mb = (_chunk + 1024 * 1024 - 1) / (1024 * 1024);
    return tr("The Fleet Server refused this upload chunk as too large. Ask its administrator to accept "
              "uploads of at least %1 MB per request. The recording stays on this computer.").arg(mb);
}

void VideoUploader::_onEarlyClose(Kind kind, qint64 offset, const HttpReply &r)
{
    // Count closes on this chunk only; any chunk taken clears the count.
    const QString at = _k(kind, "") + QString::number(offset);
    int closes = 1;
    if (_entry.value(QStringLiteral("early_close_at")).toString() == at) {
        closes = _entry.value(QStringLiteral("early_closes")).toInt() + 1;
    }
    _entry[QStringLiteral("early_close_at")] = at;
    _entry[QStringLiteral("early_closes")] = closes;
    qCInfo(VideoRecordLog) << "the Fleet Server closed the connection during chunk" << at
                           << "before answering:" << r.error << "QNetworkReply error" << r.netError
                           << "body bytes sent" << r.bodySent << "-" << closes << "of" << kMaxEarlyCloses;
    if (closes >= kMaxEarlyCloses) {
        _park(_chunkTooLargeText(), true);
        return;
    }
    _backoff(_chunkTooLargeText(), -1);
}

void VideoUploader::_done(Kind kind, const HttpReply &r)
{
    const QString sha = r.body.value(QStringLiteral("sha256")).toString().toLower();
    const qint64 size = r.body.value(QStringLiteral("size")).toInteger(-1);
    if (sha == _sha(kind) && size == _size(kind)) {
        _entry[_k(kind, "done")] = true;
        _entry[_k(kind, "offset")] = _size(kind);
        _entry[_k(kind, "server_sha256")] = sha;
        _entry[QStringLiteral("attempts")] = 0;
        _entry[QStringLiteral("backoff_index")] = 0;
        qCInfo(VideoRecordLog) << "Fleet Server confirmed" << (kind == Kind::Video ? "video" : "time map")
                               << _entry.value(QStringLiteral("name")).toString() << "sha256" << sha << "size" << size;
        _save();
        _step();
        return;
    }
    _restartOrPark(kind, tr("The Fleet Server's copy did not match this computer's file."));
}

void VideoUploader::_restartOrPark(Kind kind, const QString &why)
{
    _verified = false;
    _verifyThen(kind, [this, kind, why]() {
        if (_entry.value(_k(kind, "restarted")).toBool()) {
            _park(why + QLatin1Char(' ') + tr("It did not match after a second upload either."), true);
            return;
        }
        qCWarning(VideoRecordLog) << "upload mismatch; sending again from the start:" << why;
        _entry[_k(kind, "restarted")] = true;
        _entry[_k(kind, "offset")] = 0;
        _consecutive400 = 0;
        _save();
        _sendChunk(kind);
    });
}

// ------------------------------------------------- after the final 200 (S21)

void VideoUploader::_startRecordsPolling()
{
    _pollRecords(_kindFinished(_entry, QStringLiteral("video_")) ? Kind::Sidecar : Kind::Video);
}

void VideoUploader::_pollRecords(Kind kind)
{
    const QString id = str(_entry, _k(kind, "upload_id"));
    if (id.isEmpty()) {
        _pollLater(-1);
        return;
    }
    const quint64 session = _session;
    _client->get(uploadPath(id), [this, session, kind](const HttpReply &r) {
        if (session != _session) {
            return;
        }
        _onRecordsReply(kind, r);
    });
}

void VideoUploader::_onRecordsReply(Kind kind, const HttpReply &r)
{
    if (r.netOk && r.status == 401) {
        // Revoked: stop everything, as for an upload. The recording stays done.
        qCWarning(VideoRecordLog) << "recording status checks stopped: 401 from the Fleet Server";
        _revoked = true;
        _finish();
        return;
    }
    if (r.netOk && r.status == 404) {
        _entry[QStringLiteral("records_poll_stopped")] = true;
        _entry[QStringLiteral("records_gone")] = true;
        _recordsDetail();
        _finish();
        return;
    }
    if (!r.netOk || r.status != 200) {
        qCInfo(VideoRecordLog) << "recording status check did not answer:" << r.status << r.error;
        _pollLater(r.retryAfter);
        return;
    }
    _entry.remove(QStringLiteral("records_gone"));
    const QString state = r.body.value(QStringLiteral("state")).toString();
    if (!state.isEmpty()) {
        _entry[_k(kind, "records_state")] = state;
    }
    const QJsonValue ref = r.body.value(QStringLiteral("flight_ref"));
    if (kind == Kind::Video && ref.isString() && !ref.toString().isEmpty()) {
        _entry[QStringLiteral("records_flight_ref")] = ref.toString();
    }
    const QJsonObject receipt = r.body.value(QStringLiteral("records_receipt")).toObject();
    if (!receipt.value(QStringLiteral("statement")).toString().isEmpty()
        && !receipt.value(QStringLiteral("signature")).toString().isEmpty()) {
        _keepReceipt(kind, receipt);
    }
    if (kind == Kind::Video && !str(_entry, _k(Kind::Sidecar, "upload_id")).isEmpty()
        && !_kindFinished(_entry, QStringLiteral("sidecar_"))) {
        _pollRecords(Kind::Sidecar);
        return;
    }
    _recordsDetail();
    if (!wantsRecordsStatus(_entry)) {
        qCInfo(VideoRecordLog) << "recording received by Records:" << _entry.value(QStringLiteral("name")).toString()
                               << "at" << _entry.value(QStringLiteral("video_receipt_at")).toString();
        _finish();
        return;
    }
    _pollLater(-1);
}

void VideoUploader::_keepReceipt(Kind kind, const QJsonObject &receipt)
{
    if (!_entry.value(_k(kind, "receipt")).toObject().isEmpty()) {
        return;                         // the first receipt is kept as it came; never replaced
    }
    const QString path = receiptPath(_path(kind));
    QFile f(path);
    if (f.exists()) {
        qCWarning(VideoRecordLog) << "a Records receipt file is already next to the recording; left as it is:" << path;
    } else if (f.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        // As received: received_at, statement and signature, each string unchanged.
        f.write(QJsonDocument(receipt).toJson(QJsonDocument::Indented));
        f.close();
    } else {
        qCWarning(VideoRecordLog) << "could not keep the Records receipt next to the recording:" << f.errorString();
    }
    const QJsonObject statement =
        QJsonDocument::fromJson(receipt.value(QStringLiteral("statement")).toString().toUtf8()).object();
    _entry[_k(kind, "receipt")] = receipt;
    _entry[_k(kind, "receipt_path")] = path;
    _entry[_k(kind, "receipt_at")] = receipt.value(QStringLiteral("received_at")).toString();
    _entry[_k(kind, "receipt_sha256")] = statement.value(QStringLiteral("sha256")).toString().toLower();
    _save();
    qCInfo(VideoRecordLog) << "Records receipt kept for" << (kind == Kind::Video ? "video" : "time map")
                           << _entry.value(QStringLiteral("name")).toString()
                           << "sha256" << _entry.value(_k(kind, "receipt_sha256")).toString();
}

void VideoUploader::_pollLater(int retryAfter)
{
    const int polls = _entry.value(QStringLiteral("records_polls")).toInt() + 1;
    _entry[QStringLiteral("records_polls")] = polls;
    if (polls >= kRecordsMaxPolls) {
        _entry[QStringLiteral("records_poll_stopped")] = true;
        _recordsDetail();
        _finish();
        return;
    }
    int seconds = 0;
    if (retryAfter >= 0) {
        seconds = std::clamp(retryAfter, 1, 3600);
    } else {
        const int idx = std::clamp(_entry.value(QStringLiteral("records_backoff_index")).toInt(), 0,
                                   int(std::size(kRecordsSeconds)) - 1);
        seconds = kRecordsSeconds[idx];
        _entry[QStringLiteral("records_backoff_index")] = idx + 1;
    }
    _entry[QStringLiteral("records_next_ms")] = QDateTime::currentMSecsSinceEpoch() + qint64(seconds) * _unitMs;
    _recordsDetail();
    _finish();
}

void VideoUploader::_recordsDetail()
{
    // Guidance wording only: what the servers said, never a claim about what
    // the recording proves.
    QString text;
    if (_kindFinished(_entry, QStringLiteral("video_"))) {
        const QString at = _entry.value(QStringLiteral("video_receipt_at")).toString();
        const QString named = _entry.value(QStringLiteral("video_receipt_sha256")).toString();
        if (named == _sha(Kind::Video)) {
            text = tr("Received by Records at %1. The receipt names SHA-256 %2, the same as this computer's file.")
                       .arg(at, named);
        } else {
            text = tr("Received by Records at %1, but the receipt names SHA-256 %2, not this computer's %3. "
                      "Ask the Fleet Server's administrator to look at it.")
                       .arg(at, named.isEmpty() ? tr("(none)") : named, _sha(Kind::Video));
        }
        if (!str(_entry, QStringLiteral("sidecar_upload_id")).isEmpty()
            && !_kindFinished(_entry, QStringLiteral("sidecar_"))) {
            text += QLatin1Char(' ') + tr("The time map is not yet received by Records.");
        }
    } else {
        const QString ref = _entry.value(QStringLiteral("records_flight_ref")).toString();
        text = ref.isEmpty() ? tr("Stored on the Fleet Server.")
                             : tr("Stored on the Fleet Server, attached to flight %1.").arg(ref);
        if (_entry.value(QStringLiteral("records_gone")).toBool()) {
            text += QLatin1Char(' ') + tr("The Fleet Server no longer lists this upload.");
        } else if (_entry.value(QStringLiteral("records_poll_stopped")).toBool()) {
            text += QLatin1Char(' ') + tr("Not yet received by Records. Use Check again to ask the Fleet Server.");
        } else {
            text += QLatin1Char(' ') + tr("Not yet received by Records.");
        }
    }
    _entry[QStringLiteral("detail")] = text;
}