#include "TakPolicyTest.h"

#include "tak/TakDefaults.h"
#include "tak/TakPolicy.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtTest/QTest>

using namespace TakPolicy;

void TakPolicyTest::_alertText()
{
    QCOMPARE(alertTitle(), QStringLiteral("Remote command received"));
    QCOMPARE(alertDisclaimer(),
             QStringLiteral("Sent to the aircraft when received. This alert does not hold or cancel it. "
                            "Sender identity is as reported by ATAK and is not verified by this station. "
                            "Sent means transmitted to the aircraft. The aircraft's response is not shown here."));

    QCOMPARE(senderText(QStringLiteral("WOLF"), QStringLiteral("ANDROID-1")),
             QStringLiteral("WOLF (UID ANDROID-1)"));
    QCOMPARE(senderText(QStringLiteral("WOLF"), {}), QStringLiteral("WOLF"));
    QCOMPARE(senderText({}, QStringLiteral("ANDROID-1")), QStringLiteral("UID ANDROID-1"));
    QCOMPARE(senderText({}, {}), QStringLiteral("(sender not reported)"));

    Tak::Command cmd;
    QString err;
    QVERIFY(Tak::parseCommand(QStringLiteral("ARM 2,3"), cmd, err));
    QCOMPARE(intendedTargetText(cmd), QStringLiteral("aircraft 2, 3"));
    QVERIFY(Tak::parseCommand(QStringLiteral("RTL ALL"), cmd, err));
    QCOMPARE(intendedTargetText(cmd), QStringLiteral("all aircraft"));
    QVERIFY(Tak::parseCommand(QStringLiteral("HOLD SEL"), cmd, err));
    QCOMPARE(intendedTargetText(cmd), QStringLiteral("selected aircraft"));

    QCOMPARE(positionText(-35.36, 149.16, 400),
             QStringLiteral("-35.360000, 149.160000 at 400 ft above launch"));
    QCOMPARE(positionText(-35.36, 149.16, -1),
             QStringLiteral("-35.360000, 149.160000 at current altitude"));

    RemoteCommand c;
    c.verb           = QStringLiteral("ARM");
    c.senderCallsign = QStringLiteral("WOLF");
    c.senderUid      = QStringLiteral("ANDROID-1");
    c.targets        = { QStringLiteral("aircraft 1 (HAWK)") };
    c.result         = QStringLiteral("Arm command sent");
    QCOMPARE(summaryLine(c),
             QStringLiteral("ARM, reported sender WOLF (UID ANDROID-1), to aircraft 1 (HAWK): Arm command sent"));
    c.test = true;
    QVERIFY(summaryLine(c).endsWith(QStringLiteral(" [station test]")));

    QCOMPARE(notOnListResult(),
             QStringLiteral("Refused: reported sender is not on this station's ATAK command senders list"));
}

void TakPolicyTest::_prominentVerbs()
{
    QVERIFY(isProminentVerb(Tak::Verb::Arm));
    QVERIFY(isProminentVerb(Tak::Verb::Takeoff));
    QVERIFY(isProminentVerb(Tak::Verb::Goto));
    for (Tak::Verb v : { Tak::Verb::Rtl, Tak::Verb::Land, Tak::Verb::Hold,
                         Tak::Verb::AltHold, Tak::Verb::Guided, Tak::Verb::Disarm }) {
        QVERIFY(!isProminentVerb(v));
    }
}

void TakPolicyTest::_sentResult()
{
    // Says what was sent, never a vehicle state the dispatcher did not read.
    QCOMPARE(sentResultText(Tak::Verb::Arm, true, QStringLiteral("arm sent"), false),
             QStringLiteral("arm command sent"));
    QCOMPARE(sentResultText(Tak::Verb::Land, true, QStringLiteral("landing"), false),
             QStringLiteral("land command sent"));
    QCOMPARE(sentResultText(Tak::Verb::Goto, false, QStringLiteral("the aircraft is not armed"), false),
             QStringLiteral("not sent: telemetry reports the aircraft not armed"));
    QCOMPARE(sentResultText(Tak::Verb::Goto, false, QStringLiteral("GOTO needs a position"), false),
             QStringLiteral("not sent: GOTO needs a position"));
    QCOMPARE(sentResultText(Tak::Verb::Arm, true, QStringLiteral("already armed"), true),
             QStringLiteral("not sent: telemetry reports the aircraft already armed"));

    // One aircraft: no leading number, capitalized.
    QCOMPARE(dispatchResultText({ 3 }, { QStringLiteral("not sent: telemetry reports the aircraft not armed") }, 0),
             QStringLiteral("Not sent: telemetry reports the aircraft not armed"));
    QCOMPARE(dispatchResultText({ 1 }, { QStringLiteral("arm command sent") }, 1),
             QStringLiteral("Arm command sent"));
    // Several aircraft: a count, then each aircraft by its id.
    QCOMPARE(dispatchResultText({ 1, 2, 3 },
                                { QStringLiteral("arm command sent"),
                                  QStringLiteral("not sent: telemetry reports the aircraft already armed"),
                                  QStringLiteral("arm command sent") }, 2),
             QStringLiteral("2 of 3 sent (aircraft 1 arm command sent; aircraft 2 not sent: telemetry reports "
                            "the aircraft already armed; aircraft 3 arm command sent)"));

    // A station setting: "Not sent:". A sender refusal: "Refused:".
    QCOMPARE(notSentResult(QStringLiteral("LAND is disabled")), QStringLiteral("Not sent: LAND is disabled"));
    QVERIFY(notOnListResult().startsWith(QStringLiteral("Refused: ")));
    QVERIFY(testIdentityResult().startsWith(QStringLiteral("Refused: ")));
    QCOMPARE(parseFailedResult(QStringLiteral("missing track id")),
             QStringLiteral("Not sent: command not understood (missing track id)"));
}

