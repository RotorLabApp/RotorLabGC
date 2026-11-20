#include "LicenseLinkGateTest.h"
#include "LicenseTestRecords.h"

#include "CustomPlugin.h"
#include "Joystick.h"
#include "LicenseManager.h"
#include "LicenseRecord.h"
#include "LinkManager.h"
#include "MockConfiguration.h"
#include "MockLink.h"
#include "MultiVehicleManager.h"
#include "Vehicle.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QScopedPointer>
#include <QtCore/QSettings>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

using namespace LicenseTestRecords;

namespace {

const QString kKey = QStringLiteral("TEST-0000-0000-0042");
constexpr qint64 kDay = 24 * 60 * 60;

bool noVehicle()  { return false; }
bool oneVehicle() { return true; }

int g_takDisconnects = 0;
void countTak() { ++g_takDisconnects; }

qint64 realNow() { return QDateTime::currentSecsSinceEpoch(); }

bool stored(const char *key)
{
    return QSettings().contains(QLatin1String(key));
}

void storeEnvelope(const QByteArray &envelope)
{
    QSettings s;
    s.setValue(QStringLiteral("svc/sr"), envelope);
    s.sync();
}

/// The 5.2.0 record exactly as 5.2.0 wrote it.
void storeLegacy(const QString &key, const QString &deviceId)
{
    const QByteArray plain = QJsonDocument(QJsonObject{
        {QStringLiteral("k"), key}, {QStringLiteral("d"), deviceId}}).toJson(QJsonDocument::Compact);
    const QByteArray ks = QCryptographicHash::hash(deviceId.toUtf8(), QCryptographicHash::Sha256);
    QByteArray blob(plain.size(), '\0');
    for (int i = 0; i < plain.size(); ++i) {
        blob[i] = plain[i] ^ ks[i % ks.size()];
    }
    QSettings s;
    s.setValue(QStringLiteral("svc/rk"), blob.toBase64());
    s.sync();
}

/// A mock vehicle link configuration, auto-connect, PX4 quad.
SharedLinkConfigurationPtr addAutoConnectMock(const QString &name)
{
    MockConfiguration *const mock = new MockConfiguration(name);
    mock->setFirmwareType(MAV_AUTOPILOT_PX4);
    mock->setVehicleType(MAV_TYPE_QUADROTOR);
    mock->setDynamic(true);
    mock->setAutoConnect(true);
    return LinkManager::instance()->addConfiguration(mock);
}

class FakeJoystick : public Joystick
{
public:
    FakeJoystick() : Joystick(QStringLiteral("RotorLab gate test joystick"), 4, 4, 0) {}

private:
    bool _open() final { return true; }
    void _close() final {}
    bool _update() final { return true; }
    bool _getButton(int) const final { return false; }
    int  _getAxis(int) const final { return 0; }
    bool _getHat(int, int) const final { return false; }
};

} // namespace

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

void LicenseLinkGateTest::_clearSettings()
{
    QSettings s;
    for (const char *k : {"svc/sr", "svc/lk", "svc/rk", "svc/hw", "svc/mg", "svc/sv"}) {
        s.remove(QLatin1String(k));
    }
    s.sync();
}

/// A license manager for a test: no vehicle, TAK counted, the test's signer.
void LicenseLinkGateTest::_prepare(LicenseManager &lm, const LicenseTestRecords::Signer &s, qint64 now)
{
    lm._vehicleConnected = &noVehicle;
    lm._disconnectTak = &countTak;
    lm._pinned = {s.pinned()};
    lm._nowOverride = now;
}

void LicenseLinkGateTest::init()
{
    UnitTest::init();
    _clearSettings();
    g_takDisconnects = 0;
    LinkManager::instance()->setLinksAllowed(true);
}

void LicenseLinkGateTest::cleanup()
{
    _clearSettings();
    LinkManager::instance()->setLinksAllowed(true);
    UnitTest::cleanup();
}

// ---------------------------------------------------------------------------
// The predicate
// ---------------------------------------------------------------------------

