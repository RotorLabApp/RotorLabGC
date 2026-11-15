#pragma once

#include "UnitTest.h"

/// Pure filing rules of the RotorLab Connector (rotorlab/FilingRules.h):
/// flight-type mapping, the stored-body rewrite, the no-pilot notice and pack
/// serial parsing. No server, vehicle or settings needed.
class RotorLabFilingTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _publishedList();
    void _legacyMapping();
    void _otherText();
    void _bodyRewrite();
    void _pilotNotice();
    void _packSerials();
    void _serverFlightTypeSet();
    void _versionGate();
    void _serverTextClip();
    void _flightTypeRequeue();
};
