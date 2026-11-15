/****************************************************************************
 *
 * RotorLab GC RotorLab Connector - the spool.
 *
 ****************************************************************************/

#include "FlightSpool.h"
#include "FilingRules.h"
#include "RotorLabClient.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QStandardPaths>
#include <QtCore/QUuid>
#include <QtCore/QVariantMap>
#include <QtNetwork/QNetworkReply>

#ifdef Q_OS_WIN
#include <io.h>
#else
#include <unistd.h>
#endif

#include "QGCLoggingCategory.h"

QGC_LOGGING_CATEGORY(FlightSpoolLog, "gcs.custom.rotorlab.spool")

namespace {

/// While anything is pending. The per-item backoff decides what actually goes.
constexpr int kTickMs = 60 * 1000;

/// 1 s, 5 s, 30 s, 2 min, 10 min, then every 10 min.
const int kBackoffS[] = { 1, 5, 30, 120, 600 };

/// Done records are kept so a customer can see what filed, then swept.
constexpr qint64 kDoneKeepS = 7 * 24 * 60 * 60;

QString kindName(FlightSpool::Kind k)
{
    switch (k) {
    case FlightSpool::Kind::Flight:    return QStringLiteral("flight");
    case FlightSpool::Kind::Checklist: return QStringLiteral("checklist");
    case FlightSpool::Kind::Log:       return QStringLiteral("log");
    }
    return {};
}

FlightSpool::Kind kindFrom(const QString &s)
{
    if (s == QLatin1String("checklist")) return FlightSpool::Kind::Checklist;
    if (s == QLatin1String("log"))       return FlightSpool::Kind::Log;
    return FlightSpool::Kind::Flight;
}

double nowS() { return double(QDateTime::currentMSecsSinceEpoch()) / 1000.0; }

} // namespace

FlightSpool::FlightSpool(RotorLabClient *client, QObject *parent)
    : QObject(parent)
    , _client(client)
{
    QDir().mkpath(_dir());
    _load();

    _timer.setInterval(kTickMs);
    connect(&_timer, &QTimer::timeout, this, &FlightSpool::_tick);
    if (pendingCount() > 0) {
        _timer.start();
    }
}

FlightSpool::~FlightSpool() = default;

QString FlightSpool::_dir()
{
    // Application data, not Documents: this is internal state a crew never
    // edits by hand, and Documents is frequently a synced folder.
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + QStringLiteral("/rotorlab-spool");
}

// --------------------------------------------------------------- durable IO

bool FlightSpool::_writeDurable(const QString &path, const QByteArray &bytes)
{
    // Write to a temporary beside the target, force it all the way to the
    // platter, and only then rename. A half-written record that looks complete
    // is worse than no record.
    const QString tmp = path + QStringLiteral(".part");
    {
        QFile f(tmp);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qCWarning(FlightSpoolLog) << "cannot write" << tmp << f.errorString();
            return false;
        }
        if (f.write(bytes) != bytes.size()) {
            return false;
        }
        f.flush();
#ifdef Q_OS_WIN
        ::_commit(f.handle());
#else
        ::fsync(f.handle());
#endif
    }
    QFile::remove(path);
    return QFile::rename(tmp, path);
}

// ------------------------------------------------------------------ contents

void FlightSpool::_save(const Item &it) const
{
    QJsonObject o;
    o[QStringLiteral("id")]           = it.id;
    o[QStringLiteral("kind")]         = kindName(it.kind);
    o[QStringLiteral("external_ref")] = it.externalRef;
    o[QStringLiteral("body")]         = it.body;
    o[QStringLiteral("file")]         = it.filePath;
    o[QStringLiteral("name")]         = it.displayName;
    o[QStringLiteral("depends_on")]   = it.dependsOn;
    o[QStringLiteral("sysid")]        = it.sysid;
    o[QStringLiteral("flight_id")]    = it.flightId;
    o[QStringLiteral("created")]      = it.created;
    o[QStringLiteral("attempts")]     = it.attempts;
    o[QStringLiteral("last_attempt")] = it.lastAttempt;
    o[QStringLiteral("last_error")]   = it.lastError;
    o[QStringLiteral("last_status")]  = it.lastStatus;
    o[QStringLiteral("state")]        = (it.state == State::NeedsAttention)
                                          ? QStringLiteral("needs_attention")
                                       : (it.state == State::Done)
                                          ? QStringLiteral("done")
                                          : QStringLiteral("pending");
    _writeDurable(_dir() + QStringLiteral("/") + it.id + QStringLiteral(".json"),
                  QJsonDocument(o).toJson(QJsonDocument::Compact));
}

