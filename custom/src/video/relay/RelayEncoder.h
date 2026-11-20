/****************************************************************************
 *
 * RotorLab GC - G109: Low and Medium. Decode, scale, encode to H.264.
 *
 * - Its own software decoder (low delay, slice threads, no frame threads), as
 *   the receiver does; it never touches the receiver's decoder or display.
 * - Scales to fit inside the level's size, keeping the aspect ratio; never
 *   upscales (a smaller source keeps its size at the level's bitrate cap).
 * - Frames over the level's frame rate are dropped before encoding.
 * - H.264 out with a 1 s GOP (a keyframe is forced at least every second of
 *   media time, and on request after a new live session), no B-frames, CBR,
 *   aimed at relayEncoderBitRate (85 percent of the level's cap) with half a
 *   second of VBV, so a normal picture stays under the cap the bitrate guard
 *   judges.
 * - Every time the encoder opens (a new level, a new size, after a stream
 *   stop) and every new source generation, restarted() runs, before any of
 *   the new output: VideoRelay gives the bitrate guard its start grace again.
 * - CPU cap (EncodeLoadMeter): this thread's CPU time per encoded frame
 *   (decode, scale and encode) averaged over the last 5 s of frames; above
 *   60 percent of the frame interval, or when more than a quarter of those
 *   frames waited more than two frame intervals after they were received,
 *   overloaded() turns true. VideoRelay then steps Medium down to Low once,
 *   or stops at Low.
 *   It judges only the work of encoding frames that arrive (S20/S21 end-to-end
 *   run: the relay stopped at "busy 0.64, late frames 0 of 75" when the input
 *   paused). An input pause (nothing to push for 1 s), a source change (a new
 *   generation), and any push that opened a decoder or an encoder start the
 *   measure again, and frames received before that restart are never counted
 *   late. CPU time, not wall time, so a relay thread that the rest of GC kept
 *   off the CPU (it runs at low priority) is not counted as encoder load.
 *
 * Not a QObject and not thread safe: it lives on the relay's worker thread.
 *
 ****************************************************************************/

#pragma once

#include "EncoderProbe.h"
#include "RelayTypes.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <map>

/// The CPU cap's measure. Pure logic: every time is passed in, so tests drive
/// it with their own clock. Not thread safe (the relay's worker thread).
class EncodeLoadMeter
{
public:
    /// Nothing pushed for this long: the input paused, the measure starts again.
    static constexpr qint64 kGapNs = 1'000'000'000LL;
    static constexpr double kBusyLimit = 0.6;
    static constexpr int kWindowSeconds = 5;
    static constexpr int kJudgeSeconds = 2;

    explicit EncodeLoadMeter(int fps = 15) { setFps(fps); }

    void setFps(int fps) { _fps = std::max(1, fps); }
    int fps() const { return _fps; }

    /// Start again at `nowNs`: a source change, a (re)opened decoder or
    /// encoder, an input pause. Frames received before it are never late.
    void restart(qint64 nowNs)
    {
        _window.clear();
        _cpuNs = 0;
        _frames = 0;
        _late = 0;
        _sinceNs = nowNs;
        _lastEndNs = -1;
        ++_restarts;
    }

    /// A push begins at `startNs` (steady clock).
    void begin(qint64 startNs)
    {
        if (_lastEndNs >= 0 && startNs - _lastEndNs > kGapNs) {
            restart(startNs);       // the input paused: a gap is not encoder load
        }
        _pushFrames = 0;
        _pushLate = 0;
    }

    /// A frame went into the encoder at `nowNs`; it was received at `receiveNs` (0: unknown).
    void frameEncoded(qint64 receiveNs, qint64 nowNs)
    {
        ++_pushFrames;
        if (receiveNs > 0 && receiveNs >= _sinceNs && nowNs - receiveNs > 2 * intervalNs()) {
            ++_pushLate;
        }
    }

    /// The push ended at `endNs`, having used `cpuNs` of the thread's time.
    /// `setup`: it opened a decoder or an encoder; it is not judged and the
    /// measure starts again after it. True when the cap trips now.
    bool end(qint64 endNs, qint64 cpuNs, bool setup)
    {
        if (setup) {
            restart(endNs);
            _lastEndNs = endNs;
            return false;
        }
        _lastEndNs = endNs;
        _window.push_back(Push{std::max<qint64>(0, cpuNs), _pushFrames, _pushLate});
        _cpuNs += _window.back().cpuNs;
        _frames += _pushFrames;
        _late += _pushLate;
        while (!_window.empty() && _frames - _window.front().frames >= kWindowSeconds * _fps) {
            _cpuNs -= _window.front().cpuNs;
            _frames -= _window.front().frames;
            _late -= _window.front().late;
            _window.pop_front();
        }
        if (_frames < kJudgeSeconds * _fps) {
            return false;           // not enough frames since the last restart to judge
        }
        _busyRatio = double(_cpuNs) / (double(_frames) * double(intervalNs()));
        if (!_overloaded && (_busyRatio > kBusyLimit || _late * 4 > _frames)) {
            _overloaded = true;
            return true;
        }
        return false;
    }

