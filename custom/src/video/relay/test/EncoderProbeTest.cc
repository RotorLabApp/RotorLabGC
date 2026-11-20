#include "EncoderProbeTest.h"

#include "EncoderProbe.h"
#include "RelayEncoder.h"
#include "VideoRelay.h"

#include <QtTest/QTest>

namespace {

EncoderProbeReport fakeReport(bool ok, bool hardware)
{
    EncoderProbeReport r;
    EncoderChoice c;
    c.name = QStringLiteral("h264_mf");
    c.hardware = hardware;
    c.ok = ok;
    c.msPerFrame = 1.5;
    r.tried << c;
    if (ok) {
        r.chosen = c;
    }
    return r;
}

QualityOffer offerFor(const EncoderProbeReport &r, VideoTapCodec codec, bool known = true)
{
    return QualityOffer::compute(codec, known, r.chosen.ok);
}

} // namespace

void EncoderProbeTest::_offerNoEncoder()
{
    const QualityOffer o = offerFor(fakeReport(false, false), VideoTapCodec::H264);
    QVERIFY(o.relayAllowed);
    QCOMPARE(o.levels, QList<RelayQuality>({RelayQuality::Full}));
    QCOMPARE(o.lowest(), RelayQuality::Full);
    QCOMPARE(o.reason, QStringLiteral("Low and Medium need a video encoder this computer does not offer."));
}

void EncoderProbeTest::_offerMfHardware()
{
    const EncoderProbeReport r = fakeReport(true, true);
    const QualityOffer o = offerFor(r, VideoTapCodec::H264);
    QVERIFY(o.relayAllowed);
    QCOMPARE(o.levels, QList<RelayQuality>({RelayQuality::Low, RelayQuality::Medium, RelayQuality::Full}));
    QCOMPARE(o.lowest(), RelayQuality::Low);
    QVERIFY(o.reason.isEmpty());
    QCOMPARE(r.chosen.label(), QStringLiteral("Media Foundation (hardware)"));
}

void EncoderProbeTest::_offerMfSoftware()
{
    const EncoderProbeReport r = fakeReport(true, false);
    const QualityOffer o = offerFor(r, VideoTapCodec::H264);
    QCOMPARE(o.levels, QList<RelayQuality>({RelayQuality::Low, RelayQuality::Medium, RelayQuality::Full}));
    QCOMPARE(r.chosen.label(), QStringLiteral("Media Foundation (Windows software encoder)"));
}

void EncoderProbeTest::_offerH265WithEncoder()
{
    const QualityOffer o = offerFor(fakeReport(true, true), VideoTapCodec::H265);
    QVERIFY(o.relayAllowed);
    QCOMPARE(o.levels, QList<RelayQuality>({RelayQuality::Low, RelayQuality::Medium}));
    QVERIFY(!o.offers(RelayQuality::Full));
    QCOMPARE(o.lowest(), RelayQuality::Low);
    QCOMPARE(o.reason, QStringLiteral("Full live relay needs an H.264 camera."));
}

void EncoderProbeTest::_offerH265NoEncoder()
{
    const QualityOffer o = offerFor(fakeReport(false, false), VideoTapCodec::H265);
    QVERIFY(!o.relayAllowed);
    QVERIFY(o.levels.isEmpty());
    QCOMPARE(o.reason, QStringLiteral("Live relay needs an H.264 camera on this computer."));
}

void EncoderProbeTest::_unknownCodecIsH264()
{
    // Before the first unit the codec is unknown: offer as for H.264.
    const QualityOffer o = QualityOffer::compute(VideoTapCodec::H265, false, false);
    QVERIFY(o.relayAllowed);
    QCOMPARE(o.levels, QList<RelayQuality>({RelayQuality::Full}));
}

