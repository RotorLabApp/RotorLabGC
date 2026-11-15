/****************************************************************************
 *
 * RotorLab GC - guided-action dispatch, in SI.
 *
 ****************************************************************************/

#include "GuidedDispatch.h"

#include <cmath>

#include <QtCore/QHash>

#include "Fact.h"
#include "FactGroup.h"
#include "Vehicle.h"

namespace {

double factOr(FactGroup *fg, const char *name, double fallback)
{
    if (!fg) {
        return fallback;
    }
    if (Fact *f = fg->getFact(QLatin1String(name))) {
        return f->rawValue().toDouble();
    }
    return fallback;
}

/// Ellipsoid-minus-MSL per system id, fed from GPS_RAW_INT.
QHash<int, double> &_separationBySysid()
{
    static QHash<int, double> map;
    return map;
}

} // namespace

namespace Guided {

Verb verbFromName(const QString &name)
{
    const QString v = name.trimmed().toUpper();
    if (v == QLatin1String("GOTO"))    return Verb::Goto;
    if (v == QLatin1String("TKOFF"))   return Verb::Takeoff;
    if (v == QLatin1String("RTL"))     return Verb::Rtl;
    if (v == QLatin1String("LAND"))    return Verb::Land;
    if (v == QLatin1String("HOLD"))    return Verb::Hold;
    if (v == QLatin1String("GUIDED"))  return Verb::Guided;
    if (v == QLatin1String("ARM"))     return Verb::Arm;
    if (v == QLatin1String("DISARM"))  return Verb::Disarm;
    if (v == QLatin1String("ALTHOLD")) return Verb::AltHold;
    return Verb::Invalid;
}

void noteGpsAltitudes(int sysid, double mslM, double ellipsoidM)
{
    if (std::isnan(mslM) || std::isnan(ellipsoidM)) {
        return;
    }
    _separationBySysid()[sysid] = ellipsoidM - mslM;
}

void forgetGpsAltitudes(int sysid)
{
    _separationBySysid().remove(sysid);
}

double geoidSeparationM(const Vehicle *veh)
{
    // The GPS reports both altitudes, so the separation at the aircraft's own
    // position comes free and needs no geoid model on disk. QGC ships no EGM96
    // datum, and assuming zero is exactly the error the Fleet Server document
    // warns about - up to ~100 m, in a direction that varies with where you
    // are, and invisible until somebody compares an ATAK readout with an
    // altimeter.
    if (!veh) {
        return std::nan("");
    }
    const auto &map = _separationBySysid();
    const auto it = map.constFind(veh->id());
    return (it == map.constEnd()) ? std::nan("") : it.value();
}

double currentHaeM(const Vehicle *veh)
{
    if (!veh) {
        return std::nan("");
    }
    const double sep = geoidSeparationM(veh);
    if (std::isnan(sep)) {
        return std::nan("");
    }
    const double amsl = factOr(const_cast<Vehicle *>(veh)->vehicleFactGroup(),
                               "altitudeAMSL", std::nan(""));
    if (std::isnan(amsl)) {
        return std::nan("");
    }
    return amsl + sep;
}

Result dispatch(Vehicle *veh, Verb verb, double lat, double lon,
                Alt alt, double speedMs)
{
    if (!veh) {
        return {false, QStringLiteral("no such aircraft")};
    }

    // The server checked the customer's policy. We know the aircraft, and its
    // state may have changed since the marker was placed.
    if (!veh->armed() && verb != Verb::Arm && verb != Verb::Takeoff && verb != Verb::Guided) {
        return {false, QStringLiteral("the aircraft is not armed")};
    }

    // Resolve the target to AMSL once, here, because every autopilot frame
    // below wants AMSL. An AMSL caller costs nothing; only an HAE one needs the
    // separation, and if it is missing we refuse rather than invent it - a
    // silently wrong altitude is worse than a refusal an operator can see.
    double targetAmslM = std::nan("");
    if (alt.isSet()) {
        if (alt.datum == Alt::Datum::Amsl) {
            targetAmslM = alt.value;
        } else {
            const double sep = geoidSeparationM(veh);
            if (std::isnan(sep)) {
                return {false, QStringLiteral("no ellipsoid reference from GPS; cannot set that altitude")};
            }
            targetAmslM = alt.value - sep;
        }
    }

    const bool haveAlt = !std::isnan(targetAmslM);
    const bool haveSpd = !std::isnan(speedMs);

    switch (verb) {
    case Verb::Goto: {
        if (std::isnan(lat) || std::isnan(lon)) {
            return {false, QStringLiteral("GOTO needs a position")};
        }
        if (haveSpd && speedMs > 0.0) {
            veh->guidedModeChangeGroundSpeedMetersSecond(speedMs);
        }
        // Position AND altitude in ONE DO_REPOSITION. Splitting it into QGC's
        // goto-location then change-altitude makes the aircraft climb in
        // place: the first repositions at the current AMSL, and the second
        // re-repositions to the current lat/lon.
        //
        // DO_REPOSITION with MAV_FRAME_GLOBAL wants AMSL, which is what the
        // target was already resolved to. With no altitude in the order, hold
        // the one the aircraft is at.
        const double curAMSL = factOr(veh->vehicleFactGroup(), "altitudeAMSL", 0.0);
        const float targetAMSL = static_cast<float>(haveAlt ? targetAmslM : curAMSL);
        veh->sendMavCommandInt(veh->defaultComponentId(), MAV_CMD_DO_REPOSITION,
                               MAV_FRAME_GLOBAL, /*showError*/ false,
                               -1.0f, static_cast<float>(MAV_DO_REPOSITION_FLAGS_CHANGE_MODE),
                               0.0f, NAN, lat, lon, targetAMSL);
        return {true, QStringLiteral("sent to autopilot")};
    }
    case Verb::AltHold: {
        if (!haveAlt) {
            return {false, QStringLiteral("an altitude is required")};
        }
        // guidedModeChangeAltitude takes a CHANGE relative to where it is now.
        const double curAMSL = factOr(veh->vehicleFactGroup(), "altitudeAMSL", 0.0);
        veh->guidedModeChangeAltitude(targetAmslM - curAMSL, true);  // pause, then change
        return {true, QStringLiteral("altitude change sent")};
    }
    case Verb::Hold:
        veh->pauseVehicle();
        return {true, QStringLiteral("holding")};
    case Verb::Rtl:
        veh->guidedModeRTL(false);
        return {true, QStringLiteral("returning to launch")};
    case Verb::Land:
        veh->guidedModeLand();
        return {true, QStringLiteral("landing")};
    case Verb::Guided:
        veh->setGuidedMode(true);
        return {true, QStringLiteral("guided mode set")};
    case Verb::Arm:
        if (veh->armed()) {
            return {true, QStringLiteral("already armed")};
        }
        veh->setArmed(true, true);
        return {true, QStringLiteral("arm sent")};
    case Verb::Disarm:
        // Refusing to disarm an airborne aircraft is the connector doing its
        // job, not blocking a flight.
        if (veh->flying()) {
            return {false, QStringLiteral("the aircraft is flying")};
        }
        veh->setArmed(false, true);
        return {true, QStringLiteral("disarm sent")};
    case Verb::Takeoff: {
        double climbToM = veh->minimumTakeoffAltitudeMeters();
        if (haveAlt) {
            // guidedModeTakeoff wants height above the LAUNCH point, and on the
            // ground the aircraft is still at it.
            const double curAMSL = factOr(veh->vehicleFactGroup(), "altitudeAMSL", 0.0);
            const double curRel  = factOr(veh->vehicleFactGroup(), "altitudeRelative", 0.0);
            climbToM = targetAmslM - (curAMSL - curRel);
        }
        if (climbToM <= 0.0) {
            climbToM = veh->minimumTakeoffAltitudeMeters();
        }
        veh->guidedModeTakeoff(climbToM);
        return {true, QStringLiteral("takeoff sent")};
    }
    case Verb::Invalid:
        break;
    }
    return {false, QStringLiteral("unsupported verb")};
}

} // namespace Guided
