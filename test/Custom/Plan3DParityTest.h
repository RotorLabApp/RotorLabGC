/****************************************************************************
 *
 * RotorLab GC Plan view 3D (G66, MG4) - corpus parity with RotorLab's 3D.
 *
 * The method (G66-PARITY-PLAN.md), in short: each of
 * the 17 RotorLab-exported corpus files (test/Custom/plan-corpus/a204-rotorlab,
 * the A204 exports, tree-equal to the corpus) is opened through GC's own
 * PlanMasterController, read with Plan3D::sourceFromPlan as the 3D view
 * reads it, and built with Plan3D::Builder on the SHARED TERRAIN FIXTURE
 * G66-F1 (test/Custom/plan-corpus/g66/G66-F1.json, invented heights, NOT
 * real terrain), loaded into GC's own GroundGrid. Colors are compared only
 * on that fixture (PLAN-CONTRACT.md CL-6, decision 756 OQ9).
 *
 * Per file the test:
 *  - checks GC's scene against the contract on its own: launch reference
 *    (A-0, A-0b), sample spacing (CL-1 rev 4), each sample's state from its
 *    height above the fixture (section 4, CL-1a with the -0.5 m tolerance),
 *    the drawn items (CL-7) and the legend lines (CL-5);
 *  - writes GC's per-sample result (schema g66-samples/1) to RL_G66_OUT, or
 *    ./g66-gc when that is not set;
 *  - compares it with RotorLab's result for the same file, from
 *    RL_G66_ROTORLAB_DIR, or test/Custom/plan-corpus/g66/rotorlab when that is
 *    not set: per sample height above ground and state, matched leg by leg,
 *    in both directions, plus markers, launch reference and legend classes.
 *    Without a RotorLab result for a file, that row is skipped and says so.
 *
 * Network: every request goes to a dead local proxy (127.0.0.1:9).
 *
 ****************************************************************************/

#pragma once

#include "UnitTest.h"

class PlanMasterController;
class QNetworkProxyFactory;

class Plan3DParityTest : public UnitTest
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init() override;
    void cleanup() override;

    void _fixtureIsG66F1();
    void _exportsAreTheCorpus();
    void _comparatorSelfCheck();
    void _parity_data();
    void _parity();
    void _fixtureExercisesEveryState();
    void _legendLines();

private:
    PlanMasterController *_pmc = nullptr;
    QNetworkProxyFactory *_proxyFactory = nullptr;
};
