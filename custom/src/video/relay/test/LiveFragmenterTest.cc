#include "LiveFragmenterTest.h"

#include "LiveFragmenter.h"
#include "RelayTestSupport.h"
#include "VideoCodecInfo.h"

#include <QtTest/QTest>

using namespace RelayTest;

namespace {

/// `frames` units at 30 fps with a keyframe every `gopFrames`.
QList<LiveFragment> runStream(LiveFragmenter &f, int frames, int gopFrames, int sliceBytes,
                              const QByteArray &sps, qint64 start = 0, quint64 generation = 1)
{
    QList<LiveFragment> out;
    f.setOutput([&out](const LiveFragment &x) { out << x; });
    for (int i = 0; i < frames; ++i) {
        f.push(makeUnit(i % gopFrames == 0, start + qint64(i) * 3000, sliceBytes, sps, generation));
    }
    f.flush();
    f.setOutput({});
    return out;
}

/// The trak's tkhd width and height (version 0 or 1).
void tkhdSize(const QByteArray &tkhdPayload, int &w, int &h)
{
    const int base = quint8(tkhdPayload.at(0)) == 1 ? 4 + 84 : 4 + 72;
    w = int(be32(tkhdPayload, base) >> 16);
    h = int(be32(tkhdPayload, base + 4) >> 16);
}

} // namespace

void LiveFragmenterTest::_spsFields()
{
    // The synthetic SPS these tests feed says what it should.
    const VideoCodecInfo a = VideoCodecInfo::fromSps(false, makeSps(1920, 1080, 100, 40));
    QVERIFY(a.valid);
    QCOMPARE(a.width, 1920);
    QCOMPARE(a.height, 1080);
    QCOMPARE(a.codecString, QStringLiteral("avc1.640028"));
    const VideoCodecInfo b = VideoCodecInfo::fromSps(false, makeSps(640, 360));
    QVERIFY(b.valid);
    QCOMPARE(b.width, 640);
    QCOMPARE(b.height, 360);
    QCOMPARE(b.codecString, QStringLiteral("avc1.42c01f"));
}

void LiveFragmenterTest::_initStructure()
{
    const QByteArray sps = makeSps(1280, 720, 100, 31);
    LiveFragmenter f;
    const QList<LiveFragment> frags = runStream(f, 5, 30, 500, sps);
    QCOMPARE(int(frags.size()), 1);
    const QByteArray init = frags[0].init;

    // ftyp, then moov with mvex and one video track (what the server checks).
    bool ok = false;
    const QList<Box> top = walk(init, 0, &ok);
    QVERIFY(ok);
    QCOMPARE(int(top.size()), 2);
    QCOMPARE(top[0].type, QByteArray("ftyp"));
    QCOMPARE(top[1].type, QByteArray("moov"));
    const QList<Box> moov = walk(top[1].payload, 0, &ok);
    QVERIFY(ok);
    QVERIFY(!child(moov, "mvhd").type.isEmpty());
    QVERIFY(!child(moov, "mvex").type.isEmpty());
    int traks = 0;
    for (const Box &b : moov) {
        traks += b.type == "trak" ? 1 : 0;
    }
    QCOMPARE(traks, 1);

    const QList<Box> trak = walk(child(moov, "trak").payload, 0, &ok);
    QVERIFY(ok);
    int w = 0, h = 0;
    tkhdSize(child(trak, "tkhd").payload, w, h);
    QCOMPARE(w, 1280);
    QCOMPARE(h, 720);
    const QList<Box> mdia = walk(child(trak, "mdia").payload, 0, &ok);
    QVERIFY(ok);
    QCOMPARE(child(mdia, "hdlr").payload.mid(8, 4), QByteArray("vide"));
    const QList<Box> minf = walk(child(mdia, "minf").payload, 0, &ok);
    QVERIFY(ok);
    const QList<Box> stbl = walk(child(minf, "stbl").payload, 0, &ok);
    QVERIFY(ok);
    const Box stsd = child(stbl, "stsd");
    QCOMPARE(be32(stsd.payload, 4), 1u);
    const QList<Box> entries = walk(stsd.payload, 8, &ok);
    QVERIFY(ok);
    QCOMPARE(int(entries.size()), 1);
    QCOMPARE(entries[0].type, QByteArray("avc1"));
    const QList<Box> avc1Children = walk(entries[0].payload, 78, &ok);
    const Box avcC = child(avc1Children, "avcC");
    QVERIFY(!avcC.type.isEmpty());
    QCOMPARE(quint8(avcC.payload.at(0)), quint8(1));
    QCOMPARE(avcC.payload.mid(1, 3), sps.mid(1, 3));

    QCOMPARE(frags[0].format.codec, QStringLiteral("avc1.64001f"));
    QCOMPARE(frags[0].format.width, 1280);
    QCOMPARE(frags[0].format.height, 720);
}

