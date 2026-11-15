/****************************************************************************
 *
 * RotorLab GC RotorLab Connector - who and what we may file against.
 *
 ****************************************************************************/

#include "RotorLabRoster.h"
#include "RotorLabClient.h"
#include "RotorLabStore.h"

#include <algorithm>

#include <QtCore/QCoreApplication>
#include <QtCore/QDate>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QVariantMap>

#include "QGCLoggingCategory.h"

QGC_LOGGING_CATEGORY(RotorLabRosterLog, "gcs.custom.rotorlab.roster")

namespace {

QJsonArray arrayFrom(const QByteArray &json, const char *key)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    return doc.isObject() ? doc.object().value(QLatin1String(key)).toArray() : QJsonArray{};
}

/// True when the body is an object carrying `key` as an array, even an empty
/// one: the list was read, and an organization may truly have none.
bool hasList(const QByteArray &json, const char *key)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    return doc.isObject() && doc.object().value(QLatin1String(key)).isArray();
}

QByteArray wrap(const char *key, const QJsonArray &arr)
{
    QJsonObject o;
    o[QLatin1String(key)] = arr;
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

QStringList toStringList(const QJsonArray &arr)
{
    QStringList out;
    for (const QJsonValue &v : arr) {
        out << v.toString();
    }
    return out;
}

} // namespace

RotorLabRoster::RotorLabRoster(RotorLabClient *client, QObject *parent)
    : QObject(parent)
    , _client(client)
{
    loadCache();
}

void RotorLabRoster::loadCache()
{
    loadFrom(RotorLabStore::pilotsJson(), RotorLabStore::aircraftJson(),
             RotorLabStore::templatesJson(), RotorLabStore::readinessJson());
}

void RotorLabRoster::loadFrom(const QByteArray &pilotsJson, const QByteArray &aircraftJson,
                              const QByteArray &templatesJson, const QByteArray &readinessJson)
{
    _pilots    = arrayFrom(pilotsJson,    "pilots");
    _aircraft  = arrayFrom(aircraftJson,  "aircraft");
    _templates = arrayFrom(templatesJson, "templates");

    const QJsonDocument rd = QJsonDocument::fromJson(readinessJson);
    if (rd.isObject()) {
        const QJsonObject o = rd.object();
        _airworthiness = o.value(QStringLiteral("airworthiness")).toObject();
        _workorders    = o.value(QStringLiteral("workorders")).toArray();
        _readinessLoaded = !_airworthiness.isEmpty() || !_workorders.isEmpty();
    }
    // Loaded from the cache only when the cache holds both lists. Pilots with
    // no aircraft list would show a crew screen with nothing to pick from.
    _pilotsIn   = hasList(pilotsJson,   "pilots");
    _aircraftIn = hasList(aircraftJson, "aircraft");
    emit rosterChanged();
    emit templatesChanged();
    emit readinessChanged();
}

void RotorLabRoster::fetchAll()
{
    _fetchPilots();
    _fetchAircraft();
    _fetchTemplates();
    _fetchReadiness();
}

void RotorLabRoster::_fetchPilots()
{
    if (!_client) {
        return;
    }
    _client->getPilots([this](const HttpReply &r) { applyPilotsReply(r); });
}

void RotorLabRoster::_fetchAircraft()
{
    if (!_client) {
        return;
    }
    _client->getAircraft([this](const HttpReply &r) { applyAircraftReply(r); });
}

void RotorLabRoster::applyPilotsReply(const HttpReply &r)
{
    if (r.status != 200) {
        // Keep serving the cache. A roster fetch never blocks a crew.
        qCDebug(RotorLabRosterLog) << "pilots reply" << r.status << "- keeping the cache";
        return;
    }
    _pilots = r.body.value(QStringLiteral("pilots")).toArray();
    _pilotsIn = true;
    qCDebug(RotorLabRosterLog) << "pilots reply:" << _pilots.size() << "pilots, loaded" << loaded();
    RotorLabStore::setPilotsJson(wrap("pilots", _pilots));
    emit rosterChanged();
}