void FlightSpool::_load()
{
    const QFileInfoList files =
        QDir(_dir()).entryInfoList({ QStringLiteral("*.json") }, QDir::Files, QDir::Name);
    for (const QFileInfo &fi : files) {
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        if (!doc.isObject()) {
            continue;
        }
        const QJsonObject o = doc.object();
        Item it;
        it.id          = o.value(QStringLiteral("id")).toString();
        it.kind        = kindFrom(o.value(QStringLiteral("kind")).toString());
        it.externalRef = o.value(QStringLiteral("external_ref")).toString();
        it.body        = o.value(QStringLiteral("body")).toObject();
        it.filePath    = o.value(QStringLiteral("file")).toString();
        it.displayName = o.value(QStringLiteral("name")).toString();
        it.dependsOn   = o.value(QStringLiteral("depends_on")).toString();
        it.sysid       = o.value(QStringLiteral("sysid")).toInt(0);
        it.flightId    = o.value(QStringLiteral("flight_id")).toInt(-1);
        it.created     = o.value(QStringLiteral("created")).toDouble();
        it.attempts    = o.value(QStringLiteral("attempts")).toInt();
        it.lastAttempt = o.value(QStringLiteral("last_attempt")).toDouble();
        it.lastError   = o.value(QStringLiteral("last_error")).toString();
        it.lastStatus  = o.value(QStringLiteral("last_status")).toInt(0);

        const QString st = o.value(QStringLiteral("state")).toString();
        // Anything that was mid-send when the app stopped comes back as work
        // to do, not work that happened. Re-sending is free: the server is
        // idempotent on external_ref.
        it.state = (st == QLatin1String("needs_attention")) ? State::NeedsAttention
                 : (st == QLatin1String("done"))            ? State::Done
                                                            : State::Pending;
        if (it.id.isEmpty()) {
            continue;
        }
        _correctFlightType(it);
        _items.append(it);
    }

    // Anything that came back still waiting on a flight that has since been
    // filed gets its id now, rather than waiting for a reply that already
    // happened in an earlier run of the application.
    for (Item &it : _items) {
        const int was = it.flightId;
        _resolveDependency(it);
        if (it.flightId != was) {
            _save(it);
        }
    }

    // A log or run whose flight was discarded by an earlier build stays visible,
    // parked with Discard, instead of waiting for ever.
    for (Item &it : _items) {
        if (it.state == State::Pending && _orphan(it)) {
            _parkOrphan(it);
        }
    }

    // Sweep only what is finished, and only when it is old.
    const double cutoff = nowS() - kDoneKeepS;
    for (int i = _items.size() - 1; i >= 0; --i) {
        if (_items[i].state == State::Done && _items[i].created < cutoff) {
            _remove(_items[i].id);
        }
    }
    if (!_items.isEmpty()) {
        qCDebug(FlightSpoolLog) << "loaded" << _items.size() << "spooled items";
    }
}

void FlightSpool::_remove(const QString &id)
{
    QFile::remove(_dir() + QStringLiteral("/") + id + QStringLiteral(".json"));
    for (int i = 0; i < _items.size(); ++i) {
        if (_items[i].id == id) {
            _items.removeAt(i);
            break;
        }
    }
}

FlightSpool::Item *FlightSpool::_find(const QString &id)
{
    for (Item &it : _items) {
        if (it.id == id) {
            return &it;
        }
    }
    return nullptr;
}

FlightSpool::Item *FlightSpool::_findByRef(const QString &ref)
{
    for (Item &it : _items) {
        if (it.externalRef == ref) {
            return &it;
        }
    }
    return nullptr;
}

void FlightSpool::_resolveDependency(Item &it)
{
    if (it.dependsOn.isEmpty() || it.dependsOn == awaitNextFlight() || it.flightId > 0) {
        return;
    }
    const Item *flight = _findByRef(it.dependsOn);
    if (!flight || flight->flightId <= 0) {
        return;                     // not filed yet; _onReply() will bind it
    }
    it.flightId = flight->flightId;
    if (it.kind == Kind::Checklist) {
        // Into the body, not merged at send time, so every retry sends the
        // same bytes.
        it.body[QStringLiteral("flight_id")] = it.flightId;
    }
}