void LiveFragmenterTest::_fragmentStructureAndFlags()
{
    const QByteArray sps = makeSps(640, 360);
    LiveFragmenter f;
    const QList<LiveFragment> frags = runStream(f, 10, 5, 300, sps);
    // A keyframe every 5 frames: two fragments, each starting at one.
    QCOMPARE(int(frags.size()), 2);
    quint32 lastSeq = 0;
    for (const LiveFragment &fr : frags) {
        const ParsedFragment p = parseFragment(fr.bytes, fr.init);
        QVERIFY(p.ok);
        QCOMPARE(p.dataOffset, qint32(p.moofSize + 8));     // default-base-is-moof
        QVERIFY(p.sequence > lastSeq);
        lastSeq = p.sequence;
        QCOMPARE(int(p.samples.size()), 5);
        qint64 sum = 0;
        for (int i = 0; i < p.samples.size(); ++i) {
            sum += p.samples[i].size;
            QCOMPARE(p.samples[i].duration, 3000u);
            QCOMPARE(isSyncFlags(p.samples[i].flags), i == 0);
        }
        QCOMPARE(sum, p.mdatPayload);
        QVERIFY(fr.keyframeStart);
        // mdat holds 4-byte length-prefixed NAL units that add up.
        const QByteArray mdat = fr.bytes.mid(p.moofSize + 8);
        qint64 pos = 0;
        while (pos < mdat.size()) {
            pos += 4 + be32(mdat, pos);
        }
        QCOMPARE(pos, qint64(mdat.size()));
    }
}

void LiveFragmenterTest::_oneSecondCutsOnLongGop()
{
    // 8 s at 30 fps with a 4 s GOP: the camera's GOP is kept, fragments are cut
    // about every second, and the ones that begin mid-GOP say so in their flags.
    const QByteArray sps = makeSps(1280, 720);
    LiveFragmenter f;
    const QList<LiveFragment> frags = runStream(f, 240, 120, 1000, sps);
    QCOMPARE(int(frags.size()), 8);
    quint64 expectTfdt = 0;
    int keyStarts = 0;
    for (int i = 0; i < frags.size(); ++i) {
        const ParsedFragment p = parseFragment(frags[i].bytes, frags[i].init);
        QVERIFY(p.ok);
        QCOMPARE(int(p.samples.size()), 30);
        QCOMPARE(frags[i].duration90k, qint64(90000));
        QCOMPARE(p.tfdt, expectTfdt);       // back to back
        expectTfdt += 90000;
        const bool key = (i % 4) == 0;
        QCOMPARE(frags[i].keyframeStart, key);
        QCOMPARE(isSyncFlags(p.samples[0].flags), key);
        for (int s = 1; s < p.samples.size(); ++s) {
            QVERIFY(!isSyncFlags(p.samples[s].flags));
        }
        keyStarts += key ? 1 : 0;
        QCOMPARE(frags[i].format.fps, 30);
        QCOMPARE(frags[i].initId, frags[0].initId);     // one init for the whole stream
    }
    QCOMPARE(keyStarts, 2);
}

void LiveFragmenterTest::_splitUnderFourMegabytes()
{
    // 1 MB units at 30 fps: one second would be 30 MB, so fragments close
    // early, each at most 4 MB as serialized.
    const QByteArray sps = makeSps(1920, 1080, 100, 40);
    LiveFragmenter f;
    const QList<LiveFragment> frags = runStream(f, 12, 300, 1000000, sps);
    QCOMPARE(int(frags.size()), 3);
    int samples = 0;
    for (const LiveFragment &fr : frags) {
        QVERIFY2(fr.bytes.size() <= VideoRelayLimits::kMaxFragmentBytes, qPrintable(QString::number(fr.bytes.size())));
        const ParsedFragment p = parseFragment(fr.bytes, fr.init);
        QVERIFY(p.ok);
        samples += int(p.samples.size());
        QCOMPARE(int(p.samples.size()), 4);     // 4 x 1 MB fits, 5 does not
    }
    QCOMPARE(samples, 12);
    QCOMPARE(f.stats().droppedOversize, quint64(0));
}

