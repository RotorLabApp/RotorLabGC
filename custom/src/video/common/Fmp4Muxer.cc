/****************************************************************************
 *
 * RotorLab GC - fragmented MP4 from received access units (G95, G109).
 *
 ****************************************************************************/

#include "Fmp4Muxer.h"

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

#include "QGCLoggingCategory.h"

QGC_LOGGING_CATEGORY(Fmp4MuxerLog, "gcs.custom.video.mux")

namespace {

constexpr int kIoBufferSize = 64 * 1024;
const AVRational kTimeBase90k{1, 90000};

#if LIBAVFORMAT_VERSION_MAJOR >= 61
using AvioWriteBuf = const uint8_t *;
#else
using AvioWriteBuf = uint8_t *;
#endif

QString avError(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return QString::fromLatin1(buf);
}

} // namespace

/// The AVIO write callback. No read and no seek callback: libavformat can
/// only append.
struct Fmp4MuxerIo
{
    static int write(void *opaque, AvioWriteBuf buf, int size)
    {
        if (size > 0) {
            static_cast<Fmp4Muxer *>(opaque)->_sink(reinterpret_cast<const char *>(buf), size);
        }
        return size;
    }
};

Fmp4Muxer::Fmp4Muxer(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<Fmp4FragmentInfo>("Fmp4FragmentInfo");
}

Fmp4Muxer::~Fmp4Muxer()
{
    _close();
}

bool Fmp4Muxer::available()
{
    return av_guess_format("mp4", nullptr, nullptr) != nullptr;
}

bool Fmp4Muxer::needsNewInit(Codec codec, const QByteArray &parameterSets) const
{
    if (!isOpen()) {
        return true;
    }
    return codec != _config.codec || (!parameterSets.isEmpty() && parameterSets != _config.parameterSets);
}

bool Fmp4Muxer::_fail(const QString &what, int averror)
{
    _error = averror ? QStringLiteral("%1: %2").arg(what, avError(averror)) : what;
    qCWarning(Fmp4MuxerLog) << "fMP4 muxer:" << _error;
    return false;
}

QByteArray Fmp4Muxer::_takeOut()
{
    QByteArray bytes;
    bytes.swap(_out);
    return bytes;
}

void Fmp4Muxer::_close()
{
    if (_fmt) {
        avformat_free_context(_fmt);
        _fmt = nullptr;
    }
    if (_io) {
        av_freep(&_io->buffer);
        avio_context_free(&_io);
    }
    _out.clear();
    _started = false;
    _hasHeld = false;
    _held = Held();
    _fragOpen = false;
    _cutRequested = false;
}

void Fmp4Muxer::abort()
{
    _close();
}

