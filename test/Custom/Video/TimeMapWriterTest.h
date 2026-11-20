#pragma once

#include "UnitTest.h"

/// G95 time map sidecar (custom/src/video/record/TimeMapWriter.h) as the
/// recording writer fills it. The writer cases need the FFmpeg receiver build.
class TimeMapWriterTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _hashEqualsFile();
    void _headerFields();
    void _oneLinePerFragment();
    void _chainRecomputes();
    void _missingVehicleValuesLeftOut();
    void _armLinesAndFlightRef();
};
