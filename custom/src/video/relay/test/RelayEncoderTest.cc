#include "RelayEncoderTest.h"

#include "BitrateGuard.h"
#include "EncoderProbe.h"
#include "LiveFragmenter.h"
#include "RelayEncoder.h"
#include "RelayPipeline.h"
#include "RelayTestSupport.h"
#include "VideoCodecInfo.h"
#include "VideoRelay.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QMap>
#include <QtTest/QTest>

#include <algorithm>
#include <deque>
#include <limits>

#ifdef QGC_FFMPEG_STREAMING
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}
#endif

namespace {

#ifdef QGC_FFMPEG_STREAMING

/// A source stream made with this machine's encoder. `noise`: every pixel
/// random (the worst case for a bitrate cap: rain, foliage, sensor noise at
/// night). Otherwise a scene: a moving pattern, two moving blocks and light
/// grain, closer to an aerial picture.
QList<RelayUnit> makeSource(const EncoderChoice &enc, int width, int height, int frames, QString *error,
                            bool noise = false)
{
    QList<RelayUnit> out;
    AVCodecContext *ctx = EncoderProbe::openEncoder(enc, width, height, 30, 8000000, error);
    if (!ctx) {
        return out;
    }
    AVFrame *f = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    f->format = ctx->pix_fmt;
    f->width = width;
    f->height = height;
    av_frame_get_buffer(f, 0);
    VideoParameterSets ps(VideoTapCodec::H264);
    if (ctx->extradata && ctx->extradata_size > 0) {
        ps.scanAnnexB(QByteArray(reinterpret_cast<const char *>(ctx->extradata), ctx->extradata_size));
    }
    quint32 seed = 12345;
    const auto drain = [&]() {
        while (avcodec_receive_packet(ctx, pkt) >= 0) {
            RelayUnit u;
            u.codec = VideoTapCodec::H264;
            u.data = QByteArray(reinterpret_cast<const char *>(pkt->data), pkt->size);
            ps.scanAnnexB(u.data);
            u.parameterSets = ps.annexB();
            u.keyFrame = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
            u.pts90k = pkt->pts;
            u.dts90k = pkt->dts == AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
            u.generation = 1;
            out << u;
            av_packet_unref(pkt);
        }
    };
    for (int i = 0; i < frames; ++i) {
        av_frame_make_writable(f);
        for (int p = 0; p < 3 && f->data[p]; ++p) {
            const int rows = p == 0 ? height : (height + 1) / 2;
            for (int y = 0; y < rows; ++y) {
                uint8_t *row = f->data[p] + qsizetype(y) * f->linesize[p];
                for (int x = 0; x < f->linesize[p]; ++x) {
                    seed = seed * 1664525u + 1013904223u;
                    if (noise) {
                        row[x] = uint8_t(seed >> 24);
                    } else if (p == 0) {
                        int v = ((x + i * 3) ^ (y + i)) & 0xff;
                        const int bx = (i * 7) % width;
                        const int by = (i * 3) % height;
                        if (x > bx && x < bx + 160 && y > by && y < by + 120) {
                            v = 235;
                        }
                        if (x > width - bx - 200 && x < width - bx && y > 300 && y < 400) {
                            v = 30;
                        }
                        row[x] = uint8_t(qBound(16, v + int(seed >> 28) - 8, 235));
                    } else {
                        row[x] = uint8_t(128 + ((x / 16 + y / 16 + i) & 31) - 16);
                    }
                }
            }
        }
        f->pts = qint64(i) * 3000;
        if (avcodec_send_frame(ctx, f) == AVERROR(EAGAIN)) {
            drain();
            avcodec_send_frame(ctx, f);
        }
        drain();
    }
    avcodec_send_frame(ctx, nullptr);
    drain();
    av_packet_free(&pkt);
    av_frame_free(&f);
    avcodec_free_context(&ctx);
    return out;
}

#endif

EncoderChoice probeOrEmpty()
{
    return EncoderProbe::run().chosen;
}

// ------------------------------------------------------------ bitrate guard rig

using GState = BitrateGuard::State;

constexpr qint64 kSecond90k = 90000;
constexpr qint64 kLowCap = 800000;

struct SentFragment
{
    qint64 bytes = 0;
    qint64 duration90k = 0;
    int samples = 0;
    qint64 atPts = 0;           ///< media time of the newest unit published when it went out
    GState state = GState::Normal;
};

struct Transition
{
    qint64 atPts = 0;
    GState state = GState::Normal;
};

const char *stateName(GState s)
{
    switch (s) {
    case GState::KeyframesOnly: return "keyframes only";
    case GState::Paused:        return "paused";
    case GState::Normal:        break;
    }
    return "normal";
}

/// The relay chain as VideoRelay runs it, on the test's thread: a real tap with
/// a second consumer standing in for local recording (G95), RelayPipeline
/// (decode, scale and encode for Low; the units as they are for Full), the
/// real LiveFragmenter inside it, and BitrateGuard fed by every fragment that
/// goes out and by the pipeline's content measure. The guard's filter goes
/// back to the pipeline queued, as VideoRelay posts it to the worker thread.
class GuardRig
{
public:
    /// `grace`: as VideoRelay runs it, the start grace (kStartGrace90k) and
    /// restartGrace() on every RelayPipeline::encoderRestarted.
    GuardRig(RelayQuality quality, qint64 cap, const EncoderChoice &encoder = EncoderChoice(), bool grace = false)
    {
        recorder = tap.attach(VideoPacketTap::kMaxCapacity);
        if (grace) {
            guard.setStartGrace90k(BitrateGuard::kStartGrace90k);
            QObject::connect(&pipeline, &RelayPipeline::encoderRestarted, &pipeline, [this]() {
                guard.restartGrace();
                restarts << Restart{lastPts, guard.graceLeft90k(), sent.size(), transitions.size()};
            });
        }
        guard.setCap(cap);
        QObject::connect(&pipeline, &RelayPipeline::fragmentReady, &pipeline, [this](const LiveFragment &f) {
            if (guard.paused()) {
                ++producedWhilePaused;
                return;
            }
            sent << SentFragment{f.bytes.size(), f.duration90k, f.samples, lastPts, guard.state()};
            if (guard.onSent(f.bytes.size(), f.duration90k)) {
                _apply();
            }
        });
        QObject::connect(&pipeline, &RelayPipeline::contentMeasured, &pipeline, [this](qint64 bytes, qint64 d90k) {
            contentBytes += bytes;
            contentDuration90k += d90k;
            content << SentFragment{bytes, d90k, 0, lastPts, guard.state()};
            if (guard.onContent(bytes, d90k)) {
                _apply();
            }
        });
        pipeline.start(&tap, int(quality), encoder);
        VideoTapEvent e;
        e.type = VideoTapEvent::Type::StreamStarted;
        e.generation = 1;
        e.codec = VideoTapCodec::H264;
        e.codecKnown = true;
        tap.publishEvent(e);
        _pump();
    }

    void publish(const RelayUnit &u)
    {
        VideoTapPacket p;
        p.codec = u.codec;
        p.data = u.data;
        p.parameterSets = u.parameterSets;
        p.keyFrame = u.keyFrame;
        p.pts90k = u.pts90k;
        p.dts90k = u.dts90k;
        p.receiveSteadyNs = relaySteadyNowNs();
        p.generation = generation;
        published << p.data;
        publishedKeys << p.keyFrame;
        lastPts = u.pts90k;
        tap.publishPacket(p);
        _pump();
    }

