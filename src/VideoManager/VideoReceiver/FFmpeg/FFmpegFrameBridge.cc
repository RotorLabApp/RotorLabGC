/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "FFmpegFrameBridge.h"

#include <QtMultimedia/QVideoFrameFormat>

#include <cstring>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

namespace {

void copyPlane(uchar *dst, int dstStride, const uint8_t *src, int srcStride, int rowBytes, int rows)
{
    if (dstStride == srcStride && srcStride == rowBytes) {
        std::memcpy(dst, src, static_cast<size_t>(rowBytes) * rows);
        return;
    }
    for (int y = 0; y < rows; ++y) {
        std::memcpy(dst + static_cast<qsizetype>(y) * dstStride, src + static_cast<qsizetype>(y) * srcStride, rowBytes);
    }
}

QVideoFrameFormat::ColorSpace colorSpace(const AVFrame *frame)
{
    switch (frame->colorspace) {
    case AVCOL_SPC_BT709:
        return QVideoFrameFormat::ColorSpace_BT709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        return QVideoFrameFormat::ColorSpace_BT601;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        return QVideoFrameFormat::ColorSpace_BT2020;
    default:
        // Unspecified: HD sizes are BT.709 by convention, SD BT.601
        return (frame->height >= 720) ? QVideoFrameFormat::ColorSpace_BT709 : QVideoFrameFormat::ColorSpace_BT601;
    }
}

} // namespace

const char *FFmpegFrameBridge::pixelFormatName(const AVFrame *frame)
{
    if (!frame) {
        return "none";
    }
    const char *name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format));
    return name ? name : "unknown";
}

QVideoFrame FFmpegFrameBridge::toVideoFrame(const AVFrame *frame)
{
    if (!frame || (frame->width <= 0) || (frame->height <= 0)) {
        return QVideoFrame();
    }

    const AVPixelFormat pixFmt = static_cast<AVPixelFormat>(frame->format);
    QVideoFrameFormat::PixelFormat qtFormat;
    bool fullRange = (frame->color_range == AVCOL_RANGE_JPEG);
    switch (pixFmt) {
    case AV_PIX_FMT_YUV420P:
        qtFormat = QVideoFrameFormat::Format_YUV420P;
        break;
    case AV_PIX_FMT_YUVJ420P:
        qtFormat = QVideoFrameFormat::Format_YUV420P;
        fullRange = true;
        break;
    case AV_PIX_FMT_NV12:
        qtFormat = QVideoFrameFormat::Format_NV12;
        break;
    default:
        return QVideoFrame();
    }

    const int w = frame->width;
    const int h = frame->height;
    const int cw = (w + 1) / 2;
    const int ch = (h + 1) / 2;

    // The decoder's planes must hold what is copied: never read past them
    const bool planar = (qtFormat == QVideoFrameFormat::Format_YUV420P);
    const int planes = planar ? 3 : 2;
    const int rowBytes[3] = { w, planar ? cw : cw * 2, cw };
    for (int i = 0; i < planes; ++i) {
        if (!frame->data[i] || (frame->linesize[i] < rowBytes[i])) {
            return QVideoFrame();
        }
    }

    QVideoFrameFormat format(QSize(w, h), qtFormat);
    format.setColorSpace(colorSpace(frame));
    format.setColorRange(fullRange ? QVideoFrameFormat::ColorRange_Full : QVideoFrameFormat::ColorRange_Video);
    format.setColorTransfer(QVideoFrameFormat::ColorTransfer_BT709);

    QVideoFrame out(format);
    if (!out.map(QVideoFrame::WriteOnly)) {
        return QVideoFrame();
    }
    for (int i = 0; i < planes; ++i) {
        if (!out.bits(i) || (out.bytesPerLine(i) < rowBytes[i])) {
            out.unmap();
            return QVideoFrame();
        }
    }

    copyPlane(out.bits(0), out.bytesPerLine(0), frame->data[0], frame->linesize[0], w, h);
    if (planar) {
        copyPlane(out.bits(1), out.bytesPerLine(1), frame->data[1], frame->linesize[1], cw, ch);
        copyPlane(out.bits(2), out.bytesPerLine(2), frame->data[2], frame->linesize[2], cw, ch);
    } else {
        copyPlane(out.bits(1), out.bytesPerLine(1), frame->data[1], frame->linesize[1], cw * 2, ch);
    }
    out.unmap();

    return out;
}
