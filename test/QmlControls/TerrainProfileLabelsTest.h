/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

// RotorLab GC 5.1.2 (G78): label thinning of the Plan view terrain profile (TerrainProfileLabels.js).

#pragma once

#include "UnitTest.h"

class TerrainProfileLabelsTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _longPlanIsThinned();
    void _firstLastAndSelectedKept();
    void _sparsePlanKeepsEveryLabel();
    void _slotsWithoutLabel();
    void _widerLabelsKeepClearance();
};
