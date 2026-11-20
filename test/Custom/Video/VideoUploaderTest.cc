#include "VideoUploaderTest.h"

#include "RecordingStore.h"
#include "VideoUploader.h"
#include "fleet/FleetStore.h"
#include "net/HttpClient.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QRandomGenerator>
#include <QtCore/QTemporaryDir>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QTest>

#include <functional>
#include <memory>

namespace {

const QByteArray kToken = "rlgs_test_token_g95";
constexpr int kStation = 7;

QByteArray hex(const QByteArray &b)
{
    return QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex();
}

struct Req
{
    QByteArray method;
    QByteArray target;
    QList<QPair<QByteArray, QByteArray>> headers;
    QByteArray body;
    qint64 atMs = 0;                    ///< when the mock read it

    QByteArray header(const QByteArray &name) const
    {
        for (const auto &h : headers) {
            if (h.first.compare(name, Qt::CaseInsensitive) == 0) {
                return h.second;
            }
        }
        return {};
    }
};

struct Resp
{
    int status = 200;
    QJsonObject json;
    bool hasJson = false;
    QList<QPair<QByteArray, QByteArray>> headers;

    static Resp of(int status) { Resp r; r.status = status; return r; }
    Resp &body(const QJsonObject &o) { json = o; hasJson = true; return *this; }
    Resp &offset(qint64 n) { headers.append(qMakePair(QByteArray("Upload-Offset"), QByteArray::number(n))); return *this; }
};

/// The S21 routes as RotorLab's GC connector API ("Recorded video") describes them,
/// on 127.0.0.1, recording every request.
class MockFleet : public QObject
{
public:
    struct Upload
    {
        QString id;
        QString name;
        QString startedAt;
        qint64 size = 0;
        QByteArray sha;
        QByteArray data;
        bool stored = false;
        bool video = true;
        QString serverState = QStringLiteral("uploading");  ///< as GET /uploads/<id> reports it
        QString flightRef;                                   ///< empty: JSON null
        QJsonObject receipt;                                 ///< empty: JSON null
    };

    /// Fleet hands an upload to Records and gets its signed receipt back
    /// (statement {v, ref, kind, sha256, size, proof} as a JSON string).
    static void handOver(Upload &u, const QString &ref)
    {
        QJsonObject st;
        st[QStringLiteral("v")] = 1;
        st[QStringLiteral("ref")] = ref;
        st[QStringLiteral("kind")] = u.video ? QStringLiteral("video") : QStringLiteral("video-timemap");
        st[QStringLiteral("sha256")] = QString::fromLatin1(u.sha);
        st[QStringLiteral("size")] = u.size;
        st[QStringLiteral("proof")] = QStringLiteral("p-") + u.id;
        u.serverState = QStringLiteral("handed_over");
        u.flightRef = ref;
        u.receipt = QJsonObject();
        u.receipt[QStringLiteral("received_at")] = QStringLiteral("2026-11-18T15:20:00Z");
        u.receipt[QStringLiteral("statement")] = QString::fromUtf8(QJsonDocument(st).toJson(QJsonDocument::Compact));
        u.receipt[QStringLiteral("signature")] = QStringLiteral("c2lnbmF0dXJlLW9m") + u.id;
    }

    MockFleet()
    {
        connect(&_server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *s = _server.nextPendingConnection()) {
                auto buf = std::make_shared<QByteArray>();
                connect(s, &QTcpSocket::readyRead, this, [this, s, buf]() {
                    if (s->property("rlRefused").toBool()) {
                        s->readAll();           // refused: read nothing more
                        return;
                    }
                    buf->append(s->readAll());
                    _drain(s, *buf);
                });
                connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            }
        });
    }

    bool listen() { return _server.listen(QHostAddress::LocalHost, 0); }
    QString url() const { return QStringLiteral("http://127.0.0.1:%1").arg(_server.serverPort()); }

    QList<Req> requests;
    QHash<QString, Upload> uploads;         ///< by upload id
    bool restrictFlights = false;           ///< no report named any flight yet
    QByteArray preseedVideo;                ///< bytes the server already has for the video (offset 0 announced)
    /// Called first for every request; return true with `resp` set to answer instead.
    std::function<bool(const Req &, Resp &)> hook;
    /// A server body-size cap (-1: none). A request whose Content-Length is
    /// over it is refused as soon as its headers are read, before the body:
    /// the connection is closed with no answer, or with a 413 when
    /// `answer413` is set (then closed too). It is recorded without a body.
    qint64 maxBodyBytes = -1;
    bool answer413 = false;

    int count(const QByteArray &method, const QByteArray &targetPart = {}, const QByteArray &range = {}) const
    {
        int n = 0;
        for (const Req &r : requests) {
            if (r.method == method && (targetPart.isEmpty() || r.target.contains(targetPart))
                && (range.isEmpty() || r.header("Content-Range") == range)) {
                ++n;
            }
        }
        return n;
    }

    /// Every request carried the bearer header, and the token was never in a target.
    bool authOnlyInHeader() const
    {
        for (const Req &r : requests) {
            if (r.header("Authorization") != "Bearer " + kToken || r.target.contains(kToken)) {
                return false;
            }
        }
        return !requests.isEmpty();
    }

    Upload *byName(const QString &name)
    {
        for (Upload &u : uploads) {
            if (u.name == name) {
                return &u;
            }
        }
        return nullptr;
    }

