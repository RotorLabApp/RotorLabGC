#include "TakPolicyTest.h"

#include "tak/TakDefaults.h"
#include "tak/TakPolicy.h"

#include <QtCore/QDebug>
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
    // T16: the target as the marker names it.
    QVERIFY(Tak::parseCommand(QStringLiteral("ARM 2,3"), cmd, err));
    QCOMPARE(intendedTargetText(cmd), QStringLiteral("aircraft 2, 3 (no station named)"));
    QVERIFY(Tak::parseCommand(QStringLiteral("ARM ROTOR-2,3"), cmd, err));
    QCOMPARE(intendedTargetText(cmd), QStringLiteral("aircraft ROTOR-2, ROTOR-3"));
    QVERIFY(Tak::parseCommand(QStringLiteral("RTL ALL"), cmd, err));
    QCOMPARE(intendedTargetText(cmd), QStringLiteral("all aircraft (no station named)"));
    QVERIFY(Tak::parseCommand(QStringLiteral("RTL rotor-all"), cmd, err));
    QCOMPARE(intendedTargetText(cmd), QStringLiteral("all aircraft of ROTOR"));
    QVERIFY(Tak::parseCommand(QStringLiteral("HOLD SEL"), cmd, err));
    QCOMPARE(intendedTargetText(cmd), QStringLiteral("SEL (not accepted)"));
    QVERIFY(Tak::parseCommand(QStringLiteral("HOLD HAWK"), cmd, err));
    QCOMPARE(intendedTargetText(cmd), QStringLiteral("HAWK"));

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

// ---------------------------------------------------------------------------
// G47, G48: addressing. A routing rule, not authentication.
// ---------------------------------------------------------------------------