bool FlightSpool::_correctFlightType(Item &it)
{
    if (it.kind != Kind::Flight || it.state == State::Done) {
        return false;
    }
    if (!RotorLabFiling::normalizeFlightBody(it.body)) {
        return false;
    }
    QString now = it.body.value(QStringLiteral("flight_type")).toString();
    const QString other = it.body.value(QStringLiteral("flight_type_other")).toString();
    if (!other.isEmpty()) {
        now += QStringLiteral(" (%1)").arg(other);
    }
    QString line;
    // normalizeFlightBody() only touches flight_type and flight_type_other,
    // so reaching here means the type changed.
    if (it.state == State::NeedsAttention
        && RotorLabFiling::requeueAfterTypeFix(it.lastStatus, /*flightTypeChanged*/ true)) {
        it.state = State::Pending;
        line = QStringLiteral("a flight held for its flight type was corrected to %1 "
                              "and will file").arg(now);
    } else {
        line = QStringLiteral("flight %1: flight type corrected to %2")
                   .arg(it.externalRef, now);
    }
    _save(it);
    // Queued, not emitted here: at start-up this runs inside the constructor,
    // before the connector has connected to logMessage.
    QTimer::singleShot(0, this, [this, line]() { emit logMessage(line); });
    return true;
}

// --------------------------------------------------------------------- adding

QString FlightSpool::addFlight(const QString &externalRef, const QJsonObject &body, int sysid)
{
    Item it;
    it.id          = QUuid::createUuid().toString(QUuid::Id128);
    it.kind        = Kind::Flight;
    it.externalRef = externalRef;
    it.body        = body;
    it.sysid       = sysid;
    it.created     = nowS();
    _items.append(it);
    _save(it);                          // BEFORE the first attempt, always
    emit logMessage(QStringLiteral("flight %1 queued to file").arg(externalRef));
    emit changed();
    if (!_timer.isActive()) {
        _timer.start();
    }
    drainNow();
    return it.id;
}

QString FlightSpool::addChecklist(const QString &externalRef, const QJsonObject &body,
                                  const QString &dependsOn)
{
    Item it;
    it.id          = QUuid::createUuid().toString(QUuid::Id128);
    it.kind        = Kind::Checklist;
    it.externalRef = externalRef;
    it.body        = body;
    it.dependsOn   = dependsOn;
    it.created     = nowS();
    _resolveDependency(it);
    _items.append(it);
    _save(it);
    emit changed();
    if (!_timer.isActive()) {
        _timer.start();
    }
    drainNow();
    return it.id;
}

void FlightSpool::bindWaitingChecklists(const QString &flightRef, int flightId)
{
    bool any = false;
    for (Item &it : _items) {
        if (it.kind != Kind::Checklist || it.dependsOn != awaitNextFlight()) {
            continue;
        }
        it.dependsOn = flightRef;
        it.flightId  = flightId;
        // The body is replayed verbatim on every attempt, so the flight id has
        // to go into it here rather than being merged at send time only.
        it.body[QStringLiteral("flight_id")] = flightId;
        _save(it);
        any = true;
    }
    if (any) {
        emit changed();
        drainNow();
    }
}

QString FlightSpool::addLog(const QString &externalRef, const QString &filePath,
                            const QString &displayName, const QString &dependsOn)
{
    Item it;
    it.id          = QUuid::createUuid().toString(QUuid::Id128);
    it.kind        = Kind::Log;
    it.externalRef = externalRef;
    it.filePath    = filePath;
    it.displayName = displayName;
    it.dependsOn   = dependsOn;
    it.created     = nowS();
    _resolveDependency(it);
    _items.append(it);
    _save(it);
    emit changed();
    if (!_timer.isActive()) {
        _timer.start();
    }
    drainNow();
    return it.id;
}

QString FlightSpool::addUncollectedLog(const QString &externalRef, const QString &dependsOn,
                                       int sysid, double endedAt, double durationS,
                                       const QString &reason)
{
    Item it;
    it.id          = QUuid::createUuid().toString(QUuid::Id128);
    it.kind        = Kind::Log;
    it.externalRef = externalRef;
    it.dependsOn   = dependsOn;
    it.sysid       = sysid;
    it.created     = nowS();
    // A log sends no JSON body; this is what a later collection needs.
    it.body[QStringLiteral("ended_at")]   = endedAt;
    it.body[QStringLiteral("duration_s")] = durationS;
    _resolveDependency(it);
    _items.append(it);
    _park(_items.last(), reason);
    return it.id;
}

