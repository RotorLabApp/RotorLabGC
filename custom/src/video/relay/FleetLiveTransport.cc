/****************************************************************************
 *
 * RotorLab GC - G109: live relay transport over the Fleet enrollment.
 *
 ****************************************************************************/

#include "FleetLiveTransport.h"

#include "fleet/FleetClient.h"
#include "fleet/FleetStore.h"

#include <QtCore/QUrl>

namespace {
const QByteArray kOctet = QByteArrayLiteral("application/octet-stream");
}

FleetLiveTransport::FleetLiveTransport(QObject *parent)
    : LiveTransport(parent)
    , _client(new FleetClient(this))
{
}

FleetLiveTransport::~FleetLiveTransport()
{
    abortAll();
}

bool FleetLiveTransport::acceptableBaseUrl(const QString &base)
{
    if (base.isEmpty()) {
        return false;
    }
    // normalizeBaseUrl keeps http only for localhost, 127.0.0.1 and ::1, so a
    // stored address it would change is not one this transport sends to.
    return HttpClient::normalizeBaseUrl(base, true) == base;
}

bool FleetLiveTransport::prepare()
{
    // LiveSession has already aborted what belonged to the last session; a
    // best-effort DELETE for it may still be on the wire and is left to finish.
    const QString base = FleetStore::baseUrl();
    const QString token = FleetStore::token();
    if (!acceptableBaseUrl(base) || token.isEmpty()) {
        _client->setBaseUrl({});
        _client->setToken({});
        _label.clear();
        return false;
    }
    _client->setBaseUrl(base);
    _client->setToken(token);
    _client->setPinnedSpkiSha256(FleetStore::serverPin());
    _label = QUrl(base).host();
    return true;
}

void FleetLiveTransport::postJson(const QString &path, const QJsonObject &body, Done done, int timeoutMs)
{
    _client->post(path, body, std::move(done), timeoutMs);
}

void FleetLiveTransport::patchJson(const QString &path, const QJsonObject &body, Done done, int timeoutMs)
{
    _client->patchJson(path, body, std::move(done), timeoutMs);
}

void FleetLiveTransport::putBytes(const QString &path, const QByteArray &body, Done done, int timeoutMs)
{
    _client->putBytes(path, body, kOctet, {}, std::move(done), timeoutMs);
}

void FleetLiveTransport::postBytes(const QString &path, const QByteArray &body, Done done, int timeoutMs)
{
    _client->postBytes(path, body, kOctet, {}, std::move(done), timeoutMs);
}

bool FleetLiveTransport::canDelete()
{
    return true;
}

void FleetLiveTransport::deleteResource(const QString &path, Done done, int timeoutMs)
{
    _client->deleteResource(path, std::move(done), timeoutMs);
}

void FleetLiveTransport::abortAll()
{
    // Every reply already on the wire belongs to a session that is over (or a
    // station that was revoked): abort it on the wire, and its handler never runs.
    _client->abortAll();
}
