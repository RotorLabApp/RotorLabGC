/****************************************************************************
 *
 * RotorLab GC Plan view 3D editing (G65, MG3; contract section 4a).
 *
 ****************************************************************************/

#include "Plan3DEditor.h"

#include "Fact.h"
#include "MissionController.h"
#include "PlanMasterController.h"
#include "QmlObjectListModel.h"
#include "SimpleMissionItem.h"
#include "Vehicle.h"
#include "CameraSection.h"

#include <QtCore/QLoggingCategory>

#include <cmath>

Q_LOGGING_CATEGORY(Plan3DEditLog, "rotorlab.plan3d.edit")

namespace Plan3D {

namespace {

/// E-3 and M4: round through the decimal text 2D's fields hold, so 3D writes
/// exactly the double 2D gets from typing that text.
double viaText(double x, int decimals)
{
    return std::isfinite(x) ? QString::number(x, 'f', decimals).toDouble() : x;
}

/// E-4: simple items that may be moved and re-heighted.
bool isMovableCommand(int command)
{
    switch (command) {
    case 16:    // NAV_WAYPOINT
    case 82:    // NAV_SPLINE_WAYPOINT
    case 17:    // NAV_LOITER_UNLIM
    case 18:    // NAV_LOITER_TURNS
    case 19:    // NAV_LOITER_TIME
    case 31:    // NAV_LOITER_TO_ALT
        return true;
    default:
        return false;
    }
}

/// E-4: takeoff and land: the height only.
bool isHeightOnlyCommand(int command)
{
    return command == 22 || command == 84 || command == 21 || command == 85;
}

} // namespace

Editor::Editor(QObject *parent)
    : QObject(parent)
{
}

void Editor::setPlanMasterController(PlanMasterController *pmc)
{
    if (_pmc == pmc) {
        return;
    }
    endDrag();
    _pmc = pmc;
    _connectPlan();
}

void Editor::_connectPlan()
{
    for (const auto &c : _connections) {
        disconnect(c);
    }
    _connections.clear();
    if (!_pmc) {
        return;
    }
    // M2: a sync, a load or a replaced plan ends any drag (and the scene drops
    // its cached plan with it).
    _connections << connect(_pmc, &PlanMasterController::syncInProgressChanged, this, [this]() {
        if (_pmc && _pmc->syncInProgress()) {
            endDrag();
        }
    });
    if (MissionController *mc = _pmc->missionController()) {
        _connections << connect(mc, &MissionController::visualItemsChanged, this, &Editor::endDrag);
    }
}

SimpleMissionItem *Editor::dragItem() const
{
    return _dragItem.data();
}

bool Editor::_dragTargetOk(int vi)
{
    if (_dragVi < 0) {
        return true;
    }
    if (vi == _dragVi && _dragItem && simpleItem(vi) == _dragItem.data()) {
        return true;
    }
    endDrag();
    _setMessage(tr("The plan changed during the drag; the drag was ended and nothing was written."));
    return false;
}

void Editor::setReferences(GroundFn ground, std::function<double()> launchRef)
{
    _ground = std::move(ground);
    _launchRef = std::move(launchRef);
}

void Editor::setMissionLayer(bool missionLayer)
{
    if (_missionLayer == missionLayer) {
        return;
    }
    _missionLayer = missionLayer;
    if (!_missionLayer) {
        endDrag();
    }
    emit missionLayerChanged();
}

double Editor::snapAltitude(double metres)
{
    return viaText(metres, 1);
}

double Editor::snapDegrees(double degrees)
{
    return viaText(degrees, 7);
}

double Editor::snapHeading(double degrees)
{
    if (!std::isfinite(degrees)) {
        return degrees;
    }
    double h = viaText(std::fmod(degrees, 360.0), 1);
    if (h < 0) {
        h = viaText(h + 360.0, 1);
    }
    if (h >= 360.0) {
        h = viaText(h - 360.0, 1);
    }
    return h == 0.0 ? 0.0 : h;      // F5: never -0, which saves as "-0"
}

double Editor::snapGimbal(double degrees, double min, double max)
{
    if (!std::isfinite(degrees)) {
        return degrees;
    }
    double g = viaText(degrees, 1);
    if (std::isfinite(min)) {
        g = std::max(g, min);
    }
    if (std::isfinite(max)) {
        g = std::min(g, max);
    }
    return g == 0.0 ? 0.0 : g;
}

double Editor::altitudeForAmsl(int altitudeMode, double amsl, double launchRef, double groundAtPoint)
{
    if (!std::isfinite(amsl)) {
        return kNaN;
    }
    switch (altitudeMode) {
    case 1:     // relative to launch: A = AMSL - R (A-0)
        return std::isfinite(launchRef) ? amsl - launchRef : kNaN;
    case 2:     // AMSL
        return amsl;
    case 3:     // calculated above terrain: A is the height above the ground
    case 4:     // terrain frame
        return std::isfinite(groundAtPoint) ? amsl - groundAtPoint : kNaN;
    default:
        return kNaN;
    }
}

SimpleMissionItem *Editor::simpleItem(int vi) const
{
    if (!_pmc || !_pmc->missionController()) {
        return nullptr;
    }
    QmlObjectListModel *items = _pmc->missionController()->visualItems();
    if (!items || vi <= 0 || vi >= items->count()) {
        return nullptr;     // 0 is the planned home: read only (E-4)
    }
    return qobject_cast<SimpleMissionItem *>(items->get(vi));
}

QString Editor::_commonBlockedReason(int vi) const
{
    if (!_pmc) {
        return tr("No plan.");
    }
    if (_pmc->syncInProgress()) {
        return tr("The plan is being sent to or read from the vehicle.");   // E-5
    }
    if (!_missionLayer) {
        return tr("Only the mission layer can be edited.");                  // E-5
    }
    if (!simpleItem(vi)) {
        return tr("This item is read only in 3D.");                           // E-4
    }
    return QString();
}

QString Editor::moveBlockedReason(int vi) const
{
    const QString common = _commonBlockedReason(vi);
    if (!common.isEmpty()) {
        return common;
    }
    SimpleMissionItem *item = simpleItem(vi);
    if (!isMovableCommand(item->command())) {
        return isHeightOnlyCommand(item->command()) ? tr("A takeoff or a land can only change its height in 3D.")
                                                    : tr("This item is read only in 3D.");
    }
    if (!item->specifiesCoordinate()) {
        return tr("This item has no position.");
    }
    return QString();
}

QString Editor::heightBlockedReason(int vi) const
{
    const QString common = _commonBlockedReason(vi);
    if (!common.isEmpty()) {
        return common;
    }
    SimpleMissionItem *item = simpleItem(vi);
    if (!isMovableCommand(item->command()) && !isHeightOnlyCommand(item->command())) {
        return tr("This item is read only in 3D.");
    }
    if (!item->specifiesAltitude()) {
        return tr("This item has no height.");
    }
    const QString datum = _datumBlockedReason(item);
    if (!datum.isEmpty()) {
        return datum;
    }
    // E-2: the reference the item's mode needs must be known.
    const QGeoCoordinate c = item->coordinate();
    switch (item->altitudeMode()) {
    case QGroundControlQmlGlobal::AltitudeModeRelative:
        if (!_launchRef || !std::isfinite(_launchRef())) {
            return tr("The height of this item is above launch, and neither the ground at home nor the plan's home altitude is known, so its height cannot be dragged.");
        }
        break;
    case QGroundControlQmlGlobal::AltitudeModeAbsolute:
        break;
    case QGroundControlQmlGlobal::AltitudeModeCalcAboveTerrain:
    case QGroundControlQmlGlobal::AltitudeModeTerrainFrame:
        if (!std::isfinite(_groundAt(c.latitude(), c.longitude()))) {
            return tr("The ground under this item is not loaded, so its height above the ground cannot be dragged.");
        }
        break;
    default:
        return tr("This item's altitude mode cannot be edited in 3D.");
    }
    return QString();
}

QString Editor::_datumBlockedReason(SimpleMissionItem *item) const
{
    // A-1: the item flies by its frame; GC's altitude mode says how its
    // Altitude is entered. When they disagree, a write by the mode would land
    // in the wrong datum, so 3D does not change the height (M1).
    if (!item || !item->specifiesAltitude()) {
        return QString();
    }
    const int mode = int(item->altitudeMode());
    const int frame = int(item->missionItem().frame());
    if (!modeAgreesWithFrame(mode, frame)) {
        return tr("This item's altitude mode (%1) and frame (%2) disagree. It flies by its frame, so 3D does not change its height or its command.").arg(mode).arg(frame);
    }
    return QString();
}

double Editor::_groundAt(double lat, double lon) const
{
    if (!_ground) {
        return kNaN;
    }
    // A-11: an ArduPilot takeoff with no position climbs at launch.
    if (lat == 0 && lon == 0 && _pmc && _pmc->missionController()) {
        const QGeoCoordinate home = _pmc->missionController()->plannedHomePosition();
        lat = home.latitude();
        lon = home.longitude();
    }
    return _ground(lat, lon);
}

void Editor::_setMessage(const QString &message)
{
    _limitMessage = false;      // set again by the caller when it is a limit warning
    if (_message != message) {
        _message = message;
        emit messageChanged();
    }
}

bool Editor::checkEdit(int vi, bool height)
{
    const QString why = height ? heightBlockedReason(vi) : moveBlockedReason(vi);
    _setMessage(why);
    return why.isEmpty();
}

bool Editor::beginDrag(int vi)
{
    if (!canMove(vi) && !canChangeHeight(vi)) {
        _setMessage(moveBlockedReason(vi));
        return false;
    }
    _setMessage(QString());
    if (_dragVi >= 0 && _dragVi != vi) {
        endDrag();
    }
    if (_dragVi != vi) {
        _dragItem = simpleItem(vi);     // M2: the drag is bound to this item object
        _dragVi = vi;
        emit draggingChanged();
    }
    return true;
}

void Editor::endDrag()
{
    if (_dragVi >= 0) {
        _dragVi = -1;
        _dragItem.clear();
        emit draggingChanged();
    }
}

bool Editor::moveTo(int vi, double lat, double lon)
{
    const QString why = moveBlockedReason(vi);
    if (!why.isEmpty()) {
        _setMessage(why);
        return false;
    }
    if (!_dragTargetOk(vi)) {
        return false;
    }
    if (!std::isfinite(lat) || !std::isfinite(lon) || std::abs(lat) > 90 || std::abs(lon) > 180) {
        _setMessage(tr("That position is off the map."));
        return false;
    }
    SimpleMissionItem *item = simpleItem(vi);
    // As 2D's MissionItemIndicatorDrag: only latitude and longitude change.
    QGeoCoordinate c(snapDegrees(lat), snapDegrees(lon));
    c.setAltitude(item->coordinate().altitude());
    item->setCoordinate(c);
    // IB-6 row 1 (revision 9): past 200 km from home the item is written, as
    // 2D would, but the 3D view does not check it; say so at once.
    const QGeoCoordinate home = _pmc->missionController()->plannedHomePosition();
    // Same words as the 3D view's own drawing-limit warning (g64-bounds); no
    // contract codes on screen.
    if (home.isValid() && QGeoCoordinate(c.latitude(), c.longitude()).distanceTo(QGeoCoordinate(home.latitude(), home.longitude())) > kPlanExtentM) {
        _setMessage(tr("Item %1 is more than %2 km from home; it and its legs are drawn violet (unknown) and not checked against the terrain.")
                        .arg(item->sequenceNumber()).arg(int(kPlanExtentM / 1000)));
        _limitMessage = true;
    } else if (_limitMessage) {
        _setMessage(QString());
    }
    emit itemEdited(vi, kNaN);
    return true;
}

bool Editor::setHeightAmsl(int vi, double amsl)
{
    const QString why = heightBlockedReason(vi);
    if (!why.isEmpty()) {
        _setMessage(why);
        return false;
    }
    if (!_dragTargetOk(vi)) {
        return false;
    }
    SimpleMissionItem *item = simpleItem(vi);
    const QGeoCoordinate c = item->coordinate();
    const double exact = altitudeForAmsl(int(item->altitudeMode()), amsl,
                                         _launchRef ? _launchRef() : kNaN, _groundAt(c.latitude(), c.longitude()));
    if (!std::isfinite(exact)) {
        return false;
    }
    // E-3 (revision 10): 0.1 in the user's own vertical unit, through the
    // value the 2D field shows, stored as the 2D field converts typed text
    // back (Fact::setCookedValue). In metres this is snapAltitude. The mode
    // is never changed (E-2).
    Fact *alt = item->altitude();
    const double shown = alt->metaData() ? alt->metaData()->rawTranslator()(exact).toDouble() : exact;
    alt->setCookedValue(QString::number(shown, 'f', 1));
    const double A = alt->rawValue().toDouble();
    // IB-6 row 6 (revision 9): heights past +-100 km are drawn clamped; the
    // stored value is kept. Say so at once.
    if (std::abs(amsl) > kDrawnHeightLimitM || std::abs(A) > kDrawnHeightLimitM) {
        _setMessage(tr("Item %1: its height is beyond the %2 km drawing limit; it is drawn at the limit, violet (unknown), and not checked against the terrain.")
                        .arg(item->sequenceNumber()).arg(int(kDrawnHeightLimitM / 1000)));
        _limitMessage = true;
    } else if (_limitMessage) {
        _setMessage(QString());
    }
    // The AMSL the point is now drawn at: the stored value back in AMSL.
    const double drawn = amsl + (A - exact);
    emit itemEdited(vi, drawn);
    return true;
}

// ---------------------------------------------------------------- spline (E-6)

QString Editor::splineNote() const
{
    return tr("Changing to a spline waypoint removes the camera and speed settings on this waypoint.");
}

QString Editor::splineBlockedReason(int vi) const
{
    const QString common = _commonBlockedReason(vi);
    if (!common.isEmpty()) {
        return common;
    }
    Vehicle *v = _pmc->controllerVehicle();
    if (!v || !v->apmFirmware() || !v->multiRotor()) {
        return tr("Spline legs are offered for ArduPilot multirotors only.");      // S-2, S-3
    }
    const int command = simpleItem(vi)->command();
    if (command != 16 && command != 82) {
        return tr("Only a waypoint can be a spline waypoint.");
    }
    // M1: GC's command change sets the frame from the mode; never let it
    // change the datum of an item whose mode and frame disagree.
    return _datumBlockedReason(simpleItem(vi));
}

bool Editor::isSpline(int vi) const
{
    SimpleMissionItem *item = simpleItem(vi);
    return item && item->command() == 82;
}

bool Editor::setSpline(int vi, bool spline)
{
    const QString why = splineBlockedReason(vi);
    if (!why.isEmpty()) {
        _setMessage(why);
        return false;
    }
    SimpleMissionItem *item = simpleItem(vi);
    const int command = spline ? 82 : 16;
    if (item->command() == command) {
        return true;
    }
    // What GC's command change resets (SimpleMissionItem::_setDefaultsForCommand).
    const auto mode = item->altitudeMode();
    const double altitude = item->altitude()->rawValue().toDouble();
    const double hold = item->missionItem().param1();
    // The same command change 2D's command picker makes.
    item->setCommand(command);
    // E-6: written back as 2D's reference types them: mode, altitude, hold.
    item->setAltitudeMode(mode);
    item->altitude()->setRawValue(altitude);
    item->missionItem().setParam1(hold);
    _setMessage(QString());
    emit itemEdited(vi, kNaN);
    return true;
}

// ---------------------------------------------------------------- heading (E-7)

QString Editor::headingBlockedReason(int vi) const
{
    const QString common = _commonBlockedReason(vi);
    if (!common.isEmpty()) {
        return common;
    }
    Vehicle *v = _pmc->controllerVehicle();
    const int command = simpleItem(vi)->command();
    if (v && v->px4Firmware()) {
        if (command != 16) {
            return tr("A heading is set on a waypoint.");
        }
        return QString();
    }
    if (v && v->apmFirmware()) {
        // F8: CONDITION_YAW is a Copter command; planes and VTOLs until checked.
        if (!v->multiRotor()) {
            return tr("On ArduPilot, a heading is set for multirotors only.");
        }
        if (command != 16 && command != 82) {
            return tr("A heading is set on a waypoint.");
        }
        // M3: a turn command 3D did not write is never edited by 3D.
        const QString note = headingNote(vi);
        if (!note.isEmpty()) {
            return note;
        }
        return QString();
    }
    return tr("A heading is set for ArduPilot and PX4 plans only.");
}

namespace {

/// E-7's form of a CONDITION_YAW: absolute heading, rate 0, direction 0.
bool isE7Yaw(SimpleMissionItem *item)
{
    return item && item->command() == 115
        && item->missionItem().param2() == 0
        && item->missionItem().param3() == 0
        && item->missionItem().param4() == 0;
}

} // namespace

SimpleMissionItem *Editor::yawItemAfter(int vi) const
{
    SimpleMissionItem *next = simpleItem(vi + 1);
    return isE7Yaw(next) ? next : nullptr;
}

SimpleMissionItem *Editor::otherTurnAfter(int vi) const
{
    SimpleMissionItem *next = simpleItem(vi + 1);
    return (next && next->command() == 115 && !isE7Yaw(next)) ? next : nullptr;
}

QString Editor::headingNote(int vi) const
{
    Vehicle *v = _pmc ? _pmc->controllerVehicle() : nullptr;
    SimpleMissionItem *turn = (v && v->apmFirmware()) ? otherTurnAfter(vi) : nullptr;
    if (!turn) {
        return QString();
    }
    if (turn->missionItem().param4() != 0) {
        return tr("Heading set by a relative turn command; 3D does not edit it.");
    }
    return tr("Heading set by a turn command with its own rate or direction; 3D does not edit it.");
}

double Editor::heading(int vi) const
{
    SimpleMissionItem *item = simpleItem(vi);
    Vehicle *v = _pmc ? _pmc->controllerVehicle() : nullptr;
    if (!item || !v) {
        return kNaN;
    }
    if (v->px4Firmware()) {
        return item->command() == 16 ? item->missionItem().param4() : kNaN;
    }
    if (v->apmFirmware()) {
        SimpleMissionItem *yaw = yawItemAfter(vi);
        return yaw ? yaw->missionItem().param1() : kNaN;
    }
    return kNaN;
}

bool Editor::setHeading(int vi, double degrees)
{
    const QString why = headingBlockedReason(vi);
    if (!why.isEmpty()) {
        _setMessage(why);
        return false;
    }
    const double h = snapHeading(degrees);
    if (!std::isfinite(h)) {
        return false;
    }
    SimpleMissionItem *item = simpleItem(vi);
    if (_pmc->controllerVehicle()->px4Firmware()) {
        item->missionItem().setParam4(h);       // the waypoint's yaw, as 2D's Yaw field
    } else {
        // ArduPilot hides parameter 4 on a multirotor waypoint: a CONDITION_YAW
        // right after it, through MissionController's insert, as 2D adds one.
        SimpleMissionItem *yaw = yawItemAfter(vi);
        if (!yaw) {
            yaw = qobject_cast<SimpleMissionItem *>(_pmc->missionController()->insertSimpleMissionItem(item->coordinate(), vi + 1, false));
            if (!yaw) {
                return false;
            }
            yaw->setCommand(115);
        }
        yaw->missionItem().setParam1(h);        // absolute heading, degrees
        yaw->missionItem().setParam2(0);        // rate 0
        yaw->missionItem().setParam3(0);        // direction 0
        yaw->missionItem().setParam4(0);        // absolute
    }
    _setMessage(QString());
    emit itemEdited(vi, kNaN);
    return true;
}

bool Editor::clearHeading(int vi)
{
    const QString why = headingBlockedReason(vi);
    if (!why.isEmpty()) {
        _setMessage(why);
        return false;
    }
    if (_pmc->controllerVehicle()->px4Firmware()) {
        simpleItem(vi)->missionItem().setParam4(kNaN);     // empty Yaw field: unchanged (NaN)
    } else if (yawItemAfter(vi)) {
        _pmc->missionController()->removeVisualItem(vi + 1);
    }
    emit itemEdited(vi, kNaN);
    return true;
}

// ---------------------------------------------------------------- gimbal (E-8)

QString Editor::gimbalBlockedReason(int vi) const
{
    const QString common = _commonBlockedReason(vi);
    if (!common.isEmpty()) {
        return common;
    }
    SimpleMissionItem *item = simpleItem(vi);
    CameraSection *cs = item->cameraSection();
    if (item->command() != 16 || !cs || !cs->available()) {
        return tr("A gimbal setting is made on a waypoint.");
    }
    return QString();
}

bool Editor::hasGimbal(int vi) const
{
    SimpleMissionItem *item = simpleItem(vi);
    return item && item->cameraSection() && item->cameraSection()->available() && item->cameraSection()->specifyGimbal();
}

double Editor::gimbalPitch(int vi) const
{
    return hasGimbal(vi) ? simpleItem(vi)->cameraSection()->gimbalPitch()->rawValue().toDouble() : kNaN;
}

double Editor::gimbalYaw(int vi) const
{
    return hasGimbal(vi) ? simpleItem(vi)->cameraSection()->gimbalYaw()->rawValue().toDouble() : kNaN;
}

namespace {

double factLimit(const QVariant &v)
{
    bool ok = false;
    const double d = v.toDouble(&ok);
    return ok ? d : kNaN;
}

} // namespace

bool Editor::setGimbalPitch(int vi, double degrees)
{
    const QString why = gimbalBlockedReason(vi);
    if (!why.isEmpty()) {
        _setMessage(why);
        return false;
    }
    CameraSection *cs = simpleItem(vi)->cameraSection();
    Fact *f = cs->gimbalPitch();
    const double p = snapGimbal(degrees, factLimit(f->rawMin()), factLimit(f->rawMax()));
    if (!std::isfinite(p)) {
        return false;
    }
    cs->setSpecifyGimbal(true);     // 2D's Gimbal check box
    f->setRawValue(p);
    _setMessage(QString());
    emit itemEdited(vi, kNaN);
    return true;
}

bool Editor::setGimbalYaw(int vi, double degrees)
{
    const QString why = gimbalBlockedReason(vi);
    if (!why.isEmpty()) {
        _setMessage(why);
        return false;
    }
    if (!kGimbalYawEditing) {
        _setMessage(tr("Gimbal yaw is not edited in 3D in this version. Set it in the Camera section of the panel on the right."));
        return false;
    }
    if (!std::isfinite(heading(vi))) {
        _setMessage(tr("The gimbal yaw is relative to the vehicle heading, and no heading is set at this waypoint, so the yaw is not known."));
        return false;
    }
    CameraSection *cs = simpleItem(vi)->cameraSection();
    Fact *f = cs->gimbalYaw();
    const double y = snapGimbal(degrees, factLimit(f->rawMin()), factLimit(f->rawMax()));
    if (!std::isfinite(y)) {
        return false;
    }
    cs->setSpecifyGimbal(true);
    f->setRawValue(y);
    _setMessage(QString());
    emit itemEdited(vi, kNaN);
    return true;
}

bool Editor::clearGimbal(int vi)
{
    const QString why = gimbalBlockedReason(vi);
    if (!why.isEmpty()) {
        _setMessage(why);
        return false;
    }
    simpleItem(vi)->cameraSection()->setSpecifyGimbal(false);
    emit itemEdited(vi, kNaN);
    return true;
}

} // namespace Plan3D