void LicenseLinkGateTest::_predicateNoRecord()
{
    LicenseManager lm;
    lm._vehicleConnected = &noVehicle;
    lm._disconnectTak = &countTak;
    lm._loadRecord();
    QVERIFY(!lm.activated());
    QVERIFY(!lm.linksAllowed());
    QVERIFY(!lm.property("linksAllowed").toBool());

    // A vehicle "appearing" (it cannot, with no link) changes nothing.
    lm._vehicleConnected = &oneVehicle;
    lm._vehicleAppeared();
    QVERIFY(!lm.linksAllowed());
}

void LicenseLinkGateTest::_predicateLegacyRecord()
{
    LicenseManager lm;
    lm._vehicleConnected = &noVehicle;
    lm._disconnectTak = &countTak;
    storeLegacy(kKey, lm._deviceId);
    lm._loadRecord();
    QVERIFY(!lm.activated());
    QVERIFY(lm._legacyGate);
    QCOMPARE(lm._key, kKey);
    QVERIFY(!lm.linksAllowed());

    lm._vehicleConnected = &oneVehicle;
    lm._vehicleAppeared();
    QVERIFY(!lm.linksAllowed());

    // The online check brings a signed record: links are allowed in session.
    const Signer s = makeSigner();
    const qint64 now = realNow();
    lm._pinned = {s.pinned()};
    lm._nowOverride = now;
    lm._handleReply(LicenseManager::Op::Verify, kKey,
                    reply(QStringLiteral("valid"),
                          envelope(s, recordObject(s, lm._deviceId, kKey, now, now + 14 * kDay))),
                    true, 200);
    QVERIFY(lm.activated());
    QVERIFY(lm.linksAllowed());
}

void LicenseLinkGateTest::_predicateUnverifiableRecord()
{
    const Signer s = makeSigner();
    const Signer attacker = makeSigner();
    const qint64 now = realNow();
    LicenseManager lm;
    _prepare(lm, s, now);
    lm._storeKey(kKey);
    const QString dev = lm._deviceId;

    // Signed with a key GC does not trust, inside its window.
    storeEnvelope(envelopeBytes(envelope(attacker, recordObject(attacker, dev, kKey, now, now + 30 * kDay))));
    lm._loadRecord();
    QVERIFY(!lm.activated());
    QVERIFY(!lm.linksAllowed());

    // The same, but run out of time: still not genuine, so no lapsed opening.
    storeEnvelope(envelopeBytes(envelope(attacker, recordObject(attacker, dev, kKey, now - 15 * kDay, now - kDay))));
    lm._loadRecord();
    QVERIFY(!lm._timeLapsed);
    QVERIFY(!lm.linksAllowed());

    // A genuine record whose offline_until was edited.
    const QByteArray bytes = bytesOf(recordObject(s, dev, kKey, now - 20 * kDay, now - 6 * kDay));
    QByteArray edited = bytes;
    edited.replace(iso(now - 6 * kDay).toLatin1(), iso(now + 365 * kDay).toLatin1());
    storeEnvelope(envelopeBytes(envelope(s, edited, signRecord(s, bytes))));
    lm._loadRecord();
    QVERIFY(!lm.linksAllowed());

    // A genuine record for another device.
    storeEnvelope(envelopeBytes(envelope(s, recordObject(s, QStringLiteral("ffffffffffffffffffffffffffffffff"),
                                                         kKey, now, now + 14 * kDay))));
    lm._loadRecord();
    QVERIFY(!lm.linksAllowed());

    // Garbage where the record goes.
    storeEnvelope(QByteArrayLiteral("not a record"));
    lm._loadRecord();
    QVERIFY(!lm.linksAllowed());
}

