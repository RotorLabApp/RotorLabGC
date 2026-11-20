/****************************************************************************
 *
 * RotorLab GC - G109: Low and Medium encoder path.
 *
 ****************************************************************************/

#include "RelayEncoder.h"

#include <QtCore/QThread>

#include <algorithm>
#include <cmath>
#include <cstring>

void RelayEncoder::fitSize(int srcW, int srcH, int boxW, int boxH, int &outW, int &outH)
{
    outW = 0;
    outH = 0;
    if (srcW <= 0 || srcH <= 0 || boxW <= 0 || boxH <= 0) {
        return;
    }
    double scale = std::min(double(boxW) / srcW, double(boxH) / srcH);
    if (scale > 1.0) {
        scale = 1.0;    // never upscale
    }
    outW = std::max(2, int(std::lround(srcW * scale)) & ~1);
    outH = std::max(2, int(std::lround(srcH * scale)) & ~1);
}

#ifdef QGC_FFMPEG_STREAMING

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace {

constexpr int kMaxPixels = 4096 * 2304;     // decoder bound, as the receiver's
constexpr size_t kReceiveMapMax = 256;

bool isAnnexB(const uint8_t *d, int n)
{
    return (n >= 4 && d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 1)
           || (n >= 3 && d[0] == 0 && d[1] == 0 && d[2] == 1);
}

/// 4-byte length-prefixed NAL units to Annex B. Empty when the lengths do not add up.
QByteArray lengthPrefixedToAnnexB(const uint8_t *d, int n)
{
    QByteArray out;
    int pos = 0;
    while (pos + 4 <= n) {
        const quint32 len = (quint32(d[pos]) << 24) | (quint32(d[pos + 1]) << 16)
                            | (quint32(d[pos + 2]) << 8) | quint32(d[pos + 3]);
        pos += 4;
        if (len == 0 || len > quint32(n - pos)) {
            return {};
        }
        out.append("\x00\x00\x00\x01", 4);
        out.append(reinterpret_cast<const char *>(d + pos), int(len));
        pos += int(len);
    }
    return pos == n ? out : QByteArray();
}

} // namespace

RelayEncoder::RelayEncoder(const EncoderChoice &encoder, RelayQuality quality, Output out)
    : _choice(encoder)
    , _quality(quality)
    , _target(relayQualityTarget(quality))
    , _out(std::move(out))
    , _meter(relayQualityTarget(quality).maxFps)
{
    _pkt = av_packet_alloc();
    _frame = av_frame_alloc();
    _encPkt = av_packet_alloc();
}

RelayEncoder::~RelayEncoder()
{
    _closeEncoder();
    _closeDecoder();
    av_packet_free(&_pkt);
    av_frame_free(&_frame);
    av_packet_free(&_encPkt);
}

