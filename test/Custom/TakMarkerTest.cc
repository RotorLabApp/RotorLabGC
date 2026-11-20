#include "TakMarkerTest.h"

#include "tak/CoTMessage.h"
#include "tak/TakCommand.h"
#include "tak/TakPolicy.h"
#include "tak/TakManager.h"
#include "fleet/GuidedDispatch.h"
#include "MultiVehicleManager.h"
#include "QmlObjectListModel.h"
#include "Vehicle.h"

#include <cmath>

#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QSettings>
#include <QtCore/QTimeZone>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

namespace {

void clearTakSettings()
{
    QSettings s;
    s.remove(QStringLiteral("TAK"));
    s.sync();
}

QString iso(const QDateTime &t)
{
    return t.toUTC().toString(Qt::ISODateWithMs);
}

// One inbound CoT event, shaped like what ATAK sends. The defaults are a
// marker placed with ATAK's point dropper by ENGINE-1 (a fictional callsign).
struct Ev {
    QString   uid      = QStringLiteral("8d1f0c2e-5b7a-4c3e-9a51-0e6f2b1d4c11");
    QString   type     = QStringLiteral("a-h-G");
    QString   how      = QStringLiteral("h-g-i-g-o");
    QString   name     = QStringLiteral("HOLD 1");
    QString   dest;                                          ///< <marti><dest callsign>
    QString   linkUid  = QStringLiteral("ANDROID-ENGINE1");  ///< empty: no <link>
    QString   linkRel  = QStringLiteral("p-p");
    QString   linkCs   = QStringLiteral("ENGINE-1");
    bool      eud      = false;                              ///< <takv> + <__group> (a PLI)
    QDateTime time     = QDateTime::currentDateTimeUtc();
    QDateTime stale    = QDateTime::currentDateTimeUtc().addSecs(3600);
    QString   lat      = QStringLiteral("-35.360000");
    QString   lon      = QStringLiteral("149.160000");
};

QByteArray xml(const Ev &e)
{
    QString x = QStringLiteral("<event version=\"2.0\" uid=\"%1\" type=\"%2\" how=\"%3\" time=\"%4\" start=\"%4\" stale=\"%5\">")
                    .arg(e.uid, e.type, e.how, iso(e.time), iso(e.stale));
    x += QStringLiteral("<point lat=\"%1\" lon=\"%2\" hae=\"9999999.0\" ce=\"9999999.0\" le=\"9999999.0\"/>")
             .arg(e.lat, e.lon);
    x += QStringLiteral("<detail>");
    if (e.eud) {
        x += QStringLiteral("<takv os=\"34\" version=\"5.2.0\" device=\"TEST PHONE\" platform=\"ATAK-CIV\"/>");
    }
    if (!e.linkUid.isEmpty()) {
        x += QStringLiteral("<link uid=\"%1\" production_time=\"%2\" type=\"a-f-G-U-C\" parent_callsign=\"%3\" relation=\"%4\"/>")
                 .arg(e.linkUid, iso(e.time), e.linkCs, e.linkRel);
    }
    if (!e.name.isEmpty()) {
        x += QStringLiteral("<contact %1callsign=\"%2\"/>")
                 .arg(e.eud ? QStringLiteral("endpoint=\"*:-1:stcp\" ") : QString(), e.name);
    }
    if (e.eud) {
        x += QStringLiteral("<__group role=\"Team Member\" name=\"Cyan\"/><status battery=\"88\"/>");
    }
    if (!e.dest.isEmpty()) {
        x += QStringLiteral("<marti><dest callsign=\"%1\"/></marti>").arg(e.dest);
    }
    x += QStringLiteral("<archive/></detail></event>");
    return x.toUtf8();
}

// A contact's position report (PLI), as ATAK-CIV sends it every few seconds.
Ev contactReport(const QString &callsign)
{
    Ev e;
    e.uid   = QStringLiteral("ANDROID-0a1b2c3d4e5f6071");
    e.type  = QStringLiteral("a-f-G-U-C");
    e.how   = QStringLiteral("h-e");
    e.name  = callsign;
    e.linkUid.clear();
    e.eud   = true;
    e.stale = QDateTime::currentDateTimeUtc().addSecs(90);
    return e;
}

QStringList ignoredLines(const QSignalSpy &spy)
{
    QStringList out;
    for (const QList<QVariant> &args : spy) {
        const QString line = args.value(0).toString();
        if (line.startsWith(QStringLiteral("marker ignored: "))) {
            out << line;
        }
    }
    return out;
}


// The reasons for a signed ALT or SPD value (RotorLab 5.3.0 review,
// M3), the same for a minus, a hyphen and a plus.
QString negAltReason(const QString &value)
{
    return QStringLiteral("ALT value %1 refused: a minus sign or hyphen before the number makes it negative, "
                          "and a plus sign is not accepted. ALT is feet above launch; write the number with no "
                          "sign, as in ALT 150 or ALT_150").arg(value);
}

QString negSpdReason(const QString &value)
{
    return QStringLiteral("SPD value %1 refused: a minus sign or hyphen before the number makes it negative, "
                          "and a plus sign is not accepted. SPD is a step from 1 to 10 on this station's speed "
                          "scale (TAK settings); write the number with no sign, as in SPD 5 or SPD_5").arg(value);
}
} // namespace

/// A station that reads commands from any sender with "Command By ATAK on"
/// off: every command that reaches the command path is listed as not sent,
/// which is how these tests see that it got there.
void TakMarkerTest::readAll(TakManager &tak)
{
    // These tests use the prefix RLTEST, set on this install (security M1).
    tak._callsignPrefix = QStringLiteral("RLTEST");
    tak._prefixSetFor   = QStringLiteral("RLTEST");
    tak._commandEnabled = true;
    tak._approvedOnly   = false;
    // The per-second and per-minute limits are tested on their own.
    tak._maxCommandsPerSecond = 1000000;
    tak._maxCommandsPerMinute = 1000000;
}

void TakMarkerTest::init()
{
    UnitTest::init();
    clearTakSettings();
}

void TakMarkerTest::cleanup()
{
    clearTakSettings();
    UnitTest::cleanup();
}

// G44: the sender is the p-p (creator) link only. A link of another relation
// names some other entity and must not become the sender.
void TakMarkerTest::_senderFromParentLinkOnly()
{
    const QString now   = iso(QDateTime::currentDateTimeUtc());
    const QString stale = iso(QDateTime::currentDateTimeUtc().addSecs(600));

    const QByteArray otherLinkOnly = QStringLiteral(
        "<event version=\"2.0\" uid=\"m-1\" type=\"a-u-G\" how=\"h-g-i-g-o\" time=\"%1\" start=\"%1\" stale=\"%2\">"
        "<point lat=\"-35.36\" lon=\"149.16\" hae=\"0\" ce=\"9999999\" le=\"9999999\"/>"
        "<detail><contact callsign=\"HOLD 1\"/>"
        "<link uid=\"ANDROID-OTHER\" relation=\"c\" parent_callsign=\"BEAR\" type=\"a-f-G-U-C\"/>"
        "</detail></event>").arg(now, stale).toUtf8();
    CoT::Marker m = CoT::parse(otherLinkOnly);
    QVERIFY(m.valid);
    QVERIFY(!m.hasParentLink);
    QVERIFY(m.senderUid.isEmpty());
    QVERIFY(m.senderCallsign.isEmpty());

    // A non-parent link before the p-p link does not win, and a second p-p
    // link does not replace the first.
    const QByteArray mixed = QStringLiteral(
        "<event version=\"2.0\" uid=\"m-2\" type=\"a-u-G\" how=\"h-g-i-g-o\" time=\"%1\" start=\"%1\" stale=\"%2\">"
        "<point lat=\"-35.36\" lon=\"149.16\" hae=\"0\" ce=\"9999999\" le=\"9999999\"/>"
        "<detail><contact callsign=\"HOLD 1\"/>"
        "<link uid=\"ANDROID-OTHER\" relation=\"c\" parent_callsign=\"BEAR\"/>"
        "<link uid=\"ANDROID-WOLF\" relation=\"p-p\" parent_callsign=\"WOLF\" type=\"a-f-G-U-C\"/>"
        "<link uid=\"ANDROID-FOX\" relation=\"p-p\" parent_callsign=\"FOX\" type=\"a-f-G-U-C\"/>"
        "</detail></event>").arg(now, stale).toUtf8();
    m = CoT::parse(mixed);
    QVERIFY(m.valid);
    QVERIFY(m.hasParentLink);
    QCOMPARE(m.senderUid, QStringLiteral("ANDROID-WOLF"));
    QCOMPARE(m.senderCallsign, QStringLiteral("WOLF"));

    // With the sender list on, a marker whose only link is not p-p is an
    // unknown sender: a BEAR entry does not match BEAR on a non-parent link.
    TakManager tak;
    tak._approvedOnly   = true;
    tak._approvedPilots = { QStringLiteral("BEAR") };
    QString who;
    QVERIFY(!tak._senderApproved(CoT::parse(otherLinkOnly), who));
}

// G38: a command is read only from a marker a person placed. A contact named
// "LAUNCH 1" (its own position report), another station's aircraft track,
// a marker with no creating device, a device's report about itself and a
// stale marker are ignored with a station line. Marker types are not
// filtered: point-dropper (a-h-G) and spot (b-m-p-s-m) markers both work.
void TakMarkerTest::_placedMarkersOnly()
{
    TakManager tak;
    readAll(tak);
    QSignalSpy log(&tak, &TakManager::logMessage);

    // A contact named "LAUNCH 1": its PLI repeats, and is never a command.
    const Ev pli = contactReport(QStringLiteral("LAUNCH 1"));
    tak._handleMarker(xml(pli));
    tak._handleMarker(xml(pli));
    tak._handleMarker(xml(pli));
    QVERIFY(tak.remoteCommands().isEmpty());
    QCOMPARE(ignoredLines(log), QStringList{
        QStringLiteral("marker ignored: device position report, not a placed marker (\"LAUNCH 1\", UID ANDROID-0a1b2c3d4e5f6071)") });

    // The same contact with a machine "how" and no takv: still ignored.
    Ev pliGps = pli;
    pliGps.eud = false;
    pliGps.how = QStringLiteral("m-g");
    tak._handleMarker(xml(pliGps));
    QVERIFY(tak.remoteCommands().isEmpty());
    QVERIFY(ignoredLines(log).last().startsWith(QStringLiteral("marker ignored: machine-generated, not placed by a person")));

    // Another ground station's aircraft track named like a command.
    Ev track;
    track.uid     = QStringLiteral("OTHERGCS-1");
    track.type    = QStringLiteral("a-f-A-M-H-Q");
    track.how     = QStringLiteral("m-g");
    track.name    = QStringLiteral("LAND 1");
    track.linkUid = QStringLiteral("OTHERGCS-GCS");
    tak._handleMarker(xml(track));
    QVERIFY(tak.remoteCommands().isEmpty());

    // No "how" at all.
    Ev noHow;
    noHow.uid = QStringLiteral("nohow-1");
    noHow.how.clear();
    tak._handleMarker(xml(noHow));
    QVERIFY(tak.remoteCommands().isEmpty());
    QVERIFY(ignoredLines(log).last().startsWith(QStringLiteral("marker ignored: not marked as placed by a person")));

    // No creating device, or only a link of another relation.
    Ev orphan;
    orphan.uid = QStringLiteral("orphan-1");
    orphan.linkUid.clear();
    tak._handleMarker(xml(orphan));
    Ev otherRel;
    otherRel.uid     = QStringLiteral("otherrel-1");
    otherRel.linkRel = QStringLiteral("c");
    tak._handleMarker(xml(otherRel));
    QVERIFY(tak.remoteCommands().isEmpty());
    QCOMPARE(ignoredLines(log).last(),
             QStringLiteral("marker ignored: no creating device named (\"HOLD 1\", UID otherrel-1)"));

    // A device naming itself as its creator.
    Ev self;
    self.uid     = QStringLiteral("ANDROID-SELF");
    self.linkUid = QStringLiteral("android-self");
    tak._handleMarker(xml(self));
    QVERIFY(tak.remoteCommands().isEmpty());
    QVERIFY(ignoredLines(log).last().startsWith(QStringLiteral("marker ignored: device report about itself")));

    // Stale, and no stale time.
    Ev stale;
    stale.uid   = QStringLiteral("stale-1");
    stale.time  = QDateTime::currentDateTimeUtc().addSecs(-7200);
    stale.stale = QDateTime::currentDateTimeUtc().addSecs(-3600);
    tak._handleMarker(xml(stale));
    QVERIFY(tak.remoteCommands().isEmpty());
    QCOMPARE(ignoredLines(log).last(), QStringLiteral("marker ignored: stale (\"HOLD 1\", UID stale-1)"));
    Ev noTimes;
    noTimes.uid = QStringLiteral("notime-1");
    QByteArray noTimesXml = xml(noTimes);
    noTimesXml.replace("stale=\"", "expires=\"");
    tak._handleMarker(noTimesXml);
    QVERIFY(tak.remoteCommands().isEmpty());
    QVERIFY(ignoredLines(log).last().startsWith(QStringLiteral("marker ignored: no valid time or stale time")));

    // No marker uid: one command per marker cannot hold, so it is not read.
    Ev noUid;
    noUid.uid.clear();
    tak._handleMarker(xml(noUid));
    QVERIFY(tak.remoteCommands().isEmpty());
    QCOMPARE(ignoredLines(log).last(), QStringLiteral("marker ignored: no marker UID (\"HOLD 1\", UID not reported)"));

    // A marker or contact that names no command is not a command and gets no line.
    const qsizetype linesBefore = ignoredLines(log).size();
    Ev rally;
    rally.uid  = QStringLiteral("rally-1");
    rally.name = QStringLiteral("Rally point");
    tak._handleMarker(xml(rally));
    tak._handleMarker(xml(contactReport(QStringLiteral("ENGINE-1"))));
    QCOMPARE(ignoredLines(log).size(), linesBefore);
    QVERIFY(tak.remoteCommands().isEmpty());

    // Placed markers reach the command path: point dropper (a-h-G) and a
    // spot marker (b-m-p-s-m), with the sender from the p-p link.
    tak._handleMarker(xml(Ev()));
    QCOMPARE(int(tak.remoteCommands().size()), 1);
    const QVariantMap e = tak.remoteCommands().first().toMap();
    QCOMPARE(e.value(QStringLiteral("marker")).toString(), QStringLiteral("HOLD 1"));
    QCOMPARE(e.value(QStringLiteral("sender")).toString(), QStringLiteral("ENGINE-1 (UID ANDROID-ENGINE1)"));
    QCOMPARE(e.value(QStringLiteral("result")).toString(),
             QStringLiteral("Not sent: \"Command By ATAK on\" is off (TAK settings)"));
    Ev spot;
    spot.uid  = QStringLiteral("spot-1");
    spot.type = QStringLiteral("b-m-p-s-m");
    spot.name = QStringLiteral("RTL 1");
    tak._handleMarker(xml(spot));
    QCOMPARE(int(tak.remoteCommands().size()), 2);
    QCOMPARE(tak.remoteCommands().first().toMap().value(QStringLiteral("verb")).toString(), QStringLiteral("RTL"));

    // A plain point sent to one of this station's drones is a placed marker too...
    Ev sendTo;
    sendTo.uid  = QStringLiteral("sendto-1");
    sendTo.name = QStringLiteral("Waypoint 4");
    sendTo.dest = QStringLiteral("RLTEST-1");
    tak._handleMarker(xml(sendTo));
    QCOMPARE(int(tak.remoteCommands().size()), 3);
    QCOMPARE(tak.remoteCommands().first().toMap().value(QStringLiteral("marker")).toString(),
             QStringLiteral("point sent to RLTEST-1"));
    // ...but a contact's own report addressed to one is not.
    Ev sendToPli = contactReport(QStringLiteral("Waypoint 5"));
    sendToPli.dest = QStringLiteral("RLTEST-1");
    tak._handleMarker(xml(sendToPli));
    QCOMPARE(int(tak.remoteCommands().size()), 3);
}

