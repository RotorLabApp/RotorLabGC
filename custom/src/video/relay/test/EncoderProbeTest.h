#pragma once

#include "UnitTest.h"

/// G109: what the forward-quality drop-down offers. With a fake probe: no
/// encoder, Media Foundation hardware, Media Foundation software, an H.265
/// camera with and without an encoder, and the reason texts. Then a real probe
/// of this machine's encoders, logged for the record (each candidate, opened or
/// why not, and its time per frame); it skips when none opens.
class EncoderProbeTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _offerNoEncoder();
    void _offerMfHardware();
    void _offerMfSoftware();
    void _offerH265WithEncoder();
    void _offerH265NoEncoder();
    void _unknownCodecIsH264();
    void _levelTexts();
    void _capSteps();
    void _fitSize();
    void _realProbe();
};
