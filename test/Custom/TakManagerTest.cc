#include "TakManagerTest.h"

#include "tak/CoTMessage.h"
#include "tak/TakDefaults.h"
#include "tak/TakManager.h"
#include "rotorlab/Credential.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMap>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtCore/QXmlStreamReader>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#endif

static void clearTakSettings()
{
    QSettings s;
    s.remove(QStringLiteral("TAK"));
    s.sync();
}

static qint64 fileSize(const QString &path)
{
    QFile f(path);
    return f.exists() ? f.size() : 0;
}

void TakManagerTest::init()
{
    UnitTest::init();
    clearTakSettings();
}

void TakManagerTest::cleanup()
{
    clearTakSettings();
    // The test-only key-store entry (TakManager::passwordAccount() is a
    // per-process unit-test name in this build), never the real one.
    QVERIFY(TakManager::passwordAccount().startsWith(QStringLiteral("Unit test ")));
    Credential::erase(TakManager::passwordAccount());
    UnitTest::cleanup();
}

void TakManagerTest::_remoteCommandList()
{
    TakManager tak;
    QSignalSpy changed(&tak, &TakManager::remoteCommandsChanged);

    TakPolicy::RemoteCommand arm;
    arm.verb           = QStringLiteral("ARM");
    arm.markerText     = QStringLiteral("ARM 1");
    arm.senderCallsign = QStringLiteral("WOLF");
    arm.senderUid      = QStringLiteral("ANDROID-1");
    arm.targets        = { QStringLiteral("aircraft 1") };
    arm.result         = QStringLiteral("1 arm command sent");
    tak._recordRemoteCommand(arm, true);

    TakPolicy::RemoteCommand rtl = arm;
    rtl.verb = QStringLiteral("RTL");
    tak._recordRemoteCommand(rtl, false);

    QCOMPARE(changed.count(), 2);
    QCOMPARE(int(tak.remoteCommands().size()), 2);
    // Newest first.
    const QVariantMap newest = tak.remoteCommands().first().toMap();
    QCOMPARE(newest.value(QStringLiteral("verb")).toString(), QStringLiteral("RTL"));
    const QVariantMap first = tak.remoteCommands().last().toMap();
    QCOMPARE(first.value(QStringLiteral("title")).toString(), QStringLiteral("Remote command received"));
    QCOMPARE(first.value(QStringLiteral("sender")).toString(), QStringLiteral("WOLF (UID ANDROID-1)"));
    QCOMPARE(first.value(QStringLiteral("targets")).toString(), QStringLiteral("aircraft 1"));
    QVERIFY(!first.value(QStringLiteral("time")).toString().isEmpty());

    // Only the ARM is a banner; dismissing it keeps it in the list.
    QCOMPARE(int(tak.bannerAlerts().size()), 1);
    tak.dismissBanner(first.value(QStringLiteral("id")).toInt());
    QCOMPARE(int(tak.bannerAlerts().size()), 0);
    QCOMPARE(int(tak.remoteCommands().size()), 2);

    // The on-screen list is capped.
    for (int i = 0; i < TakManager::kMaxRemoteCommands + 5; ++i) {
        tak._recordRemoteCommand(rtl, false);
    }
    QCOMPARE(int(tak.remoteCommands().size()), TakManager::kMaxRemoteCommands);

    tak.clearRemoteCommands();
    QVERIFY(tak.remoteCommands().isEmpty());
}

void TakManagerTest::_commandLogAppends()
{
    TakManager tak;
    const QString path = tak.commandLogPath();
    QVERIFY(path.endsWith(QStringLiteral("/tak/remote-commands.jsonl")));

    TakPolicy::RemoteCommand c;
    c.verb    = QStringLiteral("HOLD");
    c.targets = { QStringLiteral("aircraft 1") };
    c.result  = QStringLiteral("1 hold command sent");

    const qint64 before = fileSize(path);
    tak._recordRemoteCommand(c, false);
    const qint64 afterOne = fileSize(path);
    QVERIFY(afterOne > before);

    // "Clear view" empties the screen only; the log is never shortened.
    tak.clearRemoteCommands();
    QCOMPARE(fileSize(path), afterOne);
    tak._recordRemoteCommand(c, false);
    QVERIFY(fileSize(path) > afterOne);

    // A refused marker, a marker using the test identity and a dismissal are
    // each their own line, and every line chains to the one before it.
    tak._commandEnabled = true;
    tak._approvedOnly   = true;
    tak._approvedPilots = { QStringLiteral("WOLF") };
    Tak::Command cmd;
    QString err;
    QVERIFY(Tak::parseCommand(QStringLiteral("ARM 1"), cmd, err));
    QString r = tak._receiveCommand(cmd, 0, 0, QStringLiteral("ARM 1"), QStringLiteral("BEAR"),
                                    QStringLiteral("ANDROID-9"), false, QByteArray("<event/>"));
    QCOMPARE(r, TakPolicy::notOnListResult());
    r = tak._receiveCommand(cmd, 0, 0, QStringLiteral("ARM 1"), TakPolicy::testSenderCallsign(),
                            QStringLiteral("ANDROID-9"), false, QByteArray("<event/>"));
    QCOMPARE(r, TakPolicy::testIdentityResult());
    QVERIFY(r.startsWith(QStringLiteral("Refused: ")));
    TakPolicy::RemoteCommand arm = c;
    arm.verb = QStringLiteral("ARM");
    tak._recordRemoteCommand(arm, true);
    tak.dismissBanner(tak.remoteCommands().first().toMap().value(QStringLiteral("id")).toInt());

    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QList<QByteArray> lines = f.readAll().split('\n');
    while (!lines.isEmpty() && lines.last().isEmpty()) {
        lines.removeLast();
    }
    QVERIFY(lines.size() >= 6);
    const QList<QByteArray> last4 = lines.mid(lines.size() - 4);
    const QJsonObject refused = QJsonDocument::fromJson(last4.at(0)).object();
    QCOMPARE(refused.value(QStringLiteral("result")).toString(), TakPolicy::notOnListResult());
    QCOMPARE(refused.value(QStringLiteral("reportedSenderCallsign")).toString(), QStringLiteral("BEAR"));
    QCOMPARE(int(refused.value(QStringLiteral("rawSha256")).toString().size()), 64);
    QVERIFY(refused.value(QStringLiteral("settings")).toObject().value(QStringLiteral("approvedOnly")).toBool());
    QCOMPARE(refused.value(QStringLiteral("schemaVersion")).toInt(), TakPolicy::kCommandLogSchema);
    QVERIFY(!refused.value(QStringLiteral("id")).toString().isEmpty());
    QVERIFY(!refused.value(QStringLiteral("receivedUtc")).toString().isEmpty());
    QVERIFY(refused.value(QStringLiteral("dispatchedUtc")).isNull());   // never reached the dispatcher
    QCOMPARE(QJsonDocument::fromJson(last4.at(3)).object().value(QStringLiteral("kind")).toString(),
             QStringLiteral("banner dismissed"));
    for (int i = 1; i < last4.size(); ++i) {
        QCOMPARE(QJsonDocument::fromJson(last4.at(i)).object().value(QStringLiteral("prevHash")).toString(),
                 TakPolicy::lineHash(last4.at(i - 1)));
    }
    QVERIFY(!tak.commandLogError());
}

void TakManagerTest::_testCommandIsListed()
{
    TakManager tak;
    tak._commandEnabled = true;
    tak._approvedOnly   = false;
    // With "Command By ATAK on" off nothing reaches the aircraft, so an ARM
    // test is listed with its result and raises no banner.
    const QString line = tak.injectTestCommand(QStringLiteral("ARM 1"));
    QVERIFY(line.startsWith(QStringLiteral("TEST ARM 1 -> ")));
    QCOMPARE(int(tak.remoteCommands().size()), 1);
    const QVariantMap e = tak.remoteCommands().first().toMap();
    QCOMPARE(e.value(QStringLiteral("verb")).toString(), QStringLiteral("ARM"));
    QCOMPARE(e.value(QStringLiteral("sender")).toString(),
             QStringLiteral("STATION-TEST (UID rotorlab-station-test)"));
    QVERIFY(e.value(QStringLiteral("test")).toBool());
    QCOMPARE(e.value(QStringLiteral("result")).toString(),
             QStringLiteral("Not sent: \"Command By ATAK on\" is off (TAK settings)"));
    QVERIFY(!e.value(QStringLiteral("banner")).toBool());
    QVERIFY(!e.value(QStringLiteral("sent")).toBool());   // nothing went to an aircraft
    QVERIFY(tak.bannerAlerts().isEmpty());

    // "Command By ATAK on" on, "Commands to aircraft on" off.
    tak._c2Allowed = true;
    tak.setC2Armed(false);
    tak.injectTestCommand(QStringLiteral("ARM 1"));
    QCOMPARE(tak.remoteCommands().first().toMap().value(QStringLiteral("result")).toString(),
             QStringLiteral("Not sent: \"Commands to aircraft on\" is off (TAK settings)"));
}