void RotorLabRoster::applyAircraftReply(const HttpReply &r)
{
    if (r.status != 200) {
        qCDebug(RotorLabRosterLog) << "aircraft reply" << r.status << "- keeping the cache";
        return;
    }
    _aircraft = r.body.value(QStringLiteral("aircraft")).toArray();
    _aircraftIn = true;
    qCDebug(RotorLabRosterLog) << "aircraft reply:" << _aircraft.size() << "aircraft, loaded" << loaded();
    RotorLabStore::setAircraftJson(wrap("aircraft", _aircraft));
    emit rosterChanged();
}

void RotorLabRoster::_fetchTemplates()
{
    if (!_client) {
        return;
    }
    _client->getChecklists([this](const HttpReply &r) {
        if (r.status != 200) {
            return;
        }
        _templates = r.body.value(QStringLiteral("templates")).toArray();
        RotorLabStore::setTemplatesJson(wrap("templates", _templates));
        emit templatesChanged();
    });
}

// ---------------------------------------------------------------- projections

QVariantList RotorLabRoster::pilots() const
{
    QVariantList out;
    for (const QJsonValue &v : _pilots) {
        const QJsonObject o = v.toObject();
        QVariantMap m;
        m[QStringLiteral("id")]       = o.value(QStringLiteral("id")).toInt();
        m[QStringLiteral("name")]     = o.value(QStringLiteral("name")).toString();
        // Shown as a quiet note beside the name. Never a disabled row: a
        // lapsed pilot in an emergency is the operator's judgement to make.
        m[QStringLiteral("currency")] = o.value(QStringLiteral("currency")).toString();
        m[QStringLiteral("lapsed")]   = toStringList(o.value(QStringLiteral("lapsed")).toArray());
        out.append(m);
    }
    return out;
}

QVariantList RotorLabRoster::aircraft() const
{
    QVariantList out;
    for (const QJsonValue &v : _aircraft) {
        const QJsonObject o = v.toObject();
        QVariantMap m;
        for (auto it = o.constBegin(); it != o.constEnd(); ++it) {
            m.insert(it.key(), it.value().toVariant());
        }
        out.append(m);
    }
    return out;
}

QVariantList RotorLabRoster::assets() const
{
    QVariantList out;
    for (const QJsonValue &v : _aircraft) {
        const QJsonObject o = v.toObject();
        QVariantMap m;
        m[QStringLiteral("id")] = o.value(QStringLiteral("id")).toInt();

        // Name first, then the registration in brackets when there is one -
        // a crew reads a tail number faster than a nickname.
        const QString name = o.value(QStringLiteral("name")).toString();
        const QString reg  = o.value(QStringLiteral("registration")).toString();
        m[QStringLiteral("name")] = reg.isEmpty() ? name
                                                  : QStringLiteral("%1  (%2)").arg(name, reg);

        // Additive fields the server grew. Shown where they help and omitted
        // where they are blank - a picker full of empty labels helps nobody.
        // Readiness and open work are copied whole into the row: the crew
        // screen reads them from here, never from a second call.
        const int id = o.value(QStringLiteral("id")).toInt();
        const QVariantMap ready = readinessFor(id);
        for (const char *key : { "state", "readiness", "why", "note", "work", "groundingWork" }) {
            m[QLatin1String(key)] = ready.value(QLatin1String(key));
        }
        m[QStringLiteral("status")] = o.value(QStringLiteral("status")).toString();
        m[QStringLiteral("registrationExpires")] =
            o.value(QStringLiteral("registration_expires")).toString();
        out.append(m);
    }
    return out;
}

