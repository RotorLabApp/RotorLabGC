/****************************************************************************
 *
 * RotorLab GC - G109: live fragment cutter on the shared Fmp4Muxer.
 *
 ****************************************************************************/

#include "LiveFragmenter.h"

#include "VideoCodecInfo.h"

#include <cmath>

namespace {

/// SPS and PPS of an Annex B buffer, as Annex B (what Fmp4Muxer::Config wants).
QByteArray h264ParameterSets(const QByteArray &annexB)
{
    QByteArray sps;
    QByteArray pps;
    for (const QByteArray &nal : VideoCodecInfo::splitAnnexB(annexB)) {
        const int type = nal.isEmpty() ? -1 : (quint8(nal.at(0)) & 0x1f);
        if (type == 7) {
            sps = nal;
        } else if (type == 8) {
            pps = nal;
        }
    }
    if (sps.isEmpty() || pps.isEmpty()) {
        return {};
    }
    const QByteArray sc = QByteArray::fromHex("00000001");
    return sc + sps + sc + pps;
}

} // namespace

LiveFragmenter::LiveFragmenter(Output out)
    : _out(std::move(out))
{
    // Direct: the muxer emits synchronously on the worker thread that calls
    // push(), whatever thread the muxer object was created on.
    QObject::connect(&_muxer, &Fmp4Muxer::initSegment, &_muxer,
                     [this](const QByteArray &bytes) { _init = bytes; }, Qt::DirectConnection);
    QObject::connect(&_muxer, &Fmp4Muxer::fragment, &_muxer,
                     [this](const QByteArray &bytes, const Fmp4FragmentInfo &info) { _onFragment(bytes, info); },
                     Qt::DirectConnection);
}

LiveFragmenter::~LiveFragmenter()
{
    _muxer.abort();
}

qint64 LiveFragmenter::_unwrap(qint64 t, bool rtp, qint64 &last, qint64 &lastRaw)
{
    if (t < 0) {
        return -1;
    }
    if (!rtp) {
        last = t;
        return t;
    }
    const quint32 raw = quint32(quint64(t));
    qint64 v = raw;
    if (lastRaw >= 0 && last >= 0) {
        v = last + qint32(raw - quint32(lastRaw));
    }
    last = v;
    lastRaw = raw;
    return v;
}

void LiveFragmenter::push(const RelayUnit &unit)
{
    if (unit.codec != VideoTapCodec::H264) {
        ++_stats.refusedCodec;
        if (!_loggedCodec) {
            _loggedCodec = true;
            qCWarning(VideoRelayLog) << "live fragments are H.264 only; an H.265 unit was refused";
        }
        return;
    }
    ++_stats.units;

    if (!_haveGeneration || unit.generation != _generation) {
        flush();
        _haveGeneration = true;
        _generation = unit.generation;
        _parameterSets.clear();     // a new generation always gets its own init segment
        _ptsLast = _ptsRaw = _dtsLast = _dtsRaw = -1;
    } else if (unit.discontinuity) {
        // A gap: close what was built before it; the unit after a gap is a keyframe.
        flush();
    }

    if (unit.keyFrame) {
        QByteArray ps = h264ParameterSets(unit.parameterSets);
        if (ps.isEmpty()) {
            ps = h264ParameterSets(unit.data);
        }
        if (ps.isEmpty()) {
            ps = _parameterSets;
        }
        const bool newInit = !_muxer.isOpen() || ps != _parameterSets
                             || _muxer.needsNewInit(Fmp4Muxer::Codec::H264, ps);
        if (newInit) {
            if (_muxer.isOpen()) {
                _muxer.finish();
            }
            _parameterSets = ps;
            if (ps.isEmpty() || !_open(unit)) {
                ++_stats.droppedWaitingKey;
                _waitKey = true;
                return;
            }
        }
    }

    if (_waitKey) {
        if (!unit.keyFrame || !_muxer.isOpen()) {
            ++_stats.droppedWaitingKey;
            return;
        }
        _waitKey = false;
    }

    const qint64 margin = qMin(kBoxMargin, _maxBytes / 16);
    if (unit.data.size() > _maxBytes - margin) {
        ++_stats.droppedOversize;
        qCWarning(VideoRelayLog) << "an access unit of" << unit.data.size()
                                 << "bytes is over the live fragment cap and was dropped; waiting for the next keyframe";
        flush();
        return;
    }

    Fmp4Muxer::Unit mu;
    mu.data = unit.data;
    mu.keyFrame = unit.keyFrame;
    mu.pts90k = _unwrap(unit.pts90k, unit.hasRtpTimestamp, _ptsLast, _ptsRaw);
    mu.dts90k = _unwrap(unit.dts90k, unit.hasRtpTimestamp, _dtsLast, _dtsRaw);
    mu.sourceTime90k = mu.dts90k >= 0 ? mu.dts90k : mu.pts90k;
    mu.receiveSteadyNs = unit.receiveSteadyNs;
    if (!_muxer.write(mu)) {
        ++_stats.muxerErrors;
        qCWarning(VideoRelayLog) << "live muxer:" << _muxer.errorString() << "; waiting for the next keyframe";
        _muxer.abort();
        _waitKey = true;
    }
}

