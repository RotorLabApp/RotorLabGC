#pragma once

#include "UnitTest.h"

/// Pure station-side policy of the ATAK command chain (tak/TakPolicy.h and
/// tak/TakDefaults.h): alert text, what the alert says was sent, the command
/// log record, the test sender and the altitude warning. No vehicle, socket
/// or settings needed.
class TakPolicyTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _alertText();
    void _prominentVerbs();
    void _sentResult();
    void _logRecord();
    void _testSender();
    void _altitudeWarning();
    void _openSenderWarning();
    void _pilotList();
    void _serverFingerprint();
    void _defaults();
    // G47, G48: addressing
    void _addressGrammar();
    void _prefixFormat();
    void _fleetCallsignNameable();
    void _classifyAddress();
    void _addressTexts();
};