void EncoderProbeTest::_levelTexts()
{
    QCOMPARE(QualityOffer::levelText(RelayQuality::Low), QStringLiteral("Low: 640x360, about 0.8 Mb/s"));
    QCOMPARE(QualityOffer::levelText(RelayQuality::Medium), QStringLiteral("Medium: 1280x720, about 2.5 Mb/s"));
    QCOMPARE(QualityOffer::levelText(RelayQuality::Full), QStringLiteral("Full: as received from the camera"));
    const RelayQualityTarget low = relayQualityTarget(RelayQuality::Low);
    QCOMPARE(low.width, 640);
    QCOMPARE(low.height, 360);
    QCOMPARE(low.bitRate, 800000);
    QCOMPARE(low.maxFps, 15);
    const RelayQualityTarget med = relayQualityTarget(RelayQuality::Medium);
    QCOMPARE(med.width, 1280);
    QCOMPARE(med.height, 720);
    QCOMPARE(med.bitRate, 2500000);
    QCOMPARE(med.maxFps, 30);
    RelayQuality q;
    QVERIFY(relayQualityFromKey(QStringLiteral("medium"), q));
    QCOMPARE(q, RelayQuality::Medium);
    QVERIFY(!relayQualityFromKey(QStringLiteral("ultra"), q));
}

void EncoderProbeTest::_capSteps()
{
    // Medium steps down to Low once; Low, or Medium after a step, stops. Never up.
    QCOMPARE(VideoRelay::capAction(RelayQuality::Medium, false), VideoRelay::CapAction::StepToLow);
    QCOMPARE(VideoRelay::capAction(RelayQuality::Medium, true), VideoRelay::CapAction::Stop);
    QCOMPARE(VideoRelay::capAction(RelayQuality::Low, false), VideoRelay::CapAction::Stop);
    QCOMPARE(VideoRelay::capStoppedText(),
             QStringLiteral("Live relay stopped: this computer cannot encode fast enough. Choose Full or turn relay off."));
}

void EncoderProbeTest::_fitSize()
{
    int w = 0, h = 0;
    RelayEncoder::fitSize(1920, 1080, 1280, 720, w, h);
    QCOMPARE(w, 1280); QCOMPARE(h, 720);
    RelayEncoder::fitSize(1920, 1080, 640, 360, w, h);
    QCOMPARE(w, 640); QCOMPARE(h, 360);
    // Never upscaled: a small source keeps its size.
    RelayEncoder::fitSize(800, 450, 1280, 720, w, h);
    QCOMPARE(w, 800); QCOMPARE(h, 450);
    // Aspect kept, even sizes.
    RelayEncoder::fitSize(1280, 1024, 1280, 720, w, h);
    QCOMPARE(h, 720);
    QCOMPARE(w, 900);
    RelayEncoder::fitSize(721, 481, 640, 360, w, h);
    QCOMPARE(w % 2, 0);
    QCOMPARE(h % 2, 0);
    QVERIFY(w <= 640 && h <= 360);
}

void EncoderProbeTest::_realProbe()
{
    const QList<EncoderChoice> candidates = EncoderProbe::candidates();
    qInfo().noquote() << QStringLiteral("G109 encoder probe: %1 candidate(s) on this platform").arg(candidates.size());
    if (candidates.isEmpty()) {
        QSKIP("no H.264 encoder candidates on this platform (Low and Medium are Windows and Android only)");
    }
    const EncoderProbeReport r = EncoderProbe::run();
    for (const EncoderChoice &c : r.tried) {
        qInfo().noquote() << QStringLiteral("G109 encoder probe: %1 [%2] %3")
                                 .arg(c.label(), c.options,
                                      c.ok ? QStringLiteral("OPENED, %1 ms per 640x360 frame").arg(c.msPerFrame, 0, 'f', 2)
                                           : QStringLiteral("did not open: %1").arg(c.detail));
    }
    // Every candidate up to the first that opened was tried, in order.
    QVERIFY(!r.tried.isEmpty());
    if (!r.chosen.ok) {
        QCOMPARE(r.tried.size(), candidates.size());
        const QualityOffer o = QualityOffer::compute(VideoTapCodec::H264, true, false);
        QCOMPARE(o.levels, QList<RelayQuality>({RelayQuality::Full}));
        QSKIP("no H.264 encoder opened on this machine; Low and Medium would not be offered");
    }
    QVERIFY(r.chosen.msPerFrame > 0);
    QCOMPARE(r.tried.last().name, r.chosen.name);
    const QualityOffer o = QualityOffer::compute(VideoTapCodec::H264, true, true);
    QVERIFY(o.offers(RelayQuality::Low));
}
