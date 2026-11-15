/****************************************************************************
 *
 * RotorLab GC Fleet Connector - the aircraft's own flight log.
 *
 ****************************************************************************/

#include "FleetLogs.h"
#include "FleetClient.h"
#include "FleetStore.h"
#include "net/LogCollector.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QVariantMap>
#include <QtNetwork/QNetworkReply>

#include "QGCLoggingCategory.h"

QGC_LOGGING_CATEGORY(FleetLogsLog, "gcs.custom.fleet.logs")

namespace {

/// How often to look for work. Slow on purpose: nothing here is urgent, the
/// server holds a flight on the wanted list for 24 hours, and a tight loop
/// scanning a log directory on a field laptop is a cost with no benefit.
constexpr int kSweepMs = 15000;

/// Give up pulling one log off the aircraft after this many tries. Not zero -
/// a radio drops - but not unlimited either, or a log that genuinely is not on
/// the card blocks the queue behind it for a day.
constexpr int kMaxCollectAttempts = 3;

/// How many times to re-send a log before parking it in front of the operator.
/// Generous, because the failure this exists for is a big file over a bad link
/// and each attempt may already have moved hundreds of megabytes - but not
/// unlimited, in case a refusal has been misread as retryable.
constexpr int kMaxUploadAttempts = 8;

QString stateName(int s)
{
    switch (s) {
    case 0: return QStringLiteral("waiting");
    case 1: return QStringLiteral("collecting");
    case 2: return QStringLiteral("ready");
    case 3: return QStringLiteral("uploading");
    case 4: return QStringLiteral("done");
    default: return QStringLiteral("parked");
    }
}

} // namespace

FleetLogs::FleetLogs(FleetClient *client, QObject *parent)
    : QObject(parent)
    , _client(client)
    , _timer(new QTimer(this))
{
    _load();

    _timer->setInterval(kSweepMs);
    connect(_timer, &QTimer::timeout, this, &FleetLogs::_sweep);
    _timer->start();

}

FleetLogs::~FleetLogs() = default;

// ----------------------------------------------------------------- the wants

void FleetLogs::queueFlight(const QJsonObject &flight)
{
    const QString ref = flight.value(QStringLiteral("flight_ref")).toString();
    if (ref.isEmpty() || _entry(ref)) {
        return;
    }
    Entry entry;
    entry.ref = ref;
    entry.callsign = flight.value(QStringLiteral("vehicle")).toString();
    entry.sysid = flight.value(QStringLiteral("sysid")).toInt();
    entry.uid = flight.value(QStringLiteral("uid")).toString();
    entry.started = flight.value(QStringLiteral("started")).toDouble();
    entry.ended = flight.value(QStringLiteral("ended")).toDouble();
    entry.durationS = flight.value(QStringLiteral("duration_s")).toDouble();
    _entries.append(entry);
    _save();
    emit modelChanged();
}

void FleetLogs::reset()
{
    if (_upload) {
        _upload->abort();
        _upload.clear();
    }
    if (_collecting) {
        LogCollector::instance()->cancel();
    }
    _collecting = false;
    _uploading = false;
    _currentRef.clear();
    _entries.clear();
    _save();
    emit modelChanged();
}

FleetLogs::Entry *FleetLogs::_entry(const QString &ref)
{
    for (Entry &e : _entries) {
        if (e.ref == ref) {
            return &e;
        }
    }
    return nullptr;
}

int FleetLogs::waitingCount() const
{
    int n = 0;
    for (const Entry &e : _entries) {
        if (e.state != Entry::State::Done) {
            ++n;
        }
    }
    return n;
}

