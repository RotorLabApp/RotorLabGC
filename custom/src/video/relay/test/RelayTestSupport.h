/****************************************************************************
 *
 * RotorLab GC - G109 tests: synthetic H.264 units, an MP4 box walker, and an
 * in-process S20 mock server on 127.0.0.1 (never a real Fleet Server, never
 * rotorlab.app).
 *
 ****************************************************************************/

#pragma once

#include "RelayTypes.h"

#include <QtCore/QByteArray>
#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QList>
#include <QtCore/QRegularExpression>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QSslCertificate>
#include <QtNetwork/QSslConfiguration>
#include <QtNetwork/QSslKey>
#include <QtNetwork/QSslServer>
#include <QtNetwork/QSslSocket>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <functional>
#include <memory>

namespace RelayTest {

// ------------------------------------------------------------------ H.264

class BitWriter
{
public:
    void bit(int b)
    {
        if ((_n & 7) == 0) {
            _bytes.append('\0');
        }
        if (b) {
            _bytes[_bytes.size() - 1] = char(quint8(_bytes.at(_bytes.size() - 1)) | (0x80 >> (_n & 7)));
        }
        ++_n;
    }
    void bits(quint32 v, int n)
    {
        for (int i = n - 1; i >= 0; --i) {
            bit((v >> i) & 1);
        }
    }
    void ue(quint32 v)
    {
        const quint64 x = quint64(v) + 1;
        int len = 0;
        while ((x >> len) > 1) {
            ++len;
        }
        bits(0, len);
        bits(quint32(x), len + 1);
    }
    void trailing()
    {
        bit(1);
        while (_n & 7) {
            bit(0);
        }
    }
    QByteArray bytes() const { return _bytes; }

private:
    QByteArray _bytes;
    int _n = 0;
};

/// Emulation prevention for a NAL payload.
inline QByteArray escape(const QByteArray &rbsp)
{
    QByteArray out;
    int zeros = 0;
    for (char c : rbsp) {
        if (zeros >= 2 && quint8(c) <= 3) {
            out.append('\x03');
            zeros = 0;
        }
        out.append(c);
        zeros = (c == 0) ? zeros + 1 : 0;
    }
    return out;
}

/// An SPS (header byte included, no start code) for a 4:2:0 picture.
inline QByteArray makeSps(int width, int height, int profile = 66, int level = 31)
{
    const int wMbs = (width + 15) / 16;
    const int hMbs = (height + 15) / 16;
    const int cropR = (wMbs * 16 - width) / 2;
    const int cropB = (hMbs * 16 - height) / 2;
    BitWriter w;
    w.bits(quint32(profile), 8);
    w.bits(profile == 66 ? 0xC0 : 0x00, 8);
    w.bits(quint32(level), 8);
    w.ue(0);
    if (profile == 100) {
        w.ue(1);    // 4:2:0
        w.ue(0);
        w.ue(0);
        w.bit(0);
        w.bit(0);
    }
    w.ue(0);        // log2_max_frame_num_minus4
    w.ue(0);        // poc type 0
    w.ue(0);
    w.ue(1);        // max_num_ref_frames
    w.bit(0);
    w.ue(quint32(wMbs - 1));
    w.ue(quint32(hMbs - 1));
    w.bit(1);       // frame_mbs_only
    w.bit(1);       // direct_8x8
    if (cropR || cropB) {
        w.bit(1);
        w.ue(0);
        w.ue(quint32(cropR));
        w.ue(0);
        w.ue(quint32(cropB));
    } else {
        w.bit(0);
    }
    w.bit(0);       // no VUI
    w.trailing();
    return QByteArray(1, '\x67') + escape(w.bytes());
}

inline QByteArray makePps()
{
    return QByteArray::fromHex("68ce3880");
}

inline QByteArray startCode() { return QByteArray::fromHex("00000001"); }

/// An access unit: [SPS PPS] slice of `sliceBytes` (no start-code patterns inside).
inline QByteArray makeAccessUnit(bool key, int sliceBytes, const QByteArray &sps = QByteArray(),
                                 const QByteArray &pps = QByteArray())
{
    QByteArray au;
    if (key && !sps.isEmpty()) {
        au += startCode() + sps + startCode() + pps;
    }
    QByteArray slice(qMax(2, sliceBytes), '\xAA');
    slice[0] = key ? '\x65' : '\x41';
    au += startCode() + slice;
    return au;
}

inline RelayUnit makeUnit(bool key, qint64 pts90k, int sliceBytes, const QByteArray &sps, quint64 generation = 1)
{
    RelayUnit u;
    u.codec = VideoTapCodec::H264;
    u.data = makeAccessUnit(key, sliceBytes, sps, makePps());
    u.parameterSets = startCode() + sps + startCode() + makePps();
    u.keyFrame = key;
    u.pts90k = pts90k;
    u.dts90k = pts90k;
    u.receiveSteadyNs = relaySteadyNowNs();
    u.generation = generation;
    return u;
}

// ------------------------------------------------------------------ MP4 boxes

struct Box
{
    QByteArray type;
    qint64 offset = 0;      ///< from the start of the buffer walked
    qint64 size = 0;
    QByteArray payload;     ///< after the 8-byte header
};

inline quint32 be32(const QByteArray &d, qint64 at)
{
    if (at < 0 || at + 4 > d.size()) {
        return 0;
    }
    return (quint32(quint8(d.at(at))) << 24) | (quint32(quint8(d.at(at + 1))) << 16)
           | (quint32(quint8(d.at(at + 2))) << 8) | quint32(quint8(d.at(at + 3)));
}

inline quint64 be64(const QByteArray &d, qint64 at)
{
    return (quint64(be32(d, at)) << 32) | be32(d, at + 4);
}

/// Boxes in `data` from `start`. `ok` false when a size does not fit.
inline QList<Box> walk(const QByteArray &data, qint64 start = 0, bool *ok = nullptr)
{
    QList<Box> out;
    qint64 pos = start;
    bool good = true;
    while (pos + 8 <= data.size()) {
        const qint64 size = be32(data, pos);
        if (size < 8 || pos + size > data.size()) {
            good = false;
            break;
        }
        Box b;
        b.type = data.mid(pos + 4, 4);
        b.offset = pos;
        b.size = size;
        b.payload = data.mid(pos + 8, size - 8);
        out << b;
        pos += size;
    }
    if (pos != data.size()) {
        good = false;
    }
    if (ok) {
        *ok = good;
    }
    return out;
}

inline Box child(const QList<Box> &boxes, const char *type)
{
    for (const Box &b : boxes) {
        if (b.type == type) {
            return b;
        }
    }
    return Box{};
}

struct TrunSample
{
    quint32 duration = 0;
    quint32 size = 0;
    quint32 flags = 0;
    qint32 cts = 0;
};

struct ParsedFragment
{
    bool ok = false;
    quint32 sequence = 0;
    quint64 tfdt = 0;
    qint32 dataOffset = 0;
    qint64 moofSize = 0;
    qint64 mdatPayload = 0;
    QList<TrunSample> samples;
};

/// Sample defaults from an init segment's trex (moov/mvex/trex).
struct TrexDefaults
{
    quint32 duration = 0;
    quint32 size = 0;
    quint32 flags = 0;
};

inline TrexDefaults trexDefaults(const QByteArray &init)
{
    TrexDefaults d;
    const Box moov = child(walk(init), "moov");
    const Box trex = child(walk(child(walk(moov.payload), "mvex").payload), "trex");
    if (trex.payload.size() >= 24) {
        d.duration = be32(trex.payload, 12);
        d.size = be32(trex.payload, 16);
        d.flags = be32(trex.payload, 20);
    }
    return d;
}

/// True when the sample flags mark a sync sample (sample_is_non_sync_sample clear).
inline bool isSyncFlags(quint32 flags) { return (flags & 0x00010000) == 0; }

/// moof + mdat, with every sample's effective duration, size and flags
/// (trun values, else the first-sample flags, else tfhd defaults, else trex).
inline ParsedFragment parseFragment(const QByteArray &frag, const QByteArray &init = QByteArray())
{
    ParsedFragment p;
    bool ok = false;
    const QList<Box> top = walk(frag, 0, &ok);
    if (!ok || top.size() != 2 || top[0].type != "moof" || top[1].type != "mdat") {
        return p;
    }
    p.moofSize = top[0].size;
    p.mdatPayload = top[1].size - 8;
    const QList<Box> moof = walk(top[0].payload);
    const Box mfhd = child(moof, "mfhd");
    p.sequence = be32(mfhd.payload, 4);
    const QList<Box> traf = walk(child(moof, "traf").payload);
    const Box tfhd = child(traf, "tfhd");
    const Box tfdt = child(traf, "tfdt");
    const Box trun = child(traf, "trun");
    if (tfhd.type.isEmpty() || tfdt.type.isEmpty() || trun.type.isEmpty()) {
        return p;
    }
    TrexDefaults d = init.isEmpty() ? TrexDefaults{} : trexDefaults(init);
    const quint32 tf = be32(tfhd.payload, 0) & 0xffffff;
    qint64 h = 8;               // version and flags, track_ID
    if (tf & 0x1) h += 8;       // base_data_offset
    if (tf & 0x2) h += 4;       // sample_description_index
    if (tf & 0x8)  { d.duration = be32(tfhd.payload, h); h += 4; }
    if (tf & 0x10) { d.size = be32(tfhd.payload, h); h += 4; }
    if (tf & 0x20) { d.flags = be32(tfhd.payload, h); h += 4; }

    if (quint8(tfdt.payload.at(0)) == 1) {
        p.tfdt = be64(tfdt.payload, 4);
    } else {
        p.tfdt = be32(tfdt.payload, 4);
    }
    const quint32 flags = be32(trun.payload, 0) & 0xffffff;
    const quint32 count = be32(trun.payload, 4);
    qint64 at = 8;
    if (flags & 0x1) {
        p.dataOffset = qint32(be32(trun.payload, at));
        at += 4;
    }
    bool haveFirst = false;
    quint32 firstFlags = 0;
    if (flags & 0x4) {
        firstFlags = be32(trun.payload, at);
        haveFirst = true;
        at += 4;
    }
    for (quint32 i = 0; i < count; ++i) {
        TrunSample s;
        s.duration = d.duration;
        s.size = d.size;
        s.flags = (i == 0 && haveFirst) ? firstFlags : d.flags;
        if (flags & 0x100) { s.duration = be32(trun.payload, at); at += 4; }
        if (flags & 0x200) { s.size = be32(trun.payload, at); at += 4; }
        if (flags & 0x400) { s.flags = be32(trun.payload, at); at += 4; }
        if (flags & 0x800) { s.cts = qint32(be32(trun.payload, at)); at += 4; }
        p.samples << s;
    }
    p.ok = at == trun.payload.size();
    return p;
}

// ------------------------------------------------------------------ S20 mock

struct MockRequest
{
    QByteArray method;
    QByteArray target;          ///< path and query, as sent
    QHash<QByteArray, QByteArray> headers;     ///< lowercase names
    QByteArray body;
    int connection = 0;
    qint64 atMs = 0;