private:
    void _drain(QTcpSocket *s, QByteArray &buf)
    {
        for (;;) {
            const qsizetype end = buf.indexOf("\r\n\r\n");
            if (end < 0) {
                return;
            }
            const QList<QByteArray> head = buf.left(end).split('\n');
            Req r;
            const QList<QByteArray> first = head.value(0).trimmed().split(' ');
            r.method = first.value(0);
            r.target = first.value(1);
            qint64 length = 0;
            for (qsizetype i = 1; i < head.size(); ++i) {
                const QByteArray l = head.at(i).trimmed();
                const qsizetype c = l.indexOf(':');
                if (c > 0) {
                    r.headers.append({l.left(c).trimmed(), l.mid(c + 1).trimmed()});
                    if (l.left(c).trimmed().compare("Content-Length", Qt::CaseInsensitive) == 0) {
                        length = l.mid(c + 1).trimmed().toLongLong();
                    }
                }
            }
            if (maxBodyBytes >= 0 && length > maxBodyBytes) {
                r.atMs = QDateTime::currentMSecsSinceEpoch();
                requests.append(r);
                buf.clear();
                s->setProperty("rlRefused", true);
                if (answer413) {
                    QJsonObject o;
                    o[QStringLiteral("error")] = QStringLiteral("Request body too large.");
                    Resp resp = Resp::of(413).body(o);
                    resp.headers.append(qMakePair(QByteArray("Connection"), QByteArray("close")));
                    _send(s, r, resp);
                    s->disconnectFromHost();
                } else {
                    s->abort();                 // closed mid-body, no answer
                }
                return;
            }
            if (buf.size() < end + 4 + length) {
                return;
            }
            r.body = buf.mid(end + 4, length);
            r.atMs = QDateTime::currentMSecsSinceEpoch();
            buf.remove(0, end + 4 + length);
            requests.append(r);
            Resp resp;
            if (!(hook && hook(r, resp))) {
                resp = _answer(r);
            }
            _send(s, r, resp);
        }
    }

    void _send(QTcpSocket *s, const Req &r, const Resp &resp)
    {
        QByteArray body = resp.hasJson ? QJsonDocument(resp.json).toJson(QJsonDocument::Compact) : QByteArray();
        if (r.method == "HEAD") {
            body.clear();
        }
        QByteArray out = "HTTP/1.1 " + QByteArray::number(resp.status) + " X\r\n";
        for (const auto &h : resp.headers) {
            out += h.first + ": " + h.second + "\r\n";
        }
        if (resp.hasJson && r.method != "HEAD") {
            out += "Content-Type: application/json\r\n";
        }
        out += "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
        s->write(out);
        s->flush();
    }

    static Resp error(int status, const QString &words)
    {
        QJsonObject o;
        o[QStringLiteral("error")] = words;
        return Resp::of(status).body(o);
    }

    Resp _answer(const Req &r)
    {
        const QByteArray path = r.target;
        if (r.method == "POST" && path.endsWith("/videos")
            && (path.startsWith("/api/gc/v1/flights/") || path == "/api/gc/v1/stations/self/videos")) {
            if (restrictFlights && path.startsWith("/api/gc/v1/flights/")) {
                return error(404, QStringLiteral("No report has named that flight yet. Use /stations/self/videos."));
            }
            const QJsonObject d = QJsonDocument::fromJson(r.body).object();
            const QString name = d.value(QStringLiteral("name")).toString();
            const QString started = d.value(QStringLiteral("started_at")).toString();
            for (Upload &u : uploads) {
                if (u.name == name && u.startedAt == started) {
                    if (u.sha != d.value(QStringLiteral("sha256")).toString().toLatin1()
                        || u.size != d.value(QStringLiteral("size")).toInteger()) {
                        return error(409, QStringLiteral("A different file was announced under this name and start time."));
                    }
                    QJsonObject o;
                    o[QStringLiteral("upload_id")] = u.id;
                    o[QStringLiteral("offset")] = qint64(u.data.size());
                    return Resp::of(200).body(o);
                }
            }
            Upload u;
            u.id = QStringLiteral("up%1").arg(uploads.size() + 1);
            u.name = name;
            u.startedAt = started;
            u.size = d.value(QStringLiteral("size")).toInteger();
            u.sha = d.value(QStringLiteral("sha256")).toString().toLatin1();
            u.video = d.value(QStringLiteral("kind")).toString(QStringLiteral("video")) == QLatin1String("video");
            if (u.video && !preseedVideo.isEmpty()) {
                u.data = preseedVideo;          // the server is ahead of what it announced
            }
            uploads.insert(u.id, u);
            QJsonObject o;
            o[QStringLiteral("upload_id")] = u.id;
            o[QStringLiteral("offset")] = 0;
            return Resp::of(200).body(o);
        }
        if (path.startsWith("/api/gc/v1/uploads/")) {
            const QString id = QString::fromLatin1(path.mid(int(qstrlen("/api/gc/v1/uploads/"))));
            if (!uploads.contains(id)) {
                return error(404, QStringLiteral("No such upload from this station."));
            }
            Upload &u = uploads[id];
            if (r.method == "HEAD") {
                return Resp::of(200).offset(u.data.size());
            }
            if (r.method == "GET") {
                // S21: where the upload stands.
                QJsonObject o;
                o[QStringLiteral("upload_id")] = u.id;
                o[QStringLiteral("offset")] = qint64(u.data.size());
                o[QStringLiteral("size")] = u.size;
                o[QStringLiteral("sha256")] = QString::fromLatin1(u.sha);
                o[QStringLiteral("state")] = u.serverState;
                o[QStringLiteral("flight_ref")] = u.flightRef.isEmpty() ? QJsonValue() : QJsonValue(u.flightRef);
                o[QStringLiteral("records_receipt")] = u.receipt.isEmpty() ? QJsonValue() : QJsonValue(u.receipt);
                return Resp::of(200).body(o);
            }
            if (r.method != "PUT") {
                return error(405, QStringLiteral("no"));
            }
            if (u.stored) {
                QJsonObject o;
                o[QStringLiteral("sha256")] = QString::fromLatin1(u.sha);
                o[QStringLiteral("size")] = u.size;
                return Resp::of(200).body(o);
            }
            const QByteArray range = r.header("Content-Range");
            const QList<QByteArray> parts = range.mid(6).split('/');
            const QList<QByteArray> ab = parts.value(0).split('-');
            const qint64 a = ab.value(0).toLongLong();
            const qint64 b = ab.value(1).toLongLong();
            if (!range.startsWith("bytes ") || parts.value(1).toLongLong() != u.size || b < a || b >= u.size) {
                return error(400, QStringLiteral("Content-Range does not fit the size announced."));
            }
            if (a != u.data.size()) {
                return error(409, QStringLiteral("Resume from Upload-Offset.")).offset(u.data.size());
            }
            if (r.body.size() != b - a + 1 || hex(r.body) != r.header("X-Chunk-SHA256")) {
                return error(400, QStringLiteral("The chunk does not match X-Chunk-SHA256. Send it again.")).offset(u.data.size());
            }
            u.data += r.body;
            if (u.data.size() < u.size) {
                QJsonObject o;
                o[QStringLiteral("offset")] = qint64(u.data.size());
                return Resp::of(200).body(o).offset(u.data.size());
            }
            if (hex(u.data) != u.sha) {
                u.data.clear();
                return error(400, QStringLiteral("The whole file does not match the hash made at capture. Send it again from the start.")).offset(0);
            }
            u.stored = true;
            u.serverState = QStringLiteral("stored");
            QJsonObject o;
            o[QStringLiteral("sha256")] = QString::fromLatin1(u.sha);
            o[QStringLiteral("size")] = u.size;
            return Resp::of(200).body(o);
        }
        return error(404, QStringLiteral("not found"));
    }

    QTcpServer _server;
};

