#pragma once

#include "UnitTest.h"

/// Device Key storage (rotorlab/Credential.h): round trip, the plain storage
/// name, the Windows Credential Manager target name kept as installs have it,
/// and the one-time move of an older base64 settings value into a real store.
class CredentialTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _roundTrip();
    void _storeNameIsPlain();
    void _windowsTargetNameKept();
    void _legacyValueMovedOnce();
    void _noStoreReadWhileArmed();
    void _eraseWhileArmedHeldBack();
    void _fileValueWinsAfterCrash();
};
