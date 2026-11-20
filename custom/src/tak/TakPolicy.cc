/****************************************************************************
 *
 * RotorLab GC TAK integration - station-side policy helpers.
 *
 ****************************************************************************/

#include "TakPolicy.h"

#include "CoTMessage.h"

#include <cmath>

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>

namespace TakPolicy {

QString alertTitle()
{
    return QStringLiteral("Remote command received");
}

QString alertDisclaimer()
{
    return QStringLiteral("Sent to the aircraft when received. This alert does not hold or cancel it. "
                          "Sender identity is as reported by ATAK and is not verified by this station. "
                          "Sent means transmitted to the aircraft. The aircraft's response is not shown here.");
}

bool isProminentVerb(Tak::Verb v)
{
    return v == Tak::Verb::Arm || v == Tak::Verb::Takeoff || v == Tak::Verb::Goto;
}

QString senderText(const QString &callsign, const QString &uid)
{
    const QString cs = callsign.trimmed();
    const QString id = uid.trimmed();
    if (!cs.isEmpty() && !id.isEmpty()) {
        return QStringLiteral("%1 (UID %2)").arg(cs, id);
    }
    if (!cs.isEmpty()) {
        return cs;
    }
    if (!id.isEmpty()) {
        return QStringLiteral("UID %1").arg(id);
    }
    return QStringLiteral("(sender not reported)");
}

QString intendedTargetText(const Tak::Command &cmd)
{
    const QString name = Tak::normalizeName(cmd.name);
    switch (cmd.target) {
    case Tak::Target::All:
        return name.isEmpty() ? QStringLiteral("all aircraft (no station named)")
                              : QStringLiteral("all aircraft of %1").arg(name);
    case Tak::Target::Sel:
        return QStringLiteral("SEL (not accepted)");
    case Tak::Target::Name:
        return name;
    case Tak::Target::Ids:
        break;
    }
    QStringList ids;
    for (int id : cmd.ids) {
        ids << (name.isEmpty() ? QString::number(id) : QStringLiteral("%1-%2").arg(name).arg(id));
    }
    if (ids.isEmpty()) {
        return QStringLiteral("no aircraft named");
    }
    return QStringLiteral("aircraft ") + ids.join(QStringLiteral(", "))
         + (name.isEmpty() ? QStringLiteral(" (no station named)") : QString());
}

QString positionText(double lat, double lon, int altFt)
{
    const QString at = altFt >= 0 ? QStringLiteral("%1 ft above launch").arg(altFt)
                                  : QStringLiteral("current altitude");
    return QStringLiteral("%1, %2 at %3").arg(lat, 0, 'f', 6).arg(lon, 0, 'f', 6).arg(at);
}

static QString sentNoun(Tak::Verb v)
{
    switch (v) {
    case Tak::Verb::Goto:    return QStringLiteral("go-to");
    case Tak::Verb::Takeoff: return QStringLiteral("takeoff");
    case Tak::Verb::Rtl:     return QStringLiteral("return-to-launch");
    case Tak::Verb::Land:    return QStringLiteral("land");
    case Tak::Verb::Hold:    return QStringLiteral("hold");
    case Tak::Verb::Guided:  return QStringLiteral("guided-mode");
    case Tak::Verb::Arm:     return QStringLiteral("arm");
    case Tak::Verb::Disarm:  return QStringLiteral("disarm");
    case Tak::Verb::AltHold: return QStringLiteral("altitude-change");
    case Tak::Verb::Invalid: break;
    }
    return QStringLiteral("unknown");
}

QString sentResultText(Tak::Verb v, bool ok, const QString &detail, bool alreadyArmed)
{
    if (alreadyArmed) {
        return QStringLiteral("not sent: telemetry reports the aircraft already armed");
    }
    if (ok) {
        return QStringLiteral("%1 command sent").arg(sentNoun(v));
    }
    // The dispatcher's one state-based refusal, said as what telemetry reports.
    if (detail == QLatin1String("the aircraft is not armed")) {
        return QStringLiteral("not sent: telemetry reports the aircraft not armed");
    }
    return QStringLiteral("not sent: %1").arg(detail);
}

static QString capitalized(const QString &s)
{
    return s.isEmpty() ? s : s.at(0).toUpper() + s.mid(1);
}

QString dispatchResultText(const QList<int> &sysids, const QStringList &results, int sentCount)
{
    if (results.size() == 1) {
        return capitalized(results.first());
    }
    QStringList parts;
    for (int i = 0; i < results.size(); ++i) {
        parts << QStringLiteral("aircraft %1 %2").arg(i < sysids.size() ? sysids.at(i) : 0).arg(results.at(i));
    }
    return QStringLiteral("%1 of %2 sent (%3)").arg(sentCount).arg(results.size())
        .arg(parts.join(QStringLiteral("; ")));
}

QString notSentResult(const QString &why)
{
    return QStringLiteral("Not sent: ") + why;
}

bool pointKnown(double lat, double lon)
{
    if (!std::isfinite(lat) || !std::isfinite(lon)) {
        return false;
    }
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
        return false;
    }
    return !(lat == 0.0 && lon == 0.0);
}

