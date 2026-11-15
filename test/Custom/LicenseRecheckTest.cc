#include "LicenseRecheckTest.h"

#include "LicenseManager.h"

#include <QtCore/QDateTime>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtTest/QTest>

namespace {

const QString kKey = QStringLiteral("TEST-0000-0000-0001");

bool noVehicle()  { return false; }
bool oneVehicle() { return true; }

bool recordStored()
{
    return QSettings().contains(QStringLiteral("svc/rk"));
}

} // namespace

void LicenseRecheckTest::_openSession(LicenseManager &lm)
{
    lm._key = kKey;
    lm._saveRecord(kKey);
    lm._setActivated(true);
    lm._inSession = true;
}

void LicenseRecheckTest::cleanup()
{
    QSettings s;
    s.remove(QStringLiteral("svc/rk"));
    s.sync();
    UnitTest::cleanup();
}

void LicenseRecheckTest::_inSessionProblemKeepsSession()
{
    const QStringList states = {QStringLiteral("expired"), QStringLiteral("revoked"),
                                QStringLiteral("wrong_device"), QStringLiteral("invalid")};
    for (const QString &state : states) {
        LicenseManager lm;
        _openSession(lm);
        const QByteArray body = QStringLiteral(R"({"state":"%1"})").arg(state).toUtf8();
        lm._handleReply(LicenseManager::Op::Verify, kKey, body, true, 200);

        QVERIFY2(lm.activated(), qPrintable(state));          // the gate stays down
        QVERIFY2(lm.attention(), qPrintable(state));          // and the state is noted
        QCOMPARE(lm._key, kKey);                              // nothing forgotten
        QVERIFY2(recordStored(), qPrintable(state));          // offline record kept
        QVERIFY2(lm.statusText().contains(QStringLiteral("keeps running for this session")),
                 qPrintable(lm.statusText()));
    }
}

void LicenseRecheckTest::_inSessionOfflineKeepsSession()
{
    LicenseManager lm;
    _openSession(lm);
    lm._handleReply(LicenseManager::Op::Verify, kKey, {}, false, 0);
    QVERIFY(lm.activated());
    QVERIFY(!lm.attention());
    QVERIFY(recordStored());
    QCOMPARE(lm.statusText(), QStringLiteral("Offline: using stored activation"));
}

void LicenseRecheckTest::_inSessionInconclusiveIsNotAProblem()
{
    LicenseManager lm;
    _openSession(lm);
    lm._handleReply(LicenseManager::Op::Verify, kKey, QByteArrayLiteral(R"({"state":"maintenance"})"), true, 503);
    QVERIFY(lm.activated());
    QVERIFY(!lm.attention());
    QVERIFY(lm.statusText().contains(QStringLiteral("using stored activation")));

    lm._handleReply(LicenseManager::Op::Verify, kKey, {}, true, 429);
    QVERIFY(lm.activated());
}

void LicenseRecheckTest::_inSessionValidClearsNote()
{
    LicenseManager lm;
    _openSession(lm);
    lm._handleReply(LicenseManager::Op::Verify, kKey, QByteArrayLiteral(R"({"state":"expired"})"), true, 200);
    QVERIFY(lm.attention());
    lm._handleReply(LicenseManager::Op::Verify, kKey, QByteArrayLiteral(R"({"state":"valid"})"), true, 200);
    QVERIFY(lm.activated());
    QVERIFY(!lm.attention());
    QVERIFY(lm.statusText().startsWith(QStringLiteral("License valid")));
}

void LicenseRecheckTest::_firstVerifyIsTheLaunchCheck()
{
    // At launch, before the session opens, the check still decides the gate,
    // as it always has: this is the start, not a flight.
    LicenseManager lm;
    lm._vehicleConnected = &noVehicle;
    lm._key = kKey;
    lm._saveRecord(kKey);
    lm._setActivated(true);
    lm._inSession = false;
    lm._handleReply(LicenseManager::Op::Verify, kKey, QByteArrayLiteral(R"({"state":"expired"})"), true, 200);
    QVERIFY(!lm.activated());
    QVERIFY(!recordStored());
    QVERIFY(lm._inSession);

    // An offline launch check opens the session too: later checks only note.
    LicenseManager offline;
    offline._key = kKey;
    offline._saveRecord(kKey);
    offline._setActivated(true);
    offline._inSession = false;
    offline._handleReply(LicenseManager::Op::Verify, kKey, {}, false, 0);
    QVERIFY(offline._inSession);
    offline._handleReply(LicenseManager::Op::Verify, kKey, QByteArrayLiteral(R"({"state":"revoked"})"), true, 200);
    QVERIFY(offline.activated());
    QVERIFY(offline.attention());
}

void LicenseRecheckTest::_activationOpensSession()
{
    LicenseManager lm;
    lm._inSession = false;
    lm._handleReply(LicenseManager::Op::Activate, kKey, QByteArrayLiteral(R"({"state":"activated"})"), true, 200);
    QVERIFY(lm.activated());
    QVERIFY(lm._inSession);
    lm._handleReply(LicenseManager::Op::Verify, kKey, QByteArrayLiteral(R"({"state":"expired"})"), true, 200);
    QVERIFY(lm.activated());
}

void LicenseRecheckTest::_timerEvery24Hours()
{
    LicenseManager lm;
    QVERIFY(lm._recheckTimer);
    QVERIFY(lm._recheckTimer->isActive());
    QCOMPARE(lm._recheckTimer->interval(), 24 * 60 * 60 * 1000);
}

void LicenseRecheckTest::_networkRegainRechecksSpaced()
{
    // No key, so nothing is sent; only the decision to re-check is tested.
    LicenseManager lm;
    lm._key.clear();

    lm._wasOnline = true;
    QVERIFY(!lm._networkReachable(true));     // was online already

    QVERIFY(!lm._networkReachable(false));    // went offline
    lm._lastAttemptMs = 0;
    QVERIFY(lm._networkReachable(true));      // came back: re-check

    lm._networkReachable(false);
    lm._lastAttemptMs = QDateTime::currentMSecsSinceEpoch() - 60 * 1000;
    QVERIFY(!lm._networkReachable(true));     // a minute after the last one: wait

    lm._networkReachable(false);
    lm._lastAttemptMs = QDateTime::currentMSecsSinceEpoch() - LicenseManager::kRegainMinSpacingMs - 1000;
    QVERIFY(lm._networkReachable(true));
}

void LicenseRecheckTest::_offlineDeactivateNamesRelease()
{
    LicenseManager lm;
    _openSession(lm);
    lm._handleReply(LicenseManager::Op::Deactivate, kKey, {}, false, 0);
    QVERIFY(lm.activated());
    QVERIFY(recordStored());
    QVERIFY(lm.statusText().contains(
        QStringLiteral("an organization admin can release this key in RotorLab: "
                       "Organization, Billing, Product licenses, then Release")));
}

void LicenseRecheckTest::_lateLaunchReplyWhileConnectedOnlyNotes()
{
    // F3: GC restarted mid-flight, or the launch reply was slow, and a vehicle
    // is connected when it lands: note only, the gate stays down.
    LicenseManager lm;
    lm._vehicleConnected = &oneVehicle;
    lm._key = kKey;
    lm._saveRecord(kKey);
    lm._setActivated(true);
    lm._inSession = false;
    lm._handleReply(LicenseManager::Op::Verify, kKey, QByteArrayLiteral(R"({"state":"revoked"})"), true, 200);
    QVERIFY(lm.activated());
    QVERIFY(lm.attention());
    QVERIFY(recordStored());
    QVERIFY(lm._inSession);
}
