#pragma once

#include "UnitTest.h"

/// G83 (GC 5.1.3): choosing "UDP h.264 Video Stream" crashed GC because the QML
/// video widget was cast to a VideoOutput without checking its type. These tests
/// cover the refusal of a widget that is not a video output and the fall back to
/// no video when the saved video source cannot be set up.
///
/// G84 (GC 5.2.0): video URL credentials are kept out of the logs, a vehicle's
/// VIDEO_STREAM_INFORMATION applies for the session only, the restart timer and
/// the video widget are never used after they are gone, and a screenshot fails
/// cleanly with Qt Multimedia video.
///
/// G67 (GC 5.3.0): "video is off" says which source is meant. A stream offered by the
/// aircraft gets its own text and the non-modal banner, once for each connection.
///
/// G88 (GC 5.3.0): with GStreamer off the Fly view's video item is a VideoOutput and GC's own
/// FFmpeg receiver decodes into it; a source the build cannot play gets the G83 message.
class VideoSinkTest : public UnitTest
{
    Q_OBJECT

private slots:
    void init() override;
    void cleanup() override;

    void _nonVideoWidgetRefused();
    void _savedSourceFallsBackToNoVideo();
    void _videoOffMessageChoice();
    void _vehicleStreamClearedOnDisconnect();

    void _redactUri();
    void _autoStreamRtspIsSessionOnly();
    void _autoStreamUnknownTypeKeepsSavedSettings();
    void _autoStreamUdpH265();
    void _herelinkSource();
    void _thermalSinkFailsMainWorks();
    void _restartPaths();
    void _restartAfterVideoManagerDestroyed();
    void _restartAfterReceiverDestroyed();
    void _widgetDestroyedClearsSink();
    void _screenshotFailsCleanly();
    void _streamSourceUsesVideoOutput();
    void _cannotPlayShowsMessage();
};