/// A store, two files and an uploader pointed at the mock.
struct Rig
{
    QTemporaryDir dir;
    MockFleet fleet;
    std::unique_ptr<RecordingStore> store;
    std::unique_ptr<HttpClient> client;
    std::unique_ptr<VideoUploader> up;
    QByteArray video;
    QByteArray sidecar;
    QString id;
    QString serverUrl;

    bool start(const QString &flightRef = QStringLiteral("ref-1"), int unitMs = 1, qint64 videoBytes = 1000)
    {
        if (!fleet.listen()) {
            return false;
        }
        serverUrl = fleet.url();
        store = std::make_unique<RecordingStore>(dir.filePath(QStringLiteral("settings.ini")));
        QRandomGenerator rng(95);
        video.resize(videoBytes);
        for (char &c : video) {
            c = char(rng.bounded(256));
        }
        sidecar = QByteArray("{\"type\":\"header\",\"kind\":\"video-timemap\"}\n").repeated(4);
        const QString vpath = dir.filePath(QStringLiteral("st-main-20261118T150207Z-p01.mp4"));
        const QString spath = dir.filePath(QStringLiteral("st-main-20261118T150207Z-p01.timemap.jsonl"));
        QFile v(vpath);
        QFile s(spath);
        if (!v.open(QIODevice::WriteOnly) || !s.open(QIODevice::WriteOnly)) {
            return false;
        }
        v.write(video);
        s.write(sidecar);
        v.close();
        s.close();

        QJsonObject e;
        e[QStringLiteral("name")] = QStringLiteral("st-main-20261118T150207Z-p01.mp4");
        e[QStringLiteral("path")] = vpath;
        e[QStringLiteral("size")] = qint64(video.size());
        e[QStringLiteral("sha256")] = QString::fromLatin1(hex(video));
        e[QStringLiteral("sidecar_name")] = QStringLiteral("st-main-20261118T150207Z-p01.timemap.jsonl");
        e[QStringLiteral("sidecar_path")] = spath;
        e[QStringLiteral("sidecar_size")] = qint64(sidecar.size());
        e[QStringLiteral("sidecar_sha256")] = QString::fromLatin1(hex(sidecar));
        e[QStringLiteral("camera")] = QStringLiteral("main");
        e[QStringLiteral("codec")] = QStringLiteral("avc1.640028");
        e[QStringLiteral("width")] = 1280;
        e[QStringLiteral("height")] = 720;
        e[QStringLiteral("fps")] = 30.0;
        e[QStringLiteral("started_at")] = QStringLiteral("2026-11-18T15:02:07Z");
        e[QStringLiteral("ended_at")] = QStringLiteral("2026-11-18T15:03:01Z");
        e[QStringLiteral("part")] = 1;
        e[QStringLiteral("flight_ref")] = flightRef;
        e[QStringLiteral("base_url")] = serverUrl;
        e[QStringLiteral("station_id")] = kStation;
        e[QStringLiteral("recovered")] = false;
        e[QStringLiteral("state")] = QStringLiteral("waiting");
        id = store->upsert(e);
        makeUploader(unitMs);
        return true;
    }

    void makeUploader(int unitMs)
    {
        up.reset();
        client = std::make_unique<HttpClient>(HttpClient::TlsPolicy::PublicCa);
        VideoUploader::Environment env;
        const QString url = serverUrl;
        env.canUpload = []() { return true; };
        env.baseUrl = [url]() { return url; };
        env.stationId = []() { return kStation; };
        env.token = []() { return QString::fromLatin1(kToken); };
        env.pin = []() { return QString(); };
        up = std::make_unique<VideoUploader>(store.get(), client.get(), env);
        up->setChunkBytes(400);
        up->setBackoffUnitMs(unitMs);
        up->setSweepMs(50);
    }

    QString state() const { return store->entry(id).value(QStringLiteral("state")).toString(); }
    QJsonObject entry() const { return store->entry(id); }
};

QJsonObject body(const Req &r)
{
    return QJsonDocument::fromJson(r.body).object();
}

} // namespace

void VideoUploaderTest::_registerAndUpload()
{
    Rig rig;
    QVERIFY(rig.start());
    rig.up->kick();
    QTRY_COMPARE_WITH_TIMEOUT(rig.state(), QStringLiteral("done"), 15000);

    const QList<Req> &rq = rig.fleet.requests;
    QVERIFY(rq.size() >= 6);
    QCOMPARE(rq.at(0).method, QByteArray("POST"));
    QCOMPARE(rq.at(0).target, QByteArray("/api/gc/v1/flights/ref-1/videos"));
    const QJsonObject v = body(rq.at(0));
    QCOMPARE(v.value(QStringLiteral("name")).toString(), QStringLiteral("st-main-20261118T150207Z-p01.mp4"));
    QCOMPARE(v.value(QStringLiteral("sha256")).toString().toLatin1(), hex(rig.video));
    QCOMPARE(v.value(QStringLiteral("size")).toInteger(), qint64(1000));
    QCOMPARE(v.value(QStringLiteral("codec")).toString(), QStringLiteral("avc1.640028"));
    QCOMPARE(v.value(QStringLiteral("recovered")).toBool(), false);
    QCOMPARE(v.value(QStringLiteral("fps")).toDouble(), 30.0);      // measured: sent as measured
    QVERIFY(!v.contains(QStringLiteral("kind")));
    // The time map registers right after, before any bytes.
    QCOMPARE(rq.at(1).method, QByteArray("POST"));
    const QJsonObject s = body(rq.at(1));
    QCOMPARE(s.value(QStringLiteral("kind")).toString(), QStringLiteral("video-timemap"));
    QCOMPARE(s.value(QStringLiteral("part_of")).toString(), QStringLiteral("st-main-20261118T150207Z-p01.mp4"));
    QCOMPARE(s.value(QStringLiteral("sha256")).toString().toLatin1(), hex(rig.sidecar));
    // Chunks in order.
    QCOMPARE(rq.at(2).header("Content-Range"), QByteArray("bytes 0-399/1000"));
    QCOMPARE(rq.at(3).header("Content-Range"), QByteArray("bytes 400-799/1000"));
    QCOMPARE(rq.at(4).header("Content-Range"), QByteArray("bytes 800-999/1000"));
    QCOMPARE(rq.at(5).header("Content-Range"), QByteArray("bytes 0-%1/%2").replace("%1", QByteArray::number(rig.sidecar.size() - 1)).replace("%2", QByteArray::number(rig.sidecar.size())));

    MockFleet::Upload *uv = rig.fleet.byName(QStringLiteral("st-main-20261118T150207Z-p01.mp4"));
    QVERIFY(uv && uv->stored);
    QCOMPARE(uv->data, rig.video);
    QVERIFY(rig.fleet.authOnlyInHeader());
    // Kept on disk after the 200: never deleted automatically.
    QVERIFY(QFile::exists(rig.entry().value(QStringLiteral("path")).toString()));
    QVERIFY(QFile::exists(rig.entry().value(QStringLiteral("sidecar_path")).toString()));
    QCOMPARE(rig.entry().value(QStringLiteral("video_server_sha256")).toString().toLatin1(), hex(rig.video));
}

