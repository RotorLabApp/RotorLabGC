/****************************************************************************
 *
 * RotorLab GC TAK integration - station-side policy helpers.
 *
 ****************************************************************************/

#include "TakPolicy.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

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
    switch (cmd.target) {
    case Tak::Target::All:
        return QStringLiteral("all aircraft");
    case Tak::Target::Sel:
        return QStringLiteral("selected aircraft");
    case Tak::Target::Ids:
        break;
    }
    QStringList ids;
    for (int id : cmd.ids) {
        ids << QString::number(id);
    }
    return ids.isEmpty() ? QStringLiteral("no aircraft named")
                         : QStringLiteral("aircraft ") + ids.join(QStringLiteral(", "));
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

} // namespace TakPolicy
