/****************************************************************************
 *
 * RotorLab GC - G109: encoder probe and the quality offer.
 *
 ****************************************************************************/

#include "EncoderProbe.h"

#include <QtCore/QElapsedTimer>

#include <cstring>

#ifdef QGC_FFMPEG_STREAMING
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
#ifdef Q_OS_ANDROID
extern "C" {
#include <libavcodec/jni.h>
}
#include <QtCore/QJniEnvironment>
#endif
#endif

// ---------------------------------------------------------------- offer

QString QualityOffer::noEncoderText()
{
    return tr("Low and Medium need a video encoder this computer does not offer.");
}

QString QualityOffer::fullNeedsH264Text()
{
    return tr("Full live relay needs an H.264 camera.");
}

QString QualityOffer::needsH264Text()
{
    return tr("Live relay needs an H.264 camera on this computer.");
}

QString QualityOffer::levelText(RelayQuality q)
{
    switch (q) {
    case RelayQuality::Low:    return tr("Low: 640x360, about 0.8 Mb/s");
    case RelayQuality::Medium: return tr("Medium: 1280x720, about 2.5 Mb/s");
    case RelayQuality::Full:   break;
    }
    return tr("Full: as received from the camera");
}

QualityOffer QualityOffer::compute(VideoTapCodec codec, bool codecKnown, bool encoderOk)
{
    QualityOffer o;
    const bool h265 = codecKnown && codec == VideoTapCodec::H265;
    if (encoderOk) {
        o.levels << RelayQuality::Low << RelayQuality::Medium;
    }
    if (!h265) {
        o.levels << RelayQuality::Full;
    }
    if (o.levels.isEmpty()) {
        o.relayAllowed = false;
        o.reason = needsH264Text();
    } else if (h265) {
        o.reason = fullNeedsH264Text();
    } else if (!encoderOk) {
        o.reason = noEncoderText();
    }
    return o;
}

// ---------------------------------------------------------------- probe

QString EncoderChoice::label() const
{
    QString s = name;
    if (name == QLatin1String("h264_mf")) {
        s = hardware ? QStringLiteral("Media Foundation (hardware)")
                     : QStringLiteral("Media Foundation (Windows software encoder)");
    } else if (name == QLatin1String("h264_mediacodec")) {
        s = QStringLiteral("MediaCodec");
    }
    return s;
}

QList<EncoderChoice> EncoderProbe::candidates()
{
    QList<EncoderChoice> out;
#if defined(Q_OS_WIN)
    EncoderChoice hw;
    hw.name = QStringLiteral("h264_mf");
    hw.options = QStringLiteral("hw_encoding=1,rate_control=cbr,scenario=live_streaming");
    hw.hardware = true;
    out << hw;
    EncoderChoice sw;
    sw.name = QStringLiteral("h264_mf");
    sw.options = QStringLiteral("hw_encoding=0,rate_control=cbr,scenario=live_streaming");
    sw.hardware = false;
    out << sw;
#elif defined(Q_OS_ANDROID)
    EncoderChoice mc;
    mc.name = QStringLiteral("h264_mediacodec");
    mc.options = QStringLiteral("bitrate_mode=cbr");
    mc.hardware = true;
    out << mc;
#endif
    return out;
}

bool EncoderProbe::buildHasEncoders()
{
#ifdef QGC_FFMPEG_STREAMING
    return !candidates().isEmpty();
#else
    return false;
#endif
}

EncoderProbeReport EncoderProbe::run(int width, int height, int frames)
{
    EncoderProbeReport report;
    for (const EncoderChoice &c : candidates()) {
        const EncoderChoice r = probeOne(c, width, height, frames);
        report.tried << r;
        qCInfo(VideoRelayLog) << "encoder probe" << r.label() << (r.ok ? "opened" : "failed")
                              << (r.ok ? QStringLiteral("%1 ms per frame").arg(r.msPerFrame, 0, 'f', 2) : r.detail);
        if (r.ok) {
            report.chosen = r;
            break;
        }
    }
    return report;
}

#ifdef QGC_FFMPEG_STREAMING

namespace {

QString avErr(int err)
{
    char buf[256] = {};
    av_strerror(err, buf, sizeof(buf));
    return QString::fromUtf8(buf);
}

AVPixelFormat pickPixFmt(const AVCodecContext *ctx, const AVCodec *codec)
{
    const void *cfg = nullptr;
    int n = 0;
    if (avcodec_get_supported_config(ctx, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &cfg, &n) >= 0 && cfg && n > 0) {
        const AVPixelFormat *fmts = static_cast<const AVPixelFormat *>(cfg);
        for (AVPixelFormat want : {AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P}) {
            for (int i = 0; i < n; ++i) {
                if (fmts[i] == want) {
                    return want;
                }
            }
        }
        return AV_PIX_FMT_NONE;
    }
    return AV_PIX_FMT_NV12;
}

void fillTestFrame(AVFrame *f, int index)
{
    // A moving gradient, so the encoder has something to encode.
    for (int y = 0; y < f->height; ++y) {
        uint8_t *row = f->data[0] + qsizetype(y) * f->linesize[0];
        for (int x = 0; x < f->width; ++x) {
            row[x] = uint8_t((x + y + index * 4) & 0xff);
        }
    }
    const int ch = (f->height + 1) / 2;
    for (int p = 1; p < 3; ++p) {
        if (!f->data[p]) {
            continue;
        }
        for (int y = 0; y < ch; ++y) {
            memset(f->data[p] + qsizetype(y) * f->linesize[p], 128, size_t(f->linesize[p]));
        }
    }
}

} // namespace

