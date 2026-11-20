#include "Fmp4MuxerTest.h"
#include "VideoTestStreams.h"

#include "Fmp4Muxer.h"
#include "VideoCodecInfo.h"

#include <QtTest/QTest>

using namespace VideoTest;

namespace {

struct Output
{
    QByteArray init;
    QList<QByteArray> fragments;
    QList<Fmp4FragmentInfo> infos;
    QByteArray all() const
    {
        QByteArray b = init;
        for (const QByteArray &f : fragments) {
            b += f;
        }
        return b;
    }
};

void capture(Fmp4Muxer &m, Output &out)
{
    QObject::connect(&m, &Fmp4Muxer::initSegment, &m, [&out](const QByteArray &b) { out.init = b; });
    QObject::connect(&m, &Fmp4Muxer::fragment, &m, [&out](const QByteArray &b, const Fmp4FragmentInfo &i) {
        out.fragments.append(b);
        out.infos.append(i);
    });
}

Fmp4Muxer::Unit unitAt(bool h265, int i, int gop)
{
    const bool key = (i % gop) == 0;
    Fmp4Muxer::Unit u;
    u.data = accessUnit(h265, key, 1500 + (i % 7) * 100);
    u.keyFrame = key;
    u.pts90k = u.dts90k = 1000000 + qint64(i) * 3000;
    u.sourceTime90k = u.dts90k;
    u.receiveSteadyNs = 5000000000LL + qint64(i) * 33333333LL;
    u.receiveUtcMs = 1795000000000LL + qint64(i) * 33;
    u.index = quint64(i + 1);
    return u;
}

/// 30 fps, `count` units, keyframe every `gop` units; then finish().
bool runStream(Fmp4Muxer &m, bool h265, int count, int gop)
{
    Fmp4Muxer::Config c;
    c.codec = h265 ? Fmp4Muxer::Codec::H265 : Fmp4Muxer::Codec::H264;
    c.parameterSets = parameterSets(h265, 1280, 720);
    if (!m.open(c)) {
        return false;
    }
    for (int i = 0; i < count; ++i) {
        if (!m.write(unitAt(h265, i, gop))) {
            return false;
        }
    }
    return m.finish();
}

/// Walk the whole output; check ftyp, moov with the sample entry, then
/// moof+mdat pairs with continuous tfdt. Returns the per-fragment facts.
QList<FragmentFacts> checkStructure(const QByteArray &all, bool h265)
{
    bool ok = true;
    const QList<Box> boxes = walk(all, 0, all.size(), &ok);
    if (!ok || boxes.size() < 4 || boxes.at(0).type != "ftyp" || boxes.at(1).type != "moov") {
        qWarning() << "bad top level" << ok << boxes.size();
        return {};
    }
    const QByteArray moov = all.mid(boxes.at(1).offset, boxes.at(1).size);
    if (!moov.contains(h265 ? "hvc1" : "avc1") || !moov.contains(h265 ? "hvcC" : "avcC") || !moov.contains("mvex")) {
        qWarning() << "sample entry missing";
        return {};
    }
    const quint32 trex = trexFlags(all, boxes);
    QList<FragmentFacts> out;
    quint64 expected = 0;
    for (int i = 2; i < boxes.size(); i += 2) {
        if (boxes.at(i).type != "moof" || i + 1 >= boxes.size() || boxes.at(i + 1).type != "mdat") {
            qWarning() << "expected moof+mdat at" << i;
            return {};
        }
        const FragmentFacts f = facts(all, boxes.at(i), trex);
        if (!f.ok || f.tfdt != expected) {
            qWarning() << "tfdt" << f.tfdt << "expected" << expected;
            return {};
        }
        expected += f.duration;
        out.append(f);
    }
    return out;
}

} // namespace

void Fmp4MuxerTest::_codecInfoH264()
{
    const VideoCodecInfo a = VideoCodecInfo::fromAnnexB(false, parameterSets(false, 1920, 1080));
    QVERIFY(a.valid);
    QCOMPARE(a.width, 1920);
    QCOMPARE(a.height, 1080);
    QCOMPARE(a.codecString, QStringLiteral("avc1.640028"));
    const VideoCodecInfo b = VideoCodecInfo::fromAnnexB(false, parameterSets(false, 1280, 720));
    QCOMPARE(b.width, 1280);
    QCOMPARE(b.height, 720);
}

