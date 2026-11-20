/****************************************************************************
 *
 * RotorLab GC Plan view 3D (G64, MG2) - the scene object Plan3DView.qml binds.
 *
 ****************************************************************************/

#include "Plan3DScene.h"

#include "Plan3DColors.h"
#include "Plan3DEditor.h"
#include "Plan3DGeometry.h"
#include "Plan3DGroundMesh.h"
#include "Plan3DGround.h"
#include "Plan3DSource.h"

#include "GeoFenceController.h"
#include "MissionController.h"
#include "PlanMasterController.h"
#include "QmlObjectListModel.h"
#include "RallyPointController.h"
#include "FlightMapSettings.h"
#include "QGCFenceCircle.h"
#include "QGCFencePolygon.h"
#include "RallyPoint.h"
#include "SimpleMissionItem.h"
#include "SettingsManager.h"
#include "Vehicle.h"
#include "ParameterManager.h"

#include <QtCore/QDateTime>
#include <QtCore/QElapsedTimer>
#include <QtCore/QLoggingCategory>
#include <QtCore/QtMath>
#include <QtQml/QQmlEngine>
#include <QtQml/qqml.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QMap>
#include <QtCore/QSet>

#include <algorithm>

Q_LOGGING_CATEGORY(Plan3DLog, "rotorlab.plan3d")