bool Fmp4Muxer::open(const Config &config)
{
    _close();
    _error.clear();
    _config = config;
    _seq = 0;
    _bytesOut = 0;
    _samplesTotal = 0;
    _lastDts = -1;
    _lastDuration = 3000;
    _warnedTimes = false;
    _fragBytes = 0;
    _fragSamples = 0;
    _fragDuration = 0;

    const bool h265 = (config.codec == Codec::H265);
    if (config.parameterSets.isEmpty()) {
        return _fail(QStringLiteral("no parameter sets"));
    }
    _info = VideoCodecInfo::fromAnnexB(h265, config.parameterSets);
    const int width = config.width > 0 ? config.width : _info.width;
    const int height = config.height > 0 ? config.height : _info.height;

    const AVOutputFormat *mp4 = av_guess_format("mp4", nullptr, nullptr);
    if (!mp4) {
        return _fail(QStringLiteral("this build's FFmpeg has no MP4 muxer"));
    }
    int ret = avformat_alloc_output_context2(&_fmt, mp4, nullptr, nullptr);
    if (ret < 0 || !_fmt) {
        _fmt = nullptr;
        return _fail(QStringLiteral("cannot create the MP4 muxer"), ret);
    }

    auto *buffer = static_cast<unsigned char *>(av_malloc(kIoBufferSize));
    if (!buffer) {
        _close();
        return _fail(QStringLiteral("out of memory"));
    }
    _io = avio_alloc_context(buffer, kIoBufferSize, 1, this, nullptr, &Fmp4MuxerIo::write, nullptr);
    if (!_io) {
        av_free(buffer);
        _close();
        return _fail(QStringLiteral("out of memory"));
    }
    _io->seekable = 0;
    _fmt->pb = _io;
    // Custom I/O; bit-exact so the same input gives the same bytes (no
    // encoder version string), which also makes the output testable.
    _fmt->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_BITEXACT;

    AVStream *st = avformat_new_stream(_fmt, nullptr);
    if (!st) {
        _close();
        return _fail(QStringLiteral("cannot add the video track"));
    }
    st->time_base = kTimeBase90k;
    AVCodecParameters *par = st->codecpar;
    par->codec_type = AVMEDIA_TYPE_VIDEO;
    par->codec_id = h265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
    if (h265) {
        par->codec_tag = MKTAG('h', 'v', 'c', '1');
    }
    par->width = width;
    par->height = height;
    const int psSize = int(config.parameterSets.size());
    par->extradata = static_cast<uint8_t *>(av_mallocz(size_t(psSize) + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!par->extradata) {
        _close();
        return _fail(QStringLiteral("out of memory"));
    }
    std::memcpy(par->extradata, config.parameterSets.constData(), size_t(psSize));
    par->extradata_size = psSize;

    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "movflags", "empty_moov+default_base_moof+frag_custom+skip_trailer", 0);
    ret = avformat_write_header(_fmt, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        _close();
        return _fail(QStringLiteral("cannot start the MP4 stream"), ret);
    }
    avio_flush(_io);
    const QByteArray init = _takeOut();
    if (init.isEmpty()) {
        _close();
        return _fail(QStringLiteral("the MP4 muxer wrote no init segment"));
    }
    _bytesOut = init.size();
    emit initSegment(init);
    return true;
}

bool Fmp4Muxer::_writeHeld(qint64 duration)
{
    if (!_hasHeld) {
        return true;
    }
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        return _fail(QStringLiteral("out of memory"));
    }
    // A padded copy: libavformat's NAL scanners may read a little past the end.
    int ret = av_new_packet(pkt, int(_held.data.size()));
    if (ret < 0) {
        av_packet_free(&pkt);
        return _fail(QStringLiteral("out of memory"), ret);
    }
    std::memcpy(pkt->data, _held.data.constData(), size_t(_held.data.size()));
    pkt->stream_index = 0;
    pkt->dts = _held.dts;
    pkt->pts = _held.pts;
    pkt->duration = qMax(qint64(1), duration);
    if (_held.keyFrame) {
        pkt->flags |= AV_PKT_FLAG_KEY;
    }
    av_packet_rescale_ts(pkt, kTimeBase90k, _fmt->streams[0]->time_base);
    ret = av_write_frame(_fmt, pkt);
    av_packet_free(&pkt);
    if (ret < 0) {
        return _fail(QStringLiteral("cannot write a frame"), ret);
    }
    _fragBytes += _held.data.size();
    _fragSamples += 1;
    _fragDuration += qMax(qint64(1), duration);
    _samplesTotal += 1;
    _hasHeld = false;
    _held = Held();
    return true;
}

bool Fmp4Muxer::_flushFragment()
{
    if (!_fragOpen || _fragSamples == 0) {
        return true;
    }
    const int ret = av_write_frame(_fmt, nullptr);
    if (ret < 0) {
        return _fail(QStringLiteral("cannot close a fragment"), ret);
    }
    avio_flush(_io);
    const QByteArray bytes = _takeOut();
    Fmp4FragmentInfo info = _frag;
    info.seq = _seq++;
    info.offset = _bytesOut;
    info.length = bytes.size();
    info.samples = _fragSamples;
    info.duration90k = _fragDuration;
    _bytesOut += bytes.size();
    _fragOpen = false;
    _fragBytes = 0;
    _fragSamples = 0;
    _fragDuration = 0;
    if (!bytes.isEmpty()) {
        emit fragment(bytes, info);
    }
    return true;
}

