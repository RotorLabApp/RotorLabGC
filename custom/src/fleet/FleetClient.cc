/****************************************************************************
 *
 * RotorLab GC Fleet Connector - system-trust HTTPS transport.
 *
 ****************************************************************************/

#include "FleetClient.h"

FleetClient::FleetClient(QObject *parent)
    : HttpClient(TlsPolicy::PublicCa, parent)
{
}

FleetClient::~FleetClient() = default;