// G43, G36: a marker runs its command once. A replay (same uid and time,
// for example a server re-sending recent CoT on connect) and an edit (same
// uid, new time, new name or point) are ignored with a station line, also
// after the link is closed. A new marker runs.
void TakMarkerTest::_replayAndEditNotRerun()
{
    TakManager tak;
    readAll(tak);
    QSignalSpy log(&tak, &TakManager::logMessage);

    const Ev marker;
    const QByteArray first = xml(marker);
    tak._handleMarker(first);
    QCOMPARE(int(tak.remoteCommands().size()), 1);

    // Replay: the identical event again, twice.
    tak._handleMarker(first);
    tak._handleMarker(first);
    QCOMPARE(int(tak.remoteCommands().size()), 1);
    QCOMPARE(ignoredLines(log), QStringList{
        QStringLiteral("marker ignored: already received; a repeated marker is not run again "
                       "(\"HOLD 1\", UID 8d1f0c2e-5b7a-4c3e-9a51-0e6f2b1d4c11)") });

    // Edit: ATAK re-sends the marker with a new time, a new name and a moved point.
    Ev edited = marker;
    edited.time = marker.time.addSecs(20);
    edited.name = QStringLiteral("RTL 1");
    edited.lat  = QStringLiteral("-35.361000");
    tak._handleMarker(xml(edited));
    QCOMPARE(int(tak.remoteCommands().size()), 1);
    QCOMPARE(ignoredLines(log).last(),
             QStringLiteral("marker ignored: changed after its command was received; an edited marker is not "
                            "run again. Place a new marker (\"RTL 1\", UID 8d1f0c2e-5b7a-4c3e-9a51-0e6f2b1d4c11)"));
    // A second edit is its own line.
    Ev edited2 = edited;
    edited2.time = marker.time.addSecs(40);
    tak._handleMarker(xml(edited2));
    QCOMPARE(ignoredLines(log).size(), qsizetype(3));

    // Closing the link does not forget what ran.
    tak.disconnectServer();
    tak._handleMarker(first);
    QCOMPARE(int(tak.remoteCommands().size()), 1);

    // A new marker runs.
    Ev other;
    other.uid = QStringLiteral("2c9e7a10-0000-4000-8000-000000000002");
    tak._handleMarker(xml(other));
    QCOMPARE(int(tak.remoteCommands().size()), 2);
}

// G36: a marker whose name starts with a verb but does not parse leaves
// a station line, once per marker.
void TakMarkerTest::_malformedMarkerLine()
{
    TakManager tak;
    readAll(tak);
    QSignalSpy log(&tak, &TakManager::logMessage);

    Ev noTarget;
    noTarget.uid  = QStringLiteral("bad-1");
    noTarget.name = QStringLiteral("HOLD");
    tak._handleMarker(xml(noTarget));
    tak._handleMarker(xml(noTarget));
    QVERIFY(tak.remoteCommands().isEmpty());
    QCOMPARE(ignoredLines(log), QStringList{
        QStringLiteral("marker ignored: command not understood (missing track id) (\"HOLD\", UID bad-1)") });

    Ev badAlt;
    badAlt.uid  = QStringLiteral("bad-2");
    badAlt.name = QStringLiteral("GOTO 1 ALT");
    tak._handleMarker(xml(badAlt));
    QVERIFY(tak.remoteCommands().isEmpty());
    QCOMPARE(ignoredLines(log).last(),
             QStringLiteral("marker ignored: command not understood (bad token ALT) (\"GOTO 1 ALT\", UID bad-2)"));

    // A malformed name on a contact's own report is ignored as a report.
    tak._handleMarker(xml(contactReport(QStringLiteral("LAUNCH"))));
    QVERIFY(ignoredLines(log).last().startsWith(QStringLiteral("marker ignored: device position report")));

    // Fixing the name of a malformed marker is not an edit of a command:
    // nothing ran, so the corrected marker reaches the command path.
    Ev fixed = noTarget;
    fixed.time = noTarget.time.addSecs(10);
    fixed.name = QStringLiteral("HOLD 1");
    tak._handleMarker(xml(fixed));
    QCOMPARE(int(tak.remoteCommands().size()), 1);
}

// G35: a negative ALT (or SPD, or aircraft number) is refused with a reason,
// never read as positive. A hyphen used as a separator still parses.
void TakMarkerTest::_negativeAltitudeRefused()
{
    const QString kNegAlt = negAltReason(QStringLiteral("-50"));
    Tak::Command cmd;
    QString err;
    for (const QString &text : { QStringLiteral("ALT -50"), QStringLiteral("ALT_-50"), QStringLiteral("ALT--50"),
                                 QStringLiteral("ALT - 50") }) {
        QVERIFY2(!Tak::parseCommand(QStringLiteral("GOTO 1 ") + text, cmd, err), qPrintable(text));
        QCOMPARE(err, kNegAlt);
    }
    QVERIFY(!Tak::parseCommand(QStringLiteral("ALTHOLD 2 ALT -50"), cmd, err));
    QCOMPARE(err, kNegAlt);
    QVERIFY(!Tak::parseCommand(QStringLiteral("GOTO 1 SPD -3"), cmd, err));
    QCOMPARE(err, negSpdReason(QStringLiteral("-3")));
    QVERIFY(!Tak::parseCommand(QStringLiteral("GOTO -2"), cmd, err));
    QCOMPARE(err, QStringLiteral("bad track id -2"));
    QVERIFY(!Tak::parseCommand(QStringLiteral("RTL 1,-2"), cmd, err));
    QCOMPARE(err, QStringLiteral("bad track list 1,-2"));

    // Unchanged: a hyphen between the verb and the aircraft number, and ALT 0.
    // G100: a hyphen before an ALT value now makes it negative ("goto-2-alt-150").
    QVERIFY(Tak::parseCommand(QStringLiteral("goto-2"), cmd, err));
    QCOMPARE(cmd.ids, QList<int>{ 2 });
    QVERIFY(!Tak::parseCommand(QStringLiteral("goto-2-alt-150"), cmd, err));
    QVERIFY(Tak::parseCommand(QStringLiteral("GOTO_2_ALT_150_SPD_5"), cmd, err));
    QCOMPARE(cmd.altFt, 150);
    QCOMPARE(cmd.spd, 5);
    QVERIFY(Tak::parseCommand(QStringLiteral("ALTHOLD 1 ALT 0"), cmd, err));
    QCOMPARE(cmd.altFt, 0);

    // From a placed marker: ignored with the reason, nothing listed or sent.
    TakManager tak;
    readAll(tak);
    QSignalSpy log(&tak, &TakManager::logMessage);
    Ev neg;
    neg.uid  = QStringLiteral("neg-1");
    neg.name = QStringLiteral("GOTO 1 ALT -50");
    tak._handleMarker(xml(neg));
    QVERIFY(tak.remoteCommands().isEmpty());
    QCOMPARE(ignoredLines(log), QStringList{
        QStringLiteral("marker ignored: command not understood (") + kNegAlt + QStringLiteral(") "
                       "(\"GOTO 1 ALT -50\", UID neg-1)") });

    // From the station's test command: listed and logged as not sent.
    const QString line = tak.injectTestCommand(QStringLiteral("GOTO 1 ALT -50"));
    QCOMPARE(line, QStringLiteral("TEST GOTO 1 ALT -50 -> Not sent: command not understood "
                                  "(") + kNegAlt + QStringLiteral(")"));
    QCOMPARE(int(tak.remoteCommands().size()), 1);
    QVERIFY(!tak.remoteCommands().first().toMap().value(QStringLiteral("sent")).toBool());
}

// G100: a minus sign or hyphen attached to an ALT or SPD number, in any form,
// makes it negative and the command is refused with the reason. It is never
// read as positive. Spaces and underscores alone still separate.
void TakMarkerTest::_negativeValueAnyForm()
{
    Tak::Command cmd;
    QString err;
    const QString kNegAlt150 = negAltReason(QStringLiteral("-150"));
    const QStringList altForms = {
        QStringLiteral("GOTO 1 ALT-150"),     // no space: the G35 gap
        QStringLiteral("GOTO 1 ALT -150"),
        QStringLiteral("GOTO 1 ALT_-150"),
        QStringLiteral("GOTO 1 ALT--150"),
        QStringLiteral("GOTO 1 ALT - 150"),
        QStringLiteral("GOTO 1 ALT-_150"),
        QStringLiteral("GOTO 1 ALT_-_150"),
        QStringLiteral("GOTO_1_ALT-150"),
        QStringLiteral("GOTO-1-ALT-150"),
        QStringLiteral("goto 1 alt-150"),
        QStringLiteral("ALTHOLD 1 ALT-150"),
        QStringLiteral("GOTO 1 SPD 5 ALT-150"),
        QStringLiteral("GOTO 1 ALT ") + QChar(0x2212) + QStringLiteral("150"),   // minus sign
        QStringLiteral("GOTO 1 ALT") + QChar(0x2013) + QStringLiteral("150"),    // en dash
        QStringLiteral("GOTO 1 ALT") + QChar(0xFF0D) + QStringLiteral("150"),    // fullwidth hyphen-minus
    };
    for (const QString &text : altForms) {
        QVERIFY2(!Tak::parseCommand(text, cmd, err), qPrintable(text));
        QVERIFY2(err == kNegAlt150, qPrintable(text + QStringLiteral(" -> ") + err));
    }

    // "-0" is a minus sign attached to the number too
    QVERIFY(!Tak::parseCommand(QStringLiteral("ALTHOLD 1 ALT-0"), cmd, err));
    QCOMPARE(err, negAltReason(QStringLiteral("-0")));
    QVERIFY(!Tak::parseCommand(QStringLiteral("ALTHOLD 1 ALT -0"), cmd, err));
    QCOMPARE(err, negAltReason(QStringLiteral("-0")));

    // SPD, the other numeric value, in the same forms
    for (const QString &text : { QStringLiteral("GOTO 1 SPD-3"), QStringLiteral("GOTO 1 SPD -3"),
                                 QStringLiteral("GOTO 1 SPD_-3"), QStringLiteral("GOTO 1 SPD--3"),
                                 QStringLiteral("GOTO-1-SPD-3"), QStringLiteral("GOTO 1 ALT 100 SPD-3") }) {
        QVERIFY2(!Tak::parseCommand(text, cmd, err), qPrintable(text));
        QVERIFY2(err == negSpdReason(QStringLiteral("-3")), qPrintable(text + QStringLiteral(" -> ") + err));
    }

    // Aircraft numbers are never negative: a minus after a separator is refused,
    // and a hyphen right after the verb stays a separator.
    QVERIFY(!Tak::parseCommand(QStringLiteral("GOTO -2"), cmd, err));
    QVERIFY(!Tak::parseCommand(QStringLiteral("GOTO_-2"), cmd, err));
    QVERIFY(!Tak::parseCommand(QStringLiteral("RTL 1,-2"), cmd, err));
    QVERIFY(Tak::parseCommand(QStringLiteral("VECTOR-2"), cmd, err));
    QCOMPARE(cmd.ids, QList<int>{ 2 });

    // Still positive: spaces and underscores, verbs that contain ALT.
    // (A plus sign is no longer accepted: see _signAndUnicodeSpaceRefused.)
    QVERIFY(Tak::parseCommand(QStringLiteral("GOTO 1 ALT 150"), cmd, err));
    QCOMPARE(cmd.altFt, 150);
    QVERIFY(Tak::parseCommand(QStringLiteral("GOTO_1_ALT_150_SPD_5"), cmd, err));
    QCOMPARE(cmd.altFt, 150);
    QCOMPARE(cmd.spd, 5);
    QVERIFY(Tak::parseCommand(QStringLiteral("SETALT 1 ALT 150"), cmd, err));
    QCOMPARE(cmd.verb, Tak::Verb::AltHold);
    QCOMPARE(cmd.altFt, 150);
    QVERIFY(Tak::parseCommand(QStringLiteral("ALTHOLD-1-ALT 150"), cmd, err));
    QCOMPARE(cmd.ids, QList<int>{ 1 });
    QCOMPARE(cmd.altFt, 150);

    // From a placed marker named with no space: ignored with the reason, nothing listed or sent
    TakManager tak;
    readAll(tak);
    QSignalSpy log(&tak, &TakManager::logMessage);
    Ev neg;
    neg.uid  = QStringLiteral("neg-nospace");
    neg.name = QStringLiteral("GOTO 1 ALT-150");
    tak._handleMarker(xml(neg));
    QVERIFY(tak.remoteCommands().isEmpty());
    QCOMPARE(ignoredLines(log), QStringList{
        QStringLiteral("marker ignored: command not understood (") + kNegAlt150 + QStringLiteral(") "
                       "(\"GOTO 1 ALT-150\", UID neg-nospace)") });
}

