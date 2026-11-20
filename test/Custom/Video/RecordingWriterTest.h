#pragma once

#include "UnitTest.h"

/// G95 recording writer (custom/src/video/record/RecordingWriter.h): fed tap
/// items directly, writing into a temporary folder. Needs the FFmpeg receiver build.
class RecordingWriterTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _hashEqualsFile();
    void _startsOnKeyframe();
    void _partSplitAtSizeLimit();
    void _newGenerationNewPart();
    void _discontinuityWritesGap();
    void _diskLowStops();
    void _sourceStopClosesPart();
};