void VideoUploaderTest::_stationRouteWithoutRef()
{
    Rig rig;
    QVERIFY(rig.start(QString()));
    rig.up->kick();
    QTRY_COMPARE_WITH_TIMEOUT(rig.state(), QStringLiteral("done"), 15000);
    QCOMPARE(rig.fleet.count("POST", "/api/gc/v1/stations/self/videos"), 2);
    QCOMPARE(rig.fleet.count("POST", "/flights/"), 0);
    QVERIFY(rig.fleet.authOnlyInHeader());
}

void VideoUploaderTest::_unknownFlightFallsBackToStation()
{
    Rig rig;
    rig.fleet.restrictFlights = true;
    QVERIFY(rig.start());
    rig.up->kick();
    QTRY_COMPARE_WITH_TIMEOUT(rig.state(), QStringLiteral("done"), 15000);
    QCOMPARE(rig.fleet.count("POST", "/api/gc/v1/flights/ref-1/videos"), 1);
    QCOMPARE(rig.fleet.count("POST", "/api/gc/v1/stations/self/videos"), 2);
}

void VideoUploaderTest::_headResumeAfterRestart()
{
    Rig rig;
    int videoPuts = 0;
    rig.fleet.hook = [&videoPuts](const Req &r, Resp &resp) {
        if (r.method == "PUT" && r.header("Content-Range").endsWith("/1000") && ++videoPuts == 2) {
            resp = Resp::of(503);           // the server goes away mid-upload
            return true;
        }
        return false;
    };
    QVERIFY(rig.start(QStringLiteral("ref-1"), 600000));   // a long backoff: nothing retries by itself
    rig.up->kick();
    QTRY_COMPARE_WITH_TIMEOUT(rig.entry().value(QStringLiteral("attempts")).toInt(), 1, 15000);
    QCOMPARE(rig.entry().value(QStringLiteral("video_offset")).toInteger(), qint64(400));

    // GC restarts: a new uploader on the same queue, the wait already over.
    rig.makeUploader(1);
    QJsonObject e = rig.entry();
    e[QStringLiteral("next_try_ms")] = 0;
    rig.store->upsert(e);
    const int before = int(rig.fleet.requests.size());
    rig.up->kick();
    QTRY_COMPARE_WITH_TIMEOUT(rig.state(), QStringLiteral("done"), 15000);
    QCOMPARE(rig.fleet.requests.at(before).method, QByteArray("HEAD"));
    QVERIFY(rig.fleet.requests.at(before).target.startsWith("/api/gc/v1/uploads/"));
    QCOMPARE(rig.fleet.requests.at(before + 1).header("Content-Range"), QByteArray("bytes 400-799/1000"));
    QCOMPARE(rig.fleet.count("PUT", {}, "bytes 0-399/1000"), 1);   // nothing sent twice
    QCOMPARE(rig.fleet.byName(QStringLiteral("st-main-20261118T150207Z-p01.mp4"))->data, rig.video);
    QVERIFY(rig.fleet.authOnlyInHeader());
}

void VideoUploaderTest::_offset409()
{
    Rig rig;
    QVERIFY(rig.start());
    rig.fleet.preseedVideo = rig.video.left(800);
    rig.up->kick();
    QTRY_COMPARE_WITH_TIMEOUT(rig.state(), QStringLiteral("done"), 15000);
    QCOMPARE(rig.fleet.count("PUT", {}, "bytes 0-399/1000"), 1);    // answered 409, offset 800
    QCOMPARE(rig.fleet.count("PUT", {}, "bytes 800-999/1000"), 1);
    QCOMPARE(rig.fleet.count("PUT", {}, "bytes 400-799/1000"), 0);
    QCOMPARE(rig.fleet.byName(QStringLiteral("st-main-20261118T150207Z-p01.mp4"))->data, rig.video);
}

void VideoUploaderTest::_chunk400Resent()
{
    Rig rig;
    int puts = 0;
    rig.fleet.hook = [&puts](const Req &r, Resp &resp) {
        if (r.method == "PUT" && r.header("Content-Range") == "bytes 400-799/1000" && ++puts == 1) {
            QJsonObject o;
            o[QStringLiteral("error")] = QStringLiteral("The chunk does not match X-Chunk-SHA256. Send it again.");
            resp = Resp::of(400).body(o).offset(400);
            return true;
        }
        return false;
    };
    QVERIFY(rig.start());
    rig.up->kick();
    QTRY_COMPARE_WITH_TIMEOUT(rig.state(), QStringLiteral("done"), 15000);
    QCOMPARE(rig.fleet.count("PUT", {}, "bytes 400-799/1000"), 2);
    QCOMPARE(rig.fleet.count("PUT", {}, "bytes 0-399/1000"), 1);
    QCOMPARE(rig.fleet.byName(QStringLiteral("st-main-20261118T150207Z-p01.mp4"))->data, rig.video);
}

void VideoUploaderTest::_finalMismatchRestartsOnce()
{
    Rig rig;
    int finals = 0;
    MockFleet *fleet = &rig.fleet;
    rig.fleet.hook = [&finals, fleet](const Req &r, Resp &resp) {
        if (r.method == "PUT" && r.header("Content-Range") == "bytes 800-999/1000" && ++finals == 1) {
            // The whole file did not match on the server: back to 0.
            for (MockFleet::Upload &u : fleet->uploads) {
                if (u.video) {
                    u.data.clear();
                }
            }
            QJsonObject o;
            o[QStringLiteral("error")] = QStringLiteral("The whole file does not match the hash made at capture.");
            resp = Resp::of(400).body(o).offset(0);
            return true;
        }
        return false;
    };
    QVERIFY(rig.start());
    rig.up->kick();
    QTRY_COMPARE_WITH_TIMEOUT(rig.state(), QStringLiteral("done"), 15000);
    QCOMPARE(rig.fleet.count("PUT", {}, "bytes 0-399/1000"), 2);
    QCOMPARE(rig.fleet.count("PUT", {}, "bytes 800-999/1000"), 2);
    QVERIFY(rig.entry().value(QStringLiteral("video_restarted")).toBool());
}