void TakManagerTest::_testCommandStationChecks()
{
    TakManager tak;
    tak._callsignPrefix = QStringLiteral("RLTEST");
    tak._prefixSetFor   = QStringLiteral("RLTEST");
    tak._c2Allowed = true;
    tak.setC2Armed(true);
    tak._verbHold = true;

    // 1. "Read commands from ATAK markers" off: refused like a real marker.
    tak._commandEnabled = false;
    QString line = tak.injectTestCommand(QStringLiteral("HOLD 1"));
    QVERIFY2(line.contains(QStringLiteral("\"Read commands from ATAK markers\" is off")), qPrintable(line));

    // 2. Approved list on without the test identity: refused, and the result
    //    names the test sender.
    tak._commandEnabled = true;
    tak._approvedOnly   = true;
    tak._approvedPilots = { QStringLiteral("WOLF") };
    line = tak.injectTestCommand(QStringLiteral("HOLD 1"));
    QVERIFY2(line.contains(TakPolicy::notOnListResult()), qPrintable(line));
    QVERIFY2(line.contains(QStringLiteral("rotorlab-station-test")), qPrintable(line));

    // 3. Test identity listed: it passes the sender check and reaches the
    //    aircraft lookup (no aircraft connected here) when it names this
    //    station's aircraft (G47). A bare number names no station.
    tak.addApprovedPilotAs(TakPolicy::testSenderUid(), QStringLiteral("uid"));
    line = tak.injectTestCommand(QStringLiteral("HOLD RLTEST-1"));
    QVERIFY2(line.contains(QStringLiteral("no aircraft RLTEST-1 is connected to this station")), qPrintable(line));
    line = tak.injectTestCommand(QStringLiteral("HOLD 1"));
    QVERIFY2(line.contains(QStringLiteral("the marker does not name a station")), qPrintable(line));

    // 4. A disabled verb is refused after the sender check.
    line = tak.injectTestCommand(QStringLiteral("LAND 1"));
    QVERIFY2(line.contains(QStringLiteral("LAND is disabled")), qPrintable(line));
}

void TakManagerTest::_testCommandParseFailure()
{
    TakManager tak;
    tak._commandEnabled = true;
    tak._approvedOnly   = false;
    const QString path = tak.commandLogPath();
    const qint64 before = fileSize(path);

    // "ARM" without a target: not sent, but returned, listed and logged.
    const QString line = tak.injectTestCommand(QStringLiteral("ARM"));
    QCOMPARE(line, QStringLiteral("TEST ARM -> ") + TakPolicy::parseFailedResult(QStringLiteral("missing track id")));
    QCOMPARE(int(tak.remoteCommands().size()), 1);
    const QVariantMap e = tak.remoteCommands().first().toMap();
    QCOMPARE(e.value(QStringLiteral("verb")).toString(), QStringLiteral("ARM"));
    QCOMPARE(e.value(QStringLiteral("result")).toString(),
             QStringLiteral("Not sent: command not understood (missing track id)"));
    QVERIFY(e.value(QStringLiteral("test")).toBool());
    QVERIFY(!e.value(QStringLiteral("sent")).toBool());
    QVERIFY(!e.value(QStringLiteral("banner")).toBool());
    QVERIFY(tak.bannerAlerts().isEmpty());

    QVERIFY(fileSize(path) > before);
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QList<QByteArray> lines = f.readAll().split('\n');
    while (!lines.isEmpty() && lines.last().isEmpty()) {
        lines.removeLast();
    }
    const QJsonObject rec = QJsonDocument::fromJson(lines.last()).object();
    QCOMPARE(rec.value(QStringLiteral("kind")).toString(), QStringLiteral("command"));
    QCOMPARE(rec.value(QStringLiteral("marker")).toString(), QStringLiteral("ARM"));
    QCOMPARE(rec.value(QStringLiteral("result")).toString(), e.value(QStringLiteral("result")).toString());
    QVERIFY(rec.value(QStringLiteral("test")).toBool());
    QVERIFY(rec.value(QStringLiteral("dispatchedUtc")).isNull());

    // An unknown verb is listed under its own first word.
    tak.injectTestCommand(QStringLiteral("jump 1"));
    const QVariantMap u = tak.remoteCommands().first().toMap();
    QCOMPARE(u.value(QStringLiteral("verb")).toString(), QStringLiteral("JUMP"));
    QCOMPARE(u.value(QStringLiteral("result")).toString(),
             QStringLiteral("Not sent: command not understood (unknown verb JUMP)"));
}

void TakManagerTest::_altitudeWarningInforms()
{
    TakManager tak;
    tak._commandEnabled = true;
    tak._approvedOnly   = false;
    tak._altWarnAboveFt = 400;
    // "Command By ATAK on" is off, so nothing is sent: the refused row carries
    // no altitude warning (and no banner). The warning shows only on a sent
    // command (TakPolicyTest::_altitudeWarning).
    tak.injectTestCommand(QStringLiteral("GOTO 1 ALT 520"));
    const QVariantMap e = tak.remoteCommands().first().toMap();
    QVERIFY(!e.value(QStringLiteral("sent")).toBool());
    QVERIFY(e.value(QStringLiteral("warning")).toString().isEmpty());
    QVERIFY(!e.value(QStringLiteral("banner")).toBool());
    QVERIFY(e.value(QStringLiteral("position")).toString().endsWith(QStringLiteral("at 520 ft above launch")));

    // Refused by the senders list: no warning either.
    tak._approvedOnly   = true;
    tak._approvedPilots = { QStringLiteral("WOLF") };
    Tak::Command cmd;
    QString err;
    QVERIFY(Tak::parseCommand(QStringLiteral("GOTO 1 ALT 520"), cmd, err));
    tak._receiveCommand(cmd, 1.0, 2.0, QStringLiteral("GOTO 1 ALT 520"), QStringLiteral("FOX"),
                        QStringLiteral("ANDROID-7"), false, QByteArray("<event/>"));
    QVERIFY(tak.remoteCommands().first().toMap().value(QStringLiteral("warning")).toString().isEmpty());
}

void TakManagerTest::_presenceOnConnect()
{
    TakManager tak;
    tak._callsignPrefix = QStringLiteral("RLTEST");
    tak._commandEnabled = true;
    tak._publishEnabled = false;
    // A real station location is set; with publishing off it is not shared.
    tak._gcsManual      = true;
    tak._gcsManualLat   = -35.363261;
    tak._gcsManualLon   = 149.165230;
    tak._gcsManualAlt   = 584.0;

    QSignalSpy rx(&tak, &TakManager::cotReceivedChanged);
    tak._onEncrypted();   // the TLS link came up (no socket in the test)
    QVERIFY(tak.connected());
    QVERIFY(!tak.cotReceivedSinceConnect());
    QVERIFY(tak._presenceTimer.isActive());
    QCOMPARE(tak._presenceTimer.interval(), TakManager::kPresenceRefreshSec * 1000);
    QVERIFY(!tak._publishTimer.isActive());   // no drone publishing

    const QByteArray ev = tak._lastPresence;
    QVERIFY2(!ev.isEmpty(), "presence event built on connect");
    QXmlStreamReader r(ev);
    QMap<QString, QXmlStreamAttributes> el;
    QStringList names;
    while (!r.atEnd() && !r.hasError()) {
        if (r.readNext() == QXmlStreamReader::StartElement) {
            names << r.name().toString();
            el.insert(r.name().toString(), r.attributes());
        }
    }
    QVERIFY2(!r.hasError(), qPrintable(r.errorString()));
    const QXmlStreamAttributes event = el.value(QStringLiteral("event"));
    QCOMPARE(event.value(QStringLiteral("uid")).toString(), tak._gcsUid);
    QCOMPARE(event.value(QStringLiteral("type")).toString(), QStringLiteral("a-f-G-U-C"));
    QCOMPARE(event.value(QStringLiteral("how")).toString(), QStringLiteral("h-g-i-g-o"));
    const QDateTime t     = QDateTime::fromString(event.value(QStringLiteral("time")).toString(), Qt::ISODateWithMs);
    const QDateTime stale = QDateTime::fromString(event.value(QStringLiteral("stale")).toString(), Qt::ISODateWithMs);
    QCOMPARE(t.secsTo(stale), qint64(TakManager::kPresenceStaleSec));
    QVERIFY(TakManager::kPresenceRefreshSec < TakManager::kPresenceStaleSec);

    // No real position: the CoT unknown point.
    const QXmlStreamAttributes pt = el.value(QStringLiteral("point"));
    QCOMPARE(pt.value(QStringLiteral("lat")).toDouble(), 0.0);
    QCOMPARE(pt.value(QStringLiteral("lon")).toDouble(), 0.0);
    QCOMPARE(pt.value(QStringLiteral("hae")).toDouble(), 9999999.0);
    QCOMPARE(pt.value(QStringLiteral("ce")).toDouble(), 9999999.0);
    QCOMPARE(pt.value(QStringLiteral("le")).toDouble(), 9999999.0);
    QVERIFY(!ev.contains("-35.36"));
    QVERIFY(!ev.contains("149.16"));
    QVERIFY(!names.contains(QStringLiteral("precisionlocation")));
    QVERIFY(!names.contains(QStringLiteral("track")));

    // The station's TAK identity, and nothing about any drone.
    QCOMPARE(el.value(QStringLiteral("contact")).value(QStringLiteral("callsign")).toString(),
             QStringLiteral("RLTEST-GCS"));
    QVERIFY(names.contains(QStringLiteral("takv")));
    QVERIFY(!names.contains(QStringLiteral("link")));
    QVERIFY(!names.contains(QStringLiteral("__video")));

    // Commands off: no presence on connect.
    TakManager quiet;
    quiet._commandEnabled = false;
    quiet._publishEnabled = false;
    quiet._onEncrypted();
    QVERIFY(quiet._lastPresence.isEmpty());
    QVERIFY(!quiet._presenceNeeded());
    quiet.disconnectServer();

    tak.disconnectServer();
    QVERIFY(!tak._presenceTimer.isActive());
}

