/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "PlanMasterControllerTest.h"
#include "MultiSignalSpyV2.h"
#include "MissionManager.h"
#include "PlanMasterController.h"
#include "MissionController.h"
#include "Vehicle.h"
#include "MockLink.h"
#include "MultiVehicleManager.h"
#include "ParameterManager.h"
#include "Fact.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

PlanMasterControllerTest::PlanMasterControllerTest(void)
    : _masterController(nullptr)
{

}

void PlanMasterControllerTest::init(void)
{
    UnitTest::init();

    _masterController = new PlanMasterController(this);
    _masterController->setFlyView(false);
    _masterController->start();
}

void PlanMasterControllerTest::cleanup(void)
{
    delete _masterController;
    _masterController = nullptr;
    UnitTest::cleanup();
}

void PlanMasterControllerTest::_testMissionFileLoad(void)
{
    _masterController->loadFromFile(":/unittest/OldFileFormat.mission");
    QCOMPARE(_masterController->missionController()->visualItems()->count(), 7);
}


void PlanMasterControllerTest::_testMissionPlannerFileLoad(void)
{
    _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
    QCOMPARE(_masterController->missionController()->visualItems()->count(), 6);
}

void PlanMasterControllerTest::_testActiveVehicleChanged(void) {
    // There was a defect where the PlanMasterController would, upon a new active vehicle,
    // overzelously disconnect all subscribers interested in the outgoing active vechicle.
    Vehicle* outgoingManagerVehicle = _masterController->managerVehicle();

    // spyMissionManager emulates a subscriber that should not be disconnected when
    // the active vehicle changes
    MultiSignalSpyV2 spyMissionManager;
    spyMissionManager.init(outgoingManagerVehicle->missionManager());
    MultiSignalSpyV2 spyMasterController;
    spyMasterController.init(_masterController);

    // Since MissionManager works with actual vehicles (which we don't have in the test cycle)
    // we have to be a bit creative emulating a signal emitted by a MissionManager.
    emit outgoingManagerVehicle->missionManager()->error(0,"");
    auto missionManagerErrorSignalMask = spyMissionManager.signalNameToMask("error");
    QVERIFY(spyMissionManager.checkOnlySignalByMask(missionManagerErrorSignalMask));
    spyMissionManager.clearSignal("error");
    QVERIFY(spyMissionManager.checkNoSignals());

    _connectMockLink(MAV_AUTOPILOT_PX4);
    auto masterControllerMgrVehicleChanged = spyMasterController.signalNameToMask("managerVehicleChanged");
    QVERIFY(spyMasterController.checkSignalByMask(masterControllerMgrVehicleChanged));

    emit outgoingManagerVehicle->missionManager()->error(0,"");
    // This signal was affected by the defect - it wouldn't reach the subscriber. Here
    // we make sure it does.
    QVERIFY(spyMissionManager.checkOnlySignalByMask(missionManagerErrorSignalMask));
}

// G91: the vehicle class comparison used by the upload prompt
void PlanMasterControllerTest::_testPlanVehicleClassMatches(void)
{
    // Same class always matches
    QVERIFY(MissionController::planVehicleClassMatches(MAV_TYPE_FIXED_WING, MAV_TYPE_FIXED_WING, false));
    QVERIFY(MissionController::planVehicleClassMatches(MAV_TYPE_VTOL_TAILSITTER_QUADROTOR, MAV_TYPE_VTOL_TILTROTOR, false));
    QVERIFY(MissionController::planVehicleClassMatches(MAV_TYPE_QUADROTOR, MAV_TYPE_HEXAROTOR, false));

    // VTOL Plan (vehicleType 20) on a QuadPlane reporting MAV_TYPE_FIXED_WING: match only when VTOL capable
    QVERIFY(MissionController::planVehicleClassMatches(MAV_TYPE_VTOL_TAILSITTER_QUADROTOR, MAV_TYPE_FIXED_WING, true));
    QVERIFY(!MissionController::planVehicleClassMatches(MAV_TYPE_VTOL_TAILSITTER_QUADROTOR, MAV_TYPE_FIXED_WING, false));

    // Real mismatches still prompt, even on a VTOL capable plane
    QVERIFY(!MissionController::planVehicleClassMatches(MAV_TYPE_QUADROTOR, MAV_TYPE_FIXED_WING, true));
    QVERIFY(!MissionController::planVehicleClassMatches(MAV_TYPE_QUADROTOR, MAV_TYPE_FIXED_WING, false));
    QVERIFY(!MissionController::planVehicleClassMatches(MAV_TYPE_VTOL_TAILSITTER_QUADROTOR, MAV_TYPE_QUADROTOR, true));
    QVERIFY(!MissionController::planVehicleClassMatches(MAV_TYPE_GROUND_ROVER, MAV_TYPE_FIXED_WING, true));
}