namespace Plan3D {

namespace {

// Everything else avoids red (CL-1: red means a strike and nothing else).
const QColor kPin(QStringLiteral("#4d8bff"));
const QColor kSelected(QStringLiteral("#f0b429"));
const QColor kRally(QStringLiteral("#84cc16"));
const QColor kBreach(QStringLiteral("#f59e0b"));
const QColor kRoi(QStringLiteral("#ec4899"));
const QColor kFenceIn(QStringLiteral("#4d8bff"));
const QColor kFenceOut(QStringLiteral("#f59e0b"));
// G67 visual M3: height stems in slate-300 (they are always drawn over the
// ground or imagery), apart from the ground contact slate. RotorLab 5.3.0
// review (accessibility N8): slate-600 in the light theme, where slate-300
// is too faint over light ground.
QColor stemColor(bool dark)
{
    return dark ? QColor(QStringLiteral("#cbd5e1")) : QColor(QStringLiteral("#475569"));
}

QString heightText(double hag)
{
    const int decimals = std::abs(hag) < 10.0 ? 1 : 0;
    return hag >= 0 ? QCoreApplication::translate("Plan3D", "%1 m above ground").arg(hag, 0, 'f', decimals)
                    : QCoreApplication::translate("Plan3D", "%1 m below the ground").arg(-hag, 0, 'f', decimals);
}

bool isRouteMarker(MarkerKind k)
{
    return k == MarkerKind::Home || k == MarkerKind::Waypoint || k == MarkerKind::PatternPoint;
}

QVector3D q(const Vec3 &v)
{
    return QVector3D(float(v.x), float(v.y), float(v.z));
}

} // namespace

SceneModel::SceneModel(QObject *parent)
    : QObject(parent)
    , _ground(new Ground(this))
    , _lines(new LineMesh())
    , _markers(new MarkerInstances())
    , _groundMesh(new GroundMesh())
    , _strikeLines(new LineMesh())
    , _strikeMarkers(new MarkerInstances())
    , _editor(new Editor(this))     // declared after _groundMesh (F1)
{
    // The GPU objects live as long as this model; QML only references them.
    QQmlEngine::setObjectOwnership(_lines, QQmlEngine::CppOwnership);
    QQmlEngine::setObjectOwnership(_markers, QQmlEngine::CppOwnership);
    QQmlEngine::setObjectOwnership(_groundMesh, QQmlEngine::CppOwnership);
    QQmlEngine::setObjectOwnership(_strikeLines, QQmlEngine::CppOwnership);
    QQmlEngine::setObjectOwnership(_strikeMarkers, QQmlEngine::CppOwnership);

    _rebuildTimer.setSingleShot(true);
    _rebuildTimer.setInterval(40);
    connect(&_rebuildTimer, &QTimer::timeout, this, &SceneModel::_rebuild);
    connect(_ground, &Ground::changed, this, &SceneModel::_scheduleRebuild);

    // G65: the editor's references are the ones this view draws with (E-2):
    // G(p) from the loaded ground, R from A-0 as the last scene used it.
    _editor->setReferences([this](double lat, double lon) { return _ground->elevation(lat, lon); },
                           [this]() { return _scene.launchRef; });
    connect(_editor, &Editor::draggingChanged, this, &SceneModel::_onEditorDragging);
    connect(_editor, &Editor::itemEdited, this, &SceneModel::_onItemEdited);
    // At most one rebuild per frame while dragging (a throttle, never restarted).
    _dragTimer.setSingleShot(true);
    _dragTimer.setInterval(15);
    connect(&_dragTimer, &QTimer::timeout, this, &SceneModel::_dragRebuild);
    _commitTimer.setSingleShot(true);
    _commitTimer.setInterval(kCommitMs);
    connect(&_commitTimer, &QTimer::timeout, this, &SceneModel::_commitDrag);
    // F4: a held mode 3 height is dropped when its time is up.
    _holdTimer.setSingleShot(true);
    connect(&_holdTimer, &QTimer::timeout, this, &SceneModel::_scheduleRebuild);
    // The legend names the terrain source (CL-6); follow the setting.
    connect(SettingsManager::instance()->flightMapSettings()->elevationMapProvider(), &Fact::rawValueChanged, this, [this]() {
        emit terrainSourceChanged();
        _scheduleRebuild();
    });
}

SceneModel::~SceneModel()
{
    delete _lines;
    delete _markers;
    delete _groundMesh;
    delete _strikeLines;
    delete _strikeMarkers;
}

QObject *SceneModel::planMasterController() const
{
    return _pmc;
}

void SceneModel::setPlanMasterController(QObject *pmc)
{
    PlanMasterController *p = qobject_cast<PlanMasterController *>(pmc);
    if (p == _pmc) {
        return;
    }
    _pmc = p;
    _editor->setPlanMasterController(p);
    _connectPlan();
    emit planMasterControllerChanged();
    _scheduleRebuild();
}

void SceneModel::setActive(bool active)
{
    if (_active == active) {
        return;
    }
    _active = active;
    emit activeChanged();
    if (_active && _pending) {
        _scheduleRebuild();
    }
}

void SceneModel::setDarkTheme(bool dark)
{
    if (_dark == dark) {
        return;
    }
    _dark = dark;
    emit darkThemeChanged();
    _upload();
}

void SceneModel::setSelectedVisualIndex(int index)
{
    if (_selected == index) {
        return;
    }
    _selected = index;
    emit selectedVisualIndexChanged();
    _uploadLinesAndMarkers();   // the ground does not depend on the selection
}

QQuick3DGeometry *SceneModel::lineGeometry() const { return _lines; }
QQuick3DInstancing *SceneModel::markerInstancing() const { return _markers; }
QQuick3DGeometry *SceneModel::groundGeometry() const { return _groundMesh; }
QQuick3DGeometry *SceneModel::strikeLineGeometry() const { return _strikeLines; }
QQuick3DInstancing *SceneModel::strikeMarkerInstancing() const { return _strikeMarkers; }
int SceneModel::segmentCount() const { return _lines->segmentCount(); }
QString SceneModel::terrainSource() const { return Ground::sourceName(); }
bool SceneModel::groundLoading() const { return _ground->loading(); }

int SceneModel::groundKnownPercent() const
{
    const int total = _ground->totalNodes();
    return total > 0 ? int(100.0 * _ground->knownNodes() / total + 0.5) : 0;
}

QVariantList SceneModel::groundArea() const
{
    const GeoBounds &a = _ground->area();
    return {a.south, a.west, a.north, a.east};
}

void SceneModel::_connectPlan()
{
    for (const auto &c : _connections) {
        disconnect(c);
    }
    _connections.clear();
    if (!_pmc) {
        return;
    }
    MissionController *mc = _pmc->missionController();
    if (mc) {
        // Any change to the route: items added, removed, moved, re-heighted,
        // a pattern regenerated, the home moved (2D edits show up live).
        _connections << connect(mc, &MissionController::visualItemsChanged, this, &SceneModel::_scheduleRebuild);
        _connections << connect(mc, &MissionController::plannedHomePositionChanged, this, &SceneModel::_scheduleRebuild);
        _connections << connect(mc, &MissionController::recalcTerrainProfile, this, &SceneModel::_scheduleRebuild);
        _connections << connect(mc, &MissionController::missionPlannedDistanceChanged, this, &SceneModel::_scheduleRebuild);
        _connections << connect(mc, &MissionController::missionBoundingCubeChanged, this, &SceneModel::_scheduleRebuild);
        _connections << connect(mc, &MissionController::globalAltitudeModeChanged, this, &SceneModel::_scheduleRebuild);
        _connections << connect(mc, &MissionController::minAMSLAltitudeChanged, this, &SceneModel::_scheduleRebuild);
        _connections << connect(mc, &MissionController::maxAMSLAltitudeChanged, this, &SceneModel::_scheduleRebuild);
    }
    if (GeoFenceController *gf = _pmc->geoFenceController()) {
        _connections << connect(gf, &GeoFenceController::breachReturnPointChanged, this, &SceneModel::_scheduleRebuild);
        _connections << connect(gf, &PlanElementController::dirtyChanged, this, &SceneModel::_scheduleRebuild);
        _connections << connect(gf->polygons(), &QmlObjectListModel::countChanged, this, &SceneModel::_scheduleRebuild);
        _connections << connect(gf->circles(), &QmlObjectListModel::countChanged, this, &SceneModel::_scheduleRebuild);
    }
    if (RallyPointController *rc = _pmc->rallyPointController()) {
        _connections << connect(rc, &PlanElementController::dirtyChanged, this, &SceneModel::_scheduleRebuild);
        _connections << connect(rc->points(), &QmlObjectListModel::countChanged, this, &SceneModel::_scheduleRebuild);
    }
    // The vehicle class and firmware decide splines (S-2) and landings.
    if (Vehicle *v = _pmc->controllerVehicle()) {
        _connections << connect(v, &Vehicle::firmwareTypeChanged, this, &SceneModel::_scheduleRebuild);
        _connections << connect(v, &Vehicle::vehicleTypeChanged, this, &SceneModel::_scheduleRebuild);
    }
    _connections << connect(_pmc, &PlanMasterController::managerVehicleChanged, this, &SceneModel::_scheduleRebuild);
    // F9: changes during a sync wait for its end.
    _connections << connect(_pmc, &PlanMasterController::syncInProgressChanged, this, [this]() {
        if (_pmc && !_pmc->syncInProgress()) {
            _scheduleRebuild();
        }
    });
    _connectItems();
}

void SceneModel::_connectItems()
{
    // Per-object edits that the controllers do not signal once the plan is
    // already dirty: a rally point moved or re-heighted, a fence vertex,
    // circle or inclusion changed, the breach return altitude (review G64
    // integration item 1). Re-made after every rebuild, as objects come and go.
    for (const auto &c : _itemConnections) {
        disconnect(c);
    }
    _itemConnections.clear();
    if (!_pmc) {
        return;
    }
    if (GeoFenceController *gf = _pmc->geoFenceController()) {
        _itemConnections << connect(gf->breachReturnAltitude(), &Fact::rawValueChanged, this, &SceneModel::_scheduleRebuild);
        QmlObjectListModel *polys = gf->polygons();
        for (int i = 0; polys && i < polys->count(); i++) {
            if (QGCFencePolygon *p = polys->value<QGCFencePolygon *>(i)) {
                _itemConnections << connect(p, &QGCMapPolygon::pathChanged, this, &SceneModel::_scheduleRebuild);
                _itemConnections << connect(p, &QGCFencePolygon::inclusionChanged, this, &SceneModel::_scheduleRebuild);
            }
        }
        QmlObjectListModel *circles = gf->circles();
        for (int i = 0; circles && i < circles->count(); i++) {
            if (QGCFenceCircle *c = circles->value<QGCFenceCircle *>(i)) {
                _itemConnections << connect(c, &QGCMapCircle::centerChanged, this, &SceneModel::_scheduleRebuild);
                _itemConnections << connect(c->radius(), &Fact::rawValueChanged, this, &SceneModel::_scheduleRebuild);
                _itemConnections << connect(c, &QGCFenceCircle::inclusionChanged, this, &SceneModel::_scheduleRebuild);
            }
        }
    }
    // The return height block (c) reads from a connected multirotor
    // (RTL_ALT on ArduPilot, RTL_RETURN_ALT on PX4): rebuild when it changes
    // or when the parameters arrive.
    if (Vehicle *v = _pmc->managerVehicle(); v && !v->isOfflineEditingVehicle()) {
        if (ParameterManager *pm = v->parameterManager()) {
            _itemConnections << connect(pm, &ParameterManager::parametersReadyChanged, this, &SceneModel::_scheduleRebuild);
            if (pm->parametersReady()) {
                const int comp = ParameterManager::defaultComponentId;
                for (const QString &name : {QStringLiteral("RTL_ALT"), QStringLiteral("RTL_RETURN_ALT")}) {
                    if (pm->parameterExists(comp, name)) {
                        _itemConnections << connect(pm->getParameter(comp, name), &Fact::rawValueChanged, this, &SceneModel::_scheduleRebuild);
                    }
                }
            }
        }
    }
    if (RallyPointController *rc = _pmc->rallyPointController()) {
        QmlObjectListModel *pts = rc->points();
        for (int i = 0; pts && i < pts->count(); i++) {
            if (RallyPoint *r = pts->value<RallyPoint *>(i)) {
                _itemConnections << connect(r, &RallyPoint::coordinateChanged, this, &SceneModel::_scheduleRebuild);
            }
        }
    }
}

void SceneModel::_scheduleRebuild()
{
    _pending = true;
    if (_dragging) {
        // G65: the drag fast path redraws the dragged item; everything else
        // (GC's own recalculations of the moved item) waits for the release.
        return;
    }
    if (_pmc && _pmc->syncInProgress()) {
        return;     // F9: one rebuild when the sync completes, never a partial plan
    }
    // Not restarted while running: a stream of changes still rebuilds every
    // 40 ms instead of waiting for the stream to stop (G65).
    if (_active && !_rebuildTimer.isActive()) {
        _rebuildTimer.start();
    }
}

void SceneModel::_rebuild()
{
    if (!_active || _dragging || (_pmc && _pmc->syncInProgress())) {
        return;
    }
    _pending = false;
    QElapsedTimer t;
    t.start();

    _connectItems();
    SourcePlan src = sourceFromPlan(_pmc);
    _applyHolds(src);
    _ground->setArea(Builder::bounds(src));
    Ground *g = _ground;
    _scene = Builder::build(src, [g](double lat, double lon) { return g->elevation(lat, lon); },
                            Builder::gridCellM(g->area(), g->columns(), g->rows()));   // G64 (c): rev 4 CL-1 sample spacing

    const Vec3 mn = _scene.boundsMin, mx = _scene.boundsMax;
    _center = QVector3D(float((mn.x + mx.x) / 2), float((mn.y + mx.y) / 2), float((mn.z + mx.z) / 2));
    _radius = std::max(50.0, 0.5 * std::sqrt((mx.x - mn.x) * (mx.x - mn.x) + (mx.y - mn.y) * (mx.y - mn.y) + (mx.z - mn.z) * (mx.z - mn.z)));
    _strikePlaces = strikePlacesOf(_scene);
    _hasFences = std::any_of(_scene.lines.cbegin(), _scene.lines.cend(), [](const LineSeg &l) {
        return l.kind == LineKind::FenceInclusion || l.kind == LineKind::FenceExclusion;
    });

    _upload();
    _buildMs = t.nsecsElapsed() / 1.0e6;
    _revision++;
    qCDebug(Plan3DLog) << "rebuild" << _scene.markers.size() << "markers" << _lines->segmentCount() << "segments" << _buildMs << "ms";
    emit sceneChanged();
}

QColor SceneModel::_lineColor(const LineSeg &s) const
{
    const QColor violet = unknownColor(_dark);
    switch (s.kind) {
    case LineKind::Path:
        return clearanceColor(s.clearance, _dark);
    case LineKind::Unknown:
    case LineKind::StemUnknown:
        return violet;
    case LineKind::Stem:
        return stemColor(_dark);
    case LineKind::FenceInclusion:
        return kFenceIn;
    case LineKind::FenceExclusion:
        return kFenceOut;
    case LineKind::Outline:
        return _dark ? QColor(QStringLiteral("#cbd5e1")) : QColor(QStringLiteral("#334155"));
    }
    return violet;
}

QColor SceneModel::_markerColor(const Marker &m) const
{
    if (m.visualIndex >= 0 && m.visualIndex == _selected && m.kind != MarkerKind::Home) {
        return kSelected;
    }
    if (m.kind == MarkerKind::Home) {
        return _dark ? QColor(QStringLiteral("#f8fafc")) : QColor(QStringLiteral("#0f172a"));
    }
    // CL-4: a point over unknown ground is violet; a point within 2 m of the
    // ground is a strike, except a touchdown in its contact span (CL-1a).
    if (m.clearance != Clearance::NoStrike) {
        return clearanceColor(m.clearance, _dark);
    }
    switch (m.kind) {
    case MarkerKind::Rally:        return kRally;
    case MarkerKind::BreachReturn: return kBreach;
    case MarkerKind::Roi:          return kRoi;
    default:                       return kPin;
    }
}

void SceneModel::_upload()
{
    _uploadLinesAndMarkers();
    _uploadGround();
}

void SceneModel::_uploadGround()
{
    // Ground (block b).
    _groundMesh->build(*_ground, _scene.frame, _dark);
    _groundWidth = _groundMesh->widthM();
    _groundDepth = _groundMesh->depthM();
}

void SceneModel::_uploadLinesAndMarkers()
{
    // Lines. Strike segments are drawn a second time, on top (G67 M2).
    QList<LineMesh::Seg> segs;
    QList<LineMesh::Seg> strikeSegs;
    segs.reserve(_scene.lines.size());
    for (const LineSeg &s : _scene.lines) {
        LineMesh::Seg o;
        o.a = q(s.a);
        o.b = q(s.b);
        o.color = _lineColor(s);
        switch (s.kind) {
        case LineKind::Path:
        case LineKind::Unknown:        o.widthPx = 3.5f; break;
        case LineKind::Stem:           o.widthPx = 1.5f; break;
        case LineKind::StemUnknown:    o.widthPx = 1.5f; o.dashed = true; break;
        case LineKind::FenceInclusion:
        case LineKind::FenceExclusion: o.widthPx = 2.0f; break;
        case LineKind::Outline:        o.widthPx = 1.5f; break;
        }
        if (s.kind == LineKind::Path && s.visualIndex >= 0 && s.visualIndex == _selected) {
            o.widthPx = 5.0f;
        }
        segs.append(o);
        if (s.kind == LineKind::Path && s.clearance == Clearance::Strike) {
            strikeSegs.append(o);
        }
    }
    // Markers.
    QList<MarkerInstances::Instance> inst;
    QList<MarkerInstances::Instance> strikeInst;
    inst.reserve(_scene.markers.size());
    for (const Marker &m : _scene.markers) {
        MarkerInstances::Instance i;
        i.pos = q(m.pos);
        i.color = _markerColor(m);
        switch (m.kind) {
        case MarkerKind::Home:          i.sizePx = 16; break;
        case MarkerKind::PatternPoint:  i.sizePx = 6;  break;
        default:                        i.sizePx = 11; break;
        }
        if (m.clearance == Clearance::Strike) {
            // On top, red and at its own size: a selected strike shows red
            // inside the larger selected marker.
            MarkerInstances::Instance r = i;
            r.color = strikeColor();
            strikeInst.append(r);
        }
        if (m.visualIndex >= 0 && m.visualIndex == _selected && m.kind != MarkerKind::Home) {
            i.sizePx *= 1.5f;
        }
        inst.append(i);
    }

    // G65: heading (E-7) and gimbal (E-8) handles of the selected item.
    _headingTipValid = _gimbalTipValid = false;
    const Marker *sel = _selected > 0 ? _markerFor(_selected) : nullptr;
    if (sel && !_dragging) {
        const QVector3D p = q(sel->pos);
        const double L = _handleLength();
        const double h = _editor->heading(_selected);
        const QColor handleColor(QStringLiteral("#38bdf8"));
        if (std::isfinite(h)) {
            const double az = qDegreesToRadians(h);
            _headingTip = p + QVector3D(float(std::sin(az) * L), 0, float(-std::cos(az) * L));
            _headingTipValid = true;
            LineMesh::Seg s;
            s.a = p;
            s.b = _headingTip;
            s.color = handleColor;
            s.widthPx = 2.5f;
            segs.append(s);
            inst.append(MarkerInstances::Instance{_headingTip, handleColor, 9.0f});
        }
        const double pitch = _editor->gimbalPitch(_selected);
        if (std::isfinite(pitch)) {
            // E-8: the yaw is relative to the vehicle heading at the point.
            const double pr = qDegreesToRadians(pitch);
            const double gl = 0.8 * L;
            // Light: darker and wider than the light stems (#475569, 1.5 px), so the
            // handle stands apart from the stem it lies along. Dark is unchanged.
            const QColor gimbalColor = _dark ? QColor(QStringLiteral("#e2e8f0")) : QColor(QStringLiteral("#1e293b"));
            const float gimbalRayPx = _dark ? 2.0f : 2.5f;
            const float gimbalAxisPx = _dark ? 1.5f : 2.5f;
            if (Editor::kGimbalYawEditing && std::isfinite(h)) {
                // Heading known and the E-8 SITL gate open: one ray, pitch and yaw.
                const double az = qDegreesToRadians(h + _editor->gimbalYaw(_selected));
                _gimbalTip = p + QVector3D(float(std::cos(pr) * std::sin(az) * gl), float(std::sin(pr) * gl), float(-std::cos(pr) * std::cos(az) * gl));
                LineMesh::Seg s;
                s.a = p;
                s.b = _gimbalTip;
                s.color = gimbalColor;
                s.widthPx = gimbalRayPx;
                segs.append(s);
            } else {
                // No heading set here, or yaw not yet checked in SITL (F10):
                // pitch only, with no direction around the vertical
                // (E-8). The ring is every direction at that pitch. No ray. The
                // handle is on the vertical axis below the point, never on it
                // (F6): its depth runs from 0.25 to 1 handle length as the pitch
                // goes from 0 to -90.
                const QVector3D c = p + QVector3D(0, float(std::sin(pr) * gl), 0);
                const double r = std::max(0.08 * gl, std::cos(pr) * gl);
                constexpr int kRing = 32;
                for (int k = 0; k < kRing; k++) {
                    const double a0 = 2.0 * M_PI * k / kRing, a1 = 2.0 * M_PI * (k + 1) / kRing;
                    LineMesh::Seg s;
                    s.a = c + QVector3D(float(r * std::cos(a0)), 0, float(r * std::sin(a0)));
                    s.b = c + QVector3D(float(r * std::cos(a1)), 0, float(r * std::sin(a1)));
                    s.color = gimbalColor;
                    s.widthPx = gimbalAxisPx;
                    s.dashed = true;
                    segs.append(s);
                }
                const double down = std::clamp(-std::sin(pr), 0.0, 1.0);
                _gimbalTip = p - QVector3D(0, float((0.25 + 0.75 * down) * gl), 0);
                LineMesh::Seg axis;
                axis.a = p;
                axis.b = _gimbalTip;
                axis.color = gimbalColor;
                axis.widthPx = gimbalAxisPx;
                segs.append(axis);
            }
            _gimbalTipValid = true;
            inst.append(MarkerInstances::Instance{_gimbalTip, gimbalColor, 8.0f});
        }
    }
    _lines->setSegments(segs);
    _strikeLines->setSegments(strikeSegs);
    _markers->setInstances(inst);
    _strikeMarkers->setInstances(strikeInst);
}

double SceneModel::_handleLength() const
{
    return std::clamp(double(_radius) * 0.06, 12.0, 150.0);
}

QVariantList SceneModel::labels(QObject *camera, qreal width, qreal height, qreal mouseX, qreal mouseY, int maxNear) const
{
    QVariantList out;
    if (!camera || width <= 0 || height <= 0) {
        return out;
    }
    const QVector3D camPos = camera->property("scenePosition").value<QVector3D>();
    const QVector3D camFwd = camera->property("forward").value<QVector3D>();
    struct Cand { qreal x, y, d2; int i; };
    QList<Cand> near;
    QVariantList selected;
    for (int i = 0; i < _scene.markers.size(); i++) {
        const Marker &m = _scene.markers[i];
        if (m.label.isEmpty()) {
            continue;
        }
        const bool isSel = m.visualIndex >= 0 && m.visualIndex == _selected && m.kind != MarkerKind::Home;
        const QVector3D p = q(m.pos);
        if (QVector3D::dotProduct(p - camPos, camFwd) <= 0) {
            continue;   // behind the camera
        }
        QVector3D v;
        QMetaObject::invokeMethod(camera, "mapToViewport", Qt::DirectConnection, Q_RETURN_ARG(QVector3D, v), Q_ARG(QVector3D, p));
        const qreal x = v.x() * width, y = v.y() * height;
        if (x < 0 || y < 0 || x > width || y > height) {
            continue;
        }
        if (isSel && selected.isEmpty()) {
            selected.append(QVariantMap{{"x", x}, {"y", y}, {"text", markerLabelText(m)}, {"selected", true}});
            continue;
        }
        if (mouseX >= 0 && m.kind != MarkerKind::PatternPoint) {
            const qreal d2 = (x - mouseX) * (x - mouseX) + (y - mouseY) * (y - mouseY);
            if (d2 < 80.0 * 80.0) {
                near.append({x, y, d2, i});
            }
        }
    }
    std::sort(near.begin(), near.end(), [](const Cand &a, const Cand &b) { return a.d2 < b.d2; });
    struct Lab { qreal x, y; QString text; bool sel; };
    QList<Lab> labs;
    for (int k = 0; k < near.size() && k < maxNear; k++) {
        const Marker &m = _scene.markers[near[k].i];
        labs.append({near[k].x, near[k].y, markerLabelText(m), false});
    }
    for (const QVariant &v : selected) {
        const QVariantMap m = v.toMap();
        labs.append({m.value("x").toReal(), m.value("y").toReal(), m.value("text").toString(), true});
    }
    // Keep labels from covering each other: walk down the screen and push
    // any label that would overlap the one above it.
    std::sort(labs.begin(), labs.end(), [](const Lab &a, const Lab &b) { return a.y < b.y; });
    constexpr qreal kRowPx = 18.0;
    for (int i = 1; i < labs.size(); i++) {
        if (labs[i].y - labs[i - 1].y < kRowPx) {
            labs[i].y = labs[i - 1].y + kRowPx;
        }
    }
    // The selected item last, so it draws on top.
    std::stable_partition(labs.begin(), labs.end(), [](const Lab &l) { return !l.sel; });
    for (const Lab &l : labs) {
        out.append(QVariantMap{{"x", l.x}, {"y", l.y}, {"text", l.text}, {"selected", l.sel}});
    }
    return out;
}

int SceneModel::pick(QObject *camera, qreal width, qreal height, qreal x, qreal y) const
{
    if (!camera || width <= 0 || height <= 0) {
        return -1;
    }
    const QVector3D camPos = camera->property("scenePosition").value<QVector3D>();
    const QVector3D camFwd = camera->property("forward").value<QVector3D>();
    int best = -1;
    qreal bestD2 = 24.0 * 24.0;
    for (const Marker &m : _scene.markers) {
        if (m.visualIndex < 0) {
            continue;
        }
        const QVector3D p = q(m.pos);
        if (QVector3D::dotProduct(p - camPos, camFwd) <= 0) {
            continue;
        }
        QVector3D v;
        QMetaObject::invokeMethod(camera, "mapToViewport", Qt::DirectConnection, Q_RETURN_ARG(QVector3D, v), Q_ARG(QVector3D, p));
        const qreal d2 = (v.x() * width - x) * (v.x() * width - x) + (v.y() * height - y) * (v.y() * height - y);
        if (d2 < bestD2) {
            bestD2 = d2;
            best = m.visualIndex;
        }
    }
    return best;
}

// ---------------------------------------------------------------- G65 editing

QObject *SceneModel::editorObject() const
{
    return _editor;
}

const Marker *SceneModel::_markerFor(int vi) const
{
    if (vi <= 0) {
        return nullptr;
    }
    for (const Marker &m : _scene.markers) {
        if (m.visualIndex == vi && (m.kind == MarkerKind::Waypoint || m.kind == MarkerKind::PatternPoint)) {
            return &m;
        }
    }
    return nullptr;
}

bool SceneModel::_ray(QObject *camera, qreal width, qreal height, qreal x, qreal y, QVector3D &origin, QVector3D &dir) const
{
    if (!camera || width <= 0 || height <= 0) {
        return false;
    }
    QVector3D a, b;
    // z is the distance from the near clip plane (QQuick3DCamera::mapFromViewport).
    QMetaObject::invokeMethod(camera, "mapFromViewport", Qt::DirectConnection, Q_RETURN_ARG(QVector3D, a), Q_ARG(QVector3D, QVector3D(float(x / width), float(y / height), 0.0f)));
    QMetaObject::invokeMethod(camera, "mapFromViewport", Qt::DirectConnection, Q_RETURN_ARG(QVector3D, b), Q_ARG(QVector3D, QVector3D(float(x / width), float(y / height), 1.0f)));
    dir = b - a;
    if (dir.lengthSquared() < 1e-12f) {
        return false;
    }
    dir.normalize();
    origin = a;
    return true;
}

namespace {

/// Where a ray meets the vertical plane through `p` that faces the ray; false when it does not.
bool hitVertical(const QVector3D &o, const QVector3D &d, const QVector3D &p, QVector3D &hit)
{
    QVector3D n(d.x(), 0, d.z());
    if (n.lengthSquared() < 1e-6f) {
        return false;   // looking straight down: no height from this ray
    }
    n.normalize();
    const float den = QVector3D::dotProduct(d, n);
    if (std::abs(den) < 1e-6f) {
        return false;
    }
    const float t = QVector3D::dotProduct(p - o, n) / den;
    if (t <= 0) {
        return false;
    }
    hit = o + d * t;
    return true;
}

/// Where a ray meets the horizontal plane y = planeY within `maxT`; false otherwise.
bool hitHorizontal(const QVector3D &o, const QVector3D &d, float planeY, float maxT, QVector3D &hit)
{
    if (std::abs(d.y()) < 1e-6f) {
        return false;
    }
    const float t = (planeY - o.y()) / d.y();
    if (t <= 0 || t > maxT) {
        return false;   // behind the camera, or toward the horizon
    }
    hit = o + d * t;
    return true;
}

} // namespace

bool SceneModel::beginDragAt(QObject *camera, qreal width, qreal height, qreal x, qreal y, int vi, bool heightDrag)
{
    if (!_editor->checkEdit(vi, heightDrag)) {
        return false;
    }
    const Marker *m = _markerFor(vi);
    QVector3D o, d, hit;
    if (!m || !_ray(camera, width, height, x, y, o, d)) {
        return false;
    }
    const QVector3D p(float(m->pos.x), float(m->pos.y), float(m->pos.z));
    if (heightDrag) {
        if (!hitVertical(o, d, p, hit)) {
            return false;
        }
        _grabDy = p.y() - hit.y();
    } else {
        if (!hitHorizontal(o, d, p.y(), 1.0e7f, hit)) {
            return false;
        }
        _grabDx = p.x() - hit.x();
        _grabDz = p.z() - hit.z();
        _dragPlaneY = p.y();
    }
    _dragHeight = heightDrag;
    return _editor->beginDrag(vi);
}

void SceneModel::dragTo(QObject *camera, qreal width, qreal height, qreal x, qreal y)
{
    if (!_dragging || _dragVi < 0) {
        return;
    }
    QVector3D o, d, hit;
    if (!_ray(camera, width, height, x, y, o, d)) {
        return;
    }
    const LocalFrame &F = _scene.frame;
    if (_dragHeight) {
        const Marker *m = _markerFor(_dragVi);
        if (!m || !hitVertical(o, d, QVector3D(float(m->pos.x), float(m->pos.y), float(m->pos.z)), hit)) {
            return;
        }
        dragToAmsl(double(hit.y()) + _grabDy + F.alt0);
    } else {
        if (!hitHorizontal(o, d, float(_dragPlaneY), 20.0f * std::max<float>(1000.0f, float(_radius)), hit)) {
            return;
        }
        const double e = double(hit.x()) + _grabDx;
        const double north = -(double(hit.z()) + _grabDz);
        dragToLatLon(F.latFromNorth(north), F.lonFromEast(e));
    }
}

void SceneModel::dragToLatLon(double lat, double lon)
{
    if (!_dragging || _dragVi < 0 || !std::isfinite(lat) || !std::isfinite(lon)) {
        return;
    }
    if (!_editor->checkEdit(_dragVi, false)) {
        return;
    }
    _pendLat = Editor::snapDegrees(lat);
    _pendLon = Editor::snapDegrees(lon);
    _pendingMove = true;
    _scheduleDragFrame();
}

void SceneModel::dragToAmsl(double amsl)
{
    if (!_dragging || _dragVi < 0 || !std::isfinite(amsl)) {
        return;
    }
    if (!_editor->checkEdit(_dragVi, true)) {
        return;
    }
    _pendAmsl = amsl;
    _pendingHeight = true;
    if (Hold *h = _holdFor(_editor->dragItem())) {
        h->amsl = amsl;     // E-2: a mode 3 point is drawn where it is dragged to
    }
    _scheduleDragFrame();
}

void SceneModel::_scheduleDragFrame()
{
    // Drawn at once from the dragged position (at most one rebuild per frame);
    // written to GC's item at most every kCommitMs (G65 fps): each write makes
    // GC recalculate the whole mission, which on wp300 costs far more than a frame.
    _dragDirty = true;
    if (!_dragTimer.isActive()) {
        _dragRebuild();
    }
    if (!_commitTimer.isActive()) {
        _commitTimer.start();
    }
}

void SceneModel::_commitDrag()
{
    // The same editor writes a 2D drag makes (E-1); only their rate differs.
    const int vi = _dragVi;
    if (vi < 0) {
        _pendingMove = _pendingHeight = false;
        return;
    }
    if (_pendingMove) {
        _pendingMove = false;
        _editor->moveTo(vi, _pendLat, _pendLon);
    }
    if (_pendingHeight && _dragVi == vi) {
        _pendingHeight = false;
        _editor->setHeightAmsl(vi, _pendAmsl);
    }
    _dragDirty = true;      // draw what the item now holds
    if (_dragging && !_dragTimer.isActive()) {
        _dragRebuild();
    }
}

void SceneModel::endDrag()
{
    _commitTimer.stop();
    _commitDrag();          // the final position is always written
    _editor->endDrag();
}

double SceneModel::markerAmsl(int vi) const
{
    const Marker *m = _markerFor(vi);
    return m ? m->amsl : kNaN;
}

void SceneModel::_onEditorDragging()
{
    const int vi = _editor->dragVisualIndex();
    if (_dragging && vi != _dragVi) {
        // Ended (released, a sync, a load, a replaced plan) or another item:
        // drop the cached plan, then the authoritative full rebuild.
        _dragging = false;
        _dragTimer.stop();
        // A drag ended by a sync, a load or a replaced plan: an unwritten
        // position is dropped, never written to whatever item is there now (M2).
        _commitTimer.stop();
        _pendingMove = _pendingHeight = false;
        _dragDirty = false;
        _dragVi = -1;
        _dragSource = SourcePlan();
        // F4: a held height stays until GC answers, at most 10 s from now.
        const qint64 until = QDateTime::currentMSecsSinceEpoch() + 10000;
        for (Hold &h : _holds) {
            if (h.untilMs == 0) {
                h.untilMs = until;
            }
        }
        if (!_holds.isEmpty()) {
            _holdTimer.start(10100);
        }
        emit draggingChanged();
        _pending = true;
        _rebuild();
    }
    if (vi >= 0 && !_dragging) {
        _rebuildTimer.stop();
        if (_pending && _active) {
            _rebuild();         // start from the current plan
        }
        _dragging = true;
        _dragVi = vi;
        _dragSource = sourceFromPlan(_pmc);
        SimpleMissionItem *item = _editor->dragItem();
        if (const Marker *m = _markerFor(vi); item && m && std::isfinite(m->amsl)) {
            if (Hold *h = _holdFor(item)) {
                h->amsl = m->amsl;
                h->untilMs = 0;
            } else {
                _holds.append(Hold{item, m->amsl, 0});
            }
            // Redraw as soon as GC answers its terrain query for this point.
            connect(item->amslAltAboveTerrain(), &Fact::rawValueChanged, this, &SceneModel::_scheduleRebuild, Qt::UniqueConnection);
        }
        if (_pmc && _pmc->missionController()) {
            if (SimpleMissionItem *item = _editor->simpleItem(vi)) {
                _pmc->missionController()->setCurrentPlanViewSeqNum(item->sequenceNumber(), false);
            }
        }
        emit draggingChanged();
    }
}

void SceneModel::_onItemEdited(int vi, double drawnAmsl)
{
    if (!_dragging || vi != _dragVi) {
        // Edits outside a point drag (spline, heading, gimbal) rebuild through
        // GC's own signals; the handles, read from the model, redraw now.
        _uploadLinesAndMarkers();
        return;
    }
    if (std::isfinite(drawnAmsl)) {
        if (Hold *h = _holdFor(_editor->dragItem())) {
            h->amsl = drawnAmsl;
        }
    }
    _dragDirty = true;
    if (!_dragTimer.isActive()) {
        _dragRebuild();         // at once; the timer then holds the next one back
    }
}

SceneModel::Hold *SceneModel::_holdFor(SimpleMissionItem *item)
{
    for (Hold &h : _holds) {
        if (item && h.item.data() == item) {
            return &h;
        }
    }
    return nullptr;
}

void SceneModel::_applyHolds(SourcePlan &plan)
{
    // F4: a mode 3 point whose AMSL GC has not answered yet keeps its last
    // height and is drawn unknown (CL-2). The hold ends only when GC answers or
    // the item leaves mode 3 or the plan. After 10 s without an answer it is
    // marked expired and stays unknown ("terrain height not available"); it
    // never turns colored on its own (F4).
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QmlObjectListModel *vis = (_pmc && _pmc->missionController()) ? _pmc->missionController()->visualItems() : nullptr;
    for (int i = _holds.size() - 1; i >= 0; i--) {
        const Hold &h = _holds[i];
        SimpleMissionItem *item = h.item.data();
        const int vi = (item && vis) ? vis->indexOf(item) : -1;
        const bool pending = item && vi > 0
                             && item->altitudeMode() == QGroundControlQmlGlobal::AltitudeModeCalcAboveTerrain
                             && !std::isfinite(item->missionItem().param7());
        if (!pending) {
            _holds.removeAt(i);
            continue;
        }
        if (h.untilMs > 0 && now >= h.untilMs) {
            _holds[i].expired = true;
        }
        for (SourceItem &si : plan.items) {
            if (si.visualIndex == vi && si.seq == item->sequenceNumber()) {
                si.params[6] = h.amsl;
                si.heightHeld = true;
                si.heightUnavailable = _holds[i].expired;
                break;
            }
        }
    }
}

void SceneModel::_patchDragSource()
{
    // M2: the item the drag is bound to, never whatever is at the index now.
    SimpleMissionItem *item = _editor->dragItem();
    if (!item || _editor->simpleItem(_dragVi) != item) {
        return;
    }
    const int seq = item->sequenceNumber();
    const MissionItem &mi = item->missionItem();
    for (SourceItem &si : _dragSource.items) {
        if (si.visualIndex != _dragVi || si.seq != seq) {
            continue;
        }
        si.command = int(mi.command());
        si.frame = int(mi.frame());
        si.params[0] = mi.param1();
        si.params[1] = mi.param2();
        si.params[2] = mi.param3();
        si.params[3] = mi.param4();
        si.params[4] = mi.param5();
        si.params[5] = mi.param6();
        si.params[6] = mi.param7();
        si.altitudeMode = item->specifiesAltitude() ? int(item->altitudeMode()) : 0;
        si.heightHeld = false;
        if (_pendingMove) {
            si.params[4] = _pendLat;
            si.params[5] = _pendLon;
        }
        if (_pendingHeight && si.altitudeMode != 0) {
            // Drawn where it is dragged to, in the item's own datum (E-2).
            const double g = _ground->elevation(si.params[4], si.params[5]);
            switch (si.altitudeMode) {
            case 1: si.params[6] = _pendAmsl - _scene.launchRef; break;     // frame 3: above R
            case 2: si.params[6] = _pendAmsl; break;                        // frame 0: AMSL
            case 3: si.params[6] = _pendAmsl; break;                        // frame 0: AMSL (held below)
            case 4: si.params[6] = std::isfinite(g) ? _pendAmsl - g : si.params[6]; break;   // frame 10
            default: break;
            }
        }
        if (si.altitudeMode == int(QGroundControlQmlGlobal::AltitudeModeCalcAboveTerrain)) {
            // E-2: while GC's terrain query for the moved point is pending,
            // the point keeps its last drawn height, drawn unknown (F4, CL-2).
            // A position or height not yet written, or written but not yet
            // answered by GC's terrain query, is held too.
            Hold *h = _holdFor(item);
            const bool unanswered = _pendingMove || _pendingHeight || !std::isfinite(si.params[6]);
            if (!unanswered) {
                if (h) {
                    h->amsl = si.params[6];
                }
            } else if (h) {
                si.params[6] = h->amsl;
                si.heightHeld = true;
            }
        }
        break;
    }
}

void SceneModel::_dragRebuild()
{
    if (!_dragging || !_dragDirty) {
        return;
    }
    _dragDirty = false;
    QElapsedTimer t;
    t.start();
    _patchDragSource();
    Ground *g = _ground;
    // The ground already loaded: no new area and no new ground mesh during a drag.
    _scene = Builder::build(_dragSource, [g](double lat, double lon) { return g->elevation(lat, lon); },
                            Builder::gridCellM(g->area(), g->columns(), g->rows()));
    _strikePlaces = strikePlacesOf(_scene);     // the legend names strikes live (G67 M2)
    _uploadLinesAndMarkers();
    _buildMs = t.nsecsElapsed() / 1.0e6;
    _revision++;
    QElapsedTimer e;
    e.start();
    emit sceneChanged();
    _emitMs = e.nsecsElapsed() / 1.0e6;
    _dragTimer.start();     // at most one rebuild per frame interval
}

QString SceneModel::pickHandle(QObject *camera, qreal width, qreal height, qreal x, qreal y) const
{
    if (!camera || width <= 0 || height <= 0) {
        return QString();
    }
    auto closeTo = [&](const QVector3D &tip) {
        QVector3D v;
        QMetaObject::invokeMethod(camera, "mapToViewport", Qt::DirectConnection, Q_RETURN_ARG(QVector3D, v), Q_ARG(QVector3D, tip));
        const qreal dx = v.x() * width - x, dy = v.y() * height - y;
        return dx * dx + dy * dy < 14.0 * 14.0;
    };
    if (_headingTipValid && closeTo(_headingTip)) {
        return QStringLiteral("heading");
    }
    if (_gimbalTipValid && closeTo(_gimbalTip)) {
        return QStringLiteral("gimbal");
    }
    return QString();
}

void SceneModel::dragHandleTo(QObject *camera, qreal width, qreal height, qreal x, qreal y, const QString &handle, bool yaw)
{
    const Marker *m = _selected > 0 ? _markerFor(_selected) : nullptr;
    QVector3D o, d, hit;
    if (!m || !_ray(camera, width, height, x, y, o, d)) {
        return;
    }
    const QVector3D p = q(m->pos);
    const auto azimuthTo = [&p](const QVector3D &h) {
        return qRadiansToDegrees(std::atan2(double(h.x() - p.x()), double(-(h.z() - p.z()))));
    };
    if (handle == QLatin1String("heading") || (handle == QLatin1String("gimbal") && yaw)) {
        if (!hitHorizontal(o, d, p.y(), 20.0f * std::max<float>(1000.0f, float(_radius)), hit)) {
            return;
        }
        const double az = azimuthTo(hit);
        if (handle == QLatin1String("heading")) {
            _editor->setHeading(_selected, az);
        } else {
            // E-8: the gimbal yaw is relative to the vehicle heading at the point.
            const double rel = std::fmod(az - _editor->heading(_selected) + 540.0, 360.0) - 180.0;
            _editor->setGimbalYaw(_selected, rel);
        }
    } else if (handle == QLatin1String("gimbal")) {
        if (!hitVertical(o, d, p, hit)) {
            return;
        }
        const double gl = 0.8 * _handleLength();
        if (Editor::kGimbalYawEditing && std::isfinite(_editor->heading(_selected))) {
            const double horiz = std::hypot(double(hit.x() - p.x()), double(hit.z() - p.z()));
            _editor->setGimbalPitch(_selected, qRadiansToDegrees(std::atan2(double(hit.y() - p.y()), std::max(horiz, 0.01 * gl))));
        } else {
            // The handle moves on the vertical axis below the point (F6):
            // depth (0.25 + 0.75 sin(-pitch)) handle lengths.
            const double down = std::clamp((double(p.y() - hit.y()) / gl - 0.25) / 0.75, 0.0, 1.0);
            _editor->setGimbalPitch(_selected, -qRadiansToDegrees(std::asin(down)));
        }
    }
}

QColor SceneModel::stateColor(const QString &state) const
{
    if (state == QLatin1String("strike")) {
        return strikeColor();
    }
    if (state == QLatin1String("unknown")) {
        return unknownColor(_dark);
    }
    if (state == QLatin1String("groundContact")) {
        return groundContactColor(_dark);
    }
    if (state == QLatin1String("fenceInclusion")) {
        return kFenceIn;
    }
    if (state == QLatin1String("fenceExclusion")) {
        return kFenceOut;
    }
    if (state == QLatin1String("stem")) {
        return stemColor(_dark);
    }
    return noStrikeColor();
}

QString markerLabelText(const Marker &m)
{
    QString text = m.label;
    if (m.kind == MarkerKind::BreachReturn) {
        // A-9 (rev 8): its label already says "height not used".
        if (!std::isfinite(m.ground)) {
            text += QCoreApplication::translate("Plan3D", "  unknown ground");
        }
        return text;
    }
    // G65 (F4, CL-2): a held mode 3 height is not a known height.
    if (m.terrainUnavailable) {
        return text + QCoreApplication::translate("Plan3D", "  terrain height not available");
    }
    if (m.heightPending) {
        return text + QCoreApplication::translate("Plan3D", "  height pending GC's terrain query");
    }
    if (!std::isfinite(m.ground)) {
        return text + QCoreApplication::translate("Plan3D", "  unknown ground: not checked");
    }
    if (!std::isfinite(m.amsl)) {
        return text + QCoreApplication::translate("Plan3D", "  unknown height: not checked");
    }
    text += QStringLiteral("  ") + heightText(m.amsl - m.ground);
    // CL-5 words for the state, so a strike is never shown by red alone, and
    // a selected strike (drawn in the selected color) still says so.
    if (m.kind == MarkerKind::Waypoint || m.kind == MarkerKind::PatternPoint || m.kind == MarkerKind::Rally) {
        switch (m.clearance) {
        case Clearance::Strike:
            text += QCoreApplication::translate("Plan3D", ": meets the terrain or within 2 m of the ground");
            break;
        case Clearance::GroundContact:
            text += QCoreApplication::translate("Plan3D", ": ground contact (takeoff or landing)");
            break;
        case Clearance::Unknown:
            text += QCoreApplication::translate("Plan3D", ": not checked");
            break;
        case Clearance::NoStrike:
            break;
        }
    }
    return text;
}

QStringList strikePlacesOf(const Scene &sc)
{
    const auto tr = [](const char *s) { return QCoreApplication::translate("Plan3D", s); };

    // Route markers in route order, grouped by visual item: the first and
    // last sequence numbers, and the route point before the item.
    struct Item {
        int  firstSeq = -1;
        int  lastSeq = -1;
        int  prevSeq = -1;      ///< -1: no route point before it; 0: home
        bool pattern = false;
        int  command = 0;
    };
    QMap<int, Item> items;
    int lastSeq = -1;
    for (const Marker &m : sc.markers) {
        if (!isRouteMarker(m.kind) || m.visualIndex < 0) {
            continue;
        }
        auto it = items.find(m.visualIndex);
        if (it == items.end()) {
            Item v;
            v.firstSeq = v.lastSeq = m.seq;
            v.prevSeq = lastSeq;
            v.pattern = m.kind == MarkerKind::PatternPoint;
            v.command = m.command;
            items.insert(m.visualIndex, v);
        } else {
            it->lastSeq = m.seq;
        }
        lastSeq = m.seq;
    }

    QSet<int> legStrike;
    bool otherLegStrike = false;
    for (const LineSeg &l : sc.lines) {
        if (l.kind == LineKind::Path && l.clearance == Clearance::Strike) {
            if (items.contains(l.visualIndex)) {
                legStrike.insert(l.visualIndex);
            } else {
                otherLegStrike = true;
            }
        }
    }
    QMap<int, const Marker *> markerStrike;     // simple items, by visual index
    QSet<int> patternMarkerStrike;
    for (const Marker &m : sc.markers) {
        if (m.clearance != Clearance::Strike || m.visualIndex < 0 || !items.contains(m.visualIndex)) {
            continue;
        }
        if (m.kind == MarkerKind::PatternPoint) {
            patternMarkerStrike.insert(m.visualIndex);
        } else if (m.kind == MarkerKind::Waypoint && !markerStrike.contains(m.visualIndex)) {
            markerStrike.insert(m.visualIndex, &m);
        }
    }
    const auto belowNote = [](const Marker &m) {
        if (std::isfinite(m.amsl) && std::isfinite(m.ground) && m.amsl < m.ground) {
            return QCoreApplication::translate("Plan3D", " (%1 m below the loaded ground)").arg(m.ground - m.amsl, 0, 'f', 1);
        }
        return QString();
    };

    QStringList out;
    for (auto it = items.cbegin(); it != items.cend(); ++it) {
        const int vi = it.key();
        const Item &v = it.value();
        if (v.pattern) {
            if (legStrike.contains(vi) || patternMarkerStrike.contains(vi)) {
                out << (v.firstSeq == v.lastSeq ? tr("the pattern at item %1").arg(v.firstSeq)
                                                : tr("the pattern of items %1 to %2").arg(v.firstSeq).arg(v.lastSeq));
            }
            continue;
        }
        if (const Marker *m = markerStrike.value(vi, nullptr)) {
            out << tr("item %1").arg(m->seq) + belowNote(*m);
        }
        if (legStrike.contains(vi)) {
            const bool loiter = Builder::isLoiter(v.command);
            if (v.prevSeq < 0) {
                out << (loiter ? tr("the circle at %1").arg(v.firstSeq) : tr("leg to %1").arg(v.firstSeq));
            } else if (v.prevSeq == 0) {
                out << (loiter ? tr("leg from home to %1 or the circle at %1").arg(v.firstSeq)
                               : tr("leg from home to %1").arg(v.firstSeq));
            } else {
                out << (loiter ? tr("leg %1 to %2 or the circle at %2").arg(v.prevSeq).arg(v.firstSeq)
                               : tr("leg %1 to %2").arg(v.prevSeq).arg(v.firstSeq));
            }
        }
    }
    if (otherLegStrike) {
        out << tr("part of the route");
    }
    for (const Marker &m : sc.markers) {
        if (m.clearance == Clearance::Strike && m.kind == MarkerKind::Rally) {
            out << m.label + belowNote(m);
        }
    }
    // A strike that none of the above names is still named.
    if (out.isEmpty() && sc.stats.samplesStrike > 0) {
        out << tr("part of the route");
    }
    out.removeDuplicates();
    return out;
}

void registerQmlTypes()
{
    qmlRegisterType<SceneModel>("RotorLab.Plan3D", 1, 0, "Plan3DSceneModel");
    qmlRegisterUncreatableType<LineMesh>("RotorLab.Plan3D", 1, 0, "Plan3DLineMesh", QStringLiteral("owned by Plan3DSceneModel"));
    qmlRegisterUncreatableType<MarkerInstances>("RotorLab.Plan3D", 1, 0, "Plan3DMarkerInstances", QStringLiteral("owned by Plan3DSceneModel"));
    qmlRegisterUncreatableType<GroundMesh>("RotorLab.Plan3D", 1, 0, "Plan3DGroundMesh", QStringLiteral("owned by Plan3DSceneModel"));
}

} // namespace Plan3D
