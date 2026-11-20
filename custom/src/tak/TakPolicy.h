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
#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include "TakCommand.h"

class QDateTime;
namespace CoT { struct Marker; }

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

/// The aircraft a command names before it is resolved (T16): "aircraft
/// ROTOR-2, ROTOR-3", "all aircraft of ROTOR", "HAWK", "SEL (not accepted)",
/// or for a marker that names no station "aircraft 2, 3 (no station named)"
/// and "all aircraft (no station named)".
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

/// A GOTO point the station may send: finite, in range, and not exactly
/// 0, 0 (what a CoT event carries for an unknown position) (G39).
bool pointKnown(double lat, double lon);

/// Why a GOTO is refused before dispatch (G39).
QString gotoMultipleRefusal();      ///< the command names more than one aircraft
QString gotoUnknownPointRefusal();  ///< the point is unknown (pointKnown false)
/// Why TKOFF is not sent to an aircraft that is not armed while the ARM verb
/// is off (G49). TKOFF arms the aircraft first, so it is held to the ARM
/// switch.
QString takeoffNeedsArmRefusal();

/// What the list shows as the destination of a GOTO with an unknown point:
/// "unknown point at 400 ft above launch" or "unknown point at current altitude".
QString unknownPointText(int altFt);

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

/// Why an inbound CoT event may not carry a command, or an empty string
/// when it is a marker a person placed (G38). Commands are read only from a
/// marker that is not a device's own report (no <takv> or <__group>), was
/// placed by a person (how h-*), names the device that created it (a p-p
/// link to a uid other than its own) and is not stale at `nowUtc`. The
/// event type is not checked: ATAK's point dropper places a-* markers.
///
/// Also refused: a time more than 5 minutes ahead of `nowUtc`, a time more
/// than 24 hours before it, and a time before `notBeforeUtc` (when valid:
/// the start of this station's session, so a marker from before a restart
/// is not run again, 5.1.2 review S3).
/// A marker whose time is at or before `repeatFloorUtc` (when valid) is
/// refused too: it may have been dropped from the repeat list (E1).
QString markerNotCommandReason(const CoT::Marker &m, const QDateTime &nowUtc,
                               const QDateTime &notBeforeUtc = QDateTime(),
                               const QDateTime &repeatFloorUtc = QDateTime());

/// Why a marker dropped earlier this session is not run when delivered again (L1).
QString droppedEarlierReason();

/// The line for command markers not read because more than `perSecond` in
/// one second or `perMinute` in one minute arrived (5.1.2 review S1).
QString rateLimitLine(int dropped, int perSecond, int perMinute);

/// The line for "marker ignored" lines not shown because more than
/// `perSecond` arrived in one second (5.1.2 review R6).
QString ignoredLinesDroppedLine(int dropped, int perSecond);

/// True for the markerNotCommandReason() reasons that come from the marker
/// time (ahead of this station's clock, more than 24 hours old, placed
/// before this station started).
bool isMarkerTimeReason(const QString &reason);

/// Why a command marker is not run when this station holds too many markers
/// to check for repeats.
QString repeatListFullReason();

/// The TAK page notice while command markers are dropped, or an empty
/// string. Counts add up while drops continue and reset after a minute with
/// none (5.1.2 review R1, R3).
QString commandDropNotice(int rateDropped, int fullDropped, int timeDropped, int perSecond, int perMinute);

/// The fly-view line for the same drops (total of the three counts), or an
/// empty string when there are none (5.1.2 review L2).
QString flyDropNotice(int total);

/// Why a command marker is not read while the callsign prefix is empty (G50).
QString emptyPrefixReason();

/// The TAK page notice while the callsign prefix is empty (G50).
QString emptyPrefixNotice();

/// The station's line for a marker it did not run (G36), e.g.
/// "marker ignored: stale (\"LAUNCH 1\", UID ANDROID-1)".
QString markerIgnoredLine(const QString &reason, const QString &name, const QString &uid);

// ---------------------------------------------------------------------------
// Addressing (G47) and SEL (G48)
//
// A marker names the aircraft as this station publishes it to TAK: the
// callsign prefix and the system id (ROTOR-2, ROTOR-1,3, ROTOR-ALL), or the
// aircraft's Fleet callsign. This is a routing rule, so that a command meant
// for one station does not act at another. It is not authentication: the
// marker text is written by the sending device, like the reported sender.
// ---------------------------------------------------------------------------

/// Seconds a station, an aircraft name or a prefix conflict seen on the link
/// is remembered after it was last seen.
inline constexpr int kAddressSeenSecs = 120;

/// Why `prefix` cannot name this station, or an empty string when it can:
/// 2 to 12 letters and digits, starting with a letter, not a command word
/// (Tak::isReservedWord), and not ROTOR, the old shared default (G47).
QString prefixProblem(const QString &prefix);