void TakManagerTest::_presenceToServerWithDirect()
{
    // Direct recipients set and publishing off: the presence still goes to
    // the server group (no <marti><dest>), so the server registers the
    // station and relays commands to it.
    TakManager tak;
    tak._commandEnabled = true;
    tak._publishEnabled = false;
    tak._publishDirect  = true;
    tak._directDest     = QStringLiteral("ALPHA,BRAVO");
    QCOMPARE(tak._publishDest(), QStringList({ QStringLiteral("ALPHA"), QStringLiteral("BRAVO") }));
    tak._onEncrypted();
    const QByteArray ev = tak._lastPresence;
    QVERIFY(!ev.isEmpty());
    QVERIFY2(!ev.contains("<marti"), ev.constData());
    QVERIFY(!ev.contains("ALPHA"));
    QVERIFY(!ev.contains("BRAVO"));
    tak.disconnectServer();
}

namespace {
// The station events (uid == the station's TAK uid) among those written.
struct StationEvent {
    double lat = 0.0;
    double hae = 0.0;
    QString how;
    bool   direct = false;   ///< carries <marti><dest>
};
QList<StationEvent> stationEvents(const QString &gcsUid, const QList<QByteArray> &sent)
{
    QList<StationEvent> out;
    for (const QByteArray &xml : sent) {
        QXmlStreamReader r(xml);
        StationEvent e;
        bool mine = false;
        while (!r.atEnd() && !r.hasError()) {
            if (r.readNext() != QXmlStreamReader::StartElement) {
                continue;
            }
            const QXmlStreamAttributes a = r.attributes();
            if (r.name() == QLatin1String("event")) {
                mine  = a.value(QStringLiteral("uid")).toString() == gcsUid;
                e.how = a.value(QStringLiteral("how")).toString();
            } else if (r.name() == QLatin1String("point")) {
                e.lat = a.value(QStringLiteral("lat")).toDouble();
                e.hae = a.value(QStringLiteral("hae")).toDouble();
            } else if (r.name() == QLatin1String("marti")) {
                e.direct = true;
            }
        }
        if (mine) {
            out << e;
        }
    }
    return out;
}
constexpr double kStationLat = -35.363261;
} // namespace

void TakManagerTest::setStationPosition(TakManager &tak)
{
    tak._shareStationPosition = true;   // the operator turned sharing on
    tak._gcsManual    = true;
    tak._gcsManualLat = kStationLat;
    tak._gcsManualLon = 149.165230;
    tak._gcsManualAlt = 584.0;
}

void TakManagerTest::_beaconKeepsLastRealPosition()
{
    // Publishing and the beacon on: once a real beacon has gone out on this
    // link, losing the position (no station GPS, vehicle link dropped) re-sends
    // the last real point. The unknown point is never sent.
    TakManager tak;
    tak._recordSent       = true;
    tak._commandEnabled   = true;
    tak._publishEnabled   = true;
    tak._publishGcsBeacon = true;
    setStationPosition(tak);
    tak._onEncrypted();
    QList<StationEvent> ev = stationEvents(tak._gcsUid, tak._sentEvents);
    QVERIFY(!ev.isEmpty());
    for (const StationEvent &e : ev) {
        QCOMPARE(e.lat, kStationLat);
        QCOMPARE(e.how, QStringLiteral("m-g"));
    }
    QVERIFY(tak._lastPresence.isEmpty());
    QVERIFY(!tak._presenceNeeded());

    // The position goes away.
    tak._gcsManual = false;
    if (tak._effectiveGcsPosition().isValid()) {
        QSKIP("this machine reports a ground station position");
    }
    tak._sentEvents.clear();
    tak._publishTick();          // the 1 Hz publisher
    tak._announcePresence();     // the presence refresh
    ev = stationEvents(tak._gcsUid, tak._sentEvents);
    QCOMPARE(int(ev.size()), 1);
    QCOMPARE(ev.first().lat, kStationLat);
    QVERIFY(ev.first().hae != 9999999.0);
    QVERIFY(tak._lastPresence.isEmpty());
    tak.disconnectServer();
}

void TakManagerTest::_presenceBeforeFirstFix()
{
    // Publishing and the beacon on, Read commands on, no real position yet on
    // this link: presence goes out. When a position arrives the beacon takes over.
    TakManager tak;
    tak._recordSent       = true;
    tak._commandEnabled   = true;
    tak._publishEnabled   = true;
    tak._publishGcsBeacon = true;
    tak._shareStationPosition = true;
    tak._gcsManual        = false;
    if (tak._effectiveGcsPosition().isValid()) {
        QSKIP("this machine reports a ground station position");
    }
    tak._onEncrypted();
    QList<StationEvent> ev = stationEvents(tak._gcsUid, tak._sentEvents);
    QCOMPARE(int(ev.size()), 1);
    QCOMPARE(ev.first().how, QStringLiteral("h-g-i-g-o"));
    QCOMPARE(ev.first().hae, 9999999.0);
    QVERIFY(tak._presenceNeeded());

    setStationPosition(tak);
    tak._sentEvents.clear();
    tak._publishTick();
    ev = stationEvents(tak._gcsUid, tak._sentEvents);
    QCOMPARE(int(ev.size()), 1);
    QCOMPARE(ev.first().lat, kStationLat);
    QVERIFY(!tak._presenceNeeded());
    tak.disconnectServer();
}