void TakPolicyTest::_addressGrammar()
{
    Tak::Command cmd;
    QString err;

    // The name before the number, in every spelling the parser accepts.
    for (const QString &text : { QStringLiteral("GOTO ROTOR-2 ALT 150"), QStringLiteral("goto rotor 2 alt 150"),
                                 QStringLiteral("GOTO_ROTOR_2_ALT_150"), QStringLiteral("GOTO-ROTOR-2 ALT 150") }) {
        QVERIFY2(Tak::parseCommand(text, cmd, err), qPrintable(text + QStringLiteral(": ") + err));
        QCOMPARE(cmd.verb, Tak::Verb::Goto);
        QCOMPARE(cmd.name, QStringLiteral("ROTOR"));
        QCOMPARE(cmd.target, Tak::Target::Ids);
        QCOMPARE(cmd.ids, QList<int>{ 2 });
        QCOMPARE(cmd.altFt, 150);
    }
    QVERIFY(Tak::parseCommand(QStringLiteral("LAND ROTOR-1,3"), cmd, err));
    QCOMPARE(cmd.name, QStringLiteral("ROTOR"));
    QCOMPARE(cmd.ids, (QList<int>{ 1, 3 }));
    QVERIFY(Tak::parseCommand(QStringLiteral("RTL ROTOR-ALL"), cmd, err));
    QCOMPARE(cmd.target, Tak::Target::All);
    QCOMPARE(cmd.name, QStringLiteral("ROTOR"));
    // A Fleet callsign alone, and one that ends in a number.
    QVERIFY(Tak::parseCommand(QStringLiteral("GOTO HAWK ALT 100"), cmd, err));
    QCOMPARE(cmd.target, Tak::Target::Name);
    QCOMPARE(cmd.name, QStringLiteral("HAWK"));
    QCOMPARE(cmd.altFt, 100);
    QVERIFY(Tak::parseCommand(QStringLiteral("HOLD Red Eagle 1"), cmd, err));
    QCOMPARE(cmd.name, QStringLiteral("RED-EAGLE"));
    QCOMPARE(cmd.ids, QList<int>{ 1 });
    // SEL parses, so the station can refuse it with its own reason (G48).
    QVERIFY(Tak::parseCommand(QStringLiteral("HOLD SEL"), cmd, err));
    QCOMPARE(cmd.target, Tak::Target::Sel);
    QVERIFY(cmd.name.isEmpty());
    QVERIFY(Tak::parseCommand(QStringLiteral("HOLD ROTOR-SEL"), cmd, err));
    QCOMPARE(cmd.target, Tak::Target::Sel);
    QCOMPARE(cmd.name, QStringLiteral("ROTOR"));
    // No name: still parses, with an empty name (the station refuses it).
    QVERIFY(Tak::parseCommand(QStringLiteral("GOTO 2"), cmd, err));
    QVERIFY(cmd.name.isEmpty());
    QCOMPARE(cmd.ids, QList<int>{ 2 });

    // Bounds and errors.
    QVERIFY(!Tak::parseCommand(QStringLiteral("HOLD A B C D E 1"), cmd, err));
    QCOMPARE(err, QStringLiteral("name has more than 4 words"));
    QVERIFY(!Tak::parseCommand(QStringLiteral("HOLD ") + QString(41, QLatin1Char('A')), cmd, err));
    QCOMPARE(err, QStringLiteral("name longer than 40 characters"));
    QVERIFY(!Tak::parseCommand(QStringLiteral("GOTO 1 ROTOR"), cmd, err));
    QCOMPARE(err, QStringLiteral("bad token ROTOR"));
    QVERIFY(!Tak::parseCommand(QStringLiteral("GOTO ALT 150"), cmd, err));
    QCOMPARE(err, QStringLiteral("bad track id ALT"));
    QVERIFY(!Tak::parseCommand(QStringLiteral("GOTO ROTOR--2"), cmd, err));
    QCOMPARE(err, QStringLiteral("bad track id -2"));
    QVERIFY(!Tak::parseCommand(QStringLiteral("GOTO ROTOR-2 ALT -150"), cmd, err));

    QCOMPARE(Tak::normalizeName(QStringLiteral("  red_eagle  1 ")), QStringLiteral("RED-EAGLE-1"));
    QVERIFY(Tak::isReservedWord(QStringLiteral("HOME")));
    QVERIFY(Tak::isReservedWord(QStringLiteral("GCS")));
    QVERIFY(!Tak::isReservedWord(QStringLiteral("ROTOR")));
}

void TakPolicyTest::_prefixFormat()
{
    // ROTOR, the old shared default, in any case.
    QCOMPARE(prefixProblem(QStringLiteral("ROTOR")), sharedDefaultPrefixReason());
    QCOMPARE(prefixProblem(QStringLiteral("rotor")), sharedDefaultPrefixReason());
    QCOMPARE(prefixProblem(QStringLiteral("Rotor")), sharedDefaultPrefixReason());
    QVERIFY(isSharedDefaultPrefix(QStringLiteral(" rotor ")));
    QVERIFY(!isSharedDefaultPrefix(QStringLiteral("ROTOR2")));
    QVERIFY(prefixProblem(QStringLiteral("ROTOR2")).isEmpty());
    QVERIFY(prefixProblem(QStringLiteral("RL3F9A")).isEmpty());
    QVERIFY(prefixProblem(QStringLiteral("Alpha2")).isEmpty());
    QVERIFY(prefixProblem(QStringLiteral("AB")).isEmpty());
    QVERIFY(prefixProblem(QStringLiteral("ABCDEFGHIJKL")).isEmpty());     // 12
    QVERIFY(!prefixProblem(QStringLiteral("ABCDEFGHIJKLM")).isEmpty());   // 13
    QVERIFY(!prefixProblem(QStringLiteral("R")).isEmpty());
    QVERIFY(!prefixProblem(QString()).isEmpty());
    QVERIFY(!prefixProblem(QStringLiteral(" ")).isEmpty());
    QVERIFY(!prefixProblem(QStringLiteral("1ROTOR")).isEmpty());
    QVERIFY(!prefixProblem(QStringLiteral("ROTOR-1")).isEmpty());
    QVERIFY(!prefixProblem(QStringLiteral("RO TOR")).isEmpty());
    QVERIFY(!prefixProblem(QStringLiteral("ROTOR_A")).isEmpty());
    for (const char *word : { "GOTO", "goto", "HOME", "Launch", "ALL", "SEL", "ALT", "SPD", "GCS", "SAFE" }) {
        QCOMPARE(prefixProblem(QLatin1String(word)),
                 QStringLiteral("%1 is a command word").arg(QString::fromLatin1(word).toUpper()));
    }
}