    bool overloaded() const { return _overloaded; }
    double busyRatio() const { return _busyRatio; }
    int frames() const { return _frames; }
    int lateFrames() const { return _late; }
    int restarts() const { return _restarts; }
    qint64 intervalNs() const { return 1'000'000'000LL / _fps; }

private:
    struct Push
    {
        qint64 cpuNs = 0;
        int frames = 0;
        int late = 0;
    };

    int _fps = 15;
    std::deque<Push> _window;
    qint64 _cpuNs = 0;
    int _frames = 0;
    int _late = 0;
    int _pushFrames = 0;
    int _pushLate = 0;
    qint64 _sinceNs = 0;
    qint64 _lastEndNs = -1;
    int _restarts = 0;
    double _busyRatio = 0;
    bool _overloaded = false;
};

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

class RelayEncoder
{
public:
    using Output = std::function<void(const RelayUnit &)>;

    struct Stats
    {
        quint64 unitsIn = 0;
        quint64 framesDecoded = 0;
        quint64 framesEncoded = 0;
        quint64 droppedForRate = 0;
        quint64 packetsOut = 0;
        quint64 decodeErrors = 0;
        quint64 encodeErrors = 0;
        quint64 encoderOpens = 0;
        quint64 sourceRestarts = 0;     ///< new generations while the encoder was open
    };

    RelayEncoder(const EncoderChoice &encoder, RelayQuality quality, Output out);
    ~RelayEncoder();

    RelayEncoder(const RelayEncoder &) = delete;
    RelayEncoder &operator=(const RelayEncoder &) = delete;

    void push(const RelayUnit &unit);
    /// Runs on this thread when the encoder (re)opens or the source starts a
    /// new generation, before any output from it.
    void setRestarted(std::function<void()> f) { _restarted = std::move(f); }
    /// The next encoded frame is a keyframe (a new live session wants one).
    void requestKeyframe() { _forceKey = true; }
    /// Drain the encoder (end of stream).
    void flush();

    bool overloaded() const { return _meter.overloaded(); }
    double busyRatio() const { return _meter.busyRatio(); }
    const EncodeLoadMeter &meter() const { return _meter; }
    QString lastError() const { return _error; }
    const Stats &stats() const { return _stats; }
    int outputWidth() const { return _outW; }
    int outputHeight() const { return _outH; }
    RelayQuality quality() const { return _quality; }

    /// The output size for a source inside a level's box: aspect kept, even,
    /// never larger than the source.
    static void fitSize(int srcW, int srcH, int boxW, int boxH, int &outW, int &outH);

    /// For tests: burn this much CPU per encoded frame (an encoder too slow for the level).
    void setInjectedDelayMs(int ms) { _injectedDelayMs = ms; }

private:
    bool _openDecoder(VideoTapCodec codec);
    void _closeDecoder();
    bool _openEncoder(int w, int h);
    void _closeEncoder();
    void _onFrame(AVFrame *frame);
    void _drainEncoder();
    void _emitPacket(AVPacket *pkt);
    qint64 _unwrapPts(const RelayUnit &unit);
    void _push(const RelayUnit &unit);

    EncoderChoice _choice;
    RelayQuality _quality;
    RelayQualityTarget _target;
    Output _out;
    std::function<void()> _restarted;

    AVCodecContext *_dec = nullptr;
    VideoTapCodec _decCodec = VideoTapCodec::H264;
    quint64 _generation = 0;
    bool _haveGeneration = false;
    bool _waitKey = true;
    AVPacket *_pkt = nullptr;
    AVFrame *_frame = nullptr;

    AVCodecContext *_enc = nullptr;
    AVFrame *_encFrame = nullptr;
    AVPacket *_encPkt = nullptr;
    SwsContext *_sws = nullptr;
    int _srcW = 0, _srcH = 0, _srcFmt = -1;
    int _outW = 0, _outH = 0;
    quint64 _outGeneration = 0;
    VideoParameterSets _outParams{VideoTapCodec::H264};

    bool _forceKey = true;
    qint64 _lastKeyPts = -1;
    qint64 _lastOutPts = -1;
    qint64 _lastEncPts = -1;
    qint64 _ptsLast = -1;       ///< unwrapped input pts
    qint64 _rtpLast = -1;       ///< last 32-bit RTP value seen
    std::map<qint64, qint64> _receiveByPts;    ///< pts -> receive time, bounded

    // CPU cap
    EncodeLoadMeter _meter;
    bool _setupInPush = false;      ///< this push opened a decoder or an encoder
    int _injectedDelayMs = 0;

    Stats _stats;
    QString _error;
};