    /// The source restarts as the receiver reports it: `stop` the stream
    /// stopped first (the encoder is flushed and opens again), otherwise only
    /// a new generation (the encoder stays open). Units published after it
    /// carry `newGeneration`.
    void restartSource(quint64 newGeneration, bool stop)
    {
        if (stop) {
            VideoTapEvent s;
            s.type = VideoTapEvent::Type::StreamStopped;
            s.generation = generation;
            tap.publishEvent(s);
        }
        generation = newGeneration;
        VideoTapEvent e;
        e.type = stop ? VideoTapEvent::Type::StreamStarted : VideoTapEvent::Type::SourceChanged;
        e.generation = generation;
        e.codec = VideoTapCodec::H264;
        e.codecKnown = true;
        tap.publishEvent(e);
        _pump();
    }

    /// Local recording's consumer got every unit, byte for byte, in order, none dropped.
    bool recordingIntact(QString *why) const
    {
        if (recorded.size() != published.size()) {
            *why = QStringLiteral("recording got %1 of %2 units").arg(recorded.size()).arg(published.size());
            return false;
        }
        for (qsizetype i = 0; i < published.size(); ++i) {
            if (recorded.at(i) != published.at(i) || recordedKeys.at(i) != publishedKeys.at(i)) {
                *why = QStringLiteral("recording unit %1 differs").arg(i);
                return false;
            }
        }
        if (recorder->stats().dropped != 0) {
            *why = QStringLiteral("recording dropped %1 units").arg(recorder->stats().dropped);
            return false;
        }
        return true;
    }

    qint64 firstAt(GState s, qint64 after = -1) const
    {
        for (const Transition &t : transitions) {
            if (t.state == s && t.atPts > after) {
                return t.atPts;
            }
        }
        return -1;
    }

    QString timeline() const
    {
        QStringList parts;
        for (const Transition &t : transitions) {
            parts << QStringLiteral("%1 at %2 s").arg(QString::fromLatin1(stateName(t.state))).arg(t.atPts / 90000.0, 0, 'f', 1);
        }
        return parts.isEmpty() ? QStringLiteral("no change") : parts.join(QStringLiteral(", "));
    }

    VideoPacketTap tap;
    std::shared_ptr<VideoPacketConsumer> recorder;
    RelayPipeline pipeline;
    BitrateGuard guard;

    /// An encoderRestarted the rig saw (with `grace`), and the guard right after it.
    struct Restart
    {
        qint64 atPts = 0;
        qint64 graceLeft90k = 0;
        qsizetype sentBefore = 0;
        qsizetype transitionsBefore = 0;
    };

    QList<SentFragment> sent;
    QList<SentFragment> content;    ///< the pipeline's content measures, in order
    QList<Restart> restarts;
    quint64 generation = 1;
    QList<Transition> transitions;
    QList<QByteArray> published;
    QList<bool> publishedKeys;
    QList<QByteArray> recorded;
    QList<bool> recordedKeys;
    qint64 lastPts = 0;
    int producedWhilePaused = 0;
    qint64 contentBytes = 0;
    qint64 contentDuration90k = 0;

private:
    void _apply()
    {
        transitions << Transition{lastPts, guard.state()};
        RelayPipeline *p = &pipeline;
        const int pass = VideoRelay::guardPass(guard.state());
        QMetaObject::invokeMethod(p, [p, pass]() { p->setPass(pass); }, Qt::QueuedConnection);
    }

    void _pump()
    {
        // The pipeline's drain and the guard's filter are queued calls, as on the worker thread.
        for (int i = 0; i < 4; ++i) {
            QCoreApplication::processEvents();
        }
        VideoTapItem item;
        while (recorder->tryPop(item)) {
            if (const VideoTapPacket *p = std::get_if<VideoTapPacket>(&item)) {
                recorded << p->data;
                recordedKeys << p->keyFrame;
            }
        }
    }
};

/// The largest 2 s window rate of what went out, per guard state, over the
/// fragments sent between `fromPts` and `toPts`. A window never spans a
/// change of state (the guard starts its own window again at each one).
struct WindowStats
{
    double maxBps = 0;
    int windows = 0;
};

QMap<int, WindowStats> sentWindows(const QList<SentFragment> &sent, qint64 fromPts = -1,
                                   qint64 toPts = std::numeric_limits<qint64>::max())
{
    QMap<int, WindowStats> out;
    std::deque<SentFragment> w;
    qint64 bytes = 0;
    qint64 dur = 0;
    int runState = -1;
    for (const SentFragment &f : sent) {
        if (f.atPts <= fromPts || f.atPts > toPts) {
            continue;
        }
        if (int(f.state) != runState) {
            w.clear();
            bytes = 0;
            dur = 0;
            runState = int(f.state);
        }
        w.push_back(f);
        bytes += f.bytes;
        dur += f.duration90k;
        while (w.size() > 1 && dur - w.front().duration90k >= BitrateGuard::kWindow90k) {
            bytes -= w.front().bytes;
            dur -= w.front().duration90k;
            w.pop_front();
        }
        if (dur >= BitrateGuard::kWindow90k) {
            WindowStats &s = out[runState];
            s.maxBps = std::max(s.maxBps, BitrateGuard::rateBps(bytes, dur));
            ++s.windows;
        }
    }
    return out;
}

/// The fragments the guard judged in a rig with `grace`: those after the
/// grace that followed the encoder's last open, counted as the guard counts
/// them (a fragment that ends the grace is not judged).
QList<SentFragment> judgedSent(const GuardRig &rig)
{
    const qsizetype openedAt = rig.restarts.isEmpty() ? 0 : rig.restarts.last().sentBefore;
    qint64 graceLeft = BitrateGuard::kStartGrace90k;
    for (qsizetype i = openedAt; i < rig.sent.size(); ++i) {
        if (graceLeft > 0) {
            graceLeft = std::max<qint64>(0, graceLeft - rig.sent.at(i).duration90k);
            continue;
        }
        return rig.sent.mid(i);
    }
    return {};
}

/// Synthetic content at 15 fps (Low's rate) with a 1 s GOP. Slice bytes are
/// random and never zero (so no start code appears inside), the size is what
/// the guard sees. Noise sizes are the measurement on h264_mf at Low: 1761 kb/s.
struct Content
{
    const char *name;
    int keyBytes;
    int deltaBytes;
    int jitterPercent;
};

const Content kNoise{"noise", 14675, 14675, 0};             // 1761 kb/s; keyframes alone 117 kb/s
const Content kNoiseHeavyKeys{"noise, heavy keyframes", 120000, 8000, 0};  // 1856 kb/s; keyframes alone 960 kb/s
const Content kScene{"scene", 16000, 5848, 4};               // 783 kb/s, the scene measurement

class SyntheticSource
{
public:
    void feed(GuardRig &rig, const Content &c, double seconds)
    {
        const int frames = int(seconds * 15 + 0.5);
        for (int i = 0; i < frames; ++i) {
            const bool key = (_frame % 15) == 0;
            int size = key ? c.keyBytes : c.deltaBytes;
            if (c.jitterPercent > 0 && !key) {
                _seed = _seed * 1664525u + 1013904223u;
                size += size * (int(_seed >> 24) % (2 * c.jitterPercent + 1) - c.jitterPercent) / 100;
            }
            rig.publish(_unit(key, _frame * 6000LL, size));
            ++_frame;
        }
    }

private:
    RelayUnit _unit(bool key, qint64 pts, int sliceBytes)
    {
        using namespace RelayTest;
        static const QByteArray sps = makeSps(640, 360);
        static const QByteArray pps = makePps();
        QByteArray slice(std::max(2, sliceBytes), '\x01');
        for (qsizetype i = 1; i < slice.size(); ++i) {
            _seed = _seed * 1664525u + 1013904223u;
            slice[i] = char(1 + (_seed >> 24) % 255);
        }
        slice[0] = key ? '\x65' : '\x41';
        RelayUnit u;
        u.codec = VideoTapCodec::H264;
        u.data = (key ? startCode() + sps + startCode() + pps : QByteArray()) + startCode() + slice;
        u.parameterSets = startCode() + sps + startCode() + pps;
        u.keyFrame = key;
        u.pts90k = pts;
        u.dts90k = pts;
        u.generation = 1;
        return u;
    }

