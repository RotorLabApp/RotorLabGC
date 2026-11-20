/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtMultimedia/QVideoFrame>

struct AVFrame;

/// G88: a decoded AVFrame as a QVideoFrame for QVideoSink::setVideoFrame.
/// Software frames only for now: YUV420P (and the full-range YUVJ420P) and NV12, copied
/// plane by plane. Hardware frames (d3d11va, mediacodec) are not used yet.
namespace FFmpegFrameBridge
{
    /// Returns an invalid QVideoFrame for a pixel format it does not handle.
    QVideoFrame toVideoFrame(const AVFrame *frame);

    /// FFmpeg's name for the frame's pixel format, for logs.
    const char *pixelFormatName(const AVFrame *frame);
}
