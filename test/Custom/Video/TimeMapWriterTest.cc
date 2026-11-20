#include "TimeMapWriterTest.h"
#include "VideoTestStreams.h"

#include "RecordingWriter.h"
#include "TimeMapWriter.h"

#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

using namespace VideoTest;

namespace {

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

QList<QJsonObject> ofType(const QList<QJsonObject> &ls, const QString &type)
{
    QList<QJsonObject> out;
    for (const QJsonObject &l : ls) {
        if (l.value(QStringLiteral("type")).toString() == type) {
            out.append(l);
        }
    }
    return out;
}

/// Record `count` units (1 s GOP) and return the part's queue entry.
QJsonObject record(const QString &folder, int count, std::function<VehicleSample(int)> vehicle = {},
                   std::function<void(RecordingWriter &, int)> each = {})
{
    RecordingConfig c;
    c.folder = folder;
    c.station = QStringLiteral("st");
    c.stationId = 3;
    c.camera = QStringLiteral("main");
    c.gcVersion = QStringLiteral("5.3.0-test");
    c.gcCommit = QStringLiteral("abc1234");
    c.freeBytes = [](const QString &) { return qint64(100) * 1024 * 1024 * 1024; };
    c.vehicle = std::move(vehicle);
    RecordingWriter w(c);
    QSignalSpy closed(&w, &RecordingWriter::partClosed);
    w.setSource(QStringLiteral("udp://0.0.0.0:5600"));
    w.setVehicle(1, QStringLiteral("Vehicle 1"), QString());
    w.setWanted(true);
    for (int i = 0; i < count; ++i) {
        if (each) {
            each(w, i);
        }
        w.handleItem(VideoTapItem(packet(false, quint64(i), (i % 30) == 0)));
    }
    w.setWanted(false);
    return closed.isEmpty() ? QJsonObject() : closed.at(0).at(0).toJsonObject();
}

} // namespace

void TimeMapWriterTest::_hashEqualsFile()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("t.timemap.jsonl"));
    TimeMapWriter tm;
    QVERIFY(tm.open(path));
    QJsonObject a;
    a[QStringLiteral("type")] = QStringLiteral("header");
    a[QStringLiteral("text")] = QStringLiteral("café");
    QVERIFY(tm.writeLine(a));
    QJsonObject b;
    b[QStringLiteral("type")] = QStringLiteral("fragment");
    b[QStringLiteral("seq")] = 0;
    QVERIFY(tm.writeLine(b));
    const qint64 size = tm.size();
    const QByteArray hash = tm.close();
    const QByteArray bytes = readAll(path);
    QCOMPARE(hash, sha256Hex(bytes));
    QCOMPARE(size, qint64(bytes.size()));
    QVERIFY(bytes.endsWith('\n'));
    QCOMPARE(bytes.count('\n'), 2);

    // Never overwrites an existing time map.
    TimeMapWriter again;
    QVERIFY(!again.open(path));
    QCOMPARE(readAll(path), bytes);

    // Chain: SHA-256(previous 32 bytes || fragment).
    const QByteArray c0 = TimeMapWriter::chainStart("init");
    QCOMPARE(c0.toHex(), sha256Hex("init"));
    QCOMPARE(TimeMapWriter::chainNext(c0, "frag").toHex(), sha256Hex(c0 + "frag"));
}