QString gotoMultipleRefusal()
{
    return QStringLiteral("GOTO names more than one aircraft. Place one marker per aircraft");
}

QString gotoUnknownPointRefusal()
{
    return QStringLiteral("the GOTO point is unknown");
}

QString takeoffNeedsArmRefusal()
{
    return QStringLiteral("the aircraft is not armed and ARM is off (TAK settings). "
                          "TKOFF does not arm the aircraft while ARM is off");
}

QString unknownPointText(int altFt)
{
    const QString at = altFt >= 0 ? QStringLiteral("%1 ft above launch").arg(altFt)
                                  : QStringLiteral("current altitude");
    return QStringLiteral("unknown point at %1").arg(at);
}

QString summaryLine(const RemoteCommand &c)
{
    QString line = QStringLiteral("%1, reported sender %2, to %3")
                       .arg(c.verb, senderText(c.senderCallsign, c.senderUid),
                            c.targets.isEmpty() ? QStringLiteral("no aircraft")
                                                : c.targets.join(QStringLiteral(", ")));
    if (!c.position.isEmpty()) {
        line += QStringLiteral(" (") + c.position + QLatin1Char(')');
    }
    if (!c.result.isEmpty()) {
        line += QStringLiteral(": ") + c.result;
    }
    if (c.test) {
        line += QStringLiteral(" [station test]");
    }
    return line;
}

QJsonObject logRecord(const RemoteCommand &c)
{
    QJsonObject o;
    o[QStringLiteral("kind")]                   = QStringLiteral("command");
    o[QStringLiteral("verb")]                   = c.verb;
    o[QStringLiteral("marker")]                 = c.markerText;
    o[QStringLiteral("reportedSenderCallsign")] = c.senderCallsign;
    o[QStringLiteral("reportedSenderUid")]      = c.senderUid;
    o[QStringLiteral("target")]                 = c.targets.join(QStringLiteral(", "));
    o[QStringLiteral("position")]               = c.position;
    o[QStringLiteral("result")]                 = c.result;
    o[QStringLiteral("warning")]                = c.warning;
    o[QStringLiteral("test")]                   = c.test;
    return o;
}

QByteArray chainRecord(QJsonObject record, const QString &prevHash)
{
    record[QStringLiteral("schemaVersion")] = kCommandLogSchema;
    record[QStringLiteral("prevHash")]      = prevHash;
    return QJsonDocument(record).toJson(QJsonDocument::Compact);
}

QString lineHash(const QByteArray &line)
{
    return QString::fromLatin1(QCryptographicHash::hash(line, QCryptographicHash::Sha256).toHex());
}

static const QLatin1String kUidPrefix("uid:");
static const QLatin1String kCallsignPrefix("callsign:");

