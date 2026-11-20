/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/


#include "MissionControllerTest.h"
#include "MissionController.h"
#include "PlanMasterController.h"
#include "SimpleMissionItem.h"
#include "MissionSettingsItem.h"
#include "SettingsManager.h"
#include "AppSettings.h"
#include "PlanViewSettings.h"
#include "MultiSignalSpy.h"
#include "CameraSection.h"
#include "QGC.h"
#include "FlightPathSegment.h"
#include "TerrainProfile.h"

#include <QtCore/QPointer>
#include <QtQuick/QSGGeometryNode>
#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QThread>
#include <QtCore/QTimer>

MissionControllerTest::MissionControllerTest(void)
{
    
}

void MissionControllerTest::cleanup(void)
{
    delete _masterController;
    delete _multiSpyMissionController;
    delete _multiSpyMissionItem;

    _masterController           = nullptr;
    _missionController          = nullptr;
    _multiSpyMissionController  = nullptr;
    _multiSpyMissionItem        = nullptr;

    MissionControllerManagerTest::cleanup();
}

void MissionControllerTest::_initForFirmwareType(MAV_AUTOPILOT firmwareType)
{
    MissionControllerManagerTest::_initForFirmwareType(firmwareType);

    // VisualMissionItem signals
    _rgVisualItemSignals[coordinateChangedSignalIndex] = SIGNAL(coordinateChanged(const QGeoCoordinate&));

    // MissionController signals
    _rgMissionControllerSignals[visualItemsChangedSignalIndex] =    SIGNAL(visualItemsChanged());

    // Master controller pulls offline vehicle info from settings
    SettingsManager::instance()->appSettings()->offlineEditingFirmwareClass()->setRawValue(QGCMAVLink::firmwareClass(firmwareType));
    _masterController = new PlanMasterController(this);
    _masterController->setFlyView(false);
    _missionController = _masterController->missionController();

    _multiSpyMissionController = new MultiSignalSpy();
    Q_CHECK_PTR(_multiSpyMissionController);
    QCOMPARE(_multiSpyMissionController->init(_missionController, _rgMissionControllerSignals, _cMissionControllerSignals), true);

    _masterController->start();

    // All signals should some through on start
    QCOMPARE(_multiSpyMissionController->checkOnlySignalsByMask(visualItemsChangedSignalMask), true);
    _multiSpyMissionController->clearAllSignals();

    QmlObjectListModel* visualItems = _missionController->visualItems();
    QVERIFY(visualItems);

    // Empty vehicle only has home position
    QCOMPARE(visualItems->count(), 1);

    // Mission Settings should be in first slot
    MissionSettingsItem* settingsItem = visualItems->value<MissionSettingsItem*>(0);
    QVERIFY(settingsItem);

    // Offline vehicle, so no home position
    QCOMPARE(settingsItem->coordinate().isValid(), false);

    // Empty mission, so no child items possible
    QCOMPARE(settingsItem->childItems()->count(), 0);

    // No waypoint lines
    QmlObjectListModel* simpleFlightPathSegments = _missionController->simpleFlightPathSegments();
    QVERIFY(simpleFlightPathSegments);
    QCOMPARE(simpleFlightPathSegments->count(), 0);
}

void MissionControllerTest::_testEmptyVehicleWorker(MAV_AUTOPILOT firmwareType)
{
    _initForFirmwareType(firmwareType);

    // FYI: A significant amount of empty vehicle testing is in _initForFirmwareType since that
    // sets up an empty vehicle

    QmlObjectListModel* visualItems = _missionController->visualItems();
    QVERIFY(visualItems);
    VisualMissionItem* visualItem = visualItems->value<VisualMissionItem*>(0);
    QVERIFY(visualItem);

    _setupVisualItemSignals(visualItem);
}

void MissionControllerTest::_testEmptyVehiclePX4(void)
{
    _testEmptyVehicleWorker(MAV_AUTOPILOT_PX4);
}

void MissionControllerTest::_testEmptyVehicleAPM(void)
{
    _testEmptyVehicleWorker(MAV_AUTOPILOT_ARDUPILOTMEGA);
}

