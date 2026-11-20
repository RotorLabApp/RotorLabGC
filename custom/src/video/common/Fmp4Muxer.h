/****************************************************************************
 *
 * RotorLab GC - fragmented MP4 from received access units (G95, G109).
 *
 * Shared by local recording (G95) and the Fleet live relay (G109).
 *
 * libavformat's own "mp4" muxer on a write-only AVIOContext with no seek
 * callback, so every byte comes out once, in order, and is never rewritten:
 *
 *   movflags = empty_moov + default_base_moof + frag_custom + skip_trailer
 *
 * The output is an init segment (ftyp + moov, emitted by open()) followed by
 * media fragments (moof + mdat, emitted as they close). Concatenated, they are
 * a complete, playable fragmented MP4 at every fragment boundary, so a running
 * hash over what is emitted equals the hash of the file it is written to.
 *
 * Access units go in as received (Annex B), are never decoded or re-encoded,
 * and are rewritten to length-prefixed NAL units by libavformat from the
 * parameter sets given at open(). H.265 uses the 'hvc1' sample entry, so the
 * in-band VPS/SPS/PPS are left to the sample entry.
 *
 * Fragments close:
 *   - before a keyframe (cutAtKeyframes),
 *   - before the first unit at or past fragment90k after the fragment start,
 *   - before a unit that would take the fragment past maxFragmentBytes,
 *   - before the next unit after cutFragment(),
 *   - at finish().
 * A fragment may begin mid-GOP; libavformat then writes non-sync sample flags.
 *
 * Every unit is held back until the next one arrives, so its duration is
 * exact (next dts minus its dts) and fragment decode times are continuous.
 *
 * Not thread safe; use it on one thread. Signals are emitted synchronously
 * from open(), write() and finish().
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QMetaType>
#include <QtCore/QObject>
#include <QtCore/QString>

#include "VideoCodecInfo.h"

struct AVFormatContext;
struct AVIOContext;

struct Fmp4FragmentInfo
{
    quint64 seq = 0;                ///< 0-based fragment number since open()
    qint64  offset = 0;             ///< stream offset of the fragment's first byte (the init segment counts)
    qint64  length = 0;             ///< bytes in this fragment (moof + mdat)
    qint64  dts90k = 0;             ///< first sample's decode time, 90 kHz, 0 at the first unit since open()
    qint64  pts90k = 0;             ///< first sample's presentation time, same timeline
    qint64  duration90k = 0;        ///< sum of the sample durations
    int     samples = 0;
    bool    startsWithKeyframe = false;
    // The first unit's metadata as given to write():
    qint64  sourceTime90k = -1;     ///< its source time (unwrapped RTP, or the demuxer's dts)
    qint64  receiveSteadyNs = 0;
    qint64  receiveUtcMs = 0;
    quint64 firstIndex = 0;
};
Q_DECLARE_METATYPE(Fmp4FragmentInfo)

class Fmp4Muxer : public QObject
{
    Q_OBJECT

public:
    enum class Codec { H264, H265 };

    struct Config
    {
        Codec      codec = Codec::H264;
        QByteArray parameterSets;       ///< Annex B: (VPS,) SPS, PPS; required
        int        width = 0;           ///< 0: from the SPS
        int        height = 0;          ///< 0: from the SPS
        qint64     fragment90k = 90000; ///< target fragment duration (1 s)
        bool       cutAtKeyframes = true;
        qint64     maxFragmentBytes = 0;    ///< 0: no byte limit
    };

    struct Unit
    {
        QByteArray data;                ///< one access unit, Annex B
        bool    keyFrame = false;
        qint64  pts90k = -1;            ///< any origin, 64-bit (unwrap RTP first); -1 unknown
        qint64  dts90k = -1;            ///< any origin; -1: same as pts
        qint64  sourceTime90k = -1;     ///< copied to Fmp4FragmentInfo (for example the unwrapped RTP time)
        qint64  receiveSteadyNs = 0;    ///< copied to Fmp4FragmentInfo; also the time base when both times are unknown
        qint64  receiveUtcMs = 0;       ///< copied to Fmp4FragmentInfo
        quint64 index = 0;              ///< copied to Fmp4FragmentInfo
    };

    explicit Fmp4Muxer(QObject *parent = nullptr);
    ~Fmp4Muxer() override;

    /// Start a stream. Emits initSegment() before it returns true. A muxer
    /// already open is closed first without emitting anything more.
    bool open(const Config &config);

    /// Add one access unit. The first one after open() must be a keyframe
    /// (earlier units are dropped and false is returned). May emit fragment()
    /// for the fragment the unit closes. False on a muxer error (errorString()).
    bool write(const Unit &unit);

    /// The next unit passed to write() starts a new fragment.
    void cutFragment() { _cutRequested = true; }

    /// Write the held unit and the last fragment, then close. Emits fragment()
    /// for what was still open. False on a muxer error.
    bool finish();

    /// Close without emitting anything (abandon the stream).
    void abort();

    bool    isOpen() const { return _fmt != nullptr; }
    QString errorString() const { return _error; }
    VideoCodecInfo codecInfo() const { return _info; }
    const Config &config() const { return _config; }
    qint64  bytesOut() const { return _bytesOut; }          ///< bytes emitted since open()
    quint64 fragmentsOut() const { return _seq; }
    int     samplesOut() const { return _samplesTotal; }
    qint64  lastDts90k() const { return _lastDts; }         ///< rebased; -1 before the first unit
    /// Bytes of access units in the fragment being built (held unit included).
    qint64  pendingBytes() const { return _fragBytes + (_hasHeld ? _held.data.size() : 0); }
    /// The tap index of the first unit of the fragment being built, or 0.
    quint64 currentFragmentFirstIndex() const { return _fragOpen ? _frag.firstIndex : 0; }

    /// True when a keyframe with these parameter sets needs a new init segment
    /// (another codec, or other parameter sets than the open stream's).
    bool needsNewInit(Codec codec, const QByteArray &parameterSets) const;

    /// Whether this build's FFmpeg has the "mp4" muxer.
    static bool available();

signals:
    void initSegment(const QByteArray &bytes);
    void fragment(const QByteArray &bytes, const Fmp4FragmentInfo &info);

private:
    struct Held
    {
        QByteArray data;
        bool    keyFrame = false;
        qint64  dts = 0;            ///< rebased
        qint64  pts = 0;            ///< rebased
    };

    bool _writeHeld(qint64 duration);
    bool _flushFragment();
    bool _fail(const QString &what, int averror = 0);
    void _close();
    QByteArray _takeOut();

    friend struct Fmp4MuxerIo;
    void _sink(const char *data, int size) { _out.append(data, size); }

    AVFormatContext *_fmt = nullptr;
    AVIOContext *_io = nullptr;
    Config  _config;
    VideoCodecInfo _info;
    QByteArray _out;                ///< bytes from the AVIO callback not yet emitted
    QString _error;

    bool    _started = false;       ///< a keyframe has been written since open()
    qint64  _t0 = 0;                ///< source time of the first unit
    qint64  _lastDts = -1;
    qint64  _lastDuration = 3000;
    bool    _hasHeld = false;
    Held    _held;
    bool    _cutRequested = false;
    bool    _warnedTimes = false;

    bool    _fragOpen = false;
    Fmp4FragmentInfo _frag;
    qint64  _fragBytes = 0;         ///< AU bytes already handed to libavformat for this fragment
    int     _fragSamples = 0;
    qint64  _fragDuration = 0;

    quint64 _seq = 0;
    qint64  _bytesOut = 0;
    int     _samplesTotal = 0;
};
