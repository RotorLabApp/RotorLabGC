#pragma once

#include "UnitTest.h"

/// G95/G109 fragmented MP4 muxer (custom/src/video/common/Fmp4Muxer.h) and the
/// codec info it reads from the SPS. Needs the FFmpeg receiver build.
class Fmp4MuxerTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _codecInfoH264();
    void _codecInfoH265();
    void _codecInfoRejectsGarbage();
    void _h264BoxesParse();
    void _h265BoxesParse();
    void _appendOnlyOffsets();
    void _midGopCutIsNonSync();
    void _parameterSetChangeNeedsNewInit();
    void _startsOnKeyframe();
    void _byteLimitSplitsFragments();
};