bool RelayEncoder::_openDecoder(VideoTapCodec codec)
{
    _closeDecoder();
    const AVCodec *dec = avcodec_find_decoder(codec == VideoTapCodec::H265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
    if (!dec) {
        _error = QStringLiteral("no decoder");
        return false;
    }
    _dec = avcodec_alloc_context3(dec);
    if (!_dec) {
        return false;
    }
    _dec->flags |= AV_CODEC_FLAG_LOW_DELAY;
    _dec->thread_type = FF_THREAD_SLICE;
    _dec->thread_count = 0;
    _dec->max_pixels = kMaxPixels;
    _dec->pkt_timebase = AVRational{1, 90000};
    if (avcodec_open2(_dec, dec, nullptr) < 0) {
        avcodec_free_context(&_dec);
        _error = QStringLiteral("the decoder did not open");
        return false;
    }
    _decCodec = codec;
    _setupInPush = true;
    return true;
}

void RelayEncoder::_closeDecoder()
{
    if (_dec) {
        avcodec_free_context(&_dec);
    }
}

bool RelayEncoder::_openEncoder(int w, int h)
{
    _closeEncoder();
    QString err;
    const int aim = relayEncoderBitRate(_quality);
    _enc = EncoderProbe::openEncoder(_choice, w, h, _target.maxFps, aim, &err);
    if (!_enc) {
        _error = QStringLiteral("%1: %2").arg(_choice.label(), err);
        qCWarning(VideoRelayLog) << "relay encoder did not open:" << _error;
        return false;
    }
    _encFrame = av_frame_alloc();
    if (!_encFrame) {
        _closeEncoder();
        return false;
    }
    _encFrame->format = _enc->pix_fmt;
    _encFrame->width = w;
    _encFrame->height = h;
    if (av_frame_get_buffer(_encFrame, 0) < 0) {
        _closeEncoder();
        return false;
    }
    _outW = w;
    _outH = h;
    ++_outGeneration;
    _outParams.clear();
    if (_enc->extradata && _enc->extradata_size > 0 && isAnnexB(_enc->extradata, _enc->extradata_size)) {
        _outParams.scanAnnexB(QByteArray(reinterpret_cast<const char *>(_enc->extradata), _enc->extradata_size));
    }
    _forceKey = true;
    _lastEncPts = -1;
    _setupInPush = true;
    ++_stats.encoderOpens;
    qCInfo(VideoRelayLog) << "relay encoder" << _choice.label() << w << "x" << h << "aim" << aim
                          << "b/s, cap" << _target.bitRate << "b/s," << _target.maxFps << "fps";
    if (_restarted) {
        _restarted();
    }
    return true;
}

void RelayEncoder::_closeEncoder()
{
    if (_enc) {
        avcodec_free_context(&_enc);
    }
    if (_encFrame) {
        av_frame_free(&_encFrame);
    }
    if (_sws) {
        sws_freeContext(_sws);
        _sws = nullptr;
    }
    _srcW = _srcH = 0;
    _srcFmt = -1;
}

qint64 RelayEncoder::_unwrapPts(const RelayUnit &u)
{
    qint64 pts = u.pts90k >= 0 ? u.pts90k : u.dts90k;
    if (pts < 0) {
        // No media time at all: the receive clock in 90 kHz.
        pts = u.receiveSteadyNs / 100000 * 9;
    } else if (u.hasRtpTimestamp) {
        const quint32 raw = quint32(quint64(pts));
        if (_rtpLast >= 0 && _ptsLast >= 0) {
            pts = _ptsLast + qint32(raw - quint32(_rtpLast));
        } else {
            pts = raw;
        }
        _rtpLast = raw;
    }
    _ptsLast = pts;
    return pts;
}

void RelayEncoder::push(const RelayUnit &unit)
{
    const qint64 t0 = relaySteadyNowNs();
    const qint64 c0 = relayThreadCpuNs();
    _meter.begin(t0);
    _setupInPush = false;
    _push(unit);
    const qint64 t1 = relaySteadyNowNs();
    const qint64 c1 = relayThreadCpuNs();
    const qint64 used = (c0 >= 0 && c1 >= c0) ? c1 - c0 : t1 - t0;
    if (_meter.end(t1, used, _setupInPush)) {
        qCWarning(VideoRelayLog) << "relay encoder cannot keep up at" << relayQualityKey(_quality)
                                 << "busy" << _meter.busyRatio() << "late frames" << _meter.lateFrames()
                                 << "of" << _meter.frames();
    }
}

void RelayEncoder::_push(const RelayUnit &unit)
{
    ++_stats.unitsIn;

    if (!_haveGeneration || unit.generation != _generation || !_dec || unit.codec != _decCodec) {
        // A new source generation on an encoder that stays open (same size):
        // a new picture for its rate control all the same.
        const bool newSource = _haveGeneration && unit.generation != _generation && _enc;
        _haveGeneration = true;
        _generation = unit.generation;
        _waitKey = true;
        _rtpLast = -1;
        _ptsLast = -1;
        _lastOutPts = -1;
        _lastKeyPts = -1;
        _receiveByPts.clear();
        if (newSource) {
            ++_stats.sourceRestarts;
            if (_restarted) {
                _restarted();
            }
        }
        if (!_openDecoder(unit.codec)) {
            return;
        }
    }
    if (_waitKey) {
        if (!unit.keyFrame) {
            return;
        }
        _waitKey = false;
    }

    QByteArray data = unit.data;
    if (unit.keyFrame && !unit.parameterSets.isEmpty()) {
        data.prepend(unit.parameterSets);
    }
    const qint64 pts = _unwrapPts(unit);
    if (av_new_packet(_pkt, int(data.size())) < 0) {
        return;
    }
    memcpy(_pkt->data, data.constData(), size_t(data.size()));
    _pkt->pts = pts;
    _pkt->dts = pts;
    if (unit.keyFrame) {
        _pkt->flags |= AV_PKT_FLAG_KEY;
    }
    _receiveByPts[pts] = unit.receiveSteadyNs;
    while (_receiveByPts.size() > kReceiveMapMax) {
        _receiveByPts.erase(_receiveByPts.begin());
    }

    int r = avcodec_send_packet(_dec, _pkt);
    av_packet_unref(_pkt);
    if (r < 0 && r != AVERROR(EAGAIN)) {
        ++_stats.decodeErrors;
        _waitKey = true;    // resync at the next keyframe
    }
    while (true) {
        r = avcodec_receive_frame(_dec, _frame);
        if (r < 0) {
            break;
        }
        ++_stats.framesDecoded;
        _onFrame(_frame);
        av_frame_unref(_frame);
    }
}

void RelayEncoder::_onFrame(AVFrame *frame)
{
    qint64 pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
    if (pts == AV_NOPTS_VALUE) {
        pts = _ptsLast;
    }
    const int fps = std::max(1, _target.maxFps);
    const qint64 interval = 90000 / fps;
    // Drop to the level's frame rate (a small tolerance for jitter).
    if (_lastOutPts >= 0 && pts - _lastOutPts < interval - interval / 8) {
        ++_stats.droppedForRate;
        return;
    }

    if (!_enc || frame->width != _srcW || frame->height != _srcH || frame->format != _srcFmt) {
        int w = 0, h = 0;
        fitSize(frame->width, frame->height, _target.width, _target.height, w, h);
        if (w <= 0 || h <= 0) {
            return;
        }
        if (!_enc || w != _outW || h != _outH) {
            if (!_openEncoder(w, h)) {
                return;
            }
        }
        _sws = sws_getCachedContext(_sws, frame->width, frame->height, AVPixelFormat(frame->format),
                                    _outW, _outH, _enc->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!_sws) {
            _error = QStringLiteral("cannot scale this picture format");
            return;
        }
        _srcW = frame->width;
        _srcH = frame->height;
        _srcFmt = frame->format;
    }
    if (av_frame_make_writable(_encFrame) < 0) {
        return;
    }
    sws_scale(_sws, frame->data, frame->linesize, 0, frame->height, _encFrame->data, _encFrame->linesize);

    if (_lastEncPts >= 0 && pts <= _lastEncPts) {
        pts = _lastEncPts + 1;  // the encoder needs increasing times
    }
    _lastOutPts = pts;
    _lastEncPts = pts;
    _encFrame->pts = pts;
    const bool key = _forceKey || _lastKeyPts < 0 || pts - _lastKeyPts >= 90000;
    if (key) {
        _encFrame->pict_type = AV_PICTURE_TYPE_I;
        _encFrame->flags |= AV_FRAME_FLAG_KEY;
        _forceKey = false;
        _lastKeyPts = pts;
    } else {
        _encFrame->pict_type = AV_PICTURE_TYPE_NONE;
        _encFrame->flags &= ~AV_FRAME_FLAG_KEY;
    }

    const auto it = _receiveByPts.find(frame->pts != AV_NOPTS_VALUE ? frame->pts : pts);
    const qint64 receivedNs = it != _receiveByPts.end() ? it->second : 0;
    const qint64 encodeAtNs = relaySteadyNowNs();

    if (_injectedDelayMs > 0) {
        // CPU work, not a sleep: the cap measures this thread's CPU time.
        const qint64 until = relaySteadyNowNs() + qint64(_injectedDelayMs) * 1'000'000;
        volatile quint64 spin = 0;
        while (relaySteadyNowNs() < until) {
            spin = spin + 1;
        }
    }

    int r = avcodec_send_frame(_enc, _encFrame);
    if (r == AVERROR(EAGAIN)) {
        _drainEncoder();
        r = avcodec_send_frame(_enc, _encFrame);
    }
    if (r < 0) {
        ++_stats.encodeErrors;
        return;
    }
    ++_stats.framesEncoded;
    _meter.frameEncoded(receivedNs, encodeAtNs);
    _drainEncoder();
}

void RelayEncoder::_drainEncoder()
{
    if (!_enc) {
        return;
    }
    while (avcodec_receive_packet(_enc, _encPkt) >= 0) {
        _emitPacket(_encPkt);
        av_packet_unref(_encPkt);
    }
}

void RelayEncoder::_emitPacket(AVPacket *pkt)
{
    if (!pkt->data || pkt->size <= 0) {
        return;
    }
    QByteArray data = isAnnexB(pkt->data, pkt->size)
        ? QByteArray(reinterpret_cast<const char *>(pkt->data), pkt->size)
        : lengthPrefixedToAnnexB(pkt->data, pkt->size);
    if (data.isEmpty()) {
        ++_stats.encodeErrors;
        return;
    }
    const bool key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
    if (key || _outParams.isEmpty()) {
        _outParams.scanAnnexB(data);
    }
    RelayUnit u;
    u.codec = VideoTapCodec::H264;
    u.data = data;
    u.parameterSets = _outParams.annexB();
    u.keyFrame = key;
    u.pts90k = pkt->pts != AV_NOPTS_VALUE ? av_rescale_q(pkt->pts, _enc->time_base, AVRational{1, 90000}) : -1;
    u.dts90k = pkt->dts != AV_NOPTS_VALUE ? av_rescale_q(pkt->dts, _enc->time_base, AVRational{1, 90000}) : u.pts90k;
    const auto it = _receiveByPts.lower_bound(u.pts90k);
    u.receiveSteadyNs = it != _receiveByPts.end() ? it->second : relaySteadyNowNs();
    u.generation = _outGeneration;
    ++_stats.packetsOut;
    if (_out) {
        _out(u);
    }
}

void RelayEncoder::flush()
{
    if (_enc) {
        avcodec_send_frame(_enc, nullptr);
        _drainEncoder();
        // The encoder cannot take frames after a flush: open a fresh one next time.
        _closeEncoder();
    }
}

#else // no FFmpeg in this build: Low and Medium are never offered

RelayEncoder::RelayEncoder(const EncoderChoice &encoder, RelayQuality quality, Output out)
    : _choice(encoder), _quality(quality), _target(relayQualityTarget(quality)), _out(std::move(out))
{
    _error = QStringLiteral("this build has no FFmpeg");
}

RelayEncoder::~RelayEncoder() = default;

void RelayEncoder::push(const RelayUnit &) {}
void RelayEncoder::flush() {}

#endif