void TakPolicyTest::_logRecord()
{
    RemoteCommand c;
    c.verb           = QStringLiteral("GOTO");
    c.markerText     = QStringLiteral("GOTO 1 ALT 520");
    c.senderCallsign = QStringLiteral("WOLF");
    c.senderUid      = QStringLiteral("ANDROID-1");
    c.targets        = { QStringLiteral("aircraft 1") };
    c.position       = positionText(1.0, 2.0, 520);
    c.result         = QStringLiteral("1 go-to command sent");
    c.warning        = altitudeWarning(520, 400);
    const QJsonObject o = logRecord(c);
    QCOMPARE(o.value(QStringLiteral("kind")).toString(), QStringLiteral("command"));
    QCOMPARE(o.value(QStringLiteral("verb")).toString(), QStringLiteral("GOTO"));
    QCOMPARE(o.value(QStringLiteral("reportedSenderCallsign")).toString(), QStringLiteral("WOLF"));
    QCOMPARE(o.value(QStringLiteral("reportedSenderUid")).toString(), QStringLiteral("ANDROID-1"));
    QCOMPARE(o.value(QStringLiteral("target")).toString(), QStringLiteral("aircraft 1"));
    QCOMPARE(o.value(QStringLiteral("result")).toString(), QStringLiteral("1 go-to command sent"));
    QVERIFY(!o.value(QStringLiteral("warning")).toString().isEmpty());
    QCOMPARE(o.value(QStringLiteral("test")).toBool(), false);

    // Chained lines: each carries the hash of the one before it.
    const QByteArray first = chainRecord(o, QString());
    QVERIFY(!first.contains('\n'));
    const QJsonObject f = QJsonDocument::fromJson(first).object();
    QCOMPARE(f.value(QStringLiteral("schemaVersion")).toInt(), kCommandLogSchema);
    QCOMPARE(f.value(QStringLiteral("prevHash")).toString(), QString());
    const QByteArray second = chainRecord(o, lineHash(first));
    QCOMPARE(QJsonDocument::fromJson(second).object().value(QStringLiteral("prevHash")).toString(),
             lineHash(first));
    QCOMPARE(int(lineHash(first).size()), 64);
    QVERIFY(lineHash(first) != lineHash(second));
}

void TakPolicyTest::_testSender()
{
    QCOMPARE(testSenderCallsign(), QStringLiteral("STATION-TEST"));
    QCOMPARE(testSenderUid(), QStringLiteral("rotorlab-station-test"));
    QVERIFY(claimsTestSender(QStringLiteral("station-test"), {}));
    QVERIFY(claimsTestSender({}, QStringLiteral("ROTORLAB-STATION-TEST")));
    QVERIFY(!claimsTestSender(QStringLiteral("WOLF"), QStringLiteral("ANDROID-1")));
}

void TakPolicyTest::_altitudeWarning()
{
    QCOMPARE(altitudeWarning(520, 400),
             QStringLiteral("Commanded altitude 520 ft (above launch) is above this station's warning level of "
                            "400 ft. Above launch is not above ground level."));
    QVERIFY(altitudeWarning(400, 400).isEmpty());   // at the level is not above it
    QVERIFY(altitudeWarning(399, 400).isEmpty());
    QVERIFY(altitudeWarning(520, 0).isEmpty());     // no level set
    QVERIFY(altitudeWarning(-1, 400).isEmpty());    // no ALT in the command
    // A received-command row carries the warning only when the command was sent.
    QCOMPARE(sentAltitudeWarning(true, 520, 400), altitudeWarning(520, 400));
    QVERIFY(sentAltitudeWarning(false, 520, 400).isEmpty());
    const QString w = altitudeWarning(520, 400).toLower();
    for (const char *word : { "illegal", "not allowed", "authoriz", "approv", "clear" }) {
        QVERIFY2(!w.contains(QLatin1String(word)), word);
    }
}

