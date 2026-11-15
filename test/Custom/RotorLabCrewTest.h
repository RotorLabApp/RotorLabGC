#pragma once

#include "UnitTest.h"

/// The crew screen, checklist and missions rules of the RotorLab Connector
/// (rotorlab/RotorLabRoster.h, namespace RotorLabCrew). No server and no
/// vehicle needed.
class RotorLabCrewTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _crewScreenDeferredWhileArmed();
    void _changePilotOpensAnyTime();
    void _preflightOffer();
    void _missionsEmptyText();
    void _armedKeyErrorClearsAtDisarm();
    void _verifyDoesNotReask();
    void _filingReconnects();
    void _externalRefHasNoHostName();
    void _everyTemplateListed();
    void _picVersionGate();
    void _picPeriods();
    void _picSorties();
    void _picWording();
    void _picLogFile();
    /// Against a local RotorLab carrying the PIC API, when
    /// ROTORLAB_PIC_CONNECTION names its CONNECTION.json; skipped otherwise.
    void _picLiveServer();
};