// G96: a null where GC needs a number (here the yaw of a DO_MOUNT_CONTROL) refuses the file at load
// with a message naming the item and the value. A null where the command means "unchanged" still loads.
void PlanMasterControllerTest::_testNullNumberRefusedAtLoad(void)
{
    auto simpleItem = [](int command, int doJumpId, const QJsonArray& params) {
        QJsonObject item;
        item[QStringLiteral("type")] = QStringLiteral("SimpleItem");
        item[QStringLiteral("command")] = command;
        item[QStringLiteral("frame")] = MAV_FRAME_GLOBAL_RELATIVE_ALT;
        item[QStringLiteral("autoContinue")] = true;
        item[QStringLiteral("doJumpId")] = doJumpId;
        item[QStringLiteral("params")] = params;
        return item;
    };
    const QJsonValue null = QJsonValue(QJsonValue::Null);

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    MissionController* const missionController = _masterController->missionController();

    // DO_MOUNT_CONTROL with a null yaw (param 3): refused, nothing loaded, the plan file is not taken over
    const QJsonArray badMount = { simpleItem(MAV_CMD_DO_MOUNT_CONTROL, 1, QJsonArray({ 0, 0, null, 0, 0, 0, 2 })) };
    const QString badMountPlan = _writeTestPlan(tempDir.path(), MAV_TYPE_QUADROTOR, badMount, QStringLiteral("badMount.plan"));
    QVERIFY(!badMountPlan.isEmpty());
    _masterController->loadFromFile(badMountPlan);
    QCOMPARE(missionController->visualItems()->count(), 1);
    QVERIFY(_masterController->currentPlanFile().isEmpty());

    // The message names the item, the value and the param, and asks for a number
    {
        QFile file(badMountPlan);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject plan = QJsonDocument::fromJson(file.readAll()).object();
        QString errorString;
        QVERIFY(!missionController->load(plan[QStringLiteral("mission")].toObject(), errorString));
        QVERIFY2(errorString.contains(QStringLiteral("Yaw")), qPrintable(errorString));
        QVERIFY2(errorString.contains(QStringLiteral("param 3 is null")), qPrintable(errorString));
        QVERIFY2(errorString.contains(QStringLiteral("Enter a number")), qPrintable(errorString));
        QVERIFY2(!errorString.contains(QChar(0x2014)), qPrintable(errorString));
    }

    // A waypoint with a null latitude: refused
    const QJsonArray badWaypoint = { simpleItem(MAV_CMD_NAV_WAYPOINT, 1, QJsonArray({ 0, 0, 0, null, null, -122.09, 50 })) };
    const QString badWaypointPlan = _writeTestPlan(tempDir.path(), MAV_TYPE_QUADROTOR, badWaypoint, QStringLiteral("badWaypoint.plan"));
    {
        QFile file(badWaypointPlan);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject plan = QJsonDocument::fromJson(file.readAll()).object();
        QString errorString;
        QVERIFY(!missionController->load(plan[QStringLiteral("mission")].toObject(), errorString));
        QVERIFY2(errorString.contains(QStringLiteral("latitude")), qPrintable(errorString));
    }

    // The same items with numbers load; a null waypoint yaw (param 4, "unchanged") is still accepted.
    // GC folds the mount control into the waypoint's camera section, so the list holds the settings item
    // and the waypoint.
    const QJsonArray good = {
        simpleItem(MAV_CMD_NAV_WAYPOINT, 1, QJsonArray({ 0, 0, 0, null, 47.633, -122.09, 50 })),
        simpleItem(MAV_CMD_DO_MOUNT_CONTROL, 2, QJsonArray({ 0, 0, 0, 0, 0, 0, 2 })),
    };
    const QString goodPlan = _writeTestPlan(tempDir.path(), MAV_TYPE_QUADROTOR, good, QStringLiteral("good.plan"));
    _masterController->loadFromFile(goodPlan);
    QVERIFY(!_masterController->currentPlanFile().isEmpty());
    QCOMPARE(missionController->visualItems()->count(), 2);
}

void PlanMasterControllerTest::_connectArduPlaneMockLink(void)
{
    QSignalSpy spyVehicle(MultiVehicleManager::instance(), &MultiVehicleManager::activeVehicleChanged);
    _mockLink = MockLink::startAPMArduPlaneMockLink(false);
    QVERIFY(_mockLink);
    QCOMPARE(spyVehicle.wait(10000), true);
    _vehicle = MultiVehicleManager::instance()->activeVehicle();
    QVERIFY(_vehicle);
    QSignalSpy spyConnect(_vehicle, &Vehicle::initialConnectComplete);
    QCOMPARE(spyConnect.wait(30000), true);
}