void LiveFragmenterTest::_oversizeUnitDropped()
{
    const QByteArray sps = makeSps(640, 360);
    LiveFragmenter f;
    f.setMaxFragmentBytes(64 * 1024);
    QList<LiveFragment> out;
    f.setOutput([&out](const LiveFragment &x) { out << x; });
    f.push(makeUnit(true, 0, 1000, sps));
    f.push(makeUnit(false, 3000, 1000, sps));
    f.push(makeUnit(false, 6000, 100 * 1024, sps));   // over the cap on its own
    QVERIFY(f.waitingForKeyframe());
    f.push(makeUnit(false, 9000, 1000, sps));         // dropped: waiting for a keyframe
    f.push(makeUnit(true, 12000, 1000, sps));
    f.push(makeUnit(false, 15000, 1000, sps));
    f.flush();
    QCOMPARE(f.stats().droppedOversize, quint64(1));
    QCOMPARE(f.stats().droppedWaitingKey, quint64(1));
    QCOMPARE(int(out.size()), 2);
    QVERIFY(out[0].keyframeStart);
    QCOMPARE(int(parseFragment(out[0].bytes, out[0].init).samples.size()), 2);
    QVERIFY(out[1].keyframeStart);
    for (const LiveFragment &fr : out) {
        QVERIFY(fr.bytes.size() <= 64 * 1024);
    }
}

void LiveFragmenterTest::_firstFragmentAfterResyncIsKeyframe()
{
    const QByteArray sps = makeSps(640, 360);
    LiveFragmenter f;
    QList<LiveFragment> out;
    f.setOutput([&out](const LiveFragment &x) { out << x; });
    f.push(makeUnit(true, 0, 100, sps));
    f.push(makeUnit(false, 3000, 100, sps));
    f.resync();     // what was being built is gone
    for (int i = 2; i < 6; ++i) {
        f.push(makeUnit(false, i * 3000, 100, sps));
    }
    QVERIFY(out.isEmpty());
    f.push(makeUnit(true, 6 * 3000, 100, sps));
    f.push(makeUnit(false, 7 * 3000, 100, sps));
    f.flush();
    QCOMPARE(int(out.size()), 1);
    QVERIFY(out[0].keyframeStart);
    QVERIFY(isSyncFlags(parseFragment(out[0].bytes, out[0].init).samples[0].flags));
}

void LiveFragmenterTest::_newInitOnGenerationAndSize()
{
    LiveFragmenter f;
    QList<LiveFragment> out;
    f.setOutput([&out](const LiveFragment &x) { out << x; });
    const QByteArray small = makeSps(640, 360);
    const QByteArray big = makeSps(1280, 720);
    f.push(makeUnit(true, 0, 100, small, 1));
    f.push(makeUnit(false, 3000, 100, small, 1));
    // New generation, same parameter sets: a new init anyway.
    f.push(makeUnit(true, 6000, 100, small, 2));
    f.push(makeUnit(false, 9000, 100, small, 2));
    // New size on a keyframe.
    f.push(makeUnit(true, 12000, 100, big, 2));
    f.push(makeUnit(false, 15000, 100, big, 2));
    f.flush();
    QCOMPARE(int(out.size()), 3);
    QVERIFY(out[0].initId < out[1].initId);
    QVERIFY(out[1].initId < out[2].initId);
    QCOMPARE(out[1].format.width, 640);
    QCOMPARE(out[2].format.width, 1280);
    QCOMPARE(out[2].format.height, 720);
    QVERIFY(out[1].init != out[2].init);
    QCOMPARE(f.stats().inits, quint64(3));
}

void LiveFragmenterTest::_h265Refused()
{
    LiveFragmenter f;
    QList<LiveFragment> out;
    f.setOutput([&out](const LiveFragment &x) { out << x; });
    RelayUnit u = makeUnit(true, 0, 100, makeSps(640, 360));
    u.codec = VideoTapCodec::H265;
    f.push(u);
    f.flush();
    QVERIFY(out.isEmpty());
    QCOMPARE(f.stats().refusedCodec, quint64(1));
}

void LiveFragmenterTest::_rtpWrapKeepsDurations()
{
    const QByteArray sps = makeSps(640, 360);
    LiveFragmenter f;
    QList<LiveFragment> out;
    f.setOutput([&out](const LiveFragment &x) { out << x; });
    const quint32 start = 0xFFFFFFFFu - 4500;
    for (int i = 0; i < 6; ++i) {
        RelayUnit u = makeUnit(i == 0, qint64(quint32(start + quint32(i) * 3000u)), 100, sps);
        u.hasRtpTimestamp = true;
        f.push(u);
    }
    f.flush();
    QCOMPARE(int(out.size()), 1);
    const ParsedFragment p = parseFragment(out[0].bytes, out[0].init);
    QCOMPARE(int(p.samples.size()), 6);
    for (const TrunSample &s : p.samples) {
        QCOMPARE(s.duration, 3000u);
    }
}
