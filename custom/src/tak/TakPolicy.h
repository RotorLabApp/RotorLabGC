/****************************************************************************
 *
 * RotorLab GC TAK integration - station-side policy helpers.
 *
 * Pure functions with no vehicle, socket or settings, so they unit-test
 * offline. TakManager calls them; the TAK page and the fly-view banner show
 * what they produce.
 *
 * Nothing here holds, delays or cancels a command. The station alert is
 * written after the command has gone to the shared dispatcher.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include "TakCommand.h"

namespace TakPolicy {

/// Heading of every station alert for a command that came from ATAK.
QString alertTitle();

/// Line shown under every banner.
QString alertDisclaimer();

/// ARM, TKOFF and GOTO are also shown as a banner over the fly view; every
/// verb is kept in the list on the TAK page.
bool isProminentVerb(Tak::Verb v);

/// The sender as ATAK reported it: "WOLF (UID ANDROID-123)", "WOLF",
/// "UID ANDROID-123" or "(sender not reported)". Not verified by the station.
QString senderText(const QString &callsign, const QString &uid);

/// The aircraft a command names before it is resolved: "aircraft 2, 3",
/// "all aircraft" or "selected aircraft".
QString intendedTargetText(const Tak::Command &cmd);

/// Where a GOTO sends the aircraft, in the units of the command:
/// "-35.360000, 149.160000 at 400 ft above launch", or "... at current altitude".
QString positionText(double lat, double lon, int altFt);

/// What one aircraft's result says in the station alert: what was SENT, never
/// an aircraft state the dispatcher did not read back. `ok` and `detail` are
/// the shared dispatcher's result; `alreadyArmed` is the one case where the
/// dispatcher read the aircraft's state and sent nothing. Lower case, e.g.
/// "arm command sent" or "not sent: telemetry reports the aircraft not armed".
QString sentResultText(Tak::Verb v, bool ok, const QString &detail, bool alreadyArmed);

/// The result line of a command that went to the dispatcher. One aircraft:
/// its result, capitalized ("Not sent: telemetry reports the aircraft not
/// armed"). Several: "2 of 3 sent (aircraft 1 arm command sent; aircraft 2
/// not sent: ...; aircraft 3 arm command sent)". `results` holds one
/// sentResultText per entry of `sysids`.
QString dispatchResultText(const QList<int> &sysids, const QStringList &results, int sentCount);

/// "Not sent: <why>", for a command a station setting kept from the aircraft.
QString notSentResult(const QString &why);

/// What the station shows about one received command.
struct RemoteCommand {
    QString     verb;            ///< canonical verb name, e.g. "ARM"
    QString     markerText;      ///< the marker text as received
    QString     senderCallsign;  ///< as reported by ATAK
    QString     senderUid;       ///< as reported by ATAK
    QStringList targets;         ///< resolved aircraft labels, or the intended target
    QString     position;        ///< GOTO destination (positionText), empty otherwise
    QString     result;          ///< what was sent, or why nothing was sent
    QString     warning;         ///< an extra warning line, empty when none
    bool        test = false;    ///< came from this station's test button
    bool        sent = false;    ///< a command went to at least one aircraft
};

/// One line for the TAK page list, e.g. "ARM, reported sender WOLF
/// (UID ANDROID-123), to aircraft 1 (HAWK): 1 arm command sent".
QString summaryLine(const RemoteCommand &c);

/// Command log schema version, written on every line.
inline constexpr int kCommandLogSchema = 1;

/// The command-specific fields of one command log line (kind "command").
QJsonObject logRecord(const RemoteCommand &c);

/// Serialize one log line (compact JSON, no newline) with the schema version
/// and the SHA-256 of the previous line (empty for the first line).
QByteArray chainRecord(QJsonObject record, const QString &prevHash);

/// Lowercase hex SHA-256 of one log line as written (without its newline).
QString lineHash(const QByteArray &line);

/// One entry on the ATAK command senders list (settings key "approvedPilots"). Stored as "uid:<value>" or
/// "callsign:<value>"; an entry saved without a prefix (older builds) matches
/// either. Both values are what the sender reports about itself in the
/// marker; the station does not verify them. A UID entry matches the reported
/// device UID only, a callsign entry the reported callsign only.
enum class PilotKind { Any, Uid, Callsign };
struct PilotEntry {
    PilotKind kind = PilotKind::Any;
    QString   value;
};
PilotEntry parsePilotEntry(const QString &stored);
QString    formatPilotEntry(PilotKind kind, const QString &value);   ///< the stored form
QString    pilotKindName(PilotKind kind);                            ///< "uid", "callsign", "any"
PilotKind  pilotKindFromName(const QString &name);
/// True when the reported sender matches an entry of `entries` (stored form).
bool       senderOnList(const QStringList &entries, const QString &callsign, const QString &uid);

/// SHA-256 fingerprint of a DER certificate as "AB:CD:...".
QString sha256Fingerprint(const QByteArray &der);
/// Same fingerprint, ignoring case, colons and spaces. Empty never matches.
bool    fingerprintsMatch(const QString &a, const QString &b);

/// Whether enrollment may send the TAK username and password to a server
/// whose certificate has fingerprint `fingerprint`. Only over a verified
/// connection: with Verify on, a chain that verified (`chainVerified`)
/// against the system trust store plus any imported CA, the same trust as
/// the CoT stream; otherwise (Verify off, self-signed or unverified chain)
/// only when the fingerprint is the one the operator accepted (`pinned`).
bool    enrollMaySend(bool verifyPeer, bool chainVerified,
                      const QString &fingerprint, const QString &pinned);

/// The TAK page warning while C2 is on and armed with "Send commands only
/// from listed senders" off, or an empty string.
QString openSenderWarning(bool c2On, bool c2Armed, bool approvedOnly);

/// The result shown for a marker whose reported sender is not on the list.
/// Starts with "Refused:" so it reads apart from a not-sent result.
QString notOnListResult();

/// The result shown for a marker whose reported sender claims the station's
/// test identity. Starts with "Refused:".
QString testIdentityResult();

/// Result for a test command the station could not read, e.g.
/// "Not sent: command not understood (missing track id)". Nothing is sent.
QString parseFailedResult(const QString &reason);

/// The sender identity the station's test command carries, so the ATAK
/// command senders list treats it like any other sender: refused while the list
/// is on unless this identity is listed. A marker from the network that
/// claims it is not the station's test and is refused.
QString testSenderCallsign();   ///< "STATION-TEST"
QString testSenderUid();        ///< "rotorlab-station-test"
bool    claimsTestSender(const QString &callsign, const QString &uid);

/// The warning line for a commanded altitude above the station's warning
/// level, or an empty string. `commandedFt` < 0 means the command names no
/// altitude; `warnAboveFt` <= 0 means no warning level is set. Both are feet
/// above the launch point. Informs only: the command is sent either way.
QString altitudeWarning(int commandedFt, double warnAboveFt);

/// The altitude warning a received-command row carries: altitudeWarning()
/// when the command was sent to an aircraft, empty when nothing was sent.
QString sentAltitudeWarning(bool sent, int commandedFt, double warnAboveFt);

} // namespace TakPolicy
