/****************************************************************************
 *
 * RotorLab GC TAK integration - Cursor on Target message build/parse.
 *
 * Building follows TAK conventions so OpenTAKServer/ATAK treat entities
 * correctly:
 *   - the GCS self-beacon is a self-describing EUD (contact + __group + takv +
 *     uid), whose uid matches the enrolled client cert (clientUid), so the
 *     server associates the TLS session with the right EUD;
 *   - each drone is an AIR TRACK (a-f-A-M-F-Q / a-f-A-M-H-Q) with track,
 *     precisionlocation and a <link relation="p-p"> back to the reporting GCS
 *     EUD (the association), and NO takv (a drone is not a TAK device);
 *   - either broadcast (no <marti>, server fans out to the group) or direct
 *     (<marti><dest>) delivery.
 *
 * Parsing recovers a command marker (named marker whose callsign carries a
 * verb, or a plain point addressed to one of our drones via <marti><dest>).
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QString>
#include <QtCore/QStringList>

namespace CoT {

// Server-routed endpoint (no direct socket). Present on an addressable entity
// so a marker can be "sent to" it (needed for the send-to-drone command path).
inline constexpr const char *kServerEndpoint = "*:-1:stcp";

/// Air CoT type from the MAVLink vehicle type so ATAK shows the right icon.
/// Fixed wing -> a-f-A-M-F-Q, rotary -> a-f-A-M-H-Q, else the default.
QString typeForVehicle(int mavType, const QString &fallback = QStringLiteral("a-f-A-M-F-Q"));

/// A publishable CoT entity (a GCS EUD or a drone track).
struct Entity {
    QString uid;
    QString callsign;
    QString cotType;
    double  lat = 0.0;
    double  lon = 0.0;
    double  hae = 0.0;              ///< height above ellipsoid (m)
    double  course = 0.0;          ///< deg
    double  speed = 0.0;           ///< m/s
    int     staleSeconds = 30;
    QString remark;

    /// false: no position is shared. The point carries the CoT "unknown"
    /// values (lat 0, lon 0, hae/ce/le 9999999), how is h-g-i-g-o and no
    /// <precisionlocation> or <track> is written. lat/lon/hae are ignored.
    bool    positionKnown = true;

    bool    hasTrack = true;       ///< emit <track course speed>
    bool    addressable = true;    ///< emit contact@endpoint (+ makes it send-to-able)
    bool    selfEud = false;       ///< emit <takv> (true only for the GCS self-beacon)

    QString parentUid;             ///< if set, emit <link relation="p-p"> to this EUD
    QString parentCallsign;
    QString parentType = QStringLiteral("a-f-G-U-C");

    QString videoUrl;              ///< if set, emit <__video><ConnectionEntry> so ATAK/OTS can play the feed
    QString videoAlias;            ///< display name for the video stream (defaults to callsign)

    QStringList dest;              ///< direct delivery to these EUD callsigns; empty = broadcast
};

/// The CoT value for an unknown height or error (hae, ce, le).
inline constexpr double kUnknownValue = 9999999.0;

/// Build a CoT <event> for an entity. Returns UTF-8 XML.
QByteArray build(const Entity &e);

/// A command marker recovered from an inbound CoT <event>.
struct Marker {
    bool    valid = false;
    double  lat = 0.0;
    double  lon = 0.0;
    QString callsign;              ///< detail/contact@callsign (may carry a verb)
    QString destCallsign;          ///< detail/marti/dest@callsign (send-to target)
    QString uid;                   ///< event@uid
    QString type;                  ///< event@type (a-u-G, b-m-p-s-m, a-f-G-U-C, ...)
    QString how;                   ///< event@how: h-* placed by a person, m-* machine-generated
    QDateTime time;                ///< event@time (UTC); invalid when missing or unreadable
    QDateTime stale;               ///< event@stale (UTC); invalid when missing or unreadable
    bool    isEud = false;         ///< carries <takv> or <__group>: a device's own report (PLI)
    QString senderUid;             ///< detail/link@uid of the placing EUD (relation p-p only)
    QString senderCallsign;        ///< detail/link@parent_callsign of the placing EUD (p-p only)
    bool    hasParentLink = false; ///< a <link relation="p-p"> was present
};

/// Parse one <event>...</event> chunk. Returns an invalid Marker on any error.
Marker parse(const QByteArray &xml);

} // namespace CoT
