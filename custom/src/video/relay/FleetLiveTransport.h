/****************************************************************************
 *
 * RotorLab GC - G109: LiveSession's calls over the station's Fleet enrollment.
 *
 * Its own FleetClient (its own connections, so a live fragment never waits
 * behind a log upload and the Fleet Connector's reset() never drops a relay
 * reply), configured from FleetStore at the start of every live session:
 * baseUrl(), token() and serverPin(). The token is set on the client and goes
 * out only in HttpClient's Authorization header; this class never copies it
 * anywhere else and never logs it.
 *
 ****************************************************************************/

#pragma once

#include "LiveTransport.h"
#include "RelayTypes.h"

class FleetClient;

class FleetLiveTransport : public LiveTransport
{
    Q_OBJECT

public:
    explicit FleetLiveTransport(QObject *parent = nullptr);
    ~FleetLiveTransport() override;

    bool prepare() override;
    void postJson(const QString &path, const QJsonObject &body, Done done, int timeoutMs) override;
    void patchJson(const QString &path, const QJsonObject &body, Done done, int timeoutMs) override;
    void putBytes(const QString &path, const QByteArray &body, Done done, int timeoutMs) override;
    void postBytes(const QString &path, const QByteArray &body, Done done, int timeoutMs) override;
    void deleteResource(const QString &path, Done done, int timeoutMs) override;
    void abortAll() override;
    QString serverLabel() const override { return _label; }

    /// True when `base` is https, or http to this machine only.
    static bool acceptableBaseUrl(const QString &base);
    /// True when this build's HttpClient can send DELETE /live/<id>.
    static bool canDelete();

private:
    FleetClient *_client = nullptr;
    QString _label;
};