// R530 security M2: a minus followed by a plus, or a minus followed by a
// Unicode space, used to slip past the sign rules and read as positive. Any
// '-' or '+' in an ALT or SPD value is now refused with the negative-value
// reason, any Unicode white space separates like a plain space, and only a
// token of plain ASCII digits is a value.
void TakMarkerTest::_signAndUnicodeSpaceRefused()
{
    Tak::Command cmd;
    QString err;

    // A minus then a plus: the token left is "+150".
    for (const QString &text : { QStringLiteral("GOTO 1 ALT -+150"), QStringLiteral("GOTO 1 ALT-+150"),
                                 QStringLiteral("GOTO 1 ALT +150"), QStringLiteral("GOTO 1 ALT_+150") }) {
        QVERIFY2(!Tak::parseCommand(text, cmd, err), qPrintable(text));
        QVERIFY2(err == negAltReason(QStringLiteral("+150")),
                 qPrintable(text + QStringLiteral(" -> ") + err));
    }
    QVERIFY(!Tak::parseCommand(QStringLiteral("GOTO 1 SPD -+3"), cmd, err));
    QVERIFY2(err == negSpdReason(QStringLiteral("+3")), qPrintable(err));
    QVERIFY(!Tak::parseCommand(QStringLiteral("GOTO 1 ALT 1+50"), cmd, err));
    QVERIFY2(err == negAltReason(QStringLiteral("1+50")), qPrintable(err));

    // A minus, then a Unicode space, then the number: folded to a plain
    // space, so the minus is seen and the reason given.
    for (const char16_t sp : { char16_t(0x00A0), char16_t(0x202F), char16_t(0x3000), char16_t(0x0085),
                               char16_t(0x2009), char16_t(0x1680) }) {
        const QString text = QStringLiteral("GOTO 1 ALT -") + QChar(sp) + QStringLiteral("150");
        QVERIFY2(!Tak::parseCommand(text, cmd, err), qPrintable(QString::number(int(sp), 16)));
        QVERIFY2(err == negAltReason(QStringLiteral("-150")),
                 qPrintable(QString::number(int(sp), 16) + QStringLiteral(" -> ") + err));
        const QString spd = QStringLiteral("GOTO 1 SPD") + QChar(sp) + QStringLiteral("-") + QChar(sp)
                          + QStringLiteral("3");
        QVERIFY2(!Tak::parseCommand(spd, cmd, err), qPrintable(QString::number(int(sp), 16)));
        QVERIFY2(err == negSpdReason(QStringLiteral("-3")),
                 qPrintable(QString::number(int(sp), 16) + QStringLiteral(" -> ") + err));
    }

    // Anything else that is not plain ASCII digits is not a number.
    for (const QString &text : { QStringLiteral("GOTO 1 ALT 15O"), QStringLiteral("GOTO 1 ALT 0X96"),
                                 QStringLiteral("GOTO 1 ALT 99999999999"),
                                 QStringLiteral("GOTO 1 ALT ") + QChar(0x0661) + QChar(0x0665) + QChar(0x0660),
                                 QStringLiteral("GOTO 1 ALT ") + QChar(0xFF11) + QChar(0xFF15) + QChar(0xFF10) }) {
        QVERIFY2(!Tak::parseCommand(text, cmd, err), qPrintable(text));
        QVERIFY2(err == QStringLiteral("bad token ALT"), qPrintable(text + QStringLiteral(" -> ") + err));
    }

    // The valid forms still parse, including a Unicode space as a separator.
    QVERIFY(Tak::parseCommand(QStringLiteral("GOTO 1 ALT 150"), cmd, err));
    QCOMPARE(cmd.altFt, 150);
    QVERIFY(Tak::parseCommand(QStringLiteral("GOTO 1 ALT_150"), cmd, err));
    QCOMPARE(cmd.altFt, 150);
    QVERIFY(Tak::parseCommand(QStringLiteral("GOTO_2_ALT_150"), cmd, err));
    QCOMPARE(cmd.ids, QList<int>{ 2 });
    QCOMPARE(cmd.altFt, 150);
    QVERIFY(Tak::parseCommand(QStringLiteral("GOTO_2_ALT_150_SPD_5"), cmd, err));
    QCOMPARE(cmd.altFt, 150);
    QCOMPARE(cmd.spd, 5);
    QVERIFY(Tak::parseCommand(QStringLiteral("GOTO") + QChar(0x00A0) + QStringLiteral("2")
                              + QChar(0x3000) + QStringLiteral("ALT") + QChar(0x202F) + QStringLiteral("150"),
                              cmd, err));
    QCOMPARE(cmd.ids, QList<int>{ 2 });
    QCOMPARE(cmd.altFt, 150);
    QVERIFY(Tak::parseCommand(QStringLiteral("ALTHOLD 1 ALT 0"), cmd, err));
    QCOMPARE(cmd.altFt, 0);

    // From a placed marker (the callsign comes straight from the CoT XML):
    // ignored with the reason, nothing listed or sent.
    TakManager tak;
    readAll(tak);
    QSignalSpy log(&tak, &TakManager::logMessage);
    Ev neg;
    neg.uid  = QStringLiteral("neg-plus");
    neg.name = QStringLiteral("GOTO 1 ALT -+150");
    tak._handleMarker(xml(neg));
    QVERIFY(tak.remoteCommands().isEmpty());
    QCOMPARE(ignoredLines(log), QStringList{
        QStringLiteral("marker ignored: command not understood (") + negAltReason(QStringLiteral("+150"))
        + QStringLiteral(") (\"GOTO 1 ALT -+150\", UID neg-plus)") });
}

// G39: a GOTO that names more than one aircraft, or whose point is unknown,
// is refused before dispatch and listed with the reason. It is never sent
// to 0, 0.
void TakMarkerTest::_gotoOneAircraftKnownPoint()
{
    QVERIFY(TakPolicy::pointKnown(-35.36, 149.16));
    QVERIFY(TakPolicy::pointKnown(0.0, 149.16));
    QVERIFY(!TakPolicy::pointKnown(0.0, 0.0));
    QVERIFY(!TakPolicy::pointKnown(91.0, 10.0));
    QVERIFY(!TakPolicy::pointKnown(10.0, -181.0));
    QVERIFY(!TakPolicy::pointKnown(std::nan(""), 10.0));

    TakManager tak;
    readAll(tak);
    tak._c2Allowed = true;
    tak.setC2Armed(true);
    tak._verbGoto = true;
    tak._verbRtl  = true;

    const QString multi   = QStringLiteral("Not sent: GOTO names more than one aircraft. Place one marker per aircraft");
    const QString unknown = QStringLiteral("Not sent: the GOTO point is unknown");
    auto result = [&tak]() { return tak.remoteCommands().first().toMap().value(QStringLiteral("result")).toString(); };

    // Several aircraft: a list and ALL.
    Ev two;
    two.uid  = QStringLiteral("goto-two");
    two.name = QStringLiteral("GOTO RLTEST-1,2");
    tak._handleMarker(xml(two));
    QCOMPARE(int(tak.remoteCommands().size()), 1);
    QCOMPARE(result(), multi);
    QVERIFY(!tak.remoteCommands().first().toMap().value(QStringLiteral("sent")).toBool());
    Ev all;
    all.uid  = QStringLiteral("goto-all");
    all.name = QStringLiteral("GOTO RLTEST-ALL ALT 200");
    tak._handleMarker(xml(all));
    QCOMPARE(result(), multi);

    // An unknown point: 0, 0 (the CoT unknown position) and out of range,
    // on a named marker and on a point sent to a drone.
    Ev zero;
    zero.uid  = QStringLiteral("goto-zero");
    zero.name = QStringLiteral("GOTO RLTEST-1");
    zero.lat  = QStringLiteral("0.0");
    zero.lon  = QStringLiteral("0.0");
    tak._handleMarker(xml(zero));
    QCOMPARE(result(), unknown);
    QCOMPARE(tak.remoteCommands().first().toMap().value(QStringLiteral("position")).toString(),
             QStringLiteral("unknown point at current altitude"));
    Ev far;
    far.uid  = QStringLiteral("goto-far");
    far.name = QStringLiteral("GOTO RLTEST-1");
    far.lat  = QStringLiteral("95.0");
    tak._handleMarker(xml(far));
    QCOMPARE(result(), unknown);
    Ev sendTo = zero;
    sendTo.uid  = QStringLiteral("goto-sendto-zero");
    sendTo.name = QStringLiteral("Waypoint");
    sendTo.dest = QStringLiteral("RLTEST-1");
    tak._handleMarker(xml(sendTo));
    QCOMPARE(result(), unknown);

    // The station test GOTO with no aircraft position: no test point, refused.
    const QString line = tak.injectTestCommand(QStringLiteral("GOTO RLTEST-1 ALT 300"));
    QCOMPARE(line, QStringLiteral("TEST GOTO RLTEST-1 ALT 300 -> ") + TakPolicy::gotoUnknownPointRefusal());
    QCOMPARE(result(), unknown);
    QCOMPARE(tak.remoteCommands().first().toMap().value(QStringLiteral("position")).toString(),
             QStringLiteral("unknown point at 300 ft above launch"));

    // A single-aircraft GOTO to a known point gets past these checks (to the
    // aircraft lookup: none connected here), and RTL may still name several.
    Ev one;
    one.uid  = QStringLiteral("goto-one");
    one.name = QStringLiteral("GOTO RLTEST-1");
    tak._handleMarker(xml(one));
    QCOMPARE(result(), TakPolicy::notSentResult(TakPolicy::noAircraftConnected(QStringLiteral("RLTEST-1"), false)));
    Ev rtl;
    rtl.uid  = QStringLiteral("rtl-two");
    rtl.name = QStringLiteral("RTL RLTEST-1,2");
    tak._handleMarker(xml(rtl));
    QCOMPARE(result(), TakPolicy::notSentResult(TakPolicy::noAircraftConnected(QString(), true)));
}

static QJsonObject lastLogLine(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    QList<QByteArray> lines = f.readAll().split('\n');
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) {
        lines.removeLast();
    }
    return lines.isEmpty() ? QJsonObject() : QJsonDocument::fromJson(lines.last()).object();
}

// G49: TKOFF never arms an aircraft while the ARM verb is off. With ARM off a
// TKOFF to a disarmed aircraft is refused with the reason in the list and in
// the command log, and the aircraft stays disarmed. With ARM on, or with the
// aircraft already armed, it is sent (simulated ArduCopter, MockLink).
void TakMarkerTest::_takeoffHeldToArmSwitch()
{
    _connectMockLink(MAV_AUTOPILOT_ARDUPILOTMEGA);
    QVERIFY(_vehicle);
    QVERIFY(!_vehicle->armed());
    const int id = _vehicle->id();

    TakManager tak;
    readAll(tak);
    tak._c2Allowed   = true;
    tak.setC2Armed(true);
    tak._verbTakeoff = true;
    tak._verbArm     = false;

    // ARM off.
    Ev off;
    off.uid  = QStringLiteral("tkoff-arm-off");
    off.name = QStringLiteral("TKOFF RLTEST-%1").arg(id);
    tak._handleMarker(xml(off));
    QCOMPARE(int(tak.remoteCommands().size()), 1);
    const QString refused = QStringLiteral("Not sent: the aircraft is not armed and ARM is off (TAK settings). "
                                           "TKOFF does not arm the aircraft while ARM is off");
    QVariantMap e = tak.remoteCommands().first().toMap();
    QCOMPARE(e.value(QStringLiteral("verb")).toString(), QStringLiteral("TKOFF"));
    QCOMPARE(e.value(QStringLiteral("result")).toString(), refused);
    QVERIFY(!e.value(QStringLiteral("sent")).toBool());
    QJsonObject rec = lastLogLine(tak.commandLogPath());
    QCOMPARE(rec.value(QStringLiteral("verb")).toString(), QStringLiteral("TKOFF"));
    QCOMPARE(rec.value(QStringLiteral("result")).toString(), refused);
    QVERIFY(rec.value(QStringLiteral("settings")).toObject().value(QStringLiteral("c2UplinkArmed")).toBool());
    QTest::qWait(1500);
    QVERIFY(!_vehicle->armed());

    // ARM on: the same command from a new marker is sent to the aircraft.
    tak._verbArm = true;
    Ev on;
    on.uid  = QStringLiteral("tkoff-arm-on");
    on.name = QStringLiteral("TKOFF RLTEST-%1").arg(id);
    tak._handleMarker(xml(on));
    QCOMPARE(int(tak.remoteCommands().size()), 2);
    e = tak.remoteCommands().first().toMap();
    QCOMPARE(e.value(QStringLiteral("result")).toString(), QStringLiteral("Takeoff command sent"));
    QVERIFY(e.value(QStringLiteral("sent")).toBool());
    rec = lastLogLine(tak.commandLogPath());
    QCOMPARE(rec.value(QStringLiteral("result")).toString(), QStringLiteral("Takeoff command sent"));
    // (The mock ArduCopter has no Guided mode, so QGC's takeoff stops before
    // arming it here; the station's part is that the command was sent.)

    // ARM off again, but the aircraft is armed: TKOFF does not arm it, so it
    // is sent.
    _vehicle->setArmed(true, false);
    QTRY_VERIFY_WITH_TIMEOUT(_vehicle->armed(), 10000);
    tak._verbArm = false;
    Ev armed;
    armed.uid  = QStringLiteral("tkoff-armed");
    armed.name = QStringLiteral("TKOFF RLTEST-%1").arg(id);
    tak._handleMarker(xml(armed));
    QCOMPARE(int(tak.remoteCommands().size()), 3);
    QCOMPARE(tak.remoteCommands().first().toMap().value(QStringLiteral("result")).toString(),
             QStringLiteral("Takeoff command sent"));

    // G39 backstop in the shared dispatcher: an armed aircraft is never sent
    // to an unknown point, whichever route asks.
    const Guided::Result g = Guided::dispatch(_vehicle, Guided::Verb::Goto, 0.0, 0.0, Guided::Alt::none(), std::nan(""));
    QVERIFY(!g.ok);
    QCOMPARE(g.detail, QStringLiteral("GOTO needs a position"));

    _vehicle->setArmed(false, false);
    _disconnectMockLink();
}