    qint64 _frame = 0;
    quint32 _seed = 2463534242u;
};

QString kbps(double bps)
{
    return QString::number(bps / 1000.0, 'f', 0);
}

} // namespace

void RelayEncoderTest::_lowOutput()
{
#ifndef QGC_FFMPEG_STREAMING
    QSKIP("this build has no FFmpeg");
#else
    const EncoderChoice enc = probeOrEmpty();
    if (!enc.ok) {
        QSKIP("no H.264 encoder opens on this machine");
    }
    QString err;
    const QList<RelayUnit> source = makeSource(enc, 1280, 720, 300, &err);
    QVERIFY2(!source.isEmpty(), qPrintable(err));
    QVERIFY(source.first().keyFrame);

    QList<RelayUnit> out;
    RelayEncoder relay(enc, RelayQuality::Low, [&out](const RelayUnit &u) { out << u; });
    for (RelayUnit u : source) {
        u.receiveSteadyNs = relaySteadyNowNs();
        relay.push(u);
    }
    relay.flush();
    QVERIFY2(!out.isEmpty(), qPrintable(relay.lastError()));
    QCOMPARE(relay.outputWidth(), 640);
    QCOMPARE(relay.outputHeight(), 360);

    // The SPS the encoder wrote says 640x360.
    const VideoCodecInfo parsed = VideoCodecInfo::fromAnnexB(false, out.first().parameterSets);
    QVERIFY(parsed.valid);
    QCOMPARE(parsed.width, 640);
    QCOMPARE(parsed.height, 360);

    // About 15 fps, increasing times, decode order = presentation order (no B-frames).
    qint64 lastPts = -1;
    qint64 lastKey = -1;
    qint64 maxKeyGap = 0;
    int keys = 0;
    qint64 bytes = 0;
    for (const RelayUnit &u : out) {
        QVERIFY(u.pts90k > lastPts);
        QCOMPARE(u.dts90k, u.pts90k);
        lastPts = u.pts90k;
        bytes += u.data.size();
        if (u.keyFrame) {
            if (lastKey >= 0) {
                maxKeyGap = qMax(maxKeyGap, u.pts90k - lastKey);
            }
            lastKey = u.pts90k;
            ++keys;
        }
    }
    const double seconds = 10.0;
    const double kbps = bytes * 8.0 / seconds / 1000.0;
    qInfo().noquote() << QStringLiteral("G109 Low on %1: %2 frames, %3 keyframes, longest GOP %4 ms, %5 kb/s")
                             .arg(enc.label()).arg(out.size()).arg(keys).arg(maxKeyGap / 90).arg(kbps, 0, 'f', 0);
    QVERIFY2(out.size() >= 135 && out.size() <= 165, qPrintable(QString::number(out.size())));
    QVERIFY(keys >= 9);
    QVERIFY2(maxKeyGap <= 90000 + 6000, qPrintable(QString::number(maxKeyGap)));
    // The encoder aims at 85 percent of the cap (680 kb/s): not starved, and
    // never over the guard's trip line over 10 s.
    const double aimKbps = relayEncoderBitRate(RelayQuality::Low) / 1000.0;
    QCOMPARE(relayEncoderBitRate(RelayQuality::Low), 680000);
    QCOMPARE(relayEncoderBitRate(RelayQuality::Medium), 2125000);
    QVERIFY2(kbps >= aimKbps * 0.8 && kbps <= kLowCap * BitrateGuard::kTripFactor / 1000.0,
             qPrintable(QString::number(kbps)));

    // Through the fragmenter: every fragment starts at a keyframe (1 s GOP, 1 s cuts).
    LiveFragmenter frag;
    QList<LiveFragment> frags;
    frag.setOutput([&frags](const LiveFragment &f) { frags << f; });
    for (const RelayUnit &u : out) {
        frag.push(u);
    }
    frag.flush();
    QVERIFY(frags.size() >= 9);
    for (const LiveFragment &f : frags) {
        QVERIFY(f.keyframeStart);
        QVERIFY(f.bytes.size() <= VideoRelayLimits::kMaxFragmentBytes);
        QCOMPARE(f.format.width, 640);
    }
#endif
}

void RelayEncoderTest::_bitrateSurvey()
{
#ifndef QGC_FFMPEG_STREAMING
    QSKIP("this build has no FFmpeg");
#else
    // Evidence, not a gate: the Low bitrate this machine's encoder gives for
    // each rate-control setting, on a scene and on pure noise.
    const EncoderChoice enc = probeOrEmpty();
    if (!enc.ok) {
        QSKIP("no H.264 encoder opens on this machine");
    }
    QStringList variants;
    variants << enc.options;
    if (enc.name == QLatin1String("h264_mf")) {
        const QString hw = enc.hardware ? QStringLiteral("hw_encoding=1") : QStringLiteral("hw_encoding=0");
        variants << hw + QStringLiteral(",rate_control=cbr")
                 << hw + QStringLiteral(",rate_control=ld_vbr")
                 << hw + QStringLiteral(",rate_control=u_vbr")
                 << hw + QStringLiteral(",rate_control=pc_vbr");
    }
    for (const bool noise : {false, true}) {
        QString err;
        const QList<RelayUnit> source = makeSource(enc, 1280, 720, 300, &err, noise);
        QVERIFY2(!source.isEmpty(), qPrintable(err));
        for (const QString &opts : variants) {
            EncoderChoice c = enc;
            c.options = opts;
            qint64 bytes = 0;
            int frames = 0;
            RelayEncoder relay(c, RelayQuality::Low, [&bytes, &frames](const RelayUnit &u) {
                bytes += u.data.size();
                ++frames;
            });
            for (RelayUnit u : source) {
                u.receiveSteadyNs = relaySteadyNowNs();
                relay.push(u);
            }
            relay.flush();
            qInfo().noquote() << QStringLiteral("G109 bitrate survey: %1 [%2] %3: %4 frames, %5 kb/s (aim 680, cap 800)")
                                     .arg(enc.label(), opts, noise ? QStringLiteral("noise") : QStringLiteral("scene"))
                                     .arg(frames)
                                     .arg(bytes * 8.0 / 10.0 / 1000.0, 0, 'f', 0);
        }
    }
#endif
}

void RelayEncoderTest::_capTripsOnSlowEncoder()
{
#ifndef QGC_FFMPEG_STREAMING
    QSKIP("this build has no FFmpeg");
#else
    const EncoderChoice enc = probeOrEmpty();
    if (!enc.ok) {
        QSKIP("no H.264 encoder opens on this machine");
    }
    QString err;
    const QList<RelayUnit> source = makeSource(enc, 1280, 720, 120, &err);
    QVERIFY2(!source.isEmpty(), qPrintable(err));

    // Medium at 30 fps has 33 ms per frame; 30 ms more per frame is over 60 percent.
    RelayEncoder slow(enc, RelayQuality::Medium, [](const RelayUnit &) {});
    slow.setInjectedDelayMs(30);
    for (RelayUnit u : source) {
        u.receiveSteadyNs = relaySteadyNowNs();
        slow.push(u);
    }
    qInfo().noquote() << QStringLiteral("G109 cap: injected slow Medium busy ratio %1").arg(slow.busyRatio(), 0, 'f', 2);
    QVERIFY(slow.overloaded());

    // The real encoder at Low keeps up.
    RelayEncoder fine(enc, RelayQuality::Low, [](const RelayUnit &) {});
    for (RelayUnit u : source) {
        u.receiveSteadyNs = relaySteadyNowNs();
        fine.push(u);
    }
    qInfo().noquote() << QStringLiteral("G109 cap: %1 at Low busy ratio %2").arg(enc.label()).arg(fine.busyRatio(), 0, 'f', 2);
    QVERIFY(!fine.overloaded());
#endif
}