#if 0
void MissionControllerTest::_testOfflineToOnlineWorker(MAV_AUTOPILOT firmwareType)
{
    // Start offline and add item
    _missionController = new MissionController();
    Q_CHECK_PTR(_missionController);
    _missionController->start(false /* flyView */);
    _missionController->insertSimpleMissionItem(QGeoCoordinate(37.803784, -122.462276), _missionController->visualItems()->count());

    // Go online to empty vehicle
    MissionControllerManagerTest::_initForFirmwareType(firmwareType);

#if 1
    // Due to current limitations, offline items will go away
    QCOMPARE(_missionController->visualItems()->count(), 1);
#else
    //Make sure our offline mission items are still there
    QCOMPARE(_missionController->visualItems()->count(), 2);
#endif
}

void MissionControllerTest::_testOfflineToOnlineAPM(void)
{
    _testOfflineToOnlineWorker(MAV_AUTOPILOT_ARDUPILOTMEGA);
}

void MissionControllerTest::_testOfflineToOnlinePX4(void)
{
    _testOfflineToOnlineWorker(MAV_AUTOPILOT_PX4);
}
#endif

void MissionControllerTest::_setupVisualItemSignals(VisualMissionItem* visualItem)
{
    delete _multiSpyMissionItem;

    _multiSpyMissionItem = new MultiSignalSpy();
    Q_CHECK_PTR(_multiSpyMissionItem);
    QCOMPARE(_multiSpyMissionItem->init(visualItem, _rgVisualItemSignals, _cVisualItemSignals), true);
}

// G90: true once every item from yawIndex on shows expectedYaw as its mission gimbal yaw and every item before it shows none (NaN).
bool MissionControllerTest::_gimbalYawPropagated(int yawIndex, double expectedYaw)
{
    for (int i=1; i<_missionController->visualItems()->count(); i++) {
        const double yaw = _missionController->visualItems()->value<VisualMissionItem*>(i)->missionGimbalYaw();
        if (i >= yawIndex ? (qIsNaN(yaw) || !QGC::fuzzyCompare(yaw, expectedYaw)) : !qIsNaN(yaw)) {
            return false;
        }
    }
    return true;
}

void MissionControllerTest::_testGimbalRecalc(void)
{
    _initForFirmwareType(MAV_AUTOPILOT_PX4);
    _missionController->insertSimpleMissionItem(QGeoCoordinate(0, 0), 1);
    _missionController->insertSimpleMissionItem(QGeoCoordinate(0, 0), 2);
    _missionController->insertSimpleMissionItem(QGeoCoordinate(0, 0), 3);
    _missionController->insertSimpleMissionItem(QGeoCoordinate(0, 0), 4);

    // No specific gimbal yaw set yet
    for (int i=1; i<_missionController->visualItems()->count(); i++) {
        VisualMissionItem* visualItem = _missionController->visualItems()->value<VisualMissionItem*>(i);
        QVERIFY(qIsNaN(visualItem->missionGimbalYaw()));
    }

    // Specify gimbal yaw on settings item should generate yaw on all subsequent items
    const int yawIndex = 2;
    SimpleMissionItem* item = _missionController->visualItems()->value<SimpleMissionItem*>(yawIndex);
    item->cameraSection()->setSpecifyGimbal(true);
    item->cameraSection()->gimbalYaw()->setRawValue(0.0);
    SettingsManager::instance()->planViewSettings()->showGimbalOnlyWhenSet()->setRawValue(false);
    // G90: the recalc is two queued hops (item specifiedGimbalYawChanged -> MissionController::_recalcMissionFlightStatusSignal
    // -> _recalcMissionFlightStatus). The old fixed qWait(500) missed the second hop whenever one event loop pass took longer
    // than 500 ms (machine under load), so the initial NaN was still there. Wait for the result instead of a fixed time.
    QTRY_VERIFY_WITH_TIMEOUT(_gimbalYawPropagated(yawIndex, 0.0), 10000);
    for (int i=1; i<_missionController->visualItems()->count(); i++) {
        //qDebug() << i;
        VisualMissionItem* visualItem = _missionController->visualItems()->value<VisualMissionItem*>(i);
        if (i >= yawIndex) {
            QCOMPARE(visualItem->missionGimbalYaw(), 0.0);
        } else {
            QVERIFY(qIsNaN(visualItem->missionGimbalYaw()));
        }
    }
}

