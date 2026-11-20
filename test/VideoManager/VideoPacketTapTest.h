#pragma once

#include "UnitTest.h"

/// G88 packet tap: the receiver's access units to recording (G95) and relay (G109)
/// consumers. Order, keyframe resync after a drop, a slow consumer never blocking the
/// publisher, attach and detach while publishing, no consumer costing nothing, copies the
/// receiver never writes, and the parameter set store.
class VideoPacketTapTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _noConsumerIsNoOp();
    void _orderAndEvents();
    void _startsAtKeyframe();
    void _keyframeResyncAfterDrop();
    void _eventsAfterFullQueue();
    void _slowConsumerDoesNotBlock();
    void _attachDetachWhileStreaming();
    void _releasedConsumerIsDropped();
    void _dataIsCopy();
    void _parameterSets();
    void _limits();
    // Security review
    void _detachWaitsForRunningWakeup();        ///< M2
    void _setWakeupWaitsForRunningWakeup();     ///< M2
    void _publishDetachDestroyConcurrently();   ///< M2
    void _byteBudgetBoundsQueue();              ///< M3
    void _byteBudgetLimits();                   ///< M3
    void _attachedFalseAfterTapDestroyed();     ///< S12
};