void TakManagerTest::_directModeAnnouncesToServer()
{
    // Direct mode with the beacon on: the beacon goes only to the recipients,
    // so presence goes to the server group, and the beacon follows at once so
    // the recipients end on the real position.
    TakManager tak;
    tak._recordSent       = true;
    tak._commandEnabled   = true;
    tak._publishEnabled   = true;
    tak._publishGcsBeacon = true;
    tak._publishDirect    = true;
    tak._directDest       = QStringLiteral("ALPHA");
    setStationPosition(tak);
    tak._onEncrypted();
    QVERIFY(tak._presenceNeeded());
    const QList<StationEvent> ev = stationEvents(tak._gcsUid, tak._sentEvents);
    int toServer = 0;
    for (int i = 0; i < ev.size(); ++i) {
        if (!ev.at(i).direct) {
            ++toServer;
            QCOMPARE(ev.at(i).how, QStringLiteral("h-g-i-g-o"));   // the server group gets no position
            QVERIFY(i + 1 < ev.size());                            // and the beacon follows
            QVERIFY(ev.at(i + 1).direct);
            QCOMPARE(ev.at(i + 1).lat, kStationLat);
        } else {
            QCOMPARE(ev.at(i).lat, kStationLat);                   // recipients: real point only
        }
    }
    QCOMPARE(toServer, 1);
    QVERIFY(ev.last().direct);

    // Registration only: while the beacon runs in direct mode, no periodic
    // group presence; the recipients are kept fresh by the real beacon.
    QCOMPARE(tak._presenceTimer.interval(), TakManager::kPresenceRefreshSec * 1000);
    tak._sentEvents.clear();
    for (int i = 0; i < 3; ++i) {
        tak._refreshPresence();   // what the presence timer runs
        tak._publishTick();       // the 1 Hz publisher
    }
    const QList<StationEvent> later = stationEvents(tak._gcsUid, tak._sentEvents);
    QCOMPARE(int(later.size()), 3);
    for (const StationEvent &e : later) {
        QVERIFY(e.direct);
        QCOMPARE(e.lat, kStationLat);
    }

    // A change that makes it needed again sends the registration once more.
    tak._commandEnabled = false;
    emit tak.configChanged();
    tak._commandEnabled = true;
    tak._sentEvents.clear();
    emit tak.configChanged();
    int group = 0;
    for (const StationEvent &e : stationEvents(tak._gcsUid, tak._sentEvents)) {
        group += e.direct ? 0 : 1;
    }
    QCOMPARE(group, 1);

    // Without direct mode the presence refresh still runs when needed
    // (publishing off): checked here through the same timer slot.
    TakManager off;
    off._recordSent     = true;
    off._commandEnabled = true;
    off._publishEnabled = false;
    off._onEncrypted();
    off._sentEvents.clear();
    off._refreshPresence();
    QCOMPARE(int(stationEvents(off._gcsUid, off._sentEvents).size()), 1);
    off.disconnectServer();
    tak.disconnectServer();
}

void TakManagerTest::_presenceAtOnceOnChange()
{
    // Read commands turned on mid-link: presence at once.
    TakManager tak;
    tak._recordSent     = true;
    tak._commandEnabled = false;
    tak._publishEnabled = false;
    tak._onEncrypted();
    QVERIFY(tak._sentEvents.isEmpty());
    tak._commandEnabled = true;
    emit tak.configChanged();
    QCOMPARE(int(stationEvents(tak._gcsUid, tak._sentEvents).size()), 1);
    QVERIFY(!tak._lastPresence.isEmpty());
    // Off and on again: at once again.
    tak._commandEnabled = false;
    emit tak.configChanged();
    tak._commandEnabled = true;
    tak._sentEvents.clear();
    emit tak.configChanged();
    QCOMPARE(int(stationEvents(tak._gcsUid, tak._sentEvents).size()), 1);
    tak.disconnectServer();

    // Publishing turned off mid-link: the beacon stops, presence at once.
    TakManager pub;
    pub._recordSent       = true;
    pub._commandEnabled   = true;
    pub._publishEnabled   = true;
    pub._publishGcsBeacon = true;
    setStationPosition(pub);
    pub._onEncrypted();
    QVERIFY(pub._lastPresence.isEmpty());
    pub._sentEvents.clear();
    pub._publishEnabled = false;
    emit pub.configChanged();
    const QList<StationEvent> ev = stationEvents(pub._gcsUid, pub._sentEvents);
    QCOMPARE(int(ev.size()), 1);
    QCOMPARE(ev.first().how, QStringLiteral("h-g-i-g-o"));
    pub.disconnectServer();
}

// ---------------------------------------------------------------------------
// m4086: the GCS beacon shares the station position only when the operator
// turned "Share this station's position with the TAK server" on.
// ---------------------------------------------------------------------------

namespace {
/// The station events among those written, as raw XML.
QList<QByteArray> stationXml(const QString &gcsUid, const QList<QByteArray> &sent)
{
    QList<QByteArray> out;
    for (const QByteArray &xml : sent) {
        if (CoT::parse(xml).uid == gcsUid) {
            out << xml;
        }
    }
    return out;
}
} // namespace

void TakManagerTest::_sharePositionDefaultOff()
{
    QCOMPARE(TakDefaults::shareStationPosition, false);
    {
        TakManager tak;   // a new install: no saved TAK settings
        QVERIFY(!tak._shareStationPosition);
        QVERIFY(!tak.property("shareStationPosition").toBool());
    }
    clearTakSettings();
    // An upgraded install: TAK settings saved by an older version, which
    // never wrote this key, with the beacon and a manual location on.
    {
        QSettings s;
        s.beginGroup(QStringLiteral("TAK"));
        s.setValue(QStringLiteral("publishEnabled"), true);
        s.setValue(QStringLiteral("publishGcsBeacon"), true);
        s.setValue(QStringLiteral("gcsManual"), true);
        s.setValue(QStringLiteral("gcsManualLat"), kStationLat);
        s.setValue(QStringLiteral("gcsManualLon"), 149.165230);
        s.endGroup();
        s.sync();
    }
    {
        TakManager tak;
        QVERIFY(tak._publishGcsBeacon);
        QVERIFY(tak._gcsManual);
        QVERIFY(!tak._shareStationPosition);
        QVERIFY(!tak._beaconPosition().isValid());
        QVERIFY(tak.gcsPositionText().startsWith(QStringLiteral("position not shared")));
        // Turned on: saved and kept.
        tak._shareStationPosition = true;
        tak._persist();
    }
    TakManager reloaded;
    QVERIFY(reloaded._shareStationPosition);
}

void TakManagerTest::_beaconWithoutPositionWhenOff()
{
    // Publishing and the beacon on, a real station location set, sharing off
    // (the default): the beacon still goes out every tick, with the CoT
    // unknown point and nothing of the location.
    TakManager tak;
    tak._recordSent       = true;
    tak._callsignPrefix   = QStringLiteral("RLTEST");
    tak._commandEnabled   = true;
    tak._publishEnabled   = true;
    tak._publishGcsBeacon = true;
    setStationPosition(tak);
    tak._shareStationPosition = false;
    QVERIFY(tak._effectiveGcsPosition().isValid());
    QVERIFY(tak._beaconAnnounces());
    QVERIFY(!tak._presenceNeeded());   // the beacon is the presence

    tak._onEncrypted();
    tak._publishTick();
    const QList<QByteArray> sent = stationXml(tak._gcsUid, tak._sentEvents);
    QVERIFY(sent.size() >= 2);
    for (const QByteArray &xml : sent) {
        QVERIFY2(!xml.contains("-35.36"), xml.constData());
        QVERIFY2(!xml.contains("149.16"), xml.constData());
        QVERIFY(!xml.contains("precisionlocation"));
        QVERIFY(xml.contains("callsign=\"RLTEST-GCS\""));
        QVERIFY(xml.contains("<takv"));
    }
    for (const StationEvent &e : stationEvents(tak._gcsUid, tak._sentEvents)) {
        QCOMPARE(e.how, QStringLiteral("h-g-i-g-o"));
        QCOMPARE(e.lat, 0.0);
        QCOMPARE(e.hae, 9999999.0);
        QVERIFY(!e.direct);
    }
    // The point is the full unknown point.
    const CoT::Marker m = CoT::parse(sent.first());
    QCOMPARE(m.uid, tak._gcsUid);
    QCOMPARE(m.lat, 0.0);
    QCOMPARE(m.lon, 0.0);
    QVERIFY2(sent.first().contains("hae=\"9999999.0\" ce=\"9999999.0\" le=\"9999999.0\""),
             sent.first().constData());

    // Sharing on, then off again mid-link: the last real point is not kept.
    tak._shareStationPosition = true;
    tak._publishTick();
    QVERIFY(tak._lastBeaconPos.isValid());
    tak._shareStationPosition = false;
    emit tak.configChanged();
    tak._sentEvents.clear();
    tak._publishTick();
    QVERIFY(!tak._lastBeaconPos.isValid());
    const QList<StationEvent> after = stationEvents(tak._gcsUid, tak._sentEvents);
    QCOMPARE(int(after.size()), 1);
    QCOMPARE(after.first().how, QStringLiteral("h-g-i-g-o"));
    QCOMPARE(after.first().lat, 0.0);
    tak.disconnectServer();
}

void TakManagerTest::_beaconPositionWhenOn()
{
    TakManager tak;
    tak._recordSent       = true;
    tak._publishEnabled   = true;
    tak._publishGcsBeacon = true;
    setStationPosition(tak);   // sharing on
    QVERIFY(tak._shareStationPosition);
    tak._onEncrypted();
    tak._publishTick();
    const QList<StationEvent> ev = stationEvents(tak._gcsUid, tak._sentEvents);
    QVERIFY(!ev.isEmpty());
    for (const StationEvent &e : ev) {
        QCOMPARE(e.how, QStringLiteral("m-g"));
        QCOMPARE(e.lat, kStationLat);
        QCOMPARE(e.hae, 584.0);
    }
    const QList<QByteArray> sent = stationXml(tak._gcsUid, tak._sentEvents);
    QVERIFY(sent.first().contains("149.16"));
    QVERIFY(sent.first().contains("precisionlocation"));
    QVERIFY(tak.gcsPositionText().startsWith(QStringLiteral("manual:")));
    tak.disconnectServer();
}

