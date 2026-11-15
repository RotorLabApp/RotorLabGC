/****************************************************************************
 *
 * RotorLab GC RotorLab Connector - the calls, over the shared transport.
 *
 ****************************************************************************/

#include "RotorLabClient.h"

#include <QtCore/QStringList>
#include <QtCore/QUrl>

RotorLabClient::RotorLabClient(QObject *parent)
    : HttpClient(TlsPolicy::PublicCa, parent)
{
}

RotorLabClient::~RotorLabClient() = default;

void RotorLabClient::getVersion(Handler done)
{
    // The correct probe: unmetered, needs only the `status` scope, and costs
    // the customer nothing. Never verify a key with a filing call.
    get(QStringLiteral("/api/v1/version"), std::move(done));
}

void RotorLabClient::getUsage(Handler done)
{
    get(QStringLiteral("/api/v1/usage"), std::move(done));
}

void RotorLabClient::getPilots(Handler done)
{
    get(QStringLiteral("/api/v1/pilots"), std::move(done));
}

void RotorLabClient::getAircraft(Handler done)
{
    get(QStringLiteral("/api/v1/aircraft"), std::move(done));
}

void RotorLabClient::getChecklists(Handler done)
{
    get(QStringLiteral("/api/v1/checklists"), std::move(done));
}

void RotorLabClient::postFlight(const QJsonObject &body, Handler done)
{
    post(QStringLiteral("/api/v1/flights"), body, std::move(done), 30000);
}

void RotorLabClient::postChecklist(const QJsonObject &body, Handler done)
{
    post(QStringLiteral("/api/v1/checklists"), body, std::move(done), 30000);
}

void RotorLabClient::getMissions(int pilotId, Handler done)
{
    QString path = QStringLiteral("/api/v1/missions");
    if (pilotId >= 0) {
        path += QStringLiteral("?pilot_id=%1").arg(pilotId);
    }
    get(path, std::move(done));
}

void RotorLabClient::getSorties(const QString &day, int assetId, Handler done)
{
    QStringList query;
    if (!day.isEmpty()) {
        query << QStringLiteral("day=%1").arg(day);
    }
    if (assetId >= 0) {
        query << QStringLiteral("asset_id=%1").arg(assetId);
    }
    QString path = QStringLiteral("/api/v1/sorties");
    if (!query.isEmpty()) {
        path += QLatin1Char('?') + query.join(QLatin1Char('&'));
    }
    get(path, std::move(done));
}

void RotorLabClient::getMissionPlan(int missionId, Handler done)
{
    // A survey plan is bigger than a status blob, and this is a file rather
    // than a field, so give it longer than the 15 s default before giving up.
    get(QStringLiteral("/api/v1/missions/%1/plan").arg(missionId), std::move(done), 30000);
}

void RotorLabClient::getHealth(Handler done)
{
    get(QStringLiteral("/api/v1/health"), std::move(done));
}

void RotorLabClient::getWorkorders(Handler done)
{
    get(QStringLiteral("/api/v1/workorders"), std::move(done));
}

QNetworkReply *RotorLabClient::postFlightLog(int flightId, const QString &filePath,
                                            const QString &displayName,
                                            Handler done, Progress progress)
{
    // The numeric flight id, never the external_ref - the endpoint takes one
    // and not the other. `name` is display-only and is percent-encoded because
    // it is whatever the autopilot called the file.
    const QString path = QStringLiteral("/api/v1/flights/%1/log?name=%2")
        .arg(flightId)
        .arg(QString::fromUtf8(QUrl::toPercentEncoding(displayName)));
    return postFile(path, filePath, std::move(done), std::move(progress));
}
