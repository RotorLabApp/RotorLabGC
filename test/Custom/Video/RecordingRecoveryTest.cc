#include "RecordingRecoveryTest.h"

#include "RecordingRecovery.h"
#include "TimeMapWriter.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimeZone>
#include <QtTest/QTest>

namespace {

constexpr qint64 kStartMs = 1795000000000LL;

QByteArray hex(const QByteArray &b)
{
    return QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex();
}

QByteArray readAll(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void writeFile(const QString &path, const QByteArray &bytes)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(f.write(bytes), qint64(bytes.size()));
}

QByteArray line(const QJsonObject &o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

/// A part as written up to a crash: an "init" of 100 bytes, then `fragments`
/// fragments of 1000 bytes each, the sidecar lines for each. Returns the full
/// video and sidecar (before any cut) and fills fragment receive times.
struct Fixture
{
    QByteArray video;
    QByteArray sidecar;
    QList<qint64> recvMs;
};

Fixture make(const QString &name, int fragments, bool withEnd)
{
    Fixture f;
    QJsonObject h;
    h[QStringLiteral("type")] = QStringLiteral("header");
    h[QStringLiteral("kind")] = QStringLiteral("video-timemap");
    h[QStringLiteral("v")] = 1;
    h[QStringLiteral("video")] = name;
    h[QStringLiteral("station")] = QStringLiteral("st");
    h[QStringLiteral("station_id")] = 7;
    h[QStringLiteral("fleet_server")] = QStringLiteral("http://127.0.0.1:8000");
    h[QStringLiteral("camera")] = QStringLiteral("main");
    h[QStringLiteral("codec")] = QStringLiteral("avc1.640028");
    h[QStringLiteral("width")] = 1280;
    h[QStringLiteral("height")] = 720;
    h[QStringLiteral("part")] = 1;
    h[QStringLiteral("flight_ref")] = QStringLiteral("ref-1");
    h[QStringLiteral("sysid")] = 1;
    h[QStringLiteral("started_at")] = QStringLiteral("2026-11-18T15:02:07.000Z");
    h[QStringLiteral("started_utc_ms")] = kStartMs;
    f.video = QByteArray(100, 'I');
    f.sidecar = line(h);
    QByteArray chain = TimeMapWriter::chainStart(f.video);
    for (int i = 0; i < fragments; ++i) {
        const QByteArray frag(1000, char('a' + i));
        QJsonObject l;
        l[QStringLiteral("type")] = QStringLiteral("fragment");
        l[QStringLiteral("seq")] = i;
        l[QStringLiteral("offset")] = qint64(f.video.size());
        l[QStringLiteral("length")] = qint64(frag.size());
        l[QStringLiteral("samples")] = 30;
        l[QStringLiteral("duration")] = 90000;
        chain = TimeMapWriter::chainNext(chain, frag);
        l[QStringLiteral("chain")] = QString::fromLatin1(chain.toHex());
        l[QStringLiteral("recv_utc_ms")] = kStartMs + i * 1000;
        f.recvMs.append(kStartMs + i * 1000);
        f.video += frag;
        f.sidecar += line(l);
    }
    if (withEnd) {
        QJsonObject end;
        end[QStringLiteral("type")] = QStringLiteral("end");
        end[QStringLiteral("ended_utc_ms")] = kStartMs + fragments * 1000;
        end[QStringLiteral("size")] = qint64(f.video.size());
        end[QStringLiteral("sha256")] = QString::fromLatin1(hex(f.video));
        f.sidecar += line(end);
    }
    return f;
}

QString iso(qint64 ms)
{
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(Qt::ISODate);
}

} // namespace

void RecordingRecoveryTest::_cutMidFragmentHashedAsFound()
{
    QTemporaryDir dir;
    const QString name = QStringLiteral("st-main-20261118T150207Z-p01.mp4");
    const Fixture f = make(name, 5, false);
    // The crash: the 5th fragment half written, its sidecar line cut mid-way.
    const QByteArray video = f.video.left(100 + 4 * 1000 + 500);
    const qsizetype lastLine = f.sidecar.lastIndexOf('\n', f.sidecar.size() - 2);
    const QByteArray sidecar = f.sidecar.left(lastLine + 1 + 20);
    const QString videoPath = QDir(dir.path()).filePath(name);
    const QString sidePath = QDir(dir.path()).filePath(QStringLiteral("st-main-20261118T150207Z-p01.timemap.jsonl"));
    writeFile(videoPath, video);
    writeFile(sidePath, sidecar);

    const QList<QJsonObject> found = RecordingRecovery::scan(dir.path(), {});
    QCOMPARE(found.size(), 1);
    const QJsonObject e = found.first();
    QVERIFY(e.value(QStringLiteral("recovered")).toBool());
    QCOMPARE(e.value(QStringLiteral("name")).toString(), name);
    QCOMPARE(e.value(QStringLiteral("size")).toInteger(), qint64(video.size()));
    QCOMPARE(e.value(QStringLiteral("sha256")).toString().toLatin1(), hex(video));
    // Never trimmed or repaired: the bytes on disk are the bytes found.
    QCOMPARE(readAll(videoPath), video);
    // ended_at is the receive time of the last complete fragment (the 4th).
    QCOMPARE(e.value(QStringLiteral("ended_at")).toString(), iso(f.recvMs.at(3)));
    QCOMPARE(e.value(QStringLiteral("started_at")).toString(), iso(kStartMs));
    QCOMPARE(e.value(QStringLiteral("flight_ref")).toString(), QStringLiteral("ref-1"));
    QCOMPARE(e.value(QStringLiteral("base_url")).toString(), QStringLiteral("http://127.0.0.1:8000"));
    QCOMPARE(e.value(QStringLiteral("station_id")).toInt(), 7);
    QVERIFY(e.value(QStringLiteral("state")).toString().isEmpty());      // not parked

    // The sidecar only grew: what the crash left, then a newline, then one recovered line.
    const QByteArray after = readAll(sidePath);
    QVERIFY(after.startsWith(sidecar));
    QCOMPARE(after.at(sidecar.size()), '\n');
    const QJsonObject rec = QJsonDocument::fromJson(after.mid(sidecar.size() + 1).trimmed()).object();
    QCOMPARE(rec.value(QStringLiteral("type")).toString(), QStringLiteral("recovered"));
    QCOMPARE(rec.value(QStringLiteral("file_size")).toInteger(), qint64(video.size()));
    QCOMPARE(rec.value(QStringLiteral("sha256")).toString().toLatin1(), hex(video));
    QCOMPARE(rec.value(QStringLiteral("last_complete_offset")).toInteger(), qint64(100 + 4 * 1000));
    QCOMPARE(rec.value(QStringLiteral("last_complete_seq")).toInt(), 3);
    QVERIFY(rec.value(QStringLiteral("fps_measured")).toBool());
    QCOMPARE(rec.value(QStringLiteral("fps")).toDouble(), 30.0);
    QCOMPARE(e.value(QStringLiteral("sidecar_sha256")).toString().toLatin1(), hex(after));
    QCOMPARE(e.value(QStringLiteral("sidecar_size")).toInteger(), qint64(after.size()));
}

void RecordingRecoveryTest::_recoveredLineNotRepeated()
{
    QTemporaryDir dir;
    const QString name = QStringLiteral("a-p01.mp4");
    const Fixture f = make(name, 3, false);
    writeFile(QDir(dir.path()).filePath(name), f.video);
    const QString side = QDir(dir.path()).filePath(QStringLiteral("a-p01.timemap.jsonl"));
    writeFile(side, f.sidecar);
    QCOMPARE(RecordingRecovery::scan(dir.path(), {}).size(), 1);
    const QByteArray once = readAll(side);
    // A second start before the queue was saved: same entry, no second line.
    const QList<QJsonObject> again = RecordingRecovery::scan(dir.path(), {});
    QCOMPARE(again.size(), 1);
    QCOMPARE(readAll(side), once);
    QVERIFY(again.first().value(QStringLiteral("recovered")).toBool());
    QCOMPARE(again.first().value(QStringLiteral("ended_at")).toString(), iso(f.recvMs.at(2)));
}

void RecordingRecoveryTest::_closedButNotQueued()
{
    QTemporaryDir dir;
    const QString name = QStringLiteral("b-p01.mp4");
    const Fixture f = make(name, 3, true);
    writeFile(QDir(dir.path()).filePath(name), f.video);
    const QString side = QDir(dir.path()).filePath(QStringLiteral("b-p01.timemap.jsonl"));
    writeFile(side, f.sidecar);
    const QList<QJsonObject> found = RecordingRecovery::scan(dir.path(), {});
    QCOMPARE(found.size(), 1);
    QVERIFY(!found.first().value(QStringLiteral("recovered")).toBool());
    QVERIFY(found.first().value(QStringLiteral("state")).toString().isEmpty());
    QCOMPARE(found.first().value(QStringLiteral("sha256")).toString().toLatin1(), hex(f.video));
    QCOMPARE(readAll(side), f.sidecar);         // nothing appended to a closed part
}

void RecordingRecoveryTest::_changedAfterCaptureParked()
{
    QTemporaryDir dir;
    const QString name = QStringLiteral("c-p01.mp4");
    const Fixture f = make(name, 3, true);
    QByteArray edited = f.video;
    edited[500] = 'Z';
    writeFile(QDir(dir.path()).filePath(name), edited);
    writeFile(QDir(dir.path()).filePath(QStringLiteral("c-p01.timemap.jsonl")), f.sidecar);
    const QList<QJsonObject> found = RecordingRecovery::scan(dir.path(), {});
    QCOMPARE(found.size(), 1);
    QCOMPARE(found.first().value(QStringLiteral("state")).toString(), QStringLiteral("parked"));
    QVERIFY(found.first().value(QStringLiteral("changed")).toBool());
    QCOMPARE(found.first().value(QStringLiteral("sha256")).toString().toLatin1(), hex(edited));
}

void RecordingRecoveryTest::_knownNamesSkipped()
{
    QTemporaryDir dir;
    const QString name = QStringLiteral("d-p01.mp4");
    const Fixture f = make(name, 2, false);
    writeFile(QDir(dir.path()).filePath(name), f.video);
    const QString side = QDir(dir.path()).filePath(QStringLiteral("d-p01.timemap.jsonl"));
    writeFile(side, f.sidecar);
    QVERIFY(RecordingRecovery::scan(dir.path(), {name}).isEmpty());
    QCOMPARE(readAll(side), f.sidecar);         // untouched
}

void RecordingRecoveryTest::_unmeasuredFpsFlagged()
{
    // Cut inside the first fragment: no rate can be measured. The registration
    // will carry the nominal rate, and the time map says so.
    QTemporaryDir dir;
    const QString name = QStringLiteral("e-p01.mp4");
    const Fixture f = make(name, 2, false);
    writeFile(QDir(dir.path()).filePath(name), f.video.left(600));
    const QString side = QDir(dir.path()).filePath(QStringLiteral("e-p01.timemap.jsonl"));
    writeFile(side, f.sidecar);
    const QList<QJsonObject> found = RecordingRecovery::scan(dir.path(), {});
    QCOMPARE(found.size(), 1);
    QCOMPARE(found.first().value(QStringLiteral("fps")).toDouble(), 0.0);
    QCOMPARE(found.first().value(QStringLiteral("ended_at")).toString(), iso(kStartMs));
    const QByteArray after = readAll(side);
    const QJsonObject rec = QJsonDocument::fromJson(after.mid(f.sidecar.size()).trimmed()).object();
    QCOMPARE(rec.value(QStringLiteral("type")).toString(), QStringLiteral("recovered"));
    QVERIFY(!rec.value(QStringLiteral("fps_measured")).toBool());
    QVERIFY(rec.contains(QStringLiteral("fps_measured")));
    QVERIFY(!rec.contains(QStringLiteral("fps_registered")));    // no nominal rate
    QVERIFY(!rec.contains(QStringLiteral("fps")));
}