void RelayEncoderTest::_guardLogic()
{
    // 1 s of media per call; byte counts are kb/s x 125.
    const auto second = [](double kbitPerS) { return qint64(kbitPerS * 125.0); };
    BitrateGuard g;
    g.setCap(kLowCap);

    // A scene at 783 kb/s, and a single second at 950: the window holds.
    for (int i = 0; i < 10; ++i) {
        QVERIFY(!g.onSent(second(783), kSecond90k));
        QVERIFY(!g.onContent(second(783), kSecond90k));
    }
    QVERIFY(!g.onSent(second(950), kSecond90k));            // (783 + 950) / 2 = 867 < 880
    QCOMPARE(g.state(), GState::Normal);

    // Noise at 1761 kb/s: over cap x 1.10 within one window, keyframes only.
    QVERIFY(g.onSent(second(1761), kSecond90k));
    QCOMPARE(g.state(), GState::KeyframesOnly);
    QCOMPARE(g.trips(), 1);
    QCOMPARE(g.quietNeeded90k(), 5 * kSecond90k);
    // Keyframes alone at 117 kb/s fit under the cap: it stays there.
    for (int i = 0; i < 5; ++i) {
        QVERIFY(!g.onSent(second(117), kSecond90k));
        QVERIFY(!g.onContent(second(1761), kSecond90k));
    }
    QCOMPARE(g.state(), GState::KeyframesOnly);
    // The content settles (half-second measures, as RelayPipeline sends them):
    // first the window clears of noise, then 5 s quiet.
    int halves = 0;
    while (!g.onContent(second(783) / 2, kSecond90k / 2)) {
        QVERIFY2(++halves < 40, "never came back");
    }
    ++halves;
    QVERIFY2(halves >= 10 && halves <= 15, qPrintable(QString::number(halves)));
    QCOMPARE(g.state(), GState::Normal);

    // Content at the cap itself counts as settled; just over it does not.
    g.reset();
    QVERIFY(g.onSent(2 * second(1761), 2 * kSecond90k));
    for (int i = 0; i < 20; ++i) {
        QVERIFY(!g.onContent(second(801), kSecond90k));
    }
    QCOMPARE(g.state(), GState::KeyframesOnly);
    int seconds = 0;
    while (!g.onContent(second(800), kSecond90k)) {
        QVERIFY(++seconds < 20);
    }
    QCOMPARE(g.state(), GState::Normal);

    // Tripping again within a minute: the quiet period doubles (backoff) ...
    QVERIFY(g.onSent(2 * second(1761), 2 * kSecond90k));
    QCOMPARE(g.trips(), 2);
    QCOMPARE(g.quietNeeded90k(), 10 * kSecond90k);
    for (int i = 0; i < 20 && g.onContent(second(500), kSecond90k) == false; ++i) {
    }
    QCOMPARE(g.state(), GState::Normal);
    QVERIFY(g.onSent(2 * second(1761), 2 * kSecond90k));
    QCOMPARE(g.quietNeeded90k(), 20 * kSecond90k);
    for (int i = 0; i < 40 && g.onContent(second(500), kSecond90k) == false; ++i) {
    }
    // ... and starts again from 5 s after a calm minute.
    for (int i = 0; i < 60; ++i) {
        g.onSent(second(500), kSecond90k);
    }
    QCOMPARE(g.trips(), 0);
    QVERIFY(g.onSent(2 * second(1761), 2 * kSecond90k));
    QCOMPARE(g.quietNeeded90k(), 5 * kSecond90k);

    // Keyframes alone over the cap: paused; nothing sent counts while paused.
    QVERIFY(!g.onSent(second(960), kSecond90k));
    QVERIFY(g.onSent(second(960), kSecond90k));
    QCOMPARE(g.state(), GState::Paused);
    QVERIFY(!g.onSent(second(5000), 4 * kSecond90k));
    QCOMPARE(g.state(), GState::Paused);

    // The quiet period is judged on its own rate (CI 80961dc: a scene at
    // 783 kb/s has 2 s windows at 816 with 3 keyframes in them, and judging
    // each window restarted the period). A window between the cap and the
    // trip line does not restart it; the overrun coming back does; a period
    // over the cap as a whole is not quiet.
    BitrateGuard q;
    q.setCap(kLowCap);
    QVERIFY(q.onSent(2 * second(1761), 2 * kSecond90k));
    QCOMPARE(q.quietNeeded90k(), 5 * kSecond90k);
    QVERIFY(!q.onContent(second(700), kSecond90k));
    QVERIFY(!q.onContent(second(700), kSecond90k));         // window 700: the period starts
    QCOMPARE(q.settled90k(), 1 * kSecond90k);
    QVERIFY(!q.onContent(second(850), kSecond90k));         // window 775
    QVERIFY(!q.onContent(second(850), kSecond90k));         // window 850: over the cap, under 880
    QCOMPARE(q.settled90k(), 3 * kSecond90k);
    QVERIFY(!q.onContent(second(700), kSecond90k));
    QVERIFY(q.onContent(second(700), kSecond90k));          // 5 s at 760 kb/s
    QCOMPARE(q.state(), GState::Normal);
    QVERIFY(q.onSent(2 * second(1761), 2 * kSecond90k));
    QCOMPARE(q.quietNeeded90k(), 10 * kSecond90k);
    QVERIFY(!q.onContent(second(700), kSecond90k));
    QVERIFY(!q.onContent(second(700), kSecond90k));
    QCOMPARE(q.settled90k(), 1 * kSecond90k);
    QVERIFY(!q.onContent(second(1761), kSecond90k));        // window 1230: the overrun again
    QCOMPARE(q.settled90k(), qint64(0));
    QVERIFY(!q.onContent(second(700), kSecond90k));
    QVERIFY(!q.onContent(second(700), kSecond90k));
    QCOMPARE(q.settled90k(), 1 * kSecond90k);
    for (int i = 0; i < 9; ++i) {
        QVERIFY(!q.onContent(second(860), kSecond90k));     // 10 s at 844 kb/s
    }
    QCOMPARE(q.settled90k(), qint64(0));
    QCOMPARE(q.state(), GState::KeyframesOnly);

    // Medium's cap; and Full has none.
    BitrateGuard m;
    m.setCap(VideoRelay::guardCap(RelayQuality::Medium));
    QVERIFY(!m.onSent(2 * second(2700), 2 * kSecond90k));       // under 2.5 x 1.10
    QVERIFY(m.onSent(2 * second(5000), 2 * kSecond90k));
    BitrateGuard full;
    full.setCap(VideoRelay::guardCap(RelayQuality::Full));
    QVERIFY(!full.onSent(second(50000), 4 * kSecond90k));
    QCOMPARE(full.state(), GState::Normal);
}