// G90: reproduces the "NaN where 0 expected" failure deterministically. A posted event that blocks the event loop for
// longer than the old fixed 500 ms wait (what a loaded machine does) left missionGimbalYaw at its initial NaN under the old
// test. The value still propagates once the event loop runs again. Also checks that only the item the user set produces a
// DO_MOUNT_CONTROL in the saved plan (upload converts the same items), with finite pitch and yaw.
void MissionControllerTest::_testGimbalRecalcSlowEventLoop(void)
{
    _initForFirmwareType(MAV_AUTOPILOT_PX4);
    for (int i=1; i<=4; i++) {
        _missionController->insertSimpleMissionItem(QGeoCoordinate(0, 0), i);
    }
    SettingsManager::instance()->planViewSettings()->showGimbalOnlyWhenSet()->setRawValue(false);

    const int yawIndex = 2;
    QTimer::singleShot(0, this, []() { QThread::msleep(700); });  // One slow event loop pass, as under load
    SimpleMissionItem* item = _missionController->visualItems()->value<SimpleMissionItem*>(yawIndex);
    item->cameraSection()->setSpecifyGimbal(true);
    item->cameraSection()->gimbalYaw()->setRawValue(0.0);

    // Not propagated yet: still NaN ("not specified"), never a made up value
    QVERIFY(qIsNaN(_missionController->visualItems()->value<VisualMissionItem*>(yawIndex)->missionGimbalYaw()));
    QTRY_VERIFY_WITH_TIMEOUT(_gimbalYawPropagated(yawIndex, 0.0), 10000);

    const QJsonArray items = _masterController->saveToJson().object()[QStringLiteral("mission")].toObject()[QStringLiteral("items")].toArray();
    int mountControlCount = 0;
    for (const QJsonValue& value : items) {
        const QJsonObject itemObject = value.toObject();
        if (itemObject[QStringLiteral("command")].toInt() != MAV_CMD_DO_MOUNT_CONTROL) {
            continue;
        }
        mountControlCount++;
        const QJsonArray params = itemObject[QStringLiteral("params")].toArray();
        QVERIFY(params[0].isDouble());  // pitch (a NaN would be saved as null)
        QVERIFY(params[2].isDouble());  // yaw
        QCOMPARE(params[2].toDouble(), 0.0);
    }
    QCOMPARE(mountControlCount, 1);
}

void MissionControllerTest::_testVehicleYawRecalc(void)
{
    _initForFirmwareType(MAV_AUTOPILOT_PX4);

    double wpDistance   = 1000;
    double wpAngleInc   = 45;
    double wpAngle      = 0;

    int cMissionItems = 4;
    QGeoCoordinate currentCoord(0, 0);
    _missionController->insertSimpleMissionItem(currentCoord, 1);
    for (int i=2; i<=cMissionItems; i++) {
        wpAngle += wpAngleInc;
        currentCoord = currentCoord.atDistanceAndAzimuth(wpDistance, wpAngle);
        _missionController->insertSimpleMissionItem(currentCoord, i);
    }

    // G90: wait for the queued recalc instead of a fixed 500 ms (same load sensitivity as _testGimbalRecalc)
    QTRY_COMPARE_WITH_TIMEOUT(_missionController->visualItems()->value<VisualMissionItem*>(cMissionItems - 1)->missionVehicleYaw(), wpAngleInc * (cMissionItems - 2), 10000);

    // No specific vehicle yaw set yet. Vehicle yaw should track flight path.
    double expectedVehicleYaw = wpAngleInc;
    for (int i=2; i<cMissionItems; i++) {
        //qDebug() << i;
        VisualMissionItem* visualItem = _missionController->visualItems()->value<VisualMissionItem*>(i);
        QCOMPARE(visualItem->missionVehicleYaw(), expectedVehicleYaw);
        if (i <= cMissionItems - 1) {
            expectedVehicleYaw += wpAngleInc;
        }
    }

    SimpleMissionItem* simpleItem = _missionController->visualItems()->value<SimpleMissionItem*>(3);
    simpleItem->missionItem().setParam4(66);

    // G90: two queued hops, wait for the result instead of a fixed 500 ms
    QTRY_COMPARE_WITH_TIMEOUT(simpleItem->missionVehicleYaw(), 66.0, 10000);

    // All item should track vehicle path except for the one changed
    expectedVehicleYaw = wpAngleInc;
    for (int i=2; i<cMissionItems; i++) {
        //qDebug() << i;
        VisualMissionItem* visualItem = _missionController->visualItems()->value<VisualMissionItem*>(i);
        QCOMPARE(visualItem->missionVehicleYaw(), i == 3 ? 66.0 : expectedVehicleYaw);
        if (i <= cMissionItems - 1) {
            expectedVehicleYaw += wpAngleInc;
        }
    }
}

