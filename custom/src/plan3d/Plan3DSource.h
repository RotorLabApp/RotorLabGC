/****************************************************************************
 *
 * RotorLab GC Plan view 3D (G64, MG2) - read the plan from GC's controllers.
 *
 * The mission comes from MissionController: each visual item is expanded
 * with its own appendMissionItems, the same expansion GC uploads, so survey,
 * corridor, structure scan and landing patterns arrive as their generated
 * items. Fence and rally come from the plan's GeoFence and RallyPoint
 * controllers. Read only: nothing here changes the plan.
 *
 ****************************************************************************/

#pragma once

#include "Plan3DBuilder.h"

class PlanMasterController;

namespace Plan3D {

SourcePlan sourceFromPlan(PlanMasterController *planMasterController);

} // namespace Plan3D