void RelayEncoderTest::_guardNoiseHeldOrPaused()
{
#ifndef QGC_FFMPEG_STREAMING
    QSKIP("this build has no FFmpeg");
#else
    // The pipeline's Full path carries the synthetic units as they are; the
    // guard gets Low's cap, so no encoder is needed and this runs on Linux too.
    {
        GuardRig rig(RelayQuality::Full, kLowCap);
        SyntheticSource src;
        src.feed(rig, kNoise, 12);
        const qint64 tripped = rig.firstAt(GState::KeyframesOnly);
        const QMap<int, WindowStats> before = sentWindows(rig.sent, -1, tripped);
        const QMap<int, WindowStats> after = sentWindows(rig.sent, tripped);
        qInfo().noquote() << QStringLiteral("G109 guard, noise at Low (synthetic %1 kb/s): %2; before %3 kb/s, keyframes only at most %4 kb/s over %5 windows")
                                 .arg(kbps(rig.contentBytes * 8.0 * 90000 / std::max<qint64>(1, rig.contentDuration90k)), rig.timeline(),
                                      kbps(before.value(int(GState::Normal)).maxBps),
                                      kbps(after.value(int(GState::KeyframesOnly)).maxBps))
                                 .arg(after.value(int(GState::KeyframesOnly)).windows);
        QVERIFY2(tripped > 0 && tripped <= 3 * kSecond90k + kSecond90k / 2, qPrintable(rig.timeline()));
        // After it acted, every window that went out is at or under the cap.
        QVERIFY(after.value(int(GState::KeyframesOnly)).windows >= 5);
        QVERIFY2(after.value(int(GState::KeyframesOnly)).maxBps <= kLowCap,
                 qPrintable(kbps(after.value(int(GState::KeyframesOnly)).maxBps)));
        QCOMPARE(after.value(int(GState::Normal)).windows, 0);
        QCOMPARE(rig.guard.state(), GState::KeyframesOnly);    // keyframes alone fit
        for (const SentFragment &f : rig.sent) {
            if (f.atPts > tripped) {
                QCOMPARE(f.samples, 1);                         // one keyframe each
            }
        }
        QString why;
        QVERIFY2(rig.recordingIntact(&why), qPrintable(why));
    }
    {
        // Keyframes alone over the cap: paused, and nothing more goes out.
        GuardRig rig(RelayQuality::Full, kLowCap);
        SyntheticSource src;
        src.feed(rig, kNoiseHeavyKeys, 12);
        const qint64 paused = rig.firstAt(GState::Paused);
        qInfo().noquote() << QStringLiteral("G109 guard, noise with heavy keyframes at Low: %1").arg(rig.timeline());
        QVERIFY2(paused > 0 && paused <= 6 * kSecond90k, qPrintable(rig.timeline()));
        QCOMPARE(rig.guard.state(), GState::Paused);
        for (const SentFragment &f : rig.sent) {
            QVERIFY(f.atPts <= paused);
        }
        QCOMPARE(rig.producedWhilePaused, 0);                  // the pipeline stops cutting too
        QCOMPARE(VideoRelay::guardPausedText(QStringLiteral("Low")),
                 QStringLiteral("Video forwarding paused: the picture needs more bandwidth than Low allows"));
        QString why;
        QVERIFY2(rig.recordingIntact(&why), qPrintable(why));
    }
#endif
}

void RelayEncoderTest::_guardSceneUnaffected()
{
#ifndef QGC_FFMPEG_STREAMING
    QSKIP("this build has no FFmpeg");
#else
    GuardRig rig(RelayQuality::Full, kLowCap);
    SyntheticSource src;
    src.feed(rig, kScene, 20);
    const QMap<int, WindowStats> w = sentWindows(rig.sent);
    qInfo().noquote() << QStringLiteral("G109 guard, scene at Low (synthetic): %1, at most %2 kb/s sent over a window")
                             .arg(rig.timeline(), kbps(w.value(int(GState::Normal)).maxBps));
    QVERIFY2(rig.transitions.isEmpty(), qPrintable(rig.timeline()));
    QCOMPARE(rig.guard.state(), GState::Normal);
    int samples = 0;
    for (const SentFragment &f : rig.sent) {
        samples += f.samples;
    }
    // Everything went out but the fragment still being built.
    QVERIFY2(samples >= int(rig.published.size()) - 15, qPrintable(QString::number(samples)));
    QVERIFY(w.value(int(GState::Normal)).windows >= 15);
    QString why;
    QVERIFY2(rig.recordingIntact(&why), qPrintable(why));
#endif
}

void RelayEncoderTest::_guardRecoversWithBackoff()
{
#ifndef QGC_FFMPEG_STREAMING
    QSKIP("this build has no FFmpeg");
#else
    GuardRig rig(RelayQuality::Full, kLowCap);
    SyntheticSource src;
    src.feed(rig, kScene, 4);           //  0 to  4 s
    src.feed(rig, kNoise, 6);           //  4 to 10 s
    src.feed(rig, kScene, 20);          // 10 to 30 s
    src.feed(rig, kNoise, 4);           // 30 to 34 s
    src.feed(rig, kScene, 20);          // 34 to 54 s
    qInfo().noquote() << QStringLiteral("G109 guard, scene, noise, scene, noise, scene at Low: %1").arg(rig.timeline());

    const qint64 trip1 = rig.firstAt(GState::KeyframesOnly);
    const qint64 back1 = rig.firstAt(GState::Normal, trip1);
    const qint64 trip2 = rig.firstAt(GState::KeyframesOnly, back1);
    const qint64 back2 = rig.firstAt(GState::Normal, trip2);
    QVERIFY2(trip1 >= 4 * kSecond90k && trip1 <= 7 * kSecond90k, qPrintable(rig.timeline()));
    // Back once the picture settled: 2 s for the window, then 5 s quiet.
    QVERIFY2(back1 >= 15 * kSecond90k && back1 <= 19 * kSecond90k, qPrintable(rig.timeline()));
    QVERIFY2(trip2 >= 30 * kSecond90k && trip2 <= 33 * kSecond90k, qPrintable(rig.timeline()));
    // Again within the minute: 10 s quiet (backoff).
    QVERIFY2(back2 >= 44 * kSecond90k && back2 <= 48 * kSecond90k, qPrintable(rig.timeline()));
    QCOMPARE(int(rig.transitions.size()), 4);
    QCOMPARE(rig.guard.state(), GState::Normal);

    // Between and after: the scene goes out whole, under the trip line.
    const QMap<int, WindowStats> mid = sentWindows(rig.sent, back1, trip2 - kSecond90k);
    const QMap<int, WindowStats> end = sentWindows(rig.sent, back2);
    QVERIFY(mid.value(int(GState::Normal)).windows >= 8);
    QVERIFY(end.value(int(GState::Normal)).windows >= 3);
    QVERIFY(mid.value(int(GState::Normal)).maxBps <= kLowCap * BitrateGuard::kTripFactor);
    QVERIFY(end.value(int(GState::Normal)).maxBps <= kLowCap * BitrateGuard::kTripFactor);
    // Once back, fragments carry whole GOPs again (the first may be the last
    // keyframe-only one, closed by the keyframe the full stream starts at).
    int whole = 0;
    for (const SentFragment &f : rig.sent) {
        if (f.atPts > back2 && f.samples >= 14) {
            ++whole;
        }
    }
    QVERIFY2(whole >= 3, qPrintable(QString::number(whole)));
    QString why;
    QVERIFY2(rig.recordingIntact(&why), qPrintable(why));
#endif
}

