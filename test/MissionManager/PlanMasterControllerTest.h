/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include "UnitTest.h"

#include <QtCore/QJsonArray>

class PlanMasterController;

class PlanMasterControllerTest : public UnitTest
{
    Q_OBJECT

public:
    PlanMasterControllerTest(void);

private slots:
    void init(void) final;
    void cleanup(void) final;

    void _testMissionFileLoad(void);
    void _testMissionPlannerFileLoad(void);
    void _testActiveVehicleChanged(void);
    void _testPlanVehicleClassMatches(void);
    void _testVtolPlanOnQuadPlaneNoMismatch(void);
    void _testNullNumberRefusedAtLoad(void);

private:
    void _connectArduPlaneMockLink(void);
    QString _writeTestPlan(const QString& dirPath, int vehicleType, const QJsonArray& items = QJsonArray(), const QString& fileName = QString());

private:
    PlanMasterController*   _masterController;
};
