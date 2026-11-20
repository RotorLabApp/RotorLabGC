/****************************************************************************
 *
 * RotorLab GC Plan view 3D (G64, MG2) - read the plan from GC's controllers.
 *
 ****************************************************************************/

#include "Plan3DSource.h"

#include "CorridorScanComplexItem.h"
#include "GeoFenceController.h"
#include "MissionController.h"
#include "MissionItem.h"
#include "QGCLoggingCategory.h"
#include "MissionSettingsItem.h"
#include "ParameterManager.h"
#include "PlanMasterController.h"
#include "QGCFenceCircle.h"
#include "QGCFencePolygon.h"
#include "QGCMapPolygon.h"
#include "QGCMapPolyline.h"
#include "QmlObjectListModel.h"
#include "RallyPoint.h"
#include "RallyPointController.h"
#include "SimpleMissionItem.h"
#include "StructureScanComplexItem.h"
#include "SurveyComplexItem.h"
#include "Vehicle.h"
#include "VisualMissionItem.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QPointer>

QGC_LOGGING_CATEGORY(Plan3DSourceLog, "rotorlab.plan3d.source")

namespace Plan3D {

namespace {

/// Per settings item (a new one per loaded plan): the plan's own home
/// altitude, and every terrain height GC's query reported for the home.
struct HomeTrack {
    QPointer<QObject> item;             ///< guards against a reused address
    double          planH = kNaN;
    QList<double>   gcTerrain;
};

QHash<const QObject *, HomeTrack> &homeTracks()
{
    static QHash<const QObject *, HomeTrack> tracks;
    return tracks;
}

/// plannedHomePosition[2] from the plan file, when the file's home is still at
/// `home` (the user has not moved it); NaN otherwise.
double homeAmslFromFile(const QString &path, const QGeoCoordinate &home)
{
    if (path.isEmpty()) {
        return kNaN;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return kNaN;
    }
    const QJsonArray h = QJsonDocument::fromJson(f.readAll()).object()[QStringLiteral("mission")].toObject()[QStringLiteral("plannedHomePosition")].toArray();
    if (h.size() < 3 || h[2].isNull()
            || std::abs(h[0].toDouble() - home.latitude()) > 1e-7 || std::abs(h[1].toDouble() - home.longitude()) > 1e-7) {
        return kNaN;
    }
    return h[2].toDouble();
}

/// A-0b needs the plan's own H, but GC replaces the home altitude with its
/// terrain query's answer (MissionSettingsItem::_setHomeAltFromTerrain), which
/// in turn moves the home and resets the settings item's terrain height to
/// NaN while it asks again. So every terrain height GC reports for the home
/// is recorded through its own signal, and a home altitude equal to one of
/// them is GC's, not the plan's. On first sight the plan file is read.
double planHomeAmsl(PlanMasterController *pmc, MissionSettingsItem *settings, const QGeoCoordinate &home)
{
    const double current = home.isValid() ? home.altitude() : kNaN;
    QHash<const QObject *, HomeTrack> &tracks = homeTracks();
    auto isGc = [current](const HomeTrack &t, double terrainNow) {
        if (!std::isfinite(current)) {
            return false;
        }
        if (std::isfinite(terrainNow) && std::abs(current - terrainNow) < 1e-6) {
            return true;
        }
        for (double g : t.gcTerrain) {
            if (std::abs(current - g) < 1e-6) {
                return true;
            }
        }
        return false;
    };
    if (!tracks.contains(settings) || tracks.value(settings).item.data() != settings) {
        // First sight: the plan file, when its home is still where the plan's
        // is, gives the plan's own H; a different current value is GC's
        // overwrite. (An unsaved home altitude edit made before the 3D view
        // first opened is not seen until it is edited again.)
        HomeTrack t;
        t.item = settings;
        const double fromFile = homeAmslFromFile(pmc->currentPlanFile(), home);
        t.planH = std::isfinite(fromFile) ? fromFile : current;
        if (std::isfinite(fromFile) && std::isfinite(current) && std::abs(current - fromFile) >= 1e-6) {
            t.gcTerrain.append(current);
        }
        if (std::isfinite(settings->terrainAltitude())) {
            t.gcTerrain.append(settings->terrainAltitude());
        }
        tracks.insert(settings, t);
        qCDebug(Plan3DSourceLog) << "home first seen: current" << current << "file" << fromFile << "plan H" << t.planH;
        QObject::connect(settings, &VisualMissionItem::terrainAltitudeChanged, qApp, [settings](double terrain) {
            if (std::isfinite(terrain) && homeTracks().contains(settings)) {
                homeTracks()[settings].gcTerrain.append(terrain);
                qCDebug(Plan3DSourceLog) << "GC terrain at home" << terrain;
            }
        });
        QObject::connect(settings, &QObject::destroyed, qApp, [](QObject *o) { homeTracks().remove(o); });
        return t.planH;
    }
    HomeTrack &t = tracks[settings];
    if (!isGc(t, settings->terrainAltitude())) {
        t.planH = current;      // the plan's value, or the user's edit
    }
    return t.planH;
}

/// A-10: the RTL return height above launch, only from a connected
/// multirotor's own parameters (never from the file, which holds none).
/// ArduCopter RTL_ALT is in centimetres (0 = return at the current height);
/// PX4 RTL_RETURN_ALT is in metres. NaN when not available.
double rtlReturnFromAircraft(Vehicle *vehicle, QString &source)
{
    if (!vehicle || vehicle->isOfflineEditingVehicle() || !vehicle->multiRotor()) {
        return kNaN;
    }
    ParameterManager *pm = vehicle->parameterManager();
    if (!pm || !pm->parametersReady()) {
        return kNaN;
    }
    const int comp = ParameterManager::defaultComponentId;
    if (vehicle->apmFirmware() && pm->parameterExists(comp, QStringLiteral("RTL_ALT"))) {
        source = QStringLiteral("RTL_ALT");
        return pm->getParameter(comp, QStringLiteral("RTL_ALT"))->rawValue().toDouble() / 100.0;
    }
    if (vehicle->px4Firmware() && pm->parameterExists(comp, QStringLiteral("RTL_RETURN_ALT"))) {
        source = QStringLiteral("RTL_RETURN_ALT");
        return pm->getParameter(comp, QStringLiteral("RTL_RETURN_ALT"))->rawValue().toDouble();
    }
    return kNaN;
}

QList<QPair<double, double>> toPath(const QList<QGeoCoordinate> &coords)
{
    QList<QPair<double, double>> out;
    out.reserve(coords.size());
    for (const QGeoCoordinate &c : coords) {
        out.append({c.latitude(), c.longitude()});
    }
    return out;
}

} // namespace

SourcePlan sourceFromPlan(PlanMasterController *pmc)
{
    SourcePlan plan;
    if (!pmc) {
        return plan;
    }
    MissionController *mc = pmc->missionController();
    Vehicle *vehicle = pmc->controllerVehicle();
    if (vehicle) {
        plan.arduPilot = vehicle->apmFirmware();
        plan.multiRotor = vehicle->multiRotor();
        plan.fixedWing = vehicle->fixedWing();
        plan.vtol = vehicle->vtol();
    }
    {
        // The plan's own vehicle is the offline one; a connected aircraft is the manager vehicle.
        QString source;
        const double rtl = rtlReturnFromAircraft(pmc->managerVehicle(), source);
        if (std::isfinite(rtl) && rtl >= 0) {
            plan.rtlReturnRelM = rtl;
            plan.rtlSource = QObject::tr("the connected aircraft (%1)").arg(source);
        }
    }

    if (mc) {
        const QGeoCoordinate home = mc->plannedHomePosition();
        plan.homeLat = home.isValid() ? home.latitude() : kNaN;
        plan.homeLon = home.isValid() ? home.longitude() : kNaN;
        plan.homeAmsl = home.isValid() ? home.altitude() : kNaN;   // NaN when the home has no altitude (A-4)

        // A-0b: the plan's own H, not GC's later overwrite from terrain.
        if (QmlObjectListModel *vis = mc->visualItems(); vis && vis->count() > 0) {
            if (auto *settings = vis->value<MissionSettingsItem *>(0)) {
                plan.homeAmsl = planHomeAmsl(pmc, settings, home);
            }
        }

        QmlObjectListModel *visualItems = mc->visualItems();
        QObject scratch;
        for (int i = 1; visualItems && i < visualItems->count(); i++) {   // 0 is the planned home
            VisualMissionItem *vi = visualItems->value<VisualMissionItem *>(i);
            if (!vi) {
                continue;
            }
            QList<MissionItem *> items;
            vi->appendMissionItems(items, &scratch);
            SimpleMissionItem *simple = qobject_cast<SimpleMissionItem *>(vi);
            for (const MissionItem *mi : items) {
                SourceItem s;
                s.seq = mi->sequenceNumber();
                s.visualIndex = i;
                s.command = int(mi->command());
                s.frame = int(mi->frame());
                s.params[0] = mi->param1();
                s.params[1] = mi->param2();
                s.params[2] = mi->param3();
                s.params[3] = mi->param4();
                s.params[4] = mi->param5();
                s.params[5] = mi->param6();
                s.params[6] = mi->param7();
                s.fromComplex = (simple == nullptr);
                if (simple && simple->specifiesAltitude() && int(mi->command()) == simple->command()) {
                    s.altitudeMode = int(simple->altitudeMode());
                }
                plan.items.append(s);
            }
            qDeleteAll(items);

            if (auto *survey = qobject_cast<SurveyComplexItem *>(vi)) {
                plan.outlines.append(SourceOutline{i, true, toPath(survey->surveyAreaPolygon()->coordinateList())});
            } else if (auto *corridor = qobject_cast<CorridorScanComplexItem *>(vi)) {
                plan.outlines.append(SourceOutline{i, false, toPath(corridor->corridorPolyline()->coordinateList())});
            } else if (auto *structure = qobject_cast<StructureScanComplexItem *>(vi)) {
                plan.outlines.append(SourceOutline{i, true, toPath(structure->structurePolygon()->coordinateList())});
            }
        }
    }

    if (GeoFenceController *gf = pmc->geoFenceController()) {
        QmlObjectListModel *polys = gf->polygons();
        for (int i = 0; polys && i < polys->count(); i++) {
            QGCFencePolygon *p = polys->value<QGCFencePolygon *>(i);
            if (!p) {
                continue;
            }
            SourceFence f;
            f.inclusion = p->inclusion();
            f.polygon = toPath(p->coordinateList());
            plan.fences.append(f);
        }
        QmlObjectListModel *circles = gf->circles();
        for (int i = 0; circles && i < circles->count(); i++) {
            QGCFenceCircle *c = circles->value<QGCFenceCircle *>(i);
            if (!c) {
                continue;
            }
            SourceFence f;
            f.circle = true;
            f.inclusion = c->inclusion();
            f.centerLat = c->center().latitude();
            f.centerLon = c->center().longitude();
            f.radiusM = c->radius()->rawValue().toDouble();
            plan.fences.append(f);
        }
        const QGeoCoordinate br = gf->breachReturnPoint();
        if (br.isValid()) {
            // The altitude GC saves and uploads is the fact, not the point's own (GeoFenceController.cc:218-222).
            plan.breachReturn = SourcePoint{br.latitude(), br.longitude(), gf->breachReturnAltitude()->rawValue().toDouble()};
        }
    }

    if (RallyPointController *rc = pmc->rallyPointController()) {
        QmlObjectListModel *pts = rc->points();
        for (int i = 0; pts && i < pts->count(); i++) {
            RallyPoint *r = pts->value<RallyPoint *>(i);
            if (!r) {
                continue;
            }
            const QGeoCoordinate c = r->coordinate();
            plan.rally.append(SourcePoint{c.latitude(), c.longitude(), c.altitude()});
        }
    }
    return plan;
}

} // namespace Plan3D