void TimeMapWriterTest::_headerFields()
{
    if (!Fmp4Muxer::available()) {
        QSKIP("this FFmpeg has no MP4 muxer");
    }
    QTemporaryDir dir;
    const QJsonObject e = record(dir.path(), 60);
    QVERIFY(!e.isEmpty());
    const QList<QJsonObject> ls = lines(e.value(QStringLiteral("sidecar_path")).toString());
    const QJsonObject h = ls.first();
    QCOMPARE(h.value(QStringLiteral("type")).toString(), QStringLiteral("header"));
    QCOMPARE(h.value(QStringLiteral("kind")).toString(), QStringLiteral("video-timemap"));
    QCOMPARE(h.value(QStringLiteral("v")).toInt(), 1);
    QCOMPARE(h.value(QStringLiteral("video")).toString(), e.value(QStringLiteral("name")).toString());
    QCOMPARE(h.value(QStringLiteral("station")).toString(), QStringLiteral("st"));
    QCOMPARE(h.value(QStringLiteral("station_id")).toInt(), 3);
    QCOMPARE(h.value(QStringLiteral("camera")).toString(), QStringLiteral("main"));
    QCOMPARE(h.value(QStringLiteral("codec")).toString(), QStringLiteral("avc1.640028"));
    QCOMPARE(h.value(QStringLiteral("width")).toInt(), 1280);
    QCOMPARE(h.value(QStringLiteral("height")).toInt(), 720);
    QCOMPARE(h.value(QStringLiteral("source_type")).toString(), QStringLiteral("udp"));
    QCOMPARE(h.value(QStringLiteral("gc_version")).toString(), QStringLiteral("5.3.0-test"));
    QCOMPARE(h.value(QStringLiteral("gc_commit")).toString(), QStringLiteral("abc1234"));
    QCOMPARE(h.value(QStringLiteral("part")).toInt(), 1);
    QCOMPARE(h.value(QStringLiteral("sysid")).toInt(), 1);
    QVERIFY(h.contains(QStringLiteral("flight_ref")));
    QVERIFY(h.value(QStringLiteral("started_at")).toString().endsWith(QLatin1Char('Z')));
    QCOMPARE(h.value(QStringLiteral("started_utc_ms")).toInteger(), packet(false, 0, true).receiveUtcMs);
    QVERIFY(!h.contains(QStringLiteral("fleet_server")));   // not enrolled in this config
}

void TimeMapWriterTest::_oneLinePerFragment()
{
    if (!Fmp4Muxer::available()) {
        QSKIP("this FFmpeg has no MP4 muxer");
    }
    QTemporaryDir dir;
    const QJsonObject e = record(dir.path(), 120);
    const QList<QJsonObject> ls = lines(e.value(QStringLiteral("sidecar_path")).toString());
    const QList<QJsonObject> frags = ofType(ls, QStringLiteral("fragment"));
    QCOMPARE(frags.size(), 4);
    QCOMPARE(ofType(ls, QStringLiteral("end")).first().value(QStringLiteral("fragments")).toInt(), 4);
    qint64 expectOffset = frags.first().value(QStringLiteral("offset")).toInteger();
    for (int i = 0; i < frags.size(); ++i) {
        const QJsonObject &f = frags.at(i);
        QCOMPARE(f.value(QStringLiteral("seq")).toInt(), i);
        QCOMPARE(f.value(QStringLiteral("offset")).toInteger(), expectOffset);
        expectOffset += f.value(QStringLiteral("length")).toInteger();
        QCOMPARE(f.value(QStringLiteral("recv_utc_ms")).toInteger(), packet(false, quint64(i * 30), true).receiveUtcMs);
        QCOMPARE(f.value(QStringLiteral("recv_steady_ns")).toInteger(), packet(false, quint64(i * 30), true).receiveSteadyNs);
        // RTP unwrapped across the 2^32 wrap in this stream.
        QCOMPARE(f.value(QStringLiteral("rtp")).toInteger(), qint64(0xfffff000LL) + qint64(i) * 90000);
        QCOMPARE(f.value(QStringLiteral("pts")).toInteger(), qint64(i) * 90000);
    }
    QCOMPARE(expectOffset, e.value(QStringLiteral("size")).toInteger());
}

void TimeMapWriterTest::_chainRecomputes()
{
    if (!Fmp4Muxer::available()) {
        QSKIP("this FFmpeg has no MP4 muxer");
    }
    QTemporaryDir dir;
    const QJsonObject e = record(dir.path(), 100);
    const QByteArray video = readAll(e.value(QStringLiteral("path")).toString());
    const QList<QJsonObject> ls = lines(e.value(QStringLiteral("sidecar_path")).toString());
    const QList<QJsonObject> frags = ofType(ls, QStringLiteral("fragment"));
    QVERIFY(!frags.isEmpty());
    const QByteArray init = video.left(frags.first().value(QStringLiteral("offset")).toInteger());
    QCOMPARE(ls.first().value(QStringLiteral("init_sha256")).toString().toLatin1(), sha256Hex(init));
    QByteArray chain = TimeMapWriter::chainStart(init);
    for (const QJsonObject &f : frags) {
        const QByteArray bytes = video.mid(f.value(QStringLiteral("offset")).toInteger(),
                                           f.value(QStringLiteral("length")).toInteger());
        chain = TimeMapWriter::chainNext(chain, bytes);
        QCOMPARE(f.value(QStringLiteral("chain")).toString().toLatin1(), chain.toHex());
    }
    QCOMPARE(ofType(ls, QStringLiteral("end")).first().value(QStringLiteral("chain")).toString().toLatin1(), chain.toHex());
}

