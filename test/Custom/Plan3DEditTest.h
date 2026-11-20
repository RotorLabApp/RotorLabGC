/****************************************************************************
 *
 * RotorLab GC Plan view 3D editing (G65, MG3) - PLAN-CONTRACT.md section 4a.
 *
 * E-9 parity: the same edit made through Plan3D::Editor (what a 3D drag
 * calls) and the 2D way (the property and Fact writes 2D's QML makes: the
 * drag handle's coordinate, the altitude text field, the command picker)
 * must save a byte-identical .plan through PlanMasterController::saveToJson.
 * Two PlanMasterControllers load the same corpus file; one is edited each
 * way. The 2D side computes its typed values from the contract's formulas
 * (E-2, E-3) on its own, so a wrong datum in the editor shows as a diff.
 *
 * Ground used here is a fixture (a gentle slope), never terrain. Network:
 * every request goes to a dead local proxy (127.0.0.1:9), so GC's terrain
 * queries fail the same way for both controllers.
 *
 ****************************************************************************/

#pragma once

#include "UnitTest.h"

class PlanMasterController;
class QNetworkProxyFactory;

class Plan3DEditTest : public UnitTest
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void _snapRules();
    void _altitudeForAmslPerMode();
    void _editableItems();
    void _heightNeedsReference();

    void _parity_ardupilot_copter_alt_relative();
    void _parity_ardupilot_copter_alt_amsl();
    void _parity_SYNTHETIC_TERRAIN_ardupilot_copter_alt_calc_above_terrain();
    void _parity_ardupilot_copter_alt_terrain_frame();
    void _parity_ardupilot_copter_alt_mixed();
    void _parity_px4_copter_alt_amsl();
    void _parity_ardupilot_copter_spline();
    void _parity_wp300_px4_copter_lawnmower();

    // E-6
    void _splineOnlyArduPilotMultirotor();
    void _splineParity();

    // E-7
    void _headingSnap();
    void _headingParityPx4();
    void _headingParityArduPilot();

    // E-8
    void _gimbalParity();

    // Code review (REVIEW-G65-code.md) must-fix and fix-now items
    void _syncGate();                       // E-5, M2
    void _modeFrameDisagreeRefused();       // M1
    void _dragBoundToItem();                // M2
    void _relativeTurnNotEdited();          // M3
    void _heldHeightDrawnUnknown();         // F4
    void _headingArduPilotMultirotorOnly(); // F8
    void _gimbalYawGated();                 // F10
    void _boundsWarning();                  // IB-6 (revision 9)
    void _dragLoiterToAltKeepsRing();       // rev 11 after a 3D move
    void _checkEditPatternReadOnly();       // 5.3.0 review, visual-ux M1

private:
    /// Loads `fileName` into two controllers, edits every editable item of
    /// `maxItems` (0: all) both ways, and compares the saved files. R comes
    /// from Builder::build on the fixture ground, as the scene gives it, so a
    /// scene that used H instead of G(home) fails. `expected` (by visual
    /// index, NaN to skip) are the hand-checked saved Altitude and params[6].
    void _parity(const QString &fileName, int maxItems = 0, const QList<double> &expected = {});

    QNetworkProxyFactory *_proxyFactory = nullptr;
    /// E-9 (revision 10) runs in metres and in feet. GC fixes the altitude
    /// unit for the process at the first item, so the suite is run twice:
    /// RL_G65_TEST_UNITS=feet selects feet.
    bool _feet = false;
};