void LicenseLinkGateTest::_predicateActivated()
{
    const Signer s = makeSigner();
    const qint64 now = realNow();

    // From the stored record at start.
    {
        LicenseManager lm;
        _prepare(lm, s, now);
        storeEnvelope(envelopeBytes(envelope(s, recordObject(s, lm._deviceId, kKey, now - 3600, now + 14 * kDay))));
        lm._storeKey(kKey);
        lm._loadRecord();
        QVERIFY(lm.activated());
        QVERIFY(lm.linksAllowed());
    }

    // From a key typed at the gate.
    _clearSettings();
    {
        LicenseManager lm;
        _prepare(lm, s, now);
        lm._loadRecord();
        QVERIFY(!lm.linksAllowed());
        lm._handleReply(LicenseManager::Op::Activate, kKey,
                        reply(QStringLiteral("activated"),
                              envelope(s, recordObject(s, lm._deviceId, kKey, now, now + 14 * kDay))),
                        true, 200);
        QVERIFY(lm.activated());
        QVERIFY(lm.linksAllowed());

        // A good word with no signed record does not allow links.
        LicenseManager other;
        _prepare(other, s, now);
        _clearSettings();
        other._loadRecord();
        other._handleReply(LicenseManager::Op::Activate, kKey, reply(QStringLiteral("activated"), {}), true, 200);
        QVERIFY(!other.linksAllowed());
    }
}

void LicenseLinkGateTest::_predicateLapsedGenuine()
{
    // The existing path must keep working: a genuine signed record whose
    // offline period ran out keeps the gate up, but its links may start, so
    // a vehicle that connects opens GC for the session.
    const Signer s = makeSigner();
    const qint64 now = realNow();
    LicenseManager lm;
    _prepare(lm, s, now);
    lm._storeKey(kKey);
    storeEnvelope(envelopeBytes(envelope(s, recordObject(s, lm._deviceId, kKey, now - 15 * kDay, now - kDay))));
    lm._loadRecord();
    QVERIFY(!lm.activated());
    QVERIFY(lm._timeLapsed);
    QVERIFY(lm.linksAllowed());

    lm._vehicleConnected = &oneVehicle;
    lm._vehicleAppeared();
    QVERIFY(lm.activated());
    QVERIFY(lm.attention());
    QVERIFY(lm.linksAllowed());

    // The clock set back below what GC has seen: also a time problem.
    LicenseManager clock;
    _prepare(clock, s, now);
    storeEnvelope(envelopeBytes(envelope(s, recordObject(s, clock._deviceId, kKey, now - 3600, now + 14 * kDay))));
    {
        QSettings st;
        st.setValue(QStringLiteral("svc/hw"), now + 10 * kDay);
        st.sync();
    }
    clock._loadRecord();
    QVERIFY(!clock.activated());
    QVERIFY(clock._timeLapsed);
    QVERIFY(clock.linksAllowed());

    // At a lapsed start the launch check says the license expired, with no
    // vehicle connected: the record goes, and so do the links.
    QSignalSpy spy(&clock, &LicenseManager::linksAllowedChanged);
    clock._inSession = false;
    clock._handleReply(LicenseManager::Op::Verify, kKey, QByteArrayLiteral(R"({"state":"expired"})"), true, 200);
    QVERIFY(!clock.linksAllowed());
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toBool(), false);
}

// G-n1: the predicate's limits in each direction. A RotorLab-signed record
// for another device opens nothing, in its window or run out of time: the
// device check comes before the time checks, so it is never a lapsed opening.
void LicenseLinkGateTest::_predicateOtherDevice()
{
    const Signer s = makeSigner();
    const qint64 now = realNow();
    const QString otherDevice = QStringLiteral("ffffffffffffffffffffffffffffffff");
    LicenseManager lm;
    _prepare(lm, s, now);
    lm._storeKey(kKey);
    QVERIFY(lm._deviceId != otherDevice);

    // Inside its window.
    storeEnvelope(envelopeBytes(envelope(s, recordObject(s, otherDevice, kKey, now - 3600, now + 14 * kDay))));
    lm._loadRecord();
    QVERIFY(!lm.activated());
    QVERIFY(!lm._timeLapsed);
    QVERIFY(!lm.linksAllowed());

    // Run out of time.
    storeEnvelope(envelopeBytes(envelope(s, recordObject(s, otherDevice, kKey, now - 15 * kDay, now - kDay))));
    lm._loadRecord();
    QVERIFY(!lm._timeLapsed);
    QVERIFY(!lm.linksAllowed());

    // With the clock set back below what GC has seen.
    storeEnvelope(envelopeBytes(envelope(s, recordObject(s, otherDevice, kKey, now - 3600, now + 14 * kDay))));
    {
        QSettings st;
        st.setValue(QStringLiteral("svc/hw"), now + 10 * kDay);
        st.sync();
    }
    lm._loadRecord();
    QVERIFY(!lm._timeLapsed);
    QVERIFY(!lm.linksAllowed());

    // A vehicle "appearing" (it cannot, with no link) changes nothing.
    lm._vehicleConnected = &oneVehicle;
    lm._vehicleAppeared();
    QVERIFY(!lm.activated());
    QVERIFY(!lm.linksAllowed());
}