void FlightSpool::attachCollectedLog(const QString &id, const QString &filePath)
{
    Item *it = _find(id);
    if (!it || !_uncollected(*it) || filePath.isEmpty()) {
        return;
    }
    it->filePath    = filePath;
    it->displayName = QFileInfo(filePath).fileName();
    if (_orphan(*it)) {
        _parkOrphan(*it);
        return;
    }
    it->state       = State::Pending;
    it->attempts    = 0;
    it->lastStatus  = 0;
    it->lastError.clear();
    _resolveDependency(*it);
    _save(*it);
    emit changed();
    if (!_timer.isActive()) {
        _timer.start();
    }
    drainNow();
}

void FlightSpool::setLogProblem(const QString &id, const QString &reason)
{
    Item *it = _find(id);
    if (it && _uncollected(*it)) {
        _park(*it, reason);
    }
}

bool FlightSpool::logToCollect(const QString &id, LogToCollect *out) const
{
    for (const Item &it : _items) {
        if (it.id != id) {
            continue;
        }
        if (!_uncollected(it) || it.state == State::Done) {
            return false;
        }
        if (out) {
            out->id        = it.id;
            out->flightRef = it.dependsOn;
            out->sysid     = it.sysid;
            out->endedAt   = it.body.value(QStringLiteral("ended_at")).toDouble();
            out->durationS = it.body.value(QStringLiteral("duration_s")).toDouble();
        }
        return true;
    }
    return false;
}

QList<FlightSpool::LogToCollect> FlightSpool::logsToCollect() const
{
    QList<LogToCollect> out;
    for (const Item &it : _items) {
        LogToCollect l;
        if (logToCollect(it.id, &l)) {
            out << l;
        }
    }
    return out;
}

bool FlightSpool::flightForRetry(const QString &id, QJsonObject *body, int *sysid) const
{
    for (const Item &it : _items) {
        if (it.id == id && it.kind == Kind::Flight && it.state != State::Done) {
            if (body) {
                *body = it.body;
            }
            if (sysid) {
                *sysid = it.sysid;
            }
            return true;
        }
    }
    return false;
}

bool FlightSpool::hasFlightTakenOffAt(const QString &takeoffIso) const
{
    if (takeoffIso.isEmpty()) {
        return false;
    }
    for (const Item &it : _items) {
        if (it.kind == Kind::Flight
            && it.body.value(QStringLiteral("takeoff_at")).toString() == takeoffIso) {
            return true;
        }
    }
    return false;
}

void FlightSpool::_park(Item &it, const QString &reason)
{
    it.state     = State::NeedsAttention;
    it.lastError = reason;
    _save(it);
    emit logMessage(QStringLiteral("%1 %2 needs attention: %3")
                        .arg(kindName(it.kind), it.externalRef, reason));
    emit changed();
}

// -------------------------------------------------------------------- draining

void FlightSpool::setFilingAllowed(bool allowed)
{
    _allowed = allowed;
    if (allowed) {
        drainNow();
    }
}

void FlightSpool::setServerVersion(const QString &version)
{
    _namesPilots = RotorLabFiling::serverNamesPilots(version);
}

bool FlightSpool::_dueNow(const Item &it) const
{
    if (it.attempts == 0) {
        return true;
    }
    const int idx  = qMin(it.attempts - 1, int(std::size(kBackoffS)) - 1);
    return nowS() - it.lastAttempt >= kBackoffS[idx];
}

FlightSpool::Item *FlightSpool::_next()
{
    // A flight before its checklists and its log. Within that, oldest first.
    for (Item &it : _items) {
        if (it.kind == Kind::Flight && it.state == State::Pending && _dueNow(it)) {
            return &it;
        }
    }
    for (Item &it : _items) {
        if (it.kind == Kind::Flight || it.state != State::Pending || !_dueNow(it)) {
            continue;
        }
        if (!it.dependsOn.isEmpty() && it.flightId < 0) {
            // Its flight has not been accepted yet. Not an error; it waits.
            continue;
        }
        return &it;
    }
    return nullptr;
}

void FlightSpool::drainNow()
{
    if (_sending || !_allowed || !_client) {
        return;
    }
    Item *it = _next();
    if (!it) {
        // Nothing to do. Stop the timer so an idle connector is idle.
        if (pendingCount() == 0 && _timer.isActive()) {
            _timer.stop();
        }
        return;
    }
    _send(*it);
}