void RelayEncoderTest::_guardRealEncoderNoise()
{
#ifndef QGC_FFMPEG_STREAMING
    QSKIP("this build has no FFmpeg");
#else
    const EncoderChoice enc = probeOrEmpty();
    if (!enc.ok) {
        QSKIP("no H.264 encoder opens on this machine (the synthetic guard cases still ran)");
    }
    QString err;
    // As VideoRelay runs it: the encoder aims at 680 kb/s and the guard has its
    // start grace (6 s), re-armed when the encoder opens. Noise still trips,
    // on the first windows after the grace.
    const QList<RelayUnit> noise = makeSource(enc, 1280, 720, 360, &err, true);     // 12 s
    QVERIFY2(!noise.isEmpty(), qPrintable(err));
    const QList<RelayUnit> scene = makeSource(enc, 1280, 720, 420, &err, false);    // 14 s
    QVERIFY2(!scene.isEmpty(), qPrintable(err));

    GuardRig rig(RelayQuality::Low, kLowCap, enc, true);
    for (const RelayUnit &u : noise) {
        rig.publish(u);
    }
    const qint64 noiseEnd = 360 * 3000;
    const qint64 contentNoiseBytes = rig.contentBytes;
    const qint64 contentNoise90k = rig.contentDuration90k;
    for (RelayUnit u : scene) {
        u.pts90k += noiseEnd;
        u.dts90k += noiseEnd;
        rig.publish(u);
    }
    const double noiseBps = BitrateGuard::rateBps(contentNoiseBytes, contentNoise90k);
    const double sceneBps = BitrateGuard::rateBps(rig.contentBytes - contentNoiseBytes,
                                                  rig.contentDuration90k - contentNoise90k);
    const qint64 acted = rig.transitions.isEmpty() ? -1 : rig.transitions.first().atPts;
    const QMap<int, WindowStats> before = sentWindows(rig.sent, -1, acted < 0 ? noiseEnd : acted);
    const QMap<int, WindowStats> held = sentWindows(rig.sent, acted, noiseEnd);
    qInfo().noquote() << QStringLiteral("G109 guard on %1 at Low: encoder made %2 kb/s on noise, %3 kb/s on the scene; %4; "
                                        "before it acted %5 kb/s, keyframes only at most %6 kb/s")
                             .arg(enc.label(), kbps(noiseBps), kbps(sceneBps), rig.timeline(),
                                  kbps(before.value(int(GState::Normal)).maxBps),
                                  kbps(held.value(int(GState::KeyframesOnly)).maxBps));

    // CI shows only failure lines: what the guard saw, for the message.
    QStringList frags;
    for (const SentFragment &f : rig.sent) {
        if (f.atPts <= noiseEnd + 2 * kSecond90k) {
            frags << QStringLiteral("%1s:%2/%3").arg(f.atPts / 90000.0, 0, 'f', 1)
                         .arg(kbps(BitrateGuard::rateBps(f.bytes, f.duration90k)))
                         .arg(f.duration90k / 90000.0, 0, 'f', 2);
        }
    }
    QStringList opens;
    for (const GuardRig::Restart &r : rig.restarts) {
        opens << QStringLiteral("%1s").arg(r.atPts / 90000.0, 0, 'f', 1);
    }
    const QString seen = QStringLiteral("noise content %1 kb/s, scene %2; %3; restarts at %4; sent (at s: kb/s / s) %5")
                             .arg(kbps(noiseBps), kbps(sceneBps), rig.timeline(), opens.join(QLatin1Char(' ')),
                                  frags.join(QLatin1Char(' ')));

    if (acted < 0) {
        // This encoder held its own rate on noise after its opening: nothing
        // for the guard to do. The windows it judged (after the grace) are
        // under the trip line. The Windows runner's software encoder at the
        // 680 kb/s aim: one opening fragment at 11.7 Mb/s (inside the grace),
        // then about 620 kb/s on noise. The guard's own trip on sustained
        // noise is _guardNoiseHeldOrPaused, _guardStartGrace and
        // _guardRestartRearmsGrace.
        const QMap<int, WindowStats> judged = sentWindows(judgedSent(rig), -1, noiseEnd);
        QVERIFY2(judged.value(int(GState::Normal)).windows >= 3, qPrintable(seen));
        QVERIFY2(judged.value(int(GState::Normal)).maxBps <= kLowCap * BitrateGuard::kTripFactor,
                 qPrintable(kbps(judged.value(int(GState::Normal)).maxBps) + QStringLiteral(" kb/s; ") + seen));
    } else {
        // The grace, the first 2 s window, and up to 2 s of fragment and
        // encoder delay between what is published and what is sent.
        QVERIFY2(acted <= BitrateGuard::kStartGrace90k + 4 * kSecond90k, qPrintable(seen));
        QVERIFY2(!rig.restarts.isEmpty(), "the encoder never reported it opened");
        // Noise at Low: held at or under the cap over the window, or paused.
        const qint64 paused = rig.firstAt(GState::Paused, acted - 1);
        if (paused > 0 && paused <= noiseEnd) {
            for (const SentFragment &f : rig.sent) {
                QVERIFY2(f.atPts <= paused || f.atPts > noiseEnd, qPrintable(rig.timeline()));
            }
        } else {
            QVERIFY2(held.value(int(GState::KeyframesOnly)).maxBps <= kLowCap,
                     qPrintable(kbps(held.value(int(GState::KeyframesOnly)).maxBps)));
        }
        QCOMPARE(held.value(int(GState::Normal)).windows, 0);
        // Recovery: the scene brings it back to everything.
        QVERIFY2(rig.guard.state() == GState::Normal, qPrintable(rig.timeline()));
        const qint64 back = rig.firstAt(GState::Normal, acted);
        QVERIFY2(back > noiseEnd, qPrintable(rig.timeline()));
        const QMap<int, WindowStats> after = sentWindows(rig.sent, back);
        QVERIFY(after.value(int(GState::Normal)).windows >= 2);
        QVERIFY(after.value(int(GState::Normal)).maxBps <= kLowCap * BitrateGuard::kTripFactor);
    }
    QString why;
    QVERIFY2(rig.recordingIntact(&why), qPrintable(why));
#endif
}

namespace {

/// Drives EncodeLoadMeter on a clock of its own, as RelayEncoder::push does.
struct MeterDriver
{
    EncodeLoadMeter meter;
    qint64 now = 1'000'000'000LL;

    explicit MeterDriver(int fps) : meter(fps) {}

    /// One push that encodes one frame received `receivedAgoNs` before it,
    /// using `cpuNs` of CPU and `wallNs` of wall time; the next push follows
    /// after `idleNs`. True when the cap tripped.
    bool push(qint64 cpuNs, qint64 receivedAgoNs = 0, qint64 wallNs = -1, qint64 idleNs = -1, bool setup = false)
    {
        meter.begin(now);
        meter.frameEncoded(now - receivedAgoNs, now);
        const qint64 wall = wallNs < 0 ? cpuNs : wallNs;
        const bool tripped = meter.end(now + wall, cpuNs, setup);
        now += wall + (idleNs < 0 ? std::max<qint64>(0, meter.intervalNs() - wall) : idleNs);
        return tripped;
    }
};

constexpr qint64 kMs = 1'000'000LL;

} // namespace

void RelayEncoderTest::_capIgnoresInputGap()
{
    // Low, 15 fps (66 ms a frame), an encoder that needs 20 ms a frame.
    MeterDriver d(relayQualityTarget(RelayQuality::Low).maxFps);
    QCOMPARE(d.meter.fps(), 15);
    for (int i = 0; i < 60; ++i) {
        QVERIFY(!d.push(20 * kMs, 5 * kMs));
    }
    QVERIFY(d.meter.busyRatio() > 0.25 && d.meter.busyRatio() < 0.35);
    const int restarts = d.meter.restarts();

    // The input pauses for 3 s (Application Settings opened, the receiver
    // stopped feeding). The frames held back come in a burst afterwards,
    // received before the pause ended, and are pushed back to back.
    d.now += 3000 * kMs;
    for (int i = 0; i < 40; ++i) {
        QVERIFY(!d.push(20 * kMs, 3000 * kMs - i * 20 * kMs, 20 * kMs, 0));
    }
    QVERIFY(d.meter.restarts() > restarts);
    QCOMPARE(d.meter.lateFrames(), 0);

    // A source restart: the decoder and encoder open again (900 ms of setup
    // in one push), and the frames that queued meanwhile follow.
    QVERIFY(!d.push(900 * kMs, 0, 900 * kMs, 0, true));
    for (int i = 0; i < 20; ++i) {
        QVERIFY(!d.push(20 * kMs, 900 * kMs, 20 * kMs, 0));
    }
    // The relay thread kept off the CPU by the rest of GC (low priority): a
    // long wall time with little CPU is not encoder load either.
    for (int i = 0; i < 60; ++i) {
        QVERIFY(!d.push(15 * kMs, 5 * kMs, 60 * kMs));
    }
    QVERIFY2(!d.meter.overloaded(), qPrintable(QString::number(d.meter.busyRatio())));
    QVERIFY(d.meter.busyRatio() < EncodeLoadMeter::kBusyLimit);
}