void TakPolicyTest::_fleetCallsignNameable()
{
    const QString p = QStringLiteral("ROTOR");
    QVERIFY(fleetCallsignNameable(QStringLiteral("HAWK"), p));
    QVERIFY(fleetCallsignNameable(QStringLiteral("Red Eagle 1"), p));
    // Shaped like any station's aircraft name (security M3): not nameable,
    // whether or not that station is seen.
    QVERIFY(!fleetCallsignNameable(QStringLiteral("hawk-2"), p));
    QVERIFY(!fleetCallsignNameable(QStringLiteral("BRAVO 1"), p));
    QVERIFY(!fleetCallsignNameable(QStringLiteral("Hawk1-12"), p));
    QVERIFY(fleetCallsignNameable(QStringLiteral("RED HAWK 1"), p));   // a prefix is one word
    QVERIFY(!fleetCallsignNameable(QString(), p));
    QVERIFY(!fleetCallsignNameable(QStringLiteral("ROTOR-2"), p));     // reads as this station's aircraft 2
    QVERIFY(!fleetCallsignNameable(QStringLiteral("rotor 2"), p));
    QVERIFY(!fleetCallsignNameable(QStringLiteral("ROTOR"), p));
    QVERIFY(!fleetCallsignNameable(QStringLiteral("1234"), p));
    QVERIFY(!fleetCallsignNameable(QStringLiteral("1 HAWK"), p));      // a number before the last word
    QVERIFY(!fleetCallsignNameable(QStringLiteral("HAWK ALT"), p));
    QVERIFY(!fleetCallsignNameable(QStringLiteral("HAWK-ALL"), p));
    QVERIFY(!fleetCallsignNameable(QStringLiteral("HAWK SEL"), p));
    QVERIFY(!fleetCallsignNameable(QStringLiteral("HAWK-GCS"), p));
    QVERIFY(!fleetCallsignNameable(QStringLiteral("HAWK,2"), p));
    QVERIFY(!fleetCallsignNameable(QStringLiteral("HAWK.2"), p));
    QVERIFY(!fleetCallsignNameable(QStringLiteral("A B C D E"), p));   // five words
    QCOMPARE(stationPrefixOf(QStringLiteral("Bravo-GCS")), QStringLiteral("BRAVO"));
    QCOMPARE(stationPrefixOf(QStringLiteral("BRAVO GCS")), QStringLiteral("BRAVO"));
    QVERIFY(stationPrefixOf(QStringLiteral("BRAVO-1")).isEmpty());
    QVERIFY(stationPrefixOf(QStringLiteral("WOLF")).isEmpty());
}