QVariantList FleetLogs::model() const
{
    QVariantList out;
    for (const Entry &e : _entries) {
        QVariantMap m;
        m[QStringLiteral("ref")]       = e.ref;
        m[QStringLiteral("callsign")]  = e.callsign.isEmpty()
                                       ? QStringLiteral("sysid %1").arg(e.sysid) : e.callsign;
        m[QStringLiteral("when")]      = QDateTime::fromSecsSinceEpoch(qint64(e.started))
                                            .toString(QStringLiteral("d MMM  hh:mm"));
        m[QStringLiteral("duration")]  = (e.durationS >= 60)
            ? QStringLiteral("%1m %2s").arg(int(e.durationS) / 60).arg(int(e.durationS) % 60)
            : QStringLiteral("%1s").arg(int(e.durationS));
        m[QStringLiteral("state")]     = stateName(int(e.state));
        m[QStringLiteral("detail")]    = e.detail;
        m[QStringLiteral("progress")]  = e.progress;
        m[QStringLiteral("retryable")] = e.retryable;
        m[QStringLiteral("file")]      = e.name;
        out.append(m);
    }
    return out;
}

// ------------------------------------------------------------------- the work

void FleetLogs::_sweep()
{
    if (_collecting) {
        // LogCollector runs the watchdog now, for every consumer at once, and
        // always ends a collection with exactly one of collected()/failed().
        // The handlers in _startCollect() clear this flag on either.
        return;
    }
    if (_uploading || !_client) {
        return;
    }

    // Upload anything already in hand first: the file is the thing at risk, and
    // it is not safe until the server has it.
    for (Entry &e : _entries) {
        if (e.state == Entry::State::Ready && !e.path.isEmpty()) {
            _startUpload(e);
            return;
        }
    }

    // Then look for files we have not noticed yet.
    bool found = false;
    for (Entry &e : _entries) {
        if (e.state != Entry::State::Waiting) {
            continue;
        }
        const QString path = _findLocalLog(e);
        if (path.isEmpty()) {
            continue;
        }
        e.path  = path;
        e.name  = QFileInfo(path).fileName();
        e.state = Entry::State::Ready;
        e.detail.clear();
        found = true;
    }
    if (found) {
        _save();
        emit modelChanged();
        _sweep();               // one of them is Ready now; start it
        return;
    }

    // Nothing on disk. Ask the aircraft for one, if that is safe right now.
    if (!FleetStore::collectLogsAutomatically()) {
        return;
    }
    for (Entry &e : _entries) {
        if (e.state == Entry::State::Waiting && e.collectAttempts < kMaxCollectAttempts) {
            QString whyNot;
            if (!_canCollect(&whyNot)) {
                return;         // not now; the reason is transient by nature
            }
            _startCollect(e);
            return;
        }
    }
}

QString FleetLogs::_findLocalLog(const Entry &e) const
{
    // The matching rules live in LogCollector, shared with the RotorLab
    // Connector: the window anchored on the flight's END (a log's reported
    // time is its modification time), and the size-versus-duration guard that
    // stops a still-open multi-session log being attached to one sortie.
    const qint64 maxBytes = qint64(_maxMb) * 1024 * 1024;
    return LogCollector::findOnDisk(e.ended > 0 ? e.ended : e.started,
                                    e.durationS, maxBytes);
}

bool FleetLogs::_canCollect(QString *whyNot) const
{
    return LogCollector::instance()->canCollect(whyNot);
}

