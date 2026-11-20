/****************************************************************************
 *
 * RotorLab GC Plan view 3D (G64, MG2) - placement and clearance math.
 *
 * Pure functions, no Qt Quick: how a mission item's frame and params[6]
 * become a height in AMSL (PLAN-CONTRACT.md section 2), how a drawn sample is
 * classed against the loaded ground (section 4), and the spline construction
 * shared with RotorLab's planner (section 3, S-5). Unit tested against the
 * plan corpus in test/Custom/Plan3DTest.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QList>
#include <QtCore/QtGlobal>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace Plan3D {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// Contract section 4: a sample with less than this height above known
/// ground is a strike (CLEAR_M in RotorLab's planner, decision 756 OQ7).
constexpr double kStrikeMarginM = 2.0;

/// Contract section 4: at least 24 samples per drawn leg. S-5: 24 per spline leg.
constexpr int kSamplesPerLeg = 24;

/// Contract rev 4, CL-1: neighboring samples are never more than S apart
/// along the ground, S = half the loaded ground grid cell, at most 15 m.
constexpr double kMaxSampleSpacingM = 15.0;

/// Drawing limits, contract revision 9 (G67 M1; the same values
/// in GC and RotorLab). A .plan can come from anyone (the Mission Library
/// download included), so nothing in it may make the scene build without
/// bound on the UI thread. None of the 17 corpus plans comes near these.
/// Past a limit, the part is drawn violet (unknown, CL-2) or not drawn,
/// never in the no strike color, and the view says so. Every limit is here,
/// so a revision is one edit.
///  - Plan extent: an item more than 200 km from home, and every leg into or
///    out of it, is drawn violet; the ground there is never loaded or used.
constexpr double kMaxPlanExtentM = 200000.0;
///  - Leg backstop: a leg longer than twice the extent is one violet line.
constexpr double kMaxLegM = 2.0 * kMaxPlanExtentM;
///  - A loiter radius over 10 km is not drawn.
constexpr double kMaxLoiterRadiusM = 10000.0;
///  - A loiter circle that would need more points than this to keep samples
///    S apart is drawn violet. 4096 points keep 15 m spacing up to a 9.7 km radius.
constexpr int kMaxLoiterRingPoints = 4096;
///  - Code cap: a leg that would need more samples than this is one violet line.
constexpr int kMaxSamplesPerLeg = 100000;
///  - Drawn samples in one scene; every drawn line segment (leg, ring, stem,
///    fence, outline) counts as one. Past it, nothing more is drawn. At the
///    limit the GPU buffers are about 62 MB (4 vertices of 56 bytes and 24
///    bytes of index per segment), well inside qsizetype and quint32 indices.
constexpr qsizetype kMaxSceneSamples = 250000;
///  - Drawn heights: an AMSL height (or a terrain-frame height above ground)
///    beyond +-100 km is unknown, drawn at the limit, violet.
constexpr double kMaxDrawnHeightM = 100000.0;

/// S for a ground grid cell of `groundCellM` (NaN or 0: the 15 m cap).
inline double sampleSpacingM(double groundCellM)
{
    return (std::isfinite(groundCellM) && groundCellM > 0) ? std::min(kMaxSampleSpacingM, groundCellM / 2.0) : kMaxSampleSpacingM;
}

/// Contract rev 3, A-0b: a note when the plan's home altitude and the
/// ground at home differ by more than this.
constexpr double kHomeDatumNoteM = 3.0;

/// Ground AMSL in metres at (lat, lon), NaN when the ground is not known
/// (A-3: outside the loaded area, a failed or pending tile, a refused request).
using GroundFn = std::function<double(double lat, double lon)>;

/// How an item's params[6] is read, decided by its MAVLink frame (A-1: an item
/// is drawn by its frame and params[6]).
enum class Datum {
    None,       ///< frame carries no position height (MAV_FRAME_MISSION and others)
    Amsl,       ///< frame 0 or 5: params[6] is AMSL (mode 2, and mode 3 after GC's terrain math)
    Relative,   ///< frame 3 or 6: params[6] is metres above launch
    Terrain,    ///< frame 10 or 11: params[6] is metres above the autopilot's terrain
};

Datum datumForFrame(int frame);

/// Section 4 states of one drawn sample.
enum class Clearance {
    NoStrike,   ///< ground known and the sample is at least kStrikeMarginM above it
    Strike,     ///< ground known and the sample is less than kStrikeMarginM above it, or below it
    Unknown,    ///< the check could not be made (CL-2)
    GroundContact,  ///< CL-1a: in a takeoff or landing contact span, HAG -0.5 m to under M; never a strike
};

struct Height {
    Datum   datum       = Datum::None;
    double  amsl        = kNaN;     ///< AMSL that flies, NaN when it cannot be known (A-4, A-5, A-3 for terrain frame)
    double  terrainHag  = kNaN;     ///< terrain frame only: the nominal height above ground (params[6])
    bool    known() const { return std::isfinite(amsl); }
};

/// Contract section 2. `homeAmsl` is H (plannedHomePosition[2], NaN if
/// missing), `groundAmsl` is G(p) at the item (NaN if unknown).
///  - frame 0/5: AMSL = params[6]; NaN params[6] (mode 3 before terrain, A-5) is unknown.
///  - frame 3/6: AMSL = H + params[6]; unknown without H (A-4).
///  - frame 10/11: AMSL = G(p) + params[6] by this product's terrain (A-6); unknown without G(p).
Height placeHeight(int frame, double param7, double homeAmsl, double groundAmsl);

/// Section 4 for one sample at `sampleAmsl` (NaN when the height is unknown).
Clearance classify(double sampleAmsl, double groundAmsl);

/// Contract rev 5, CL-1a: inside the contact span of a takeoff or landing
/// leg, a sample down to this far below the ground is ground contact, not a
/// strike (the ground grid and the planned touchdown need not match exactly,
/// see the contract). Further below is red.
constexpr double kContactBelowGroundM = 0.5;

/// CL-1a for a sample inside a contact span: Unknown without a height or
/// ground; NoStrike at HAG of M or more; GroundContact at HAG of at least
/// -kContactBelowGroundM and less than M; Strike below that.
Clearance classifyContact(double sampleAmsl, double groundAmsl);

/// Section 4 for a sample on a drawn leg. `inContactSpan` is true only for
/// the samples of a takeoff or landing leg that CL-1a puts in the contact
/// span (Builder decides the span); every other sample is classify().
Clearance classifyLegSample(double sampleAmsl, double groundAmsl, bool inContactSpan);

/// CL-1a contact span of one leg, from the height above ground of each of its
/// samples in order (NaN when unknown). `takeoff`: from the start up to, not
/// including, the first sample whose HAG reaches M. `landing`: the samples
/// after the last sample whose HAG is at least M, up to the touchdown. An
/// unknown sample ends a span as a sample at M would, so a dip past it is
/// never in the span. Both flags may be set (a leg out of launch into a land).
QList<bool> contactSpan(const QList<double> &hag, bool takeoff, bool landing);

/// A-1, triple absent: GC reads frame 3 as mode 1, every other frame (6 included)
/// as mode 2 (SimpleMissionItem.cc:312-313, MissionItem.h:98).
int defaultAltitudeModeForFrame(int frame);

/// A-1: does the item's AltitudeMode (1 relative, 2 AMSL, 3 calculated above
/// terrain, 4 terrain frame) agree with its frame? Mode 1 needs frame 3,
/// modes 2 and 3 need frame 0, mode 4 needs frame 10 (the INT forms 6, 5 and
/// 11 count as the same frame). A mode outside 1 to 4 never agrees.
bool modeAgreesWithFrame(int altitudeMode, int frame);

struct Vec3 {
    double x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(const Vec3 &o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3 &o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
};

Vec3 lerp(const Vec3 &a, const Vec3 &b, double t);

/// S-5: Catmull-Rom with tension 0.5 through all route points, the same
/// construction RotorLab's planner draws (non-centripetal, uniform
/// parameter, ends extended by reflection). Returns the point at fraction
/// `t` (0..1) of the leg from pts[leg] to pts[leg + 1]. With fewer than three
/// points the leg is straight (S-7).
Vec3 catmullRomLegPoint(const Vec3 *pts, int count, int leg, double t, double tension = 0.5);

/// Local frame used by the scene: x east, y up, z south (Qt Quick 3D is
/// right handed with y up). Equirectangular around a reference point; good to
/// centimetres over the few kilometres a plan covers.
struct LocalFrame {
    double lat0 = 0, lon0 = 0, alt0 = 0;
    double east(double lon) const;
    double north(double lat) const;
    double lonFromEast(double e) const;
    double latFromNorth(double n) const;
    Vec3 toScene(double lat, double lon, double amsl) const { return {east(lon), amsl - alt0, -north(lat)}; }
};

} // namespace Plan3D