void MissionControllerTest::_testLoadJsonSectionAvailable(void)
{
    _initForFirmwareType(MAV_AUTOPILOT_PX4);
    _masterController->loadFromFile(":/unittest/SectionTest.plan");

    QmlObjectListModel* visualItems = _missionController->visualItems();
    QVERIFY(visualItems);
    QCOMPARE(visualItems->count(), 5);

    // Check that only waypoint items have camera and speed sections
    for (int i=1; i<visualItems->count(); i++) {
        SimpleMissionItem* item = visualItems->value<SimpleMissionItem*>(i);
        QVERIFY(item);
        if ((int)item->command() == MAV_CMD_NAV_WAYPOINT) {
            QCOMPARE(item->cameraSection()->available(), true);
            QCOMPARE(item->speedSection()->available(), true);
        } else {
            QCOMPARE(item->cameraSection()->available(), false);
            QCOMPARE(item->speedSection()->available(), false);
        }

    }
}

void MissionControllerTest::_testGlobalAltMode(void)
{
    _initForFirmwareType(MAV_AUTOPILOT_PX4);

    struct  _globalAltMode_s {
        QGroundControlQmlGlobal::AltMode   altMode;
        MAV_FRAME                               expectedMavFrame;
    } altModeTestCases[] = {
        { QGroundControlQmlGlobal::AltitudeModeRelative,            MAV_FRAME_GLOBAL_RELATIVE_ALT },
        { QGroundControlQmlGlobal::AltitudeModeAbsolute,            MAV_FRAME_GLOBAL },
        { QGroundControlQmlGlobal::AltitudeModeCalcAboveTerrain,    MAV_FRAME_GLOBAL },
        { QGroundControlQmlGlobal::AltitudeModeTerrainFrame,        MAV_FRAME_GLOBAL_TERRAIN_ALT },
    };

    for (const _globalAltMode_s& testCase: altModeTestCases) {
        _missionController->removeAll();
        _missionController->setGlobalAltitudeMode(testCase.altMode);

        _missionController->insertTakeoffItem(QGeoCoordinate(0, 0), 1);
        _missionController->insertSimpleMissionItem(QGeoCoordinate(0, 0), 2);
        _missionController->insertSimpleMissionItem(QGeoCoordinate(0, 0), 3);
        _missionController->insertSimpleMissionItem(QGeoCoordinate(0, 0), 4);

        SimpleMissionItem* si = qobject_cast<SimpleMissionItem*>(_missionController->visualItems()->value<VisualMissionItem*>(1));
        QCOMPARE(si->altitudeMode(), QGroundControlQmlGlobal::AltitudeModeRelative);
        QCOMPARE(si->missionItem().frame(), MAV_FRAME_GLOBAL_RELATIVE_ALT);

        for (int i=2; i<_missionController->visualItems()->count(); i++) {
            qDebug() << i;
            SimpleMissionItem* si = qobject_cast<SimpleMissionItem*>(_missionController->visualItems()->value<VisualMissionItem*>(i));
            QCOMPARE(si->altitudeMode(), testCase.altMode);
            QCOMPARE(si->missionItem().frame(), testCase.expectedMavFrame);
        }
    }
}

