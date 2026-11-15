/****************************************************************************
 *
 * RotorLab GC RotorLab Connector - filing rules.
 *
 * Small, pure functions that decide what goes into a flight body and what the
 * station says about a reply. Kept free of the connector and the spool so they
 * can be unit tested without a server, a vehicle or a settings file.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QDate>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

namespace RotorLabFiling {

/// The server's published `flight_type` list, in the order the picker shows
/// it (the `flight_type` enum on POST /api/v1/flights in RotorLab's API).
QStringList flightTypeCodes();

/// The human label for a code, e.g. "Public safety" for public_safety.
QString flightTypeLabel(const QString &code);

/// Longest `flight_type_other` the server takes.
constexpr int kFlightTypeOtherMax = 80;

/// Sent with `other` when the crew picked other and typed nothing: the server
/// requires the text with other, and a flight is never parked for this.
QString flightTypeOtherFallback();

/// One line, control characters stripped, trimmed, at most 80 characters.
QString cleanFlightTypeOther(const QString &text);

struct FlightType {
    QString type;       ///< a published code, or empty for "not set"
    QString other;      ///< only ever non-empty when type is "other"
};

/// Map whatever was stored or typed onto the published list. Legacy words
/// from earlier GC builds: mapping -> survey; patrol, search, delivery and
/// any other unknown word -> other, with the word as `flight_type_other`.
/// `forSending` fills the empty other text with flightTypeOtherFallback().
FlightType normalizeFlightType(const QString &type, const QString &other,
                               bool forSending);

/// True when a parked flight goes back in the queue: RotorLab refused it with
/// a 400 (or the record, saved by an earlier build, carries no status: 0) AND
/// normalizeFlightBody() just changed its flight_type or flight_type_other.
/// The change is the evidence; RotorLab's sentence is never matched, because
/// its wording ("What for must be one of ...", "Say what the flight was for:
/// Other needs a description") is not a contract. A 400 for any other reason
/// leaves the type unchanged, so that flight stays parked.
bool requeueAfterTypeFix(int lastStatus, bool flightTypeChanged);

/// Rewrite a stored flight body so its flight type is one the server takes.
/// Returns true when the body changed. external_ref and everything else are
/// left exactly as they were.
bool normalizeFlightBody(QJsonObject &body);

/// Longest server sentence GC shows or logs, in code points. Anything longer
/// is cut there and ends with an ellipsis.
constexpr int kServerTextMax = 500;

/// A sentence from the server, trimmed and clipped to kServerTextMax code
/// points (a surrogate pair is never split).
QString clipServerText(const QString &text);

/// True when `version` (from GET /api/v1/version, e.g. "3.5.2" or
/// "v3.5.2+abc") is at least major.minor.patch. False when it cannot be read.
bool versionAtLeast(const QString &version, int major, int minor, int patch);

/// RotorLab 3.5.2 (A54) records a flight with no pilot named as "Pilot not
/// named", lists it for naming, and answers with pilot_named and pilot_note.
/// Older servers do none of that. False when the version is unknown.
bool serverNamesPilots(const QString &version);

/// GC's own sentence for a flight that filed with no pilot named, on a
/// server that lists such flights for naming (serverNamesPilots()).
QString noPilotFallback();

/// GC's own sentence for the same flight on an older or unknown server. It
/// says only what this station did, nothing about how RotorLab recorded it.
QString noPilotNeutral();

/// What to tell the operator about the pilot after a 200 on POST
/// /api/v1/flights, or empty when there is nothing to say.
/// With `namesPilots` (server 3.5.2 or later): `pilot_note` is shown
/// verbatim (clipped) when present, and with no `pilot_named` in the reply
/// the fallback is used only when the station sent no pilot_id.
/// Without it, pilot_named and pilot_note are ignored and the neutral
/// sentence is used when the station sent no pilot_id. Nothing here claims
/// how the server credited the flight.
QString pilotNotice(const QJsonObject &reply, bool sentPilotId, bool namesPilots);

/// Which flight a notice is about: local takeoff time ("14:05", or
/// "12 Nov 14:05" when not `today`) and the aircraft, e.g. "14:05, Alpha 2".
/// Empty when neither is known. `takeoffIso` is the body's takeoff_at.
QString flightNoticePrefix(const QString &takeoffIso, const QString &aircraft,
                           const QDate &today);

/// Most packs on one flight, and the longest serial. Packs are not sent to
/// RotorLab until a server takes them (A55); these limits are kept for then.
constexpr int kPackSerialsMax = 12;
constexpr int kPackSerialMax  = 64;

/// The crew screen's "Packs" field, typed or scanned: serials separated by
/// commas or whitespace. Each is trimmed and stripped of control characters;
/// empties, duplicates and serials over 64 characters are dropped; at most 12
/// are kept, first come first kept.
QStringList parsePackSerials(const QString &text);

/// The largest flight log RotorLab stores for one file: 512 MB
/// (RotorLab's limit; POST /api/v1/flights/{id}/log answers
/// 413 above it). RotorLab does not publish it on /api/v1 today, so this is
/// the default; maxLogBytesFrom() takes a published value when one appears.
constexpr qint64 kMaxLogBytes = qint64(512) * 1024 * 1024;

/// `max_file_bytes` from a GET /api/v1/usage reply when it is a positive
/// number, else kMaxLogBytes.
qint64 maxLogBytesFrom(const QJsonObject &usageReply);

/// Whole megabytes, rounded up, for a sentence ("600 MB").
QString megabytesText(qint64 bytes);

/// The spool card's sentence for a log over the limit, naming the file so
/// the crew can attach it in RotorLab by hand.
QString logTooLargeText(qint64 bytes, qint64 maxBytes, const QString &filePath);

/// The body to send when the operator presses Try again on a parked flight.
/// `stored` is the body as first filed; `fresh` is the body built now from
/// the station's current settings. Try again only FILLS what the stored
/// flight lacks: a pilot_id when none was stored, an asset_id when only an
/// asset_name (or nothing) was stored, an asset_name when nothing was
/// stored, and a site, flight type, crew or compliance field that is missing
/// or empty. A value the stored flight already has is never overwritten, so
/// yesterday's flight never takes today's pilot or aircraft. external_ref,
/// the times, the measured values and the remarks are always the stored ones.
QJsonObject retryFlightBody(const QJsonObject &stored, const QJsonObject &fresh);

} // namespace RotorLabFiling
