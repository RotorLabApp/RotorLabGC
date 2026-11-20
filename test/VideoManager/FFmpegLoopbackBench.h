#pragma once

#include "UnitTest.h"

/// G88: receive-and-decode measurement for the FFmpeg receiver (test only).
/// Standalone (runs only as --unittest:FFmpegLoopbackBench) and only useful with a sender
/// on the port (an RTP sender, not part of this repository).
/// QGC_G88_WINDOW=1 draws into a QQuickWindow with a VideoOutput (socket to swap); otherwise
/// frames go to a plain QVideoSink (socket to setVideoFrame).
/// Environment: QGC_G88_PORT (5600), QGC_G88_SECONDS (60), QGC_FFMPEG_STATS_DIR (the CSV files).
class FFmpegLoopbackBench : public UnitTest
{
    Q_OBJECT

private slots:
    void _receive();
};