// G76: MissionController reuses the flight path segment of an unchanged leg when it rebuilds the legs. Before
// G76 the reuse test was inverted, so every recalc rebuilt every leg (a new terrain query each time).
void MissionControllerTest::_testFlightPathSegmentReuse(void)
{
    _initForFirmwareType(MAV_AUTOPILOT_ARDUPILOTMEGA);
    const QGeoCoordinate start(47.0, 8.0);
    for (int i=1; i<=3; i++) {
        _missionController->insertSimpleMissionItem(start.atDistanceAndAzimuth(100.0 * i, 90), i);
    }
    QTRY_COMPARE_WITH_TIMEOUT(_missionController->simpleFlightPathSegments()->count(), 2, 10000);

    SimpleMissionItem* item1 = _missionController->visualItems()->value<SimpleMissionItem*>(1);
    SimpleMissionItem* item2 = _missionController->visualItems()->value<SimpleMissionItem*>(2);
    QVERIFY(item1 && item2);
    QTRY_VERIFY_WITH_TIMEOUT(item1->simpleFlightPathSegment() && item2->simpleFlightPathSegment(), 10000);
    QPointer<FlightPathSegment> leg12 = item1->simpleFlightPathSegment();
    QPointer<FlightPathSegment> leg23 = item2->simpleFlightPathSegment();
    QCOMPARE(leg12->segmentType(), FlightPathSegment::SegmentTypeGeneric);
    QVERIFY(leg12->reuseKey() != 0);

    // Adding an item at the end rebuilds the legs; the two unchanged legs keep their segments
    _missionController->insertSimpleMissionItem(start.atDistanceAndAzimuth(400.0, 90), 4);
    QTRY_COMPARE_WITH_TIMEOUT(_missionController->simpleFlightPathSegments()->count(), 3, 10000);
    QVERIFY(!leg12.isNull());
    QVERIFY(!leg23.isNull());
    QCOMPARE(item1->simpleFlightPathSegment(), leg12.data());
    QCOMPARE(item2->simpleFlightPathSegment(), leg23.data());
    QVERIFY(_missionController->simpleFlightPathSegments()->contains(leg12.data()));

    // A reused segment follows its items: moving item 2 moves the end of leg 1-2
    const QGeoCoordinate moved = start.atDistanceAndAzimuth(250.0, 45);
    item2->setCoordinate(moved);
    QTRY_VERIFY_WITH_TIMEOUT(!leg12.isNull() && leg12->coordinate2().distanceTo(moved) < 0.01, 10000);
}

// G76: a leg switched to or from terrain frame gets the new segment type without reloading the plan.
void MissionControllerTest::_testFlightPathSegmentFrameSwitch(void)
{
    _initForFirmwareType(MAV_AUTOPILOT_ARDUPILOTMEGA);
    const QGeoCoordinate start(47.0, 8.0);
    for (int i=1; i<=3; i++) {
        _missionController->insertSimpleMissionItem(start.atDistanceAndAzimuth(100.0 * i, 90), i);
    }
    SimpleMissionItem* item1 = _missionController->visualItems()->value<SimpleMissionItem*>(1);
    SimpleMissionItem* item2 = _missionController->visualItems()->value<SimpleMissionItem*>(2);
    QVERIFY(item1 && item2);
    QTRY_VERIFY_WITH_TIMEOUT(item1->simpleFlightPathSegment() && item2->simpleFlightPathSegment(), 10000);
    QPointer<FlightPathSegment> oldLeg12 = item1->simpleFlightPathSegment();
    QPointer<FlightPathSegment> leg23 = item2->simpleFlightPathSegment();
    QCOMPARE(oldLeg12->segmentType(), FlightPathSegment::SegmentTypeGeneric);

    // Item 1 to terrain frame: the leg out of it follows the terrain
    item1->setAltitudeMode(QGroundControlQmlGlobal::AltitudeModeTerrainFrame);
    QCOMPARE(item1->missionItem().frame(), MAV_FRAME_GLOBAL_TERRAIN_ALT);
    QTRY_VERIFY_WITH_TIMEOUT(item1->simpleFlightPathSegment() &&
                             item1->simpleFlightPathSegment()->segmentType() == FlightPathSegment::SegmentTypeTerrainFrame, 10000);
    QVERIFY(oldLeg12.isNull());                                 // the old Generic segment is deleted, not left in the model
    QCOMPARE(item2->simpleFlightPathSegment(), leg23.data());   // the unchanged leg 2-3 is reused
    QCOMPARE(leg23->segmentType(), FlightPathSegment::SegmentTypeGeneric);

    // And back
    QPointer<FlightPathSegment> terrainLeg12 = item1->simpleFlightPathSegment();
    item1->setAltitudeMode(QGroundControlQmlGlobal::AltitudeModeRelative);
    QTRY_VERIFY_WITH_TIMEOUT(item1->simpleFlightPathSegment() &&
                             item1->simpleFlightPathSegment()->segmentType() == FlightPathSegment::SegmentTypeGeneric, 10000);
    QVERIFY(terrainLeg12.isNull());
    QCOMPARE(_missionController->simpleFlightPathSegments()->count(), 2);
}