QVariantList RotorLabRoster::templates(const QString &kind) const
{
    QVariantList out;
    for (const QJsonValue &v : _templates) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("kind")).toString() != kind) {
            continue;
        }
        // A template the customer switched off is not offered.
        if (o.value(QStringLiteral("active")).toInt(1) == 0) {
            continue;
        }
        QVariantMap m;
        m[QStringLiteral("id")]    = o.value(QStringLiteral("id")).toInt();
        m[QStringLiteral("kind")]  = kind;
        m[QStringLiteral("name")]  = o.value(QStringLiteral("name")).toString();
        m[QStringLiteral("items")] = toStringList(o.value(QStringLiteral("items")).toArray());
        out.append(m);
    }
    return out;
}

bool RotorLabRoster::hasTemplates(const QString &kind) const
{
    return !templates(kind).isEmpty();
}

// ------------------------------------------------------------------ readiness

void RotorLabRoster::_saveReadiness()
{
    QJsonObject o;
    o[QStringLiteral("airworthiness")] = _airworthiness;
    o[QStringLiteral("workorders")]    = _workorders;
    RotorLabStore::setReadinessJson(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

void RotorLabRoster::_fetchReadiness()
{
    if (!_client) {
        return;
    }
    // Two independent calls. Either may be empty or may fail, and neither is
    // allowed to hold up the other - this whole section is advisory, so a
    // partial answer is worth more than no answer.
    _client->getHealth([this](const HttpReply &r) {
        if (r.status != 200) {
            return;
        }
        _airworthiness = r.body.value(QStringLiteral("airworthiness")).toObject();
        _readinessLoaded = true;
        _saveReadiness();
        emit readinessChanged();
    });
    _client->getWorkorders([this](const HttpReply &r) {
        if (r.status != 200) {
            return;
        }
        _workorders = r.body.value(QStringLiteral("workorders")).toArray();
        _readinessLoaded = true;
        _saveReadiness();
        emit readinessChanged();
    });
}

namespace {

bool truthy(const QJsonValue &v)
{
    if (v.isBool())   return v.toBool();
    if (v.isDouble()) return v.toDouble() != 0.0;
    if (v.isString()) {
        const QString t = v.toString().trimmed().toLower();
        return !t.isEmpty() && t != QLatin1String("0") && t != QLatin1String("false");
    }
    return false;
}

int priorityRank(const QString &p)
{
    if (p == QLatin1String("grounding")) return 0;
    if (p == QLatin1String("urgent"))    return 1;
    if (p == QLatin1String("soon"))      return 2;
    if (p == QLatin1String("routine"))   return 3;
    return 4;
}

QDate dateOf(const QString &iso)
{
    return QDate::fromString(iso.trimmed().left(10), Qt::ISODate);
}

} // namespace

QString RotorLabRoster::readinessWord(const QJsonObject &entry)
{
    // RotorLab's own printed word wins. Mapping here is only the fallback for
    // an entry that carries the state alone (RotorLab's own words).
    const QString printed = entry.value(QStringLiteral("readiness")).toString().trimmed();
    if (!printed.isEmpty()) {
        return printed;
    }
    const QString state = entry.value(QStringLiteral("state")).toString().trimmed();
    if (state == QLatin1String("go"))      return tr("No flags");
    if (state == QLatin1String("limits"))  return tr("Limits on record");
    if (state == QLatin1String("unknown")) return tr("Condition unknown");
    if (state == QLatin1String("no_go"))   return tr("No go");
    if (state == QLatin1String("advisory")) {
        // As RotorLab words it: the count of notes is the length of `why`;
        // with no `why` at all the count is unknown.
        if (!entry.value(QStringLiteral("why")).isArray()) return tr("No flags, with notes");
        const int n = entry.value(QStringLiteral("why")).toArray().size();
        if (n == 0) return tr("No flags");
        return n == 1 ? tr("No flags, 1 note") : tr("No flags, %1 notes").arg(n);
    }
    // A word this build does not know is shown exactly as RotorLab sent it.
    return state;
}

QVariantList RotorLabRoster::workFrom(const QJsonArray &workorders, int assetId)
{
    QVariantList out;
    for (const QJsonValue &v : workorders) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("asset_id")).toInt(-1) != assetId) {
            continue;
        }
        // Done and cancelled work is history, and history does not belong on
        // a pre-flight screen. "closed" is the older status word; a filled
        // `closed` timestamp means the same.
        const QString status = o.value(QStringLiteral("status")).toString().trimmed();
        if (status == QLatin1String("done") || status == QLatin1String("cancelled")
            || status == QLatin1String("closed")
            || !o.value(QStringLiteral("closed")).toString().trimmed().isEmpty()) {
            continue;
        }
        const QString title = o.value(QStringLiteral("title")).toString().trimmed();
        if (title.isEmpty()) {
            continue;
        }

        const QString priority = o.value(QStringLiteral("priority")).toString().trimmed();
        const bool isGroundingPriority = priority == QLatin1String("grounding");
        const bool grounding = isGroundingPriority
                               || truthy(o.value(QStringLiteral("grounding_hold")));

        // overdue and due_soon are RotorLab's own, counted from the
        // organization's today. GC keeps no
        // window of its own. due_by is shown as its date part.
        const QString dueBy = o.value(QStringLiteral("due_by")).toString().trimmed();
        const QDate due = dateOf(dueBy);
        const bool overdue = truthy(o.value(QStringLiteral("overdue")));
        const bool dueSoon = !overdue && truthy(o.value(QStringLiteral("due_soon")));

        // The sentence states what RotorLab has on record and nothing more.
        QStringList extra;
        if (!priority.isEmpty() && priority != QLatin1String("routine") && !isGroundingPriority) {
            extra << priority;
        }
        if (grounding && !isGroundingPriority) {
            extra << tr("grounding hold");
        }
        if (status == QLatin1String("in_progress")) {
            extra << tr("in progress");
        }
        if (due.isValid()) {
            extra << tr("due by %1").arg(due.toString(Qt::ISODate));
        }
        if (overdue) {
            extra << tr("past due");
        } else if (dueSoon) {
            extra << tr("due soon");
        }
        QString line = grounding ? tr("Open grounding work order: %1").arg(title)
                                 : tr("Open work order: %1").arg(title);
        if (!extra.isEmpty()) {
            line += QStringLiteral(" (") + extra.join(QStringLiteral(", ")) + QLatin1Char(')');
        }

        QVariantMap m;
        m[QStringLiteral("text")]      = title;
        m[QStringLiteral("priority")]  = priority;
        m[QStringLiteral("status")]    = status;
        m[QStringLiteral("dueBy")]     = dueBy;
        m[QStringLiteral("grounding")] = grounding;
        m[QStringLiteral("overdue")]   = overdue;
        m[QStringLiteral("dueSoon")]   = dueSoon;
        m[QStringLiteral("line")]      = line;
        out.append(m);
    }

    // Worst first, so the line a crew reads before launching is the one that
    // matters most: grounding work, then the server's priority order, then the
    // earliest due date (no date last).
    std::stable_sort(out.begin(), out.end(), [](const QVariant &a, const QVariant &b) {
        const QVariantMap x = a.toMap();
        const QVariantMap y = b.toMap();
        const bool gx = x.value(QStringLiteral("grounding")).toBool();
        const bool gy = y.value(QStringLiteral("grounding")).toBool();
        if (gx != gy) return gx;
        const int px = priorityRank(x.value(QStringLiteral("priority")).toString());
        const int py = priorityRank(y.value(QStringLiteral("priority")).toString());
        if (px != py) return px < py;
        const QDate dx = dateOf(x.value(QStringLiteral("dueBy")).toString());
        const QDate dy = dateOf(y.value(QStringLiteral("dueBy")).toString());
        if (dx.isValid() != dy.isValid()) return dx.isValid();
        return dx.isValid() && dx < dy;
    });
    return out;
}