// G50: an empty callsign prefix stops command reading. The TAK page says so
// (commandPrefixEmpty and its notice) and each command marker leaves a line.
void TakMarkerTest::_emptyPrefixShown()
{
    TakManager tak;
    readAll(tak);
    QVERIFY(!tak.commandPrefixEmpty());
    // G47: an empty prefix is not saved from the page any more (T12)...
    QVERIFY(tak.setProperty("callsignPrefix", QString()));
    QVERIFY(!tak.commandPrefixEmpty());
    QCOMPARE(tak.prefixEntryError(), TakPolicy::prefixEntryRefusal());
    // ...but one saved by an older version still stops command reading.
    tak._callsignPrefix.clear();
    QVERIFY(tak.commandPrefixEmpty());
    QVERIFY(tak.property("commandPrefixEmpty").toBool());
    QCOMPARE(tak.property("emptyPrefixNotice").toString(),
             QStringLiteral("Commands are not read: the callsign prefix is empty. "
                            "Set a callsign prefix under TAK / OpenTAKServer."));

    QSignalSpy log(&tak, &TakManager::logMessage);
    Ev marker;
    marker.uid = QStringLiteral("prefix-1");
    tak._handleMarker(xml(marker));
    QVERIFY(tak.remoteCommands().isEmpty());
    QCOMPARE(ignoredLines(log), QStringList{
        QStringLiteral("marker ignored: the callsign prefix is empty, so this station reads no commands "
                       "(\"HOLD 1\", UID prefix-1)") });

    // With a prefix set the notice goes and the same marker is read.
    tak._callsignPrefix = QStringLiteral("RLTEST");
    QVERIFY(!tak.commandPrefixEmpty());
    tak._handleMarker(xml(marker));
    QCOMPARE(int(tak.remoteCommands().size()), 1);
}

// 5.1.2 review M1: a flood of unique, never-stale command markers cannot grow
// memory without bound. Oversize events and fields are not read, the stored
// stale time is clamped, and a full replay list refuses new commands instead
// of growing. (The cap is lowered here so the test stays fast; the code path
// is the same as at the default of 20,000.)
void TakMarkerTest::_floodBounded()
{
    TakManager tak;
    readAll(tak);
    tak._maxMarkersSeen = 200;
    // Commands on and any sender accepted: these entries are not dropped
    // first, so the full list refuses (R1 eviction is tested separately).
    tak._c2Allowed = true;
    tak.setC2Armed(true);
    QSignalSpy log(&tak, &TakManager::logMessage);

    for (int i = 0; i < 260; ++i) {
        Ev e;
        e.uid   = QStringLiteral("flood-%1").arg(i);
        e.stale = QDateTime(QDate(9999, 12, 31), QTime(0, 0), QTimeZone::UTC);
        tak._handleMarker(xml(e));
    }
    QCOMPARE(int(tak._markersSeen.size()), 200);
    QCOMPARE(int(tak.remoteCommands().size()), 200);
    QVERIFY(ignoredLines(log).last().startsWith(QStringLiteral("marker ignored: too many markers received this session to check for repeats; not run")));
    // The year-9999 stale time was clamped to about a day.
    const QDateTime limit = QDateTime::currentDateTimeUtc().addSecs(26 * 3600);
    for (const auto &s : std::as_const(tak._markersSeen)) {
        QVERIFY(s.stale < limit);
    }
    // Pruning removes entries whose stored stale time has passed.
    for (auto it = tak._markersSeen.begin(); it != tak._markersSeen.end(); ++it) {
        it->stale = QDateTime::currentDateTimeUtc().addSecs(-1);
    }
    tak._pruneMarkersSeen();
    QVERIFY(tak._markersSeen.isEmpty());

    // At most 10 "marker ignored" lines a second (review R6): most of the 60
    // refusals above were summarized.
    tak._flushRateSummary();
    bool summarized = false;
    for (const QList<QVariant> &args : log) {
        summarized = summarized || args.value(0).toString().endsWith(
            QStringLiteral(" more \"marker ignored\" lines not shown (at most 10 a second)"));
    }
    QVERIFY(summarized);
    QTest::qWait(1100);

    // An oversize event and an oversize UID are not read.
    Ev big;
    big.uid  = QStringLiteral("big-1");
    big.name = QStringLiteral("HOLD 1");
    QByteArray bigXml = xml(big);
    bigXml.replace("<archive/>", QByteArray("<remarks>") + QByteArray(70 * 1024, 'x') + "</remarks>");
    const int before = int(tak.remoteCommands().size());
    tak._handleMarker(bigXml);
    Ev longUid;
    longUid.uid = QString(200, QLatin1Char('u'));
    tak._handleMarker(xml(longUid));
    QCOMPARE(int(tak.remoteCommands().size()), before);
    QVERIFY(tak._markersSeen.isEmpty());
    const QStringList lines = ignoredLines(log);
    QVERIFY(lines.at(lines.size() - 2).startsWith(QStringLiteral("marker ignored: event larger than 64 KB")));
    QVERIFY(lines.last().startsWith(QStringLiteral("marker ignored: a name or UID is longer than 128 characters")));
    QVERIFY(lines.last().size() < 400);
}

