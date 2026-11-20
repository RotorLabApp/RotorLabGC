/****************************************************************************
 *
 * RotorLab GC - G109: the calls LiveSession makes, as an interface.
 *
 * The one implementation used by GC and by the tests is FleetLiveTransport,
 * which goes through HttpClient (bearer header, server pin, loopback-only
 * plain http). The interface exists so the session's logic does not depend
 * on how the bytes are carried.
 *
 ****************************************************************************/

#pragma once

#include "net/HttpClient.h"

#include <QtCore/QJsonObject>
#include <QtCore/QObject>

class LiveTransport : public QObject
{
    Q_OBJECT

public:
    using Done = HttpClient::Handler;

    explicit LiveTransport(QObject *parent = nullptr) : QObject(parent) {}
    ~LiveTransport() override = default;

    /// Read the server, token and pin for a new live session (they are never
    /// cached across sessions). False when there is nothing to send to.
    virtual bool prepare() = 0;

    virtual void postJson(const QString &path, const QJsonObject &body, Done done, int timeoutMs) = 0;
    /// PATCH /live/<id> {"flight_ref"}.
    virtual void patchJson(const QString &path, const QJsonObject &body, Done done, int timeoutMs) = 0;
    virtual void putBytes(const QString &path, const QByteArray &body, Done done, int timeoutMs) = 0;
    virtual void postBytes(const QString &path, const QByteArray &body, Done done, int timeoutMs) = 0;
    virtual void deleteResource(const QString &path, Done done, int timeoutMs) = 0;

    /// Abort what is on the wire; its handlers never run.
    virtual void abortAll() = 0;

    /// Server name for the status line (host only, never a credential).
    virtual QString serverLabel() const = 0;
};