void VideoUploaderTest::_final200MismatchRestarts()
{
    Rig rig;
    int finals = 0;
    MockFleet *fleet = &rig.fleet;
    rig.fleet.hook = [&finals, fleet](const Req &r, Resp &resp) {
        if (r.method == "PUT" && r.header("Content-Range") == "bytes 800-999/1000" && ++finals == 1) {
            for (MockFleet::Upload &u : fleet->uploads) {
                if (u.video) {
                    u.data.clear();
                }
            }
            QJsonObject o;
            o[QStringLiteral("sha256")] = QString(64, QLatin1Char('0'));
            o[QStringLiteral("size")] = 1000;
            resp = Resp::of(200).body(o);
            return true;
        }
        return false;
    };
    QVERIFY(rig.start());
    rig.up->kick();
    QTRY_COMPARE_WITH_TIMEOUT(rig.state(), QStringLiteral("done"), 15000);
    QCOMPARE(rig.fleet.count("PUT", {}, "bytes 0-399/1000"), 2);
    QCOMPARE(rig.entry().value(QStringLiteral("video_server_sha256")).toString().toLatin1(), hex(rig.video));
}

void VideoUploaderTest::_stop401KeepsEverything()
{
    Rig rig;
    rig.fleet.hook = [](const Req &, Resp &resp) {
        QJsonObject o;
        o[QStringLiteral("error")] = QStringLiteral("This station was revoked.");
        resp = Resp::of(401).body(o);
        return true;
    };
    QVERIFY(rig.start());
    rig.up->kick();
    QTRY_VERIFY_WITH_TIMEOUT(rig.up->revoked(), 15000);
    QCOMPARE(rig.fleet.requests.size(), 1);
    QTest::qWait(300);
    QCOMPARE(rig.fleet.requests.size(), 1);     // stopped, not looping
    QVERIFY(rig.state() != QLatin1String("done"));
    QVERIFY(rig.state() != QLatin1String("parked"));
    QVERIFY(!rig.up->statusText().isEmpty());
    QVERIFY(QFile::exists(rig.entry().value(QStringLiteral("path")).toString()));
    QVERIFY(rig.fleet.authOnlyInHeader());

    // Enrolled and reachable again: it carries on.
    rig.fleet.hook = {};
    rig.up->resume();
    QTRY_COMPARE_WITH_TIMEOUT(rig.state(), QStringLiteral("done"), 15000);
}

void VideoUploaderTest::_backoff503()
{
    Rig rig;
    int posts = 0;
    rig.fleet.hook = [&posts](const Req &r, Resp &resp) {
        if (r.method == "POST" && ++posts <= 2) {
            resp = Resp::of(503);
            resp.headers.append(qMakePair(QByteArray("Retry-After"), QByteArray("1")));
            return true;
        }
        return false;
    };
    QVERIFY(rig.start());
    rig.up->kick();
    QTRY_COMPARE_WITH_TIMEOUT(rig.state(), QStringLiteral("done"), 15000);
    QCOMPARE(rig.fleet.count("POST", "/flights/ref-1/videos"), 4);   // 2 refused, then video and time map
}

void VideoUploaderTest::_wrongServerWaits()
{
    Rig rig;
    QVERIFY(rig.start());
    QJsonObject e = rig.entry();
    e[QStringLiteral("base_url")] = QStringLiteral("http://127.0.0.1:1");    // captured under another enrollment
    rig.store->upsert(e);
    rig.up->kick();
    QTRY_COMPARE_WITH_TIMEOUT(rig.state(), QStringLiteral("wrong_server"), 5000);
    QTest::qWait(200);
    QVERIFY(rig.fleet.requests.isEmpty());
    QVERIFY(!rig.entry().value(QStringLiteral("detail")).toString().isEmpty());

    rig.up->sendToThisServer(rig.id);           // the operator's explicit action
    QTRY_COMPARE_WITH_TIMEOUT(rig.state(), QStringLiteral("done"), 15000);
    QCOMPARE(rig.entry().value(QStringLiteral("base_url")).toString(), rig.serverUrl);
}

void VideoUploaderTest::_changedFileParked()
{
    Rig rig;
    int videoPuts = 0;
    rig.fleet.hook = [&videoPuts](const Req &r, Resp &resp) {
        if (r.method == "PUT" && r.header("Content-Range").endsWith("/1000") && ++videoPuts == 2) {
            resp = Resp::of(503);
            return true;
        }
        return false;
    };
    QVERIFY(rig.start(QStringLiteral("ref-1"), 600000));
    rig.up->kick();
    QTRY_COMPARE_WITH_TIMEOUT(rig.entry().value(QStringLiteral("attempts")).toInt(), 1, 15000);

    // Someone edits the file on this computer (same size).
    QFile f(rig.entry().value(QStringLiteral("path")).toString());
    QVERIFY(f.open(QIODevice::ReadWrite));
    QVERIFY(f.seek(10));
    f.write("X");
    f.close();

    QJsonObject e = rig.entry();
    e[QStringLiteral("next_try_ms")] = 0;
    rig.store->upsert(e);
    QTRY_COMPARE_WITH_TIMEOUT(rig.state(), QStringLiteral("parked"), 15000);
    QVERIFY(rig.entry().value(QStringLiteral("changed")).toBool());
    QCOMPARE(rig.fleet.count("PUT", {}, "bytes 400-799/1000"), 1);   // the 503'd one; nothing after the change
}

void VideoUploaderTest::_fpsNullWhenUnknown()
{
    // A part too short to measure registers fps null, never a nominal 30.
    Rig rig;
    QVERIFY(rig.start());
    QJsonObject e = rig.entry();
    e[QStringLiteral("fps")] = 0.0;
    rig.store->upsert(e);
    rig.up->kick();
    QTRY_COMPARE_WITH_TIMEOUT(rig.state(), QStringLiteral("done"), 15000);
    const QJsonObject v = body(rig.fleet.requests.at(0));
    QVERIFY(v.contains(QStringLiteral("fps")));
    QVERIFY(v.value(QStringLiteral("fps")).isNull());
    QVERIFY(rig.fleet.requests.at(0).body.contains("\"fps\":null"));
    // The time map registration carries no fps at all.
    QVERIFY(!body(rig.fleet.requests.at(1)).contains(QStringLiteral("fps")));
}