bool LiveFragmenter::_open(const RelayUnit &unit)
{
    Q_UNUSED(unit)
    Fmp4Muxer::Config c;
    c.codec = Fmp4Muxer::Codec::H264;
    c.parameterSets = _parameterSets;
    c.fragment90k = VideoRelayLimits::kFragmentTarget90k;
    c.cutAtKeyframes = true;
    c.maxFragmentBytes = _maxBytes - qMin(kBoxMargin, _maxBytes / 16);
    _init.clear();
    if (!_muxer.open(c) || _init.isEmpty()) {
        ++_stats.muxerErrors;
        qCWarning(VideoRelayLog) << "live muxer did not open:" << _muxer.errorString();
        _muxer.abort();
        return false;
    }
    const VideoCodecInfo info = _muxer.codecInfo();
    _format.codec = info.codecString;
    _format.width = info.width;
    _format.height = info.height;
    ++_initId;
    ++_stats.inits;
    _waitKey = false;
    return true;
}

void LiveFragmenter::_onFragment(const QByteArray &bytes, const Fmp4FragmentInfo &info)
{
    if (info.samples <= 0 || bytes.isEmpty()) {
        return;     // nothing playable (skip_trailer leaves nothing in practice)
    }
    if (bytes.size() > _maxBytes) {
        // Never sent: the cap is the contract's. Resume at the next keyframe.
        ++_stats.droppedOversize;
        qCWarning(VideoRelayLog) << "a live fragment of" << bytes.size() << "bytes passed the cap and was dropped";
        _waitKey = true;
        return;
    }
    if (info.duration90k > 0 && info.samples >= 2) {
        _format.fps = int(std::lround(90000.0 * info.samples / double(info.duration90k)));
    }
    LiveFragment f;
    f.bytes = bytes;
    f.initId = _initId;
    f.init = _init;
    f.format = _format;
    f.keyframeStart = info.startsWithKeyframe;
    f.duration90k = info.duration90k;
    f.samples = info.samples;
    f.lastReceiveSteadyNs = info.receiveSteadyNs > 0
        ? info.receiveSteadyNs + info.duration90k * 100000 / 9
        : 0;
    ++_stats.fragments;
    if (_out) {
        _out(f);
    }
}

void LiveFragmenter::flush()
{
    if (_muxer.isOpen()) {
        _muxer.finish();
    }
    _waitKey = true;
}

void LiveFragmenter::resync()
{
    _muxer.abort();
    _waitKey = true;
}

void LiveFragmenter::reset()
{
    _muxer.abort();
    _waitKey = true;
    _haveGeneration = false;
    _parameterSets.clear();
    _init.clear();
    _format = LiveFormat{};
    _ptsLast = _ptsRaw = _dtsLast = _dtsRaw = -1;
}