// A record for this device under the wrong license key (key_sha256 is not
// the stored key's), or signed under a key GC does not pin (unknown kid):
// no links, in its window or run out of time.
void LicenseLinkGateTest::_predicateWrongOrUnknownKey()
{
    const Signer s = makeSigner();
    const Signer unknown = makeSigner();
    const qint64 now = realNow();
    const QString otherKey = QStringLiteral("TEST-0000-0000-0099");
    LicenseManager lm;
    _prepare(lm, s, now);
    lm._storeKey(kKey);
    const QString dev = lm._deviceId;

    // Wrong license key, RotorLab-signed, inside its window.
    storeEnvelope(envelopeBytes(envelope(s, recordObject(s, dev, otherKey, now - 3600, now + 14 * kDay))));
    lm._loadRecord();
    QVERIFY(!lm.activated());
    QVERIFY(!lm._timeLapsed);
    QVERIFY(!lm.linksAllowed());

    // Wrong license key, run out of time: WrongKey, not a time problem.
    storeEnvelope(envelopeBytes(envelope(s, recordObject(s, dev, otherKey, now - 15 * kDay, now - kDay))));
    lm._loadRecord();
    QVERIFY(!lm._timeLapsed);
    QVERIFY(!lm.linksAllowed());

    // Unknown signing key, inside its window.
    storeEnvelope(envelopeBytes(envelope(unknown, recordObject(unknown, dev, kKey, now - 3600, now + 14 * kDay))));
    lm._loadRecord();
    QVERIFY(!lm.activated());
    QVERIFY(!lm._timeLapsed);
    QVERIFY(!lm.linksAllowed());

    // Unknown signing key, run out of time.
    storeEnvelope(envelopeBytes(envelope(unknown, recordObject(unknown, dev, kKey, now - 15 * kDay, now - kDay))));
    lm._loadRecord();
    QVERIFY(!lm._timeLapsed);
    QVERIFY(!lm.linksAllowed());

    // An unknown kid in the envelope over a genuine signature is still unknown.
    const QByteArray bytes = bytesOf(recordObject(s, dev, kKey, now - 3600, now + 14 * kDay));
    QJsonObject env = envelope(s, bytes, signRecord(s, bytes));
    env.insert(QStringLiteral("kid"), unknown.kid);
    storeEnvelope(envelopeBytes(env));
    lm._loadRecord();
    QVERIFY(!lm._timeLapsed);
    QVERIFY(!lm.linksAllowed());

    lm._vehicleConnected = &oneVehicle;
    lm._vehicleAppeared();
    QVERIFY(!lm.activated());
    QVERIFY(!lm.linksAllowed());
}

// A genuine record (RotorLab-signed, this device, this key) with the clock
// set back: a time problem, so links run through _timeLapsed for the vehicle
// lift, and a vehicle that connects opens GC for the session.
void LicenseLinkGateTest::_predicateClockBackGenuine()
{
    const Signer s = makeSigner();
    const qint64 now = realNow();

    // Below the highest time GC has seen.
    {
        LicenseManager lm;
        _prepare(lm, s, now);
        lm._storeKey(kKey);
        storeEnvelope(envelopeBytes(envelope(s, recordObject(s, lm._deviceId, kKey, now - 3600, now + 14 * kDay))));
        {
            QSettings st;
            st.setValue(QStringLiteral("svc/hw"), now + 10 * kDay);
            st.sync();
        }
        lm._loadRecord();
        QVERIFY(!lm.activated());
        QVERIFY(lm._timeLapsed);
        QVERIFY(lm.linksAllowed());

        lm._vehicleConnected = &oneVehicle;
        lm._vehicleAppeared();
        QVERIFY(lm.activated());
        QVERIFY(lm.linksAllowed());
    }

    // Before the record was issued.
    _clearSettings();
    {
        LicenseManager lm;
        _prepare(lm, s, now);
        lm._storeKey(kKey);
        storeEnvelope(envelopeBytes(envelope(s, recordObject(s, lm._deviceId, kKey, now + 2 * kDay, now + 16 * kDay))));
        lm._loadRecord();
        QVERIFY(!lm.activated());
        QVERIFY(lm._timeLapsed);
        QVERIFY(lm.linksAllowed());
    }
}