PilotEntry parsePilotEntry(const QString &stored)
{
    const QString s = stored.trimmed();
    if (s.startsWith(kUidPrefix, Qt::CaseInsensitive)) {
        return { PilotKind::Uid, s.mid(kUidPrefix.size()).trimmed() };
    }
    if (s.startsWith(kCallsignPrefix, Qt::CaseInsensitive)) {
        return { PilotKind::Callsign, s.mid(kCallsignPrefix.size()).trimmed() };
    }
    return { PilotKind::Any, s };
}

QString formatPilotEntry(PilotKind kind, const QString &value)
{
    const QString v = value.trimmed();
    switch (kind) {
    case PilotKind::Uid:      return QString(kUidPrefix) + v;
    case PilotKind::Callsign: return QString(kCallsignPrefix) + v;
    case PilotKind::Any:      break;
    }
    return v;
}

QString pilotKindName(PilotKind kind)
{
    switch (kind) {
    case PilotKind::Uid:      return QStringLiteral("uid");
    case PilotKind::Callsign: return QStringLiteral("callsign");
    case PilotKind::Any:      break;
    }
    return QStringLiteral("any");
}

PilotKind pilotKindFromName(const QString &name)
{
    if (name.compare(QLatin1String("uid"), Qt::CaseInsensitive) == 0) {
        return PilotKind::Uid;
    }
    if (name.compare(QLatin1String("callsign"), Qt::CaseInsensitive) == 0) {
        return PilotKind::Callsign;
    }
    return PilotKind::Any;
}

bool senderOnList(const QStringList &entries, const QString &callsign, const QString &uid)
{
    const QString cs = callsign.trimmed();
    const QString id = uid.trimmed();
    for (const QString &stored : entries) {
        const PilotEntry e = parsePilotEntry(stored);
        if (e.value.isEmpty()) {
            continue;
        }
        const bool uidMatch = !id.isEmpty() && e.value.compare(id, Qt::CaseInsensitive) == 0;
        const bool csMatch  = !cs.isEmpty() && e.value.compare(cs, Qt::CaseInsensitive) == 0;
        switch (e.kind) {
        case PilotKind::Uid:
            if (uidMatch) return true;
            break;
        case PilotKind::Callsign:
            if (csMatch) return true;
            break;
        case PilotKind::Any:
            if (uidMatch || csMatch) return true;
            break;
        }
    }
    return false;
}

QString sha256Fingerprint(const QByteArray &der)
{
    if (der.isEmpty()) {
        return QString();
    }
    return QString::fromLatin1(QCryptographicHash::hash(der, QCryptographicHash::Sha256).toHex(':')).toUpper();
}

static QString normalizedFingerprint(const QString &f)
{
    QString out;
    for (const QChar c : f) {
        if (c.isLetterOrNumber()) {
            out += c.toUpper();
        }
    }
    return out;
}

bool fingerprintsMatch(const QString &a, const QString &b)
{
    const QString na = normalizedFingerprint(a);
    return !na.isEmpty() && na == normalizedFingerprint(b);
}

bool enrollMaySend(bool verifyPeer, bool chainVerified,
                   const QString &fingerprint, const QString &pinned)
{
    if (fingerprintsMatch(fingerprint, pinned)) {
        return true;
    }
    return verifyPeer && chainVerified;
}

QString openSenderWarning(bool c2On, bool c2Armed, bool approvedOnly)
{
    if (c2On && c2Armed && !approvedOnly) {
        return QStringLiteral("Any sender on this TAK server can command the aircraft.");
    }
    return QString();
}

QString notOnListResult()
{
    return QStringLiteral("Refused: reported sender is not on this station's ATAK command senders list");
}

QString testIdentityResult()
{
    return QStringLiteral("Refused: reported sender uses this station's test identity");
}

QString parseFailedResult(const QString &reason)
{
    return notSentResult(QStringLiteral("command not understood (%1)").arg(reason));
}

