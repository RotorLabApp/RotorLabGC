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
    void _sharePositionDefaultOff();        ///< m4086
    void _beaconWithoutPositionWhenOff();   ///< m4086
    void _beaconPositionWhenOn();           ///< m4086
    void _conflictSeenWithSharingOff();     ///< m4086
    void _ownUidsExact();
    void _openSenderWarning();
    void _pilotListKinds();
    void _fingerprintPinning();
    void _newInstallDefaults();
    void _savedSettingsKept();

    // GC 5.1.2, TAK link
    void _sendToFollowsFleetCallsign();     ///< G40 (i)
    void _fleetJoinClosesLink();            ///< G40 (ii)
    void _verifyOffLogged();                ///< G40 (iii)
    void _passwordMovedToKeyStore();        ///< G41
    void _passwordNeverInSettings();        ///< G41
    void _clientKeyOwnerOnly();             ///< G41
    void _statusAfterFailedConnect();       ///< G45
    void _receiveBufferBounded();           ///< G46, S2
    void _keyFileFailsClosed();             ///< G41 review S5
    void _plainPasswordKeptUntilStored();   ///< G41 review S6
    void _pendingPasswordCleared();         ///< G41 review R2
    void _pendingPasswordChanged();         ///< G41 review R2
    void _keyReplaceFailureCleansUp();      ///< G41 review R4
    void _disconnectClearsReceive();        ///< G46 review R5
    void _storeRefusalNamesItsCause();      ///< G41: armed text only when armed
};