void LicenseLinkGateTest::_predicateSignalAndTak()
{
    const Signer s = makeSigner();
    const qint64 now = realNow();
    LicenseManager lm;
    _prepare(lm, s, now);
    lm._loadRecord();
    QVERIFY(!lm.linksAllowed());
    QSignalSpy spy(&lm, &LicenseManager::linksAllowedChanged);

    lm._handleReply(LicenseManager::Op::Activate, kKey,
                    reply(QStringLiteral("activated"),
                          envelope(s, recordObject(s, lm._deviceId, kKey, now, now + 14 * kDay))),
                    true, 200);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toBool(), true);
    QCOMPARE(g_takDisconnects, 0);

    // A re-check that only changes the status does not signal again.
    lm._handleReply(LicenseManager::Op::Verify, kKey, {}, false, 0);
    QCOMPARE(spy.count(), 1);

    // The user deactivates with no vehicle connected: links go, TAK closes.
    lm._handleReply(LicenseManager::Op::Deactivate, kKey, QByteArrayLiteral(R"({"state":"deactivated"})"), true, 200);
    QVERIFY(!lm.activated());
    QVERIFY(!lm.linksAllowed());
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).toBool(), false);
    QCOMPARE(g_takDisconnects, 1);
}

// ---------------------------------------------------------------------------
// LinkManager
// ---------------------------------------------------------------------------

void LicenseLinkGateTest::_linkManagerRefusesWhileNotAllowed()
{
    LinkManager *const links = LinkManager::instance();
    QSignalSpy refused(links, &LinkManager::linkRefused);
    QSignalSpy changed(links, &LinkManager::linksAllowedChanged);
    links->setLinksAllowed(false, LicenseManager::linkRefusedText());
    QVERIFY(!links->linksAllowed());
    QCOMPARE(changed.count(), 1);

    // A manual connect is refused, with the plain message.
    MockConfiguration *const mock = new MockConfiguration(QStringLiteral("Gate manual"));
    mock->setDynamic(true);
    SharedLinkConfigurationPtr config = links->addConfiguration(mock);
    QVERIFY(!links->createConnectedLink(config));
    QVERIFY(!config->link());
    QCOMPARE(refused.count(), 1);
    QCOMPARE(refused.at(0).at(0).toString(), QStringLiteral("Activate RotorLab GC to connect to a vehicle."));

    // The QML entry point too.
    links->createConnectedLink(config.get());
    QVERIFY(!config->link());
    QCOMPARE(refused.count(), 2);

    // The MockLink helpers go through the same path.
    QVERIFY(!MockLink::startPX4MockLink(false));
    QCOMPARE(refused.count(), 3);

    QTest::qWait(200);
    QCOMPARE(MultiVehicleManager::instance()->vehicles()->count(), 0);
    links->removeConfiguration(config.get());
}

void LicenseLinkGateTest::_linkManagerStartsAutoConnectWhenAllowed()
{
    LinkManager *const links = LinkManager::instance();
    links->setLinksAllowed(false, LicenseManager::linkRefusedText());
    SharedLinkConfigurationPtr config = addAutoConnectMock(QStringLiteral("Gate auto-connect"));

    // Held while not allowed, refused quietly (no message at start).
    QSignalSpy refused(links, &LinkManager::linkRefused);
    links->startAutoConnectedLinks();
    QVERIFY(!config->link());
    QCOMPARE(refused.count(), 0);

    // Allowed: the auto-connect link starts and the vehicle comes up.
    QSignalSpy spyVehicle(MultiVehicleManager::instance(), &MultiVehicleManager::activeVehicleChanged);
    links->setLinksAllowed(true);
    QVERIFY(config->link());
    QVERIFY(spyVehicle.wait(10000));
    QVERIFY(MultiVehicleManager::instance()->activeVehicle());

    // Started once: a second start does not open a second link.
    LinkInterface *const first = config->link();
    links->startAutoConnectedLinks();
    QCOMPARE(config->link(), first);

    // Not allowed again (no vehicle may stay behind the gate): every link
    // closes and the vehicle goes.
    spyVehicle.clear();
    links->setLinksAllowed(false);
    QVERIFY(spyVehicle.wait(10000));
    QVERIFY(!MultiVehicleManager::instance()->activeVehicle());
    QTRY_VERIFY_WITH_TIMEOUT(!config->link(), 5000);

    links->removeConfiguration(config.get());
}