AVCodecContext *EncoderProbe::openEncoder(const EncoderChoice &choice, int width, int height,
                                          int fps, int bitRate, QString *error)
{
    const AVCodec *codec = avcodec_find_encoder_by_name(choice.name.toLatin1().constData());
    if (!codec) {
        if (error) *error = QStringLiteral("not in this FFmpeg");
        return nullptr;
    }
#ifdef Q_OS_ANDROID
    if (choice.name == QLatin1String("h264_mediacodec") && !av_jni_get_java_vm(nullptr)) {
        // Qt's FFmpeg plugin normally hands FFmpeg the Java VM; make sure.
        av_jni_set_java_vm(QJniEnvironment::javaVM(), nullptr);
    }
#endif
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        if (error) *error = QStringLiteral("out of memory");
        return nullptr;
    }
    const AVPixelFormat pix = pickPixFmt(ctx, codec);
    if (pix == AV_PIX_FMT_NONE) {
        avcodec_free_context(&ctx);
        if (error) *error = QStringLiteral("takes neither NV12 nor YUV420P input");
        return nullptr;
    }
    fps = qBound(1, fps, 60);
    ctx->width = width;
    ctx->height = height;
    ctx->pix_fmt = pix;
    ctx->time_base = AVRational{1, 90000};
    ctx->framerate = AVRational{fps, 1};
    ctx->gop_size = fps;            // 1 s GOP
    ctx->max_b_frames = 0;          // live: no B-frames
    ctx->bit_rate = bitRate;
    ctx->rc_max_rate = bitRate;
    // Half a second of VBV: a tighter buffer keeps each second's output near
    // the target. h264_mf hands bit_rate, rc_max_rate and rc_buffer_size to
    // the MFT (AVEncCommonMeanBitRate, AVEncCommonMaxBitRate,
    // AVEncCommonBufferSize); an MFT may still overshoot, so the relay also
    // aims below the cap (relayEncoderBitRate) and the guard judges the cap.
    ctx->rc_buffer_size = bitRate / 2;
    ctx->profile = AV_PROFILE_H264_MAIN;

    AVDictionary *opts = nullptr;
    for (const QString &kv : choice.options.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const qsizetype eq = kv.indexOf(QLatin1Char('='));
        if (eq > 0) {
            av_dict_set(&opts, kv.left(eq).toLatin1().constData(), kv.mid(eq + 1).toLatin1().constData(), 0);
        }
    }
    const int ret = avcodec_open2(ctx, codec, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        if (error) *error = QStringLiteral("avcodec_open2: %1").arg(avErr(ret));
        avcodec_free_context(&ctx);
        return nullptr;
    }
    return ctx;
}

EncoderChoice EncoderProbe::probeOne(EncoderChoice c, int width, int height, int frames)
{
    c.ok = false;
    QString err;
    AVCodecContext *ctx = openEncoder(c, width, height, 30, 800000, &err);
    if (!ctx) {
        c.detail = err;
        return c;
    }
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    int packets = 0;
    bool failed = false;
    QElapsedTimer timer;
    if (!frame || !pkt) {
        c.detail = QStringLiteral("out of memory");
        failed = true;
    } else {
        frame->format = ctx->pix_fmt;
        frame->width = width;
        frame->height = height;
        if (av_frame_get_buffer(frame, 0) < 0) {
            c.detail = QStringLiteral("no frame buffer");
            failed = true;
        }
    }
    const auto drain = [&]() {
        while (true) {
            const int r = avcodec_receive_packet(ctx, pkt);
            if (r < 0) {
                return r;
            }
            ++packets;
            av_packet_unref(pkt);
        }
    };
    timer.start();
    for (int i = 0; i < frames && !failed; ++i) {
        if (av_frame_make_writable(frame) < 0) {
            failed = true;
            break;
        }
        fillTestFrame(frame, i);
        frame->pts = qint64(i) * 3000;
        int r = avcodec_send_frame(ctx, frame);
        if (r == AVERROR(EAGAIN)) {
            drain();
            r = avcodec_send_frame(ctx, frame);
        }
        if (r < 0) {
            c.detail = QStringLiteral("send_frame: %1").arg(avErr(r));
            failed = true;
            break;
        }
        const int d = drain();
        if (d < 0 && d != AVERROR(EAGAIN) && d != AVERROR_EOF) {
            c.detail = QStringLiteral("receive_packet: %1").arg(avErr(d));
            failed = true;
        }
    }
    if (!failed) {
        avcodec_send_frame(ctx, nullptr);
        drain();
        const qint64 ns = timer.nsecsElapsed();
        c.msPerFrame = frames > 0 ? double(ns) / 1e6 / frames : 0;
        c.ok = packets > 0;
        if (!c.ok) {
            c.detail = QStringLiteral("opened but gave no output");
        }
    }
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return c;
}

#else // no FFmpeg receiver in this build

AVCodecContext *EncoderProbe::openEncoder(const EncoderChoice &, int, int, int, int, QString *error)
{
    if (error) *error = QStringLiteral("this build has no FFmpeg");
    return nullptr;
}

EncoderChoice EncoderProbe::probeOne(EncoderChoice c, int, int, int)
{
    c.ok = false;
    c.detail = QStringLiteral("this build has no FFmpeg");
    return c;
}

#endif
