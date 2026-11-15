/****************************************************************************
 *
 * RotorLab GC - guided-action dispatch, in SI.
 *
 * The implementation of "carry out this order on this aircraft". The local
 * ATAK marker path is its only caller; the Fleet Connector accepts no orders.
 *
 *   TakManager     parses an ATAK marker's text itself, where the grammar is
 *                  `ALT <ft>` and `SPD <1-10>` - feet and a notch.
 *   The Fleet Connector no longer accepts server-originated aircraft orders.
 *
 * The two callers genuinely disagree about units, which is exactly why the
 * conversion belongs at each boundary and NOT in here. This function speaks
 * SI and nothing else: meters and meters per second.
 *
 * The altitude datum is not a units problem to convert away. An ATAK marker's
 * `ALT 400` means 400 ft above the launch point, and always has. Forcing that
 * route to express itself in HAE would make it need a geoid separation it never
 * needed, so an aircraft whose GPS reports no ellipsoid altitude would start
 * refusing orders it has always accepted. The datum travels WITH the altitude,
 * and only a caller that genuinely holds an HAE number pays for conversion.
 *
 * Keeping the conversion here means a caller cannot accidentally confuse
 * meters, feet, and altitude datums in code that dispatches commands.
 *
 ****************************************************************************/

#pragma once

#include <cmath>

#include <QtCore/QString>

class Vehicle;

namespace Guided {

/// What a dispatch attempt did, in words an ATAK operator will read.
struct Result {
    bool    ok = false;
    QString detail;
};

enum class Verb {
    Invalid, Goto, Takeoff, Rtl, Land, Hold, Guided, Arm, Disarm, AltHold,
};

/// A target altitude together with the datum it is measured from, so a caller
/// cannot hand over a bare number and hope the other end agrees.
struct Alt {
    enum class Datum { None, Hae, Amsl };

    Datum  datum = Datum::None;
    double value = 0.0;         ///< meters, in `datum`

    /// No altitude in the order: hold whatever the aircraft is at.
    static Alt none()             { return {}; }
    /// Meters above the WGS84 ellipsoid for a caller with an HAE target.
    static Alt hae(double m)      { return { Datum::Hae,  m }; }
    /// Meters above mean sea level (what the autopilot's frames want).
    static Alt amsl(double m)     { return { Datum::Amsl, m }; }

    bool isSet() const { return datum != Datum::None && !std::isnan(value); }
};

/// Parse a canonical verb name ("GOTO", "RTL", ...). Aliases are the callers'
/// business; this takes the canonical form both of them already produce.
Verb verbFromName(const QString &name);

/// Carry `verb` out on `veh`.
///
/// `alt` carries its own datum; use `Alt::none()` to leave altitude alone.
/// `speedMs` is m/s; pass NaN to leave the speed alone. `lat`/`lon` are degrees
/// and are only read for Goto.
///
/// Sanity checks against the aircraft's CURRENT state live here, so both
/// routes refuse the same things for the same reasons. Refusing is always
/// allowed and is never a failure of the connector.
Result dispatch(Vehicle *veh, Verb verb, double lat, double lon,
                Alt alt, double speedMs);

/// Record the two altitudes a GPS reports for one system, in meters. Called
/// from the MAVLink GPS_RAW_INT hook. QGC's GPS fact group carries no
/// altitude, and QGC ships no geoid datum, so the receiver's own pair is the
/// only honest source for the separation.
void noteGpsAltitudes(int sysid, double mslM, double ellipsoidM);

/// Forget a system's cached altitudes (it disconnected).
void forgetGpsAltitudes(int sysid);

/// Height of the WGS84 ellipsoid above MSL at the vehicle, in meters, from the
/// GPS's own two altitudes. Add it to an MSL altitude to get HAE. Returns NaN
/// when the GPS has not reported an ellipsoid altitude - many do not.
double geoidSeparationM(const Vehicle *veh);

/// The vehicle's current altitude in meters HAE, or NaN when it cannot be
/// determined without guessing.
double currentHaeM(const Vehicle *veh);

} // namespace Guided