QVariantMap RotorLabRoster::readinessFrom(const QJsonObject &airworthiness,
                                          const QJsonArray &workorders, int assetId)
{
    QVariantMap m;
    m[QStringLiteral("state")]         = QString();
    m[QStringLiteral("readiness")]     = QString();
    m[QStringLiteral("why")]           = QStringList();
    m[QStringLiteral("note")]          = QString();
    m[QStringLiteral("work")]          = QVariantList();
    m[QStringLiteral("groundingWork")] = false;
    if (assetId < 0) {
        return m;
    }

    // The server keys this map by asset id rendered as a string.
    const QJsonObject a = airworthiness.value(QString::number(assetId)).toObject();
    const QString state = a.value(QStringLiteral("state")).toString().trimmed();
    QStringList why;
    for (const QJsonValue &w : a.value(QStringLiteral("why")).toArray()) {
        const QString t = w.isString() ? w.toString()
                                       : w.toObject().value(QStringLiteral("text")).toString();
        if (!t.trimmed().isEmpty()) {
            why << t.trimmed();
        }
    }
    const QString word = a.isEmpty() ? QString() : readinessWord(a);

    // Every state but go gets a note, in RotorLab's own word. Nothing is
    // relabeled: advisory, limits and unknown are not "caution".
    QString note;
    if (state != QLatin1String("go") && !word.isEmpty()) {
        note = tr("RotorLab lists this aircraft as %1").arg(word);
        if (!why.isEmpty()) {
            note += QStringLiteral(": ") + why.join(QStringLiteral("; "));
        }
    }

    const QVariantList work = workFrom(workorders, assetId);
    bool groundingWork = false;
    for (const QVariant &w : work) {
        groundingWork = groundingWork || w.toMap().value(QStringLiteral("grounding")).toBool();
    }

    m[QStringLiteral("state")]         = state;
    m[QStringLiteral("readiness")]     = word;
    m[QStringLiteral("why")]           = why;
    m[QStringLiteral("note")]          = note;
    m[QStringLiteral("work")]          = work;
    m[QStringLiteral("groundingWork")] = groundingWork;
    return m;
}

