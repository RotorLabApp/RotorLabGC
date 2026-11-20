#pragma once

#include "UnitTest.h"

/// G64 block (b), ground and terrain: the pure pieces of the Plan view 3D
/// ground (custom/src/plan3d/Plan3DGround.h, Plan3DGroundMesh.h). Grid
/// layout and bilinear lookup with unknown (NaN) ground, the grouping of
/// grid nodes by GC terrain tile, the per-tile load and retry state, the
/// ground mesh's unknown (violet) vertices, and the imagery tile plan and
/// composition. No network, terrain cache or settings needed.
class Plan3DGroundTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _gridLayout();
    void _elevationBilinear();
    void _unknownIsNaN();
    void _providerChangeReloads();
    void _groupByTile();
    void _scheduleRetries();
    void _scheduleRetryAlone();
    void _meshUnknownViolet();
    void _meshAllUnknown();
    void _imageryPlan();
    void _imageryCompose();
    void _areaClampedAndCapped();     ///< G67 integration must-fix 1
};
