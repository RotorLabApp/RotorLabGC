/****************************************************************************
 *
 * RotorLab GC Plan view 3D (G64, MG2, block (c)) - placement and leg colors.
 *
 * Tests PLAN-CONTRACT.md revision 2 section 2 (placement by frame and
 * params[6], A-1 to A-11) and section 4 (CL-1 to CL-7, leg colors) against
 * the 17 corpus plans in test/Custom/plan-corpus, loaded through GC's own
 * PlanMasterController and read with Plan3D::sourceFromPlan, as the 3D view
 * reads them.
 *
 * Ground used by these tests is never real terrain:
 *  - "unknown" ground: NaN everywhere (A-3);
 *  - "fixture" ground: a constant or shaped test function, named as such;
 *  - SYNTHETIC terrain: the constant 100 m the two synthetic corpus files
 *    were saved with (CORPUS.md). Tests on those two files carry
 *    SYNTHETIC_TERRAIN in their names and prove arithmetic only.
 *
 * Network: every request goes to a dead local proxy (127.0.0.1:9).
 *
 ****************************************************************************/

#pragma once

#include "UnitTest.h"

#include <QtCore/QJsonObject>

class PlanMasterController;
class QNetworkProxyFactory;

namespace Plan3D { struct SourcePlan; struct Scene; }

class Plan3DTest : public UnitTest
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init() override;
    void cleanup() override;

    // Section 2 and section 4 rules on their own (no plan).
    void _placeHeightByFrame();
    void _modeAgreesWithFrame();
    void _defaultModeWithoutTriple();
    void _classifyThresholds();
    void _groundContactRule();
    void _colors();
    void _splineMath();

    // Rules on fixture plans built in the test.
    void _fixtureModeFrameDisagreeDrawnByFrame();
    void _fixtureNoHomeAltitudeUnknown();
    void _fixtureCalcAboveTerrainWithoutAmslUnknown();
    void _fixtureLaunchReferenceFromGround();
    void _fixtureHomeDatumNoteThreshold();
    void _fixtureSampleSpacingLongFixedWingLeg();
    void _wp300BuildTime();
    void _fixtureSamplesPerLeg();
    void _fixtureRidgeStrikeMidLeg();
    void _cl1aCopterTakeoffAndLanding();
    void _cl1aBelowGroundEdge();
    void _cl1aMidLegDipStaysRed();
    void _fixtureBreachReturnAtGround();
    void _fixtureUnknownGroundPatch();
    void _fixtureTerrainFrameLegFollowsGround();
    void _fixtureRtlUnknownAndNeverRed();
    void _fixtureRtlHeightFromAircraft();
    void _fixtureRtlToNearestRally();
    void _fixtureSplineUnknownControlPoint();
    void _fixtureDoJumpSkippedWarned();
    void _planHomeKeptAfterGcTerrain();
    void _fixtureSplineOnlyArduPilotMultirotor();
    void _fixtureLoiterToAltTangentExit();
    void _rtlFromConnectedAircraft();

    // The 17 corpus plans.
    void _corpus_wp300_px4_copter_lawnmower();
    void _corpus_px4_copter_survey_sony_a6000();
    void _corpus_ardupilot_copter_survey_custom_camera();
    void _corpus_SYNTHETIC_TERRAIN_ardupilot_copter_survey_follow_terrain();
    void _corpus_px4_copter_corridor();
    void _corpus_ardupilot_copter_structure();
    void _corpus_ardupilot_plane_fw_landing();
    void _corpus_ardupilot_vtol_vtol_landing();
    void _corpus_ardupilot_copter_alt_relative();
    void _corpus_ardupilot_copter_alt_amsl();
    void _corpus_SYNTHETIC_TERRAIN_ardupilot_copter_alt_calc_above_terrain();
    void _corpus_ardupilot_copter_alt_terrain_frame();
    void _corpus_ardupilot_copter_alt_mixed();
    void _corpus_px4_copter_alt_amsl();
    void _corpus_ardupilot_copter_fence_rally();
    void _corpus_px4_copter_fence_rally();
    void _corpus_ardupilot_copter_spline();
    void _corpusAllSeventeenRender();
    // G67 (5.2.0 review must-fixes): marker colors, strike places, label text.
    void _markerColorRoundTrip();
    void _strikePlacesNamed();
    void _labelTextNamesClearance();
    void _wp300FarHome();             ///< G67 integration must-fix 1: home 12,800 km away
    void _rev11LoiterLegs();          ///< contract rev 11, section 4b, L-1 to L-6
    void _rev11CorpusParity();        ///< rev 11 changes only the landing approaches

    // G67 M1, contract rev 9: drawing limits on malformed plans (Plan3DMath.h).
    void _boundsHugeLoiterRadius();
    void _boundsLoiterRingTooManyPoints();
    void _boundsBeyondExtent();
    void _boundsLegBackstop();
    void _boundsSampleCapPerLeg();
    void _boundsHugeHeights();
    void _boundsManyItems();
    void _boundsNanAndInfValues();
    void _boundsLineMeshBudget();
    void _boundsCorpusUnderLimits();

private:
    /// Loads a corpus file through GC's own load path and reads it as the 3D
    /// view does. Fills `json` with the file's own tree for independent checks.
    bool _load(const QString &fileName, Plan3D::SourcePlan &plan, QJsonObject &json);
    /// The structure scan file: GC 5.1.1 Debug aborts in its load (G71), so the
    /// scan is rebuilt from the file's own values through GC's API, as the
    /// corpus harness built it, and checked against the file.
    bool _loadStructure(const QString &fileName, Plan3D::SourcePlan &plan, QJsonObject &json);

    /// Checks shared by every corpus plan: placement of every item that is in
    /// the file by frame and params[6] (never Altitude), the section 4 classes
    /// on unknown ground and on the given ground, RTL never red.
    void _commonCorpusChecks(const QString &fileName, const Plan3D::SourcePlan &plan, const QJsonObject &json,
                             double groundM, bool syntheticTerrain);

    PlanMasterController *_pmc = nullptr;
    QNetworkProxyFactory *_proxyFactory = nullptr;
};