    QByteArray path() const { const qsizetype q = target.indexOf('?'); return q < 0 ? target : target.left(q); }
    int seq() const
    {
        const qsizetype q = target.indexOf("seq=");
        return q < 0 ? -1 : target.mid(q + 4).toInt();
    }
};

struct MockResponse
{
    int status = 204;
    QByteArray body;
    QList<QPair<QByteArray, QByteArray>> headers;
    int delayMs = 0;
};

/// Speaks the S20 routes as the Fleet Server does (RotorLab's GC connector
/// API, "Live video"): POST /live 200 {"live_id"}, 415 for a
/// codec other than H.264 and 400 for fps outside 1 to 240 or a bad
/// flight_ref; PATCH {"flight_ref"} 200 {"ok": true}, 400 for a bad
/// ref; init, frag and DELETE 200 {"ok": true}; 409 for a
/// fragment before init or a seq that did not count up; 404 once a stream has
/// ended. Over HTTP/1.1 keep-alive (or TLS with a test
/// certificate), records every request, and lets a test force answers.
class MockFleetServer : public QObject
{
public:
    explicit MockFleetServer(bool tls = false) : _tls(tls) {}

    QTcpServer *server = nullptr;
    QList<MockRequest> requests;
    QByteArray rawReceived;     ///< every application byte, all connections
    int outstanding = 0;
    int maxOutstanding = 0;
    int connections = 0;
    int fragDelayMs = 0;        ///< delay on every fragment answer
    /// Return true and fill `out` to override the S20 behavior for one request.
    std::function<bool(const MockRequest &, MockResponse &)> override;

