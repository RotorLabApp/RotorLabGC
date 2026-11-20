#pragma once

#include "UnitTest.h"

/// G95 upload to the Fleet Server (custom/src/video/record/VideoUploader.h)
/// against an in-process mock of the S21 routes on 127.0.0.1 (never a real
/// server). Plain Qt.
class VideoUploaderTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _registerAndUpload();
    void _stationRouteWithoutRef();
    void _unknownFlightFallsBackToStation();
    void _headResumeAfterRestart();
    void _offset409();
    void _chunk400Resent();
    void _finalMismatchRestartsOnce();
    void _final200MismatchRestarts();
    void _stop401KeepsEverything();
    void _backoff503();
    void _wrongServerWaits();
    void _changedFileParked();
    void _fpsNullWhenUnknown();
    void _retryAfter503Honored();
    void _recordsStatusAndReceipt();
    void _recordsPollingCap();
    void _noFleetStaysLocal();
    void _httpPatchJson();
    void _earlyCloseOnLargeChunkParks();
    void _chunk413Parks();
};