// G76: with reuse, a command change that changes a leg's type (here a waypoint made a land) still rebuilds that leg.
void MissionControllerTest::_testFlightPathSegmentCommandSwitch(void)
{
    _initForFirmwareType(MAV_AUTOPILOT_ARDUPILOTMEGA);
    const QGeoCoordinate start(47.0, 8.0);
    for (int i=1; i<=2; i++) {
        _missionController->insertSimpleMissionItem(start.atDistanceAndAzimuth(100.0 * i, 90), i);
    }
    SimpleMissionItem* item1 = _missionController->visualItems()->value<SimpleMissionItem*>(1);
    SimpleMissionItem* item2 = _missionController->visualItems()->value<SimpleMissionItem*>(2);
    QVERIFY(item1 && item2);
    QTRY_VERIFY_WITH_TIMEOUT(item1->simpleFlightPathSegment(), 10000);
    QCOMPARE(item1->simpleFlightPathSegment()->segmentType(), FlightPathSegment::SegmentTypeGeneric);

    item2->setCommand(MAV_CMD_NAV_LAND);
    QTRY_VERIFY_WITH_TIMEOUT(item1->simpleFlightPathSegment() &&
                             item1->simpleFlightPathSegment()->segmentType() == FlightPathSegment::SegmentTypeLand, 10000);
}

// G76 (CL-1a): an ArduPilot multirotor takeoff has no position of its own, so the plan's first leg runs from the
// planned home to the first waypoint. That leg is the climb from the ground: a takeoff leg, whose start is ground
// contact rather than a strike. Before this fix it was a generic leg and its start was drawn as a strike.
void MissionControllerTest::_testFlightPathSegmentLaunchClimb(void)
{
    _initForFirmwareType(MAV_AUTOPILOT_ARDUPILOTMEGA);
    const QGeoCoordinate start(47.0, 8.0);
    _missionController->insertTakeoffItem(start, 1);
    VisualMissionItem* takeoff = _missionController->visualItems()->value<VisualMissionItem*>(1);
    QVERIFY(takeoff && takeoff->isTakeoffItem());
    if (takeoff->specifiesCoordinate()) {
        QSKIP("the offline vehicle's takeoff has a position; this case needs the ArduPilot multirotor takeoff");
    }
    _missionController->insertSimpleMissionItem(start.atDistanceAndAzimuth(300, 90), 2);
    _missionController->insertSimpleMissionItem(start.atDistanceAndAzimuth(600, 90), 3);

    VisualMissionItem* home = _missionController->visualItems()->value<VisualMissionItem*>(0);
    VisualMissionItem* wp1  = _missionController->visualItems()->value<VisualMissionItem*>(2);
    QVERIFY(home && wp1);
    QTRY_VERIFY_WITH_TIMEOUT(home->simpleFlightPathSegment() && wp1->simpleFlightPathSegment(), 10000);
    QCOMPARE(home->simpleFlightPathSegment()->segmentType(), FlightPathSegment::SegmentTypeTakeoff);
    QCOMPARE(wp1->simpleFlightPathSegment()->segmentType(), FlightPathSegment::SegmentTypeGeneric);

    // A-11: the takeoff climbs straight up at launch, so the leg starts at the takeoff's own height, not at the
    // home altitude, and follows it when the takeoff height changes
    SimpleMissionItem* takeoffItem = qobject_cast<SimpleMissionItem*>(takeoff);
    QVERIFY(takeoffItem);
    takeoffItem->altitude()->setRawValue(40.0);
    QTRY_VERIFY_WITH_TIMEOUT(home->simpleFlightPathSegment() &&
                             QGC::fuzzyCompare(home->simpleFlightPathSegment()->coord1AMSLAlt(), takeoff->amslEntryAlt()), 10000);
    FlightPathSegment* launchLeg = home->simpleFlightPathSegment();
    QCOMPARE(launchLeg->launchStart(), FlightPathSegment::LaunchStartClimbAtLaunch);
    QVERIFY(qAbs(launchLeg->coord1AMSLAlt() - (home->amslExitAlt() + 40.0)) < 0.01);
    QCOMPARE(wp1->simpleFlightPathSegment()->launchStart(), FlightPathSegment::LaunchStartNone);
    takeoffItem->altitude()->setRawValue(77.0);
    QTRY_VERIFY_WITH_TIMEOUT(home->simpleFlightPathSegment() &&
                             qAbs(home->simpleFlightPathSegment()->coord1AMSLAlt() - (home->amslExitAlt() + 77.0)) < 0.01, 10000);

    // A takeoff leg's start is ground contact; the same line on a generic leg is a strike
    const QList<double> flat = QList<double>(11, 10.0);
    const QList<double> distances = FlightPathSegment::sampleDistances(11, 10, 10);
    QCOMPARE(FlightPathSegment::classifySamples(FlightPathSegment::SegmentTypeTakeoff, 10, 60, 100, distances, flat).first(), FlightPathSegment::TerrainGroundContact);
    QCOMPARE(FlightPathSegment::classifySamples(FlightPathSegment::SegmentTypeGeneric, 10, 60, 100, distances, flat).first(), FlightPathSegment::TerrainStrike);
}