void VideoUploaderTest::_retryAfter503Honored()
{
    // Server 2.0 sends Retry-After: 30 on 503; GC waits that long, not its
    // own 1 s first step. Seconds are 20 ms here.
    constexpr int unit = 20;
    Rig rig;
    int posts = 0;
    rig.fleet.hook = [&posts](const Req &r, Resp &resp) {
        if (r.method == "POST" && ++posts == 1) {
            QJsonObject o;
            o[QStringLiteral("error")] = QStringLiteral("Fleet is busy. Try again shortly.");
            resp = Resp::of(503).body(o);
            resp.headers.append(qMakePair(QByteArray("Retry-After"), QByteArray("30")));
            return true;
        }
        return false;
    };
    QVERIFY(rig.start(QStringLiteral("ref-1"), unit));
    rig.up->kick();
    QTRY_COMPARE_WITH_TIMEOUT(rig.entry().value(QStringLiteral("attempts")).toInt(), 1, 15000);
    QVERIFY(rig.entry().value(QStringLiteral("detail")).toString().contains(QStringLiteral("30 s")));
    QCOMPARE(rig.entry().value(QStringLiteral("backoff_index")).toInt(), 0);   // own schedule untouched
    QTRY_COMPARE_WITH_TIMEOUT(rig.state(), QStringLiteral("done"), 15000);
    QVERIFY(rig.fleet.requests.size() >= 2);
    const qint64 waited = rig.fleet.requests.at(1).atMs - rig.fleet.requests.at(0).atMs;
    QVERIFY2(waited >= 30 * unit - 50, qPrintable(QString::number(waited)));
}

void VideoUploaderTest::_recordsStatusAndReceipt()
{
    // After the final 200: GET /uploads/<id> for state, flight_ref and the
    // Records receipt. Seconds are 10 minutes here, so nothing runs by itself;
    // the test moves the clock by clearing records_next_ms.
    constexpr int unit = 600000;
    Rig rig;
    QVERIFY(rig.start(QString(), unit));       // station route: no flight ref at capture
    rig.up->kick();
    QTRY_COMPARE_WITH_TIMEOUT(rig.state(), QStringLiteral("done"), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!rig.up->busy(), 5000);
    QVERIFY(VideoUploader::wantsRecordsStatus(rig.entry()));
    QCOMPARE(rig.fleet.count("GET"), 0);
    QVERIFY(rig.entry().value(QStringLiteral("detail")).toString().contains(QStringLiteral("Not yet received by Records")));
    const qint64 firstDue = rig.entry().value(QStringLiteral("records_next_ms")).toInteger() - QDateTime::currentMSecsSinceEpoch();
    QVERIFY(firstDue > 100LL * unit && firstDue <= 120LL * unit);              // every few minutes

    const auto due = [&rig]() {
        QJsonObject e = rig.entry();
        e[QStringLiteral("records_next_ms")] = 0;
        rig.store->upsert(e);
        rig.up->kick();
    };

    // A 503 with Retry-After: the next check waits what the server said.
    int gets = 0;
    rig.fleet.hook = [&gets](const Req &r, Resp &resp) {
        if (r.method == "GET" && ++gets == 1) {
            resp = Resp::of(503);
            resp.headers.append(qMakePair(QByteArray("Retry-After"), QByteArray("30")));
            return true;
        }
        return false;
    };
    due();
    QTRY_COMPARE_WITH_TIMEOUT(rig.fleet.count("GET"), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!rig.up->busy(), 5000);
    const qint64 wait503 = rig.entry().value(QStringLiteral("records_next_ms")).toInteger() - QDateTime::currentMSecsSinceEpoch();
    QVERIFY(wait503 > 25LL * unit && wait503 <= 30LL * unit);

    // Stored, not yet attached or received: video and time map both asked.
    due();
    QTRY_COMPARE_WITH_TIMEOUT(rig.fleet.count("GET"), 3, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!rig.up->busy(), 5000);
    QCOMPARE(rig.entry().value(QStringLiteral("video_records_state")).toString(), QStringLiteral("stored"));
    QCOMPARE(rig.entry().value(QStringLiteral("sidecar_records_state")).toString(), QStringLiteral("stored"));
    QVERIFY(rig.entry().value(QStringLiteral("records_flight_ref")).toString().isEmpty());
    QVERIFY(VideoUploader::wantsRecordsStatus(rig.entry()));
    const qint64 backoff = rig.entry().value(QStringLiteral("records_next_ms")).toInteger() - QDateTime::currentMSecsSinceEpoch();
    QVERIFY(backoff > 250LL * unit && backoff <= 300LL * unit);                // backing off

    // Fleet attaches it to a flight and hands both to Records.
    MockFleet::Upload *uv = rig.fleet.byName(QStringLiteral("st-main-20261118T150207Z-p01.mp4"));
    MockFleet::Upload *us = rig.fleet.byName(QStringLiteral("st-main-20261118T150207Z-p01.timemap.jsonl"));
    QVERIFY(uv && us);
    MockFleet::handOver(*uv, QStringLiteral("flight-ref-0001"));
    MockFleet::handOver(*us, QStringLiteral("flight-ref-0001"));
    due();
    QTRY_COMPARE_WITH_TIMEOUT(rig.fleet.count("GET"), 5, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!rig.up->busy(), 5000);
    const QJsonObject e = rig.entry();
    QCOMPARE(e.value(QStringLiteral("state")).toString(), QStringLiteral("done"));
    QCOMPARE(e.value(QStringLiteral("video_records_state")).toString(), QStringLiteral("handed_over"));
    QCOMPARE(e.value(QStringLiteral("records_flight_ref")).toString(), QStringLiteral("flight-ref-0001"));
    QCOMPARE(e.value(QStringLiteral("video_receipt_at")).toString(), QStringLiteral("2026-11-18T15:20:00Z"));
    QCOMPARE(e.value(QStringLiteral("video_receipt_sha256")).toString().toLatin1(), hex(rig.video));
    QCOMPARE(e.value(QStringLiteral("sidecar_receipt_sha256")).toString().toLatin1(), hex(rig.sidecar));
    QVERIFY(!VideoUploader::wantsRecordsStatus(e));

    // Kept next to the recording exactly as received.
    const QString vpath = e.value(QStringLiteral("path")).toString();
    QCOMPARE(e.value(QStringLiteral("video_receipt_path")).toString(), VideoUploader::receiptPath(vpath));
    QFile rf(VideoUploader::receiptPath(vpath));
    QVERIFY(rf.open(QIODevice::ReadOnly));
    const QJsonObject kept = QJsonDocument::fromJson(rf.readAll()).object();
    rf.close();
    QCOMPARE(kept, uv->receipt);
    QCOMPARE(kept.value(QStringLiteral("statement")).toString(), uv->receipt.value(QStringLiteral("statement")).toString());
    QVERIFY(QFile::exists(VideoUploader::receiptPath(e.value(QStringLiteral("sidecar_path")).toString())));
    QVERIFY(QFile::exists(vpath));

    // The operator sees it, in guidance words only.
    const QString detail = e.value(QStringLiteral("detail")).toString();
    QVERIFY(detail.contains(QStringLiteral("Received by Records at 2026-11-18T15:20:00Z")));
    QVERIFY(detail.contains(QString::fromLatin1(hex(rig.video))));
    for (const char *word : {"evidence", "certif", "admissib"}) {
        QVERIFY2(!detail.contains(QLatin1String(word), Qt::CaseInsensitive), word);
    }

    // Done: no more checks, and a later answer never replaces the receipt.
    uv->receipt[QStringLiteral("signature")] = QStringLiteral("changed");
    due();
    QTest::qWait(300);
    QCOMPARE(rig.fleet.count("GET"), 5);
    QCOMPARE(rig.entry().value(QStringLiteral("video_receipt")).toObject(), kept);
    QVERIFY(rig.fleet.authOnlyInHeader());
}