void Fmp4MuxerTest::_codecInfoH265()
{
    const VideoCodecInfo a = VideoCodecInfo::fromAnnexB(true, parameterSets(true, 1280, 720));
    QVERIFY(a.valid);
    QCOMPARE(a.width, 1280);
    QCOMPARE(a.height, 720);
    QCOMPARE(a.codecString, QStringLiteral("hvc1.1.6.L93.90"));
}

void Fmp4MuxerTest::_codecInfoRejectsGarbage()
{
    QVERIFY(!VideoCodecInfo::fromAnnexB(false, QByteArray()).valid);
    QVERIFY(!VideoCodecInfo::fromAnnexB(false, sc() + QByteArray("\x67\x4d\x00\x1f", 4)).valid);
    QVERIFY(!VideoCodecInfo::fromAnnexB(true, sc() + QByteArray(64, char(0xff))).valid);
    QByteArray huge = sc() + QByteArray(1, char(0x67)) + QByteArray(4096, char(0x00));
    QVERIFY(!VideoCodecInfo::fromAnnexB(false, huge).valid);
}

void Fmp4MuxerTest::_h264BoxesParse()
{
    if (!Fmp4Muxer::available()) {
        QSKIP("this FFmpeg has no MP4 muxer");
    }
    Fmp4Muxer m;
    Output out;
    capture(m, out);
    QVERIFY2(runStream(m, false, 150, 30), qPrintable(m.errorString()));
    QVERIFY(!out.init.isEmpty());
    const QList<FragmentFacts> f = checkStructure(out.all(), false);
    QCOMPARE(f.size(), 5);                  // a 1 s GOP: one fragment per GOP
    int samples = 0;
    for (const FragmentFacts &x : f) {
        QVERIFY(x.firstSync);
        samples += x.samples;
    }
    QCOMPARE(samples, 150);
    QCOMPARE(m.codecInfo().codecString, QStringLiteral("avc1.640028"));
}

void Fmp4MuxerTest::_h265BoxesParse()
{
    if (!Fmp4Muxer::available()) {
        QSKIP("this FFmpeg has no MP4 muxer");
    }
    Fmp4Muxer m;
    Output out;
    capture(m, out);
    QVERIFY2(runStream(m, true, 90, 30), qPrintable(m.errorString()));
    const QList<FragmentFacts> f = checkStructure(out.all(), true);
    QCOMPARE(f.size(), 3);
    for (const FragmentFacts &x : f) {
        QVERIFY(x.firstSync);
        QCOMPARE(x.samples, 30);
        QCOMPARE(x.duration, quint64(90000));
    }
}

void Fmp4MuxerTest::_appendOnlyOffsets()
{
    if (!Fmp4Muxer::available()) {
        QSKIP("this FFmpeg has no MP4 muxer");
    }
    Fmp4Muxer m;
    Output out;
    capture(m, out);
    QVERIFY(runStream(m, false, 100, 45));
    // Every byte comes out once, in order: each fragment starts where the
    // previous output ended, and nothing is emitted twice or rewritten.
    qint64 at = out.init.size();
    for (int i = 0; i < out.infos.size(); ++i) {
        QCOMPARE(out.infos.at(i).offset, at);
        QCOMPARE(out.infos.at(i).length, qint64(out.fragments.at(i).size()));
        QCOMPARE(out.infos.at(i).seq, quint64(i));
        at += out.fragments.at(i).size();
    }
    QCOMPARE(at, qint64(out.all().size()));
    QCOMPARE(m.bytesOut(), at);

    // Same input, same bytes (bit-exact): the hash at capture is reproducible.
    Fmp4Muxer m2;
    Output out2;
    capture(m2, out2);
    QVERIFY(runStream(m2, false, 100, 45));
    QCOMPARE(sha256Hex(out2.all()), sha256Hex(out.all()));
}

