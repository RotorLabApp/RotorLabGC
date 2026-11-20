/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

// RotorLab GC 5.1.2 (G70, G71): load then save of every plan in the G61 corpus
// (test/Custom/plan-corpus, saved by stock GC 5.1.1) must give the same bytes.

#pragma once

#include "UnitTest.h"

class PlanMasterController;

class PlanCorpusRoundTripTest : public UnitTest
{
    Q_OBJECT

protected:
    void init() final;
    void cleanup() final;

private slots:
    void initTestCase();
    void cleanupTestCase();
    void _roundTripIsIdentity_data();
    void _roundTripIsIdentity();
    void _amslAltAboveTerrainLoadedFromItsOwnKey();
    void _structureScanLoads();
    void _emptyInsertIsNoOp();

private:
    PlanMasterController* _load(const QString& fileName);
};
