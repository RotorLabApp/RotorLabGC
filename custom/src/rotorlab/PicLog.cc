/****************************************************************************
 *
 * RotorLab GC RotorLab Connector - who the crew recorded as pilot in command.
 *
 ****************************************************************************/

#include "PicLog.h"
#include "FilingRules.h"

#include <algorithm>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QStandardPaths>

namespace RotorLabPic {

namespace {

constexpr int kMaxPeriods  = 24;     ///< PicPeriod array maxItems
constexpr int kMaxReason   = 200;
constexpr int kMaxName     = 80;
constexpr int kMaxCrewRows = 12;

QString iso(const QDateTime &t)
{
    return t.toUTC().toString(Qt::ISODate);
}

bool appliesTo(const Handoff &h, int sysid)
{
    return h.sysid < 0 || sysid < 0 || h.sysid == sysid;
}

QJsonObject period(int pilotId, const QString &name, const QDateTime &from,
                   const QString &reason, const QDateTime &sentAt, int sysid)
{
    QJsonObject p;
    p[QStringLiteral("role")]     = QStringLiteral("remote_pic");
    p[QStringLiteral("pilot_id")] = pilotId;
    p[QStringLiteral("from")]     = iso(from);
    if (!reason.trimmed().isEmpty()) {
        p[QStringLiteral("reason")] = reason.trimmed().left(kMaxReason);
    }
    if (!name.isEmpty()) {
        // The server keeps it only when pilot_id names nobody in the org.
        p[QStringLiteral("name")] = name.left(kMaxName);
    }
    if (sentAt.isValid()) {
        p[QStringLiteral("sent_at")] = iso(sentAt);
    }
    if (sysid >= 0) {
        p[QStringLiteral("sysid")] = QString::number(sysid);
    }
    return p;
}

} // namespace

bool serverTakesPic(const QString &serverVersion)
{
    return RotorLabFiling::versionAtLeast(serverVersion, kMinMajor, kMinMinor, kMinPatch);
}

bool picEnabled(const QString &serverVersion, bool sortiesAnswered)
{
    return sortiesAnswered || serverTakesPic(serverVersion);
}

QJsonObject toJson(const Handoff &h)
{
    QJsonObject o;
    o[QStringLiteral("at")]        = iso(h.at);
    o[QStringLiteral("sysid")]     = h.sysid;
    o[QStringLiteral("from_id")]   = h.fromId;
    o[QStringLiteral("from_name")] = h.fromName;
    o[QStringLiteral("to_id")]     = h.toId;
    o[QStringLiteral("to_name")]   = h.toName;
    if (!h.reason.isEmpty()) {
        o[QStringLiteral("reason")] = h.reason;
    }
    return o;
}

bool fromJson(const QJsonObject &o, Handoff *out)
{
    const QDateTime at = QDateTime::fromString(o.value(QStringLiteral("at")).toString(), Qt::ISODate);
    if (!at.isValid()) {
        return false;
    }
    out->at       = at.toUTC();
    out->sysid    = o.value(QStringLiteral("sysid")).toInt(-1);
    out->fromId   = o.value(QStringLiteral("from_id")).toInt(-1);
    out->fromName = o.value(QStringLiteral("from_name")).toString();
    out->toId     = o.value(QStringLiteral("to_id")).toInt(-1);
    out->toName   = o.value(QStringLiteral("to_name")).toString();
    out->reason   = o.value(QStringLiteral("reason")).toString();
    return true;
}

bool pilotAt(const QList<Handoff> &log, const QDateTime &t, int sysid,
             int *pilotId, QString *pilotName)
{
    const Handoff *last = nullptr;
    for (const Handoff &h : log) {
        if (h.at <= t && appliesTo(h, sysid) && (!last || h.at >= last->at)) {
            last = &h;
        }
    }
    if (!last) {
        return false;
    }
    *pilotId = last->toId;
    *pilotName = last->toId >= 0 ? last->toName : QString();
    return true;
}

int filingPilot(const QList<Handoff> &log, const QDateTime &takeoff, int sysid,
                int currentPilot, bool picEnabled)
{
    int id = -1;
    QString name;
    if (pilotAt(log, takeoff, sysid, &id, &name)) {
        return id;
    }
    return picEnabled ? -1 : currentPilot;
}

QJsonArray buildPeriods(const QList<Handoff> &log, const QDateTime &takeoff,
                        const QDateTime &landing, int sysid)
{
    QList<QJsonObject> periods;

    int atTakeoff = -1;
    QString atTakeoffName;
    if (pilotAt(log, takeoff, sysid, &atTakeoff, &atTakeoffName) && atTakeoff >= 0) {
        periods << period(atTakeoff, atTakeoffName, takeoff, {}, {}, sysid);
    }

    // The changes made in the air, oldest first.
    QList<Handoff> during;
    for (const Handoff &h : log) {
        if (h.at > takeoff && (!landing.isValid() || h.at < landing) && appliesTo(h, sysid)) {
            during << h;
        }
    }
    std::stable_sort(during.begin(), during.end(),
                     [](const Handoff &a, const Handoff &b) { return a.at < b.at; });

    bool open = !periods.isEmpty();
    for (const Handoff &h : during) {
        if (open) {
            periods.last()[QStringLiteral("to")] = iso(h.at);
            open = false;
        }
        if (h.toId < 0) {
            continue;       // no pilot named from here
        }
        periods << period(h.toId, h.toName, h.at, h.reason, h.at, sysid);
        open = true;
    }

    // The server takes 24. Keep the first (the pilot at takeoff) and the most
    // recent, so the record ends with who brought it home.
    while (periods.size() > kMaxPeriods) {
        periods.removeAt(1);
    }
    QJsonArray out;
    for (const QJsonObject &p : periods) {
        out.append(p);
    }
    return out;
}

QVariantList seatsFor(const QJsonObject &sortiesReply, int assetId, QVariantMap *info)
{
    const QJsonArray sorties = sortiesReply.value(QStringLiteral("sorties")).toArray();
    QJsonObject pick;
    for (const QJsonValue &v : sorties) {
        const QJsonObject s = v.toObject();
        if (assetId >= 0 && s.value(QStringLiteral("asset_id")).toInt(-1) != assetId) {
            continue;
        }
        if (s.value(QStringLiteral("status")).toString() == QLatin1String("out")) {
            pick = s;
            break;
        }
        if (pick.isEmpty()) {
            pick = s;
        }
    }
    if (info) {
        info->clear();
    }
    if (pick.isEmpty()) {
        return {};
    }
    if (info) {
        (*info)[QStringLiteral("id")]             = pick.value(QStringLiteral("id")).toInt(-1);
        (*info)[QStringLiteral("jobTitle")]       = pick.value(QStringLiteral("job_title")).toString();
        (*info)[QStringLiteral("operatingBasis")] = pick.value(QStringLiteral("operating_basis")).toString();
        (*info)[QStringLiteral("window")]         = pick.value(QStringLiteral("window")).toString();
    }

    QVariantList planned;
    QVariantList rest;
    for (const QJsonValue &v : pick.value(QStringLiteral("seats")).toArray()) {
        const QJsonObject s = v.toObject();
        const int id = s.value(QStringLiteral("user_id")).toInt(-1);
        if (id < 0) {
            continue;
        }
        QVariantMap m;
        m[QStringLiteral("id")]         = id;
        m[QStringLiteral("name")]       = s.value(QStringLiteral("name")).toString();
        m[QStringLiteral("role")]       = s.value(QStringLiteral("role")).toString();
        m[QStringLiteral("duty")]       = s.value(QStringLiteral("duty")).toString();
        m[QStringLiteral("dutyLabel")]  = s.value(QStringLiteral("duty_label")).toString();
        m[QStringLiteral("controls")]   = s.value(QStringLiteral("controls")).toBool();
        m[QStringLiteral("plannedPic")] = s.value(QStringLiteral("planned_pic")).toBool();
        m[QStringLiteral("plannedBy")]  = s.value(QStringLiteral("planned_by")).toString();
        (m.value(QStringLiteral("plannedPic")).toBool() ? planned : rest) << m;
    }
    return planned + rest;
}

QVariantList mergeCrew(const QVariantList &seats, const QVariantList &pilots)
{
    QVariantList out;
    QList<int> seen;
    for (const QVariant &v : seats) {
        QVariantMap seat = v.toMap();
        const int id = seat.value(QStringLiteral("id")).toInt();
        if (seen.contains(id)) {
            continue;       // one row per person; the first seat names them
        }
        seen << id;
        // Currency words from the roster row, when the person is on it.
        for (const QVariant &p : pilots) {
            const QVariantMap pm = p.toMap();
            if (pm.value(QStringLiteral("id")).toInt() == id) {
                for (auto it = pm.constBegin(); it != pm.constEnd(); ++it) {
                    if (!seat.contains(it.key())) {
                        seat.insert(it.key(), it.value());
                    }
                }
                break;
            }
        }
        seat[QStringLiteral("seat")] = true;
        out << seat;
    }
    for (const QVariant &p : pilots) {
        QVariantMap pm = p.toMap();
        if (seen.contains(pm.value(QStringLiteral("id")).toInt())) {
            continue;
        }
        pm[QStringLiteral("seat")] = false;
        out << pm;
    }
    return out;
}

int plannedPic(const QVariantList &seats)
{
    for (const QVariant &v : seats) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("plannedPic")).toBool()) {
            return m.value(QStringLiteral("id")).toInt();
        }
    }
    return -1;
}