// 5.1.2 review S1: at most 5 command markers per second (and 60 per minute)
// are read from the stream; the rest are counted in one summary line.
void TakMarkerTest::_rateLimited()
{
    TakManager tak;
    readAll(tak);
    tak._maxCommandsPerSecond = 5;
    tak._maxCommandsPerMinute = 60;
    QSignalSpy log(&tak, &TakManager::logMessage);
    // Start at the beginning of a second so the burst stays in one window.
    while (QDateTime::currentMSecsSinceEpoch() % 1000 > 100) {
        QTest::qWait(5);
    }
    for (int i = 0; i < 12; ++i) {
        Ev e;
        e.uid = QStringLiteral("rate-%1").arg(i);
        tak._handleMarker(xml(e));
    }
    QCOMPARE(int(tak.remoteCommands().size()), 5);
    tak._flushRateSummary();
    bool found = false;
    for (const QList<QVariant> &args : log) {
        found = found || args.value(0).toString() ==
            QStringLiteral("marker ignored: more than 5 command markers in one second or 60 in one minute; 7 not read");
    }
    QVERIFY(found);
    // The list signal for stream commands is coalesced into one.
    QTest::qWait(1100);
    QSignalSpy changed(&tak, &TakManager::remoteCommandsChanged);
    Ev more;
    more.uid = QStringLiteral("rate-more");
    tak._handleMarker(xml(more));
    QCOMPARE(changed.count(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(changed.count(), 1, 2000);
}

// 5.1.2 review S3: a marker placed before this station started is not run
// (a server replay after a restart), nor one far in the future or more than
// a day old.
void TakMarkerTest::_markerTimeLimits()
{
    TakManager tak;
    readAll(tak);
    QSignalSpy log(&tak, &TakManager::logMessage);

    Ev old;
    old.uid  = QStringLiteral("before-start");
    old.time = tak._sessionStartUtc.addSecs(-120);
    tak._handleMarker(xml(old));
    QVERIFY(tak.remoteCommands().isEmpty());
    QVERIFY(ignoredLines(log).last().startsWith(
        QStringLiteral("marker ignored: placed before this station started; a marker from before a restart is not run")));

    Ev skew;   // within the 60 s allowance for clock skew
    skew.uid  = QStringLiteral("skew");
    skew.time = tak._sessionStartUtc.addSecs(-30);
    tak._handleMarker(xml(skew));
    QCOMPARE(int(tak.remoteCommands().size()), 1);

    Ev future;
    future.uid  = QStringLiteral("future");
    future.time = QDateTime::currentDateTimeUtc().addSecs(600);
    tak._handleMarker(xml(future));
    QVERIFY(ignoredLines(log).last().startsWith(
        QStringLiteral("marker ignored: time is more than 5 minutes ahead of this station's clock")));

    tak._sessionStartUtc = QDateTime::currentDateTimeUtc().addDays(-3);
    Ev dayOld;
    dayOld.uid  = QStringLiteral("day-old");
    dayOld.time = QDateTime::currentDateTimeUtc().addSecs(-25 * 3600);
    tak._handleMarker(xml(dayOld));
    QVERIFY(ignoredLines(log).last().startsWith(QStringLiteral("marker ignored: placed more than 24 hours ago")));
    QCOMPARE(int(tak.remoteCommands().size()), 1);
}

// 5.1.2 review R1: a flood from a sender that is not on the ATAK command
// senders list does not use up the budget or the repeat list of a listed
// sender. Repeats do not use the budget either.
void TakMarkerTest::_approvedNotStarved()
{
    TakManager tak;
    readAll(tak);
    tak._approvedOnly   = true;
    tak._approvedPilots = { QStringLiteral("uid:ANDROID-ENGINE1") };
    tak._maxCommandsPerSecond = 5;
    tak._maxCommandsPerMinute = 60;
    while (QDateTime::currentMSecsSinceEpoch() % 1000 > 100) {
        QTest::qWait(5);
    }
    // Unlisted peer: 30 fresh markers in one second, plus repeats.
    for (int i = 0; i < 30; ++i) {
        Ev e;
        e.uid     = QStringLiteral("peer-%1").arg(i);
        e.linkUid = QStringLiteral("ANDROID-PEER");
        e.linkCs  = QStringLiteral("PEER");
        const QByteArray x = xml(e);
        tak._handleMarker(x);
        tak._handleMarker(x);
    }
    QCOMPARE(int(tak.remoteCommands().size()), 5);   // the unlisted budget
    // The listed sender is still read in the same second.
    Ev mine;
    mine.uid = QStringLiteral("mine-1");
    tak._handleMarker(xml(mine));
    QCOMPARE(int(tak.remoteCommands().size()), 6);
    QCOMPARE(tak.remoteCommands().first().toMap().value(QStringLiteral("sender")).toString(),
             QStringLiteral("ENGINE-1 (UID ANDROID-ENGINE1)"));

}

// Builds a station for the repeat-list tests: a 5-entry list, a session that
// started two hours ago, and limits out of the way.
static Ev markerAt(const QString &uid, int minutesAgo, const QString &linkUid = QStringLiteral("ANDROID-ENGINE1"))
{
    Ev e;
    e.uid     = uid;
    e.time    = QDateTime::currentDateTimeUtc().addSecs(-60 * minutesAgo);
    e.linkUid = linkUid;
    return e;
}

// 5.1.2 review E1: when the repeat list is full, the oldest entry goes only
// if its marker time is at least 30 minutes old, and the floor rises to that
// time, so a replay of it is refused. A marker placed now is never refused by
// the floor. Same with the ATAK command senders list on and off.
void TakMarkerTest::_repeatFloor()
{
    for (const bool listOn : { false, true }) {
        TakManager tak;
        readAll(tak);
        tak._approvedOnly   = listOn;
        tak._approvedPilots = { QStringLiteral("uid:ANDROID-ENGINE1") };
        tak._maxMarkersSeen = 5;
        tak._sessionStartUtc = QDateTime::currentDateTimeUtc().addSecs(-2 * 3600);
        QSignalSpy log(&tak, &TakManager::logMessage);

        QList<QByteArray> old;
        for (int i = 0; i < 5; ++i) {
            old << xml(markerAt(QStringLiteral("old-%1").arg(i), 60 - i));
            tak._handleMarker(old.last());
        }
        QCOMPARE(int(tak._markersSeen.size()), 5);
        QVERIFY(!tak._repeatFloorUtc.isValid());

        // A marker placed now: the oldest (60 minutes) goes, the floor rises to it.
        tak._handleMarker(xml(markerAt(QStringLiteral("now-1"), 0)));
        QCOMPARE(int(tak._markersSeen.size()), 5);
        QVERIFY(tak._markersSeen.contains(QStringLiteral("now-1")));
        QVERIFY(!tak._markersSeen.contains(QStringLiteral("old-0")));
        QVERIFY(tak._repeatFloorUtc.isValid());
        QVERIFY(tak._repeatFloorUtc <= QDateTime::currentDateTimeUtc().addSecs(-30 * 60));
        const int listed = int(tak.remoteCommands().size());

        // A replay of the evicted marker is refused by the floor, as a
        // marker-time drop.
        tak._handleMarker(old.first());
        QCOMPARE(int(tak.remoteCommands().size()), listed);
        QVERIFY(ignoredLines(log).last().startsWith(
            QStringLiteral("marker ignored: older than this station can check for repeats")));
        QCOMPARE(tak._dropCount[TakManager::DropTime], 1);

        // An edit of the evicted marker (same UID, a new time after the
        // floor) is not run as a new command either.
        Ev edit = markerAt(QStringLiteral("old-0"), 1);
        edit.name = QStringLiteral("RTL 1");
        tak._handleMarker(xml(edit));
        QCOMPARE(int(tak.remoteCommands().size()), listed);
        QVERIFY(ignoredLines(log).last().startsWith(QStringLiteral(
            "marker ignored: not read earlier this session (rate limit or repeat check); not run when delivered again")));

        // Markers placed now keep running while the old ones turn over.
        for (int i = 2; i <= 5; ++i) {
            tak._handleMarker(xml(markerAt(QStringLiteral("now-%1").arg(i), 0)));
        }
        QCOMPARE(int(tak.remoteCommands().size()), listed + 4);
        // Now the list holds only recent markers: the next one is refused.
        tak._handleMarker(xml(markerAt(QStringLiteral("now-6"), 0)));
        QCOMPARE(int(tak.remoteCommands().size()), listed + 4);
        QVERIFY(ignoredLines(log).last().startsWith(QStringLiteral(
            "marker ignored: too many markers received this session to check for repeats; not run")));
    }
}

// 5.1.2 review E1: a flood from an unlisted peer no longer locks the list for
// a listed sender once the flood's entries are 30 minutes old; a fresh flood
// still fails closed.
void TakMarkerTest::_unlistedFloodTurnsOver()
{
    TakManager tak;
    readAll(tak);
    tak._approvedOnly   = true;
    tak._approvedPilots = { QStringLiteral("uid:ANDROID-ENGINE1") };
    tak._maxMarkersSeen = 20;
    tak._sessionStartUtc = QDateTime::currentDateTimeUtc().addSecs(-2 * 3600);
    for (int i = 0; i < 20; ++i) {
        tak._handleMarker(xml(markerAt(QStringLiteral("peer-%1").arg(i), 45, QStringLiteral("ANDROID-PEER"))));
    }
    QCOMPARE(int(tak._markersSeen.size()), 20);
    tak._handleMarker(xml(markerAt(QStringLiteral("listed-1"), 0)));
    QVERIFY(tak._markersSeen.contains(QStringLiteral("listed-1")));
    QCOMPARE(tak.remoteCommands().first().toMap().value(QStringLiteral("sender")).toString(),
             QStringLiteral("ENGINE-1 (UID ANDROID-ENGINE1)"));

    TakManager fresh;
    readAll(fresh);
    fresh._approvedOnly   = true;
    fresh._approvedPilots = { QStringLiteral("uid:ANDROID-ENGINE1") };
    fresh._maxMarkersSeen = 20;
    for (int i = 0; i < 20; ++i) {
        fresh._handleMarker(xml(markerAt(QStringLiteral("peer-%1").arg(i), 0, QStringLiteral("ANDROID-PEER"))));
    }
    fresh._handleMarker(xml(markerAt(QStringLiteral("listed-1"), 0)));
    QVERIFY(!fresh._markersSeen.contains(QStringLiteral("listed-1")));
}

// 5.1.2 review L1: a marker dropped by the rate limit is not run if the
// server delivers it again later in the session.
void TakMarkerTest::_droppedNotRunLater()
{
    TakManager tak;
    readAll(tak);
    tak._maxCommandsPerSecond = 1;
    QSignalSpy log(&tak, &TakManager::logMessage);
    while (QDateTime::currentMSecsSinceEpoch() % 1000 > 100) {
        QTest::qWait(5);
    }
    Ev first;
    first.uid = QStringLiteral("rate-a");
    Ev second;
    second.uid = QStringLiteral("rate-b");
    tak._handleMarker(xml(first));
    const QByteArray dropped = xml(second);
    tak._handleMarker(dropped);
    QCOMPARE(int(tak.remoteCommands().size()), 1);
    QTest::qWait(1100);
    tak._handleMarker(dropped);
    QCOMPARE(int(tak.remoteCommands().size()), 1);
    QVERIFY(ignoredLines(log).last().startsWith(QStringLiteral(
        "marker ignored: not read earlier this session (rate limit or repeat check); not run when delivered again")));
    // A new marker still runs.
    Ev third;
    third.uid = QStringLiteral("rate-c");
    tak._handleMarker(xml(third));
    QCOMPARE(int(tak.remoteCommands().size()), 2);
}

// 5.1.2 review R1, R3: the TAK page notice while markers are dropped by the
// rate limit, a full repeat list or the marker time. It is updated by the
// one-second timer, not per marker, and clears a minute after the last drop.
void TakMarkerTest::_dropNotice()
{
    TakManager tak;
    readAll(tak);
    tak._maxCommandsPerSecond = 1;
    QSignalSpy changed(&tak, &TakManager::commandDropNoticeChanged);
    QVERIFY(tak.commandDropNotice().isEmpty());

    for (int i = 0; i < 3; ++i) {
        Ev e;
        e.uid = QStringLiteral("notice-%1").arg(i);
        tak._handleMarker(xml(e));
    }
    Ev ahead;
    ahead.uid  = QStringLiteral("ahead");
    ahead.time = QDateTime::currentDateTimeUtc().addSecs(900);
    tak._handleMarker(xml(ahead));
    QCOMPARE(changed.count(), 0);           // not per marker
    tak._markerTimerTick();
    QCOMPARE(changed.count(), 1);
    // The fly-view line: the same counts, as one total (review L2).
    QCOMPARE(tak.property("flyDropNotice").toString(),
             QStringLiteral("ATAK command markers not read or not run: %1 so far. "
                            "The commands in them were not sent to the aircraft.")
                 .arg(tak._dropCount[TakManager::DropRate] + tak._dropCount[TakManager::DropTime]));
    QVERIFY(tak._dropCount[TakManager::DropRate] >= 2);
    const QString notice = tak.property("commandDropNotice").toString();
    QVERIFY2(notice.contains(QStringLiteral("ATAK command markers not read because more arrived than this station "
                                            "reads (1 a second, 1000000 a minute): ")),
             qPrintable(notice));
    QVERIFY2(notice.contains(QStringLiteral(" so far. This notice clears after a minute with none.")),
             qPrintable(notice));
    QVERIFY2(notice.contains(QStringLiteral("ATAK command markers not run because of the marker time (ahead of this "
                                            "station's clock, placed before this station started, more than 24 "
                                            "hours old, or older than this station can check for repeats): 1 so "
                                            "far. The marker time is set by the sending device's clock. This "
                                            "notice clears after a minute with none.")),
             qPrintable(notice));

    // A minute without drops clears it.
    for (qint64 &t : tak._dropLastMs) {
        t -= 61 * 1000;
    }
    tak._markerTimerTick();
    QVERIFY(tak.commandDropNotice().isEmpty());
    QVERIFY(tak.flyDropNotice().isEmpty());
    QCOMPARE(changed.count(), 2);

    // Full repeat list.
    QCOMPARE(TakPolicy::commandDropNotice(0, 2, 0, 5, 60),
             QStringLiteral("ATAK command markers not run because too many markers were received this "
                            "session to check for repeats: 2 so far. This notice clears after a minute with none."));
    QCOMPARE(TakPolicy::markerIgnoredLine(QStringLiteral("event larger than 64 KB"), QString(), QString()),
             QStringLiteral("marker ignored: event larger than 64 KB"));
}

// ---------------------------------------------------------------------------
// G47, G48: addressing. A marker names the aircraft as this station publishes
// it. This is routing, so that a command meant for one station does not act
// at another; it is not authentication.
// ---------------------------------------------------------------------------

namespace {

/// Another GC station's own event: <prefix>-GCS with its own uid.
Ev stationEvent(const QString &callsign, const QString &uid)
{
    Ev e = contactReport(callsign);
    e.uid = uid;
    return e;
}

/// An aircraft track another GC station publishes, linked to its station.
Ev aircraftTrack(const QString &uid, const QString &callsign, const QString &stationUid,
                 const QString &stationCallsign)
{
    Ev e;
    e.uid     = uid;
    e.type    = QStringLiteral("a-f-A-M-H-Q");
    e.how     = QStringLiteral("m-g");
    e.name    = callsign;
    e.linkUid = stationUid;
    e.linkCs  = stationCallsign;
    e.eud     = true;
    return e;
}

QString notSent(const QString &why)
{
    return TakPolicy::notSentResult(why);
}

} // namespace

/// Commands on, with every verb used here enabled. No aircraft is
/// connected, so a command for this station that passes every check is
/// listed as "no aircraft ... is connected to this station".
void TakMarkerTest::commandsOn(TakManager &tak)
{
    if (tak._prefixSetFor.isEmpty()) {
        tak._callsignPrefix = QStringLiteral("RLTEST");
        tak._prefixSetFor   = QStringLiteral("RLTEST");
    }
    tak._c2Allowed = true;
    tak.setC2Armed(true);
    tak._verbHold  = true;
    tak._verbRtl   = true;
    tak._verbGoto  = true;
    tak._verbLand  = true;
}

void TakMarkerTest::_addressedMarkers()
{
    TakManager tak;
    readAll(tak);
    commandsOn(tak);
    auto send = [&tak](const QString &uid, const QString &name, const QString &dest = QString()) {
        Ev e;
        e.uid  = uid;
        e.name = name;
        e.dest = dest;
        tak._handleMarker(xml(e));
        return tak.remoteCommands().first().toMap();
    };
    auto result = [](const QVariantMap &e) { return e.value(QStringLiteral("result")).toString(); };

    // A bare number names no station: refused on every station (T1).
    QVariantMap e = send(QStringLiteral("a-1"), QStringLiteral("HOLD 1"));
    QCOMPARE(result(e), notSent(QStringLiteral("the marker does not name a station, so this station does not "
                                               "run it. If it was meant for an aircraft of this station, the "
                                               "marker must name it, as in HOLD RLTEST-1")));
    QCOMPARE(e.value(QStringLiteral("targets")).toString(), QStringLiteral("aircraft 1 (no station named)"));
    QVERIFY(!e.value(QStringLiteral("sent")).toBool());
    e = send(QStringLiteral("a-1b"), QStringLiteral("LAND 2,3"));
    QVERIFY(result(e).startsWith(notSent(QStringLiteral("the marker does not name a station"))));
    // Bare ALL (T2).
    e = send(QStringLiteral("a-2"), QStringLiteral("RTL ALL"));
    QCOMPARE(result(e), notSent(QStringLiteral("ALL does not name a station, so this station does not run it. "
                                               "RLTEST-ALL names every aircraft of this station")));
    // SEL, with or without a name (T3, G48).
    for (const QString &text : { QStringLiteral("HOLD SEL"), QStringLiteral("HOLD RLTEST-SEL") }) {
        e = send(QStringLiteral("sel-") + text, text);
        QCOMPARE(result(e), notSent(TakPolicy::addressRefusal({ TakPolicy::Address::Sel }, Tak::Command{ Tak::Verb::Hold },
                                                              QStringLiteral("RLTEST"))));
        QVERIFY(result(e).contains(QStringLiteral("SEL is not accepted from ATAK")));
        QCOMPARE(e.value(QStringLiteral("targets")).toString(), QStringLiteral("SEL (not accepted)"));
    }
    // This station's name: past addressing, to the aircraft lookup.
    e = send(QStringLiteral("a-5"), QStringLiteral("HOLD RLTEST-1"));
    QCOMPARE(result(e), notSent(TakPolicy::noAircraftConnected(QStringLiteral("RLTEST-1"), false)));
    QCOMPARE(e.value(QStringLiteral("targets")).toString(), QStringLiteral("aircraft RLTEST-1"));
    e = send(QStringLiteral("a-6"), QStringLiteral("hold rltest_1"));
    QCOMPARE(result(e), notSent(TakPolicy::noAircraftConnected(QStringLiteral("RLTEST-1"), false)));
    e = send(QStringLiteral("a-6b"), QStringLiteral("RTL RLTEST-ALL"));
    QCOMPARE(result(e), notSent(TakPolicy::noAircraftConnected(QString(), true)));
    // This station, but no aircraft (T18).
    e = send(QStringLiteral("a-7"), QStringLiteral("HOLD RLTEST"));
    QCOMPARE(result(e), notSent(QStringLiteral("the marker names this station (RLTEST) but no aircraft, so this "
                                               "station does not run it. A marker must name the aircraft by its "
                                               "number, as in HOLD RLTEST-1 for aircraft 1")));
    // A name no station on the link publishes (T4).
    e = send(QStringLiteral("a-8"), QStringLiteral("HOLD CHARLIE-1"));
    QCOMPARE(result(e), notSent(QStringLiteral("no station or aircraft named CHARLIE is seen on this TAK server, "
                                               "so this station does not run it. This station's prefix is RLTEST")));
    // GOTO stays one aircraft (G39) with names too.
    const QString multi = notSent(TakPolicy::gotoMultipleRefusal());
    QCOMPARE(result(send(QStringLiteral("g-1"), QStringLiteral("GOTO RLTEST-1,2"))), multi);
    QCOMPARE(result(send(QStringLiteral("g-2"), QStringLiteral("GOTO RLTEST-ALL ALT 200"))), multi);
    // Marker text and send-to name different aircraft (T11); the same one,
    // or a marker sent to the station contact, is fine.
    e = send(QStringLiteral("m-1"), QStringLiteral("HOLD RLTEST-1"), QStringLiteral("RLTEST-2"));
    QCOMPARE(result(e), notSent(QStringLiteral("the marker text names RLTEST-1 but the marker was sent to "
                                               "RLTEST-2, so it is not clear which aircraft it is for, and this "
                                               "station does not run it. A marker sent to an aircraft must name "
                                               "that same aircraft")));
    e = send(QStringLiteral("m-2"), QStringLiteral("HOLD RLTEST-1"), QStringLiteral("RLTEST-1"));
    QCOMPARE(result(e), notSent(TakPolicy::noAircraftConnected(QStringLiteral("RLTEST-1"), false)));
    e = send(QStringLiteral("m-3"), QStringLiteral("HOLD RLTEST-1"), QStringLiteral("RLTEST-GCS"));
    QCOMPARE(result(e), notSent(TakPolicy::noAircraftConnected(QStringLiteral("RLTEST-1"), false)));
    // Nothing reached an aircraft.
    for (const QVariant &v : tak.remoteCommands()) {
        QVERIFY(!v.toMap().value(QStringLiteral("sent")).toBool());
    }
    // The command log records the prefix in force.
    QCOMPARE(lastLogLine(tak.commandLogPath()).value(QStringLiteral("settings")).toObject()
                 .value(QStringLiteral("callsignPrefix")).toString(), QStringLiteral("RLTEST"));
}

void TakMarkerTest::_otherStationMarkers()
{
    TakManager tak;
    readAll(tak);
    commandsOn(tak);
    QSignalSpy log(&tak, &TakManager::logMessage);
    auto send = [&tak](const QString &uid, const QString &name) {
        Ev e;
        e.uid  = uid;
        e.name = name;
        tak._handleMarker(xml(e));
    };

    // Station BRAVO announces itself, and one aircraft (FALCON, uid BRAVO-1).
    tak._handleMarker(xml(stationEvent(QStringLiteral("BRAVO-GCS"), QStringLiteral("rotorlab-bravo00000001"))));
    tak._handleMarker(xml(aircraftTrack(QStringLiteral("BRAVO-1"), QStringLiteral("FALCON"),
                                        QStringLiteral("rotorlab-bravo00000001"), QStringLiteral("BRAVO-GCS"))));
    QVERIFY(tak.remoteCommands().isEmpty());
    QVERIFY(ignoredLines(log).isEmpty());
    QVERIFY(!tak._prefixConflictActive());

    // A marker for BRAVO: one line here, nothing listed, nothing run (T5).
    send(QStringLiteral("o-1"), QStringLiteral("HOLD BRAVO-2"));
    QVERIFY(tak.remoteCommands().isEmpty());
    QCOMPARE(ignoredLines(log).last(),
             QStringLiteral("marker ignored: addressed to BRAVO-2, which station BRAVO publishes, not this "
                            "station (RLTEST) (\"HOLD BRAVO-2\", UID o-1)"));
    send(QStringLiteral("o-2"), QStringLiteral("GOTO FALCON ALT 100"));
    QVERIFY(tak.remoteCommands().isEmpty());
    QVERIFY(ignoredLines(log).last().startsWith(
        QStringLiteral("marker ignored: addressed to FALCON, which station BRAVO publishes, not this station (RLTEST)")));
    send(QStringLiteral("o-3"), QStringLiteral("RTL bravo all"));
    QVERIFY(tak.remoteCommands().isEmpty());

    // The same marker edited to name this station is not run here (T6).
    Ev edited;
    edited.uid  = QStringLiteral("o-1");
    edited.time = QDateTime::currentDateTimeUtc().addSecs(20);
    edited.name = QStringLiteral("HOLD RLTEST-1");
    tak._handleMarker(xml(edited));
    QVERIFY(tak.remoteCommands().isEmpty());
    QCOMPARE(ignoredLines(log).last(),
             QStringLiteral("marker ignored: this marker was addressed to another station earlier this session; a "
                            "changed or repeated marker is not run here (\"HOLD RLTEST-1\", UID o-1)"));

    // Other stations' markers do not use this station's rate budget or fill
    // its repeat list.
    tak._maxCommandsPerSecond = 1;
    for (int i = 0; i < 5; ++i) {
        send(QStringLiteral("o-rate-%1").arg(i), QStringLiteral("HOLD BRAVO-1"));
    }
    send(QStringLiteral("own-1"), QStringLiteral("HOLD RLTEST-1"));
    QCOMPARE(int(tak.remoteCommands().size()), 1);
    QCOMPARE(tak.remoteCommands().first().toMap().value(QStringLiteral("result")).toString(),
             notSent(TakPolicy::noAircraftConnected(QStringLiteral("RLTEST-1"), false)));
    QCOMPARE(int(tak._markersSeen.size()), 1);
    tak._maxCommandsPerSecond = 1000000;

    // Two minutes after BRAVO was last seen, its name is unknown here: the
    // marker is refused on this station with a reason (T4).
    for (auto it = tak._seenStations.begin(); it != tak._seenStations.end(); ++it) {
        it.value() -= (TakPolicy::kAddressSeenSecs + 1) * 1000LL;
    }
    for (auto it = tak._seenAircraft.begin(); it != tak._seenAircraft.end(); ++it) {
        it.value().second -= (TakPolicy::kAddressSeenSecs + 1) * 1000LL;
    }
    send(QStringLiteral("o-late"), QStringLiteral("HOLD BRAVO-2"));
    QCOMPARE(tak.remoteCommands().first().toMap().value(QStringLiteral("result")).toString(),
             notSent(QStringLiteral("no station or aircraft named BRAVO is seen on this TAK server, so this "
                                    "station does not run it. This station's prefix is RLTEST")));
    tak._pruneAddressSeen();
    QVERIFY(tak._seenStations.isEmpty());
    QVERIFY(tak._seenAircraft.isEmpty());
}

void TakMarkerTest::_prefixConflictFailsClosed()
{
    TakManager tak;
    readAll(tak);
    commandsOn(tak);
    auto send = [&tak](const QString &uid, const QString &name) {
        Ev e;
        e.uid  = uid;
        e.name = name;
        tak._handleMarker(xml(e));
        return tak.remoteCommands().first().toMap().value(QStringLiteral("result")).toString();
    };
    QVERIFY(tak.prefixConflictNotice().isEmpty());

    // Not a conflict: this station's own events relayed back, and a placed
    // marker that is merely named RLTEST-GCS.
    tak._handleMarker(xml(stationEvent(QStringLiteral("RLTEST-GCS"), tak._gcsUid)));
    tak._handleMarker(xml(aircraftTrack(QStringLiteral("RLTEST-1"), QStringLiteral("RLTEST-1"), tak._gcsUid,
                                        QStringLiteral("RLTEST-GCS"))));
    Ev placed;
    placed.uid  = QStringLiteral("p-1");
    placed.name = QStringLiteral("RLTEST-GCS");
    tak._handleMarker(xml(placed));
    QVERIFY(!tak._prefixConflictActive());

    // Another station on the same prefix: every command is refused (T7),
    // with a TAK page notice (T8) and a fly-view line (T9).
    QSignalSpy changed(&tak, &TakManager::addressingChanged);
    tak._handleMarker(xml(stationEvent(QStringLiteral("rltest-gcs"), QStringLiteral("rotorlab-other0000001"))));
    QVERIFY(tak._prefixConflictActive());
    QVERIFY(changed.count() >= 1);
    QVERIFY(tak.prefixConflictNotice().startsWith(
        QStringLiteral("Commands from ATAK markers are not sent to the aircraft: another station on this TAK "
                       "server also uses the callsign prefix RLTEST (last seen ")));
    tak._updateDropNotice();
    QCOMPARE(tak.flyDropNotice(),
             QStringLiteral("ATAK commands not sent to the aircraft: another station uses prefix RLTEST (TAK settings)"));
    const QString conflict = notSent(TakPolicy::prefixConflictRefusal(QStringLiteral("RLTEST")));
    QCOMPARE(send(QStringLiteral("c-1"), QStringLiteral("HOLD RLTEST-1")), conflict);
    QCOMPARE(send(QStringLiteral("c-1b"), QStringLiteral("RTL RLTEST-ALL")), conflict);
    QVERIFY(tak.injectTestCommand(QStringLiteral("HOLD RLTEST-1")).endsWith(
        TakPolicy::prefixConflictRefusal(QStringLiteral("RLTEST"))));

    // It clears 2 minutes after the other station was last seen.
    tak._prefixConflictMs -= (TakPolicy::kAddressSeenSecs + 1) * 1000LL;
    tak._updateDropNotice();
    QVERIFY(!tak._prefixConflictActive());
    QVERIFY(tak.prefixConflictNotice().isEmpty());
    QVERIFY(tak.flyDropNotice().isEmpty());
    QCOMPARE(send(QStringLiteral("c-2"), QStringLiteral("HOLD RLTEST-1")), notSent(TakPolicy::noAircraftConnected(QStringLiteral("RLTEST-1"), false)));

    // Not cleared while it is still seen: 1 minute later it holds.
    tak._handleMarker(xml(stationEvent(QStringLiteral("RLTEST-GCS"), QStringLiteral("rotorlab-other0000001"))));
    tak._prefixConflictMs -= 60 * 1000LL;
    QVERIFY(tak._prefixConflictActive());
    tak._prefixConflictMs = 0;

    // An aircraft track another station publishes as RLTEST-7 is a conflict
    // too, and so is a track it reports under RLTEST-GCS.
    tak._handleMarker(xml(aircraftTrack(QStringLiteral("RLTEST-7"), QStringLiteral("RLTEST-7"),
                                        QStringLiteral("rotorlab-other0000001"), QStringLiteral("RLTEST-GCS"))));
    QVERIFY(tak._prefixConflictActive());
    tak._prefixConflictMs = 0;
    tak._handleMarker(xml(aircraftTrack(QStringLiteral("OTHER-3"), QStringLiteral("OWL"),
                                        QStringLiteral("rotorlab-other0000001"), QStringLiteral("RLTEST-GCS"))));
    QVERIFY(tak._prefixConflictActive());
    tak._prefixConflictMs = 0;

    // A Fleet callsign another device also shows is not nameable while seen.
    tak._fleet[3].callsign = QStringLiteral("HAWK");
    QCOMPARE(send(QStringLiteral("f-1"), QStringLiteral("HOLD HAWK")),
             notSent(TakPolicy::noAircraftConnected(QStringLiteral("HAWK"), false)));
    tak._handleMarker(xml(contactReport(QStringLiteral("Hawk"))));
    QCOMPARE(send(QStringLiteral("f-2"), QStringLiteral("HOLD HAWK")),
             notSent(QStringLiteral("HAWK is also the callsign of an aircraft another station publishes. Name the "
                                    "aircraft as RLTEST-3")));
}

void TakMarkerTest::_prefixRules()
{
    TakManager tak;
    readAll(tak);
    commandsOn(tak);
    QSignalSpy log(&tak, &TakManager::logMessage);

    // A prefix that cannot name this station is not saved, and nothing changes.
    for (const QString &bad : { QStringLiteral("R"), QStringLiteral("GOTO"), QStringLiteral("all"),
                                QStringLiteral("RLTEST-1"), QStringLiteral("1AB"), QStringLiteral("ABCDEFGHIJKLM"),
                                QString() }) {
        QVERIFY(tak.setProperty("callsignPrefix", bad));
        QCOMPARE(tak.callsignPrefix(), QStringLiteral("RLTEST"));
        QCOMPARE(tak.prefixEntryError(), TakPolicy::prefixEntryRefusal());
        QVERIFY(tak.c2Armed());
    }
    // ROTOR, the old shared default, typed in any case: refused with its
    // own reason on the same path (T12c).
    for (const QString &rotor : { QStringLiteral("ROTOR"), QStringLiteral(" rotor "), QStringLiteral("Rotor") }) {
        log.clear();
        QVERIFY(tak.setProperty("callsignPrefix", rotor));
        QCOMPARE(tak.callsignPrefix(), QStringLiteral("RLTEST"));
        QCOMPARE(tak.prefixEntryError(), TakPolicy::sharedDefaultPrefixRefusal());
        QCOMPARE(tak.prefixEntryError(),
                 QStringLiteral("Not saved: ROTOR was the shared default in earlier versions; choose a prefix only "
                                "this station uses."));
        QVERIFY(tak.prefixSet());
        QVERIFY(tak.c2Armed());
        QVERIFY(!log.isEmpty());
        QVERIFY2(log.last().value(0).toString().endsWith(TakPolicy::sharedDefaultPrefixReason()),
                 qPrintable(log.last().value(0).toString()));
    }
    QVERIFY(tak.prefixChangedNotice().isEmpty());

    // A marker placed before the change, delivered after it.
    Ev before;
    before.uid  = QStringLiteral("pc-1");
    before.name = QStringLiteral("HOLD ALPHA-1");
    before.time = QDateTime::currentDateTimeUtc().addSecs(-10);

    // A valid change turns "Commands to aircraft on" off (T14).
    QVERIFY(tak.setProperty("callsignPrefix", QStringLiteral(" Alpha ")));
    QCOMPARE(tak.callsignPrefix(), QStringLiteral("Alpha"));
    QVERIFY(tak.prefixEntryError().isEmpty());
    QVERIFY(!tak.c2Armed());
    QCOMPARE(tak.prefixChangedNotice(),
             QStringLiteral("Callsign prefix changed from RLTEST to Alpha. \"Commands to aircraft on\" was turned "
                            "off. Markers placed before the change are not sent to the aircraft."));
    QVERIFY(log.count() >= 1);

    tak._handleMarker(xml(before));
    QVERIFY(tak.remoteCommands().isEmpty());
    QCOMPARE(ignoredLines(log).last(),
             QStringLiteral("marker ignored: placed before this station's callsign prefix changed; not run "
                            "(\"HOLD ALPHA-1\", UID pc-1)"));

    Ev after;
    after.uid  = QStringLiteral("pc-2");
    after.name = QStringLiteral("HOLD ALPHA-1");
    after.time = QDateTime::currentDateTimeUtc().addSecs(1);
    tak._handleMarker(xml(after));
    QCOMPARE(tak.remoteCommands().first().toMap().value(QStringLiteral("result")).toString(),
             notSent(QStringLiteral("\"Commands to aircraft on\" is off (TAK settings)")));

    tak.setC2Armed(true);
    Ev own;
    own.uid  = QStringLiteral("pc-3");
    own.name = QStringLiteral("HOLD alpha 1");
    own.time = after.time;
    tak._handleMarker(xml(own));
    QCOMPARE(tak.remoteCommands().first().toMap().value(QStringLiteral("result")).toString(),
             notSent(TakPolicy::noAircraftConnected(QStringLiteral("ALPHA-1"), false)));
    Ev old;
    old.uid  = QStringLiteral("pc-4");
    old.name = QStringLiteral("HOLD RLTEST-1");
    old.time = after.time;
    tak._handleMarker(xml(old));
    QCOMPARE(tak.remoteCommands().first().toMap().value(QStringLiteral("result")).toString(),
             notSent(QStringLiteral("no station or aircraft named RLTEST is seen on this TAK server, so this "
                                    "station does not run it. This station's prefix is ALPHA")));

    // A saved prefix that cannot name this station (from an older version):
    // commands are not read, and the page says so (T13).
    tak._callsignPrefix = QStringLiteral("R-1");
    QCOMPARE(tak.prefixProblemNotice(), TakPolicy::prefixInvalidNotice(QStringLiteral("R-1")));
    const qsizetype listed = tak.remoteCommands().size();
    Ev invalid;
    invalid.uid  = QStringLiteral("pc-5");
    invalid.name = QStringLiteral("HOLD R-1-1");
    invalid.time = after.time;
    tak._handleMarker(xml(invalid));
    QCOMPARE(tak.remoteCommands().size(), listed);
    QVERIFY(ignoredLines(log).last().startsWith(QStringLiteral("marker ignored: ") + TakPolicy::prefixInvalidReason()));
    QVERIFY(tak.injectTestCommand(QStringLiteral("HOLD RLTEST-1")).endsWith(TakPolicy::prefixInvalidReason()));
    tak._callsignPrefix = QStringLiteral("RLTEST");
    QVERIFY(tak.prefixProblemNotice().isEmpty());
}

// The station test button follows the same addressing rules as a marker.
void TakMarkerTest::_testButtonAddressing()
{
    TakManager tak;
    tak._commandEnabled = true;
    tak._approvedOnly   = false;
    commandsOn(tak);
    auto test = [&tak](const QString &text) {
        const QString line = tak.injectTestCommand(text);
        const QString prefix = QStringLiteral("TEST %1 -> ").arg(text);
        return line.startsWith(prefix) ? line.mid(prefix.size()) : line;
    };
    const QString t1 = QStringLiteral("the marker does not name a station, so this station does not run it. If "
                                      "it was meant for an aircraft of this station, the marker must name it, as in "
                                      "HOLD RLTEST-1");
    QCOMPARE(test(QStringLiteral("HOLD 1")), t1);
    QCOMPARE(tak.remoteCommands().first().toMap().value(QStringLiteral("result")).toString(), notSent(t1));
    QVERIFY(!tak.lastTestSent());
    QVERIFY(tak.remoteCommands().first().toMap().value(QStringLiteral("test")).toBool());
    QVERIFY(test(QStringLiteral("RTL ALL")).startsWith(QStringLiteral("ALL does not name a station")));
    QVERIFY(test(QStringLiteral("HOLD SEL")).startsWith(QStringLiteral("SEL is not accepted from ATAK")));
    QVERIFY(test(QStringLiteral("HOLD RLTEST-SEL")).startsWith(QStringLiteral("SEL is not accepted from ATAK")));
    QCOMPARE(test(QStringLiteral("GOTO RLTEST-1,2")), TakPolicy::gotoMultipleRefusal());
    QCOMPARE(test(QStringLiteral("GOTO RLTEST-ALL")), TakPolicy::gotoMultipleRefusal());
    QVERIFY(test(QStringLiteral("HOLD CHARLIE-1")).startsWith(QStringLiteral("no station or aircraft named CHARLIE")));
    tak._seenStations.insert(QStringLiteral("BRAVO"), QDateTime::currentMSecsSinceEpoch());
    QCOMPARE(test(QStringLiteral("HOLD BRAVO-1")),
             QStringLiteral("addressed to BRAVO-1, which station BRAVO publishes, not this station (RLTEST)"));
    QCOMPARE(test(QStringLiteral("HOLD RLTEST-1")), TakPolicy::noAircraftConnected(QStringLiteral("RLTEST-1"), false));
    tak._prefixConflictMs = QDateTime::currentMSecsSinceEpoch();
    tak._prefixConflictPrefix = QStringLiteral("RLTEST");
    QCOMPARE(test(QStringLiteral("HOLD RLTEST-1")), TakPolicy::prefixConflictRefusal(QStringLiteral("RLTEST")));
    for (const QVariant &v : tak.remoteCommands()) {
        QVERIFY(!v.toMap().value(QStringLiteral("sent")).toBool());
    }
}

// With a real (simulated) aircraft: SEL never reaches the selected aircraft,
// a bare number never reaches aircraft N, and this station's name does.
void TakMarkerTest::_addressingWithAircraft()
{
    _connectMockLink(MAV_AUTOPILOT_ARDUPILOTMEGA);
    QVERIFY(_vehicle);
    const int id = _vehicle->id();

    TakManager tak;
    readAll(tak);
    commandsOn(tak);
    MultiVehicleManager *mvm = MultiVehicleManager::instance();
    mvm->selectVehicle(id);
    QCOMPARE(mvm->selectedVehicles()->count(), 1);

    auto send = [&tak](const QString &uid, const QString &name) {
        Ev e;
        e.uid  = uid;
        e.name = name;
        tak._handleMarker(xml(e));
        return tak.remoteCommands().first().toMap();
    };
    QVariantMap e = send(QStringLiteral("w-1"), QStringLiteral("HOLD SEL"));
    QVERIFY(e.value(QStringLiteral("result")).toString().contains(QStringLiteral("SEL is not accepted from ATAK")));
    QCOMPARE(e.value(QStringLiteral("targets")).toString(), QStringLiteral("SEL (not accepted)"));
    QVERIFY(!e.value(QStringLiteral("sent")).toBool());
    e = send(QStringLiteral("w-2"), QStringLiteral("HOLD RLTEST-SEL"));
    QVERIFY(e.value(QStringLiteral("result")).toString().contains(QStringLiteral("SEL is not accepted from ATAK")));
    QVERIFY(tak._resolveTargets(Tak::Command{ Tak::Verb::Hold, Tak::Target::Sel }).isEmpty());

    e = send(QStringLiteral("w-3"), QStringLiteral("HOLD %1").arg(id));
    QVERIFY(e.value(QStringLiteral("result")).toString().contains(QStringLiteral("does not name a station")));
    QVERIFY(!e.value(QStringLiteral("sent")).toBool());

    // This station's names resolve to the aircraft (checked without sending
    // it a command): <prefix>-<id>, <prefix>-ALL and its Fleet callsign. A
    // bare number and SEL resolve to none.
    auto targets = [&tak](const QString &text) {
        Tak::Command cmd;
        QString err;
        return Tak::parseCommand(text, cmd, err) ? tak._addressedTargets(cmd) : QList<Vehicle *>();
    };
    QVERIFY(targets(QStringLiteral("HOLD RLTEST-%1").arg(id)) == QList<Vehicle *>{ _vehicle });
    QVERIFY(targets(QStringLiteral("RTL rltest all")) == QList<Vehicle *>{ _vehicle });
    QVERIFY(targets(QStringLiteral("HOLD %1").arg(id)).isEmpty());
    QVERIFY(targets(QStringLiteral("RTL ALL")).isEmpty());
    QVERIFY(targets(QStringLiteral("HOLD SEL")).isEmpty());
    QVERIFY(targets(QStringLiteral("HOLD RLTEST-SEL")).isEmpty());
    QVERIFY(targets(QStringLiteral("HOLD BRAVO-%1").arg(id)).isEmpty());
    tak._fleet[id].callsign = QStringLiteral("HAWK");
    QVERIFY(targets(QStringLiteral("HOLD hawk")) == QList<Vehicle *>{ _vehicle });

    e = send(QStringLiteral("w-6"), QStringLiteral("HOLD RLTEST-%1").arg(id + 1));
    QCOMPARE(e.value(QStringLiteral("result")).toString(),
             notSent(TakPolicy::noAircraftConnected(QStringLiteral("RLTEST-%1").arg(id + 1), false)));
    // The target column names this station's aircraft as it is published (T16).
    QCOMPARE(tak._targetLabel(_vehicle), QStringLiteral("aircraft RLTEST-%1 (HAWK)").arg(id));
    tak._fleet.remove(id);
    QCOMPARE(tak._targetLabel(_vehicle), QStringLiteral("aircraft RLTEST-%1").arg(id));

    mvm->deselectAllVehicles();
    _disconnectMockLink();
}

// ---------------------------------------------------------------------------
// Review fixes: security M1 (no shared default prefix), M3 (Fleet callsigns
// shaped like a station aircraft name), M4 (a prefix in use is not saved).
// ---------------------------------------------------------------------------

void TakMarkerTest::_prefixPerInstall()
{
    TakManager a;
    const QString p = a.callsignPrefix();
    QVERIFY2(QRegularExpression(QStringLiteral("^RL[0-9A-F]{4}$")).match(p).hasMatch(), qPrintable(p));
    QCOMPARE(p, TakPolicy::defaultPrefixFor(a._gcsUid));
    QVERIFY(TakPolicy::prefixProblem(p).isEmpty());
    QVERIFY(!a.prefixSet());
    QCOMPARE(a.prefixNotSetNotice(), TakPolicy::prefixNotSetNotice(p));
    QCOMPARE(a.property("prefixNotSetNotice").toString(), TakPolicy::prefixNotSetNotice(p));

    // "Commands to aircraft on" cannot be turned on.
    a._commandEnabled = true;
    a._approvedOnly   = false;
    a._c2Allowed      = true;
    a._verbHold       = true;
    QSignalSpy log(&a, &TakManager::logMessage);
    a.setC2Armed(true);
    QVERIFY(!a.c2Armed());
    QCOMPARE(log.last().value(0).toString(), TakPolicy::prefixNotSetArmLine());

    // Markers are not read, and the test button is refused, with a reason.
    Ev e;
    e.uid  = QStringLiteral("ns-1");
    e.name = QStringLiteral("HOLD %1-1").arg(p);
    a._handleMarker(xml(e));
    QVERIFY(a.remoteCommands().isEmpty());
    QVERIFY(ignoredLines(log).last().startsWith(QStringLiteral("marker ignored: ") + TakPolicy::prefixNotSetReason()));
    a._c2Armed = true;   // even past the switch, the address check refuses
    QVERIFY(a.injectTestCommand(QStringLiteral("HOLD %1-1").arg(p)).endsWith(TakPolicy::prefixNotSetReason()));
    a._c2Armed = false;

    // Confirmed on this install: commands can be turned on, and it is kept.
    a.confirmCallsignPrefix();
    QVERIFY(a.prefixSet());
    QVERIFY(a.prefixNotSetNotice().isEmpty());
    QCOMPARE(a.callsignPrefix(), p);
    a.setC2Armed(true);
    QVERIFY(a.c2Armed());
    a._persist();
    TakManager b;
    QCOMPARE(b.callsignPrefix(), p);
    QVERIFY(b.prefixSet());
}

void TakMarkerTest::_upgradedRotorNotSet()
{
    // An install upgraded from a version whose shared default was ROTOR is
    // offered its own per-install default, never ROTOR. The same
    // holds if ROTOR had been confirmed on this install.
    for (const QString &setFor : { QString(), QStringLiteral("ROTOR") }) {
        clearTakSettings();
        {
            QSettings s;
            s.setValue(QStringLiteral("TAK/callsignPrefix"), QStringLiteral("ROTOR"));
            s.setValue(QStringLiteral("TAK/gcsUid"), QStringLiteral("rotorlab-0123456789ab"));
            if (!setFor.isEmpty()) {
                s.setValue(QStringLiteral("TAK/prefixSetFor"), setFor);
            }
            s.sync();
        }
        TakManager u;
        QCOMPARE(u.callsignPrefix(), QStringLiteral("RL89AB"));
        QCOMPARE(u.callsignPrefix(), TakPolicy::defaultPrefixFor(QStringLiteral("rotorlab-0123456789ab")));
        QVERIFY(!u.prefixSet());
        QVERIFY(u.prefixProblemNotice().isEmpty());
        QCOMPARE(u.prefixNotSetNotice(), TakPolicy::prefixNotSetNotice(QStringLiteral("RL89AB")));
        QCOMPARE(QSettings().value(QStringLiteral("TAK/callsignPrefix")).toString(), QStringLiteral("RL89AB"));
        u._c2Allowed = true;
        u.setC2Armed(true);
        QVERIFY(!u.c2Armed());
        // ROTOR cannot be confirmed back.
        QVERIFY(u.setProperty("callsignPrefix", QStringLiteral("ROTOR")));
        QCOMPARE(u.callsignPrefix(), QStringLiteral("RL89AB"));
        QCOMPARE(u.prefixEntryError(), TakPolicy::sharedDefaultPrefixRefusal());
        QVERIFY(!u.prefixSet());
        // The offered default ("Use RL89AB for this station") confirms.
        u.confirmCallsignPrefix();
        QVERIFY(u.prefixSet());
        QVERIFY(u.prefixEntryError().isEmpty());
        u.setC2Armed(true);
        QVERIFY(u.c2Armed());
    }

    // An existing RLxxxx, saved but not yet confirmed, is kept and confirms
    // as before.
    clearTakSettings();
    {
        QSettings s;
        s.setValue(QStringLiteral("TAK/callsignPrefix"), QStringLiteral("RL3F9A"));
        s.setValue(QStringLiteral("TAK/gcsUid"), QStringLiteral("rotorlab-0123456789ab"));
        s.sync();
    }
    {
        TakManager r;
        QCOMPARE(r.callsignPrefix(), QStringLiteral("RL3F9A"));
        QVERIFY(!r.prefixSet());
        QCOMPARE(r.prefixNotSetNotice(), TakPolicy::prefixNotSetNotice(QStringLiteral("RL3F9A")));
        QVERIFY(r.setProperty("callsignPrefix", QStringLiteral("RL3F9A")));
        QVERIFY(r.prefixSet());
        QVERIFY(r.prefixEntryError().isEmpty());
        r._c2Allowed = true;
        r.setC2Armed(true);
        QVERIFY(r.c2Armed());
        r._persist();
    }
    TakManager reloaded;
    QCOMPARE(reloaded.callsignPrefix(), QStringLiteral("RL3F9A"));
    QVERIFY(reloaded.prefixSet());
}

void TakMarkerTest::_prefixSeenNotSaved()
{
    TakManager tak;
    readAll(tak);
    commandsOn(tak);
    tak._handleMarker(xml(stationEvent(QStringLiteral("BRAVO-GCS"), QStringLiteral("rotorlab-bravo00000001"))));
    tak._handleMarker(xml(aircraftTrack(QStringLiteral("OTHER-1"), QStringLiteral("FALCON"),
                                        QStringLiteral("rotorlab-charlie000001"), QStringLiteral("CHARLIE-GCS"))));

    // A prefix another station used in the last 2 minutes is not saved (T12b).
    QVERIFY(tak.setProperty("callsignPrefix", QStringLiteral("bravo")));
    QCOMPARE(tak.callsignPrefix(), QStringLiteral("RLTEST"));
    QCOMPARE(tak.prefixEntryError(), TakPolicy::prefixSeenRefusal(QStringLiteral("bravo")));
    QVERIFY(tak.c2Armed());
    QVERIFY(tak.setProperty("callsignPrefix", QStringLiteral("Charlie")));
    QCOMPARE(tak.callsignPrefix(), QStringLiteral("RLTEST"));
    QCOMPARE(tak.prefixEntryError(), TakPolicy::prefixSeenRefusal(QStringLiteral("Charlie")));

    // A change keeps a conflict already found: no window in which commands
    // are read (security M4).
    tak._prefixConflictMs     = QDateTime::currentMSecsSinceEpoch();
    tak._prefixConflictPrefix = QStringLiteral("RLTEST");
    QVERIFY(tak.setProperty("callsignPrefix", QStringLiteral("ALPHA")));
    QCOMPARE(tak.callsignPrefix(), QStringLiteral("ALPHA"));
    QVERIFY(tak.prefixSet());
    QVERIFY(tak._prefixConflictActive());
    tak.setC2Armed(true);
    QVERIFY(tak.c2Armed());
    Ev e;
    e.uid  = QStringLiteral("seen-1");
    e.name = QStringLiteral("HOLD ALPHA-1");
    e.time = QDateTime::currentDateTimeUtc().addSecs(1);
    tak._handleMarker(xml(e));
    QCOMPARE(tak.remoteCommands().first().toMap().value(QStringLiteral("result")).toString(),
             notSent(TakPolicy::prefixConflictRefusal(QStringLiteral("RLTEST"))));
    // Confirming a prefix while it is in conflict is refused as well.
    tak._prefixConflictPrefix = QStringLiteral("ALPHA");
    tak._prefixSetFor.clear();
    QVERIFY(tak.setProperty("callsignPrefix", QStringLiteral("ALPHA")));
    QVERIFY(!tak.prefixSet());
    QCOMPARE(tak.prefixEntryError(), TakPolicy::prefixSeenRefusal(QStringLiteral("ALPHA")));

    // Two minutes after BRAVO was last seen, BRAVO can be saved.
    tak._prefixConflictMs = 0;
    for (auto it = tak._seenStations.begin(); it != tak._seenStations.end(); ++it) {
        it.value() -= (TakPolicy::kAddressSeenSecs + 1) * 1000LL;
    }
    QVERIFY(tak.setProperty("callsignPrefix", QStringLiteral("BRAVO")));
    QCOMPARE(tak.callsignPrefix(), QStringLiteral("BRAVO"));
    QVERIFY(tak.prefixEntryError().isEmpty());
}

void TakMarkerTest::_stationShapedFleetCallsign()
{
    TakManager tak;
    readAll(tak);
    commandsOn(tak);
    // ALPHA's aircraft 3 has the Fleet callsign "BRAVO 1", the name station
    // BRAVO publishes for its aircraft 1. With no BRAVO seen, a marker for
    // BRAVO-1 is still not this station's (security M3).
    tak._fleet[3].callsign = QStringLiteral("BRAVO 1");
    Ev e;
    e.uid  = QStringLiteral("shape-1");
    e.name = QStringLiteral("HOLD BRAVO-1");
    tak._handleMarker(xml(e));
    QCOMPARE(tak.remoteCommands().first().toMap().value(QStringLiteral("result")).toString(),
             notSent(QStringLiteral("no station or aircraft named BRAVO is seen on this TAK server, so this station "
                                    "does not run it. This station's prefix is RLTEST")));
    bool noted = false;
    for (const QVariant &row : tak.fleetModel()) {
        const QVariantMap r = row.toMap();
        if (r.value(QStringLiteral("sysid")).toInt() == 3) {
            QCOMPARE(r.value(QStringLiteral("nameNote")).toString(),
                     QStringLiteral("Markers cannot name this aircraft by BRAVO 1. In a marker, use RLTEST-3."));
            noted = true;
        }
    }
    QVERIFY(noted);
    // A Fleet callsign of more words stays nameable.
    tak._fleet[3].callsign = QStringLiteral("RED HAWK 1");
    e.uid  = QStringLiteral("shape-2");
    e.name = QStringLiteral("HOLD red hawk 1");
    tak._handleMarker(xml(e));
    QCOMPARE(tak.remoteCommands().first().toMap().value(QStringLiteral("result")).toString(),
             notSent(TakPolicy::noAircraftConnected(QStringLiteral("RED-HAWK-1"), false)));

    // A flood of station names does not push out a station already seen,
    // and the table stays bounded.
    tak._handleMarker(xml(stationEvent(QStringLiteral("BRAVO-GCS"), QStringLiteral("rotorlab-bravo00000001"))));
    for (int i = 0; i < TakManager::kMaxAddressSeen + 100; ++i) {
        tak._noteAddressSignals(CoT::parse(xml(stationEvent(QStringLiteral("F%1-GCS").arg(i, 4, 10, QLatin1Char('0')),
                                                            QStringLiteral("rotorlab-flood%1").arg(i)))));
    }
    QVERIFY(tak._seenStations.size() <= TakManager::kMaxAddressSeen);
    QVERIFY(tak._seenStations.contains(QStringLiteral("BRAVO")));
    const qint64 before = tak._seenStations.value(QStringLiteral("BRAVO"));
    QTest::qWait(5);
    tak._noteAddressSignals(CoT::parse(xml(stationEvent(QStringLiteral("BRAVO-GCS"),
                                                        QStringLiteral("rotorlab-bravo00000001")))));
    QVERIFY(tak._seenStations.value(QStringLiteral("BRAVO")) >= before);
    Ev other;
    other.uid  = QStringLiteral("shape-3");
    other.name = QStringLiteral("HOLD BRAVO-2");
    const qsizetype listed = tak.remoteCommands().size();
    tak._handleMarker(xml(other));
    QCOMPARE(tak.remoteCommands().size(), listed);   // still another station's: log only
}

// Laptop two-station run on efd6b36: station ALPHA's aircraft carried the
// Fleet callsign "BRAVO 1", and station BRAVO showed a prefix conflict (T8)
// and refused HOLD BRAVO 1 (T7) until 2 minutes after the name was removed.
// A Fleet name on another station's aircraft is never station presence.
void TakMarkerTest::_fleetNameOnOtherAircraftNoConflict()
{
    TakManager tak;
    readAll(tak);
    tak._callsignPrefix = QStringLiteral("BRAVO");
    tak._prefixSetFor   = QStringLiteral("BRAVO");
    commandsOn(tak);
    int n = 0;
    auto send = [&tak, &n](const QString &name) {
        Ev e;
        e.uid  = QStringLiteral("fn-%1").arg(++n);
        e.name = name;
        tak._handleMarker(xml(e));
        return tak.remoteCommands().first().toMap().value(QStringLiteral("result")).toString();
    };
    const QString alphaUid = QStringLiteral("rotorlab-alpha00000001");
    tak._handleMarker(xml(stationEvent(QStringLiteral("ALPHA-GCS"), alphaUid)));
    // ALPHA's aircraft 1 named "BRAVO 1", "BRAVO-2" and "BRAVO GCS" in its Fleet.
    tak._handleMarker(xml(aircraftTrack(QStringLiteral("ALPHA-1"), QStringLiteral("BRAVO 1"), alphaUid,
                                        QStringLiteral("ALPHA-GCS"))));
    tak._handleMarker(xml(aircraftTrack(QStringLiteral("ALPHA-2"), QStringLiteral("bravo-2"), alphaUid,
                                        QStringLiteral("ALPHA-GCS"))));
    tak._handleMarker(xml(aircraftTrack(QStringLiteral("ALPHA-3"), QStringLiteral("BRAVO GCS"), alphaUid,
                                        QStringLiteral("ALPHA-GCS"))));
    QVERIFY(!tak._prefixConflictActive());
    QVERIFY(tak.prefixConflictNotice().isEmpty());
    tak._updateDropNotice();
    QVERIFY(tak.flyDropNotice().isEmpty());
    QCOMPARE(tak._prefixLastSeenElsewhere(QStringLiteral("BRAVO")), qint64(0));
    QVERIFY(!tak._seenStations.contains(QStringLiteral("BRAVO")));
    QVERIFY(!tak._seenAircraft.contains(QStringLiteral("BRAVO-1")));
    QVERIFY(!tak._seenAircraft.contains(QStringLiteral("BRAVO-2")));
    QVERIFY(tak._seenStations.contains(QStringLiteral("ALPHA")));
    QCOMPARE(tak._seenAircraft.value(QStringLiteral("ALPHA-1")).first, QStringLiteral("ALPHA"));

    // BRAVO's commands stay on: they reach the dispatcher (no aircraft here).
    QVERIFY(tak.c2Armed());
    QCOMPARE(send(QStringLiteral("HOLD BRAVO 1")), notSent(TakPolicy::noAircraftConnected(QStringLiteral("BRAVO-1"), false)));
    QVERIFY(!send(QStringLiteral("RTL BRAVO-ALL")).contains(TakPolicy::prefixConflictRefusal(QStringLiteral("BRAVO"))));
    // ALPHA's own aircraft names stay ALPHA's: logged only, not listed (T5).
    const qsizetype listed = tak.remoteCommands().size();
    send(QStringLiteral("HOLD ALPHA-1"));
    QCOMPARE(tak.remoteCommands().size(), listed);

    // T15: "BRAVO 1" is still not nameable on BRAVO's own Fleet either, and
    // T10: a nameable Fleet name another station also shows still conflicts.
    QVERIFY(!TakPolicy::fleetCallsignNameable(QStringLiteral("BRAVO 1"), QStringLiteral("BRAVO")));
    QVERIFY(!TakPolicy::fleetCallsignNameable(QStringLiteral("BRAVO 1"), QStringLiteral("ALPHA")));
    tak._fleet[4].callsign = QStringLiteral("HAWK");
    QCOMPARE(send(QStringLiteral("HOLD HAWK")), notSent(TakPolicy::noAircraftConnected(QStringLiteral("HAWK"), false)));
    tak._handleMarker(xml(aircraftTrack(QStringLiteral("ALPHA-4"), QStringLiteral("Hawk"), alphaUid,
                                        QStringLiteral("ALPHA-GCS"))));
    QCOMPARE(send(QStringLiteral("HOLD HAWK")),
             notSent(QStringLiteral("HAWK is also the callsign of an aircraft another station publishes. Name the "
                                    "aircraft as BRAVO-4")));
    QVERIFY(!tak._prefixConflictActive());
}

// The real conflicts still fail closed: a BRAVO-<n> track parented to
// another station's -GCS, and BRAVO-GCS from another uid.
void TakMarkerTest::_realPrefixTrackStillConflicts()
{
    TakManager tak;
    readAll(tak);
    tak._callsignPrefix = QStringLiteral("BRAVO");
    tak._prefixSetFor   = QStringLiteral("BRAVO");
    commandsOn(tak);
    int n = 0;
    auto send = [&tak, &n](const QString &name) {
        Ev e;
        e.uid  = QStringLiteral("rc-%1").arg(++n);
        e.name = name;
        tak._handleMarker(xml(e));
        return tak.remoteCommands().first().toMap().value(QStringLiteral("result")).toString();
    };
    const QString conflict = notSent(TakPolicy::prefixConflictRefusal(QStringLiteral("BRAVO")));
    const QString alphaUid = QStringLiteral("rotorlab-alpha00000001");

    // A track whose uid is BRAVO-1, parented to ALPHA-GCS, whatever its callsign.
    tak._handleMarker(xml(aircraftTrack(QStringLiteral("BRAVO-1"), QStringLiteral("FALCON"), alphaUid,
                                        QStringLiteral("ALPHA-GCS"))));
    QVERIFY(tak._prefixConflictActive());
    QVERIFY(!tak.prefixConflictNotice().isEmpty());
    QCOMPARE(send(QStringLiteral("HOLD BRAVO 1")), conflict);
    tak._prefixConflictMs = 0;
    tak._updateConflictNotice();
    QVERIFY(tak.prefixConflictNotice().isEmpty());

    // A track reported for BRAVO-GCS by another uid.
    tak._handleMarker(xml(aircraftTrack(QStringLiteral("OTHER-9"), QStringLiteral("OWL"),
                                        QStringLiteral("rotorlab-other0000001"), QStringLiteral("BRAVO-GCS"))));
    QVERIFY(tak._prefixConflictActive());
    QCOMPARE(send(QStringLiteral("HOLD BRAVO-1")), conflict);
    tak._prefixConflictMs = 0;

    // BRAVO-GCS from another station uid.
    tak._handleMarker(xml(stationEvent(QStringLiteral("Bravo GCS"), QStringLiteral("rotorlab-other0000002"))));
    QVERIFY(tak._prefixConflictActive());
    QCOMPARE(send(QStringLiteral("RTL BRAVO-ALL")), conflict);

    // This station's own events relayed back never count.
    tak._prefixConflictMs = 0;
    tak._handleMarker(xml(stationEvent(QStringLiteral("BRAVO-GCS"), tak._gcsUid)));
    tak._handleMarker(xml(aircraftTrack(QStringLiteral("BRAVO-1"), QStringLiteral("BRAVO-1"), tak._gcsUid,
                                        QStringLiteral("BRAVO-GCS"))));
    QVERIFY(!tak._prefixConflictActive());
}
