/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "StructureScanComplexItemTest.h"
#include "PlanMasterController.h"
#include "MultiSignalSpy.h"
#include "StructureScanComplexItem.h"
#include "MissionItem.h"
#include "Vehicle.h"
#include "AppSettings.h"
#include "SettingsManager.h"

#include <QtCore/QJsonArray>
#include <QtCore/QScopedPointer>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

StructureScanComplexItemTest::StructureScanComplexItemTest(void)
{
    _polyPoints << QGeoCoordinate(47.633550640000003, -122.08982199) << QGeoCoordinate(47.634129020000003, -122.08887249) <<
                   QGeoCoordinate(47.633619320000001, -122.08811074) << QGeoCoordinate(47.633189139999999, -122.08900124);
}

void StructureScanComplexItemTest::init(void)
{
    UnitTest::init();

    _rgSignals[dirtyChangedIndex] = SIGNAL(dirtyChanged(bool));

    _masterController = new PlanMasterController(this);
    _structureScanItem = new StructureScanComplexItem(_masterController, false /* flyView */, QString() /* kmlFile */);
    _structureScanItem->setDirty(false);

    _multiSpy = new MultiSignalSpy();
    Q_CHECK_PTR(_multiSpy);
    QCOMPARE(_multiSpy->init(_structureScanItem, _rgSignals, _cSignals), true);
}

void StructureScanComplexItemTest::cleanup(void)
{
    delete _masterController;
    delete _multiSpy;

    _masterController   = nullptr;
    _structureScanItem  = nullptr;  // Deleted when _masterController is deleted
    _multiSpy           = nullptr;

    UnitTest::cleanup();
}

void StructureScanComplexItemTest::_testDirty(void)
{
    QVERIFY(!_structureScanItem->dirty());
    _structureScanItem->setDirty(false);
    QVERIFY(!_structureScanItem->dirty());
    QVERIFY(_multiSpy->checkNoSignals());

    _structureScanItem->setDirty(true);
    QVERIFY(_structureScanItem->dirty());
    QVERIFY(_multiSpy->checkOnlySignalByMask(dirtyChangedMask));
    QVERIFY(_multiSpy->pullBoolFromSignalIndex(dirtyChangedIndex));
    _multiSpy->clearAllSignals();

    _structureScanItem->setDirty(false);
    QVERIFY(!_structureScanItem->dirty());
    QVERIFY(_multiSpy->checkOnlySignalByMask(dirtyChangedMask));
    QVERIFY(!_multiSpy->pullBoolFromSignalIndex(dirtyChangedIndex));
    _multiSpy->clearAllSignals();

    // These facts should set dirty when changed
    QList<Fact*> rgFacts;
    rgFacts << _structureScanItem->entranceAlt() << _structureScanItem->layers();
    for(Fact* fact: rgFacts) {
        qDebug() << fact->name();
        QVERIFY(!_structureScanItem->dirty());
        if (fact->typeIsBool()) {
            fact->setRawValue(!fact->rawValue().toBool());
        } else {
            fact->setRawValue(fact->rawValue().toDouble() + 1);
        }
        QVERIFY(_multiSpy->checkSignalByMask(dirtyChangedMask));
        QVERIFY(_multiSpy->pullBoolFromSignalIndex(dirtyChangedIndex));
        _structureScanItem->setDirty(false);
        _multiSpy->clearAllSignals();
    }
    rgFacts.clear();
}

void StructureScanComplexItemTest::_initItem(void)
{
    QGCMapPolygon* mapPolygon = _structureScanItem->structurePolygon();

    for (int i=0; i<_polyPoints.count(); i++) {
        QGeoCoordinate& vertex = _polyPoints[i];
        mapPolygon->appendVertex(vertex);
    }

    _structureScanItem->cameraCalc()->setCameraBrand(CameraCalc::canonicalManualCameraName());
    _structureScanItem->layers()->setCookedValue(2);
    _structureScanItem->setDirty(false);

    _validateItem(_structureScanItem);
}

void StructureScanComplexItemTest::_validateItem(StructureScanComplexItem* item)
{
    QGCMapPolygon* mapPolygon = item->structurePolygon();

    for (int i=0; i<_polyPoints.count(); i++) {
        QGeoCoordinate& expectedVertex = _polyPoints[i];
        QGeoCoordinate actualVertex = mapPolygon->vertexCoordinate(i);
        QCOMPARE(expectedVertex, actualVertex);
    }

    QVERIFY(_structureScanItem->cameraCalc()->isManualCamera());
    QCOMPARE(item->layers()->cookedValue().toInt(), 2);
}