void FlightSpool::_tick()
{
    drainNow();
}

void FlightSpool::_send(Item &it)
{
    if (it.kind == Kind::Log && !QFileInfo::exists(it.filePath)) {
        // Moved or deleted. Retrying on a timer cannot bring it back, so it is
        // parked with the reason and a Discard, not retried every 10 minutes.
        it.lastStatus = 0;
        _park(it, tr("The log file is no longer at %1. It was moved or deleted. Put it "
                     "back and press Try again, or Discard this log.")
                      .arg(QDir::toNativeSeparators(it.filePath)));
        // Something else may be ready to go.
        QTimer::singleShot(0, this, &FlightSpool::drainNow);
        return;
    }
    _correctFlightType(it);
    _sending = true;
    it.state       = State::Sending;
    it.attempts   += 1;
    it.lastAttempt = nowS();
    _save(it);

    const QString id = it.id;

    switch (it.kind) {
    case Kind::Flight: {
        // Packs are not sent until a RotorLab release takes them (A55). A
        // body stored by an earlier build may still carry them.
        QJsonObject body = it.body;
        body.remove(QStringLiteral("battery_serials"));
        _client->postFlight(body, [this, id](const HttpReply &r) {
            _onReply(id, r.status, r.error, r.body);
        });
        break;
    }
    case Kind::Checklist: {
        // The complete body, byte for byte, on every attempt. A partial retry
        // replaces the stored run and loses the failure notes.
        QJsonObject body = it.body;
        if (it.flightId > 0) {
            body[QStringLiteral("flight_id")] = it.flightId;
        }
        _client->postChecklist(body, [this, id](const HttpReply &r) {
            _onReply(id, r.status, r.error, r.body);
        });
        break;
    }
    case Kind::Log: {
        _inFlight = _client->postFlightLog(it.flightId, it.filePath, it.displayName,
                                           [this, id](const HttpReply &r) {
            _onReply(id, r.status, r.error, r.body);
        });
        break;
    }
    }
}

