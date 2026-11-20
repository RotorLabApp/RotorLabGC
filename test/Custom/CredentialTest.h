#pragma once

#include "UnitTest.h"

/// Device Key storage (rotorlab/Credential.h): round trip, the plain storage
/// name, the Windows Credential Manager target name and the move from its old
/// em-dash name,
/// the one-time move of an older base64 settings value into a real store, and
/// (G82) no settings-file write when there is no store: kept for this session
/// only, an old file copy removed and not used, and the settings page text.
class CredentialTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _roundTrip();
    void _storeNameIsPlain();
    void _windowsTargetName();
    void _windowsOldTargetMigrated();
    void _entryLabels();
    void _legacyValueMovedOnce();
    void _noStoreReadWhileArmed();
    void _eraseWhileArmedHeldBack();
    void _fileValueWinsAfterCrash();
    void _noStoreNoFileWrite();
    void _noStoreOldFileCopyRemoved();
    void _fileCopyRule();
    void _refusalText();
    void _testFlagBlocksStoreWrites();

    // G105: removes every entry the test's accounts could have left.
    void cleanup() override;
};