QString testSenderCallsign()
{
    return QStringLiteral("STATION-TEST");
}

QString testSenderUid()
{
    return QStringLiteral("rotorlab-station-test");
}

bool claimsTestSender(const QString &callsign, const QString &uid)
{
    return callsign.trimmed().compare(testSenderCallsign(), Qt::CaseInsensitive) == 0
        || uid.trimmed().compare(testSenderUid(), Qt::CaseInsensitive) == 0;
}

QString altitudeWarning(int commandedFt, double warnAboveFt)
{
    if (commandedFt < 0 || !(warnAboveFt > 0.0) || commandedFt <= warnAboveFt) {
        return QString();
    }
    return QStringLiteral("Commanded altitude %1 ft (above launch) is above this station's "
                          "warning level of %2 ft. Above launch is not above ground level.")
        .arg(commandedFt)
        .arg(QString::number(warnAboveFt, 'f', 0));
}

QString sentAltitudeWarning(bool sent, int commandedFt, double warnAboveFt)
{
    return sent ? altitudeWarning(commandedFt, warnAboveFt) : QString();
}

static const char *const kTimeReasons[] = {
    "time is more than 5 minutes ahead of this station's clock",
    "placed more than 24 hours ago",
    "placed before this station started; a marker from before a restart is not run",
    "older than this station can check for repeats",
};