    bool listen(const QSslCertificate &cert = QSslCertificate(), const QSslKey &key = QSslKey())
    {
        if (_tls) {
            auto *s = new QSslServer(this);
            QSslConfiguration cfg = QSslConfiguration::defaultConfiguration();
            cfg.setLocalCertificate(cert);
            cfg.setPrivateKey(key);
            cfg.setPeerVerifyMode(QSslSocket::VerifyNone);
            s->setSslConfiguration(cfg);
            QObject::connect(s, &QSslServer::startedEncryptionHandshake, this,
                             [this](QSslSocket *sock) { _watch(sock); });
            QObject::connect(s, &QTcpServer::pendingConnectionAvailable, this, [s]() {
                while (s->hasPendingConnections()) {
                    s->nextPendingConnection();
                }
            });
            server = s;
        } else {
            server = new QTcpServer(this);
            QObject::connect(server, &QTcpServer::pendingConnectionAvailable, this, [this]() {
                while (server->hasPendingConnections()) {
                    _watch(server->nextPendingConnection());
                }
            });
        }
        _clock.start();
        return server->listen(QHostAddress::LocalHost, 0);
    }

    QString url() const
    {
        return QStringLiteral("%1://127.0.0.1:%2")
            .arg(_tls ? QStringLiteral("https") : QStringLiteral("http"))
            .arg(server->serverPort());
    }