void RelayEncoderTest::_capTripsOnTrulySlowEncoder()
{
    // An encoder that needs 50 ms of CPU a frame at Low (66 ms): 0.75, over
    // 0.6. It trips once 2 s of frames are in, with no pause to hide behind.
    {
        MeterDriver d(relayQualityTarget(RelayQuality::Low).maxFps);
        int pushes = 0;
        while (!d.push(50 * kMs, 5 * kMs)) {
            QVERIFY2(++pushes < 100, "a slow encoder never tripped the cap");
        }
        QVERIFY(pushes + 1 >= 2 * d.meter.fps());
        QVERIFY(d.meter.overloaded());
        QVERIFY(d.meter.busyRatio() > EncodeLoadMeter::kBusyLimit);
    }
    // Fast on the CPU but behind: every frame waits 4 intervals. Late frames trip it.
    {
        MeterDriver d(relayQualityTarget(RelayQuality::Low).maxFps);
        int pushes = 0;
        while (!d.push(5 * kMs, 4 * d.meter.intervalNs())) {
            QVERIFY2(++pushes < 100, "late frames never tripped the cap");
        }
        QVERIFY(d.meter.lateFrames() * 4 > d.meter.frames());
    }
    // A slow encoder with input pauses in between still trips after them.
    {
        MeterDriver d(relayQualityTarget(RelayQuality::Medium).maxFps);
        for (int i = 0; i < 10; ++i) {
            QVERIFY(!d.push(40 * kMs, 5 * kMs));
        }
        d.now += 2000 * kMs;
        bool tripped = false;
        for (int i = 0; i < 200 && !tripped; ++i) {
            tripped = d.push(40 * kMs, 5 * kMs);
        }
        QVERIFY(tripped);
    }
    // What VideoRelay does with it: Medium steps down to Low once; Low stops.
    QCOMPARE(VideoRelay::capAction(RelayQuality::Medium, false), VideoRelay::CapAction::StepToLow);
    QCOMPARE(VideoRelay::capAction(RelayQuality::Low, true), VideoRelay::CapAction::Stop);
    QCOMPARE(VideoRelay::capAction(RelayQuality::Low, false), VideoRelay::CapAction::Stop);
    QCOMPARE(VideoRelay::capAction(RelayQuality::Medium, true), VideoRelay::CapAction::Stop);
}

void RelayEncoderTest::_capSurvivesSourceRestart()
{
#ifndef QGC_FFMPEG_STREAMING
    QSKIP("this build has no FFmpeg");
#else
    const EncoderChoice enc = probeOrEmpty();
    if (!enc.ok) {
        QSKIP("no H.264 encoder opens on this machine");
    }
    QString err;
    const QList<RelayUnit> source = makeSource(enc, 1280, 720, 90, &err);
    QVERIFY2(!source.isEmpty(), qPrintable(err));

    // The real encoder at Low: 3 s of video, the input pauses for 1.5 s, then
    // the source restarts (a new generation: the decoder opens again) with
    // frames that were received before the pause ended.
    RelayEncoder relay(enc, RelayQuality::Low, [](const RelayUnit &) {});
    for (RelayUnit u : source) {
        u.receiveSteadyNs = relaySteadyNowNs();
        relay.push(u);
    }
    const int restarts = relay.meter().restarts();
    const qint64 pausedAt = relaySteadyNowNs();
    QTest::qWait(1500);
    for (RelayUnit u : source) {
        u.generation = 2;
        u.pts90k += 90 * 3000;      // the source's clock goes on
        u.dts90k += 90 * 3000;
        u.receiveSteadyNs = pausedAt;
        relay.push(u);
    }
    qInfo().noquote() << QStringLiteral("G109 cap across a pause and a source restart on %1: busy %2, late %3 of %4, restarts %5")
                             .arg(enc.label()).arg(relay.busyRatio(), 0, 'f', 2)
                             .arg(relay.meter().lateFrames()).arg(relay.meter().frames())
                             .arg(relay.meter().restarts() - restarts);
    QVERIFY(relay.meter().restarts() >= restarts + 2);     // the pause, and the new decoder
    QCOMPARE(relay.meter().lateFrames(), 0);
    QVERIFY(!relay.overloaded());
    QVERIFY(relay.stats().framesEncoded > 60);
#endif
}

void RelayEncoderTest::_guardStartGrace()
{
    const auto second = [](double kbitPerS) { return qint64(kbitPerS * 125.0); };

    // The start of every Low session in the end-to-end run: 1080 kb/s for the
    // first 2 s (rate control settling, the opening keyframes), then a
    // normal picture at 790. Without a grace the first window trips.
    BitrateGuard bare;
    bare.setCap(kLowCap);
    QVERIFY(!bare.onSent(second(1080), kSecond90k));
    QVERIFY(bare.onSent(second(1080), kSecond90k));
    QCOMPARE(bare.state(), GState::KeyframesOnly);

    // With the grace VideoRelay gives (6 s of media) it never trips.
    QCOMPARE(BitrateGuard::kStartGrace90k, 6 * kSecond90k);
    BitrateGuard g;
    g.setStartGrace90k(BitrateGuard::kStartGrace90k);
    g.setCap(kLowCap);
    QCOMPARE(g.graceLeft90k(), BitrateGuard::kStartGrace90k);
    QVERIFY(!g.onSent(second(1080), kSecond90k));
    QVERIFY(!g.onSent(second(1080), kSecond90k));
    for (int i = 0; i < 40; ++i) {
        QVERIFY(!g.onSent(second(790), kSecond90k));
        QVERIFY(!g.onContent(second(790), kSecond90k));
    }
    QCOMPARE(g.state(), GState::Normal);
    QCOMPARE(g.graceLeft90k(), qint64(0));

    // The cap still holds for sustained content: noise from the start trips
    // on the first window after the grace (8 s in), and every restart
    // (reset, a new level) gives the grace again.
    g.reset();
    QCOMPARE(g.graceLeft90k(), BitrateGuard::kStartGrace90k);
    int seconds = 0;
    while (!g.onSent(second(1761), kSecond90k)) {
        QVERIFY2(++seconds < 20, "noise never tripped the guard");
    }
    QCOMPARE(seconds + 1, int(BitrateGuard::kStartGrace90k / kSecond90k) + 2);
    QCOMPARE(g.state(), GState::KeyframesOnly);
    g.setCap(VideoRelay::guardCap(RelayQuality::Medium));
    QCOMPARE(g.state(), GState::Normal);
    QCOMPARE(g.graceLeft90k(), BitrateGuard::kStartGrace90k);
    // Full has no cap: nothing to judge, grace or not.
    g.setCap(VideoRelay::guardCap(RelayQuality::Full));
    QVERIFY(!g.onSent(second(50000), 10 * kSecond90k));
}