// G76: an item loaded from a .plan asks for the ground under it, as an item built from a MissionItem does. Before
// this fix the load set the coordinate before the signals were connected, so no terrain query was made: a terrain
// frame item had no AMSL height and every 2D leg touching it stayed unknown, however long terrain had been loaded.
void MissionControllerTest::_testLoadedItemQueriesTerrain(void)
{
    _initForFirmwareType(MAV_AUTOPILOT_ARDUPILOTMEGA);
    const QGeoCoordinate where(47.0, 8.0);
    _missionController->insertSimpleMissionItem(where, 1);
    SimpleMissionItem* item = _missionController->visualItems()->value<SimpleMissionItem*>(1);
    QVERIFY(item);
    item->setAltitudeMode(QGroundControlQmlGlobal::AltitudeModeTerrainFrame);
    QJsonArray saved;
    item->save(saved);
    QVERIFY(!saved.isEmpty());

    SimpleMissionItem loaded(_masterController, false /* flyView */, true /* forLoad */);
    QSignalSpy coordinateSpy(&loaded, &VisualMissionItem::coordinateChanged);
    QSignalSpy terrainSpy(&loaded, &VisualMissionItem::terrainAltitudeChanged);
    QString errorString;
    QVERIFY2(loaded.load(saved[0].toObject(), 2, errorString), qPrintable(errorString));
    QVERIFY(coordinateSpy.count() >= 1);
    QVERIFY(coordinateSpy.last().at(0).value<QGeoCoordinate>().distanceTo(where) < 0.01);
    QVERIFY(terrainSpy.count() >= 1);   // the terrain update ran (it clears the old value before asking)
    QCOMPARE(loaded.altitudeMode(), QGroundControlQmlGlobal::AltitudeModeTerrainFrame);
}