QString markerNotCommandReason(const CoT::Marker &m, const QDateTime &nowUtc, const QDateTime &notBeforeUtc,
                               const QDateTime &repeatFloorUtc)
{
    if (m.uid.trimmed().isEmpty()) {
        return QStringLiteral("no marker UID");
    }
    if (m.isEud) {
        return QStringLiteral("device position report, not a placed marker");
    }
    if (m.how.startsWith(QLatin1String("m-"), Qt::CaseInsensitive)) {
        return QStringLiteral("machine-generated, not placed by a person");
    }
    if (!m.how.startsWith(QLatin1String("h-"), Qt::CaseInsensitive)) {
        return QStringLiteral("not marked as placed by a person");
    }
    if (!m.hasParentLink || m.senderUid.trimmed().isEmpty()) {
        return QStringLiteral("no creating device named");
    }
    if (m.senderUid.trimmed().compare(m.uid.trimmed(), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("device report about itself, not a placed marker");
    }
    if (!m.time.isValid() || !m.stale.isValid()) {
        return QStringLiteral("no valid time or stale time");
    }
    if (m.stale <= nowUtc) {
        return QStringLiteral("stale");
    }
    if (m.time > nowUtc.addSecs(5 * 60)) {
        return QLatin1String(kTimeReasons[0]);
    }
    if (m.time < nowUtc.addSecs(-24 * 3600)) {
        return QLatin1String(kTimeReasons[1]);
    }
    if (notBeforeUtc.isValid() && m.time < notBeforeUtc) {
        return QLatin1String(kTimeReasons[2]);
    }
    if (repeatFloorUtc.isValid() && m.time <= repeatFloorUtc) {
        return QLatin1String(kTimeReasons[3]);
    }
    return QString();
}

QString rateLimitLine(int dropped, int perSecond, int perMinute)
{
    return QStringLiteral("marker ignored: more than %1 command markers in one second or %2 in one minute; "
                          "%3 not read")
        .arg(perSecond).arg(perMinute).arg(dropped);
}

QString emptyPrefixReason()
{
    return QStringLiteral("the callsign prefix is empty, so this station reads no commands");
}

QString emptyPrefixNotice()
{
    return QStringLiteral("Commands are not read: the callsign prefix is empty. "
                          "Set a callsign prefix under TAK / OpenTAKServer.");
}

QString markerIgnoredLine(const QString &reason, const QString &name, const QString &uid)
{
    if (name.isEmpty() && uid.isEmpty()) {
        return QStringLiteral("marker ignored: %1").arg(reason);
    }
    return QStringLiteral("marker ignored: %1 (\"%2\", UID %3)")
        .arg(reason, name, uid.isEmpty() ? QStringLiteral("not reported") : uid);
}

QString ignoredLinesDroppedLine(int dropped, int perSecond)
{
    return QStringLiteral("%1 more \"marker ignored\" lines not shown (at most %2 a second)")
        .arg(dropped).arg(perSecond);
}

bool isMarkerTimeReason(const QString &reason)
{
    for (const char *r : kTimeReasons) {
        if (reason == QLatin1String(r)) {
            return true;
        }
    }
    return false;
}

QString droppedEarlierReason()
{
    return QStringLiteral("not read earlier this session (rate limit or repeat check); not run when delivered again");
}

QString flyDropNotice(int total)
{
    if (total <= 0) {
        return QString();
    }
    return QStringLiteral("ATAK command markers not read or not run: %1 so far. "
                          "The commands in them were not sent to the aircraft.").arg(total);
}

QString repeatListFullReason()
{
    return QStringLiteral("too many markers received this session to check for repeats; not run");
}

QString commandDropNotice(int rateDropped, int fullDropped, int timeDropped, int perSecond, int perMinute)
{
    QStringList lines;
    if (rateDropped > 0) {
        lines << QStringLiteral("ATAK command markers not read because more arrived than this station reads "
                                "(%2 a second, %3 a minute): %1 so far. This notice clears after a minute with none.")
                     .arg(rateDropped).arg(perSecond).arg(perMinute);
    }
    if (fullDropped > 0) {
        lines << QStringLiteral("ATAK command markers not run because too many markers were received this session "
                                "to check for repeats: %1 so far. This notice clears after a minute with none.")
                     .arg(fullDropped);
    }
    if (timeDropped > 0) {
        lines << QStringLiteral("ATAK command markers not run because of the marker time (ahead of this station's "
                                "clock, placed before this station started, more than 24 hours old, or older than "
                                "this station can check for repeats): %1 so far. The marker time is set by the "
                                "sending device's clock. This notice clears after a minute with none.")
                     .arg(timeDropped);
    }
    return lines.join(QLatin1Char('\n'));
}

// ---------------------------------------------------------------------------
// Addressing (G47) and SEL (G48)
// ---------------------------------------------------------------------------

QString prefixProblem(const QString &prefix)
{
    static const QRegularExpression format(QStringLiteral("^[A-Za-z][A-Za-z0-9]{1,11}$"));
    if (!format.match(prefix).hasMatch()) {
        return QStringLiteral("use 2 to 12 letters and digits, starting with a letter");
    }
    if (Tak::isReservedWord(prefix.toUpper())) {
        return QStringLiteral("%1 is a command word").arg(prefix.toUpper());
    }
    if (isSharedDefaultPrefix(prefix)) {
        return sharedDefaultPrefixReason();
    }
    return QString();
}

bool isSharedDefaultPrefix(const QString &prefix)
{
    return prefix.trimmed().compare(QLatin1String("ROTOR"), Qt::CaseInsensitive) == 0;
}

QString sharedDefaultPrefixReason()
{
    return QStringLiteral("ROTOR was the shared default in earlier versions; choose a prefix only this station uses.");
}

bool fleetCallsignNameable(const QString &callsign, const QString &prefix)
{
    static const QRegularExpression chars(QStringLiteral("^[A-Za-z0-9 \\-]+$"));
    static const QRegularExpression hasLetter(QStringLiteral("[A-Z]"));
    const QString t = callsign.trimmed();
    if (t.isEmpty() || !chars.match(t).hasMatch()) {
        return false;
    }
    const QString n = Tak::normalizeName(t);
    if (n.isEmpty() || n.size() > Tak::kMaxNameChars) {
        return false;
    }
    const QStringList words = n.split(QLatin1Char('-'), Qt::SkipEmptyParts);
    if (words.isEmpty() || words.size() > Tak::kMaxNameWords) {
        return false;
    }
    static const QStringList reserved = { QStringLiteral("ALT"), QStringLiteral("SPD"), QStringLiteral("ALL"),
                                          QStringLiteral("SEL"), QStringLiteral("GCS") };
    for (int i = 0; i < words.size(); ++i) {
        if (reserved.contains(words.at(i))) {
            return false;
        }
        // Only the last word may be a number: "HAWK 1" reads as HAWK,
        // aircraft 1, while "HAWK 1 B" would not parse.
        if (i < words.size() - 1 && !hasLetter.match(words.at(i)).hasMatch()) {
            return false;
        }
    }
    if (!hasLetter.match(n).hasMatch()) {
        return false;   // a number alone reads as an aircraft number
    }
    // Shaped like any station's aircraft name, <prefix>-<number> (security
    // M3): "BRAVO 1" is the uid station BRAVO publishes for its aircraft 1,
    // whether or not BRAVO is seen on the link. A name of more words with a
    // number ("RED HAWK 1") cannot be one, because a prefix is one word.
    static const QRegularExpression stationAircraft(QStringLiteral("^[A-Z][A-Z0-9]{1,11}-[0-9]+$"));
    if (stationAircraft.match(n).hasMatch()) {
        return false;
    }
    const QString own = Tak::normalizeName(prefix);
    if (!own.isEmpty()) {
        const QRegularExpression ownForm(QStringLiteral("^%1(-[0-9]+)?$").arg(QRegularExpression::escape(own)));
        if (ownForm.match(n).hasMatch()) {
            return false;
        }
    }
    return true;
}

QString stationPrefixOf(const QString &callsign)
{
    static const QRegularExpression station(QStringLiteral("^([A-Z][A-Z0-9]{1,11})-GCS$"));
    const QRegularExpressionMatch mo = station.match(Tak::normalizeName(callsign));
    return mo.hasMatch() ? mo.captured(1) : QString();
}

AddressResult classifyAddress(const Tak::Command &cmd, const AddressInput &in)
{
    AddressResult a;
    const QString nm = Tak::normalizeName(cmd.name);
    a.name = nm;
    if (cmd.target == Tak::Target::Sel) {
        a.kind = Address::Sel;
        return a;
    }
    if (nm.isEmpty()) {
        a.kind = Address::Unaddressed;
        return a;
    }
    const QString own = Tak::normalizeName(in.ownPrefix);
    const bool single = cmd.target == Tak::Target::Ids && cmd.ids.size() == 1;
    const QString key = cmd.target == Tak::Target::Name ? nm
                      : single ? QStringLiteral("%1-%2").arg(nm).arg(cmd.ids.first())
                               : QString();
    if (single) {
        a.name = key;
    }

    auto finishOwn = [&cmd](AddressResult r) {
        r.kind = Address::Own;
        if (cmd.sentToId >= 0 && (r.all || r.ids != QList<int>{ cmd.sentToId })) {
            r.kind = Address::Mismatch;
        }
        return r;
    };

    // An aircraft's Fleet callsign, as ATAK shows it.
    if (!key.isEmpty() && in.fleetNames.contains(key)) {
        a.ids = { in.fleetNames.value(key) };
        if (in.fleetConflicts.contains(key) || in.otherAircraft.contains(key)
            || in.otherStations.contains(nm)) {
            a.kind = Address::FleetConflict;
            return a;
        }
        return finishOwn(a);
    }
    // This station's prefix.
    if (!own.isEmpty() && nm == own) {
        if (cmd.target == Tak::Target::Name) {
            a.kind = Address::NoAircraft;
            return a;
        }
        a.all = cmd.target == Tak::Target::All;
        a.ids = cmd.ids;
        if (a.all) {
            a.name = QStringLiteral("%1-ALL").arg(nm);
        }
        return finishOwn(a);
    }
    // Another station seen on this link.
    if (!key.isEmpty() && in.otherAircraft.contains(key)) {
        a.kind    = Address::OtherStation;
        a.station = in.otherAircraft.value(key);
        return a;
    }
    if (in.otherStations.contains(nm)) {
        a.kind    = Address::OtherStation;
        a.station = nm;
        if (cmd.target == Tak::Target::All) {
            a.name = QStringLiteral("%1-ALL").arg(nm);
        }
        return a;
    }
    a.kind = Address::UnknownStation;
    return a;
}

QString otherStationReason(const QString &name, const QString &station, const QString &ownPrefix)
{
    return QStringLiteral("addressed to %1, which station %2 publishes, not this station (%3)")
        .arg(name, station, Tak::normalizeName(ownPrefix));
}

QString addressRefusal(const AddressResult &a, const Tak::Command &cmd, const QString &ownPrefix)
{
    const QString own  = Tak::normalizeName(ownPrefix);
    const QString verb = Tak::verbName(cmd.verb);
    switch (a.kind) {
    case Address::Own:
        return QString();
    case Address::Sel:
        return QStringLiteral("SEL is not accepted from ATAK, so this station does not run it. The aircraft "
                              "selected on this station are chosen here and can change at any time, so a marker "
                              "cannot say which aircraft it reaches. A marker must name each aircraft by its number, "
                              "as in %1 %2-1 for aircraft 1").arg(verb, own);
    case Address::Unaddressed:
        if (cmd.target == Tak::Target::All) {
            return QStringLiteral("ALL does not name a station, so this station does not run it. %1-ALL names "
                                  "every aircraft of this station").arg(own);
        }
        return QStringLiteral("the marker does not name a station, so this station does not run it. If it was "
                              "meant for an aircraft of this station, the marker must name it, as in %1 %2-%3")
            .arg(verb, own).arg(cmd.ids.isEmpty() ? 1 : cmd.ids.first());
    case Address::NoAircraft:
        return QStringLiteral("the marker names this station (%1) but no aircraft, so this station does not run "
                              "it. A marker must name the aircraft by its number, as in %2 %1-1 for aircraft 1")
            .arg(own, verb);
    case Address::UnknownStation:
        return QStringLiteral("no station or aircraft named %1 is seen on this TAK server, so this station does "
                              "not run it. This station's prefix is %2").arg(Tak::normalizeName(cmd.name), own);
    case Address::OtherStation:
        return otherStationReason(a.name, a.station, ownPrefix);
    case Address::Mismatch:
        return QStringLiteral("the marker text names %1 but the marker was sent to %2, so it is not clear which "
                              "aircraft it is for, and this station does not run it. A marker sent to an aircraft "
                              "must name that same aircraft").arg(a.name, cmd.sentTo);
    case Address::FleetConflict:
        return QStringLiteral("%1 is also the callsign of an aircraft another station publishes. Name the aircraft "
                              "as %2-%3")
            .arg(a.name, own).arg(a.ids.isEmpty() ? 1 : a.ids.first());
    }
    return QString();
}

QString otherStationEarlierReason()
{
    return QStringLiteral("this marker was addressed to another station earlier this session; a changed or "
                          "repeated marker is not run here");
}

QString prefixConflictRefusal(const QString &prefix)
{
    return QStringLiteral("another station on this TAK server also uses the callsign prefix %1, so a marker "
                          "cannot say which station it is for. Each station needs its own prefix (TAK settings)")
        .arg(Tak::normalizeName(prefix));
}

QString prefixConflictNotice(const QString &prefix, const QString &lastSeenUtc)
{
    return QStringLiteral("Commands from ATAK markers are not sent to the aircraft: another station on this "
                          "TAK server also uses the callsign prefix %1 (last seen %2 UTC). Give each station its "
                          "own prefix under TAK / OpenTAKServer.").arg(Tak::normalizeName(prefix), lastSeenUtc);
}

QString prefixConflictFlyLine(const QString &prefix)
{
    return QStringLiteral("ATAK commands not sent to the aircraft: another station uses prefix %1 (TAK settings)")
        .arg(Tak::normalizeName(prefix));
}

QString prefixEntryRefusal()
{
    return QStringLiteral("Not saved: use 2 to 12 letters and digits, starting with a letter. Command words such "
                          "as GOTO, ALL, SEL, ALT, SPD and GCS cannot be a prefix.");
}

QString sharedDefaultPrefixRefusal()
{
    return QStringLiteral("Not saved: ") + sharedDefaultPrefixReason();
}

QString prefixInvalidNotice(const QString &prefix)
{
    return QStringLiteral("Commands are not read: the callsign prefix %1 cannot name this station. Set a prefix "
                          "of 2 to 12 letters and digits under TAK / OpenTAKServer.").arg(prefix);
}

QString prefixInvalidReason()
{
    return QStringLiteral("the callsign prefix cannot name this station, so this station reads no commands");
}

QString prefixChangedLine(const QString &from, const QString &to)
{
    return QStringLiteral("Callsign prefix changed from %1 to %2. \"Commands to aircraft on\" was turned off. "
                          "Markers placed before the change are not sent to the aircraft.").arg(from, to);
}

QString fleetNotNameableNote(const QString &callsign, const QString &prefix, int sysid)
{
    return QStringLiteral("Markers cannot name this aircraft by %1. In a marker, use %2-%3.")
        .arg(callsign.trimmed(), Tak::normalizeName(prefix)).arg(sysid);
}

QString defaultPrefixFor(const QString &stationUid)
{
    // Only the random station uid: never the host, user, tail number or
    // site, because the prefix is broadcast in every station event.
    QString hex;
    for (const QChar ch : stationUid) {
        if (ch.isLetterOrNumber() && ch.unicode() < 128) {
            hex += ch.toUpper();
        }
    }
    if (hex.startsWith(QStringLiteral("ROTORLAB"))) {
        hex.remove(0, 8);
    }
    while (hex.size() < 4) {
        hex.prepend(QLatin1Char('0'));
    }
    return QStringLiteral("RL") + hex.right(4);
}

QString prefixSeenRefusal(const QString &prefix)
{
    return QStringLiteral("Not saved: another station on this TAK server used the callsign prefix %1 in the last "
                          "2 minutes. Choose a prefix no other station uses.").arg(Tak::normalizeName(prefix));
}

QString prefixNotSetNotice(const QString &prefix)
{
    return QStringLiteral("Commands are not read: the callsign prefix %1 has not been set on this station. Each "
                          "station on a TAK server needs its own prefix. Set or confirm it under TAK / "
                          "OpenTAKServer.").arg(prefix);
}

QString prefixNotSetReason()
{
    return QStringLiteral("the callsign prefix has not been set on this station, so this station reads no commands");
}

QString prefixNotSetArmLine()
{
    return QStringLiteral("\"Commands to aircraft on\" not turned on: set this station's callsign prefix first "
                          "(TAK settings)");
}

QString prefixFloorReason()
{
    return QStringLiteral("placed before this station's callsign prefix changed; not run");
}

QString noAircraftConnected(const QString &name, bool several)
{
    if (several || name.isEmpty()) {
        return QStringLiteral("none of the named aircraft is connected to this station");
    }
    return QStringLiteral("no aircraft %1 is connected to this station").arg(name);
}

QString addressingHelp(const QString &prefix)
{
    const QString p = Tak::normalizeName(prefix);
    return QStringLiteral("A marker names the aircraft as this station publishes it to TAK: %1-1, %1-1,3, "
                          "%1-ALL, or the aircraft's Fleet callsign. A marker that names no station, or names "
                          "SEL, is not sent to any aircraft by this station; another station on the server may "
                          "still act on it. The name only routes a marker to a station. It does not show who "
                          "placed the marker, and anyone on this TAK server can write any name.").arg(p);
}

} // namespace TakPolicy