void StructureScanComplexItemTest::_testSaveLoad(void)
{
    _initItem();

    QJsonArray  items;
    _structureScanItem->save(items);

    QString errorString;
    StructureScanComplexItem* newItem = new StructureScanComplexItem(_masterController, false /* flyView */, QString() /* kmlFile */);
    QVERIFY(newItem->load(items[0].toObject(), 10, errorString));
    QVERIFY(errorString.isEmpty());
    _validateItem(newItem);
    newItem->deleteLater();
}

// G96, PLAN-CONTRACT revision 9 IB-6 bound 3: more than 100 layers is refused at file load with a
// message, never clamped. Both the stored Layers value and the count GC recomputes are checked.
void StructureScanComplexItemTest::_testLayerBoundAtLoad(void)
{
    _initItem();

    QJsonArray items;
    _structureScanItem->save(items);
    QJsonObject base = items[0].toObject();

    // Manual camera: a frontal spacing of 2 m, bottom 0 m
    QJsonObject cameraCalc = base[QStringLiteral("CameraCalc")].toObject();
    cameraCalc[CameraCalc::adjustedFootprintFrontalName] = 2.0;
    base[QStringLiteral("CameraCalc")] = cameraCalc;
    base[StructureScanComplexItem::scanBottomAltName] = 0.0;

    auto withLayers = [&base](double structureHeight, double layers) {
        QJsonObject json = base;
        json[StructureScanComplexItem::structureHeightName] = structureHeight;
        json[StructureScanComplexItem::layersName] = layers;
        return json;
    };

    // Exactly 100 layers loads, unclamped
    {
        QString errorString;
        StructureScanComplexItem* item = new StructureScanComplexItem(_masterController, false /* flyView */, QString() /* kmlFile */);
        QVERIFY2(item->load(withLayers(200.0, 100), 10, errorString), qPrintable(errorString));
        QCOMPARE(item->layers()->rawValue().toInt(), 100);
        item->deleteLater();
    }

    // Stored Layers 101: refused with a message naming the count and the bound
    {
        QString errorString;
        StructureScanComplexItem* item = new StructureScanComplexItem(_masterController, false /* flyView */, QString() /* kmlFile */);
        QVERIFY(!item->load(withLayers(10.0, 101), 10, errorString));
        QVERIFY2(errorString.contains(QStringLiteral("101 layers")), qPrintable(errorString));
        QVERIFY2(errorString.contains(QStringLiteral("100 layers")), qPrintable(errorString));
        QVERIFY2(!errorString.contains(QChar(0x2014)), qPrintable(errorString));
        item->deleteLater();
    }

    // Heights that recompute to 101 layers, whatever the file says: refused
    {
        QString errorString;
        StructureScanComplexItem* item = new StructureScanComplexItem(_masterController, false /* flyView */, QString() /* kmlFile */);
        QVERIFY(!item->load(withLayers(202.0, 5), 10, errorString));
        QVERIFY2(errorString.contains(QStringLiteral("101 layers")), qPrintable(errorString));
        item->deleteLater();
    }

    // A huge structure height: refused before any flight path is built
    {
        QString errorString;
        StructureScanComplexItem* item = new StructureScanComplexItem(_masterController, false /* flyView */, QString() /* kmlFile */);
        QVERIFY(!item->load(withLayers(1.0e9, 5), 10, errorString));
        QVERIFY(!errorString.isEmpty());
        item->deleteLater();
    }

    // A zero frontal spacing with a real height: refused, never divided by zero
    {
        QJsonObject json = withLayers(50.0, 5);
        QJsonObject zeroCamera = json[QStringLiteral("CameraCalc")].toObject();
        zeroCamera[CameraCalc::adjustedFootprintFrontalName] = 0.0;
        json[QStringLiteral("CameraCalc")] = zeroCamera;
        QString errorString;
        StructureScanComplexItem* item = new StructureScanComplexItem(_masterController, false /* flyView */, QString() /* kmlFile */);
        QVERIFY(!item->load(json, 10, errorString));
        QVERIFY2(errorString.contains(QStringLiteral("unbounded")), qPrintable(errorString));
        item->deleteLater();
    }
}

