/****************************************************************************
 *
 * RotorLab GC RotorLab Connector - missions planned in RotorLab.app.
 *
 ****************************************************************************/

#include "MissionLibrary.h"
#include "RotorLabClient.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QStandardPaths>
#include <QtCore/QStringList>
#include <QtCore/QVariantMap>

#include "QGCLoggingCategory.h"

QGC_LOGGING_CATEGORY(MissionLibraryLog, "gcs.custom.rotorlab.missions")

namespace {

/// Turn a mission name into something safe to put on a filesystem, on every
/// platform, without losing which mission it was.
QString safeStem(const QString &name, int id)
{
    QString out;
    for (const QChar &c : name) {
        out += (c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_'))
                   ? c : QLatin1Char('-');
    }
    out = out.left(48).trimmed();
    while (out.endsWith(QLatin1Char('-'))) {
        out.chop(1);
    }
    // The id always goes on the end: two missions may share a name, and the
    // crew must never load yesterday's because it happened to be called the
    // same thing.
    return out.isEmpty() ? QStringLiteral("mission-%1").arg(id)
                         : QStringLiteral("%1-%2").arg(out).arg(id);
}

} // namespace

MissionLibrary::MissionLibrary(RotorLabClient *client, QObject *parent)
    : QObject(parent)
    , _client(client)
{
}

QString MissionLibrary::planDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + QStringLiteral("/rotorlab-missions");
}

void MissionLibrary::fetch(int pilotId)
{
    if (!_client) {
        return;
    }
    _client->getMissions(pilotId, [this](const HttpReply &r) {
        if (r.status != 200) {
            // Not an error worth shouting about: missions are an extra, and a
            // station with none must look identical to one that could not ask.
            _lastError = r.error;
            emit changed();
            return;
        }
        _missions = r.body.value(QStringLiteral("missions")).toArray();
        _loaded = true;
        _lastError.clear();
        emit changed();
    });
}

QVariantList MissionLibrary::missions() const
{
    QVariantList out;
    for (const QJsonValue &v : _missions) {
        const QJsonObject o = v.toObject();
        const int id = o.value(QStringLiteral("id")).toInt(-1);
        if (id < 0) {
            continue;           // without an id there is no plan to fetch
        }

        // The element shape is not pinned by the contract, so take the first
        // field that carries a human name and fall back to the id rather than
        // showing a blank row.
        QString name;
        for (const char *k : { "name", "title", "label", "reference" }) {
            name = o.value(QLatin1String(k)).toString().trimmed();
            if (!name.isEmpty()) {
                break;
            }
        }
        if (name.isEmpty()) {
            name = QStringLiteral("Mission %1").arg(id);
        }

        // The second line: what the mission is, and whose it is. The server
        // sends a waypoint count and an owner, which tell a crew far more
        // before they open it than a created/updated timestamp would.
        QStringList bits;
        const int wp = o.value(QStringLiteral("waypoints")).toInt(-1);
        if (wp >= 0) {
            bits << tr("%n waypoint(s)", nullptr, wp);
        }
        const QString owner = o.value(QStringLiteral("owner_name")).toString().trimmed();
        if (!owner.isEmpty()) {
            bits << owner;
        }
        // Only when it is NOT the ordinary case: an org-visible mission is the
        // norm and saying so on every row is noise.
        const QString vis = o.value(QStringLiteral("visibility")).toString().trimmed();
        if (!vis.isEmpty() && vis != QLatin1String("org")) {
            bits << vis;
        }

        QVariantMap m;
        m[QStringLiteral("id")]     = id;
        m[QStringLiteral("name")]   = name;
        m[QStringLiteral("detail")] = bits.join(QStringLiteral("  ·  "));
        out.append(m);
    }
    return out;
}

QString MissionLibrary::_nameFor(int missionId) const
{
    for (const QVariant &v : missions()) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("id")).toInt() == missionId) {
            return m.value(QStringLiteral("name")).toString();
        }
    }
    return QStringLiteral("Mission %1").arg(missionId);
}

void MissionLibrary::download(int missionId)
{
    if (!_client || _busy) {
        return;
    }
    _busy = true;
    _lastError.clear();
    emit changed();

    const QString name = _nameFor(missionId);
    _client->getMissionPlan(missionId, [this, missionId, name](const HttpReply &r) {
        _busy = false;

        if (r.status != 200) {
            _lastError = r.error.isEmpty()
                             ? tr("RotorLab could not send that mission (%1).").arg(r.status)
                             : r.error;
            emit changed();
            emit failed(_lastError);
            return;
        }
        // The endpoint answers with an ENVELOPE, not a bare file:
        //   { id, name, owner_id, plan: { fileType, version, groundStation, ... } }
        // The `.plan` QGC loads is the inner object. Writing the envelope out
        // gets "required keys are missing: groundStation", because QGC looks
        // for them at the top level and they are one level down.
        QJsonObject plan = r.body.value(QStringLiteral("plan")).toObject();
        if (plan.isEmpty() && r.body.contains(QStringLiteral("fileType"))) {
            // A bare plan, should the server ever send one unwrapped.
            plan = r.body;
        }
        if (plan.isEmpty()) {
            _lastError = tr("That mission came back without a plan in it.");
            emit changed();
            emit failed(_lastError);
            return;
        }

        QDir().mkpath(planDir());
        const QString path = planDir() + QStringLiteral("/")
                             + safeStem(name, missionId) + QStringLiteral(".plan");

        // Written from the JSON DOM, which is faithful: QJsonObject keeps every
        // key it parsed, including ones this build knows nothing about, so
        // nothing is lost on the way through. What must NEVER happen is a
        // round trip through QGC's own typed mission model - that keeps only
        // what it models, and the crew would fly the difference.
        const QByteArray bytes = QJsonDocument(plan).toJson(QJsonDocument::Indented);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || f.write(bytes) != bytes.size()) {
            _lastError = tr("Could not save the mission to this computer.");
            qCWarning(MissionLibraryLog) << "cannot write" << path << f.errorString();
            emit changed();
            emit failed(_lastError);
            return;
        }
        f.close();

        emit logMessage(tr("downloaded mission \"%1\"").arg(name));
        emit changed();
        emit planReady(path, name);
    });
}
