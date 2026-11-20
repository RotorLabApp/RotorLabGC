#pragma once

#include "UnitTest.h"

/// G95 start-up recovery (custom/src/video/record/RecordingRecovery.h) on a
/// fixture left as a crash leaves it. Plain Qt.
class RecordingRecoveryTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _cutMidFragmentHashedAsFound();
    void _recoveredLineNotRepeated();
    void _closedButNotQueued();
    void _changedAfterCaptureParked();
    void _knownNamesSkipped();
    void _unmeasuredFpsFlagged();
};
