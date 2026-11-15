#include "TakManagerTest.h"

#include "tak/TakDefaults.h"
#include "tak/TakManager.h"

#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMap>
#include <QtCore/QSettings>
#include <QtCore/QXmlStreamReader>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

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
    //    aircraft lookup (no aircraft connected here).
    tak.addApprovedPilotAs(TakPolicy::testSenderUid(), QStringLiteral("uid"));
    line = tak.injectTestCommand(QStringLiteral("HOLD 1"));
    QVERIFY2(line.contains(QStringLiteral("no matching tracks")), qPrintable(line));

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
    tak._callsignPrefix = QStringLiteral("ROTOR");
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
             QStringLiteral("ROTOR-GCS"));
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

void TakManagerTest::_ownUidsExact()
{
    TakManager tak;
    tak._callsignPrefix = QStringLiteral("ROTOR");
    tak._onEncrypted();
    tak._ownUids.insert(QStringLiteral("ROTOR-1"));   // a drone this station published
    auto ev = [](const QString &uid) {
        return QStringLiteral("<event version=\"2.0\" uid=\"%1\" type=\"a-f-A-M-H-Q\" how=\"m-g\">"
                              "<point lat=\"1\" lon=\"2\" hae=\"0\" ce=\"9\" le=\"9\"/><detail/></event>")
            .arg(uid).toUtf8();
    };
    QSignalSpy spy(&tak, &TakManager::cotReceivedChanged);
    tak._noteReceived(ev(tak._gcsUid));
    tak._noteReceived(ev(QStringLiteral("ROTOR-1")));
    QVERIFY(!tak.cotReceivedSinceConnect());
    // Another station with the same prefix is CoT received.
    tak._noteReceived(ev(QStringLiteral("ROTOR-2")));
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