void FleetLogs::_startCollect(Entry &e)
{
    LogCollector *const lc = LogCollector::instance();

    ++e.collectAttempts;
    e.state    = Entry::State::Collecting;
    e.progress = 0.0;
    e.detail   = QStringLiteral("asking the aircraft for its log");
    _collecting = true;
    _currentRef = e.ref;
    emit modelChanged();
    emit logMessage(QStringLiteral("collecting the log for %1").arg(e.callsign));

    const QString ref = e.ref;

    // One shot. Both handlers are torn down as soon as either fires, so a
    // later collection for a different flight cannot deliver into this one.
    auto conns = std::make_shared<QList<QMetaObject::Connection>>();
    auto done  = [conns]() {
        for (const QMetaObject::Connection &c : *conns) {
            QObject::disconnect(c);
        }
        conns->clear();
    };

    conns->append(connect(lc, &LogCollector::collected, this,
                          [this, ref, done](const QString &path) {
        done();
        _collecting = false;
        _currentRef.clear();
        Entry *it = _entry(ref);
        if (!it) {
            return;
        }
        it->path  = path;
        it->name  = QFileInfo(path).fileName();
        it->state = Entry::State::Ready;
        it->detail.clear();
        emit logMessage(QStringLiteral("collected %1").arg(it->name));
        _save();
        emit modelChanged();
        _sweep();
    }));

    conns->append(connect(lc, &LogCollector::failed, this,
                          [this, ref, done](const QString &why, bool retryable) {
        done();
        _collecting = false;
        _currentRef.clear();
        Entry *it = _entry(ref);
        if (!it) {
            return;
        }
        if (!retryable) {
            _park(*it, why, false, /*onSize*/ true);
            return;
        }
        // A radio drops. Not parked - the next sweep tries again until the
        // attempt limit, and after that the crew attaches the file by hand.
        it->state    = Entry::State::Waiting;
        it->progress = 0.0;
        it->detail   = (it->collectAttempts >= kMaxCollectAttempts)
            ? QStringLiteral("could not download it from the aircraft: attach it by hand")
            : why;
        _save();
        emit modelChanged();
    }));

    lc->collect(e.sysid, e.ended > 0 ? e.ended : e.started, e.durationS,
                qint64(_maxMb) * 1024 * 1024);
}

void FleetLogs::_startUpload(Entry &e)
{
    if (!_client || e.path.isEmpty()) {
        return;
    }
    const QFileInfo fi(e.path);
    if (!fi.exists() || fi.size() <= 0) {
        // The file moved or was cleaned up under us.
        e.state = Entry::State::Waiting;
        e.path.clear();
        e.detail = QStringLiteral("the file is no longer there");
        _save();
        emit modelChanged();
        return;
    }
    if (fi.size() > qint64(_maxMb) * 1024 * 1024) {
        _park(e, QStringLiteral("that log is larger than the %1 MB this server takes").arg(_maxMb),
              false, /*onSize*/ true);
        return;
    }

    _uploading  = true;
    _currentRef = e.ref;
    e.state     = Entry::State::Uploading;
    e.progress  = 0.0;
    e.detail    = QStringLiteral("uploading");
    ++e.uploadAttempts;
    emit modelChanged();

    const QString ref  = e.ref;
    const QString name = e.name;
    const QString suffix = fi.suffix().toLower();
    if (suffix != QLatin1String("bin") && suffix != QLatin1String("tlog")
        && suffix != QLatin1String("ulg")) {
        _park(e, QStringLiteral("only .bin, .tlog and .ulg logs are supported"), false);
        _uploading = false;
        _currentRef.clear();
        return;
    }
    QFile file(e.path);
    if (!file.open(QIODevice::ReadOnly)) {
        _uploading = false;
        _currentRef.clear();
        _park(e, QStringLiteral("cannot read the log: %1").arg(file.errorString()), true);
        return;
    }
    QCryptographicHash digest(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(1024 * 1024);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
            _uploading = false;
            _currentRef.clear();
            _park(e, QStringLiteral("cannot read the log: %1").arg(file.errorString()), true);
            return;
        }
        digest.addData(chunk);
    }
    const QByteArray sha256 = digest.result().toHex();
    const qint64 fileSize = fi.size();
    const QString path = QStringLiteral("/api/gc/v1/flights/%1/log?name=%2")
        .arg(QString::fromUtf8(QUrl::toPercentEncoding(ref)),
             QString::fromUtf8(QUrl::toPercentEncoding(name)));

    _upload = _client->putFile(path, e.path, sha256,
        [this, ref, name, sha256, fileSize](const FleetReply &r) {
        _uploading = false;
        _currentRef.clear();
        Entry *e = _entry(ref);
        if (!e) {
            return;
        }
        e->progress = 0.0;

        if (r.status == 200
            && r.body.value(QStringLiteral("sha256")).toString().compare(
                   QString::fromLatin1(sha256), Qt::CaseInsensitive) == 0
            && r.body.value(QStringLiteral("size")).toVariant().toLongLong() == fileSize) {
            e->state  = Entry::State::Done;
            e->detail = QStringLiteral("stored");
            emit logMessage(QStringLiteral("%1 %2").arg(name, e->detail));
            _save();
            emit modelChanged();
            _sweep();
            return;
        }

        // The distinction that matters is whether retrying can ever work: a
        // retry costs nothing because the server dedupes on the body hash, but
        // a loop on a permanent refusal is just noise in front of the crew.
        switch (r.status) {
        case 400:
            _retryUpload(*e, r.error.isEmpty()
                                 ? QStringLiteral("the server did not verify the log; retrying")
                                 : r.error);
            break;
        case 404:
            _retryUpload(*e, QStringLiteral("flight report is not available on the server yet"));
            break;
        case 413:
            _park(*e, QStringLiteral("that log is larger than one file may be"), false,
                  /*onSize*/ true);
            break;
        case 503:
            // A transport failure or a 5xx - including the timeout of an upload
            // that stalled mid-body, which is the common one on a big file.
            _retryUpload(*e, r.error.isEmpty()
                             ? QStringLiteral("upload did not complete; will try again")
                             : r.error);
            break;
        default:
            _retryUpload(*e, r.error.isEmpty()
                             ? QStringLiteral("upload did not complete; will try again")
                             : r.error);
            break;
        }
        _save();
        emit modelChanged();
    },
        [this, ref](qint64 sent, qint64 total) {
        Entry *e = _entry(ref);
        if (e && total > 0) {
            e->progress = double(sent) / double(total);
            emit modelChanged();
        }
    });
}

