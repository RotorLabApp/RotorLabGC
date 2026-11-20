/****************************************************************************
 *
 * RotorLab GC - G109 live relay to the Fleet Server: shared types.
 *
 * The relay works on one representation of video from the tap to the wire:
 * RelayUnit, an H.264 or H.265 access unit in Annex B with its clocks. Full
 * quality takes the tap's units as they are; Low and Medium take the
 * encoder's output units. Both go through the same LiveFragmenter and
 * LiveSession, so only one path writes boxes and only one path sends.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QLoggingCategory>
#include <QtCore/QMetaType>
#include <QtCore/QString>

#include "VideoPacketTap.h"

Q_DECLARE_LOGGING_CATEGORY(VideoRelayLog)

namespace VideoRelayLimits {
/// Contract S20: a fragment (moof + mdat as serialized) is at most 4 MB.
constexpr qint64 kMaxFragmentBytes = 4 * 1024 * 1024;
/// Cut a fragment about every second (90 kHz units).
constexpr qint64 kFragmentTarget90k = 90000;
/// Backlog bound before dropping to the next keyframe.
constexpr qint64 kBacklog90k = 3 * 90000;
/// A gap in decode time larger than this is treated as a break in the clock.
constexpr qint64 kMaxSampleGap90k = 10 * 90000;
}

/// Forward quality. The order is the order of the drop-down.
enum class RelayQuality : quint8 { Low, Medium, Full };

struct RelayQualityTarget
{
    int width = 0;          ///< 0 for Full (as received)
    int height = 0;
    int bitRate = 0;        ///< bits per second, 0 for Full
    int maxFps = 0;         ///< 0 for Full
};

inline RelayQualityTarget relayQualityTarget(RelayQuality q)
{
    switch (q) {
    case RelayQuality::Low:    return {640, 360, 800000, 15};
    case RelayQuality::Medium: return {1280, 720, 2500000, 30};
    case RelayQuality::Full:   break;
    }
    return {};
}

/// What the Low and Medium encoders aim at, in percent of the level's cap.
/// Rate control runs over its own target for tens of seconds on a normal
/// picture (Media Foundation, P5 run: 903 to 908 kb/s on an 800 kb/s target),
/// and the bitrate guard judges against the cap itself (BitrateGuard.h).
constexpr int kEncoderAimPercent = 85;

/// The encoder's target and maximum rate for a level: Low 680 kb/s, Medium
/// 2125 kb/s; 0 for Full.
inline int relayEncoderBitRate(RelayQuality q)
{
    return int(qint64(relayQualityTarget(q).bitRate) * kEncoderAimPercent / 100);
}

inline QString relayQualityKey(RelayQuality q)
{
    switch (q) {
    case RelayQuality::Low:    return QStringLiteral("low");
    case RelayQuality::Medium: return QStringLiteral("medium");
    case RelayQuality::Full:   break;
    }
    return QStringLiteral("full");
}

inline bool relayQualityFromKey(const QString &key, RelayQuality &out)
{
    if (key == QLatin1String("low"))    { out = RelayQuality::Low;    return true; }
    if (key == QLatin1String("medium")) { out = RelayQuality::Medium; return true; }
    if (key == QLatin1String("full"))   { out = RelayQuality::Full;   return true; }
    return false;
}

/// One access unit, Annex B, with the clocks the relay needs.
struct RelayUnit
{
    VideoTapCodec codec = VideoTapCodec::H264;
    QByteArray data;            ///< Annex B access unit
    QByteArray parameterSets;   ///< Annex B SPS and PPS (and VPS) current for this unit; may be empty
    bool keyFrame = false;
    bool discontinuity = false; ///< first unit of a generation or after a gap
    bool hasRtpTimestamp = false;   ///< pts90k and dts90k are 32-bit RTP times that wrap
    qint64 pts90k = -1;
    qint64 dts90k = -1;
    qint64 receiveSteadyNs = 0; ///< std::chrono::steady_clock, as the receiver stamps it
    quint64 generation = 0;     ///< a change means a new stream: new init, start at a keyframe

    static RelayUnit fromTap(const VideoTapPacket &p)
    {
        RelayUnit u;
        u.codec = p.codec;
        u.data = p.data;
        u.parameterSets = p.parameterSets;
        u.keyFrame = p.keyFrame;
        u.discontinuity = p.discontinuity;
        u.hasRtpTimestamp = p.hasRtpTimestamp;
        u.pts90k = p.pts90k;
        u.dts90k = p.dts90k;
        u.receiveSteadyNs = p.receiveSteadyNs;
        u.generation = p.generation;
        return u;
    }
};

/// What POST /live describes.
struct LiveFormat
{
    QString codec;      ///< "avc1.PPCCLL"
    int width = 0;
    int height = 0;
    int fps = 0;        ///< estimated from the stream; 0 when unknown
};

/// One serialized media fragment (moof + mdat) and the init segment it needs.
struct LiveFragment
{
    QByteArray bytes;
    quint64 initId = 0;         ///< changes with every new init segment
    QByteArray init;            ///< ftyp + moov for initId (implicitly shared)
    LiveFormat format;
    bool keyframeStart = false; ///< the first sample is a sync sample
    qint64 duration90k = 0;
    int samples = 0;
    qint64 lastReceiveSteadyNs = 0;
};

Q_DECLARE_METATYPE(LiveFragment)

/// Steady clock in nanoseconds, the same clock the receiver stamps units with.
qint64 relaySteadyNowNs();
/// CPU time this thread has used, in ns, or -1 where the platform cannot say.
/// The CPU cap (RelayEncoder) measures with it, so time the thread spent
/// preempted (the relay thread runs at low priority) or waiting is not load.
qint64 relayThreadCpuNs();
