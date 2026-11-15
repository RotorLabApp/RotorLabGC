#pragma once

#include "UnitTest.h"

/// The RotorLab Connector's spool (rotorlab/FlightSpool.h) and the re-arm
/// window on disk (rotorlab/FlightWatcher.h), with no server: replies are fed
/// straight to the spool. Runs in QStandardPaths test mode, so the real spool
/// on this computer is never touched.
class RotorLabSpoolTest : public UnitTest
{
    Q_OBJECT

protected slots:
    void init() override;
    void cleanup() override;

private slots:
    void _rejectedKeyStopsAndResumes();
    void _missingLogParks();
    void _uncollectedLog();
    void _tooLargeLog();
    void _retryKeepsReference();
    void _retryFlightBody();
    void _logLimit();
    void _pendingFlightsOnDisk();
    void _noLogPullWhileArmed();
    void _discardFlightTakesItsLogs();
    void _orphanLogParkedOnLoad();
    void _checklistRunsFollowTheirFlight();
};
