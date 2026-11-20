#pragma once

#include "UnitTest.h"

class LicenseManager;

/// The signed offline activation record (G89; license/LicenseRecord.h,
/// license/Ed25519.h, license/LicenseManager.h).
///
/// Ed25519 (Monocypher 4.0.3 behind GC's wrapper) against RFC 8032, with
/// flipped-bit and non-canonical S vectors, and against the RotorLab server's
/// published test fixture; the record checks (signature, key, device, product, expiry,
/// issued in the future, clock rollback, shape); the start-up gate fed from
/// the settings file; the migration from the 5.2.0 unsigned record; and the
/// rule that GC never locks while a vehicle is connected. Every record is
/// signed with a key pair generated in the test. Nothing is sent to a
/// license server: replies are fed straight in.
class LicenseRecordTest : public UnitTest
{
    Q_OBJECT

protected slots:
    void init() override;
    void cleanup() override;

private slots:
    // Ed25519
    void _ed25519Rfc8032Vector1();
    void _ed25519Rfc8032Vector1FlippedBits();
    void _ed25519Rfc8032Vector1NonCanonicalS();
    void _ed25519MatchesRotorLabFixture();
    void _ed25519RefusesNonCanonicalS();

    // The record, pure checks
    void _validRecord();
    void _badSignature();
    void _unknownSigningKey();
    void _wrongDevice();
    void _wrongProduct();
    void _wrongKey();
    void _expired();
    void _futureIssued();
    void _clockRollback();
    void _malformed();
    void _pinnedKeysFromHex();
    void _keyHashNormalizes();

    // RotorLab's own record fixtures (server 3.7.0 and G98)
    void _rotorlabServerFixtureRecord();
    void _g98Fixtures();

    // The gate at start, from the settings file
    void _startValidRecordOpens();
    void _startEditedRecordStaysClosed();
    void _startWrongDeviceStaysClosed();
    void _startExpiredAsksForConnection();
    void _startFutureIssuedRefused();
    void _startClockRollbackRefused();

    // Online replies
    void _activateNeedsSignedRecord();
    void _verifyRefreshesRecordAndResetsClockMark();

    // Migration from 5.2.0
    void _legacyRecordNeverOpensOffline();
    void _legacyRecordNotLiftedByVehicle();
    void _legacyKeySentOnline();
    void _legacyGateOpensOnSignedReply();
    void _legacyRecordReplacedBySignedRecord();
    void _legacyRecordNotHonoredAfterSignedRecord();

    // Never stop a flight
    void _expiredAtStartWithVehicleOpens();
    void _vehicleConnectingLiftsTimeOnlyGate();
    void _forgedRecordNotLiftedByVehicle();
    void _noLockDuringConnectedSession();

    // The gate's key field
    void _gateKeyMaskedAfterDeactivate();

private:
    static void _clearSettings();
    static void _storeEnvelope(const QByteArray &envelope);
    static void _storeLegacy(const QString &key, const QString &deviceId);
    static QString _deviceId();
};
