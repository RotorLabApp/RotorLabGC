#pragma once

#include "UnitTest.h"

/// The single-instance guard (src/RunGuard.h): a lock left by an instance that
/// is gone never blocks the next start, and a live instance is never displaced.
class RunGuardTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _secondLiveGuardRefused();
    void _deadOwnerCleared();
    void _ownPidCleared();
    void _youngLockOfLiveStrangerKept();
    void _oldUnheldLockCleared();
    void _foreignLockKeptAndUnguarded();
};
