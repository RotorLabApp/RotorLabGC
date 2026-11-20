#pragma once

#include "UnitTest.h"

/// When post-flight log collection asks the aircraft, how often, and what the
/// operator is told (custom/src/net/LogCollectPolicy.h, G98 F-3).
class LogCollectPolicyTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _armedUsesNoAttempt();
    void _notBeforeDisarmOrFlightEnd();
    void _retriesSpacedOut();
    void _reasonKeptInGaveUpText();
    void _disarmRefusalRecognized();
    void _settleWaitsForSizeToHold();
    void _settleCapTakesWhatIsListed();
    void _f3Replay();
};
