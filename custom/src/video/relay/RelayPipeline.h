/****************************************************************************
 *
 * RotorLab GC - G109: the relay's worker. Lives on its own thread.
 *
 * Pops the tap consumer's queue (woken by the tap, never blocking the
 * receiver), then either cuts the units as they are (Full) or runs them
 * through RelayEncoder first (Low, Medium), and hands each finished fragment
 * to the GUI thread, where LiveSession sends it. The tap's own bounded queue
 * is the only buffer before this point: if this thread falls behind, the tap
 * drops units for this consumer alone and it resumes at a keyframe.
 *
 ****************************************************************************/

#pragma once

#include "EncoderProbe.h"
#include "LiveFragmenter.h"
#include "RelayEncoder.h"
#include "RelayTypes.h"

#include <QtCore/QMutex>
#include <QtCore/QObject>

#include <atomic>
#include <memory>

class RelayPipeline : public QObject
{
    Q_OBJECT

public:
    /// Shared with the tap's wakeup callback, which runs on the receiver's
    /// thread and may outlive this object by a moment.
    struct Waker
    {
        QMutex mutex;
        RelayPipeline *target = nullptr;
        std::atomic_bool scheduled{false};
    };

    explicit RelayPipeline(QObject *parent = nullptr);
    ~RelayPipeline() override;

    /// Tap capacity for the relay's consumer: about 4 s at 30 fps.
    static constexpr int kTapCapacity = 120;

    // Called on the worker thread (VideoRelay posts them with invokeMethod).
    void probe();
    /// Attach to `tap` and start producing fragments at `quality`.
    void start(VideoPacketTap *tap, int quality, const EncoderChoice &encoder);
    /// Keep the tap, change the quality (Medium to Low after the CPU cap, or
    /// the operator's choice).
    void setQuality(int quality, const EncoderChoice &encoder);
    void stop();
    void requestKeyframe();

    /// What the bitrate guard lets through to the fragmenter (Low and Medium).
    /// Only this consumer's own units are filtered: the tap, the display and
    /// local recording never see it.
    enum class Pass { All, KeyframesOnly, Nothing };
    void setPass(int pass);

signals:
    void probeDone(const EncoderProbeReport &report);
    void fragmentReady(const LiveFragment &fragment);
    /// A new stream started (or the source or sender changed).
    void streamStarted(int codec, bool codecKnown);
    /// The first unit of a codec was seen.
    void codecSeen(int codec);
    void streamStopped();
    void encoderOverloaded();
    void encoderFailed(const QString &detail);
    /// The encoder's whole output over about half a second of media: every
    /// access unit, before the guard's filter. The guard's content measure.
    void contentMeasured(qint64 bytes, qint64 duration90k);
    /// The Low or Medium encoder opened again or the source started a new
    /// generation (RelayEncoder::setRestarted). Emitted before any of the new
    /// output, so the GUI thread sees it before the fragments and content
    /// measures that follow. VideoRelay gives the bitrate guard its start
    /// grace again.
    void encoderRestarted();

private slots:
    void _drain();

private:
    void _handle(const VideoTapPacket &p);
    void _handle(const VideoTapEvent &e);
    void _detach();
    void _onEncoded(const RelayUnit &u);

    Pass _pass = Pass::All;
    bool _passWaitKey = false;      ///< back to All: from the next keyframe
    qint64 _contentBytes = 0;
    qint64 _contentDuration90k = 0;
    qint64 _contentLastPts = -1;

    VideoPacketTap *_tap = nullptr;
    std::shared_ptr<VideoPacketConsumer> _consumer;
    std::shared_ptr<Waker> _waker;
    LiveFragmenter _fragmenter;
    std::unique_ptr<RelayEncoder> _encoder;
    RelayQuality _quality = RelayQuality::Full;
    EncoderChoice _choice;
    bool _overloadSent = false;
    bool _encoderFailSent = false;
    int _lastCodec = -1;
};

Q_DECLARE_METATYPE(EncoderChoice)
Q_DECLARE_METATYPE(EncoderProbeReport)