QJsonArray crewRows(const QVariantList &seats, int picId)
{
    QJsonArray out;
    QList<int> seen;
    for (const QVariant &v : seats) {
        const QVariantMap m = v.toMap();
        const int id = m.value(QStringLiteral("id")).toInt();
        const QString duty = m.value(QStringLiteral("duty")).toString();
        if (id < 0 || id == picId || duty.isEmpty() || seen.contains(id)) {
            continue;
        }
        seen << id;
        QJsonObject r;
        r[QStringLiteral("user_id")] = id;
        r[QStringLiteral("duty")]    = duty;
        out.append(r);
        if (out.size() >= kMaxCrewRows) {
            break;
        }
    }
    return out;
}

QString pilotWord(const QString &operatingBasis, const QString &jurisdiction)
{
    if (operatingBasis == QLatin1String("part107")
        || (operatingBasis.isEmpty() && jurisdiction == QLatin1String("US"))) {
        return QCoreApplication::translate("RotorLabPic", "Remote pilot in command");
    }
    if (jurisdiction == QLatin1String("GB") || jurisdiction == QLatin1String("EU")) {
        return QCoreApplication::translate("RotorLabPic", "Remote pilot");
    }
    return QCoreApplication::translate("RotorLabPic", "Pilot");
}

QString picLine(const QString &name, const QDateTime &from,
                const QString &operatingBasis, const QString &jurisdiction)
{
    if (name.isEmpty()) {
        return {};
    }
    const QString word = pilotWord(operatingBasis, jurisdiction);
    if (!from.isValid()) {
        return QStringLiteral("%1: %2").arg(word, name);
    }
    return QCoreApplication::translate("RotorLabPic", "%1: %2 (from %3)")
        .arg(word, name, from.toLocalTime().toString(QStringLiteral("HH:mm")));
}

} // namespace RotorLabPic