QString PlanMasterControllerTest::_writeTestPlan(const QString& dirPath, int vehicleType, const QJsonArray& items, const QString& fileName)
{
    QJsonObject mission;
    mission[QStringLiteral("version")] = 2;
    mission[QStringLiteral("firmwareType")] = MAV_AUTOPILOT_ARDUPILOTMEGA;
    mission[QStringLiteral("vehicleType")] = vehicleType;
    mission[QStringLiteral("cruiseSpeed")] = 15;
    mission[QStringLiteral("hoverSpeed")] = 5;
    mission[QStringLiteral("items")] = items;
    mission[QStringLiteral("plannedHomePosition")] = QJsonArray({ 47.633, -122.0907, 20 });

    QJsonObject geoFence;
    geoFence[QStringLiteral("version")] = 2;
    geoFence[QStringLiteral("circles")] = QJsonArray();
    geoFence[QStringLiteral("polygons")] = QJsonArray();

    QJsonObject rally;
    rally[QStringLiteral("version")] = 2;
    rally[QStringLiteral("points")] = QJsonArray();

    QJsonObject plan;
    plan[QStringLiteral("fileType")] = QStringLiteral("Plan");
    plan[QStringLiteral("groundStation")] = QStringLiteral("QGroundControl");
    plan[QStringLiteral("version")] = 1;
    plan[QStringLiteral("mission")] = mission;
    plan[QStringLiteral("geoFence")] = geoFence;
    plan[QStringLiteral("rallyPoints")] = rally;

    const QString path = QDir(dirPath).filePath(fileName.isEmpty() ? QStringLiteral("vehicleType%1.plan").arg(vehicleType) : fileName);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return QString();
    }
    file.write(QJsonDocument(plan).toJson());
    file.close();
    return path;
}

// G91: a VTOL Plan (vehicleType 20) uploaded to an ArduPlane QuadPlane must not raise the
// "different firmware or vehicle type" prompt, while a multi-rotor Plan on the same plane still does.
void PlanMasterControllerTest::_testVtolPlanOnQuadPlaneNoMismatch(void)
{
    _connectArduPlaneMockLink();
    QCOMPARE(_masterController->managerVehicle(), _vehicle);
    QVERIFY(QGCMAVLink::isFixedWing(_vehicle->vehicleType()));

    ParameterManager* const paramMgr = _vehicle->parameterManager();
    QVERIFY(paramMgr->parameterExists(ParameterManager::defaultComponentId, QStringLiteral("Q_ENABLE")));
    Fact* const qEnable = paramMgr->getParameter(ParameterManager::defaultComponentId, QStringLiteral("Q_ENABLE"));

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    // The Plans need at least one item: with an empty mission GC goes back to tracking the connected
    // vehicle's type (MissionController::_allItemsRemoved), so the Plan's vehicleType would not stick.
    QJsonObject waypoint;
    waypoint[QStringLiteral("type")] = QStringLiteral("SimpleItem");
    waypoint[QStringLiteral("command")] = MAV_CMD_NAV_WAYPOINT;
    waypoint[QStringLiteral("frame")] = MAV_FRAME_GLOBAL_RELATIVE_ALT;
    waypoint[QStringLiteral("autoContinue")] = true;
    waypoint[QStringLiteral("doJumpId")] = 1;
    waypoint[QStringLiteral("params")] = QJsonArray({ 0, 0, 0, QJsonValue(QJsonValue::Null), 47.634, -122.0907, 50 });
    const QJsonArray items = { waypoint };
    const QString vtolPlan = _writeTestPlan(tempDir.path(), MAV_TYPE_VTOL_TAILSITTER_QUADROTOR, items);
    const QString copterPlan = _writeTestPlan(tempDir.path(), MAV_TYPE_QUADROTOR, items);
    QVERIFY(!vtolPlan.isEmpty());
    QVERIFY(!copterPlan.isEmpty());

    MissionController* const missionController = _masterController->missionController();

    // QuadPlane: Q_ENABLE set
    qEnable->setRawValue(1);
    _masterController->loadFromFile(vtolPlan);
    QCOMPARE(_masterController->controllerVehicle()->vehicleType(), MAV_TYPE_VTOL_TAILSITTER_QUADROTOR);
    QCOMPARE(missionController->sendToVehiclePreCheck(), MissionController::SendToVehiclePreCheckStateOk);

    _masterController->loadFromFile(copterPlan);
    QCOMPARE(_masterController->controllerVehicle()->vehicleType(), MAV_TYPE_QUADROTOR);
    QCOMPARE(missionController->sendToVehiclePreCheck(), MissionController::SendToVehiclePreCheckStateFirwmareVehicleMismatch);

    // Plain plane: Q_ENABLE off, so a VTOL Plan is a real mismatch
    qEnable->setRawValue(0);
    _masterController->loadFromFile(vtolPlan);
    QCOMPARE(missionController->sendToVehiclePreCheck(), MissionController::SendToVehiclePreCheckStateFirwmareVehicleMismatch);
}