void TakPolicyTest::_classifyAddress()
{
    AddressInput in;
    in.ownPrefix  = QStringLiteral("Rotor");
    in.fleetNames = { { QStringLiteral("HAWK"), 3 }, { QStringLiteral("RED-EAGLE-1"), 4 } };
    in.otherStations = { QStringLiteral("BRAVO") };
    in.otherAircraft = { { QStringLiteral("FALCON"), QStringLiteral("BRAVO") } };

    auto kind = [&in](const QString &text, AddressResult *out = nullptr) {
        Tak::Command cmd;
        QString err;
        if (!Tak::parseCommand(text, cmd, err)) {
            qWarning() << text << err;
            return Address::Unaddressed;
        }
        const AddressResult a = classifyAddress(cmd, in);
        if (out) {
            *out = a;
        }
        return a.kind;
    };
    AddressResult a;
    QCOMPARE(kind(QStringLiteral("HOLD ROTOR-2"), &a), Address::Own);
    QCOMPARE(a.ids, QList<int>{ 2 });
    QCOMPARE(a.name, QStringLiteral("ROTOR-2"));
    QCOMPARE(kind(QStringLiteral("LAND rotor 1,3"), &a), Address::Own);
    QCOMPARE(a.ids, (QList<int>{ 1, 3 }));
    QCOMPARE(kind(QStringLiteral("RTL ROTOR-ALL"), &a), Address::Own);
    QVERIFY(a.all);
    QCOMPARE(kind(QStringLiteral("GOTO HAWK"), &a), Address::Own);
    QCOMPARE(a.ids, QList<int>{ 3 });
    QCOMPARE(kind(QStringLiteral("HOLD red eagle 1"), &a), Address::Own);
    QCOMPARE(a.ids, QList<int>{ 4 });

    QCOMPARE(kind(QStringLiteral("HOLD SEL")), Address::Sel);
    QCOMPARE(kind(QStringLiteral("HOLD ROTOR-SEL")), Address::Sel);
    QCOMPARE(kind(QStringLiteral("HOLD 2")), Address::Unaddressed);
    QCOMPARE(kind(QStringLiteral("HOLD 1,2")), Address::Unaddressed);
    QCOMPARE(kind(QStringLiteral("RTL ALL")), Address::Unaddressed);
    QCOMPARE(kind(QStringLiteral("HOLD ROTOR")), Address::NoAircraft);
    QCOMPARE(kind(QStringLiteral("HOLD BRAVO-2"), &a), Address::OtherStation);
    QCOMPARE(a.station, QStringLiteral("BRAVO"));
    QCOMPARE(a.name, QStringLiteral("BRAVO-2"));
    QCOMPARE(kind(QStringLiteral("RTL BRAVO-ALL"), &a), Address::OtherStation);
    QCOMPARE(a.name, QStringLiteral("BRAVO-ALL"));
    QCOMPARE(kind(QStringLiteral("GOTO FALCON"), &a), Address::OtherStation);
    QCOMPARE(a.station, QStringLiteral("BRAVO"));
    QCOMPARE(kind(QStringLiteral("HOLD CHARLIE-1")), Address::UnknownStation);
    QCOMPARE(kind(QStringLiteral("HOLD ROTR-1")), Address::UnknownStation);
    QCOMPARE(kind(QStringLiteral("GOTO OWL")), Address::UnknownStation);

    // A Fleet callsign another station's aircraft also shows.
    in.fleetConflicts = { QStringLiteral("HAWK") };
    QCOMPARE(kind(QStringLiteral("GOTO HAWK")), Address::FleetConflict);
    in.fleetConflicts.clear();
    in.otherAircraft.insert(QStringLiteral("HAWK"), QStringLiteral("BRAVO"));
    QCOMPARE(kind(QStringLiteral("GOTO HAWK")), Address::FleetConflict);
    in.otherAircraft.remove(QStringLiteral("HAWK"));
    // A Fleet callsign that reads as another station's aircraft.
    // A name alone that is another station's prefix (security S6).
    in.fleetNames.insert(QStringLiteral("BRAVO"), 6);
    QCOMPARE(kind(QStringLiteral("GOTO BRAVO")), Address::FleetConflict);
    in.fleetNames.insert(QStringLiteral("BRAVO-7"), 5);
    QCOMPARE(kind(QStringLiteral("HOLD BRAVO-7")), Address::FleetConflict);

    // The marker text and its send-to must name the same aircraft (T11).
    Tak::Command cmd;
    QString err;
    QVERIFY(Tak::parseCommand(QStringLiteral("HOLD ROTOR-1"), cmd, err));
    cmd.sentTo = QStringLiteral("ROTOR-2");
    cmd.sentToId = 2;
    QCOMPARE(classifyAddress(cmd, in).kind, Address::Mismatch);
    cmd.sentToId = 1;
    QCOMPARE(classifyAddress(cmd, in).kind, Address::Own);
    QVERIFY(Tak::parseCommand(QStringLiteral("RTL ROTOR-ALL"), cmd, err));
    cmd.sentToId = 1;
    QCOMPARE(classifyAddress(cmd, in).kind, Address::Mismatch);
}