void FleetLogs::_retryUpload(Entry &e, const QString &why)
{
    // Put it back in the queue. The server dedupes on the SHA-256 of the body,
    // so re-sending costs nothing anywhere even if an earlier attempt had
    // actually landed - which is exactly why a blind retry is the right answer
    // to a connection that dropped mid-body.
    if (e.uploadAttempts >= kMaxUploadAttempts) {
        _park(e, QStringLiteral("%1: gave up after %2 attempts")
                     .arg(why).arg(e.uploadAttempts), true);
        return;
    }
    e.state  = Entry::State::Ready;
    e.detail = why;
}

void FleetLogs::_park(Entry &e, const QString &why, bool retryable, bool onSize)
{
    e.state        = Entry::State::Parked;
    e.detail       = why;
    e.retryable    = retryable;
    e.parkedOnSize = onSize;
    e.progress     = 0.0;
    // Deliberately keep e.path. The file stays where it is and stays ours; the
    // operator can retry, and nothing here ever deletes a log.
    emit logMessage(QStringLiteral("%1: %2").arg(e.callsign, why));
    _save();
    emit modelChanged();
}

// -------------------------------------------------------- operator actions

void FleetLogs::attachFile(const QString &externalRef, const QString &filePath)
{
    Entry *e = _entry(externalRef);
    if (!e) {
        return;
    }
    QString local = filePath;
    if (local.startsWith(QStringLiteral("file:"))) {
        local = QUrl(local).toLocalFile();
    }
    const QFileInfo fi(local);
    if (!fi.exists() || fi.size() <= 0) {
        return;
    }
    e->path      = fi.absoluteFilePath();
    e->name      = fi.fileName();
    e->state     = Entry::State::Ready;
    e->detail.clear();
    e->retryable = false;
    _save();
    emit modelChanged();
    _sweep();
}

