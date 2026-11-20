/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "PlanManagerTransferTest.h"
#include "PlanManager.h"
#include "MissionItem.h"
#include "MissionManager.h"
#include "RallyPointManager.h"
#include "MockLink.h"
#include "Vehicle.h"

#include <QtPositioning/QGeoCoordinate>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

namespace {
    // Long enough for every retry PlanManager may make
    constexpr int kTransferWaitMs = PlanManager::_ackTimeoutMilliseconds * (PlanManager::_maxRetryCount + 3);
    // Long enough for the busy back-off: (_maxBusyRetryCount + 1) back-offs plus slack
    constexpr int kBusyWaitMs = PlanManager::_busyBackoffMilliseconds * (PlanManager::_maxBusyRetryCount + 1) + PlanManager::_ackTimeoutMilliseconds * 2;
    // A ground station other than this one (this GC uses system id 255, component 190 by default)
    constexpr int kOtherGcsSystemId = 254;
    constexpr int kOtherGcsComponentId = 190;
}

QList<MissionItem*> PlanManagerTransferTest::_makeMissionItems(int count)
{
    // Item 0 is the planned home position, which PX4's PlanManager drops before the upload
    QList<MissionItem*> items;
    for (int i = 0; i < count; i++) {
        items.append(new MissionItem(i,
                                     MAV_CMD_NAV_WAYPOINT,
                                     MAV_FRAME_GLOBAL_RELATIVE_ALT,
                                     0, 0, 0, 0,
                                     47.3769 + (i * 0.0001),
                                     8.549444,
                                     30,
                                     true,      // autoContinue
                                     false,     // isCurrentItem
                                     nullptr));
    }
    return items;
}

void PlanManagerTransferTest::init()
{
    UnitTest::init();
    _connectMockLink(MAV_AUTOPILOT_PX4);
}

// G92: an empty rally upload is MISSION_COUNT 0 answered only by MISSION_ACK. When that ack is lost GC must
// resend MISSION_COUNT 0, not fail with "vehicle failed to send final ack".
void PlanManagerTransferTest::_testEmptyRallyUploadLostAck()
{
    RallyPointManager* const rally = _vehicle->rallyPointManager();
    QVERIFY(rally);
    QVERIFY(!rally->inProgress());

    _mockLink->setMissionItemFailureMode(MockLinkMissionItemHandler::FailWriteEmptyListFirstAckLost, MAV_MISSION_ERROR);
    const int countsBefore = _mockLink->missionCountCount();

    QSignalSpy spyComplete(rally, &RallyPointManager::sendComplete);
    QSignalSpy spyError(rally, &RallyPointManager::error);

    rally->sendToVehicle(QList<QGeoCoordinate>());
    QVERIFY(rally->inProgress());

    QVERIFY(spyComplete.wait(kTransferWaitMs));
    QCOMPARE(spyComplete.count(), 1);
    QCOMPARE(spyComplete.takeFirst().at(0).toBool(), false /* error */);
    QCOMPARE(spyError.count(), 0);
    QVERIFY(_mockLink->missionCountCount() - countsBefore >= 2);
    QVERIFY(!rally->inProgress());
}

// G93: PX4 stops requesting items for longer than one ack timeout (lost MISSION_REQUESTs, healed by PX4's own
// 250 ms re-request). GC must keep waiting and finish the upload, not fail with "Vehicle did not request all items".
void PlanManagerTransferTest::_testUploadSurvivesRequestPause()
{
    MissionManager* const missionManager = _vehicle->missionManager();
    QVERIFY(missionManager);
    QVERIFY(MockLinkMissionItemHandler::_writeRequestDelayMs > PlanManager::_ackTimeoutMilliseconds);
    QVERIFY(MockLinkMissionItemHandler::_writeRequestDelayMs < PlanManager::_ackTimeoutMilliseconds * (PlanManager::_maxWriteStallCount + 1));

    _mockLink->setMissionItemFailureMode(MockLinkMissionItemHandler::FailWriteRequest1Delayed, MAV_MISSION_ERROR);

    QSignalSpy spyComplete(missionManager, &PlanManager::sendComplete);
    QSignalSpy spyError(missionManager, &PlanManager::error);

    missionManager->writeMissionItems(_makeMissionItems(5));

    QVERIFY(spyComplete.wait(kTransferWaitMs));
    QCOMPARE(spyComplete.takeFirst().at(0).toBool(), false /* error */);
    QCOMPARE(spyError.count(), 0);
    QCOMPARE(missionManager->missionItems().count(), 4);
}

