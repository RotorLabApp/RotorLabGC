/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

// RotorLab GC 5.2.0 (G86): the breach return editor's model says the height is not used for
// ArduPilot and PX4, and a .plan with a breach return still round-trips byte for byte.

#pragma once

#include "UnitTest.h"

class PlanMasterController;

class GeoFenceBreachReturnTest : public UnitTest
{
    Q_OBJECT

protected:
    void init() final;
    void cleanup() final;

private slots:
    void _heightUsage_data();
    void _heightUsage();

private:
    PlanMasterController* _load(const QString& path);
};