void FlightSpool::_onReply(const QString &id, int status, const QString &error,
                           const QJsonObject &body)
{
    _sending = false;
    Item *it = _find(id);
    if (!it) {
        return;
    }
    it->lastError  = RotorLabFiling::clipServerText(error);
    it->lastStatus = status;

    if (status == 200) {
        emit serverAnswered();
        it->state = State::Done;
        if (it->kind == Kind::Flight) {
            // Keep the numeric id: the log endpoint needs it and a checklist
            // wants it. Idempotent on external_ref, so a re-file returns the
            // same id rather than a second flight.
            const int flightId = body.value(QStringLiteral("id")).toInt(
                body.value(QStringLiteral("flight")).toObject()
                    .value(QStringLiteral("id")).toInt(-1));
            it->flightId = flightId;
            _save(*it);
            if (body.value(QStringLiteral("voided")).toBool()) {
                // A re-send of a flight that was voided in RotorLab: nothing
                // new was filed, so it must not read as "filed".
                QString msg = RotorLabFiling::clipServerText(
                    body.value(QStringLiteral("message")).toString());
                if (msg.isEmpty()) {
                    msg = tr("This flight was voided in RotorLab. Nothing new was filed.");
                }
                emit logMessage(QStringLiteral("flight %1: %2").arg(it->externalRef, msg));
                emit filingNotice(it->body, {}, { msg });
            } else {
                emit logMessage(QStringLiteral("filed flight %1").arg(it->externalRef));
                QStringList pilotNotes;
                QStringList notes;
                const QString pilot = RotorLabFiling::pilotNotice(
                    body, it->body.contains(QStringLiteral("pilot_id")), _namesPilots);
                if (!pilot.isEmpty()) {
                    pilotNotes << pilot;
                }
                // What RotorLab says about the pilot in command periods and the
                // crew lines it left off. Its own sentences, shown like all
                // other RotorLab text in GC: plain text, never reworded, and
                // clipped to 500 characters. The flight is filed either way.
                for (const char *key : { "pic_note", "crew_note" }) {
                    const QString note = RotorLabFiling::clipServerText(
                        body.value(QLatin1String(key)).toString());
                    if (!note.isEmpty()) {
                        notes << note;
                    }
                }
                if (!pilotNotes.isEmpty() || !notes.isEmpty()) {
                    emit filingNotice(it->body, pilotNotes, notes);
                }
            }
            emit flightFiled(it->externalRef, flightId);

            // Release anything waiting on it, including the pre-flight run
            // that was completed before this flight existed.
            // Maintenance the flight's own data raised. The contract does not
            // pin the element shape, so accept a plain string or an object and
            // take the first human-readable field; anything unrecognised is
            // skipped rather than shown as JSON to a crew at the truck.
            QStringList raised;
            const QJsonArray work = body.value(QStringLiteral("work_raised")).toArray();
            for (const QJsonValue &w : work) {
                if (w.isString()) {
                    const QString t = w.toString().trimmed();
                    if (!t.isEmpty()) {
                        raised << t;
                    }
                    continue;
                }
                const QJsonObject o = w.toObject();
                for (const char *k : { "title", "summary", "description",
                                       "reason", "name" }) {
                    const QString t = o.value(QLatin1String(k)).toString().trimmed();
                    if (!t.isEmpty()) {
                        raised << t;
                        break;
                    }
                }
            }
            if (!raised.isEmpty()) {
                emit workRaised(raised);
            }

            const QString ref = it->externalRef;
            bindWaitingChecklists(ref, flightId);
            for (Item &dep : _items) {
                if (dep.dependsOn == ref && dep.flightId < 0) {
                    dep.flightId = flightId;
                    _save(dep);
                }
            }
        } else {
            _save(*it);
            emit logMessage(it->kind == Kind::Log
                            ? QStringLiteral("uploaded the log for %1").arg(it->externalRef)
                            : QStringLiteral("filed a checklist run"));
        }
        emit changed();
        drainNow();
        return;
    }

    switch (status) {
    case 400:
        // Ours, and it will fail identically on the thousandth attempt.
        it->state = State::NeedsAttention;
        emit logMessage(QStringLiteral("RotorLab refused it: %1").arg(it->lastError));
        break;
    case 401:
        // Stop the whole drain. Do not retry a rejected key on a timer.
        it->state = State::Pending;
        _allowed = false;
        emit logMessage(QStringLiteral("filing stopped: the Device Key was rejected"));
        break;
    case 403:
        // Plan, trial or suspension. Keep it - a subscription lapsing on a
        // Tuesday must not lose Tuesday's flying - and show the sentence.
        it->state = State::Pending;
        emit logMessage(it->lastError);
        break;
    case 404:
        // The flight belongs to another organisation, or does not exist. Never
        // rebuild a reference; there is nothing to fix.
        it->state = State::NeedsAttention;
        break;
    case 413:
        it->state = State::NeedsAttention;
        it->lastError = tr("RotorLab refused this log as larger than it takes for one file. "
                           "Attach it to the flight in RotorLab by hand: %1")
                            .arg(QDir::toNativeSeparators(it->filePath));
        emit logMessage(QStringLiteral("that log is larger than RotorLab accepts"));
        break;
    default:
        // Transport, 5xx, timeout. Transient by nature.
        it->state = State::Pending;
        break;
    }
    _save(*it);
    emit changed();
}

// ----------------------------------------------------------------- reporting

int FlightSpool::pendingCount() const
{
    int n = 0;
    for (const Item &it : _items) {
        if (it.state == State::Pending || it.state == State::Sending) {
            ++n;
        }
    }
    return n;
}

int FlightSpool::needsAttentionCount() const
{
    int n = 0;
    for (const Item &it : _items) {
        if (it.state == State::NeedsAttention) {
            ++n;
        }
    }
    return n;
}

QString FlightSpool::summaryText() const
{
    int flights = 0, checklists = 0, logs = 0;
    for (const Item &it : _items) {
        if (it.state != State::Pending && it.state != State::Sending) {
            continue;
        }
        switch (it.kind) {
        case Kind::Flight:    ++flights;    break;
        case Kind::Checklist: ++checklists; break;
        case Kind::Log:       ++logs;       break;
        }
    }
    QStringList parts;
    if (flights)    parts << tr("%n flight(s)", nullptr, flights);
    if (checklists) parts << tr("%n checklist(s)", nullptr, checklists);
    if (logs)       parts << tr("%n log(s)", nullptr, logs);
    if (parts.isEmpty()) {
        return {};
    }
    return tr("%1 waiting to file.").arg(parts.join(tr(" and ")));
}