void LicenseLinkGateTest::_licenseDrivesLinkManager()
{
    // The real wiring (CustomPlugin::wireLinkGate), with a test license.
    const Signer s = makeSigner();
    const qint64 now = realNow();
    LicenseManager lm;
    _prepare(lm, s, now);
    lm._vehicleConnected = &LicenseManager::_anyVehicleConnected;   // the real one
    lm._loadRecord();
    QVERIFY(!lm.linksAllowed());

    LinkManager *const links = LinkManager::instance();
    CustomPlugin::wireLinkGate(&lm, links);
    QVERIFY(!links->linksAllowed());

    SharedLinkConfigurationPtr config = addAutoConnectMock(QStringLiteral("Gate wired"));
    links->startAutoConnectedLinks();
    QVERIFY(!config->link());

    // Activation by key: links are allowed and the auto-connect link starts.
    QSignalSpy spyVehicle(MultiVehicleManager::instance(), &MultiVehicleManager::activeVehicleChanged);
    lm._handleReply(LicenseManager::Op::Activate, kKey,
                    reply(QStringLiteral("activated"),
                          envelope(s, recordObject(s, lm._deviceId, kKey, now, now + 14 * kDay))),
                    true, 200);
    QVERIFY(lm.activated());
    QVERIFY(links->linksAllowed());
    QVERIFY(config->link());
    QVERIFY(spyVehicle.wait(10000));

    // Deactivate is refused while the vehicle is connected; nothing is sent.
    lm.deactivate();
    QVERIFY(!lm.busy());
    QVERIFY(lm.activated());
    QCOMPARE(lm.statusText(), QStringLiteral("Deactivate this device is off while a vehicle is connected."));
    QVERIFY(links->linksAllowed());

    // The user disconnects the vehicle, then the deactivate reply arrives.
    spyVehicle.clear();
    links->removeConfiguration(config.get());
    QVERIFY(spyVehicle.wait(10000));
    QCOMPARE(MultiVehicleManager::instance()->vehicles()->count(), 0);
    lm._handleReply(LicenseManager::Op::Deactivate, kKey, QByteArrayLiteral(R"({"state":"deactivated"})"), true, 200);
    QVERIFY(!lm.activated());
    QVERIFY(!links->linksAllowed());
    QCOMPARE(g_takDisconnects, 1);

    QObject::disconnect(&lm, nullptr, links, nullptr);
}

// ---------------------------------------------------------------------------
// Joystick
// ---------------------------------------------------------------------------

void LicenseLinkGateTest::_joystickSendsNothingWhileNotAllowed()
{
    Vehicle *const vehicle = MultiVehicleManager::instance()->offlineEditingVehicle();
    QVERIFY(vehicle);
    FakeJoystick js;
    js._activeVehicle = vehicle;
    vehicle->setJoystickEnabled(true);
    QSignalSpy armed(&js, &Joystick::setArmed);

    js._executeButtonAction(QStringLiteral("Arm"), true);
    QCOMPARE(armed.count(), 1);   // the check under test is what stops it below

    LinkManager::instance()->setLinksAllowed(false, LicenseManager::linkRefusedText());
    js._executeButtonAction(QStringLiteral("Arm"), true);
    js._executeButtonAction(QStringLiteral("Disarm"), true);
    QCOMPARE(armed.count(), 1);

    LinkManager::instance()->setLinksAllowed(true);
    js._executeButtonAction(QStringLiteral("Disarm"), true);
    QCOMPARE(armed.count(), 2);

    vehicle->setJoystickEnabled(false);
    js._activeVehicle = nullptr;
    js._exitThread = true;
}