void VideoUploaderTest::_recordsPollingCap()
{
    constexpr int unit = 600000;
    Rig rig;
    QVERIFY(rig.start(QStringLiteral("ref-1"), unit));
    rig.up->kick();
    QTRY_COMPARE_WITH_TIMEOUT(rig.state(), QStringLiteral("done"), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!rig.up->busy(), 5000);
    QJsonObject e = rig.entry();
    e[QStringLiteral("records_polls")] = VideoUploader::kRecordsMaxPolls - 1;
    e[QStringLiteral("records_next_ms")] = 0;
    rig.store->upsert(e);
    rig.up->kick();
    QTRY_VERIFY_WITH_TIMEOUT(rig.entry().value(QStringLiteral("records_poll_stopped")).toBool(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!rig.up->busy(), 5000);
    const int gets = rig.fleet.count("GET");
    QVERIFY(gets >= 1);
    QVERIFY(!VideoUploader::wantsRecordsStatus(rig.entry()));
    QVERIFY(rig.entry().value(QStringLiteral("detail")).toString().contains(QStringLiteral("Check again")));
    rig.up->kick();
    QTest::qWait(200);
    QCOMPARE(rig.fleet.count("GET"), gets);                // stopped asking

    rig.up->checkRecords(rig.id);                          // the operator's "Check again"
    QTRY_VERIFY_WITH_TIMEOUT(rig.fleet.count("GET") > gets, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!rig.up->busy(), 5000);
    QVERIFY(!rig.entry().value(QStringLiteral("records_poll_stopped")).toBool());
    QCOMPARE(rig.entry().value(QStringLiteral("state")).toString(), QStringLiteral("done"));
}

void VideoUploaderTest::_noFleetStaysLocal()
{
    // RotorLab Connector as the route and no Fleet enrollment :
    // the real environment says no, nothing is sent, the recording stays.
    FleetStore::clearAll();
    Rig rig;
    QVERIFY(rig.start());
    rig.up.reset();
    const VideoUploader::Environment env = VideoUploader::fleetEnvironment([]() { return true; });
    QVERIFY(!env.canUpload());
    rig.up = std::make_unique<VideoUploader>(rig.store.get(), rig.client.get(), env);
    rig.up->setSweepMs(50);
    rig.up->kick();
    QTest::qWait(400);
    QVERIFY(rig.fleet.requests.isEmpty());
    QCOMPARE(rig.state(), QStringLiteral("waiting"));
    QVERIFY(QFile::exists(rig.entry().value(QStringLiteral("path")).toString()));
    QVERIFY(QFile::exists(rig.entry().value(QStringLiteral("sidecar_path")).toString()));
    FleetStore::clearAll();
}

void VideoUploaderTest::_httpPatchJson()
{
    // The shared HttpClient's PATCH (the relay sends PATCH /live/<id> with it):
    // JSON body, bearer header only, the answer decoded like any JSON call.
    MockFleet fleet;
    QVERIFY(fleet.listen());
    fleet.hook = [](const Req &r, Resp &resp) {
        if (r.method == "PATCH" && r.target == "/api/gc/v1/live/lv1") {
            QJsonObject o;
            o[QStringLiteral("flight_ref")] = QJsonDocument::fromJson(r.body).object().value(QStringLiteral("flight_ref"));
            resp = Resp::of(200).body(o);
            return true;
        }
        return false;
    };
    HttpClient client(HttpClient::TlsPolicy::PublicCa);
    client.setBaseUrl(fleet.url());
    client.setToken(QString::fromLatin1(kToken));
    bool got = false;
    HttpReply reply;
    QJsonObject b;
    b[QStringLiteral("flight_ref")] = QStringLiteral("ref-9");
    client.patchJson(QStringLiteral("/api/gc/v1/live/lv1"), b, [&](const HttpReply &r) { reply = r; got = true; });
    QTRY_VERIFY_WITH_TIMEOUT(got, 10000);
    QCOMPARE(reply.status, 200);
    QCOMPARE(reply.body.value(QStringLiteral("flight_ref")).toString(), QStringLiteral("ref-9"));
    QCOMPARE(fleet.requests.size(), 1);
    QCOMPARE(fleet.requests.at(0).method, QByteArray("PATCH"));
    QVERIFY(fleet.requests.at(0).header("Content-Type").startsWith("application/json"));
    QCOMPARE(body(fleet.requests.at(0)).value(QStringLiteral("flight_ref")).toString(), QStringLiteral("ref-9"));
    QVERIFY(fleet.authOnlyInHeader());
}

namespace {

/// Every detail text the uploader showed while `rig` ran.
struct DetailLog
{
    QStringList seen;

    void watch(Rig &rig)
    {
        QObject::connect(rig.up.get(), &VideoUploader::changed, rig.up.get(), [this, &rig]() {
            const QString d = rig.entry().value(QStringLiteral("detail")).toString();
            if (!d.isEmpty() && (seen.isEmpty() || seen.last() != d)) {
                seen.append(d);
            }
        });
    }

    /// No raw Qt transport sentence reached the operator.
    bool plain() const
    {
        for (const QString &d : seen) {
            for (const char *raw : {"Connection closed", "Unknown error", "Unable to write",
                                    "remote host closed", "Error transferring"}) {
                if (d.contains(QLatin1String(raw), Qt::CaseInsensitive)) {
                    return false;
                }
            }
        }
        return true;
    }
};

} // namespace

void VideoUploaderTest::_earlyCloseOnLargeChunkParks()
{
    // S20/S21 end-to-end: the server's body-size cap (1 MB) is below GC's
    // 8 MB chunk, and it closes the connection mid-body with no answer.
    Rig rig;
    rig.fleet.maxBodyBytes = 1024 * 1024;
    constexpr qint64 kVideo = 3 * 1024 * 1024 + 123;
    QVERIFY(rig.start(QStringLiteral("ref-1"), 1, kVideo));
    rig.up->setChunkBytes(VideoUploader::kChunkBytes);
    DetailLog log;
    log.watch(rig);
    rig.up->kick();
    QTRY_COMPARE_WITH_TIMEOUT(rig.state(), QStringLiteral("parked"), 20000);

    const QByteArray firstChunk = "bytes 0-" + QByteArray::number(kVideo - 1) + "/" + QByteArray::number(kVideo);
    // A few tries on the same chunk, then parked instead of retrying forever.
    // (Qt sends a request once more by itself when the connection closes
    // before any answer, so the wire sees up to two PUTs per try.)
    const QJsonObject e = rig.entry();
    QCOMPARE(e.value(QStringLiteral("early_closes")).toInt(), VideoUploader::kMaxEarlyCloses);
    const int puts = rig.fleet.count("PUT", {}, firstChunk);
    QVERIFY2(puts >= VideoUploader::kMaxEarlyCloses && puts <= 2 * VideoUploader::kMaxEarlyCloses,
             qPrintable(QString::number(puts)));
    QTest::qWait(300);
    QCOMPARE(rig.fleet.count("PUT", {}, firstChunk), puts);
    QVERIFY(e.value(QStringLiteral("retryable")).toBool());          // "Try again"
    QVERIFY(e.value(QStringLiteral("detail")).toString().contains(QStringLiteral("refused this upload chunk as too large")));
    QVERIFY(e.value(QStringLiteral("detail")).toString().contains(QStringLiteral("8 MB")));
    // The waits in between said the same plain thing; no raw Qt text.
    bool waitedPlainly = false;
    for (const QString &d : log.seen) {
        waitedPlainly |= d.startsWith(QStringLiteral("Waiting to try again"))
                         && d.contains(QStringLiteral("refused this upload chunk as too large"));
    }
    QVERIFY2(waitedPlainly, qPrintable(log.seen.join(QStringLiteral(" | "))));
    QVERIFY2(log.plain(), qPrintable(log.seen.join(QStringLiteral(" | "))));
    // The copy stays on disk, unchanged.
    QFile f(e.value(QStringLiteral("path")).toString());
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(hex(f.readAll()), hex(rig.video));
    f.close();
    QVERIFY(QFile::exists(e.value(QStringLiteral("sidecar_path")).toString()));

    // The server's cap is raised to fit (here: GC's chunks made to fit it),
    // and the operator's "Try again" sends it.
    rig.up->setChunkBytes(512 * 1024);
    rig.up->retry(rig.id);
    QTRY_COMPARE_WITH_TIMEOUT(rig.state(), QStringLiteral("done"), 20000);
    QCOMPARE(rig.fleet.byName(QStringLiteral("st-main-20261118T150207Z-p01.mp4"))->data, rig.video);
    QVERIFY(QFile::exists(e.value(QStringLiteral("path")).toString()));
}

void VideoUploaderTest::_chunk413Parks()
{
    // The same cap, but the server answers 413 before closing. Whether Qt
    // hands GC the 413 or only the close, the operator reads one plain
    // sentence and the recording parks with "Try again", kept on disk.
    Rig rig;
    rig.fleet.maxBodyBytes = 1024 * 1024;
    rig.fleet.answer413 = true;
    constexpr qint64 kVideo = 2 * 1024 * 1024;
    QVERIFY(rig.start(QStringLiteral("ref-1"), 1, kVideo));
    rig.up->setChunkBytes(VideoUploader::kChunkBytes);
    DetailLog log;
    log.watch(rig);
    rig.up->kick();
    QTRY_COMPARE_WITH_TIMEOUT(rig.state(), QStringLiteral("parked"), 20000);
    const int puts = rig.fleet.count("PUT");
    QVERIFY2(puts >= 1 && puts <= 2 * VideoUploader::kMaxEarlyCloses, qPrintable(QString::number(puts)));
    QTest::qWait(300);
    QCOMPARE(rig.fleet.count("PUT"), puts);
    const QJsonObject e = rig.entry();
    QVERIFY(e.value(QStringLiteral("retryable")).toBool());
    QVERIFY(e.value(QStringLiteral("detail")).toString().contains(QStringLiteral("refused this upload chunk as too large")));
    QVERIFY2(log.plain(), qPrintable(log.seen.join(QStringLiteral(" | "))));
    QVERIFY(QFile::exists(e.value(QStringLiteral("path")).toString()));
    QCOMPARE(QFileInfo(e.value(QStringLiteral("path")).toString()).size(), kVideo);

    // A 413 that reaches GC as an answer (the body read, then refused) parks
    // at once, with the same sentence: no retry loop.
    Rig plain;
    plain.fleet.hook = [](const Req &r, Resp &resp) {
        if (r.method == "PUT" && r.target.startsWith("/api/gc/v1/uploads/") && r.body.size() > 256) {
            QJsonObject o;
            o[QStringLiteral("error")] = QStringLiteral("Request body too large.");
            resp = Resp::of(413).body(o);
            return true;
        }
        return false;
    };
    QVERIFY(plain.start());
    plain.up->kick();
    QTRY_COMPARE_WITH_TIMEOUT(plain.state(), QStringLiteral("parked"), 15000);
    QCOMPARE(plain.fleet.count("PUT"), 1);
    QVERIFY(plain.entry().value(QStringLiteral("retryable")).toBool());
    QVERIFY(plain.entry().value(QStringLiteral("detail")).toString().startsWith(
        QStringLiteral("The Fleet Server refused this upload chunk as too large.")));
    QVERIFY(QFile::exists(plain.entry().value(QStringLiteral("path")).toString()));
}