void TakManagerTest::_conflictSeenWithSharingOff()
{
    // Station A, sharing off: its position-free beacon.
    TakManager a;
    a._recordSent       = true;
    a._callsignPrefix   = QStringLiteral("RLTEST");
    a._gcsUid           = QStringLiteral("rotorlab-aaaaaaaaaaaa");
    a._publishEnabled   = true;
    a._publishGcsBeacon = true;
    setStationPosition(a);
    a._shareStationPosition = false;
    a._onEncrypted();
    const QList<QByteArray> beacon = stationXml(a._gcsUid, a._sentEvents);
    QVERIFY(!beacon.isEmpty());
    QVERIFY(!beacon.first().contains("-35.36"));
    a.disconnectServer();

    // The beacon parses as a device report.
    const CoT::Marker m = CoT::parse(beacon.first());
    QVERIFY(m.isEud);
    QCOMPARE(m.callsign, QStringLiteral("RLTEST-GCS"));

    // Station B on the same prefix sees the conflict.
    TakManager b;
    b._callsignPrefix = QStringLiteral("RLTEST");
    b._prefixSetFor   = QStringLiteral("RLTEST");
    b._gcsUid         = QStringLiteral("rotorlab-bbbbbbbbbbbb");
    b._commandEnabled = true;
    b._approvedOnly   = false;
    QVERIFY(!b._prefixConflictActive());
    b._handleMarker(beacon.first());
    QVERIFY(b._prefixConflictActive());
    QVERIFY(!b.prefixConflictNotice().isEmpty());

    // Station C on another prefix records A's prefix as in use.
    TakManager c;
    c._callsignPrefix = QStringLiteral("BRAVO");
    c._prefixSetFor   = QStringLiteral("BRAVO");
    c._gcsUid         = QStringLiteral("rotorlab-cccccccccccc");
    c._commandEnabled = true;
    c._approvedOnly   = false;
    c._handleMarker(beacon.first());
    QVERIFY(!c._prefixConflictActive());
    QVERIFY(c._prefixLastSeenElsewhere(QStringLiteral("RLTEST")) > 0);
}

void TakManagerTest::_ownUidsExact()
{
    TakManager tak;
    tak._callsignPrefix = QStringLiteral("RLTEST");
    tak._onEncrypted();
    tak._ownUids.insert(QStringLiteral("RLTEST-1"));   // a drone this station published
    auto ev = [](const QString &uid) {
        return QStringLiteral("<event version=\"2.0\" uid=\"%1\" type=\"a-f-A-M-H-Q\" how=\"m-g\">"
                              "<point lat=\"1\" lon=\"2\" hae=\"0\" ce=\"9\" le=\"9\"/><detail/></event>")
            .arg(uid).toUtf8();
    };
    QSignalSpy spy(&tak, &TakManager::cotReceivedChanged);
    tak._noteReceived(ev(tak._gcsUid));
    tak._noteReceived(ev(QStringLiteral("RLTEST-1")));
    QVERIFY(!tak.cotReceivedSinceConnect());
    // Another station with the same prefix is CoT received.
    tak._noteReceived(ev(QStringLiteral("RLTEST-2")));
    QVERIFY(tak.cotReceivedSinceConnect());
    QCOMPARE(spy.count(), 1);
    // A new link starts over.
    tak._onEncrypted();
    QVERIFY(!tak.cotReceivedSinceConnect());
    QVERIFY(tak._ownUids.isEmpty());
    tak.disconnectServer();
}

void TakManagerTest::_openSenderWarning()
{
    TakManager tak;
    QSignalSpy spy(&tak, &TakManager::trustStateChanged);
    QVERIFY(tak.openSenderWarning().isEmpty());      // list on by default
    tak._approvedOnly = false;
    tak._c2Allowed    = true;
    tak._prefixSetFor = tak._callsignPrefix;   // set on this install (security M1)
    tak.setC2Armed(true);
    QCOMPARE(tak.openSenderWarning(), QStringLiteral("Any sender on this TAK server can command the aircraft."));
    QVERIFY(spy.count() >= 1);
    tak.setC2Armed(false);
    QVERIFY(tak.openSenderWarning().isEmpty());
}

void TakManagerTest::_pilotListKinds()
{
    TakManager tak;
    tak.addApprovedPilotAs(QStringLiteral("ANDROID-1"), QStringLiteral("uid"));
    tak.addApprovedPilotAs(QStringLiteral("android-1"), QStringLiteral("uid"));     // duplicate
    tak.addApprovedPilotAs(QStringLiteral("WOLF"), QStringLiteral("callsign"));
    tak.addApprovedPilotAs(QStringLiteral("  "), QStringLiteral("callsign"));       // empty
    QCOMPARE(tak.approvedPilots(), QStringList({ QStringLiteral("uid:ANDROID-1"), QStringLiteral("callsign:WOLF") }));
    const QVariantMap row = tak.approvedPilotRows().first().toMap();
    QCOMPARE(row.value(QStringLiteral("kind")).toString(), QStringLiteral("uid"));
    QCOMPARE(row.value(QStringLiteral("value")).toString(), QStringLiteral("ANDROID-1"));

    QString who;
    tak._approvedOnly = true;
    QVERIFY(tak._senderApproved(QStringLiteral("SOMEONE"), QStringLiteral("ANDROID-1"), who));
    QVERIFY(!tak._senderApproved(QStringLiteral("ANDROID-1"), QStringLiteral("OTHER"), who));
    QVERIFY(tak._senderApproved(QStringLiteral("WOLF"), QStringLiteral("OTHER"), who));

    tak.removeApprovedPilot(QStringLiteral("uid:ANDROID-1"));
    QCOMPARE(tak.approvedPilots(), QStringList({ QStringLiteral("callsign:WOLF") }));
}

void TakManagerTest::_fingerprintPinning()
{
    TakManager tak;
    QSignalSpy spy(&tak, &TakManager::fingerprintChanged);
    const QString fp = TakPolicy::sha256Fingerprint("server");
    tak._setPendingFingerprint(fp, false);
    QCOMPARE(tak.pendingFingerprint(), fp);
    tak.acceptServerFingerprint();
    QCOMPARE(tak.pinnedFingerprint(), fp);
    QVERIFY(tak.pendingFingerprint().isEmpty());
    QVERIFY(spy.count() >= 2);
    {
        TakManager reloaded;   // persisted
        QCOMPARE(reloaded.pinnedFingerprint(), fp);
    }
    // A placeholder (no certificate seen) cannot be accepted.
    tak._setPendingFingerprint(QStringLiteral("(no certificate)"), false);
    tak.acceptServerFingerprint();
    QCOMPARE(tak.pinnedFingerprint(), fp);
    tak.forgetServerFingerprint();
    QVERIFY(tak.pinnedFingerprint().isEmpty());
}

void TakManagerTest::_newInstallDefaults()
{
    TakManager tak;   // no saved TAK settings
    QCOMPARE(tak._verbGoto,    TakDefaults::verbGoto);
    QCOMPARE(tak._verbRtl,     TakDefaults::verbRtl);
    QCOMPARE(tak._verbLand,    TakDefaults::verbLand);
    QCOMPARE(tak._verbHold,    TakDefaults::verbHold);
    QCOMPARE(tak._verbAltHold, TakDefaults::verbAltHold);
    QCOMPARE(tak._verbArm,     TakDefaults::verbArm);
    QCOMPARE(tak._verbTakeoff, TakDefaults::verbTakeoff);
    QVERIFY(!tak._verbGoto && !tak._verbRtl && !tak._verbLand && !tak._verbHold && !tak._verbAltHold);
    QCOMPARE(tak._approvedOnly, TakDefaults::approvedOnly);
    QVERIFY(tak._approvedOnly);
    QVERIFY(tak._verifyPeer);
    QVERIFY(tak.pinnedFingerprint().isEmpty());
    QCOMPARE(tak._altWarnAboveFt, 0.0);
}

void TakManagerTest::_savedSettingsKept()
{
    {
        QSettings s;
        s.beginGroup(QStringLiteral("TAK"));
        s.setValue(QStringLiteral("verbGoto"), true);
        s.setValue(QStringLiteral("approvedOnly"), false);
        s.setValue(QStringLiteral("altWarnAboveFt"), 400.0);
        s.endGroup();
        s.sync();
    }
    TakManager tak;
    QVERIFY(tak._verbGoto);
    QVERIFY(!tak._approvedOnly);
    QCOMPARE(tak._altWarnAboveFt, 400.0);
    QVERIFY(!tak._verbRtl);   // not saved: the default
}