// ------------------------------------------------------------------- the file

PicLog::PicLog(QObject *parent)
    : PicLog(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                 + QStringLiteral("/rotorlab-pic"), parent)
{
}

PicLog::PicLog(const QString &dir, QObject *parent)
    : QObject(parent)
    , _dir(dir)
{
    // Nothing is read back. The file is the station's record of what the crew
    // did; the filing uses only the changes made since this GC started, so a
    // pilot named before a restart never lands on a flight after it (the
    // pilot is held in memory only, like the rest of the run context).
}

void PicLog::append(const RotorLabPic::Handoff &h)
{
    // Memory first, so the filing has it whatever the disk does.
    _entries << h;

    QDir().mkpath(_dir);
    QFile f(_dir + QStringLiteral("/") + h.at.toUTC().date().toString(QStringLiteral("yyyy-MM-dd"))
            + QStringLiteral(".jsonl"));
    if (!f.open(QIODevice::Append | QIODevice::WriteOnly)) {
        emit logMessage(QStringLiteral("could not write the pilot change log: %1").arg(f.errorString()));
        return;
    }
    QByteArray line = QJsonDocument(RotorLabPic::toJson(h)).toJson(QJsonDocument::Compact);
    line.append('\n');
    if (f.write(line) != line.size()) {
        emit logMessage(QStringLiteral("could not write the pilot change log: %1").arg(f.errorString()));
    }
}
