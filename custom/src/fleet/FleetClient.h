/****************************************************************************
 *
 * RotorLab GC Fleet Connector - system-trust HTTPS transport.
 *
 ****************************************************************************/

#pragma once

#include "net/HttpClient.h"

using FleetReply = HttpReply;

class FleetClient : public HttpClient
{
    Q_OBJECT

public:
    explicit FleetClient(QObject *parent = nullptr);
    ~FleetClient() override;
};
