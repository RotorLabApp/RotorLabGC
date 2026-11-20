#pragma once

#include "UnitTest.h"

/// G19: the Fleet Connector enrolls with RotorLab Server 2.0 as its published
/// API states (docs/api/openapi.json, POST /api/gc/v1/enroll).
///
/// The code is 16 case-sensitive characters of A-Z, a-z, 0-9, '-' and '_',
/// sent exactly as typed. An optional ".<pin>" (lowercase hex SHA-256 of the
/// server certificate's SubjectPublicKeyInfo) is checked against the TLS peer
/// before any request byte is written, and kept for later connections.
///
/// The wire cases run against local servers on 127.0.0.1 made by the test;
/// the TLS ones need openssl to make a certificate and skip without it.
///
/// _liveEnroll runs only when RLGC_FLEET_LIVE_URL and RLGC_FLEET_LIVE_CODE_FILE
/// are set. It reads the code from that file, never prints it or the token,
/// and writes the HTTP status and station_id to RLGC_FLEET_LIVE_RESULT.
class FleetEnrollTest : public UnitTest
{
    Q_OBJECT

private slots:
    void init() override;
    void cleanup() override;

    void _parseKeepsCodeExactly();
    void _errorTexts();
    void _openapiExampleReachesBodyUnchanged();
    void _spkiMatchesOpenssl();
    void _codeDotPinSendsOnlyCodeAndKeepsPin();
    void _wrongPinRefusesBeforeSending();
    void _pinOverPlainHttpRefused();
    void _liveEnroll();

    /// G40: joining a Fleet Server closes a live TAK link and turns
    /// "Commands to aircraft on" off; the RotorLab Connector taking over
    /// does not.
    void _takRouteEdge();
    void _joinClosesTakLink();
};