void TimeMapWriterTest::_missingVehicleValuesLeftOut()
{
    if (!Fmp4Muxer::available()) {
        QSKIP("this FFmpeg has no MP4 muxer");
    }
    // Nothing known: no vehicle fields at all.
    {
        QTemporaryDir dir;
        const QJsonObject e = record(dir.path(), 60, [](int) { return VehicleSample(); });
        for (const QJsonObject &f : ofType(lines(e.value(QStringLiteral("sidecar_path")).toString()), QStringLiteral("fragment"))) {
            for (const char *k : {"lat", "lon", "alt_m", "age_ms", "boot_ms", "gps_utc_us", "time_age_ms"}) {
                QVERIFY2(!f.contains(QLatin1String(k)), k);
            }
        }
    }
    // Boot time and a position without altitude: those only, with their age.
    {
        QTemporaryDir dir;
        const qint64 base = packet(false, 0, true).receiveSteadyNs;
        const QJsonObject e = record(dir.path(), 60, [base](int sysid) {
            VehicleSample s;
            if (sysid == 1) {
                s.hasBoot = true;
                s.bootMs = 123456;
                s.timeSteadyNs = base - 200000000LL;
                s.hasPosition = true;
                s.lat = 38.5;
                s.lon = -90.25;
                s.posSteadyNs = base - 500000000LL;
            }
            return s;
        });
        const QList<QJsonObject> frags = ofType(lines(e.value(QStringLiteral("sidecar_path")).toString()), QStringLiteral("fragment"));
        QVERIFY(!frags.isEmpty());
        const QJsonObject &f = frags.first();
        QCOMPARE(f.value(QStringLiteral("boot_ms")).toInteger(), qint64(123456));
        QCOMPARE(f.value(QStringLiteral("time_age_ms")).toInteger(), qint64(200));
        QCOMPARE(f.value(QStringLiteral("lat")).toDouble(), 38.5);
        QCOMPARE(f.value(QStringLiteral("lon")).toDouble(), -90.25);
        QCOMPARE(f.value(QStringLiteral("age_ms")).toInteger(), qint64(500));
        QVERIFY(!f.contains(QStringLiteral("alt_m")));
        QVERIFY(!f.contains(QStringLiteral("gps_utc_us")));
    }
}

void TimeMapWriterTest::_armLinesAndFlightRef()
{
    if (!Fmp4Muxer::available()) {
        QSKIP("this FFmpeg has no MP4 muxer");
    }
    QTemporaryDir dir;
    const QJsonObject e = record(dir.path(), 90, {}, [](RecordingWriter &w, int i) {
        if (i == 10) {
            w.setVehicle(1, QStringLiteral("Vehicle 1"), QStringLiteral("ref-abc"));
            w.mark(QStringLiteral("arm"), 1, QStringLiteral("ref-abc"));
        } else if (i == 70) {
            w.mark(QStringLiteral("disarm"), 1, QStringLiteral("ref-abc"));
        }
    });
    const QList<QJsonObject> ls = lines(e.value(QStringLiteral("sidecar_path")).toString());
    QCOMPARE(ofType(ls, QStringLiteral("arm")).size(), 1);
    QCOMPARE(ofType(ls, QStringLiteral("disarm")).size(), 1);
    QCOMPARE(ofType(ls, QStringLiteral("arm")).first().value(QStringLiteral("flight_ref")).toString(), QStringLiteral("ref-abc"));
    QCOMPARE(e.value(QStringLiteral("flight_ref")).toString(), QStringLiteral("ref-abc"));
}
