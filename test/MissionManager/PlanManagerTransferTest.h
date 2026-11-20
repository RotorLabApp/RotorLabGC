/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

// RotorLab GC 5.3.0 (G92, G93): PlanManager upload state machine against a PX4 MockLink that loses
// acks, reports "WPM: REJ. CMD: Busy" for a stale open transfer, or pauses its item requests.

#pragma once

#include "UnitTest.h"

class PlanManagerTransferTest : public UnitTest
{
    Q_OBJECT

private slots:
    void init() final;

    // G92
    void _testEmptyRallyUploadLostAck();
    void _testEmptyRallyUploadOwnStaleBusy();

    // G93
    void _testUploadSurvivesRequestPause();
    void _testStalledUploadCancelsSoRallyFollows();
    void _testMissionUploadBacksOffOnOwnBusy();

    // Never cancel another operator's transfer
    void _testOtherGcsTransferNotCancelled();
    void _testOtherGcsSameTypeTransferNotCancelled();
    void _testUnownedStaleTransferNotCancelled();

private:
    static QList<MissionItem*> _makeMissionItems(int count);
    void _abandonMissionUploadWithLostCancel();
};