    QList<MockRequest> fragments() const
    {
        QList<MockRequest> out;
        for (const MockRequest &r : requests) {
            if (r.method == "POST" && r.target.contains("/frag?")) {
                out << r;
            }
        }
        return out;
    }

    int count(const QByteArray &method, const QByteArray &contains) const
    {
        int n = 0;
        for (const MockRequest &r : requests) {
            if (r.method == method && r.target.contains(contains)) {
                ++n;
            }
        }
        return n;
    }

    QStringList liveIds() const { return _liveIds; }
    int liveCount() const { return int(_liveIds.size()); }
    int fragCount() const { return int(fragments().size()); }

    /// The flight each stream is named with now (POST /live, then PATCH).
    QHash<QString, QString> flightRefs;

    /// The server's rule for a flight ref: empty, or [A-Za-z0-9_-]{8,64}.
    static bool validRef(const QString &ref)
    {
        static const QRegularExpression ok(QStringLiteral("^[A-Za-z0-9_-]{8,64}$"));
        return ref.isEmpty() || ok.match(ref).hasMatch();
    }

private:
    static QByteArray header(const QByteArray &head, const QByteArray &name)
    {
        for (const QByteArray &line : head.split('\n')) {
            const qsizetype colon = line.indexOf(':');
            if (colon > 0 && line.left(colon).trimmed().toLower() == name) {
                return line.mid(colon + 1).trimmed();
            }
        }
        return {};
    }

    void _watch(QTcpSocket *s)
    {
        const int conn = ++connections;
        auto buf = std::make_shared<QByteArray>();
        QObject::connect(s, &QTcpSocket::readyRead, this, [this, s, buf, conn]() {
            const QByteArray chunk = s->readAll();
            rawReceived += chunk;
            *buf += chunk;
            while (true) {
                const qsizetype he = buf->indexOf("\r\n\r\n");
                if (he < 0) {
                    return;
                }
                const QByteArray head = buf->left(he);
                const qint64 len = header(head, "content-length").toLongLong();
                if (buf->size() < he + 4 + len) {
                    return;
                }
                MockRequest r;
                const QList<QByteArray> first = head.left(head.indexOf("\r\n")).split(' ');
                r.method = first.value(0);
                r.target = first.value(1);
                for (const QByteArray &line : head.split('\n').mid(1)) {
                    const qsizetype colon = line.indexOf(':');
                    if (colon > 0) {
                        r.headers.insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed());
                    }
                }
                r.body = buf->mid(he + 4, len);
                r.connection = conn;
                r.atMs = _clock.elapsed();
                buf->remove(0, he + 4 + len);
                requests << r;
                ++outstanding;
                maxOutstanding = qMax(maxOutstanding, outstanding);
                MockResponse resp = _answer(r);
                QPointer<QTcpSocket> sp(s);
                const auto send = [this, sp, resp]() {
                    --outstanding;
                    if (!sp) {
                        return;
                    }
                    QByteArray out = "HTTP/1.1 " + QByteArray::number(resp.status) + " X\r\n";
                    for (const auto &h : resp.headers) {
                        out += h.first + ": " + h.second + "\r\n";
                    }
                    if (!resp.body.isEmpty()) {
                        out += "Content-Type: application/json\r\n";
                    }
                    out += "Content-Length: " + QByteArray::number(resp.body.size()) + "\r\n\r\n" + resp.body;
                    sp->write(out);
                    sp->flush();
                };
                if (resp.delayMs > 0) {
                    QTimer::singleShot(resp.delayMs, this, send);
                } else {
                    send();
                }
            }
        });
    }

