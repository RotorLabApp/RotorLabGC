#pragma once

#include "UnitTest.h"

/// A76 on the wire: TakManager::enroll() against a local TLS server on
/// 127.0.0.1 with a self-signed certificate made by the test. No HTTP bytes
/// (no request line, no Authorization header) may reach the server before
/// the operator accepts the server's fingerprint; after accepting, the
/// Authorization header arrives. Needs openssl to make the certificate; the
/// test skips without it.
class TakEnrollWireTest : public UnitTest
{
    Q_OBJECT

private slots:
    void init() override;
    void cleanup() override;

    void _noCredentialsBeforeAccept();
};