QVariantMap RotorLabRoster::readinessFor(int assetId) const
{
    return readinessFrom(_airworthiness, _workorders, assetId);
}

// ------------------------------------------------------- crew screen rules

namespace RotorLabCrew {

Open whenToOpen(bool active, bool anyArmed, bool onDemand)
{
    if (!active) {
        return Open::No;
    }
    if (onDemand || !anyArmed) {
        return Open::Now;
    }
    return Open::AfterDisarm;
}

bool offerPreflight(bool active, bool anyArmed, bool vehicleConnected,
                    bool hasTemplates, bool offeredThisSortie,
                    const QString &currentOffer)
{
    return active && !anyArmed && vehicleConnected && hasTemplates
           && !offeredThisSortie && currentOffer.isEmpty();
}

bool clearErrorAtDisarm(const QString &lastError, const QString &armedRefusal)
{
    return !armedRefusal.isEmpty() && lastError == armedRefusal;
}

bool askAfterVerify(bool askSetting, bool pilotSet, bool aircraftSet, bool newKey)
{
    if (!askSetting) {
        return false;
    }
    return newKey || !(pilotSet && aircraftSet);
}

bool reconnectOnFiled(bool offline, bool verifiedBefore)
{
    return offline && verifiedBefore;
}

QString missionsEmptyText(bool loaded, const QString &pilotName)
{
    if (!loaded) {
        return QCoreApplication::translate("RotorLabCrew", "Not loaded yet.");
    }
    if (pilotName.isEmpty()) {
        return QCoreApplication::translate("RotorLabCrew",
                                           "No missions published to your organization.");
    }
    return QCoreApplication::translate("RotorLabCrew",
               "None made by %1 among the missions published to your organization.")
        .arg(pilotName);
}

} // namespace RotorLabCrew
