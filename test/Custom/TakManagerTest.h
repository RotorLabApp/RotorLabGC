#pragma once

#include "UnitTest.h"

class TakManager;

/// TakManager's ATAK command chain without a TAK server or aircraft: the
/// remote command list, banner and command log, the test command's station
/// checks and the new-install defaults. Each test starts from empty TAK
/// settings (the unit-test build keeps its own settings file).
class TakManagerTest : public UnitTest
{
    Q_OBJECT

    static void setStationPosition(TakManager &tak);

private slots:
    void init() override;
    void cleanup() override;

    void _remoteCommandList();
    void _commandLogAppends();
    void _testCommandIsListed();
    void _testCommandStationChecks();
    void _testCommandParseFailure();
    void _altitudeWarningInforms();
    void _presenceOnConnect();
    void _presenceToServerWithDirect();
    void _beaconKeepsLastRealPosition();
    void _presenceBeforeFirstFix();
    void _directModeAnnouncesToServer();
    void _presenceAtOnceOnChange();
    void _ownUidsExact();
    void _openSenderWarning();
    void _pilotListKinds();
    void _fingerprintPinning();
    void _newInstallDefaults();
    void _savedSettingsKept();
};
