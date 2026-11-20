#pragma once

#include "UnitTest.h"

/// G109: live fragments for the Fleet Server (contract S20). Box structure
/// and sample flags (walked back from the bytes), 1 s cuts on a long GOP that
/// begin mid-GOP with non-sync flags, back-to-back decode times, the 4 MB cap
/// on the serialized fragment, an oversize unit dropped and the stream resumed
/// at a keyframe, new init segments, H.265 refused, RTP wrap, and the SPS
/// fields the relay reads. No encoder, no network.
class LiveFragmenterTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _spsFields();
    void _initStructure();
    void _fragmentStructureAndFlags();
    void _oneSecondCutsOnLongGop();
    void _splitUnderFourMegabytes();
    void _oversizeUnitDropped();
    void _firstFragmentAfterResyncIsKeyframe();
    void _newInitOnGenerationAndSize();
    void _h265Refused();
    void _rtpWrapKeepsDurations();
};
