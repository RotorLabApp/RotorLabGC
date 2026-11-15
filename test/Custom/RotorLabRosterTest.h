#pragma once

#include "UnitTest.h"

/// Readiness and open work on the crew screen's aircraft rows
/// (rotorlab/RotorLabRoster.h), fed with JSON shaped like RotorLab 3.5.1's
/// /api/v1/health, /api/v1/workorders and /api/v1/aircraft. No server needed.
class RotorLabRosterTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _stateWords();
    void _notes();
    void _workCopy();
    void _dueDates();
    void _assetRows();
    void _loadedNeedsPilotsAndAircraft();
    void _loadedFromCache();
};