QString FlightSpool::lastAttemptText() const
{
    double latest = 0;
    for (const Item &it : _items) {
        latest = qMax(latest, it.lastAttempt);
    }
    if (latest <= 0) {
        return {};
    }
    const qint64 secs = qint64(nowS() - latest);
    if (secs < 60)   return tr("Last attempt just now.");
    if (secs < 3600) return tr("Last attempt %n minute(s) ago.", nullptr, int(secs / 60));
    return tr("Last attempt %n hour(s) ago.", nullptr, int(secs / 3600));
}

QVariantList FlightSpool::model() const
{
    QVariantList out;
    for (const Item &it : _items) {
        if (it.state == State::Done) {
            continue;
        }
        QVariantMap m;
        m[QStringLiteral("id")]       = it.id;
        m[QStringLiteral("kind")]     = kindName(it.kind);
        m[QStringLiteral("ref")]      = it.externalRef;
        m[QStringLiteral("attempts")] = it.attempts;
        m[QStringLiteral("detail")]   = it.lastError;
        m[QStringLiteral("parked")]   = (it.state == State::NeedsAttention);
        m[QStringLiteral("uncollected")] = _uncollected(it);
        m[QStringLiteral("logs")]        = it.kind == Kind::Flight
                                               ? dependentCount(it.id, Kind::Log) : 0;
        m[QStringLiteral("checklists")]  = it.kind == Kind::Flight
                                               ? dependentCount(it.id, Kind::Checklist) : 0;
        m[QStringLiteral("when")]     = QDateTime::fromSecsSinceEpoch(qint64(it.created))
                                            .toString(QStringLiteral("d MMM  hh:mm"));
        out.append(m);
    }
    return out;
}

bool FlightSpool::_orphan(const Item &it) const
{
    if (it.kind == Kind::Flight || it.dependsOn.isEmpty() || it.dependsOn == awaitNextFlight()
        || it.flightId > 0) {
        return false;
    }
    for (const Item &f : _items) {
        if (f.kind == Kind::Flight && f.externalRef == it.dependsOn) {
            return false;
        }
    }
    return true;
}

void FlightSpool::_parkOrphan(Item &it)
{
    it.lastStatus = 0;
    QString reason = it.kind == Kind::Checklist
        ? tr("The flight this checklist run belongs to is no longer waiting to file on this "
             "computer, so the run cannot be filed. Discard it.")
        : tr("The flight this log belongs to is no longer waiting to file on this "
             "computer, so the log cannot be sent. Discard it.");
    if (!it.filePath.isEmpty()) {
        reason += QLatin1Char(' ')
                  + tr("To keep it, attach the file to the flight in RotorLab by hand: %1")
                        .arg(QDir::toNativeSeparators(it.filePath));
    }
    _park(it, reason);
}

int FlightSpool::dependentCount(const QString &id, Kind kind) const
{
    QString ref;
    for (const Item &it : _items) {
        if (it.id == id && it.kind == Kind::Flight) {
            ref = it.externalRef;
        }
    }
    int n = 0;
    for (const Item &it : _items) {
        if (!ref.isEmpty() && it.kind == kind && it.dependsOn == ref
            && it.state != State::Done) {
            ++n;
        }
    }
    return n;
}

void FlightSpool::forget(const QString &id)
{
    // A flight takes the logs and checklist runs waiting on it with it. _remove() deletes only
    // the spool's own record (<id>.json in the spool folder), never the log
    // file a record points at.
    QStringList gone{ id };
    if (const Item *it = _find(id); it && it->kind == Kind::Flight) {
        for (const Item &dep : _items) {
            if (dep.kind != Kind::Flight && dep.dependsOn == it->externalRef
                && dep.state != State::Done) {
                gone << dep.id;
            }
        }
    }
    for (const QString &g : gone) {
        _remove(g);
    }
    emit changed();
}

void FlightSpool::retry(const QString &id, const QJsonObject &newBody)
{
    if (Item *it = _find(id)) {
        if (_uncollected(*it)) {
            return;             // nothing to send until the connector collects it
        }
        if (it->kind == Kind::Flight && !newBody.isEmpty()
            && newBody.value(QStringLiteral("external_ref")).toString() == it->externalRef) {
            it->body = newBody;
        }
        it->state    = State::Pending;
        it->attempts = 0;
        it->lastError.clear();
        _allowed = true;
        _save(*it);
        emit changed();
        drainNow();
    }
}