void FleetLogs::cancelCurrent()
{
    if (_uploading && _upload) {
        _upload->abort();           // the handler puts it back to Ready
    }
    if (_collecting) {
        // LogCollector deletes the partial file and answers with failed(),
        // which the handler in _startCollect() turns back into Waiting.
        LogCollector::instance()->cancel();
    }
}

void FleetLogs::forget(const QString &externalRef)
{
    for (int i = 0; i < _entries.size(); ++i) {
        if (_entries[i].ref == externalRef) {
            _entries.removeAt(i);
            _save();
            emit modelChanged();
            return;
        }
    }
}

void FleetLogs::retry(const QString &externalRef)
{
    Entry *e = _entry(externalRef);
    if (!e) {
        return;
    }
    e->collectAttempts = 0;
    e->uploadAttempts  = 0;
    e->parkedOnSize    = false;
    e->state  = e->path.isEmpty() ? Entry::State::Waiting : Entry::State::Ready;
    e->detail.clear();
    // A 409 is server-wide, so retrying one entry is also the operator telling
    // us to believe the server again.
    _save();
    emit modelChanged();
    _sweep();
}

// --------------------------------------------------------------- persistence

void FleetLogs::_save() const
{
    QJsonArray arr;
    for (const Entry &e : _entries) {
        if (e.state == Entry::State::Done) {
            continue;               // the server has it; nothing to carry over
        }
        QJsonObject o;
        o[QStringLiteral("ref")]       = e.ref;
        o[QStringLiteral("callsign")]  = e.callsign;
        o[QStringLiteral("sysid")]     = e.sysid;
        o[QStringLiteral("uid")]       = e.uid;
        o[QStringLiteral("started")]   = e.started;
        o[QStringLiteral("ended")]     = e.ended;
        o[QStringLiteral("duration")]  = e.durationS;
        o[QStringLiteral("path")]      = e.path;
        o[QStringLiteral("name")]      = e.name;
        o[QStringLiteral("detail")]    = e.detail;
        o[QStringLiteral("retryable")] = e.retryable;
        o[QStringLiteral("onSize")]    = e.parkedOnSize;
        o[QStringLiteral("tries")]     = e.uploadAttempts;
        o[QStringLiteral("parked")]    = (e.state == Entry::State::Parked);
        arr.append(o);
    }
    FleetStore::setLogQueueJson(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

void FleetLogs::_load()
{
    const QJsonDocument doc = QJsonDocument::fromJson(FleetStore::logQueueJson());
    if (!doc.isArray()) {
        return;
    }
    for (const QJsonValue &v : doc.array()) {
        const QJsonObject o = v.toObject();
        Entry e;
        e.ref       = o.value(QStringLiteral("ref")).toString();
        if (e.ref.isEmpty()) {
            continue;
        }
        e.callsign  = o.value(QStringLiteral("callsign")).toString();
        e.sysid     = o.value(QStringLiteral("sysid")).toInt();
        e.uid       = o.value(QStringLiteral("uid")).toString();
        e.started   = o.value(QStringLiteral("started")).toDouble();
        e.ended     = o.value(QStringLiteral("ended")).toDouble();
        e.durationS = o.value(QStringLiteral("duration")).toDouble();
        e.path      = o.value(QStringLiteral("path")).toString();
        e.name      = o.value(QStringLiteral("name")).toString();
        e.detail    = o.value(QStringLiteral("detail")).toString();
        e.retryable    = o.value(QStringLiteral("retryable")).toBool();
        e.parkedOnSize   = o.value(QStringLiteral("onSize")).toBool();
        e.uploadAttempts = o.value(QStringLiteral("tries")).toInt();

        // A collection or upload that was in flight when the app stopped comes
        // back as work to do, not as work that happened. Re-uploading is free.
        if (o.value(QStringLiteral("parked")).toBool()) {
            e.state = Entry::State::Parked;
        } else {
            e.state = e.path.isEmpty() ? Entry::State::Waiting : Entry::State::Ready;
        }
        _entries.append(e);
    }
}