// ---------------------------------------------------------------------------
// GC 5.1.2 (TAK link): G40, G41, G45, G46
// ---------------------------------------------------------------------------

namespace {

/// A point an ATAK user placed and sent to `dest` (a placed marker: p-p
/// creator link, human "how", not stale).
QByteArray sendToEvent(const QString &dest, const QString &uid)
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    return QStringLiteral(
               "<event version=\"2.0\" uid=\"%4\" type=\"a-u-G\" how=\"h-g-i-g-o\" "
               "time=\"%1\" start=\"%1\" stale=\"%2\">"
               "<point lat=\"37.7700\" lon=\"-122.4900\" hae=\"10\" ce=\"5\" le=\"5\"/>"
               "<detail><contact callsign=\"Waypoint 7\"/>"
               "<link uid=\"ANDROID-g40test\" relation=\"p-p\" type=\"a-f-G-U-C\" parent_callsign=\"KESTREL\"/>"
               "<marti><dest callsign=\"%3\"/></marti></detail></event>")
        .arg(now.toString(Qt::ISODateWithMs), now.addSecs(300).toString(Qt::ISODateWithMs), dest, uid)
        .toUtf8();
}

bool anyLine(const QSignalSpy &spy, const QString &part)
{
    for (const QList<QVariant> &args : spy) {
        if (args.value(0).toString().contains(part)) {
            return true;
        }
    }
    return false;
}

} // namespace

void TakManagerTest::_sendToFollowsFleetCallsign()
{
    TakManager tak;
    tak._callsignPrefix    = QStringLiteral("RLTEST");
    tak._prefixSetFor      = QStringLiteral("RLTEST");
    tak._commandEnabled    = true;
    tak._c2Allowed         = true;
    tak.setC2Armed(true);
    tak._verbGoto          = true;
    tak._approvedOnly      = false;
    tak._dronesAddressable = true;

    // Control: no Fleet callsign, addressing on. The point is read (no
    // aircraft here, so it is listed as not sent).
    {
        QSignalSpy log(&tak, &TakManager::logMessage);
        tak._handleMarker(sendToEvent(QStringLiteral("RLTEST-3"), QStringLiteral("marker-g40-1")));
        QCOMPARE(int(tak.remoteCommands().size()), 1);
        QVERIFY(!anyLine(log, QStringLiteral("is not read")));
    }
    tak.clearRemoteCommands();

    // Aircraft 3 publishes the Fleet callsign HAWK: RLTEST-3 is not its name.
    tak._fleet[3].callsign = QStringLiteral("HAWK");
    {
        QSignalSpy log(&tak, &TakManager::logMessage);
        tak._handleMarker(sendToEvent(QStringLiteral("RLTEST-3"), QStringLiteral("marker-g40-2")));
        QCOMPARE(int(tak.remoteCommands().size()), 0);
        QVERIFY(anyLine(log, QStringLiteral("point sent to RLTEST-3 is not read: aircraft 3 is published as HAWK")));
    }
    // Another aircraft without a Fleet callsign is still addressable.
    tak._handleMarker(sendToEvent(QStringLiteral("RLTEST-4"), QStringLiteral("marker-g40-3")));
    QCOMPARE(int(tak.remoteCommands().size()), 1);
    tak.clearRemoteCommands();

    // "Let ATAK address drones" off: no send-to point is read at all.
    tak._fleet.clear();
    tak._dronesAddressable = false;
    {
        QSignalSpy log(&tak, &TakManager::logMessage);
        tak._handleMarker(sendToEvent(QStringLiteral("RLTEST-3"), QStringLiteral("marker-g40-4")));
        QCOMPARE(int(tak.remoteCommands().size()), 0);
        QVERIFY(anyLine(log, QStringLiteral("point sent to RLTEST-3 is not read: \"Let ATAK address drones\" is off")));
    }
}

void TakManagerTest::_fleetJoinClosesLink()
{
    TakManager tak;
    tak._prefixSetFor = tak._callsignPrefix;   // set on this install (security M1)
    tak._c2Allowed = true;
    tak._onEncrypted();          // a live TAK link (no socket in the test)
    tak.setC2Armed(true);
    QVERIFY(tak.connected());
    QVERIFY(tak.c2Armed());
    QVERIFY(tak.fleetHandOffNotice().isEmpty());

    QSignalSpy notice(&tak, &TakManager::fleetHandOffNoticeChanged);
    QSignalSpy log(&tak, &TakManager::logMessage);
    tak.handOffToFleet();
    QVERIFY(!tak.connected());
    QVERIFY(!tak.c2Armed());
    QVERIFY(!tak._presenceTimer.isActive());
    QVERIFY(!tak._publishTimer.isActive());
    QCOMPARE(notice.count(), 1);
    QVERIFY(tak.fleetHandOffNotice().contains(QStringLiteral("The TAK link was closed")));
    QVERIFY(tak.fleetHandOffNotice().contains(QStringLiteral("\"Commands to aircraft on\" was turned off")));
    QVERIFY(tak.statusText().contains(QStringLiteral("Fleet Server")));
    QVERIFY(anyLine(log, QStringLiteral("Fleet Server joined")));
    // C2 permission and the other settings are kept; only the session arm is off.
    QVERIFY(tak._c2Allowed);

    // S4: while the Fleet Server is the route, no direct link and no
    // commands, whoever asks.
    QVERIFY(tak._fleetIsRoute);
    tak._host = QStringLiteral("127.0.0.1");
    tak._port = 9;
    tak.connectServer();
    QVERIFY(!tak._socket);
    QVERIFY(tak.statusText().contains(QStringLiteral("Fleet Server")));
    QSignalSpy armSpy(&tak, &TakManager::c2ArmedChanged);
    tak.setC2Armed(true);
    QVERIFY(!tak.c2Armed());
    QCOMPARE(armSpy.count(), 1);   // the switch is told it stayed off
    QVERIFY(anyLine(log, QStringLiteral("\"Commands to aircraft on\" not turned on")));

    // Nothing live: nothing to say.
    tak.clearFleetHandOff();
    QVERIFY(!tak._fleetIsRoute);
    QVERIFY(tak.fleetHandOffNotice().isEmpty());
    tak.handOffToFleet();
    QVERIFY(tak.fleetHandOffNotice().isEmpty());
    QVERIFY(tak._fleetIsRoute);

    // Armed without a link: the switch is still turned off.
    tak.clearFleetHandOff();
    tak.setC2Armed(true);
    QVERIFY(tak.c2Armed());
    tak.handOffToFleet();
    QVERIFY(!tak.c2Armed());
    QVERIFY(!tak.fleetHandOffNotice().isEmpty());
    QVERIFY(!tak.fleetHandOffNotice().contains(QStringLiteral("TAK link was closed")));
}

void TakManagerTest::_verifyOffLogged()
{
    TakManager tak;
    QVERIFY(tak.verifyPeer());   // default on
    QSignalSpy log(&tak, &TakManager::logMessage);
    tak.setVerifyPeer(false);
    QVERIFY(!tak.verifyPeer());
    QVERIFY(anyLine(log, QStringLiteral("\"Verify server certificate\" turned off")));
    tak.setVerifyPeer(true);
    QVERIFY(anyLine(log, QStringLiteral("\"Verify server certificate\" turned on")));

    // Each connect with it off says so again.
    tak.setVerifyPeer(false);
    tak._host = QStringLiteral("127.0.0.1");
    tak._port = 9;
    log.clear();
    tak.connectServer();
    QVERIFY(anyLine(log, QStringLiteral("connecting with \"Verify server certificate\" off")));
    tak.disconnectServer();
}