/// True for ROTOR in any case: the prefix every station had by default in
/// earlier versions. An upgraded install saved with it is offered its own
/// default (defaultPrefixFor) instead, and it cannot be entered.
bool isSharedDefaultPrefix(const QString &prefix);
/// Why ROTOR cannot be a prefix (prefixProblem, logged on the T12 path).
QString sharedDefaultPrefixReason();

/// True when markers may name an aircraft by this Fleet callsign: letters,
/// digits, spaces and hyphens only; a letter in every word but the last; no
/// ALT, SPD, ALL, SEL or GCS word; at most Tak::kMaxNameWords words and
/// Tak::kMaxNameChars characters; and not read as <prefix>-<number>.
bool fleetCallsignNameable(const QString &callsign, const QString &prefix);

/// The station prefix in a station callsign "<prefix>-GCS" (normalized, upper
/// case), or an empty string when `callsign` is not one.
QString stationPrefixOf(const QString &callsign);

enum class Address {
    Own,            ///< for this station: `ids` / `all` say which aircraft
    OtherStation,   ///< a name another station seen on the link publishes
    Unaddressed,    ///< no station named (GOTO 2, RTL ALL)
    NoAircraft,     ///< this station named, but no aircraft (GOTO ROTOR)
    UnknownStation, ///< a name no station seen on the link publishes
    Sel,            ///< SEL, never run (G48)
    Mismatch,       ///< marker text and its send-to name different aircraft
    FleetConflict,  ///< a Fleet callsign another station also uses
};

struct AddressInput {
    QString             ownPrefix;
    QHash<QString, int> fleetNames;       ///< nameable Fleet callsign (normalized) -> sysid
    QSet<QString>       fleetConflicts;   ///< those seen on another station's aircraft
    QSet<QString>       otherStations;    ///< prefixes of other stations seen (normalized)
    QHash<QString, QString> otherAircraft; ///< another station's aircraft name -> its station prefix
};

struct AddressResult {
    Address     kind = Address::Unaddressed;
    bool        all = false;    ///< Own: every aircraft of this station
    QList<int>  ids;            ///< Own: the system ids named
    QString     name;           ///< the name as compared, e.g. "ROTOR-2" or "HAWK"
    QString     station;        ///< OtherStation: that station's prefix
};

/// Which station a parsed command is for.
AddressResult classifyAddress(const Tak::Command &cmd, const AddressInput &in);

/// The reason a command with this address is not run here (without
/// "Not sent: "), or an empty string for Own. T1 to T5, T10, T11, T18.
QString addressRefusal(const AddressResult &a, const Tak::Command &cmd, const QString &ownPrefix);

/// T5: the reason fragment for a name another station publishes.
QString otherStationReason(const QString &name, const QString &station, const QString &ownPrefix);
/// T6: an edit or repeat of a marker that named another station.
QString otherStationEarlierReason();
/// T7: why a command is not run while another station uses this prefix.
QString prefixConflictRefusal(const QString &prefix);
/// T8: the TAK page notice while another station uses this prefix.
QString prefixConflictNotice(const QString &prefix, const QString &lastSeenUtc);
/// T9: the fly-view line for the same.
QString prefixConflictFlyLine(const QString &prefix);
/// T12: the TAK page line when an entered prefix is not saved.
QString prefixEntryRefusal();
/// T12c: the TAK page line when ROTOR is entered.
QString sharedDefaultPrefixRefusal();
/// T13: the TAK page notice and the marker reason for a saved prefix that
/// cannot name this station.
QString prefixInvalidNotice(const QString &prefix);
QString prefixInvalidReason();
/// T14: logged and shown when the prefix changes.
QString prefixChangedLine(const QString &from, const QString &to);
/// T15: the Fleet row note for a callsign markers cannot name.
QString fleetNotNameableNote(const QString &callsign, const QString &prefix, int sysid);
/// T17: the TAK page help line for addressing.
QString addressingHelp(const QString &prefix);

/// The default prefix of a new install (security M1): "RL" and 4 characters
/// of the random station uid, so no two installs share one by default.
QString defaultPrefixFor(const QString &stationUid);
/// T12b: an entered prefix another station used on the link in the last
/// kAddressSeenSecs (security M4).
QString prefixSeenRefusal(const QString &prefix);
/// T19: the TAK page notice and the marker reason while the prefix has not
/// been set or confirmed on this install (security M1).
QString prefixNotSetNotice(const QString &prefix);
QString prefixNotSetReason();
/// T19c: logged when "Commands to aircraft on" is asked for before then.
QString prefixNotSetArmLine();
/// T14b: a marker placed before the prefix last changed.
QString prefixFloorReason();
/// T20: a command for this station whose aircraft is not connected. One
/// aircraft: "no aircraft ROTOR-3 is connected to this station"; several or
/// ALL: "none of the named aircraft is connected to this station".
QString noAircraftConnected(const QString &name, bool several);

} // namespace TakPolicy
