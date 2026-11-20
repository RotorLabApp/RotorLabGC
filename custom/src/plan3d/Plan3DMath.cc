/****************************************************************************
 *
 * RotorLab GC Plan view 3D (G64, MG2) - placement and clearance math.
 *
 ****************************************************************************/

#include "Plan3DMath.h"

#include <QtCore/QtMath>

namespace Plan3D {

namespace {
constexpr double kEarthRadiusM = 6378137.0;   // WGS84 equatorial radius
}

Datum datumForFrame(int frame)
{
    switch (frame) {
    case 0:     // MAV_FRAME_GLOBAL
    case 5:     // MAV_FRAME_GLOBAL_INT
        return Datum::Amsl;
    case 3:     // MAV_FRAME_GLOBAL_RELATIVE_ALT
    case 6:     // MAV_FRAME_GLOBAL_RELATIVE_ALT_INT
        return Datum::Relative;
    case 10:    // MAV_FRAME_GLOBAL_TERRAIN_ALT
    case 11:    // MAV_FRAME_GLOBAL_TERRAIN_ALT_INT
        return Datum::Terrain;
    default:
        return Datum::None;
    }
}

Height placeHeight(int frame, double param7, double homeAmsl, double groundAmsl)
{
    Height h;
    h.datum = datumForFrame(frame);
    switch (h.datum) {
    case Datum::Amsl:
        h.amsl = std::isfinite(param7) ? param7 : kNaN;
        break;
    case Datum::Relative:
        h.amsl = (std::isfinite(param7) && std::isfinite(homeAmsl)) ? homeAmsl + param7 : kNaN;
        break;
    case Datum::Terrain:
        h.terrainHag = std::isfinite(param7) ? param7 : kNaN;
        h.amsl = (std::isfinite(param7) && std::isfinite(groundAmsl)) ? groundAmsl + param7 : kNaN;
        break;
    case Datum::None:
        break;
    }
    return h;
}

Clearance classify(double sampleAmsl, double groundAmsl)
{
    if (!std::isfinite(sampleAmsl) || !std::isfinite(groundAmsl)) {
        return Clearance::Unknown;
    }
    return (sampleAmsl - groundAmsl) < kStrikeMarginM ? Clearance::Strike : Clearance::NoStrike;
}

Clearance classifyContact(double sampleAmsl, double groundAmsl)
{
    if (!std::isfinite(sampleAmsl) || !std::isfinite(groundAmsl)) {
        return Clearance::Unknown;
    }
    const double hag = sampleAmsl - groundAmsl;
    if (hag >= kStrikeMarginM) {
        return Clearance::NoStrike;
    }
    return hag >= -kContactBelowGroundM ? Clearance::GroundContact : Clearance::Strike;
}

Clearance classifyLegSample(double sampleAmsl, double groundAmsl, bool inContactSpan)
{
    return inContactSpan ? classifyContact(sampleAmsl, groundAmsl) : classify(sampleAmsl, groundAmsl);
}

QList<bool> contactSpan(const QList<double> &hag, bool takeoff, bool landing)
{
    const int n = int(hag.size());
    QList<bool> span(n, false);
    auto ends = [](double h) { return !std::isfinite(h) || h >= kStrikeMarginM; };
    if (takeoff) {
        for (int i = 0; i < n && !ends(hag[i]); i++) {
            span[i] = true;
        }
    }
    if (landing) {
        for (int i = n - 1; i >= 0 && !ends(hag[i]); i--) {
            span[i] = true;
        }
    }
    return span;
}

int defaultAltitudeModeForFrame(int frame)
{
    return frame == 3 ? 1 : 2;     // MissionItem::relativeAltitude() is frame 3 only
}

bool modeAgreesWithFrame(int altitudeMode, int frame)
{
    const Datum d = datumForFrame(frame);
    switch (altitudeMode) {
    case 1: return d == Datum::Relative;
    case 2:
    case 3: return d == Datum::Amsl;
    case 4: return d == Datum::Terrain;
    default: return false;
    }
}

Vec3 lerp(const Vec3 &a, const Vec3 &b, double t)
{
    return a + (b - a) * t;
}

Vec3 catmullRomLegPoint(const Vec3 *pts, int count, int leg, double t, double tension)
{
    if (count < 2 || leg < 0 || leg >= count - 1) {
        return {};
    }
    const Vec3 &p1 = pts[leg];
    const Vec3 &p2 = pts[leg + 1];
    if (count < 3) {
        return lerp(p1, p2, t);
    }
    const Vec3 p0 = leg > 0 ? pts[leg - 1] : (p1 - p2) + p1;
    const Vec3 p3 = leg + 2 < count ? pts[leg + 2] : (p2 - pts[leg]) + p2;

    // Cubic Hermite with tangents tension * (p[i+1] - p[i-1]).
    auto axis = [t, tension](double x0, double x1, double x2, double x3) {
        const double t0 = tension * (x2 - x0);
        const double t1 = tension * (x3 - x1);
        const double c0 = x1;
        const double c1 = t0;
        const double c2 = -3 * x1 + 3 * x2 - 2 * t0 - t1;
        const double c3 = 2 * x1 - 2 * x2 + t0 + t1;
        return c0 + c1 * t + c2 * t * t + c3 * t * t * t;
    };
    return {axis(p0.x, p1.x, p2.x, p3.x), axis(p0.y, p1.y, p2.y, p3.y), axis(p0.z, p1.z, p2.z, p3.z)};
}

double LocalFrame::east(double lon) const
{
    return qDegreesToRadians(lon - lon0) * kEarthRadiusM * std::cos(qDegreesToRadians(lat0));
}

double LocalFrame::north(double lat) const
{
    return qDegreesToRadians(lat - lat0) * kEarthRadiusM;
}

double LocalFrame::lonFromEast(double e) const
{
    return lon0 + qRadiansToDegrees(e / (kEarthRadiusM * std::cos(qDegreesToRadians(lat0))));
}

double LocalFrame::latFromNorth(double n) const
{
    return lat0 + qRadiansToDegrees(n / kEarthRadiusM);
}

} // namespace Plan3D