// G93: when GC does give up in the middle of an upload it must cancel the transfer on the vehicle, so the
// GeoFence and Rally uploads that PlanMasterController starts next are not answered "WPM: REJ. CMD: Busy".
void PlanManagerTransferTest::_testStalledUploadCancelsSoRallyFollows()
{
    MissionManager* const missionManager = _vehicle->missionManager();
    RallyPointManager* const rally = _vehicle->rallyPointManager();
    QVERIFY(missionManager);
    QVERIFY(rally);

    _mockLink->setMissionItemFailureMode(MockLinkMissionItemHandler::FailWriteRequest1NoResponseStaysBusy, MAV_MISSION_ERROR);
    const int cancelsBefore = _mockLink->missionCancelAckCount();

    QSignalSpy spyMissionComplete(missionManager, &PlanManager::sendComplete);
    QSignalSpy spyMissionError(missionManager, &PlanManager::error);

    missionManager->writeMissionItems(_makeMissionItems(5));

    QVERIFY(spyMissionComplete.wait(kTransferWaitMs));
    QCOMPARE(spyMissionComplete.takeFirst().at(0).toBool(), true /* error */);
    QCOMPARE(spyMissionError.count(), 1);
    QCOMPARE(spyMissionError.takeFirst().at(0).toInt(), static_cast<int>(PlanManager::ProtocolError));

    // GC cancelled the open upload on the vehicle
    QTRY_VERIFY_WITH_TIMEOUT(_mockLink->missionCancelAckCount() > cancelsBefore, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(!_mockLink->missionBusyStaleTransfer(), 2000);

    // The empty rally upload that follows goes through on its first MISSION_COUNT
    _mockLink->setMissionItemFailureMode(MockLinkMissionItemHandler::FailNone, MAV_MISSION_ACCEPTED);
    const int countsBefore = _mockLink->missionCountCount();

    QSignalSpy spyRallyComplete(rally, &RallyPointManager::sendComplete);
    QSignalSpy spyRallyError(rally, &RallyPointManager::error);

    rally->sendToVehicle(QList<QGeoCoordinate>());

    QVERIFY(spyRallyComplete.wait(kTransferWaitMs));
    QCOMPARE(spyRallyComplete.takeFirst().at(0).toBool(), false /* error */);
    QCOMPARE(spyRallyError.count(), 0);
    QCOMPARE(_mockLink->missionCountCount() - countsBefore, 1);
}

// This GC instance abandons a mission upload (PX4 stops requesting at item 1) and its cancel is lost, so the vehicle
// still holds GC's own upload open. Used to set up the "own stale transfer" cases.
void PlanManagerTransferTest::_abandonMissionUploadWithLostCancel()
{
    MissionManager* const missionManager = _vehicle->missionManager();
    QVERIFY(missionManager);

    _mockLink->setMissionItemFailureMode(MockLinkMissionItemHandler::FailWriteRequest1NoResponseStaysBusy, MAV_MISSION_ERROR);
    _mockLink->setMissionDropNextCancel(true);

    QSignalSpy spyComplete(missionManager, &PlanManager::sendComplete);
    missionManager->writeMissionItems(_makeMissionItems(5));
    QVERIFY(spyComplete.wait(kTransferWaitMs));
    QCOMPARE(spyComplete.takeFirst().at(0).toBool(), true /* error */);

    // The give-up cancel was lost: the vehicle still holds our upload
    QVERIFY(_mockLink->missionBusyStaleTransfer());
    _mockLink->setMissionItemFailureMode(MockLinkMissionItemHandler::FailNone, MAV_MISSION_ACCEPTED);
}

// G92: PX4 still holds an upload of another type that THIS GC abandoned moments ago (its cancel was lost). PX4 sends
// its Busy reject (MAV_MISSION_ERROR stamped MAV_MISSION_TYPE_MISSION) to us as that transfer's partner. GC cancels
// its own stale transfer once and retries, so the empty rally upload succeeds.
void PlanManagerTransferTest::_testEmptyRallyUploadOwnStaleBusy()
{
    _abandonMissionUploadWithLostCancel();
    if (QTest::currentTestFailed()) {
        return;
    }

    RallyPointManager* const rally = _vehicle->rallyPointManager();
    QVERIFY(rally);
    const int cancelsBefore = _mockLink->missionCancelAckCount();
    const int countsBefore = _mockLink->missionCountCount();

    QSignalSpy spyComplete(rally, &RallyPointManager::sendComplete);
    QSignalSpy spyError(rally, &RallyPointManager::error);

    rally->sendToVehicle(QList<QGeoCoordinate>());

    QVERIFY(spyComplete.wait(kTransferWaitMs));
    QCOMPARE(spyComplete.takeFirst().at(0).toBool(), false /* error */);
    QCOMPARE(spyError.count(), 0);
    QCOMPARE(_mockLink->missionCancelAckCount() - cancelsBefore, 1);
    QVERIFY(!_mockLink->missionBusyStaleTransfer());
    QVERIFY(_mockLink->missionCountCount() - countsBefore >= 2);
}

// G93: PX4 answers our own MISSION_COUNT with MAV_MISSION_ERROR because the mission upload this GC abandoned is still
// open. GC cancels its own transfer once, backs off and resends MISSION_COUNT instead of failing at once.
void PlanManagerTransferTest::_testMissionUploadBacksOffOnOwnBusy()
{
    _abandonMissionUploadWithLostCancel();
    if (QTest::currentTestFailed()) {
        return;
    }

    MissionManager* const missionManager = _vehicle->missionManager();
    const int cancelsBefore = _mockLink->missionCancelAckCount();
    const int countsBefore = _mockLink->missionCountCount();

    QSignalSpy spyComplete(missionManager, &PlanManager::sendComplete);
    QSignalSpy spyError(missionManager, &PlanManager::error);

    missionManager->writeMissionItems(_makeMissionItems(5));

    QVERIFY(spyComplete.wait(kTransferWaitMs));
    QCOMPARE(spyComplete.takeFirst().at(0).toBool(), false /* error */);
    QCOMPARE(spyError.count(), 0);
    QCOMPARE(_mockLink->missionCancelAckCount() - cancelsBefore, 1);
    QVERIFY(_mockLink->missionCountCount() - countsBefore >= 2);
    QCOMPARE(missionManager->missionItems().count(), 4);
}

// Safety: another ground station (or a TAK driven upload) holds a mission transfer on the vehicle. PX4 addresses its
// Busy reject to that station. GC must never cancel it: it backs off, retries, then fails cleanly as vehicle busy.
void PlanManagerTransferTest::_testOtherGcsTransferNotCancelled()
{
    RallyPointManager* const rally = _vehicle->rallyPointManager();
    QVERIFY(rally);

    _mockLink->setMissionBusyStaleTransfer(MAV_MISSION_TYPE_MISSION, kOtherGcsSystemId, kOtherGcsComponentId);
    const int cancelsBefore = _mockLink->missionCancelAckCount();

    QSignalSpy spyComplete(rally, &RallyPointManager::sendComplete);
    QSignalSpy spyError(rally, &RallyPointManager::error);

    rally->sendToVehicle(QList<QGeoCoordinate>());

    QVERIFY(spyComplete.wait(kBusyWaitMs));
    QCOMPARE(spyComplete.takeFirst().at(0).toBool(), true /* error */);
    QCOMPARE(spyError.count(), 1);
    const QList<QVariant> errorArgs = spyError.takeFirst();
    QCOMPARE(errorArgs.at(0).toInt(), static_cast<int>(PlanManager::VehicleBusyError));
    QVERIFY(errorArgs.at(1).toString().contains(QStringLiteral("busy")));

    // No cancel was sent, and the other station's transfer is untouched
    QCOMPARE(_mockLink->missionCancelAckCount(), cancelsBefore);
    QVERIFY(_mockLink->missionBusyStaleTransfer());
}

// Safety, same type: another ground station holds a mission upload and we upload a mission. No cancel, clean busy failure.
void PlanManagerTransferTest::_testOtherGcsSameTypeTransferNotCancelled()
{
    MissionManager* const missionManager = _vehicle->missionManager();
    QVERIFY(missionManager);

    _mockLink->setMissionBusyStaleTransfer(MAV_MISSION_TYPE_MISSION, kOtherGcsSystemId, kOtherGcsComponentId);
    const int cancelsBefore = _mockLink->missionCancelAckCount();

    QSignalSpy spyComplete(missionManager, &PlanManager::sendComplete);
    QSignalSpy spyError(missionManager, &PlanManager::error);

    missionManager->writeMissionItems(_makeMissionItems(5));

    QVERIFY(spyComplete.wait(kBusyWaitMs));
    QCOMPARE(spyComplete.takeFirst().at(0).toBool(), true /* error */);
    QCOMPARE(spyError.count(), 1);
    QCOMPARE(spyError.takeFirst().at(0).toInt(), static_cast<int>(PlanManager::VehicleBusyError));
    QCOMPARE(_mockLink->missionCancelAckCount(), cancelsBefore);
    QVERIFY(_mockLink->missionBusyStaleTransfer());
}

// Safety: the Busy reject is addressed to our system and component ids (for example a second ground station that uses
// the same default ids), but this GC instance never abandoned a transfer of that type. GC must not cancel it.
void PlanManagerTransferTest::_testUnownedStaleTransferNotCancelled()
{
    RallyPointManager* const rally = _vehicle->rallyPointManager();
    QVERIFY(rally);

    _mockLink->setMissionBusyStaleTransfer(MAV_MISSION_TYPE_MISSION);   // partner ids equal ours
    const int cancelsBefore = _mockLink->missionCancelAckCount();

    QSignalSpy spyComplete(rally, &RallyPointManager::sendComplete);
    QSignalSpy spyError(rally, &RallyPointManager::error);

    rally->sendToVehicle(QList<QGeoCoordinate>());

    QVERIFY(spyComplete.wait(kBusyWaitMs));
    QCOMPARE(spyComplete.takeFirst().at(0).toBool(), true /* error */);
    QCOMPARE(spyError.count(), 1);
    QCOMPARE(spyError.takeFirst().at(0).toInt(), static_cast<int>(PlanManager::VehicleBusyError));
    QCOMPARE(_mockLink->missionCancelAckCount(), cancelsBefore);
    QVERIFY(_mockLink->missionBusyStaleTransfer());
}
