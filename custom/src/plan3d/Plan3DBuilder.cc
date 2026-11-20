/****************************************************************************
 *
 * RotorLab GC Plan view 3D (G64, MG2) - scene content from a plan.
 *
 * Contract references are to PLAN-CONTRACT.md: section 2 (A-0 to A-11,
 * placement by frame and params[6]), section 3 (S-1 to S-7, splines) and
 * section 4 (CL-1 to CL-7, clearance colors; CL-1a ground contact, rev 5).
 *
 ****************************************************************************/

#include "Plan3DBuilder.h"

#include <QtCore/QCoreApplication>

#include <algorithm>

namespace Plan3D {

namespace {

QString tr(const char *s)
{
    return QCoreApplication::translate("Plan3D", s);
}

/// One point of the drawn route. The leg INTO a point carries the flags.
struct RoutePt {
    double  lat = kNaN, lon = kNaN;
    Height  h;
    double  ground = kNaN;
    Vec3    drawn;
    int     command = 0;
    int     visualIndex = -1;
    int     seq = -1;               ///< the item's sequence number, for warnings
    double  loiterRadius = kNaN;    ///< signed, positive clockwise; NaN when not a loiter
    bool    spline = false;         ///< the leg into this point is a spline (S-1)
    bool    unknownLeg = false;     ///< the leg into this point has no height in the file (A-10)
    bool    contactTakeoff = false; ///< CL-1a: the leg into this point is a takeoff (22, 84, or the climb from home)
    bool    contactLand = false;    ///< CL-1a: the leg into this point is a landing (21, 85, a landing pattern's final leg)
    int     markerIndex = -1;       ///< this point's marker in Scene::markers, -1 when none
    bool    noLegInto = false;      ///< no leg is drawn into this point (a loiter exit: the ring is the path)
    bool    far = false;            ///< rev 9: more than kMaxPlanExtentM from home; its legs are violet
    int     loiterExit = 0;         ///< rev 11 L-3 to L-5: a LoiterExit for a loiter drawn as flown, 0 otherwise
};

/// The color of a drawn segment between two samples: strike, then unknown,
/// then ground contact (CL-1a), then no strike.
Clearance worst(Clearance a, Clearance b)
{
    if (a == Clearance::Strike || b == Clearance::Strike) {
        return Clearance::Strike;
    }
    if (a == Clearance::Unknown || b == Clearance::Unknown) {
        return Clearance::Unknown;
    }
    if (a == Clearance::GroundContact || b == Clearance::GroundContact) {
        return Clearance::GroundContact;
    }
    return Clearance::NoStrike;
}

void count(Stats &st, Clearance c)
{
    switch (c) {
    case Clearance::Strike:         st.samplesStrike++;         break;
    case Clearance::GroundContact:  st.samplesGroundContact++;  break;   // CL-1a: never counted as a strike
    case Clearance::Unknown:        st.samplesUnknown++;        break;
    case Clearance::NoStrike:       st.samplesNoStrike++;       break;
    }
}

bool validLatLon(double lat, double lon)
{
    return std::isfinite(lat) && std::isfinite(lon) && !(lat == 0.0 && lon == 0.0)
        && lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0;
}

/// Great-circle distance in metres between two valid positions (haversine),
/// for the rev 9 plan extent: right across the antimeridian and the poles.
double groundDistanceM(double lat1, double lon1, double lat2, double lon2)
{
    constexpr double kR = 6371008.8;
    constexpr double kDeg = M_PI / 180.0;
    const double dLat = (lat2 - lat1) * kDeg, dLon = (lon2 - lon1) * kDeg;
    const double h = std::sin(dLat / 2) * std::sin(dLat / 2)
                   + std::cos(lat1 * kDeg) * std::cos(lat2 * kDeg) * std::sin(dLon / 2) * std::sin(dLon / 2);
    return 2.0 * kR * std::asin(std::sqrt(std::clamp(h, 0.0, 1.0)));
}

/// Rev 9 plan extent: true when (lat, lon) is more than kMaxPlanExtentM from
/// a valid planned home. Without a home there is no extent (the leg backstop
/// and the scene budget still bound the drawing).
bool beyondExtent(const SourcePlan &plan, double lat, double lon)
{
    return validLatLon(plan.homeLat, plan.homeLon) && validLatLon(lat, lon)
        && groundDistanceM(plan.homeLat, plan.homeLon, lat, lon) > kMaxPlanExtentM;
}

// ---- Rev 11, section 4b: loiter legs as flown (LOITER-EXIT-SOURCES.md) ----

/// L-2 (rev 11, settled): the ring drawn when the aircraft sets the radius
/// (ArduPilot 19, or a radius of 0 or 1) is the firmware's default loiter
/// radius, since the plan carries none:
/// - ArduPlane WP_LOITER_RAD, 60 m: ArduPlane/config.h:186-187
///   (LOITER_RADIUS_DEFAULT) and ArduPlane/Parameters.cpp:264, ArduPilot ad3f9dd6.
/// - PX4 NAV_LOITER_RAD, 80 m: src/modules/navigator/navigator_params.c:55,
///   PX4 85df8c22.
constexpr double kArduPlaneLoiterRadiusM = 60.0;
constexpr double kPx4LoiterRadiusM = 80.0;

double aircraftLoiterRadiusM(bool arduPilot)
{
    return arduPilot ? kArduPlaneLoiterRadiusM : kPx4LoiterRadiusM;
}

/// How the leg out of a loiter leaves the ring (L-3 to L-5).
enum LoiterExit {
    LoiterExitNone = 1,     ///< no outbound leg (17 never completes)
    LoiterExitTangent,      ///< from the tangent point T toward the next item
    LoiterExitCenter,       ///< the tracked line from the center C (PX4 without heading, param4 0)
};

/// L-7: rev 11 applies to aircraft that fly a loiter as a ring, fixed wing
/// and VTOL. A multirotor holds its loiters at the point and keeps the
/// earlier drawing. The PX4 18 refusal (L-5) applies to every vehicle.
bool loitersAsFlown(const SourcePlan &plan)
{
    return plan.loitersAsFlown && (!plan.multiRotor || plan.vtol);
}

/// L-2: the flown radius, signed (negative counter-clockwise). Sets
/// `aircraftRadius` when the aircraft's own radius applies.
double flownLoiterRadius(const SourceItem &it, bool arduPilot, bool &aircraftRadius)
{
    aircraftRadius = false;
    const double r = it.command == 31 ? it.params[1] : it.params[2];
    if ((arduPilot && it.command == 19) || !std::isfinite(r) || std::abs(r) < 1.5) {
        aircraftRadius = true;
        const double fw = aircraftLoiterRadiusM(arduPilot);
        return (std::isfinite(r) && r < 0) ? -fw : fw;
    }
    return r;
}

/// L-3 to L-5: the outbound rule of a loiter item.
LoiterExit loiterExit(const SourceItem &it, bool arduPilot)
{
    switch (it.command) {
    case 18:
        return arduPilot ? LoiterExitTangent : LoiterExitNone;     // PX4 refuses 18 (L-5)
    case 19:
    case 31: {
        if (arduPilot) {
            return LoiterExitTangent;   // L-3: ArduPlane always waits for the tangent heading
        }
        const double heading = it.command == 31 ? it.params[0] : it.params[1];
        if (std::isfinite(heading) && heading > 0) {
            return LoiterExitTangent;   // L-4: heading required
        }
        return (std::isfinite(it.params[3]) && it.params[3] > 0) ? LoiterExitTangent : LoiterExitCenter;
    }
    default:
        return LoiterExitNone;          // 17: L-5
    }
}

/// L-1: where the line from `from` toward the center `c` meets the ring of
/// radius `r` (scene east and north, metres). From inside the ring, the
/// nearest point of the ring.
QPair<double, double> loiterEntry(double fromE, double fromN, double cE, double cN, double r)
{
    const double dE = fromE - cE, dN = fromN - cN;
    const double d = std::hypot(dE, dN);
    if (!(d > 1e-6)) {
        return {cE + r, cN};
    }
    return {cE + r * dE / d, cN + r * dN / d};
}

/// L-3, L-4: the tangent point T on the ring of signed radius `rSigned`
/// (positive clockwise) around (cE, cN) toward (nE, nN). False when the
/// next item is on or inside the ring (then the leg starts at C).
bool loiterTangent(double cE, double cN, double rSigned, double nE, double nN, double &tE, double &tN)
{
    const double r = std::abs(rSigned);
    const double dx = nE - cE, dn = nN - cN;
    const double d = std::hypot(dx, dn);
    if (!(d > r + 0.5)) {
        return false;
    }
    const double theta = std::atan2(dn, dx);
    const double alpha = std::acos(r / d);
    // Clockwise leaves from the tangent point on the left of the line to the
    // next item: bearing b - acos(R/D), which is theta + alpha in east-north.
    const double phi = rSigned > 0 ? theta + alpha : theta - alpha;
    tE = cE + r * std::cos(phi);
    tN = cN + r * std::sin(phi);
    return true;
}

} // namespace

bool Builder::isRoutePoint(int command)
{
    switch (command) {
    case 16:    // NAV_WAYPOINT
    case 17:    // NAV_LOITER_UNLIM
    case 18:    // NAV_LOITER_TURNS
    case 19:    // NAV_LOITER_TIME
    case 21:    // NAV_LAND
    case 22:    // NAV_TAKEOFF
    case 31:    // NAV_LOITER_TO_ALT
    case 82:    // NAV_SPLINE_WAYPOINT
    case 84:    // NAV_VTOL_TAKEOFF
    case 85:    // NAV_VTOL_LAND
        return true;
    default:
        return false;
    }
}

double Builder::loiterRadius(const SourceItem &item)
{
    if (!isLoiter(item.command)) {
        return kNaN;
    }
    // NAV_LOITER_TO_ALT carries the radius in param 2; 17, 18 and 19 in param 3.
    const double r = item.command == 31 ? item.params[1] : item.params[2];
    return (std::isfinite(r) && std::abs(r) >= 0.5) ? r : kNaN;
}

GeoBounds Builder::bounds(const SourcePlan &plan, double marginM)
{
    GeoBounds b;
    auto add = [&b, &plan](double lat, double lon) {
        if (!validLatLon(lat, lon) || beyondExtent(plan, lat, lon)) {
            return;     // rev 9: nothing beyond the plan extent widens the ground
        }
        b.south = std::isfinite(b.south) ? std::min(b.south, lat) : lat;
        b.north = std::isfinite(b.north) ? std::max(b.north, lat) : lat;
        b.west  = std::isfinite(b.west)  ? std::min(b.west, lon)  : lon;
        b.east  = std::isfinite(b.east)  ? std::max(b.east, lon)  : lon;
    };
    add(plan.homeLat, plan.homeLon);
    for (const SourceItem &it : plan.items) {
        if (isRoutePoint(it.command) || isRoi(it.command)) {
            add(it.params[4], it.params[5]);
            const double r = std::abs(loiterRadius(it));
            // G67 M1: a radius over the drawing limit is not drawn, so it does not widen the ground either.
            if (std::isfinite(r) && r <= kMaxLoiterRadiusM && validLatLon(it.params[4], it.params[5])) {
                LocalFrame f{it.params[4], it.params[5], 0};
                add(f.latFromNorth(r), f.lonFromEast(r));
                add(f.latFromNorth(-r), f.lonFromEast(-r));
            }
        }
    }
    for (const SourceFence &f : plan.fences) {
        if (f.circle) {
            if (!validLatLon(f.centerLat, f.centerLon)) {
                continue;
            }
            LocalFrame lf{f.centerLat, f.centerLon, 0};
            add(lf.latFromNorth(f.radiusM), lf.lonFromEast(f.radiusM));
            add(lf.latFromNorth(-f.radiusM), lf.lonFromEast(-f.radiusM));
        } else {
            for (const auto &v : f.polygon) {
                add(v.first, v.second);
            }
        }
    }
    add(plan.breachReturn.lat, plan.breachReturn.lon);
    for (const SourcePoint &r : plan.rally) {
        add(r.lat, r.lon);
    }
    for (const SourceOutline &o : plan.outlines) {
        for (const auto &v : o.path) {
            add(v.first, v.second);
        }
    }
    if (!b.valid()) {
        return b;
    }
    LocalFrame f{(b.south + b.north) / 2, (b.west + b.east) / 2, 0};
    const double side = std::max(f.north(b.north) - f.north(b.south), f.east(b.east) - f.east(b.west));
    const double m = std::max(marginM, 0.15 * side);
    b.north = f.latFromNorth(f.north(b.north) + m);
    b.south = f.latFromNorth(f.north(b.south) - m);
    b.east  = f.lonFromEast(f.east(b.east) + m);
    b.west  = f.lonFromEast(f.east(b.west) - m);
    return b;
}

double Builder::gridCellM(const GeoBounds &area, int cols, int rows)
{
    if (!area.valid() || cols < 2 || rows < 2) {
        return kNaN;
    }
    LocalFrame f{(area.south + area.north) / 2, (area.west + area.east) / 2, 0};
    const double ns = (f.north(area.north) - f.north(area.south)) / (rows - 1);
    const double ew = (f.east(area.east) - f.east(area.west)) / (cols - 1);
    return std::min(ns, ew);
}

Scene Builder::build(const SourcePlan &plan, const GroundFn &groundFn, double groundCellM)
{
    Scene sc;
    // Rev 9: the ground beyond the plan extent is never used (unknown, A-3).
    auto G = [&groundFn, &plan](double lat, double lon) -> double {
        if (!groundFn || !std::isfinite(lat) || !std::isfinite(lon) || beyondExtent(plan, lat, lon)) {
            return kNaN;
        }
        return groundFn(lat, lon);
    };
    const bool homeValid = validLatLon(plan.homeLat, plan.homeLon);

    // G67 M1, contract rev 9: every line goes through push() or a room()
    // check, so one scene never holds more than kMaxSceneSamples drawn
    // samples. Past the budget nothing more is drawn and the view says so; a
    // part over a limit is never drawn in the no strike color.
    auto limit = [&sc](const QString &w) {
        sc.limitWarnings << w;
        sc.warnings << w;
    };
    auto room = [&](qsizetype k) {
        if (!sc.overSampleBudget && sc.lines.size() + k > kMaxSceneSamples) {
            sc.overSampleBudget = true;
            limit(tr("This plan is too large to draw in full in 3D (over %1 drawn samples). The rest of it is not drawn and not checked against the terrain.")
                      .arg(kMaxSceneSamples));
        }
        return !sc.overSampleBudget;
    };
    auto push = [&](const LineSeg &s) {
        if (room(1)) {
            sc.lines.append(s);
        }
    };
    const QString kmLimitHeight = QString::number(kMaxDrawnHeightM / 1000.0, 'g', 6);
    const QString kmLimitExtent = QString::number(kMaxPlanExtentM / 1000.0, 'g', 6);

    // A-0 (rev 3): every relative height is measured from R, the loaded
    // ground at the planned home when it is known (the same ground the legs
    // are checked against, CL-3), else the plan's H. A-4: unknown only when
    // both are missing.
    const double groundAtHome = homeValid ? G(plan.homeLat, plan.homeLon) : kNaN;
    double H = std::isfinite(groundAtHome) ? groundAtHome : plan.homeAmsl;   // R
    if (std::isfinite(H) && std::abs(H) > kMaxDrawnHeightM) {
        // Rev 9: a launch reference beyond the drawn height limit is unknown (A-4).
        limit(tr("The planned home altitude is beyond the %1 km drawing limit; heights above launch are unknown and not checked against the terrain.")
                  .arg(kmLimitHeight));
        H = kNaN;
    }
    sc.launchRef = H;
    sc.launchRefFromGround = std::isfinite(groundAtHome);
    const double Hdrawn = H;
    // Rev 4 CL-1: samples never more than S apart along the ground.
    const double S = sampleSpacingM(groundCellM);
    sc.sampleSpacingM = S;

    // Reference: the planned home, or the middle of the plan without one.
    if (homeValid) {
        sc.frame = LocalFrame{plan.homeLat, plan.homeLon, std::isfinite(Hdrawn) ? Hdrawn : 0.0};
    } else {
        const GeoBounds b = bounds(plan, 0);
        sc.frame = LocalFrame{b.valid() ? (b.south + b.north) / 2 : 0.0, b.valid() ? (b.west + b.east) / 2 : 0.0, std::isfinite(Hdrawn) ? Hdrawn : 0.0};
    }
    const LocalFrame &F = sc.frame;
    // Display only: where a point whose height is unknown is drawn from. It
    // is never used to class anything (A-3: nothing is computed as if the
    // ground were level with launch).
    const double drawFloor = std::isfinite(Hdrawn) ? Hdrawn : 0.0;

    if (!std::isfinite(H)) {
        sc.warnings << tr("The plan's home has no altitude and the ground at home is not loaded, so heights above launch are unknown.");
    } else if (std::isfinite(plan.homeAmsl) && std::isfinite(groundAtHome)
               && std::abs(plan.homeAmsl - groundAtHome) > kHomeDatumNoteM) {
        // A-0b: a note, never a check.
        sc.homeDatumNote = true;
        sc.warnings << tr("The plan's home altitude (%1 m) differs from the ground at home (%2 m) by %3 m and may be stale; heights above launch are drawn from the ground at home.")
                           .arg(plan.homeAmsl, 0, 'f', 1).arg(groundAtHome, 0, 'f', 1).arg(std::abs(plan.homeAmsl - groundAtHome), 0, 'f', 1);
    }

    auto addStem = [&sc, &F, drawFloor, &push](const Vec3 &top, double ground, int visualIndex) {
        LineSeg s;
        s.a = top;
        s.visualIndex = visualIndex;
        if (std::isfinite(ground)) {
            s.b = Vec3(top.x, ground - F.alt0, top.z);
            s.kind = LineKind::Stem;
        } else {
            // CL-4: no known ground to stand on. Dashed, down to launch level, display only.
            s.b = Vec3(top.x, drawFloor - F.alt0, top.z);
            s.kind = LineKind::StemUnknown;
            sc.stats.pointsUnknownGround++;
        }
        push(s);
    };

    // A point at `amsl` (NaN when unknown) placed in the scene; an unknown
    // height is drawn at launch level plus `fallbackAbove`, display only.
    // Rev 9: never drawn beyond +-kMaxDrawnHeightM.
    auto drawnAt = [&F, drawFloor](double lat, double lon, double amsl, double fallbackAbove) {
        const double at = std::isfinite(amsl) ? amsl : drawFloor + (std::isfinite(fallbackAbove) ? fallbackAbove : 0.0);
        return F.toScene(lat, lon, std::clamp(at, -kMaxDrawnHeightM, kMaxDrawnHeightM));
    };

    // Rev 9: a height beyond +-kMaxDrawnHeightM (AMSL, or above ground for
    // terrain frame) is unknown: drawn at the limit, violet, never classed.
    auto boundHeight = [&](Height &h, const QString &what) {
        const bool overHag = h.datum == Datum::Terrain && std::isfinite(h.terrainHag) && std::abs(h.terrainHag) > kMaxDrawnHeightM;
        const bool overAmsl = std::isfinite(h.amsl) && std::abs(h.amsl) > kMaxDrawnHeightM;
        if (overHag || overAmsl) {
            limit(tr("%1: its height is beyond the %2 km drawing limit; it is drawn at the limit, violet (unknown), and not checked against the terrain.")
                      .arg(what, kmLimitHeight));
            h.amsl = kNaN;
            if (overHag) {
                h.terrainHag = kNaN;
            }
        }
    };
    auto farPoint = [&](double lat, double lon, const QString &what) {
        if (!beyondExtent(plan, lat, lon)) {
            return false;
        }
        limit(tr("%1 is more than %2 km from home; it is drawn violet (unknown) and not checked against the terrain.")
                  .arg(what, kmLimitExtent));
        return true;
    };

    // ---- Home and the route ---------------------------------------------

    QList<RoutePt> route;
    RoutePt homePt;
    if (homeValid) {
        homePt.lat = plan.homeLat;
        homePt.lon = plan.homeLon;
        homePt.h.datum = Datum::Amsl;
        homePt.h.amsl = H;
        homePt.ground = G(homePt.lat, homePt.lon);
        homePt.drawn = F.toScene(homePt.lat, homePt.lon, drawFloor);
        homePt.visualIndex = 0;
        route.append(homePt);

        Marker m;
        m.pos = homePt.drawn;
        m.kind = MarkerKind::Home;
        // Home is the launch point on the ground: it is drawn in its own
        // color and is not a flight sample.
        m.clearance = std::isfinite(homePt.ground) ? Clearance::NoStrike : Clearance::Unknown;
        m.label = tr("Home");
        m.amsl = H;
        m.ground = homePt.ground;
        m.visualIndex = 0;
        m.seq = 0;
        sc.markers.append(m);
    }

    // G66 parity: the legs the checked samples lie on.
    auto beginLeg = [&sc](double fromLat, double fromLon, double fromAmsl, double toLat, double toLon, double toAmsl,
                          LegShape shape, bool contactLeg, int command, int visualIndex) {
        SampleLeg l;
        l.fromLat = fromLat;
        l.fromLon = fromLon;
        l.fromAmsl = fromAmsl;
        l.toLat = toLat;
        l.toLon = toLon;
        l.toAmsl = toAmsl;
        l.shape = shape;
        l.contactLeg = contactLeg;
        l.command = command;
        l.visualIndex = visualIndex;
        sc.sampleLegs.append(l);
        return int(sc.sampleLegs.size()) - 1;
    };
    auto addSample = [&sc](int leg, double t, double lat, double lon, double amsl, double ground, Clearance c) {
        PathSample s;
        s.leg = leg;
        s.t = t;
        s.lat = lat;
        s.lon = lon;
        s.amsl = amsl;
        s.ground = ground;
        s.clearance = c;
        sc.samples.append(s);
    };

    auto addLoiterRing = [&](const RoutePt &p) {
        const double r = std::abs(p.loiterRadius);
        if (!std::isfinite(r)) {
            return;
        }
        // Rev 4 CL-1, counted in double: G67 M1 caps the ring at
        // kMaxLoiterRingPoints. A ring that needs more is drawn violet.
        const double want = std::ceil(2.0 * M_PI * r / S);
        const bool tooMany = !(want <= kMaxLoiterRingPoints);
        const int n = tooMany ? kMaxLoiterRingPoints : std::max(48, int(want));
        if (!room(n)) {
            return;
        }
        if (tooMany) {
            limit(tr("Item %1: the loiter circle is too large to sample (more than %2 points); it is drawn violet (unknown) and not checked against the terrain.")
                      .arg(p.seq).arg(kMaxLoiterRingPoints));
        }
        const Vec3 c = p.drawn;
        const int ringLeg = beginLeg(p.lat, p.lon, p.h.amsl, p.lat, p.lon, p.h.amsl, LegShape::Ring, false, p.command, p.visualIndex);
        Clearance prevC = Clearance::Unknown;
        Vec3 prevV;
        for (int i = 0; i <= n; i++) {
            const double a = 2.0 * M_PI * i / n;
            const double e = c.x + r * std::cos(a);
            const double nn = -c.z + r * std::sin(a);
            const double lat = F.latFromNorth(nn), lon = F.lonFromEast(e);
            const double g = G(lat, lon);
            double amsl = p.h.amsl;
            if (p.h.datum == Datum::Terrain) {
                amsl = (std::isfinite(g) && std::isfinite(p.h.terrainHag)) ? g + p.h.terrainHag : kNaN;
            }
            const Vec3 v(e, std::isfinite(amsl) ? amsl - F.alt0 : c.y, -nn);
            const Clearance cl = tooMany ? Clearance::Unknown : classify(amsl, g);
            addSample(ringLeg, double(i) / n, lat, lon, amsl, g, cl);   // G66
            if (i > 0) {
                count(sc.stats, cl);
                LineSeg s;
                s.a = prevV;
                s.b = v;
                s.clearance = worst(prevC, cl);
                s.kind = tooMany ? LineKind::Unknown : LineKind::Path;
                s.visualIndex = p.visualIndex;
                sc.lines.append(s);     // room(n) checked above
            }
            prevV = v;
            prevC = cl;
        }
    };

    // A route point copied from `from` to another position at the same
    // height (terrain frame keeps its height above ground).
    auto atSameHeight = [&](const RoutePt &from, double lat, double lon) {
        RoutePt q = from;
        q.lat = lat;
        q.lon = lon;
        q.ground = G(lat, lon);
        q.command = 0;
        q.loiterRadius = kNaN;
        q.spline = false;
        q.unknownLeg = false;
        q.contactTakeoff = false;
        q.contactLand = false;
        q.markerIndex = -1;
        q.noLegInto = false;
        q.far = beyondExtent(plan, lat, lon);
        if (q.h.datum == Datum::Terrain) {
            q.h.amsl = (std::isfinite(q.ground) && std::isfinite(q.h.terrainHag)) ? q.ground + q.h.terrainHag : kNaN;
        }
        q.drawn = F.toScene(lat, lon, q.h.known() ? q.h.amsl : from.drawn.y + F.alt0);
        return q;
    };

    QStringList undrawn;
    int skippedHigh = 0;    // commands 100 and above: DO_, camera and other non-navigation items
    int jumps = 0;

    for (const SourceItem &it : plan.items) {
        if (isRoi(it.command)) {
            const double lat = it.params[4], lon = it.params[5];
            if (!validLatLon(lat, lon)) {
                continue;
            }
            const double g = G(lat, lon);
            Height h = placeHeight(it.frame, it.params[6], H, g);
            farPoint(lat, lon, tr("ROI %1").arg(it.seq));
            boundHeight(h, tr("ROI %1").arg(it.seq));
            Marker m;
            m.kind = MarkerKind::Roi;
            m.amsl = h.amsl;
            m.ground = g;
            m.pos = drawnAt(lat, lon, h.amsl, it.params[6]);
            // An ROI is where the camera and nose point, not a point flown
            // through: it is never classed a strike (CL-1, red means a strike
            // only). Violet when its height or ground is unknown (CL-2).
            m.clearance = classify(h.amsl, g) == Clearance::Unknown ? Clearance::Unknown : Clearance::NoStrike;
            m.visualIndex = it.visualIndex;
            m.seq = it.seq;
            m.command = it.command;
            // FW-4 (rev 7): from GC 5.2.0 an ArduPilot structure scan expands to
            // 195 at the structure center (the mean of its corners), at the
            // middle of the scanned band, above launch, in place of 196. It
            // reaches the view through the scan's own appendMissionItems, as at
            // upload, and is drawn where GC sends it. PX4 keeps 196 and 197,
            // which carry no position and are not drawn.
            m.label = (it.fromComplex && it.command == 195) ? tr("ROI %1 (structure center)").arg(it.seq)
                                                             : tr("ROI %1").arg(it.seq);
            sc.markers.append(m);
            addStem(m.pos, g, it.visualIndex);
            continue;
        }

        if (isRtl(it.command)) {
            // A-10: return to launch. Where and how high the aircraft returns
            // is its own setting, not in the plan, so every RTL leg is drawn
            // violet (unknown) and never classed: never red (CL-1).
            if (route.isEmpty() || !homeValid) {
                continue;
            }
            const RoutePt last = route.last();
            const bool descends = !plan.fixedWing || plan.vtol;   // a plane loiters over its destination

            // Destination: with rally points, ArduPilot (RALLY_INCL_HOME, 1 by
            // default) and PX4 (safe points, home included) return to the
            // nearest of the rally points and home; without, home.
            double destLat = homePt.lat, destLon = homePt.lon;
            Vec3 destDrawn = homePt.drawn;
            int destRally = -1;
            const double le = F.east(last.lon), ln = F.north(last.lat);
            double best = std::hypot(F.east(destLon) - le, F.north(destLat) - ln);
            for (int r = 0; r < plan.rally.size(); r++) {
                const SourcePoint &rp = plan.rally[r];
                if (!validLatLon(rp.lat, rp.lon)) {
                    continue;
                }
                const double d = std::hypot(F.east(rp.lon) - le, F.north(rp.lat) - ln);
                if (d < best) {
                    best = d;
                    destLat = rp.lat;
                    destLon = rp.lon;
                    destDrawn = drawnAt(rp.lat, rp.lon, placeHeight(3, rp.alt, H, kNaN).amsl, rp.alt);
                    destRally = r;
                }
            }

            // Return height: read from a connected aircraft when available
            // (drawn only, still not classed), otherwise the last height.
            const double returnAmsl = (std::isfinite(plan.rtlReturnRelM) && std::isfinite(H) && last.h.known())
                                        ? std::max(last.h.amsl, H + plan.rtlReturnRelM) : kNaN;
            const double drawnY = std::isfinite(returnAmsl) ? returnAmsl - F.alt0 : last.drawn.y;

            auto rtlPoint = [&](double lat, double lon, const Vec3 &drawn) {
                RoutePt q;
                q.lat = lat;
                q.lon = lon;
                q.ground = G(lat, lon);
                q.drawn = drawn;
                q.command = 20;
                q.unknownLeg = true;
                q.visualIndex = it.visualIndex;
                return q;   // q.h stays unknown: nothing after it is classed from it
            };
            if (drawnY > last.drawn.y + 0.01) {
                route.append(rtlPoint(last.lat, last.lon, Vec3(last.drawn.x, drawnY, last.drawn.z)));
            }
            route.append(rtlPoint(destLat, destLon, Vec3(destDrawn.x, drawnY, destDrawn.z)));
            if (descends) {
                route.append(rtlPoint(destLat, destLon, destDrawn));
            }
            sc.hasUnknownRtl = true;
            if (destRally >= 0) {
                sc.warnings << tr("Return to launch is drawn to the nearest of the rally points and home (Rally %1); the autopilot's own rally settings decide where it goes.").arg(destRally + 1);
            }
            if (std::isfinite(returnAmsl)) {
                sc.warnings << tr("Return to launch is drawn at %1 m above launch, read from %2; it is not checked against the terrain.")
                                   .arg(plan.rtlReturnRelM, 0, 'f', 0).arg(plan.rtlSource);
            }
            continue;
        }

        if (!isRoutePoint(it.command)) {
            if (it.command >= 100) {
                skippedHigh++;
                jumps += it.command == 177 ? 1 : 0;   // MAV_CMD_DO_JUMP
            } else if (it.command != 0) {
                undrawn << QString::number(it.command);
            }
            continue;
        }

        double lat = it.params[4], lon = it.params[5];
        if (!validLatLon(lat, lon)) {
            if (isTakeoff(it.command) && homeValid) {
                // A-11: an ArduPilot takeoff with no position climbs at launch.
                lat = plan.homeLat;
                lon = plan.homeLon;
            } else if (isLand(it.command) && !route.isEmpty()) {
                // A land with no position lands where it is.
                lat = route.last().lat;
                lon = route.last().lon;
            } else {
                undrawn << QString::number(it.command);
                continue;
            }
        }

        RoutePt p;
        p.lat = lat;
        p.lon = lon;
        p.command = it.command;
        p.visualIndex = it.visualIndex;
        p.seq = it.seq;
        p.loiterRadius = loiterRadius(it);
        bool aircraftRadius = false;
        if (isLoiter(it.command) && loitersAsFlown(plan)) {
            // Rev 11 L-2 to L-5.
            p.loiterRadius = flownLoiterRadius(it, plan.arduPilot, aircraftRadius);
            p.loiterExit = loiterExit(it, plan.arduPilot);
        }
        if (std::isfinite(p.loiterRadius) && std::abs(p.loiterRadius) > kMaxLoiterRadiusM) {
            // G67 M1: no ring and no tangent exit; the point itself is still drawn.
            limit(tr("Item %1: the loiter radius (%2 m) is over the %3 km drawing limit; the circle is not drawn and not checked against the terrain.")
                      .arg(it.seq).arg(std::abs(p.loiterRadius), 0, 'g', 6).arg(QString::number(kMaxLoiterRadiusM / 1000.0, 'g', 6)));
            p.loiterRadius = kNaN;
        }
        p.ground = G(lat, lon);
        p.h = placeHeight(it.frame, it.params[6], H, p.ground);    // A-1: frame and params[6], never Altitude
        if (p.h.datum == Datum::None) {
            sc.warnings << tr("Item %1 (command %2) has frame %3, which carries no position height; not drawn.").arg(it.seq).arg(it.command).arg(it.frame);
            continue;
        }
        boundHeight(p.h, tr("Item %1").arg(it.seq));
        if (beyondExtent(plan, lat, lon)) {
            p.far = true;
            limit(tr("Item %1 is more than %2 km from home; it and its legs are drawn violet (unknown) and not checked against the terrain.")
                      .arg(it.seq).arg(kmLimitExtent));
        }
        if (p.h.datum == Datum::Terrain) {
            sc.hasTerrainFrame = true;
        }
        if (!it.fromComplex && it.altitudeMode != 0 && !modeAgreesWithFrame(it.altitudeMode, it.frame)) {
            if (it.altitudeMode == 2 && p.h.datum == Datum::Terrain) {
                sc.warnings << tr("Item %1 uses a terrain frame but is labeled AMSL in GC. It is drawn above terrain, which is how the autopilot flies it.")
                                   .arg(it.seq);
            } else {
                sc.warnings << tr("Item %1: its altitude mode and its frame disagree. It is drawn by its frame, which is what is sent to the aircraft.")
                                   .arg(it.seq);
            }
        }
        if (!it.fromComplex && it.altitudeMode == 3 && !std::isfinite(it.params[6])) {
            sc.warnings << tr("Item %1 is calculated above terrain and has no height yet, so it is drawn as unknown.").arg(it.seq);
        }
        p.drawn = drawnAt(lat, lon, p.h.amsl, p.h.datum == Datum::Amsl ? 0.0 : it.params[6]);
        // G65 (E-2, CL-2): a held height is drawn, but the point and its legs
        // are unknown until GC's terrain query answers.
        const bool heightPending = it.heightHeld && p.h.known();
        if (heightPending) {
            p.h.amsl = kNaN;
        }
        if (it.command == 82) {
            if (plan.arduPilot && plan.multiRotor) {
                p.spline = true;
                sc.hasSpline = true;
            } else {
                sc.warnings << tr("Item %1 is a spline waypoint, which only ArduPilot multirotors fly as a curve. It is drawn straight; the autopilot may fly it straight or refuse it.").arg(it.seq);
            }
        }

        const qsizetype firstNew = route.size();
        bool noOutbound = false;
        if (!route.isEmpty()) {
            const RoutePt prev = route.last();
            // Leaving a loiter. Rev 11 (L-3, L-4): from the tangent point T
            // toward this item, or from the center when this item is inside
            // the ring or the rule tracks from C; L-5: no outbound leg at all.
            // Before rev 11, and for a multirotor: only 31 left on the tangent.
            const bool tangent = prev.loiterExit ? prev.loiterExit == LoiterExitTangent : prev.command == 31;
            noOutbound = prev.loiterExit == LoiterExitNone;
            double tE = 0, tN = 0;
            if (tangent && std::isfinite(prev.loiterRadius)
                    && loiterTangent(prev.drawn.x, -prev.drawn.z, prev.loiterRadius, p.drawn.x, -p.drawn.z, tE, tN)) {
                RoutePt exitPt = atSameHeight(prev, F.latFromNorth(tN), F.lonFromEast(tE));
                exitPt.visualIndex = prev.visualIndex;
                exitPt.seq = prev.seq;
                exitPt.noLegInto = true;    // the ring drawn around the loiter is the path to here
                route.append(exitPt);
            }
        }

        if (isLand(it.command) && !route.isEmpty()
                && ((it.command == 21 && plan.multiRotor) || it.command == 85)) {
            // A multirotor, or a VTOL in its landing, flies to the point at its
            // height and then descends.
            const RoutePt prev = route.last();
            if (std::abs(prev.drawn.x - p.drawn.x) > 0.01 || std::abs(prev.drawn.z - p.drawn.z) > 0.01) {
                RoutePt over = atSameHeight(prev, lat, lon);
                over.unknownLeg = prev.unknownLeg;
                over.visualIndex = it.visualIndex;
                over.seq = it.seq;
                route.append(over);
            }
        }
        // CL-1a (rev 5): a takeoff (22, 84, or the climb from the planned
        // home to the first item) and a landing (21, 85, the final leg of a
        // landing pattern, which ends in one of them) leave or reach the
        // ground by design. Which of their samples are in the contact span is
        // decided per sample when the leg is sampled; a plan that does not
        // start on the ground gets an empty span there.
        const bool fromHome = homeValid && route.size() == 1;
        p.contactTakeoff = !route.isEmpty() && (fromHome || isTakeoff(it.command));
        p.contactLand = !route.isEmpty() && isLand(it.command);

        if (p.loiterExit && std::isfinite(p.loiterRadius) && !route.isEmpty()) {
            // Rev 11 L-1: the inbound leg ends where it meets the ring; the
            // spoke to the center is never flown and is not drawn.
            const RoutePt &prev = route.last();
            const auto e = loiterEntry(prev.drawn.x, -prev.drawn.z, p.drawn.x, -p.drawn.z, std::abs(p.loiterRadius));
            RoutePt entry = atSameHeight(p, F.latFromNorth(e.second), F.lonFromEast(e.first));
            entry.visualIndex = p.visualIndex;
            entry.seq = p.seq;
            entry.contactTakeoff = p.contactTakeoff;
            entry.contactLand = p.contactLand;
            route.append(entry);
            p.noLegInto = true;
        }
        if (noOutbound) {
            // Rev 11 L-5: after a 17 the aircraft never leaves the ring.
            if (firstNew < route.size()) {
                route[firstNew].noLegInto = true;
            } else {
                p.noLegInto = true;
            }
        }
        p.markerIndex = int(sc.markers.size());
        route.append(p);

        Marker m;
        m.pos = p.drawn;
        m.kind = it.fromComplex ? MarkerKind::PatternPoint : MarkerKind::Waypoint;
        m.clearance = classify(p.h.amsl, p.ground);
        m.visualIndex = it.visualIndex;
        m.seq = it.seq;
        m.command = it.command;
        m.amsl = p.h.amsl;
        m.ground = p.ground;
        m.heightPending = heightPending;
        m.terrainUnavailable = heightPending && it.heightUnavailable;
        m.label = isTakeoff(it.command) ? tr("%1 Takeoff").arg(it.seq)
                : isLand(it.command)    ? tr("%1 Land").arg(it.seq)
                : aircraftRadius        ? tr("%1 (radius set by the aircraft)").arg(it.seq)
                : QString::number(it.seq);
        sc.markers.append(m);
        addStem(p.drawn, p.ground, it.visualIndex);

        if (std::isfinite(p.loiterRadius)) {
            addLoiterRing(p);
        }
        if (it.command == 18 && !plan.arduPilot && plan.loitersAsFlown) {
            // Rev 11 L-5: PX4 refuses LOITER_TURNS at upload.
            sc.warnings << tr("Item %1: the autopilot does not accept this item (PX4 refuses loiter turns at upload), so nothing after it is drawn.").arg(it.seq);
            break;
        }
    }
    sc.stats.skippedCommands = skippedHigh;
    if (skippedHigh > 0) {
        sc.warnings << (skippedHigh == 1
                            ? tr("1 item with a command of 100 or above (camera, speed and other non-navigation commands) is not drawn in 3D.")
                            : tr("%1 items with commands of 100 or above (camera, speed and other non-navigation commands) are not drawn in 3D.").arg(skippedHigh));
    }
    if (jumps > 0) {
        sc.warnings << (jumps == 1
                            ? tr("This plan has 1 DO_JUMP item: the repeated legs are flown but not drawn or checked here.")
                            : tr("This plan has %1 DO_JUMP items: the repeated legs are flown but not drawn or checked here.").arg(jumps));
    }
    undrawn.removeDuplicates();
    if (!undrawn.isEmpty()) {
        sc.warnings << tr("Commands not drawn: %1.").arg(undrawn.join(QStringLiteral(", ")));
    }

    // ---- Legs: at least kSamplesPerLeg samples each (section 4) --------

    QList<Vec3> drawnPts;
    drawnPts.reserve(route.size());
    for (const RoutePt &p : route) {
        drawnPts.append(p.drawn);
    }
    for (int i = 1; i < route.size(); i++) {
        const RoutePt &a = route[i - 1];
        const RoutePt &b = route[i];

        if (b.noLegInto) {
            continue;
        }
        if (b.unknownLeg) {
            // A-10, CL-2: the height is not in the file. Violet, never red.
            push(LineSeg{a.drawn, b.drawn, Clearance::Unknown, LineKind::Unknown, b.visualIndex});
            sc.stats.samplesUnknown += kSamplesPerLeg;
            const int rtlLeg = beginLeg(a.lat, a.lon, a.h.amsl, b.lat, b.lon, kNaN, LegShape::UnknownHeight, false, b.command, b.visualIndex);
            addSample(rtlLeg, 0.0, a.lat, a.lon, kNaN, a.ground, Clearance::Unknown);
            addSample(rtlLeg, 1.0, b.lat, b.lon, kNaN, b.ground, Clearance::Unknown);
            continue;
        }
        if (sc.overSampleBudget) {
            break;      // G67 M1: nothing more is drawn
        }
        // Rev 9: a leg into or out of an item beyond the plan extent, or
        // longer than kMaxLegM, is one violet line, never classed.
        const double chordM = std::hypot(b.drawn.x - a.drawn.x, b.drawn.z - a.drawn.z);
        if (a.far || b.far || !(chordM <= kMaxLegM)) {
            if (!a.far && !b.far) {
                limit(tr("Item %1: the leg into it is longer than %2 km; it is drawn violet (unknown) and not checked against the terrain.")
                          .arg(b.seq).arg(QString::number(kMaxLegM / 1000.0, 'g', 6)));
            }
            push(LineSeg{a.drawn, b.drawn, Clearance::Unknown, LineKind::Unknown, b.visualIndex});
            sc.stats.samplesUnknown += kSamplesPerLeg;
            const int unknownLeg = beginLeg(a.lat, a.lon, a.h.amsl, b.lat, b.lon, kNaN, LegShape::UnknownHeight, false, b.command, b.visualIndex);
            addSample(unknownLeg, 0.0, a.lat, a.lon, kNaN, a.ground, Clearance::Unknown);
            addSample(unknownLeg, 1.0, b.lat, b.lon, kNaN, b.ground, Clearance::Unknown);
            continue;
        }

        const bool spline = b.spline && route.size() >= 3;     // S-1, S-7
        // CL-2: the curve's shape comes from its four control points; if any
        // of them has an unknown height, the leg's height is unknown too.
        bool splineUnknown = false;
        if (spline) {
            for (int k = std::max(0, i - 2); k <= std::min(int(route.size()) - 1, i + 1); k++) {
                splineUnknown |= !route[k].h.known();
            }
        }
        const bool terrainLeg = b.h.datum == Datum::Terrain;  // A-7
        double hagA = kNaN;
        if (terrainLeg) {
            hagA = a.h.datum == Datum::Terrain ? a.h.terrainHag
                 : ((a.h.known() && std::isfinite(a.ground)) ? a.h.amsl - a.ground : kNaN);
        }
        const bool straightKnown = a.h.known() && b.h.known();

        // Rev 4 CL-1: at least kSamplesPerLeg samples, and more until no two
        // neighbors are more than S apart along the ground. Both ends sampled.
        auto at = [&](double t) {
            return spline ? catmullRomLegPoint(drawnPts.constData(), int(drawnPts.size()), i - 1, t)
                          : lerp(a.drawn, b.drawn, t);
        };
        // G67 M1: counted in double and capped at kMaxSamplesPerLeg before
        // any int conversion; NaN or infinity counts as over the cap.
        const double chordN = std::ceil(chordM / S);
        bool tooMany = !(chordN <= kMaxSamplesPerLeg);
        int n = tooMany ? kMaxSamplesPerLeg : std::max(kSamplesPerLeg, int(chordN));
        if (spline && !tooMany) {
            // The curve is longer than its chord: refine until the widest gap fits.
            for (int pass = 0; pass < 6; pass++) {
                double widest = 0;
                Vec3 prev = at(0);
                for (int s = 1; s <= n; s++) {
                    const Vec3 v = at(double(s) / n);
                    widest = std::max(widest, std::hypot(v.x - prev.x, v.z - prev.z));
                    prev = v;
                }
                if (widest <= S) {
                    break;
                }
                const double next = std::ceil(n * widest / S) + 1;
                if (!(next <= kMaxSamplesPerLeg)) {
                    tooMany = true;
                    break;
                }
                n = int(next);
            }
        }
        if (tooMany) {
            // G67 M1: too long to sample S apart. One straight violet line,
            // never classed (CL-2), and the view says so.
            if (room(1)) {
                limit(tr("Item %1: the leg into it is too long to sample (more than %2 samples); it is drawn violet (unknown) and not checked against the terrain.")
                          .arg(b.seq).arg(kMaxSamplesPerLeg));
                sc.lines.append(LineSeg{a.drawn, b.drawn, Clearance::Unknown, LineKind::Unknown, b.visualIndex});
                sc.stats.samplesUnknown += kSamplesPerLeg;
                const int unknownLeg = beginLeg(a.lat, a.lon, a.h.amsl, b.lat, b.lon, kNaN, LegShape::UnknownHeight, false, b.command, b.visualIndex);
                addSample(unknownLeg, 0.0, a.lat, a.lon, kNaN, a.ground, Clearance::Unknown);
                addSample(unknownLeg, 1.0, b.lat, b.lon, kNaN, b.ground, Clearance::Unknown);
            }
            continue;
        }
        if (!room(n)) {
            break;      // G67 M1: over the scene budget, nothing more is drawn
        }

        const int sampleLeg = beginLeg(a.lat, a.lon, a.h.amsl, b.lat, b.lon, b.h.amsl,
                                       spline ? LegShape::Spline : (terrainLeg ? LegShape::Terrain : LegShape::Straight),
                                       b.contactTakeoff || b.contactLand, b.command, b.visualIndex);
        // Sample heights first: CL-1a's contact span depends on the whole leg.
        QList<Vec3> vs(n + 1);
        QList<double> amslS(n + 1, kNaN), groundS(n + 1, kNaN), hagS(n + 1, kNaN);
        for (int s = 0; s <= n; s++) {
            const double t = double(s) / n;
            Vec3 v = at(t);
            const double lat = F.latFromNorth(-v.z);
            const double lon = F.lonFromEast(v.x);
            const double g = G(lat, lon);
            double amsl = kNaN;
            if (terrainLeg) {
                // A-7: a leg into a terrain-frame point follows the terrain at a
                // height interpolated between the two heights above ground.
                const double hag = hagA + (b.h.terrainHag - hagA) * t;
                amsl = (std::isfinite(g) && std::isfinite(hag)) ? g + hag : kNaN;
                if (std::isfinite(amsl)) {
                    v.y = amsl - F.alt0;
                }
            } else if (straightKnown && !splineUnknown) {
                // CL-3: compared in AMSL (S-6: a spline on its drawn approximation).
                amsl = v.y + F.alt0;
            }
            vs[s] = v;
            amslS[s] = amsl;
            groundS[s] = g;
            hagS[s] = (std::isfinite(amsl) && std::isfinite(g)) ? amsl - g : kNaN;
        }
        // CL-1a: only a takeoff or landing leg has a contact span; every other
        // leg, and every sample outside the span, keeps the strike rule.
        const QList<bool> span = (b.contactTakeoff || b.contactLand)
                                   ? contactSpan(hagS, b.contactTakeoff, b.contactLand)
                                   : QList<bool>(n + 1, false);

        Vec3 prevV;
        Clearance prevC = Clearance::Unknown;
        for (int s = 0; s <= n; s++) {
            const Vec3 v = vs[s];
            // The one place a leg sample's section 4 state is decided (CL-1,
            // CL-1a span, CL-2). Hook here for per-sample exports (G66).
            const Clearance c = classifyLegSample(amslS[s], groundS[s], span[s]);
            addSample(sampleLeg, double(s) / n, F.latFromNorth(-v.z), F.lonFromEast(v.x), amslS[s], groundS[s], c);   // G66
            if (s > 0 || i == 1) {
                count(sc.stats, c);
            }
            if (s > 0) {
                LineSeg seg;
                seg.a = prevV;
                seg.b = v;
                seg.clearance = worst(prevC, c);
                seg.kind = LineKind::Path;
                seg.visualIndex = b.visualIndex;
                sc.lines.append(seg);   // room(n) checked above
            }
            prevV = v;
            prevC = c;
        }
        // CL-1a: a touchdown (or a takeoff point still in the span) is ground
        // contact on its marker too, by the same last sample.
        if (span[n] && b.markerIndex >= 0 && b.markerIndex < sc.markers.size()) {
            sc.markers[b.markerIndex].clearance = classifyContact(amslS[n], groundS[n]);
        }
    }

    if (sc.hasUnknownRtl) {
        sc.warnings << tr("Return to launch: the return height is the aircraft's setting and is not in the plan, so that leg is drawn violet (unknown).");
    }

    // ---- Fences: prisms without a strike check (CL-7) -------------------
    // A .plan holds no fence altitude (1.10), so the prism is drawn from the
    // ground to above the route, display only.
    double routeTop = drawFloor;
    for (const RoutePt &p : route) {
        routeTop = std::max(routeTop, p.drawn.y + F.alt0);
    }
    const double fenceTop = routeTop + 20.0;
    auto groundOr = [&](double lat, double lon) {
        const double g = G(lat, lon);
        return std::isfinite(g) ? g : drawFloor;
    };
    for (const SourceFence &f : plan.fences) {
        QList<QPair<double, double>> ring = f.polygon;
        if (f.circle) {
            ring.clear();
            if (!validLatLon(f.centerLat, f.centerLon) || !(f.radiusM > 0)) {
                continue;
            }
            LocalFrame lf{f.centerLat, f.centerLon, 0};
            const int n = 64;
            for (int i = 0; i < n; i++) {
                const double a = 2.0 * M_PI * i / n;
                ring.append({lf.latFromNorth(f.radiusM * std::sin(a)), lf.lonFromEast(f.radiusM * std::cos(a))});
            }
        }
        if (ring.size() < 2) {
            continue;
        }
        const LineKind kind = f.inclusion ? LineKind::FenceInclusion : LineKind::FenceExclusion;
        const int verticalEvery = f.circle ? 8 : 1;
        for (int i = 0; i < ring.size(); i++) {
            const auto &p0 = ring[i];
            const auto &p1 = ring[(i + 1) % ring.size()];
            if (!validLatLon(p0.first, p0.second) || !validLatLon(p1.first, p1.second)) {
                continue;   // G67 M1: a vertex that is not a position is not drawn
            }
            const Vec3 b0 = F.toScene(p0.first, p0.second, groundOr(p0.first, p0.second));
            const Vec3 b1 = F.toScene(p1.first, p1.second, groundOr(p1.first, p1.second));
            const Vec3 t0 = F.toScene(p0.first, p0.second, fenceTop);
            const Vec3 t1 = F.toScene(p1.first, p1.second, fenceTop);
            push(LineSeg{b0, b1, Clearance::Unknown, kind, -1});
            push(LineSeg{t0, t1, Clearance::Unknown, kind, -1});
            if (i % verticalEvery == 0) {
                push(LineSeg{b0, t0, Clearance::Unknown, kind, -1});
            }
        }
    }
    if (!plan.fences.isEmpty()) {
        sc.warnings << tr("Fences are drawn from the ground to above the route: a plan holds no fence altitude, and fences are not checked against the terrain.");
    }

    // ---- Pattern footprints on the ground, split to follow the terrain --
    for (const SourceOutline &o : plan.outlines) {
        const int n = int(o.path.size());
        const int edges = o.closed ? n : n - 1;
        for (int i = 0; i < edges; i++) {
            const auto &p0 = o.path[i];
            const auto &p1 = o.path[(i + 1) % n];
            if (!validLatLon(p0.first, p0.second) || !validLatLon(p1.first, p1.second)) {
                continue;   // G67 M1: a vertex that is not a position is not drawn
            }
            const Vec3 a(F.east(p0.second), 0, -F.north(p0.first));
            const Vec3 b(F.east(p1.second), 0, -F.north(p1.first));
            const double len = std::hypot(b.x - a.x, b.z - a.z);
            const int steps = int(std::clamp(std::ceil(len / 25.0), 1.0, 200.0));   // clamped in double (G67 M1)
            Vec3 prev;
            for (int s = 0; s <= steps; s++) {
                Vec3 v = lerp(a, b, double(s) / steps);
                v.y = groundOr(F.latFromNorth(-v.z), F.lonFromEast(v.x)) + 0.3 - F.alt0;
                if (s > 0) {
                    push(LineSeg{prev, v, Clearance::Unknown, LineKind::Outline, o.visualIndex});
                }
                prev = v;
            }
        }
    }

    // ---- Rally points: above launch (A-9) ---------------------------------
    auto addPoint = [&](const SourcePoint &sp, MarkerKind kind, const QString &label) {
        if (!validLatLon(sp.lat, sp.lon)) {
            return;
        }
        const double g = G(sp.lat, sp.lon);
        Height h = placeHeight(3, sp.alt, H, g);
        farPoint(sp.lat, sp.lon, label);
        boundHeight(h, label);
        Marker m;
        m.kind = kind;
        m.amsl = h.amsl;
        m.ground = g;
        m.clearance = classify(h.amsl, g);
        m.pos = drawnAt(sp.lat, sp.lon, h.amsl, sp.alt);
        m.label = label;
        sc.markers.append(m);
        addStem(m.pos, g, -1);
        if (m.clearance == Clearance::Strike && h.amsl < g) {
            sc.warnings << tr("%1 is %2 m above launch, %3 m below the loaded ground, and is drawn red.")
                               .arg(label, QString::number(sp.alt, 'g', 6), QString::number(g - h.amsl, 'f', 1));
        } else if (m.clearance == Clearance::Strike) {
            sc.warnings << tr("%1 is %2 m above launch, within 2 m of the loaded ground, and is drawn red.")
                               .arg(label, QString::number(sp.alt, 'g', 6));
        } else if (std::isfinite(sp.alt) && sp.alt == 0.0) {
            sc.warnings << tr("%1 is at 0 m above launch.").arg(label);
        }
    };
    for (int i = 0; i < plan.rally.size(); i++) {
        addPoint(plan.rally[i], MarkerKind::Rally, tr("Rally %1").arg(i + 1));
    }

    // ---- Breach return point (A-9, rev 8) ----------------------------------
    // ArduPilot and PX4 never use its height (G86), so it is a position marker
    // only: drawn at the ground under its position, never strike colored, and
    // its stored height (SourcePoint::alt, as read) is not used or changed.
    if (validLatLon(plan.breachReturn.lat, plan.breachReturn.lon)) {
        const double g = G(plan.breachReturn.lat, plan.breachReturn.lon);
        Marker m;
        m.kind = MarkerKind::BreachReturn;
        m.amsl = kNaN;                          // height not used
        m.ground = g;
        m.clearance = Clearance::NoStrike;      // excluded from strike coloring; drawn in its own color
        // On unknown ground it is drawn at launch level, display only (A-3).
        m.pos = F.toScene(plan.breachReturn.lat, plan.breachReturn.lon, std::isfinite(g) ? g : drawFloor);
        m.label = tr("Breach return (height not used)");
        sc.markers.append(m);
    }

    // ---- Bounds of everything drawn --------------------------------------
    bool first = true;
    auto grow = [&](const Vec3 &v) {
        if (first) {
            sc.boundsMin = sc.boundsMax = v;
            first = false;
            return;
        }
        sc.boundsMin = Vec3(std::min(sc.boundsMin.x, v.x), std::min(sc.boundsMin.y, v.y), std::min(sc.boundsMin.z, v.z));
        sc.boundsMax = Vec3(std::max(sc.boundsMax.x, v.x), std::max(sc.boundsMax.y, v.y), std::max(sc.boundsMax.z, v.z));
    };
    for (const LineSeg &l : sc.lines) {
        grow(l.a);
        grow(l.b);
    }
    for (const Marker &m : sc.markers) {
        grow(m.pos);
    }

    sc.warnings.removeDuplicates();
    return sc;
}

} // namespace Plan3D