// G76: the Plan view terrain profile repaints when the plan's terrain changes, and its terrain series carries the
// loaded heights. 41f1eff added a second queued slot to TerrainProfile's compressed _updateSignal; the signal
// compression treated the two slots as one call and dropped the repaint, so the profile kept its first paint
// (before any terrain had loaded): no terrain line and an axis from 0.
void MissionControllerTest::_testTerrainProfileSeries(void)
{
    _initForFirmwareType(MAV_AUTOPILOT_ARDUPILOTMEGA);
    const QGeoCoordinate start(47.0, 8.0);
    for (int i=1; i<=3; i++) {
        _missionController->insertSimpleMissionItem(start.atDistanceAndAzimuth(100.0 * i, 90), i);
    }
    QTRY_COMPARE_WITH_TIMEOUT(_missionController->simpleFlightPathSegments()->count(), 2, 10000);
    QTest::qWait(500);  // let the queued recalcs settle

    // Loaded terrain for both legs: 11 heights each, 10 m apart, from 400 m to 420 m
    QList<FlightPathSegment*> segments;
    for (int i = 1; i <= 2; i++) {
        FlightPathSegment* segment = _missionController->visualItems()->value<VisualMissionItem*>(i)->simpleFlightPathSegment();
        QVERIFY(segment);
        segments.append(segment);
    }
    QList<double> heights;
    for (int i = 0; i < 11; i++) {
        heights.append(400.0 + (2.0 * i));
    }
    for (FlightPathSegment* segment: segments) {
        segment->applyTerrainDataForTest(heights, 10.0, 10.0);
        QCOMPARE(segment->amslTerrainHeights().count(), 11);
    }

    TerrainProfile profile;
    profile.setProperty("visibleWidth", 500.0);
    profile.setMissionController(_missionController);

    // A profile update asks for a repaint and refreshes the leg state line; neither is dropped
    const int paintsBefore = profile.paintRequestCountForTest();
    emit _missionController->recalcTerrainProfile();
    QTRY_VERIFY_WITH_TIMEOUT(profile.paintRequestCountForTest() > paintsBefore, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(profile.strikeLegCount() + profile.unknownLegCount() + profile.noStrikeLegCount(), 2, 5000);

    // The painted terrain series carries every loaded height, and the height axis spans them
    QSGNode* root = profile.updatePaintNode(nullptr, nullptr);
    QVERIFY(root);
    QSGGeometryNode* terrainNode = static_cast<QSGGeometryNode*>(root->childAtIndex(0));
    QVERIFY(terrainNode && terrainNode->geometry());
    QCOMPARE(terrainNode->geometry()->vertexCount(), 22);
    QSGGeometryNode* missingNode = static_cast<QSGGeometryNode*>(root->childAtIndex(1));
    QCOMPARE(missingNode->geometry()->vertexCount(), 0);     // no "missing terrain" line once terrain is loaded
    QVERIFY(profile.property("minAMSLAlt").toDouble() <= 400.0);
    QVERIFY(profile.property("maxAMSLAlt").toDouble() >= 420.0);
    delete root;
}

// G76 (A-10, CL-2): the leg back to launch of an RTL is flown at the aircraft's own return height, which is not in the
// plan. The 2D view draws it unknown (violet, dashed) as the 3D view does; before, it was checked as a descent to the
// home altitude and drawn red near launch. The terrain profile's distance axis now runs to the end of that leg.
void MissionControllerTest::_testFlightPathSegmentRtlLeg(void)
{
    _initForFirmwareType(MAV_AUTOPILOT_ARDUPILOTMEGA);
    const QGeoCoordinate start(47.0, 8.0);
    for (int i=1; i<=2; i++) {
        _missionController->insertSimpleMissionItem(start.atDistanceAndAzimuth(300.0 * i, 90), i);
    }
    SimpleMissionItem* rtl = qobject_cast<SimpleMissionItem*>(_missionController->insertSimpleMissionItem(start.atDistanceAndAzimuth(900.0, 90), 3));
    QVERIFY(rtl);
    rtl->setCommand(MAV_CMD_NAV_RETURN_TO_LAUNCH);

    VisualMissionItem* item1 = _missionController->visualItems()->value<VisualMissionItem*>(1);
    VisualMissionItem* item2 = _missionController->visualItems()->value<VisualMissionItem*>(2);
    QVERIFY(item1 && item2);
    QTRY_VERIFY_WITH_TIMEOUT(item2->simpleFlightPathSegment() && item2->simpleFlightPathSegment()->heightNotInPlan(), 10000);
    FlightPathSegment* rtlLeg = item2->simpleFlightPathSegment();
    QVERIFY(rtlLeg->coordinate2().distanceTo(_missionController->visualItems()->value<VisualMissionItem*>(0)->coordinate()) < 0.01);
    QVERIFY(item1->simpleFlightPathSegment());
    QVERIFY(!item1->simpleFlightPathSegment()->heightNotInPlan());

    // Loaded ground under the RTL leg 1 m below the home altitude: still unknown, never a strike
    QList<double> ground(11, rtlLeg->coord2AMSLAlt() - 1.0);
    rtlLeg->applyTerrainDataForTest(ground, rtlLeg->totalDistance() / 10.0, rtlLeg->totalDistance() / 10.0);
    QCOMPARE(rtlLeg->terrainState(), FlightPathSegment::TerrainUnknown);
    QVERIFY(!rtlLeg->terrainCollision());
    // The same leg without RTL would be a strike at its end
    QCOMPARE(FlightPathSegment::legState(FlightPathSegment::classifySamples(FlightPathSegment::SegmentTypeGeneric, rtlLeg->coord1AMSLAlt(),
                                                                            rtlLeg->coord2AMSLAlt(), rtlLeg->totalDistance(),
                                                                            FlightPathSegment::sampleDistances(11, rtlLeg->totalDistance() / 10.0, rtlLeg->totalDistance() / 10.0),
                                                                            ground)),
             FlightPathSegment::TerrainStrike);

    // The terrain profile's distance runs to the end of the RTL leg, past the mission total, which leaves it out
    TerrainProfile profile;
    profile.setProperty("visibleWidth", 500.0);
    profile.setMissionController(_missionController);
    double drawn = 0;
    for (int i = 0; i < _missionController->visualItems()->count(); i++) {
        FlightPathSegment* segment = _missionController->visualItems()->value<VisualMissionItem*>(i)->simpleFlightPathSegment();
        drawn += segment ? segment->totalDistance() : 0.0;
    }
    QVERIFY(drawn > _missionController->missionTotalDistance() + 100.0);
    QTRY_VERIFY_WITH_TIMEOUT(qAbs(profile.profileDistance() - drawn) < 0.01, 5000);
}