bool Fmp4Muxer::write(const Unit &unit)
{
    if (!isOpen()) {
        return _fail(QStringLiteral("the muxer is not open"));
    }
    if (!_started && !unit.keyFrame) {
        return false;                       // a stream starts on a keyframe
    }
    if (unit.data.isEmpty()) {
        return true;
    }

    // Source times: dts falls back to pts and both to the receive clock.
    qint64 dts = unit.dts90k >= 0 ? unit.dts90k : unit.pts90k;
    qint64 pts = unit.pts90k >= 0 ? unit.pts90k : dts;
    if (dts < 0) {
        dts = pts = unit.receiveSteadyNs / 100000 * 9;      // ns to 90 kHz
    }
    if (!_started) {
        _t0 = dts;
        _started = true;
    }
    qint64 relDts = dts - _t0;
    qint64 relPts = pts - _t0;
    // libavformat needs strictly increasing decode times and pts >= dts. A
    // source without decode times but with B-frames, or a clock step, would
    // break that; keep the order and say so once.
    if (relDts <= _lastDts) {
        if (!_warnedTimes) {
            qCWarning(Fmp4MuxerLog) << "decode times not increasing; adjusted to keep order";
            _warnedTimes = true;
        }
        relDts = _lastDts + 1;
    }
    if (relPts < relDts) {
        relPts = relDts;
    }

    // The held unit now has an exact duration.
    if (_hasHeld) {
        const qint64 duration = relDts - _held.dts;
        _lastDuration = duration;
        if (!_writeHeld(duration)) {
            return false;
        }
    }

    // Does this unit start a new fragment?
    if (_fragOpen && _fragSamples > 0) {
        bool cut = _cutRequested;
        if (unit.keyFrame && _config.cutAtKeyframes) {
            cut = true;
        }
        if (_config.fragment90k > 0 && relDts - _frag.dts90k >= _config.fragment90k) {
            cut = true;
        }
        if (_config.maxFragmentBytes > 0 && _fragBytes + unit.data.size() > _config.maxFragmentBytes) {
            cut = true;
        }
        if (cut && !_flushFragment()) {
            return false;
        }
    }
    _cutRequested = false;

    if (!_fragOpen) {
        _fragOpen = true;
        _frag = Fmp4FragmentInfo();
        _frag.dts90k = relDts;
        _frag.pts90k = relPts;
        _frag.startsWithKeyframe = unit.keyFrame;
        _frag.sourceTime90k = unit.sourceTime90k;
        _frag.receiveSteadyNs = unit.receiveSteadyNs;
        _frag.receiveUtcMs = unit.receiveUtcMs;
        _frag.firstIndex = unit.index;
        _fragBytes = 0;
        _fragSamples = 0;
        _fragDuration = 0;
    }

    _held.data = unit.data;
    _held.keyFrame = unit.keyFrame;
    _held.dts = relDts;
    _held.pts = relPts;
    _hasHeld = true;
    _lastDts = relDts;
    return true;
}

bool Fmp4Muxer::finish()
{
    if (!isOpen()) {
        return true;
    }
    bool ok = true;
    if (_hasHeld) {
        ok = _writeHeld(_lastDuration > 0 ? _lastDuration : 3000);
    }
    if (ok) {
        ok = _flushFragment();
    }
    if (ok) {
        const int ret = av_write_trailer(_fmt);
        if (ret < 0) {
            ok = _fail(QStringLiteral("cannot finish the MP4 stream"), ret);
        } else {
            avio_flush(_io);
            // skip_trailer: normally nothing. Anything libavformat did write is
            // still emitted, in order, so the output stays append-only.
            const QByteArray tail = _takeOut();
            if (!tail.isEmpty()) {
                Fmp4FragmentInfo info;
                info.seq = _seq++;
                info.offset = _bytesOut;
                info.length = tail.size();
                _bytesOut += tail.size();
                emit fragment(tail, info);
            }
        }
    }
    _close();
    return ok;
}