void StructureScanComplexItemTest::_testItemCount(void)
{
    QList<MissionItem*> items;

    _initItem();
    _structureScanItem->appendMissionItems(items, this);
    QCOMPARE(items.count() - 1, _structureScanItem->lastSequenceNumber());

}

// G85: ArduCopter rejects MAV_CMD_DO_SET_ROI_WPNEXT_OFFSET (196) with MAV_MISSION_UNSUPPORTED, which fails
// the whole upload. ArduPilot gets MAV_CMD_DO_SET_ROI_LOCATION (195) at the structure center instead.
// The caller owns masterController; the items are children of the structure scan item, which is a child of masterController.
QList<MissionItem*> StructureScanComplexItemTest::_expandForFirmware(MAV_AUTOPILOT firmwareType, PlanMasterController*& masterController)
{
    masterController = new PlanMasterController(firmwareType, MAV_TYPE_QUADROTOR);
    StructureScanComplexItem* item = new StructureScanComplexItem(masterController, false /* flyView */, QString() /* kmlFile */);
    for (const QGeoCoordinate& vertex: _polyPoints) {
        item->structurePolygon()->appendVertex(vertex);
    }
    item->cameraCalc()->setCameraBrand(CameraCalc::canonicalManualCameraName());
    item->layers()->setCookedValue(2);

    QList<MissionItem*> items;
    item->appendMissionItems(items, item);
    _lastSequenceNumber = item->lastSequenceNumber();
    return items;
}

void StructureScanComplexItemTest::_testArduPilotNoRoiWpNextOffset(void)
{
    PlanMasterController* rawController = nullptr;
    QList<MissionItem*> items = _expandForFirmware(MAV_AUTOPILOT_ARDUPILOTMEGA, rawController);
    QScopedPointer<PlanMasterController> masterController(rawController);
    QCOMPARE(items.count() - 1, _lastSequenceNumber);
    QVERIFY(masterController->controllerVehicle()->apmFirmware());
    QVERIFY(items.count() > 4);

    int roiLocationCount = 0;
    int roiNoneCount = 0;
    for (const MissionItem* missionItem: items) {
        QVERIFY2(missionItem->command() != MAV_CMD_DO_SET_ROI_WPNEXT_OFFSET, "ArduPilot structure scan must not contain command 196");
        if (missionItem->command() == MAV_CMD_DO_SET_ROI_LOCATION) {
            roiLocationCount++;
        } else if (missionItem->command() == MAV_CMD_DO_SET_ROI_NONE) {
            roiNoneCount++;
        }
    }
    QCOMPARE(roiLocationCount, 1);
    QCOMPARE(roiNoneCount, 1);

    // ROI is item 1, right after the entrance waypoint, aimed at the structure center
    const MissionItem* roi = items[1];
    QCOMPARE(roi->command(), MAV_CMD_DO_SET_ROI_LOCATION);
    QCOMPARE(roi->frame(), MAV_FRAME_GLOBAL_RELATIVE_ALT);
    QCOMPARE(roi->param1(), 0.0);
    double sumLat = 0;
    double sumLon = 0;
    for (const QGeoCoordinate& vertex: _polyPoints) {
        sumLat += vertex.latitude();
        sumLon += vertex.longitude();
    }
    const QGeoCoordinate expectedCenter(sumLat / _polyPoints.count(), sumLon / _polyPoints.count());
    QGeoCoordinate center(roi->param5(), roi->param6());
    QVERIFY(center.isValid());
    QVERIFY(center.distanceTo(expectedCenter) < 0.01);
    QVERIFY(roi->param7() > 0.0);
}

