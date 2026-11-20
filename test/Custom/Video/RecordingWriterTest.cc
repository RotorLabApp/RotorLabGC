#include "RecordingWriterTest.h"
#include "VideoTestStreams.h"

#include "RecordingWriter.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

using namespace VideoTest;

namespace {

RecordingConfig config(const QString &folder)
{
    RecordingConfig c;
    c.folder = folder;
    c.station = QStringLiteral("Test Station");
    c.stationId = 7;
    c.baseUrl = QStringLiteral("http://127.0.0.1:8000");
    c.camera = QStringLiteral("main");
    c.gcVersion = QStringLiteral("test");
    c.gcCommit = QStringLiteral("abc1234");
    c.freeBytes = [](const QString &) { return qint64(100) * 1024 * 1024 * 1024; };
    return c;
}

QByteArray readAll(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QList<QJsonObject> lines(const QString &path)
{
    QList<QJsonObject> out;
    for (const QByteArray &l : readAll(path).split('\n')) {
        const QJsonDocument d = QJsonDocument::fromJson(l);
        if (d.isObject()) {
            out.append(d.object());
        }
    }
    return out;
}

int countType(const QList<QJsonObject> &ls, const QString &type)
{
    int n = 0;
    for (const QJsonObject &l : ls) {
        n += (l.value(QStringLiteral("type")).toString() == type) ? 1 : 0;
    }
    return n;
}

void feed(RecordingWriter &w, int from, int to, int gop, quint64 generation = 1, int size = 2000)
{
    for (int i = from; i < to; ++i) {
        w.handleItem(VideoTapItem(packet(false, quint64(i), (i % gop) == 0, size, generation)));
    }
}

} // namespace

void RecordingWriterTest::_hashEqualsFile()
{
    if (!Fmp4Muxer::available()) {
        QSKIP("this FFmpeg has no MP4 muxer");
    }
    QTemporaryDir dir;
    RecordingWriter w(config(dir.path()));
    QSignalSpy closed(&w, &RecordingWriter::partClosed);
    w.setWanted(true);
    feed(w, 0, 95, 30);
    QVERIFY(w.partOpen());
    w.setWanted(false);
    QCOMPARE(closed.count(), 1);
    const QJsonObject e = closed.at(0).at(0).toJsonObject();
    const QByteArray video = readAll(e.value(QStringLiteral("path")).toString());
    QVERIFY(!video.isEmpty());
    QCOMPARE(e.value(QStringLiteral("size")).toInteger(), qint64(video.size()));
    QCOMPARE(e.value(QStringLiteral("sha256")).toString().toLatin1(), sha256Hex(video));
    QVERIFY(video.mid(4, 4) == "ftyp");
    const QByteArray side = readAll(e.value(QStringLiteral("sidecar_path")).toString());
    QCOMPARE(e.value(QStringLiteral("sidecar_sha256")).toString().toLatin1(), sha256Hex(side));
    QCOMPARE(e.value(QStringLiteral("sidecar_size")).toInteger(), qint64(side.size()));
    QCOMPARE(e.value(QStringLiteral("codec")).toString(), QStringLiteral("avc1.640028"));
    QCOMPARE(e.value(QStringLiteral("width")).toInt(), 1280);
    QCOMPARE(e.value(QStringLiteral("height")).toInt(), 720);
    QCOMPARE(e.value(QStringLiteral("part")).toInt(), 1);
    QCOMPARE(e.value(QStringLiteral("base_url")).toString(), QStringLiteral("http://127.0.0.1:8000"));
    QCOMPARE(e.value(QStringLiteral("station_id")).toInt(), 7);
    QVERIFY(qAbs(e.value(QStringLiteral("fps")).toDouble() - 30.0) < 0.5);
    QVERIFY(e.value(QStringLiteral("name")).toString().startsWith(QStringLiteral("Test_Station-main-")));
    QVERIFY(e.value(QStringLiteral("name")).toString().endsWith(QStringLiteral("-p01.mp4")));
    QVERIFY(e.value(QStringLiteral("sidecar_name")).toString().endsWith(QStringLiteral("-p01.timemap.jsonl")));
    // The end line carries the same hash.
    const QList<QJsonObject> ls = lines(e.value(QStringLiteral("sidecar_path")).toString());
    QCOMPARE(ls.last().value(QStringLiteral("type")).toString(), QStringLiteral("end"));
    QCOMPARE(ls.last().value(QStringLiteral("sha256")).toString(), e.value(QStringLiteral("sha256")).toString());
    QVERIFY(ls.last().value(QStringLiteral("fps_measured")).toBool());
}

void RecordingWriterTest::_startsOnKeyframe()
{
    if (!Fmp4Muxer::available()) {
        QSKIP("this FFmpeg has no MP4 muxer");
    }
    QTemporaryDir dir;
    RecordingWriter w(config(dir.path()));
    QSignalSpy closed(&w, &RecordingWriter::partClosed);
    w.setWanted(true);
    feed(w, 1, 30, 30);                 // units 1..29: no keyframe yet
    QVERIFY(!w.partOpen());
    QVERIFY(QDir(dir.path()).entryList(QDir::Files).isEmpty());
    feed(w, 30, 75, 30);
    w.setWanted(false);
    QCOMPARE(closed.count(), 1);
    const QList<QJsonObject> ls = lines(closed.at(0).at(0).toJsonObject().value(QStringLiteral("sidecar_path")).toString());
    int samples = 0;
    bool first = true;
    for (const QJsonObject &l : ls) {
        if (l.value(QStringLiteral("type")).toString() == QLatin1String("fragment")) {
            if (first) {
                QVERIFY(l.value(QStringLiteral("keyframe")).toBool());
                QCOMPARE(l.value(QStringLiteral("dts")).toInteger(), qint64(0));
                first = false;
            }
            samples += l.value(QStringLiteral("samples")).toInt();
        }
    }
    QCOMPARE(samples, 45);
}

void RecordingWriterTest::_partSplitAtSizeLimit()
{
    if (!Fmp4Muxer::available()) {
        QSKIP("this FFmpeg has no MP4 muxer");
    }
    QTemporaryDir dir;
    RecordingConfig c = config(dir.path());
    c.partLimitBytes = 50000;           // under 1 s of this test stream
    RecordingWriter w(c);
    QSignalSpy closed(&w, &RecordingWriter::partClosed);
    w.setWanted(true);
    feed(w, 0, 150, 30);
    w.setWanted(false);
    QVERIFY(closed.count() >= 3);
    for (int i = 0; i < closed.count(); ++i) {
        const QJsonObject e = closed.at(i).at(0).toJsonObject();
        QCOMPARE(e.value(QStringLiteral("part")).toInt(), i + 1);
        QVERIFY(e.value(QStringLiteral("name")).toString().endsWith(QStringLiteral("-p%1.mp4").arg(i + 1, 2, 10, QLatin1Char('0'))));
        const QByteArray video = readAll(e.value(QStringLiteral("path")).toString());
        QVERIFY(video.mid(4, 4) == "ftyp");             // each part plays on its own
        QCOMPARE(e.value(QStringLiteral("sha256")).toString().toLatin1(), sha256Hex(video));
        const QList<QJsonObject> ls = lines(e.value(QStringLiteral("sidecar_path")).toString());
        QCOMPARE(ls.first().value(QStringLiteral("part")).toInt(), i + 1);
        for (const QJsonObject &l : ls) {
            if (l.value(QStringLiteral("type")).toString() == QLatin1String("fragment")) {
                QVERIFY(l.value(QStringLiteral("keyframe")).toBool());     // a part starts on a keyframe
                break;
            }
        }
        if (i + 1 < closed.count()) {
            // Split at the first keyframe after the limit: never far over it.
            QVERIFY(e.value(QStringLiteral("size")).toInteger() >= c.partLimitBytes);
            QVERIFY(e.value(QStringLiteral("size")).toInteger() < c.partLimitBytes + 80000);
        }
    }
}

void RecordingWriterTest::_newGenerationNewPart()
{
    if (!Fmp4Muxer::available()) {
        QSKIP("this FFmpeg has no MP4 muxer");
    }
    QTemporaryDir dir;
    RecordingWriter w(config(dir.path()));
    QSignalSpy closed(&w, &RecordingWriter::partClosed);
    w.setWanted(true);
    feed(w, 0, 40, 30, 1);
    feed(w, 40, 60, 30, 2);             // new generation: units until its keyframe are dropped
    feed(w, 60, 90, 30, 2);
    w.setWanted(false);
    QCOMPARE(closed.count(), 2);
    QCOMPARE(closed.at(1).at(0).toJsonObject().value(QStringLiteral("part")).toInt(), 2);
}

void RecordingWriterTest::_discontinuityWritesGap()
{
    if (!Fmp4Muxer::available()) {
        QSKIP("this FFmpeg has no MP4 muxer");
    }
    QTemporaryDir dir;
    RecordingWriter w(config(dir.path()));
    QSignalSpy closed(&w, &RecordingWriter::partClosed);
    w.setWanted(true);
    feed(w, 0, 40, 30);
    VideoTapPacket p = packet(false, 50, true);   // units 40..49 lost; a keyframe resumes
    p.discontinuity = true;
    p.droppedBefore = 4;
    w.handleItem(VideoTapItem(p));
    feed(w, 51, 70, 30);
    w.setWanted(false);
    QCOMPARE(closed.count(), 1);                  // a gap does not end the part
    const QList<QJsonObject> ls = lines(closed.at(0).at(0).toJsonObject().value(QStringLiteral("sidecar_path")).toString());
    QCOMPARE(countType(ls, QStringLiteral("gap")), 1);
    for (const QJsonObject &l : ls) {
        if (l.value(QStringLiteral("type")).toString() == QLatin1String("gap")) {
            QCOMPARE(l.value(QStringLiteral("before_recv_utc_ms")).toInteger(), packet(false, 39, false).receiveUtcMs);
            QCOMPARE(l.value(QStringLiteral("after_recv_utc_ms")).toInteger(), p.receiveUtcMs);
            QCOMPARE(l.value(QStringLiteral("dropped_by_gc")).toInt(), 4);
        }
    }
}

void RecordingWriterTest::_diskLowStops()
{
    if (!Fmp4Muxer::available()) {
        QSKIP("this FFmpeg has no MP4 muxer");
    }
    QTemporaryDir dir;
    RecordingConfig c = config(dir.path());
    auto free = std::make_shared<qint64>(qint64(3) * 1024 * 1024 * 1024);
    c.freeBytes = [free](const QString &) { return *free; };
    c.diskCheckMs = 1000;
    RecordingWriter w(c);
    QSignalSpy closed(&w, &RecordingWriter::partClosed);
    QSignalSpy stopped(&w, &RecordingWriter::stopped);
    w.setWanted(true);
    feed(w, 0, 60, 30);
    *free = qint64(1) * 1024 * 1024 * 1024;       // under 2 GB
    feed(w, 60, 120, 30);
    QCOMPARE(stopped.count(), 1);
    QCOMPARE(stopped.at(0).at(0).toInt(), int(RecordingWriter::StopDiskLow));
    QCOMPARE(stopped.at(0).at(1).toString(), RecordingWriter::stopText(RecordingWriter::StopDiskLow));
    QCOMPARE(closed.count(), 1);
    QVERIFY(!w.partOpen());
    const QJsonObject e = closed.at(0).at(0).toJsonObject();
    QVERIFY(QFile::exists(e.value(QStringLiteral("path")).toString()));        // nothing deleted
    QCOMPARE(e.value(QStringLiteral("sha256")).toString().toLatin1(), sha256Hex(readAll(e.value(QStringLiteral("path")).toString())));
    feed(w, 120, 180, 30);
    QVERIFY(!w.partOpen());                     // stays stopped until a new start
    QCOMPARE(QDir(dir.path()).entryList({QStringLiteral("*.mp4")}, QDir::Files).size(), 1);
}

void RecordingWriterTest::_sourceStopClosesPart()
{
    if (!Fmp4Muxer::available()) {
        QSKIP("this FFmpeg has no MP4 muxer");
    }
    QTemporaryDir dir;
    RecordingWriter w(config(dir.path()));
    QSignalSpy closed(&w, &RecordingWriter::partClosed);
    QSignalSpy stopped(&w, &RecordingWriter::stopped);
    w.setWanted(true);
    feed(w, 0, 40, 30);
    VideoTapEvent ev;
    ev.type = VideoTapEvent::Type::StreamStopped;
    ev.stopReason = VideoTapEvent::StopReason::Timeout;
    ev.generation = 1;
    w.handleItem(VideoTapItem(ev));
    QCOMPARE(closed.count(), 1);
    QCOMPARE(stopped.count(), 1);
    QCOMPARE(stopped.at(0).at(0).toInt(), int(RecordingWriter::StopSourceStopped));
    // The source comes back: a new recording at its first keyframe.
    feed(w, 0, 40, 30, 2);
    QVERIFY(w.partOpen());
    w.setWanted(false);
    QCOMPARE(closed.count(), 2);
}
