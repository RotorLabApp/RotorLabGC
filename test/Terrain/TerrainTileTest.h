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

class TerrainTileTest : public UnitTest
{
    Q_OBJECT

private slots:
    // RotorLab GC 5.2.0, G79
    void _testCopernicusRoundsToNearest();
    void _testCopernicusNullIsUnknown();
    void _testCopernicusBadRowsAndValues();
    void _testToTileElevationLimits();
    void _testCoordinateQueryUnknownHeightFails();
    void _testPathQueryUnknownHeightFails();
    void _testOldCopernicusCacheKeyNotUsed();
    void _testMigrationMovesOldPendingRows();
    void _testMigrationOldFinishedSetBecomesIncomplete();
};