// G67: on ArduPilot the gimbal pitch is not used (ROI_LOCATION at the structure center), so the
// editor shows it read only with a note. The stored value is kept as read through save and load.
// PX4 still uses it (param 5 of ROI_WPNEXT_OFFSET).
void StructureScanComplexItemTest::_testGimbalPitchReadOnlyOnArduPilot(void)
{
    const QString note = QStringLiteral("Not used on ArduPilot: the camera points at the center of the structure.");
    for (const MAV_AUTOPILOT firmware : { MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_AUTOPILOT_PX4 }) {
        const bool ardupilot = (firmware == MAV_AUTOPILOT_ARDUPILOTMEGA);
        QScopedPointer<PlanMasterController> masterController(new PlanMasterController(firmware, MAV_TYPE_QUADROTOR));
        StructureScanComplexItem* item = new StructureScanComplexItem(masterController.data(), false /* flyView */, QString() /* kmlFile */);
        for (const QGeoCoordinate& vertex: _polyPoints) {
            item->structurePolygon()->appendVertex(vertex);
        }
        item->cameraCalc()->setCameraBrand(CameraCalc::canonicalManualCameraName());
        item->gimbalPitch()->setRawValue(-30);

        QCOMPARE(item->gimbalPitchUsed(), !ardupilot);
        QCOMPARE(item->gimbalPitchNote(), note);
        QVERIFY(!note.contains(QChar(0x2014)));

        // The value is kept as read
        QJsonArray items;
        item->save(items);
        QCOMPARE(items.count(), 1);
        StructureScanComplexItem* loaded = new StructureScanComplexItem(masterController.data(), false /* flyView */, QString() /* kmlFile */);
        QString errorString;
        QVERIFY2(loaded->load(items[0].toObject(), 10, errorString), qPrintable(errorString));
        QCOMPARE(loaded->gimbalPitch()->rawValue().toDouble(), -30.0);
        QCOMPARE(loaded->gimbalPitchUsed(), !ardupilot);

        // PX4 sends it; ArduPilot sends no gimbal pitch
        QList<MissionItem*> missionItems;
        loaded->appendMissionItems(missionItems, loaded);
        QVERIFY(missionItems.count() > 2);
        if (ardupilot) {
            QCOMPARE(missionItems[1]->command(), MAV_CMD_DO_SET_ROI_LOCATION);
        } else {
            QCOMPARE(missionItems[1]->command(), MAV_CMD_DO_SET_ROI_WPNEXT_OFFSET);
            QCOMPARE(missionItems[1]->param5(), -30.0);
        }
    }

    // The field follows a firmware change in the Plan view
    QScopedPointer<PlanMasterController> masterController(new PlanMasterController(MAV_AUTOPILOT_PX4, MAV_TYPE_QUADROTOR));
    StructureScanComplexItem* item = new StructureScanComplexItem(masterController.data(), false /* flyView */, QString() /* kmlFile */);
    QSignalSpy usedSpy(item, &StructureScanComplexItem::gimbalPitchUsedChanged);
    QVERIFY(item->gimbalPitchUsed());
    Fact* const firmwareClass = SettingsManager::instance()->appSettings()->offlineEditingFirmwareClass();
    const QVariant firmwareClassWas = firmwareClass->rawValue();
    firmwareClass->setRawValue(QGCMAVLink::firmwareClass(MAV_AUTOPILOT_PX4));
    masterController->controllerVehicle()->trackFirmwareVehicleTypeChanges();
    QVERIFY(item->gimbalPitchUsed());
    firmwareClass->setRawValue(QGCMAVLink::firmwareClass(MAV_AUTOPILOT_ARDUPILOTMEGA));
    QVERIFY(usedSpy.count() > 0);
    QVERIFY(!item->gimbalPitchUsed());
    masterController->controllerVehicle()->stopTrackingFirmwareVehicleTypeChanges();
    firmwareClass->setRawValue(firmwareClassWas);
}

void StructureScanComplexItemTest::_testPX4KeepsRoiWpNextOffset(void)
{
    PlanMasterController* rawController = nullptr;
    QList<MissionItem*> items = _expandForFirmware(MAV_AUTOPILOT_PX4, rawController);
    QScopedPointer<PlanMasterController> masterController(rawController);
    QCOMPARE(items.count() - 1, _lastSequenceNumber);
    QVERIFY(masterController->controllerVehicle()->px4Firmware());

    // PX4 accepts 196 (A204 SITL, PX4 v1.15.4): unchanged
    const MissionItem* roi = items[1];
    QCOMPARE(roi->command(), MAV_CMD_DO_SET_ROI_WPNEXT_OFFSET);
    QCOMPARE(roi->frame(), MAV_FRAME_MISSION);
    QCOMPARE(roi->param5(), 0.0);   // gimbal pitch fact default
    QCOMPARE(roi->param6(), 0.0);
    QCOMPARE(roi->param7(), 90.0);
    QCOMPARE(items[items.count() - 2]->command(), MAV_CMD_DO_SET_ROI_NONE);
    for (int i = 0; i < items.count(); i++) {
        if (i != 1) {
            QVERIFY(items[i]->command() != MAV_CMD_DO_SET_ROI_LOCATION);
        }
    }
}