void TakManagerTest::_passwordMovedToKeyStore()
{
    const QString account = TakManager::passwordAccount();
    QVERIFY(account.startsWith(QStringLiteral("Unit test ")));
    Credential::erase(account);
    {
        // What 5.1.1 and earlier left: the password in plain text.
        QSettings s;
        s.setValue(QStringLiteral("TAK/username"), QStringLiteral("kestrel"));
        s.setValue(QStringLiteral("TAK/password"), QStringLiteral("unit-test-not-a-real-password"));
        s.sync();
    }
    {
        TakManager tak;
        QCOMPARE(tak._password, QStringLiteral("unit-test-not-a-real-password"));
        QSettings s;
        QVERIFY(!s.contains(QStringLiteral("TAK/password")));
        QCOMPARE(s.value(QStringLiteral("TAK/username")).toString(), QStringLiteral("kestrel"));
        if (Credential::inOsStore()) {
            QCOMPARE(Credential::load(account), QStringLiteral("unit-test-not-a-real-password"));
            QVERIFY(tak.passwordStorageText().contains(Credential::storeName()));
            QCOMPARE(tak.passwordStorageText(), QStringLiteral("Password saved in %1. The readable copy that an earlier version kept in the settings file has been deleted.").arg(Credential::storeName()));
        } else {
            QVERIFY(tak.passwordStorageText().contains(QStringLiteral("this session only")));
        }
    }
    // Once: the next start reads it from the store and does not move again.
    {
        TakManager tak;
        QSettings s;
        QVERIFY(!s.contains(QStringLiteral("TAK/password")));
        if (Credential::inOsStore()) {
            Credential::dropCache();
            TakManager fresh;
            QCOMPARE(fresh._password, QStringLiteral("unit-test-not-a-real-password"));
            QCOMPARE(fresh.passwordStorageText(),
                     QStringLiteral("Password saved in %1.").arg(Credential::storeName()));
        }
    }
    Credential::erase(account);
}

void TakManagerTest::_passwordNeverInSettings()
{
    const QString account = TakManager::passwordAccount();
    Credential::erase(account);
    TakManager tak;
    tak._password = QStringLiteral("unit-test-typed-password");
    tak._persist();
    {
        QSettings s;
        QVERIFY(!s.contains(QStringLiteral("TAK/password")));
        // Not anywhere else in the settings file either.
        for (const QString &k : s.allKeys()) {
            QVERIFY2(!s.value(k).toString().contains(QStringLiteral("unit-test-typed-password")), qPrintable(k));
        }
    }
    if (Credential::inOsStore()) {
        QCOMPARE(Credential::load(account), QStringLiteral("unit-test-typed-password"));
        QCOMPARE(tak.passwordStorageText(), QStringLiteral("Password saved in %1.").arg(Credential::storeName()));
    } else {
        QVERIFY(tak.passwordStorageText().contains(QStringLiteral("this session only")));
    }
    // Clearing the field removes the stored copy.
    tak._password.clear();
    tak._persist();
    QVERIFY(tak.passwordStorageText().isEmpty());
    Credential::dropCache();
    QVERIFY(Credential::load(account).isEmpty());
}

void TakManagerTest::_clientKeyOwnerOnly()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString key = dir.filePath(QStringLiteral("client_key.pem"));
    const QByteArray pem = QByteArrayLiteral("-----BEGIN PRIVATE KEY-----\nunit-test\n-----END PRIVATE KEY-----\n");
    QVERIFY(TakManager::_writePemFile(key, pem, true));
    {
        QFile f(key);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), pem);   // still readable by its owner
    }
#ifdef Q_OS_WIN
    const std::wstring native = QDir::toNativeSeparators(key).toStdWString();
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    QCOMPARE(::GetNamedSecurityInfoW(native.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                     nullptr, nullptr, &dacl, nullptr, &sd), DWORD(ERROR_SUCCESS));
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    QVERIFY(::GetSecurityDescriptorControl(sd, &control, &revision));
    const bool isProtected = (control & SE_DACL_PROTECTED) != 0;
    const int aces = dacl ? int(dacl->AceCount) : -1;
    ::LocalFree(sd);
    QVERIFY(isProtected);      // nothing inherited from the profile folder
    QCOMPARE(aces, 1);         // the current user only
#else
    const QFileDevice::Permissions p = QFile::permissions(key);
    QVERIFY(p & QFileDevice::ReadOwner);
    QVERIFY(!(p & (QFileDevice::ReadGroup | QFileDevice::ReadOther
                   | QFileDevice::WriteGroup | QFileDevice::WriteOther)));
#endif
    // A certificate is not secret and keeps the folder's permissions.
    QVERIFY(TakManager::_writePemFile(dir.filePath(QStringLiteral("client_cert.pem")), pem, false));
}

void TakManagerTest::_statusAfterFailedConnect()
{
    // A port nothing listens on: take a free one and close it again.
    quint16 port = 0;
    {
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        port = probe.serverPort();
    }
    TakManager tak;
    tak._host = QStringLiteral("127.0.0.1");
    tak._port = port;
    tak.connectServer();
    QCOMPARE(tak.statusText(), QStringLiteral("connecting..."));
    QTRY_VERIFY_WITH_TIMEOUT(tak.statusText().startsWith(QStringLiteral("connect failed: ")), 15000);
    QVERIFY(!tak.connected());
    // It stays that way; it does not fall back to "connecting...".
    QTest::qWait(300);
    QVERIFY(tak.statusText().startsWith(QStringLiteral("connect failed: ")));

    // A link that was up and dropped still says "disconnected".
    tak._onEncrypted();
    tak._onDisconnected();
    QCOMPARE(tak.statusText(), QStringLiteral("disconnected"));
    tak.disconnectServer();
}

void TakManagerTest::_receiveBufferBounded()
{
    TakManager tak;
    tak._gcsUid = QStringLiteral("rotorlab-g46test0001");
    auto ev = [](const QString &uid) {
        return QStringLiteral("<event version=\"2.0\" uid=\"%1\" type=\"a-f-G-U-C\" how=\"h-e\">"
                              "<point lat=\"1\" lon=\"2\" hae=\"0\" ce=\"1\" le=\"1\"/></event>")
            .arg(uid).toUtf8();
    };

    // An event that never ends: the buffer grows up to the bound, then the
    // partial event is dropped with a log line.
    QSignalSpy log(&tak, &TakManager::logMessage);
    tak._consumeRx(QByteArrayLiteral("<event uid=\"endless\"><detail>"));
    const QByteArray filler(64 * 1024, 'x');
    qsizetype fed = 0;
    while (fed <= TakManager::kMaxRxBufferBytes) {
        tak._consumeRx(filler);
        fed += filler.size();
        QVERIFY(tak._rxBuf.size() <= TakManager::kMaxRxBufferBytes);
    }
    QVERIFY(tak._rxBuf.isEmpty());
    QVERIFY(anyLine(log, QStringLiteral("TAK receive buffer over 1024 KB with no complete event")));
    QVERIFY(!tak.cotReceivedSinceConnect());

    // The stream carries on: the next complete event is read.
    tak._consumeRx(ev(QStringLiteral("ANDROID-after")));
    QVERIFY(tak.cotReceivedSinceConnect());

    // S2: many complete events in one read are handled in passes from the
    // event loop, not all at once, and none is lost or dropped.
    TakManager tak3;
    tak3._gcsUid = QStringLiteral("rotorlab-g46test0003");
    QByteArray burst;
    const int n = TakManager::kMaxRxEventsPerPass * 3 + 7;
    for (int i = 0; i < n; ++i) {
        burst += ev(QStringLiteral("ANDROID-burst-%1").arg(i));
    }
    QSignalSpy log3(&tak3, &TakManager::logMessage);
    tak3._consumeRx(burst);
    QVERIFY(!tak3._rxBuf.isEmpty());          // only one pass so far
    QVERIFY(tak3._rxDrainScheduled);
    QTRY_VERIFY_WITH_TIMEOUT(tak3._rxBuf.isEmpty(), 5000);
    QVERIFY(!anyLine(log3, QStringLiteral("dropped")));

    // Events below the bound split across reads still join up.
    TakManager tak2;
    tak2._gcsUid = QStringLiteral("rotorlab-g46test0002");
    const QByteArray whole = ev(QStringLiteral("ANDROID-split"));
    tak2._consumeRx(whole.left(20));
    QVERIFY(!tak2.cotReceivedSinceConnect());
    tak2._consumeRx(whole.mid(20));
    QVERIFY(tak2.cotReceivedSinceConnect());
    QVERIFY(tak2._rxBuf.isEmpty());
}

void TakManagerTest::_keyFileFailsClosed()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString key = dir.filePath(QStringLiteral("client_key.pem"));
    const QByteArray oldPem = QByteArrayLiteral("-----BEGIN PRIVATE KEY-----\nold\n-----END PRIVATE KEY-----\n");
    const QByteArray newPem = QByteArrayLiteral("-----BEGIN PRIVATE KEY-----\nnew\n-----END PRIVATE KEY-----\n");
    QVERIFY(TakManager::_writePemFile(key, oldPem, true));

    // The file cannot be restricted: nothing is written, the old file stays.
    TakManager::s_failRestrictForTest = true;
    const bool wrote = TakManager::_writePemFile(key, newPem, true);
    TakManager::s_failRestrictForTest = false;
    QVERIFY(!wrote);
    QVERIFY(!QFile::exists(key + QStringLiteral(".new")));
    {
        QFile f(key);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), oldPem);
    }

    // A key file at load that cannot be restricted is not used.
    const QString certDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/tak");
    QDir().mkpath(certDir);
    {
        QFile f(certDir + QStringLiteral("/client_key.pem"));
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(oldPem);
    }
    TakManager::s_failRestrictForTest = true;
    {
        TakManager tak;
        QVERIFY(tak._clientKeyPem.isEmpty());
        QVERIFY(!tak.enrolled());
    }
    TakManager::s_failRestrictForTest = false;
    {
        TakManager tak;
        QCOMPARE(tak._clientKeyPem, oldPem);
    }
    QFile::remove(certDir + QStringLiteral("/client_key.pem"));
}

