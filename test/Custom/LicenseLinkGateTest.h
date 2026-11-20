#pragma once

#include "UnitTest.h"

class LicenseManager;
namespace LicenseTestRecords { struct Signer; }

/// No link behind the gate.
///
/// LicenseManager::linksAllowed() across the license states; LinkManager
/// refusing every link and holding the auto-connect links while it is false,
/// and starting them when it turns true; the wiring between the two
/// (CustomPlugin::wireLinkGate); the joystick sending nothing while links are
/// not allowed; and "Deactivate this device" refused while a vehicle is
/// connected, with TAK disconnected when the gate comes back. Nothing is sent
/// to a license server: replies are fed straight in.
class LicenseLinkGateTest : public UnitTest
{
    Q_OBJECT

protected slots:
    void init() override;
    void cleanup() override;

private slots:
    // The predicate
    void _predicateNoRecord();
    void _predicateLegacyRecord();
    void _predicateUnverifiableRecord();
    void _predicateActivated();
    void _predicateLapsedGenuine();
    void _predicateOtherDevice();
    void _predicateWrongOrUnknownKey();
    void _predicateClockBackGenuine();
    void _predicateSignalAndTak();

    // LinkManager
    void _linkManagerRefusesWhileNotAllowed();
    void _linkManagerStartsAutoConnectWhenAllowed();
    void _licenseDrivesLinkManager();

    // Joystick
    void _joystickSendsNothingWhileNotAllowed();

    // Deactivate
    void _deactivateRefusedWhileVehicleConnected();
    void _deactivateReplyWithVehicleKeepsSession();
    void _gateSaysNoVehicleLinks();

private:
    static void _clearSettings();
    static void _prepare(LicenseManager &lm, const LicenseTestRecords::Signer &s, qint64 now);
};
