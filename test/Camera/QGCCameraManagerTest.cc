/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "QGCCameraManagerTest.h"
#include "QGCCameraManager.h"
#include "MockLink.h"
#include "MultiVehicleManager.h"
#include "Vehicle.h"

#include <QtTest/QTest>

void QGCCameraManagerTest::_testCameraList()
{
    const QList<CameraMetaData*> cameraList = QGCCameraManager::_parseCameraMetaData(QStringLiteral(":/json/CameraMetaData.json"));

    QVERIFY(!cameraList.isEmpty());

    qDeleteAll(cameraList);
}

// A camera info request in flight holds a raw CameraStruct pointer as its handler data. Deleting the
// camera manager while the vehicle still has that request pending used to leave the vehicle with a
// dangling pointer: the next MAV_CMD timeout called _handleCameraInfoRetry on freed memory.
void QGCCameraManagerTest::_testCameraInfoRequestCancelledOnDelete()
{
    constexpr int cameraCompId = MAV_COMP_ID_CAMERA6;

    _connectMockLink();

    Vehicle *const vehicle = MultiVehicleManager::instance()->activeVehicle();
    QVERIFY(vehicle);
    QGCCameraManager *const cameraManager = vehicle->cameraManager();
    QVERIFY(cameraManager);

    // Vehicle stops responding so the camera info request stays pending
    _mockLink->setCommLost(true);

    mavlink_message_t heartbeat{};
    (void) mavlink_msg_heartbeat_pack_chan(
        static_cast<uint8_t>(vehicle->id()),
        cameraCompId,
        _mockLink->mavlinkChannel(),
        &heartbeat,
        MAV_TYPE_CAMERA,
        MAV_AUTOPILOT_INVALID,
        0,
        0,
        MAV_STATE_STANDBY);
    cameraManager->_handleHeartbeat(heartbeat);

    const int entryIndex = vehicle->_findMavCommandListEntryIndex(cameraCompId, MAV_CMD_REQUEST_MESSAGE);
    QVERIFY(entryIndex != -1);
    QVERIFY(vehicle->_requestMessageInfoMap.value(cameraCompId).contains(MAVLINK_MSG_ID_CAMERA_INFORMATION));

    // Make the request time out on every check so we don't have to wait for the ack timeout
    vehicle->_mavCommandList[entryIndex].ackTimeoutMSecs = -1;

    vehicle->deleteCameraManager();

    // Run the timeout check past max retries. Without the fix this calls
    // _requestCameraInfoMessageResultHandler -> _handleCameraInfoRetry with the deleted CameraStruct.
    for (int i = 0; i <= Vehicle::_mavCommandMaxRetryCount; i++) {
        vehicle->_sendMavCommandResponseTimeoutCheck();
    }

    QCOMPARE(vehicle->_findMavCommandListEntryIndex(cameraCompId, MAV_CMD_REQUEST_MESSAGE), -1);
    QVERIFY(!vehicle->_requestMessageInfoMap.value(cameraCompId).contains(MAVLINK_MSG_ID_CAMERA_INFORMATION));

    _mockLink->setCommLost(false);
}