void TakPolicyTest::_openSenderWarning()
{
    const QString w = QStringLiteral("Any sender on this TAK server can command the aircraft.");
    QCOMPARE(openSenderWarning(true, true, false), w);
    QVERIFY(openSenderWarning(true, true, true).isEmpty());
    QVERIFY(openSenderWarning(true, false, false).isEmpty());
    QVERIFY(openSenderWarning(false, true, false).isEmpty());
}

void TakPolicyTest::_pilotList()
{
    QCOMPARE(formatPilotEntry(PilotKind::Uid, QStringLiteral(" ANDROID-1 ")), QStringLiteral("uid:ANDROID-1"));
    QCOMPARE(formatPilotEntry(PilotKind::Callsign, QStringLiteral("WOLF")), QStringLiteral("callsign:WOLF"));
    QCOMPARE(parsePilotEntry(QStringLiteral("UID:ANDROID-1")).kind, PilotKind::Uid);
    QCOMPARE(parsePilotEntry(QStringLiteral("uid:ANDROID-1")).value, QStringLiteral("ANDROID-1"));
    QCOMPARE(parsePilotEntry(QStringLiteral("WOLF")).kind, PilotKind::Any);

    const QString cs  = QStringLiteral("WOLF");
    const QString uid = QStringLiteral("ANDROID-1");
    // A UID entry matches the reported UID only, never a callsign set to it.
    QVERIFY(senderOnList({ QStringLiteral("uid:android-1") }, cs, uid));
    QVERIFY(!senderOnList({ QStringLiteral("uid:ANDROID-1") }, QStringLiteral("ANDROID-1"), QStringLiteral("OTHER")));
    QVERIFY(!senderOnList({ QStringLiteral("uid:WOLF") }, cs, uid));
    // A callsign entry matches the reported callsign only.
    QVERIFY(senderOnList({ QStringLiteral("callsign:wolf") }, cs, uid));
    QVERIFY(!senderOnList({ QStringLiteral("callsign:ANDROID-1") }, cs, uid));
    // An older entry without a prefix matches either.
    QVERIFY(senderOnList({ QStringLiteral("wolf") }, cs, uid));
    QVERIFY(senderOnList({ QStringLiteral("android-1") }, cs, uid));
    // Empty values and empty reports never match.
    QVERIFY(!senderOnList({ QStringLiteral("uid:") }, cs, {}));
    QVERIFY(!senderOnList({ QStringLiteral("uid:ANDROID-1") }, cs, {}));
    QVERIFY(!senderOnList({}, cs, uid));
}

void TakPolicyTest::_serverFingerprint()
{
    const QByteArray der("not really DER, any bytes hash the same way");
    const QString fp = sha256Fingerprint(der);
    QCOMPARE(int(fp.size()), 32 * 3 - 1);                  // "AB:CD:..." 32 bytes
    QCOMPARE(fp, fp.toUpper());
    QVERIFY(sha256Fingerprint({}).isEmpty());
    QVERIFY(fingerprintsMatch(fp, fp.toLower().remove(QLatin1Char(':'))));
    QVERIFY(!fingerprintsMatch(fp, sha256Fingerprint("other")));
    QVERIFY(!fingerprintsMatch({}, {}));

    // Credentials go only over a verified connection.
    // Verify on and the chain verified (system trust store or imported CA).
    QVERIFY(enrollMaySend(true, true, fp, {}));
    // Self-signed or otherwise unverified chain: only with the accepted pin.
    QVERIFY(!enrollMaySend(true, false, fp, {}));
    QVERIFY(enrollMaySend(true, false, fp, fp));
    // Verify off: only with the accepted pin, even if the chain verified.
    QVERIFY(!enrollMaySend(false, true, fp, {}));
    QVERIFY(enrollMaySend(false, false, fp, fp));
    QVERIFY(!enrollMaySend(false, false, fp, sha256Fingerprint("other")));
    QVERIFY(!enrollMaySend(false, false, {}, {}));
}

void TakPolicyTest::_defaults()
{
    QVERIFY(TakDefaults::approvedOnly);
    QVERIFY(TakDefaults::verifyPeer);
    // The new-install table: every verb opt in, no altitude warning level.
    QVERIFY(!TakDefaults::verbGoto);
    QVERIFY(!TakDefaults::verbRtl);
    QVERIFY(!TakDefaults::verbLand);
    QVERIFY(!TakDefaults::verbHold);
    QVERIFY(!TakDefaults::verbAltHold);
    QVERIFY(!TakDefaults::verbGuided);
    QVERIFY(!TakDefaults::verbArm);
    QVERIFY(!TakDefaults::verbDisarm);
    QVERIFY(!TakDefaults::verbTakeoff);
    QCOMPARE(TakDefaults::altWarnAboveFt, 0.0);
}