void Fmp4MuxerTest::_midGopCutIsNonSync()
{
    if (!Fmp4Muxer::available()) {
        QSKIP("this FFmpeg has no MP4 muxer");
    }
    Fmp4Muxer m;
    Output out;
    capture(m, out);
    QVERIFY(runStream(m, false, 120, 60));  // 2 s GOP, 1 s fragments
    const QList<FragmentFacts> f = checkStructure(out.all(), false);
    QCOMPARE(f.size(), 4);
    QVERIFY(f.at(0).firstSync);
    QVERIFY(!f.at(1).firstSync);            // starts mid-GOP
    QVERIFY(f.at(2).firstSync);
    QVERIFY(!f.at(3).firstSync);
    QVERIFY(out.infos.at(0).startsWithKeyframe);
    QVERIFY(!out.infos.at(1).startsWithKeyframe);
    QCOMPARE(out.infos.at(1).dts90k, qint64(90000));
    QCOMPARE(out.infos.at(1).sourceTime90k, qint64(1000000 + 90000));
}

void Fmp4MuxerTest::_parameterSetChangeNeedsNewInit()
{
    if (!Fmp4Muxer::available()) {
        QSKIP("this FFmpeg has no MP4 muxer");
    }
    Fmp4Muxer m;
    QVERIFY(m.needsNewInit(Fmp4Muxer::Codec::H264, parameterSets(false, 1280, 720)));     // not open
    Fmp4Muxer::Config c;
    c.parameterSets = parameterSets(false, 1280, 720);
    Output out;
    capture(m, out);
    QVERIFY(m.open(c));
    const QByteArray firstInit = out.init;
    QVERIFY(!m.needsNewInit(Fmp4Muxer::Codec::H264, parameterSets(false, 1280, 720)));
    QVERIFY(!m.needsNewInit(Fmp4Muxer::Codec::H264, QByteArray()));    // none in this unit
    QVERIFY(m.needsNewInit(Fmp4Muxer::Codec::H264, parameterSets(false, 1920, 1080)));
    QVERIFY(m.needsNewInit(Fmp4Muxer::Codec::H265, parameterSets(true, 1280, 720)));
    m.abort();

    c.parameterSets = parameterSets(false, 1920, 1080);
    QVERIFY(m.open(c));
    QVERIFY(out.init != firstInit);         // a new init segment for the new size
    QCOMPARE(m.codecInfo().width, 1920);
}

void Fmp4MuxerTest::_startsOnKeyframe()
{
    if (!Fmp4Muxer::available()) {
        QSKIP("this FFmpeg has no MP4 muxer");
    }
    Fmp4Muxer m;
    Output out;
    capture(m, out);
    Fmp4Muxer::Config c;
    c.parameterSets = parameterSets(false, 1280, 720);
    QVERIFY(m.open(c));
    QVERIFY(!m.write(unitAt(false, 1, 30)));    // not a keyframe: refused
    QVERIFY(m.errorString().isEmpty());
    for (int i = 30; i < 60; ++i) {
        QVERIFY(m.write(unitAt(false, i, 30)));
    }
    QVERIFY(m.finish());
    const QList<FragmentFacts> f = checkStructure(out.all(), false);
    QCOMPARE(f.size(), 1);
    QVERIFY(f.at(0).firstSync);
    QCOMPARE(f.at(0).samples, 30);
}

void Fmp4MuxerTest::_byteLimitSplitsFragments()
{
    if (!Fmp4Muxer::available()) {
        QSKIP("this FFmpeg has no MP4 muxer");
    }
    Fmp4Muxer m;
    Output out;
    capture(m, out);
    Fmp4Muxer::Config c;
    c.parameterSets = parameterSets(false, 1280, 720);
    c.maxFragmentBytes = 10000;             // about 5 units
    QVERIFY(m.open(c));
    for (int i = 0; i < 30; ++i) {
        QVERIFY(m.write(unitAt(false, i, 30)));
    }
    QVERIFY(m.finish());
    QVERIFY(out.fragments.size() >= 5);
    for (const Fmp4FragmentInfo &i : std::as_const(out.infos)) {
        QVERIFY(i.samples <= 7);
    }
    QVERIFY(!checkStructure(out.all(), false).isEmpty());
}