    MockResponse _answer(const MockRequest &r)
    {
        MockResponse out;
        if (override && override(r, out)) {
            return out;
        }
        const QByteArray path = r.path();
        if (r.method == "POST" && path == "/api/gc/v1/live") {
            // S20: H.264 only.
            if (!r.body.contains("\"avc1")) {
                out.status = 415;
                out.body = "{\"error\":\"Live video is H.264 only.\"}";
                return out;
            }
            const QJsonObject start = QJsonDocument::fromJson(r.body).object();
            const double fps = start.value(QStringLiteral("fps")).toDouble(-1);
            if (fps < 1 || fps > 240) {
                out.status = 400;
                out.body = "{\"error\":\"fps is a number from 1 to 240.\"}";
                return out;
            }
            const QString ref = start.value(QStringLiteral("flight_ref")).toString();
            if (!validRef(ref)) {
                out.status = 400;
                out.body = "{\"error\":\"flight_ref is empty or a flight's ref.\"}";
                return out;
            }
            const QString id = QStringLiteral("L%1").arg(++_liveCounter);
            _liveIds << id;
            _lastSeq.insert(id, -1);
            flightRefs.insert(id, ref);
            out.status = 200;
            out.body = "{\"live_id\":\"" + id.toLatin1() + "\"}";
            return out;
        }
        const QByteArray prefix = "/api/gc/v1/live/";
        if (!path.startsWith(prefix)) {
            out.status = 404;
            return out;
        }
        QByteArray rest = path.mid(prefix.size());
        const qsizetype slash = rest.indexOf('/');
        const QString id = QString::fromLatin1(slash < 0 ? rest : rest.left(slash));
        const QByteArray tail = slash < 0 ? QByteArray() : rest.mid(slash);
        if (!_lastSeq.contains(id)) {
            out.status = 404;
            return out;
        }
        if (r.method == "PUT" && tail == "/init") {
            _initDone.insert(id, true);
            out.status = 200;
            out.body = "{\"ok\":true}";
        } else if (r.method == "POST" && tail == "/frag") {
            const int seq = r.seq();
            if (!_initDone.value(id) || seq <= _lastSeq.value(id)) {
                out.status = 409;   // no init yet, or seq did not count up
            } else {
                _lastSeq[id] = seq;
                out.status = 200;
                out.body = "{\"ok\":true}";
            }
            out.delayMs = fragDelayMs;
        } else if (r.method == "PATCH" && tail.isEmpty()) {
            // Name the flight on the same stream; "" once it has landed.
            const QJsonValue v = QJsonDocument::fromJson(r.body).object().value(QStringLiteral("flight_ref"));
            if (!v.isString() || !validRef(v.toString())) {
                out.status = 400;
                out.body = "{\"error\":\"Send {\\\"flight_ref\\\": \\\"...\\\"}, a flight's ref, or empty when it has landed.\"}";
            } else {
                flightRefs.insert(id, v.toString());
                out.status = 200;
                out.body = "{\"ok\":true}";
            }
        } else if (r.method == "DELETE" && tail.isEmpty()) {
            _lastSeq.remove(id);
            out.status = 200;
            out.body = "{\"ok\":true}";
        } else {
            out.status = 404;
        }
        return out;
    }

    bool _tls;
    QElapsedTimer _clock;
    int _liveCounter = 0;
    QStringList _liveIds;
    QHash<QString, int> _lastSeq;
    QHash<QString, bool> _initDone;
};

} // namespace RelayTest