// ---------------------------------------------------------------------------
// Deactivate
// ---------------------------------------------------------------------------

void LicenseLinkGateTest::_deactivateRefusedWhileVehicleConnected()
{
    const Signer s = makeSigner();
    const qint64 now = realNow();
    LicenseManager lm;
    _prepare(lm, s, now);
    storeEnvelope(envelopeBytes(envelope(s, recordObject(s, lm._deviceId, kKey, now - 3600, now + 14 * kDay))));
    lm._storeKey(kKey);
    lm._loadRecord();
    QVERIFY(lm.activated());

    lm._vehicleConnected = &oneVehicle;
    QSignalSpy busy(&lm, &LicenseManager::busyChanged);
    lm.deactivate();
    QCOMPARE(busy.count(), 0);   // nothing sent
    QVERIFY(lm.activated());
    QVERIFY(lm.linksAllowed());
    QCOMPARE(lm.statusText(), LicenseManager::deactivateVehicleText());
    QCOMPARE(lm.property("deactivateVehicleText").toString(),
             QStringLiteral("Deactivate this device is off while a vehicle is connected."));
    QVERIFY(stored("svc/sr"));
    QCOMPARE(g_takDisconnects, 0);
}

void LicenseLinkGateTest::_deactivateReplyWithVehicleKeepsSession()
{
    // The request went out with no vehicle; one connected before the reply.
    const Signer s = makeSigner();
    const qint64 now = realNow();
    LicenseManager lm;
    _prepare(lm, s, now);
    storeEnvelope(envelopeBytes(envelope(s, recordObject(s, lm._deviceId, kKey, now - 3600, now + 14 * kDay))));
    lm._storeKey(kKey);
    lm._loadRecord();
    QVERIFY(lm.activated());

    lm._vehicleConnected = &oneVehicle;
    lm._handleReply(LicenseManager::Op::Deactivate, kKey, QByteArrayLiteral(R"({"state":"deactivated"})"), true, 200);
    QVERIFY(lm.activated());
    QVERIFY(lm.attention());
    QVERIFY(lm.linksAllowed());
    QVERIFY(lm.statusText().contains(QStringLiteral("stays open for this session because a vehicle is connected")));
    QVERIFY(!stored("svc/sr"));
    QVERIFY(lm._key.isEmpty());
    QCOMPARE(g_takDisconnects, 0);

    // The next start shows the gate, and no links.
    LicenseManager next;
    _prepare(next, s, now);
    next._loadRecord();
    QVERIFY(!next.activated());
    QVERIFY(!next.linksAllowed());
}

void LicenseLinkGateTest::_gateSaysNoVehicleLinks()
{
    LicenseManager lm;
    lm._vehicleConnected = &noVehicle;
    lm._disconnectTak = &countTak;
    lm._key.clear();   // the gate's recheck() on show then sends nothing
    lm._loadRecord();
    lm._key.clear();
    QVERIFY(!lm.linksAllowed());

    QQmlEngine engine;
    engine.addImportPath(QStringLiteral("qrc:/qml"));
    engine.rootContext()->setContextProperty(QStringLiteral("LicenseManager"), &lm);
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/custom/license/LicenseGate.qml")));
    QScopedPointer<QObject> gate(component.create());
    QVERIFY2(gate, qPrintable(component.errorString()));
    QObject *const line = gate->findChild<QObject *>(QStringLiteral("licenseGateNoLinks"));
    QVERIFY(line);
    QCOMPARE(line->property("text").toString(), LicenseManager::noLinksText());
    QCOMPARE(LicenseManager::noLinksText(),
             QStringLiteral("RotorLab GC does not connect to vehicles until this device is activated."));
    QVERIFY(line->property("visible").toBool());

    // A genuine record that only ran out of time: gate up, links allowed,
    // so the line goes.
    lm._timeLapsed = true;
    lm._setStatus(LicenseManager::expiredText());
    QVERIFY(lm.linksAllowed());
    QVERIFY(gate->property("visible").toBool());
    QVERIFY(!line->property("visible").toBool());
}