// Every addressing text, pinned (legal, regulatory and security review).
void TakPolicyTest::_addressTexts()
{
    const QString p = QStringLiteral("ROTOR");
    auto refusal = [&p](const QString &text, AddressInput in = AddressInput()) {
        in.ownPrefix = p;
        Tak::Command cmd;
        QString err;
        if (!Tak::parseCommand(text, cmd, err)) {
            return QStringLiteral("parse failed: ") + err;
        }
        return addressRefusal(classifyAddress(cmd, in), cmd, p);
    };
    // T1
    const QString t1 = refusal(QStringLiteral("GOTO 2 ALT 150"));
    QCOMPARE(t1, QStringLiteral("the marker does not name a station, so this station does not run it. If it was "
                                "meant for an aircraft of this station, the marker must name it, as in GOTO ROTOR-2"));
    QCOMPARE(notSentResult(refusal(QStringLiteral("land 1,3"))),
             QStringLiteral("Not sent: the marker does not name a station, so this station does not run it. If it "
                            "was meant for an aircraft of this station, the marker must name it, as in LAND ROTOR-1"));
    // T2
    const QString t2 = refusal(QStringLiteral("RTL ALL"));
    QCOMPARE(t2, QStringLiteral("ALL does not name a station, so this station does not run it. ROTOR-ALL names "
                                "every aircraft of this station"));
    // T3
    const QString t3 = refusal(QStringLiteral("HOLD SEL"));
    QCOMPARE(t3, QStringLiteral("SEL is not accepted from ATAK, so this station does not run it. The aircraft "
                                "selected on this station are chosen here and can change at any time, so a marker "
                                "cannot say which aircraft it reaches. A marker must name each aircraft by its "
                                "number, as in HOLD ROTOR-1 for aircraft 1"));
    QCOMPARE(refusal(QStringLiteral("HOLD ROTOR-SEL")), t3);
    // T4
    const QString t4 = refusal(QStringLiteral("HOLD CHARLIE-1"));
    QCOMPARE(t4, QStringLiteral("no station or aircraft named CHARLIE is seen on this TAK server, so this station "
                                "does not run it. This station's prefix is ROTOR"));
    // T5
    AddressInput bravo;
    bravo.otherStations = { QStringLiteral("BRAVO") };
    const QString t5 = refusal(QStringLiteral("HOLD BRAVO-2"), bravo);
    QCOMPARE(t5, QStringLiteral("addressed to BRAVO-2, which station BRAVO publishes, not this station (ROTOR)"));
    QCOMPARE(markerIgnoredLine(otherStationReason(QStringLiteral("BRAVO-2"), QStringLiteral("BRAVO"), p),
                               QStringLiteral("HOLD BRAVO-2"), QStringLiteral("u-1")),
             QStringLiteral("marker ignored: addressed to BRAVO-2, which station BRAVO publishes, not this station "
                            "(ROTOR) (\"HOLD BRAVO-2\", UID u-1)"));
    // T6
    QCOMPARE(otherStationEarlierReason(),
             QStringLiteral("this marker was addressed to another station earlier this session; a changed or "
                            "repeated marker is not run here"));
    // T7, T8, T9
    QCOMPARE(prefixConflictRefusal(p),
             QStringLiteral("another station on this TAK server also uses the callsign prefix ROTOR, so a marker "
                            "cannot say which station it is for. Each station needs its own prefix (TAK settings)"));
    QCOMPARE(prefixConflictNotice(QStringLiteral("rotor"), QStringLiteral("14:05:09")),
             QStringLiteral("Commands from ATAK markers are not sent to the aircraft: another station on this TAK "
                            "server also uses the callsign prefix ROTOR (last seen 14:05:09 UTC). Give each station "
                            "its own prefix under TAK / OpenTAKServer."));
    QCOMPARE(prefixConflictFlyLine(p),
             QStringLiteral("ATAK commands not sent to the aircraft: another station uses prefix ROTOR (TAK settings)"));
    // T10
    AddressInput hawk;
    hawk.fleetNames = { { QStringLiteral("HAWK"), 3 } };
    hawk.fleetConflicts = { QStringLiteral("HAWK") };
    const QString t10 = refusal(QStringLiteral("GOTO HAWK"), hawk);
    QCOMPARE(t10, QStringLiteral("HAWK is also the callsign of an aircraft another station publishes. Name the "
                                 "aircraft as ROTOR-3"));
    // T11
    QString t11;
    {
        Tak::Command cmd;
        QString err;
        QVERIFY(Tak::parseCommand(QStringLiteral("HOLD ROTOR-1"), cmd, err));
        cmd.sentTo = QStringLiteral("ROTOR-2");
        cmd.sentToId = 2;
        AddressInput in;
        in.ownPrefix = p;
        t11 = addressRefusal(classifyAddress(cmd, in), cmd, p);
        QCOMPARE(t11, QStringLiteral("the marker text names ROTOR-1 but the marker was sent to ROTOR-2, so it is not "
                                     "clear which aircraft it is for, and this station does not run it. A marker sent "
                                     "to an aircraft must name that same aircraft"));
    }
    // T12, T12b, T13
    QCOMPARE(prefixEntryRefusal(),
             QStringLiteral("Not saved: use 2 to 12 letters and digits, starting with a letter. Command words such "
                            "as GOTO, ALL, SEL, ALT, SPD and GCS cannot be a prefix."));
    // T12c
    QCOMPARE(sharedDefaultPrefixReason(),
             QStringLiteral("ROTOR was the shared default in earlier versions; choose a prefix only this station "
                            "uses."));
    QCOMPARE(sharedDefaultPrefixRefusal(),
             QStringLiteral("Not saved: ROTOR was the shared default in earlier versions; choose a prefix only this "
                            "station uses."));
    QCOMPARE(prefixSeenRefusal(QStringLiteral("bravo")),
             QStringLiteral("Not saved: another station on this TAK server used the callsign prefix BRAVO in the last "
                            "2 minutes. Choose a prefix no other station uses."));
    QCOMPARE(prefixInvalidNotice(QStringLiteral("R-1")),
             QStringLiteral("Commands are not read: the callsign prefix R-1 cannot name this station. Set a prefix "
                            "of 2 to 12 letters and digits under TAK / OpenTAKServer."));
    QCOMPARE(prefixInvalidReason(),
             QStringLiteral("the callsign prefix cannot name this station, so this station reads no commands"));
    // T14, T14b
    QCOMPARE(prefixChangedLine(QStringLiteral("ROTOR"), QStringLiteral("ALPHA")),
             QStringLiteral("Callsign prefix changed from ROTOR to ALPHA. \"Commands to aircraft on\" was turned "
                            "off. Markers placed before the change are not sent to the aircraft."));
    QCOMPARE(prefixFloorReason(), QStringLiteral("placed before this station's callsign prefix changed; not run"));
    // T15
    QCOMPARE(fleetNotNameableNote(QStringLiteral(" BRAVO 1 "), p, 3),
             QStringLiteral("Markers cannot name this aircraft by BRAVO 1. In a marker, use ROTOR-3."));
    // T17
    QCOMPARE(addressingHelp(QStringLiteral("rotor")),
             QStringLiteral("A marker names the aircraft as this station publishes it to TAK: ROTOR-1, ROTOR-1,3, "
                            "ROTOR-ALL, or the aircraft's Fleet callsign. A marker that names no station, or names "
                            "SEL, is not sent to any aircraft by this station; another station on the server may "
                            "still act on it. The name only routes a marker to a station. It does not show who "
                            "placed the marker, and anyone on this TAK server can write any name."));
    // T18
    const QString t18 = refusal(QStringLiteral("HOLD ROTOR"));
    QCOMPARE(t18, QStringLiteral("the marker names this station (ROTOR) but no aircraft, so this station does not "
                                 "run it. A marker must name the aircraft by its number, as in HOLD ROTOR-1 for "
                                 "aircraft 1"));
    // T19, T19 reason, T19c (security M1)
    QCOMPARE(prefixNotSetNotice(QStringLiteral("RL3F9A")),
             QStringLiteral("Commands are not read: the callsign prefix RL3F9A has not been set on this station. "
                            "Each station on a TAK server needs its own prefix. Set or confirm it under TAK / "
                            "OpenTAKServer."));
    QCOMPARE(prefixNotSetReason(),
             QStringLiteral("the callsign prefix has not been set on this station, so this station reads no commands"));
    QCOMPARE(prefixNotSetArmLine(),
             QStringLiteral("\"Commands to aircraft on\" not turned on: set this station's callsign prefix first "
                            "(TAK settings)"));
    // T20 (was "no matching tracks")
    QCOMPARE(noAircraftConnected(QStringLiteral("ROTOR-3"), false),
             QStringLiteral("no aircraft ROTOR-3 is connected to this station"));
    QCOMPARE(noAircraftConnected(QStringLiteral("ROTOR-1"), true),
             QStringLiteral("none of the named aircraft is connected to this station"));

    // The default prefix comes from the random station uid only.
    QCOMPARE(defaultPrefixFor(QStringLiteral("rotorlab-0123456789ab")), QStringLiteral("RL89AB"));
    QVERIFY(defaultPrefixFor(QStringLiteral("rotorlab-0123456789ab"))
            != defaultPrefixFor(QStringLiteral("rotorlab-0123456789cd")));
    QVERIFY(prefixProblem(defaultPrefixFor(QStringLiteral("rotorlab-ffffffffffff"))).isEmpty());

    // Guidance only: no addressing text says a flight may go, and none says
    // addressing makes a command secure, authenticated or safe.
    const QStringList all = {
        t1, t2, t3, t4, t5, otherStationEarlierReason(), prefixConflictRefusal(p),
        prefixConflictNotice(p, QStringLiteral("00:00:00")), prefixConflictFlyLine(p), t10, t11,
        prefixEntryRefusal(), sharedDefaultPrefixReason(), sharedDefaultPrefixRefusal(), prefixSeenRefusal(p), prefixInvalidNotice(p), prefixInvalidReason(),
        prefixChangedLine(p, p), prefixFloorReason(), fleetNotNameableNote(p, p, 1), addressingHelp(p), t18,
        prefixNotSetNotice(p), prefixNotSetReason(), prefixNotSetArmLine(),
        noAircraftConnected(p, false), noAircraftConnected(p, true) };
    for (const QString &t : all) {
        for (const char *w : { "secure", "authentic", "verified", "safe", "cleared", "may fly", "may go",
                               "authorized", "approved", "permitted", "protect", "prevent", "ensure",
                               "guarantee" }) {
            QVERIFY2(!t.contains(QLatin1String(w), Qt::CaseInsensitive), qPrintable(t));
        }
        QVERIFY2(!t.contains(QChar(0x2014)), qPrintable(t));   // no em dash
    }
}
