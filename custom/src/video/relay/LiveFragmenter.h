/****************************************************************************
 *
 * RotorLab GC - G109: cuts H.264 access units into live fMP4 fragments.
 *
 * On the shared Fmp4Muxer (custom/src/video/common):
 * libavformat's mp4 muxer, empty_moov + default_base_moof + frag_custom.
 *
 * - A fragment closes about every second, at the next access unit, at every
 *   keyframe, and early so that it stays under the 4 MB cap. The muxer limits
 *   the access-unit bytes per fragment to the cap less a margin for the boxes
 *   (kBoxMargin); the serialized moof + mdat is checked again here, and a
 *   fragment over 4 MB is never handed on.
 * - A fragment may begin mid-GOP; the muxer's sample flags then say it does
 *   not start with a sync sample. Each sample's duration is the step to the
 *   next unit, so fragments are back to back.
 * - A new init segment on a new generation, new parameter sets, or after a
 *   gap (discontinuity); it starts at a keyframe. A unit too big for any
 *   fragment is dropped with a log line and the stream resumes at the next
 *   keyframe.
 * - RTP times (32-bit, wrapping) are unwrapped before the muxer sees them.
 * - Only H.264 is cut. H.265 goes live only after RelayEncoder made it H.264.
 *
 * Not thread safe: it lives on the relay's worker thread.
 *
 ****************************************************************************/

#pragma once

#include "Fmp4Muxer.h"
#include "RelayTypes.h"

#include <functional>

class LiveFragmenter
{
public:
    using Output = std::function<void(const LiveFragment &)>;

    /// Room left for moof, trun entries and start-code to length rewriting.
    static constexpr qint64 kBoxMargin = 64 * 1024;

    struct Stats
    {
        quint64 fragments = 0;
        quint64 units = 0;
        quint64 droppedWaitingKey = 0;
        quint64 droppedOversize = 0;
        quint64 refusedCodec = 0;
        quint64 inits = 0;
        quint64 muxerErrors = 0;
    };

    explicit LiveFragmenter(Output out = {});
    ~LiveFragmenter();

    LiveFragmenter(const LiveFragmenter &) = delete;
    LiveFragmenter &operator=(const LiveFragmenter &) = delete;

    void setOutput(Output out) { _out = std::move(out); }
    /// For tests: a smaller cap than the contract's 4 MB.
    void setMaxFragmentBytes(qint64 bytes) { _maxBytes = bytes; }

    void push(const RelayUnit &unit);
    /// Close the fragment being built (end of stream, mode change).
    void flush();
    /// Drop what is being built and start again at the next keyframe.
    void resync();
    /// Forget everything.
    void reset();

    const Stats &stats() const { return _stats; }
    bool waitingForKeyframe() const { return _waitKey; }
    quint64 initId() const { return _initId; }

private:
    bool _open(const RelayUnit &unit);
    void _onFragment(const QByteArray &bytes, const Fmp4FragmentInfo &info);
    qint64 _unwrap(qint64 t, bool rtp, qint64 &last, qint64 &lastRaw);

    Output _out;
    Fmp4Muxer _muxer;
    qint64 _maxBytes = VideoRelayLimits::kMaxFragmentBytes;

    bool _haveGeneration = false;
    quint64 _generation = 0;
    bool _waitKey = true;
    QByteArray _parameterSets;

    QByteArray _init;
    LiveFormat _format;
    quint64 _initId = 0;

    qint64 _ptsLast = -1, _ptsRaw = -1;
    qint64 _dtsLast = -1, _dtsRaw = -1;

    Stats _stats;
    bool _loggedCodec = false;
};