void TakManagerTest::_plainPasswordKeptUntilStored()
{
    if (!Credential::inOsStore()) {
        QSKIP("no system key store here: the plain value is removed at once by design");
    }
    const QString account = TakManager::passwordAccount();
    Credential::erase(account);
    {
        QSettings s;
        s.setValue(QStringLiteral("TAK/password"), QStringLiteral("unit-test-armed-password"));
        s.sync();
    }
    // An aircraft is armed at start: the key store refuses the move.
    Credential::setArmedProbe([]() { return true; });
    {
        TakManager tak;
        QCOMPARE(tak._password, QStringLiteral("unit-test-armed-password"));
        QVERIFY(tak.passwordStorageText().startsWith(
            QStringLiteral("Password not moved to %1 yet.").arg(Credential::storeName())));
        QVERIFY(tak.passwordStorageText().contains(QStringLiteral("stays in the settings file")));
        tak._persist();   // a settings save while still armed
        QSettings s;
        QCOMPARE(s.value(QStringLiteral("TAK/password")).toString(), QStringLiteral("unit-test-armed-password"));

        // Disarmed: the next settings save moves it, no retyping.
        Credential::setArmedProbe([]() { return false; });
        tak._persist();
        QSettings s2;
        QVERIFY(!s2.contains(QStringLiteral("TAK/password")));
        QCOMPARE(Credential::load(account), QStringLiteral("unit-test-armed-password"));
        QCOMPARE(tak.passwordStorageText(), QStringLiteral("Password saved in %1. The readable copy that an earlier version kept in the settings file has been deleted.").arg(Credential::storeName()));
    }
    Credential::setArmedProbe(nullptr);
    Credential::erase(account);
}

namespace {
void seedPlainPassword(const QString &value)
{
    QSettings s;
    s.setValue(QStringLiteral("TAK/password"), value);
    s.sync();
}
bool plainPasswordInSettings()
{
    QSettings s;
    return s.contains(QStringLiteral("TAK/password"));
}
} // namespace

void TakManagerTest::_pendingPasswordCleared()
{
    if (!Credential::inOsStore()) {
        QSKIP("no system key store here: the plain value is removed at once by design");
    }
    const QString account = TakManager::passwordAccount();
    Credential::erase(account);
    seedPlainPassword(QStringLiteral("unit-test-old-password"));
    Credential::setArmedProbe([]() { return true; });
    {
        TakManager tak;
        QVERIFY(tak._plainPasswordPending);
        // The operator clears the field and settings are saved, still armed.
        tak._password.clear();
        tak._persist();
        QVERIFY(!tak._plainPasswordPending);
        QVERIFY(!plainPasswordInSettings());
        QVERIFY(tak.passwordStorageText().isEmpty());
    }
    // Restart, disarmed: no password anywhere.
    Credential::setArmedProbe([]() { return false; });
    {
        TakManager tak;
        QVERIFY(tak._password.isEmpty());
        QVERIFY(!plainPasswordInSettings());
    }
    Credential::dropCache();
    QVERIFY(Credential::load(account).isEmpty());
    Credential::setArmedProbe(nullptr);
}

void TakManagerTest::_pendingPasswordChanged()
{
    if (!Credential::inOsStore()) {
        QSKIP("no system key store here: the plain value is removed at once by design");
    }
    const QString account = TakManager::passwordAccount();
    Credential::erase(account);
    seedPlainPassword(QStringLiteral("unit-test-old-password"));
    Credential::setArmedProbe([]() { return true; });
    {
        TakManager tak;
        QVERIFY(tak._plainPasswordPending);
        // A new password typed while still armed: the store refuses it, and
        // the old plain copy is removed, not kept.
        tak._password = QStringLiteral("unit-test-new-password");
        tak._persist();
        QVERIFY(!tak._plainPasswordPending);
        QVERIFY(!plainPasswordInSettings());
        QVERIFY(tak.passwordStorageText().contains(QStringLiteral("kept for this session only")));

        // Disarmed: the next save stores the new one.
        Credential::setArmedProbe([]() { return false; });
        tak._persist();
        QCOMPARE(Credential::load(account), QStringLiteral("unit-test-new-password"));
        QVERIFY(!plainPasswordInSettings());
    }
    Credential::setArmedProbe(nullptr);
    Credential::erase(account);
}

void TakManagerTest::_keyReplaceFailureCleansUp()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // The target cannot be replaced: a folder is in its place.
    const QString key = dir.filePath(QStringLiteral("client_key.pem"));
    QVERIFY(QDir().mkpath(key));
    QString error;
    QVERIFY(!TakManager::_writePemFile(key, QByteArrayLiteral("unit-test"), true, &error));
    QVERIFY2(error.startsWith(QStringLiteral("could not replace the key file")), qPrintable(error));
    QVERIFY(!QFile::exists(key + QStringLiteral(".new")));
}

void TakManagerTest::_disconnectClearsReceive()
{
    TakManager tak;
    tak._gcsUid = QStringLiteral("rotorlab-r5test00001");
    QByteArray burst;
    for (int i = 0; i < TakManager::kMaxRxEventsPerPass * 2; ++i) {
        burst += QStringLiteral("<event version=\"2.0\" uid=\"ANDROID-r5-%1\" type=\"a-f-G-U-C\" how=\"h-e\">"
                                "<point lat=\"1\" lon=\"2\" hae=\"0\" ce=\"1\" le=\"1\"/></event>")
                     .arg(i).toUtf8();
    }
    tak._consumeRx(burst);
    QVERIFY(!tak._rxBuf.isEmpty());
    tak.disconnectServer();
    QVERIFY(tak._rxBuf.isEmpty());
    // The drain already scheduled finds nothing and does not reschedule.
    QTest::qWait(100);
    QVERIFY(tak._rxBuf.isEmpty());
    QVERIFY(!tak._rxDrainScheduled);
}

void TakManagerTest::_storeRefusalNamesItsCause()
{
    if (!Credential::inOsStore()) {
        QSKIP("no system key store here");
    }
#ifdef Q_OS_WIN
    const QString account = TakManager::passwordAccount();
    Credential::erase(account);
    // Over Windows Credential Manager's 2560-byte limit: refused with no
    // aircraft armed. The note must not name the armed rule.
    const QString tooLong(3000, QLatin1Char('x'));
    Credential::setArmedProbe([]() { return false; });
    seedPlainPassword(tooLong);
    {
        TakManager tak;
        QVERIFY(tak._plainPasswordPending);
        QVERIFY(!tak.passwordStorageText().contains(QStringLiteral("armed")));
        QVERIFY(tak.passwordStorageText().contains(QStringLiteral("did not accept it")));
        QVERIFY(tak.passwordStorageText().contains(QStringLiteral("stays in the settings file")));
        QVERIFY(plainPasswordInSettings());

        // Session-only note: a new over-long password typed this session.
        tak._password = tooLong + QStringLiteral("y");
        tak._persist();
        QVERIFY(!tak.passwordStorageText().contains(QStringLiteral("armed")));
        QVERIFY(tak.passwordStorageText().contains(QStringLiteral("kept for this session only")));
        QVERIFY(tak.passwordStorageText().contains(QStringLiteral("did not accept it")));

        // The same refusal while an aircraft is armed names the armed rule.
        Credential::setArmedProbe([]() { return true; });
        tak._password = QStringLiteral("unit-test-short");
        tak._persist();
        QVERIFY(tak.passwordStorageText().contains(QStringLiteral("while an aircraft is armed")));
    }
    // Pending while armed names the armed rule.
    seedPlainPassword(QStringLiteral("unit-test-old-password"));
    {
        TakManager tak;
        QVERIFY(tak.passwordStorageText().contains(QStringLiteral("while an aircraft is armed")));
    }
    Credential::setArmedProbe(nullptr);
    Credential::erase(account);
#else
    QSKIP("the size refusal is specific to Windows Credential Manager");
#endif
}