void RelayEncoderTest::_guardRestartRearmsGrace()
{
    const auto second = [](double kbitPerS) { return qint64(kbitPerS * 125.0); };

    // Pure logic, everywhere. A Low stream running normally: the grace spent,
    // a full window at 790 kb/s.
    BitrateGuard g;
    g.setStartGrace90k(BitrateGuard::kStartGrace90k);
    g.setCap(kLowCap);
    for (int i = 0; i < 20; ++i) {
        QVERIFY(!g.onSent(second(790), kSecond90k));
        QVERIFY(!g.onContent(second(790), kSecond90k));
    }
    QCOMPARE(g.graceLeft90k(), qint64(0));
    QCOMPARE(g.state(), GState::Normal);

    // The P5 run's source restart: a new encoder opens at about 1 Mb/s. Not
    // re-armed, the first second of it trips against a window still holding
    // the old stream (the run tripped 0.3 s after the encoder opened).
    BitrateGuard stale = g;
    QVERIFY(stale.onSent(second(1080), kSecond90k));
    QCOMPARE(stale.state(), GState::KeyframesOnly);

    // Re-armed (what VideoRelay does on RelayPipeline::encoderRestarted): the
    // grace again, the old windows gone, and the new encoder's opening
    // seconds are not judged.
    g.restartGrace();
    QCOMPARE(g.graceLeft90k(), BitrateGuard::kStartGrace90k);
    QCOMPARE(g.state(), GState::Normal);
    for (int i = 0; i < 6; ++i) {
        QVERIFY(!g.onSent(second(1080), kSecond90k));
    }
    QCOMPARE(g.graceLeft90k(), qint64(0));
    for (int i = 0; i < 40; ++i) {
        QVERIFY(!g.onSent(second(770), kSecond90k));
        QVERIFY(!g.onContent(second(770), kSecond90k));
    }
    QCOMPARE(g.state(), GState::Normal);

    // Noise after a restart still trips, on the first window after the grace.
    g.restartGrace();
    int seconds = 0;
    while (!g.onSent(second(1761), kSecond90k)) {
        QVERIFY2(++seconds < 20, "noise never tripped the guard after a restart");
    }
    QCOMPARE(seconds + 1, int(BitrateGuard::kStartGrace90k / kSecond90k) + 2);
    QCOMPARE(g.state(), GState::KeyframesOnly);
    QCOMPARE(g.trips(), 1);
    // A restart is not the operator's choice: the state and the backoff stay.
    g.restartGrace();
    QCOMPARE(g.state(), GState::KeyframesOnly);
    QCOMPARE(g.trips(), 1);
    QCOMPARE(g.cap(), kLowCap);

#ifdef QGC_FFMPEG_STREAMING
    // The real chain, where an encoder opens: the pipeline reports every
    // encoder open and every new source generation before the new output.
    const EncoderChoice enc = probeOrEmpty();
    if (!enc.ok) {
        QSKIP("no H.264 encoder opens on this machine (the guard logic above still ran)");
    }
    QString err;
    const QList<RelayUnit> scene = makeSource(enc, 1280, 720, 300, &err, false);    // 10 s
    QVERIFY2(!scene.isEmpty(), qPrintable(err));
    const qint64 span = 300 * 3000;

    GuardRig rig(RelayQuality::Low, kLowCap, enc, true);
    const auto play = [&rig, &scene](qint64 offset90k) {
        for (RelayUnit u : scene) {
            u.pts90k += offset90k;
            u.dts90k += offset90k;
            rig.publish(u);
        }
    };
    play(0);
    QCOMPARE(rig.restarts.size(), qsizetype(1));
    QCOMPARE(rig.guard.graceLeft90k(), qint64(0));      // 10 s published, the grace spent

    // A source restart as in the P5 run: the stream stops (the encoder is
    // flushed) and starts again on a new generation; the encoder opens again.
    rig.restartSource(2, true);
    play(span);
    // Only a new generation (the receiver on another URI), the encoder stays open.
    const qint64 graceBeforeSourceChange = rig.guard.graceLeft90k();
    rig.restartSource(3, false);
    play(2 * span);

    QStringList at;
    for (const GuardRig::Restart &r : rig.restarts) {
        at << QStringLiteral("%1 s (grace %2 s)").arg(r.atPts / 90000.0, 0, 'f', 1).arg(r.graceLeft90k / 90000.0, 0, 'f', 1);
    }
    qInfo().noquote() << QStringLiteral("G109 guard restarts on %1: %2; %3")
                             .arg(enc.label(), at.join(QStringLiteral(", ")), rig.timeline());
    QCOMPARE(rig.restarts.size(), qsizetype(3));
    QCOMPARE(graceBeforeSourceChange, qint64(0));
    for (const GuardRig::Restart &r : rig.restarts) {
        QCOMPARE(r.graceLeft90k, BitrateGuard::kStartGrace90k);
    }
    QVERIFY(rig.restarts.at(1).atPts < span + 2 * kSecond90k);
    QVERIFY(rig.restarts.at(2).atPts < 2 * span + 2 * kSecond90k);
    QVERIFY2(rig.transitions.isEmpty(), qPrintable(rig.timeline()));
    QCOMPARE(rig.guard.graceLeft90k(), qint64(0));
    QString why;
    QVERIFY2(rig.recordingIntact(&why), qPrintable(why));
#endif
}

void RelayEncoderTest::_guardRealEncoderSceneUnderCap()
{
#ifndef QGC_FFMPEG_STREAMING
    QSKIP("this build has no FFmpeg");
#else
    // The P5 run's complaint: a normal picture at Low tripped the guard every
    // time Low started. 60 s of the scene through the real chain, as
    // VideoRelay runs it (the encoder aiming at 680 kb/s, the guard judging
    // the 800 kb/s cap with its start grace): no trip, and under the cap.
    const EncoderChoice enc = probeOrEmpty();
    if (!enc.ok) {
        QSKIP("no H.264 encoder opens on this machine");
    }
    QString err;
    const QList<RelayUnit> scene = makeSource(enc, 1280, 720, 600, &err, false);    // 20 s
    QVERIFY2(!scene.isEmpty(), qPrintable(err));
    const qint64 span = 600 * 3000;

    GuardRig rig(RelayQuality::Low, kLowCap, enc, true);
    for (int loop = 0; loop < 3; ++loop) {
        for (RelayUnit u : scene) {
            u.pts90k += loop * span;
            u.dts90k += loop * span;
            rig.publish(u);
        }
    }
    const qint64 end = 3 * span;

    // What the encoder made: over the whole minute, over each 10 s, and the
    // largest 2 s window (the guard's own window, before any grace).
    const double wholeBps = BitrateGuard::rateBps(rig.contentBytes, rig.contentDuration90k);
    QStringList tens;
    double maxTenBps = 0;
    {
        qint64 b = 0;
        qint64 d = 0;
        for (const SentFragment &c : rig.content) {
            b += c.bytes;
            d += c.duration90k;
            if (d >= 10 * kSecond90k) {
                const double r = BitrateGuard::rateBps(b, d);
                tens << kbps(r);
                maxTenBps = std::max(maxTenBps, r);
                b = 0;
                d = 0;
            }
        }
    }
    const QMap<int, WindowStats> contentWindows = sentWindows(rig.content);
    const QMap<int, WindowStats> sentAll = sentWindows(rig.sent);
    const QMap<int, WindowStats> judged = sentWindows(judgedSent(rig));
    qInfo().noquote() << QStringLiteral("G109 Low scene for 60 s on %1: encoder made %2 kb/s (aim %3, cap %4); "
                                        "per 10 s %5; largest 2 s window: content %6, sent %7, judged %8 kb/s; %9")
                             .arg(enc.label(), kbps(wholeBps), kbps(relayEncoderBitRate(RelayQuality::Low)), kbps(kLowCap),
                                  tens.join(QStringLiteral(" ")), kbps(contentWindows.value(int(GState::Normal)).maxBps),
                                  kbps(sentAll.value(int(GState::Normal)).maxBps),
                                  kbps(judged.value(int(GState::Normal)).maxBps))
                             .arg(rig.timeline());

    QVERIFY2(rig.contentDuration90k >= end - 2 * kSecond90k, qPrintable(QString::number(rig.contentDuration90k)));
    QVERIFY2(!rig.restarts.isEmpty(), "the encoder never reported it opened");
    QVERIFY2(rig.transitions.isEmpty(), qPrintable(rig.timeline()));
    QCOMPARE(rig.guard.state(), GState::Normal);
    QVERIFY2(wholeBps <= kLowCap, qPrintable(kbps(wholeBps)));
    QVERIFY2(maxTenBps <= kLowCap, qPrintable(tens.join(QStringLiteral(" "))));
    QVERIFY(judged.value(int(GState::Normal)).windows >= 20);
    QVERIFY2(judged.value(int(GState::Normal)).maxBps <= kLowCap * BitrateGuard::kTripFactor,
             qPrintable(kbps(judged.value(int(GState::Normal)).maxBps)));
    QString why;
    QVERIFY2(rig.recordingIntact(&why), qPrintable(why));
#endif
}
