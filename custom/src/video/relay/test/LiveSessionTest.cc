#include "LiveSessionTest.h"

#include "BitrateGuard.h"
#include "FleetLiveTransport.h"
#include "LiveSession.h"
#include "RelayPipeline.h"
#include "RelayTestSupport.h"
#include "VideoRelay.h"

#include "fleet/FleetConnector.h"
#include "fleet/FleetStore.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>
#include <QtTest/QTest>

#include <memory>

using namespace RelayTest;

namespace {

const QString kToken = QStringLiteral("rlgs_relaytest_9f2c41d07be3");

LiveFragment frag(bool key, quint64 initId = 1, qint64 duration90k = 90000, int bytes = 2000)
{
    LiveFragment f;
    f.bytes = QByteArray(bytes, key ? 'K' : 'n');
    f.initId = initId;
    f.init = QByteArray("INIT-") + QByteArray::number(initId) + QByteArray(100, 'i');
    f.format.codec = QStringLiteral("avc1.42c01f");
    f.format.width = 640;
    f.format.height = 360;
    f.format.fps = 15;
    f.keyframeStart = key;
    f.duration90k = duration90k;
    f.samples = 15;
    f.lastReceiveSteadyNs = relaySteadyNowNs();
    return f;
}

struct Rig
{
    std::unique_ptr<MockFleetServer> server;
    std::unique_ptr<FleetLiveTransport> transport;
    std::unique_ptr<LiveSession> session;

    explicit Rig(bool tls = false) : server(std::make_unique<MockFleetServer>(tls)) {}

    bool startPlain()
    {
        if (!server->listen()) {
            return false;
        }
        FleetStore::setBaseUrl(server->url());
        FleetStore::setToken(kToken);
        FleetStore::setServerPin({});
        _make();
        return true;
    }

    void _make()
    {
        transport = std::make_unique<FleetLiveTransport>();
        session = std::make_unique<LiveSession>(transport.get());
        session->setBackoffScheduleMs({50, 100, 200});
        session->setQualityKey(QStringLiteral("low"));
        session->start();
    }
};

QString findOpenssl()
{
    QString exe = QStandardPaths::findExecutable(QStringLiteral("openssl"));
    if (!exe.isEmpty()) {
        return exe;
    }
    for (const QString &c : {QStringLiteral("C:/Program Files/Git/usr/bin/openssl.exe"),
                             QStringLiteral("C:/Program Files/Git/mingw64/bin/openssl.exe")}) {
        if (QFile::exists(c)) {
            return c;
        }
    }
    return {};
}

bool runOpenssl(const QString &ossl, const QStringList &args)
{
    QProcess p;
    p.start(ossl, args);
    return p.waitForFinished(60000) && p.exitCode() == 0;
}

} // namespace

void LiveSessionTest::init()
{
    UnitTest::init();
    FleetStore::clearAll();
}

void LiveSessionTest::cleanup()
{
    FleetStore::clearAll();
    UnitTest::cleanup();
}

void LiveSessionTest::_orderAndSeqOneAtATime()
{
    Rig rig;
    QVERIFY(rig.startPlain());
    rig.server->fragDelayMs = 30;
    rig.session->push(frag(true));
    for (int i = 0; i < 2; ++i) {
        rig.session->push(frag(false, 1, 30000));
    }
    QTRY_COMPARE_WITH_TIMEOUT(rig.server->fragCount(), 3, 5000);
    for (int i = 0; i < 2; ++i) {
        rig.session->push(frag(false, 1, 30000));
    }
    QTRY_COMPARE_WITH_TIMEOUT(rig.server->fragCount(), 5, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(rig.session->sentFragments(), quint64(5), 5000);

    const QList<MockRequest> &reqs = rig.server->requests;
    QCOMPARE(reqs[0].method, QByteArray("POST"));
    QCOMPARE(reqs[0].target, QByteArray("/api/gc/v1/live"));
    QCOMPARE(reqs[1].method, QByteArray("PUT"));
    QCOMPARE(reqs[1].target, QByteArray("/api/gc/v1/live/L1/init"));
    const QList<MockRequest> frags = rig.server->fragments();
    for (int i = 0; i < frags.size(); ++i) {
        QCOMPARE(frags[i].seq(), i);
        QVERIFY(frags[i].path() == "/api/gc/v1/live/L1/frag");
        QCOMPARE(frags[i].headers.value("content-type"), QByteArray("application/octet-stream"));
    }
    QCOMPARE(frags[0].body, frag(true).bytes);
    QCOMPARE(rig.server->maxOutstanding, 1);
    QCOMPARE(rig.session->state(), LiveSession::State::Live);

    // The POST /live body describes the stream.
    const QJsonObject body = QJsonDocument::fromJson(reqs[0].body).object();
    QCOMPARE(body.value(QStringLiteral("camera")).toString(), QStringLiteral("main"));
    QCOMPARE(body.value(QStringLiteral("quality")).toString(), QStringLiteral("low"));
    QCOMPARE(body.value(QStringLiteral("codec")).toString(), QStringLiteral("avc1.42c01f"));
    QCOMPARE(body.value(QStringLiteral("width")).toInt(), 640);
    QCOMPARE(body.value(QStringLiteral("height")).toInt(), 360);
    QVERIFY(body.contains(QStringLiteral("flight_ref")));
}

void LiveSessionTest::_initFirstAndAfterChange()
{
    Rig rig;
    QVERIFY(rig.startPlain());
    // Short fragments, so the four together stay under the 3 s backlog.
    rig.session->push(frag(true, 1, 30000));
    rig.session->push(frag(false, 1, 30000));
    rig.session->push(frag(true, 2, 30000));
    rig.session->push(frag(false, 2, 30000));
    QTRY_COMPARE_WITH_TIMEOUT(rig.server->fragCount(), 4, 5000);
    QStringList order;
    for (const MockRequest &r : rig.server->requests) {
        QByteArray what = r.target;
        if (r.target.contains("/init")) {
            what = QByteArray("init");
        } else if (r.target.contains("/frag")) {
            what = QByteArray("frag");
        }
        order << QString::fromLatin1(r.method + QByteArray(" ") + what);
    }
    QCOMPARE(order, QStringList({QStringLiteral("POST /api/gc/v1/live"), QStringLiteral("PUT init"),
                                 QStringLiteral("POST frag"), QStringLiteral("POST frag"),
                                 QStringLiteral("PUT init"), QStringLiteral("POST frag"), QStringLiteral("POST frag")}));
    QVERIFY(rig.server->requests[1].body.startsWith("INIT-1"));
    QVERIFY(rig.server->requests[4].body.startsWith("INIT-2"));
}

void LiveSessionTest::_backlogDropsToNextKeyframe()
{
    Rig rig;
    QVERIFY(rig.startPlain());
    rig.server->fragDelayMs = 400;
    rig.session->push(frag(true));
    QTRY_COMPARE_WITH_TIMEOUT(rig.server->fragCount(), 1, 5000);
    // While seq 0 is on the wire: n n K n n. Past 3 s of media the two before
    // the keyframe are dropped; seq skips over them.
    rig.session->push(frag(false));
    rig.session->push(frag(false));
    rig.session->push(frag(true));
    rig.session->push(frag(false));
    QCOMPARE(rig.session->droppedFragments(), quint64(2));
    QCOMPARE(rig.session->backlogFragments(), 2);
    rig.session->push(frag(false));
    QCOMPARE(rig.session->backlogFragments(), 3);
    QTRY_COMPARE_WITH_TIMEOUT(rig.server->fragCount(), 4, 10000);
    const QList<MockRequest> f = rig.server->fragments();
    QCOMPARE(f[0].seq(), 0);
    QCOMPARE(f[1].seq(), 3);
    QCOMPARE(f[2].seq(), 4);
    QCOMPARE(f[3].seq(), 5);
    QCOMPARE(f[1].body, frag(true).bytes);
    QCOMPARE(rig.server->count("POST", "/api/gc/v1/live/"), 4);   // no 409, same session

    // A backlog with no keyframe after its head is dropped whole; nothing more
    // is queued until a fragment that starts with a keyframe.
    rig.server->fragDelayMs = 600;
    rig.session->push(frag(true));
    QTRY_COMPARE_WITH_TIMEOUT(rig.server->fragCount(), 5, 5000);
    for (int i = 0; i < 4; ++i) {
        rig.session->push(frag(false));
    }
    QCOMPARE(rig.session->backlogFragments(), 0);
    rig.session->push(frag(false));
    QCOMPARE(rig.session->backlogFragments(), 0);
    rig.session->push(frag(true));
    QCOMPARE(rig.session->backlogFragments(), 1);
}

void LiveSessionTest::_backoffSchedule()
{
    Rig rig;
    int refusals = 0;
    rig.server->override = [&refusals](const MockRequest &r, MockResponse &out) {
        if (r.method == "POST" && r.target == "/api/gc/v1/live" && refusals < 3) {
            ++refusals;
            out.status = 503;
            return true;
        }
        return false;
    };
    QVERIFY(rig.startPlain());
    rig.session->push(frag(true));
    QTRY_COMPARE_WITH_TIMEOUT(rig.session->state(), LiveSession::State::Paused, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(rig.server->fragCount(), 1, 5000);
    QList<qint64> opens;
    for (const MockRequest &r : rig.server->requests) {
        if (r.target == "/api/gc/v1/live") {
            opens << r.atMs;
        }
    }
    QCOMPARE(int(opens.size()), 4);
    // 50, 100, 200 ms between the attempts (the test schedule), never sooner.
    QVERIFY2(opens[1] - opens[0] >= 45, qPrintable(QString::number(opens[1] - opens[0])));
    QVERIFY2(opens[2] - opens[1] >= 95, qPrintable(QString::number(opens[2] - opens[1])));
    QVERIFY2(opens[3] - opens[2] >= 195, qPrintable(QString::number(opens[3] - opens[2])));
    QCOMPARE(rig.session->state(), LiveSession::State::Live);
    QCOMPARE(LiveSession::revokedText(),
             QStringLiteral("Live relay stopped: this ground station was removed from the Fleet Server."));
}

void LiveSessionTest::_conflictStartsNewSession()
{
    Rig rig;
    rig.server->override = [](const MockRequest &r, MockResponse &out) {
        if (r.path() == "/api/gc/v1/live/L1/frag" && r.seq() == 1) {
            out.status = 409;
            return true;
        }
        return false;
    };
    QVERIFY(rig.startPlain());
    rig.server->fragDelayMs = 20;
    rig.session->push(frag(true, 1, 30000));
    rig.session->push(frag(false, 1, 30000));
    rig.session->push(frag(false, 1, 30000));     // dropped by the new session: not a keyframe
    rig.session->push(frag(true, 1, 30000));
    rig.session->push(frag(false, 1, 30000));
    QTRY_COMPARE_WITH_TIMEOUT(rig.server->liveCount(), 2, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(rig.server->count("POST", "/api/gc/v1/live/L2/frag"), 2, 5000);
    const QList<MockRequest> f = rig.server->fragments();
    QList<int> l2;
    for (const MockRequest &r : f) {
        if (r.path() == "/api/gc/v1/live/L2/frag") {
            l2 << r.seq();
        }
    }
    QCOMPARE(l2, QList<int>({0, 1}));      // seq from 0 on the new live_id
    QCOMPARE(rig.server->count("PUT", "/api/gc/v1/live/L2/init"), 1);  // init again
    // The new session starts at a keyframe.
    for (const MockRequest &r : f) {
        if (r.path() == "/api/gc/v1/live/L2/frag" && r.seq() == 0) {
            QCOMPARE(r.body, frag(true).bytes);
        }
    }
}

void LiveSessionTest::_unsupportedMediaStops()
{
    // The server refuses anything but H.264 on POST /live with 415.
    Rig rig;
    QVERIFY(rig.startPlain());
    LiveFragment f = frag(true);
    f.format.codec = QStringLiteral("hvc1.1.6.L93.B0");
    rig.session->push(f);
    QTRY_COMPARE_WITH_TIMEOUT(rig.session->state(), LiveSession::State::Stopped, 5000);
    QCOMPARE(rig.session->stopReason(), LiveSession::h265Text());
    QCOMPARE(rig.session->stopReason(), QStringLiteral("Live relay needs an H.264 camera on this computer."));
    QCOMPARE(rig.server->liveCount(), 0);
    QCOMPARE(rig.server->count("DELETE", "/live"), 0);     // no stream was opened
    const int before = int(rig.server->requests.size());
    rig.session->push(frag(true));
    QTest::qWait(200);
    QCOMPARE(int(rig.server->requests.size()), before);
}

void LiveSessionTest::_endedStreamStartsNewSession()
{
    // 404 on a fragment: the server ended the stream (ten seconds idle, or a
    // second POST /live for the camera). A new session, init again, seq from 0.
    Rig rig;
    rig.server->override = [](const MockRequest &r, MockResponse &out) {
        if (r.path() == "/api/gc/v1/live/L1/frag" && r.seq() == 1) {
            out.status = 404;
            return true;
        }
        return false;
    };
    QVERIFY(rig.startPlain());
    rig.session->push(frag(true, 1, 30000));
    rig.session->push(frag(false, 1, 30000));
    rig.session->push(frag(true, 1, 30000));
    QTRY_COMPARE_WITH_TIMEOUT(rig.server->count("POST", "/api/gc/v1/live/L2/frag"), 1, 5000);
    QCOMPARE(rig.server->count("PUT", "/api/gc/v1/live/L2/init"), 1);
    QCOMPARE(rig.server->fragments().last().seq(), 0);
    QCOMPARE(rig.session->state(), LiveSession::State::Live);
}

void LiveSessionTest::_tooLargeContinues()
{
    Rig rig;
    rig.server->override = [](const MockRequest &r, MockResponse &out) {
        if (r.target.contains("/frag") && r.seq() == 1) {
            out.status = 413;
            return true;
        }
        return false;
    };
    QVERIFY(rig.startPlain());
    rig.session->push(frag(true));
    rig.session->push(frag(false));
    rig.session->push(frag(false));
    QTRY_COMPARE_WITH_TIMEOUT(rig.server->fragCount(), 3, 5000);
    QCOMPARE(rig.server->fragments()[2].seq(), 2);
    QCOMPARE(rig.server->liveCount(), 1);
    QCOMPARE(rig.session->state(), LiveSession::State::Live);
}

void LiveSessionTest::_badRequestNewSessionOnceThenStop()
{
    Rig rig;
    rig.server->override = [](const MockRequest &r, MockResponse &out) {
        if (r.target.contains("/frag")) {
            out.status = 400;
            out.body = "{\"error\":\"moof: track id 2 is not track 1\"}";
            return true;
        }
        return false;
    };
    QVERIFY(rig.startPlain());
    rig.session->push(frag(true));
    rig.session->push(frag(true));
    QTRY_COMPARE_WITH_TIMEOUT(rig.session->state(), LiveSession::State::Stopped, 5000);
    QCOMPARE(rig.server->liveCount(), 2);
    QCOMPARE(rig.session->stopReason(), QStringLiteral("Live relay stopped: moof: track id 2 is not track 1"));
}

void LiveSessionTest::_unauthorizedStopsAndClears()
{
    Rig rig;
    rig.server->override = [](const MockRequest &r, MockResponse &out) {
        if (r.target.contains("/frag") && r.seq() == 1) {
            out.status = 401;
            return true;
        }
        return false;
    };
    QVERIFY(rig.startPlain());
    rig.server->fragDelayMs = 50;
    rig.session->push(frag(true, 1, 30000));
    rig.session->push(frag(false, 1, 30000));
    rig.session->push(frag(false, 1, 30000));
    rig.session->push(frag(false, 1, 30000));
    QTRY_COMPARE_WITH_TIMEOUT(rig.session->state(), LiveSession::State::Revoked, 5000);
    QCOMPARE(rig.session->backlogFragments(), 0);
    QCOMPARE(rig.session->stopReason(), LiveSession::revokedText());
    QTest::qWait(300);
    QCOMPARE(rig.server->count("DELETE", "/live"), 0);  // the token no longer works
    const int before = int(rig.server->requests.size());
    rig.session->push(frag(true));
    QTest::qWait(200);
    QCOMPARE(int(rig.server->requests.size()), before);
}

void LiveSessionTest::_revokedMidStreamAborts()
{
    Rig rig;
    QVERIFY(rig.startPlain());
    rig.server->fragDelayMs = 2000;
    rig.session->push(frag(true));
    rig.session->push(frag(false));
    QTRY_COMPARE_WITH_TIMEOUT(rig.server->fragCount(), 1, 5000);
    // The Fleet Connector goes to Revoked while seq 0 is on the wire.
    rig.session->revoke();
    QCOMPARE(rig.session->state(), LiveSession::State::Revoked);
    QCOMPARE(rig.session->backlogFragments(), 0);
    QTest::qWait(2500);     // the late answer is dropped, nothing more goes out
    QCOMPARE(rig.server->fragCount(), 1);
    QCOMPARE(rig.server->count("DELETE", "/live"), 0);
    QCOMPARE(rig.session->sentFragments(), quint64(0));
    QCOMPARE(rig.session->state(), LiveSession::State::Revoked);
}

void LiveSessionTest::_stopSendsDelete()
{
    Rig rig;
    QVERIFY(rig.startPlain());
    rig.session->push(frag(true));
    QTRY_COMPARE_WITH_TIMEOUT(rig.session->sentFragments(), quint64(1), 5000);
    rig.session->stop();
    QCOMPARE(rig.session->state(), LiveSession::State::Idle);
    if (!FleetLiveTransport::canDelete()) {
        // HttpClient has no DELETE yet: the server ends the stream after ten idle seconds.
        QTest::qWait(300);
        QCOMPARE(rig.server->count("DELETE", "/live"), 0);
        QSKIP("no DELETE in this build's HttpClient");
    }
    QTRY_COMPARE_WITH_TIMEOUT(rig.server->count("DELETE", "/api/gc/v1/live/L1"), 1, 5000);
}

void LiveSessionTest::_bearerOnlyInHeader()
{
    Rig rig;
    QVERIFY(rig.startPlain());
    rig.session->push(frag(true));
    rig.session->push(frag(false));
    QTRY_COMPARE_WITH_TIMEOUT(rig.server->fragCount(), 2, 5000);
    rig.session->stop();
    if (FleetLiveTransport::canDelete()) {
        QTRY_COMPARE_WITH_TIMEOUT(rig.server->count("DELETE", "/live"), 1, 5000);
    }
    const QByteArray token = kToken.toLatin1();
    for (const MockRequest &r : rig.server->requests) {
        QCOMPARE(r.headers.value("authorization"), QByteArray("Bearer ") + token);
        QVERIFY(!r.target.contains(token));
        QVERIFY(!r.body.contains(token));
    }
    // Exactly once per request on the wire: in its Authorization header.
    QCOMPARE(int(rig.server->rawReceived.count(token)), int(rig.server->requests.size()));
}

void LiveSessionTest::_pinMismatchSendsNothing()
{
    if (!QSslSocket::supportsSsl()) {
        QSKIP("no TLS backend");
    }
    const QString ossl = findOpenssl();
    if (ossl.isEmpty()) {
        QSKIP("openssl not found; it is needed to make the test certificate");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString keyPath = dir.path() + QStringLiteral("/key.pem");
    const QString certPath = dir.path() + QStringLiteral("/cert.pem");
    QVERIFY(runOpenssl(ossl, {QStringLiteral("req"), QStringLiteral("-x509"), QStringLiteral("-newkey"),
                              QStringLiteral("rsa:2048"), QStringLiteral("-nodes"), QStringLiteral("-keyout"), keyPath,
                              QStringLiteral("-out"), certPath, QStringLiteral("-days"), QStringLiteral("1"),
                              QStringLiteral("-subj"), QStringLiteral("/CN=127.0.0.1")}));
    QFile kf(keyPath), cf(certPath);
    QVERIFY(kf.open(QIODevice::ReadOnly) && cf.open(QIODevice::ReadOnly));
    const QSslKey key(kf.readAll(), QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
    const QSslCertificate cert(cf.readAll(), QSsl::Pem);

    Rig rig(true);
    QVERIFY(rig.server->listen(cert, key));
    FleetStore::setBaseUrl(rig.server->url());
    FleetStore::setToken(kToken);
    FleetStore::setServerPin(QString(64, QLatin1Char('a')));    // not this server's key
    rig._make();
    rig.session->push(frag(true));
    QTRY_COMPARE_WITH_TIMEOUT(rig.session->state(), LiveSession::State::Stopped, 10000);
    QVERIFY(rig.session->stopReason().contains(QStringLiteral("does not match")));
    QTest::qWait(200);
    QVERIFY(rig.server->rawReceived.isEmpty());   // not one application byte
    QVERIFY(rig.server->requests.isEmpty());

    // The right pin goes through.
    Rig good(true);
    QVERIFY(good.server->listen(cert, key));
    FleetStore::setBaseUrl(good.server->url());
    FleetStore::setServerPin(HttpClient::spkiSha256Hex(cert.toDer()));
    good._make();
    good.session->push(frag(true));
    QTRY_COMPARE_WITH_TIMEOUT(good.server->fragCount(), 1, 10000);
}

void LiveSessionTest::_notEnrolledSendsNothing()
{
    Rig rig;
    QVERIFY(rig.server->listen());
    // No token stored, and an http address that is not this machine: neither is used.
    FleetStore::setBaseUrl(rig.server->url());
    rig._make();
    rig.session->push(frag(true));
    QTRY_COMPARE_WITH_TIMEOUT(rig.session->state(), LiveSession::State::Paused, 3000);
    QVERIFY(rig.server->requests.isEmpty());

    QVERIFY(FleetLiveTransport::acceptableBaseUrl(QStringLiteral("http://127.0.0.1:8000")));
    QVERIFY(FleetLiveTransport::acceptableBaseUrl(QStringLiteral("https://fleet.example.test:8443")));
    QVERIFY(!FleetLiveTransport::acceptableBaseUrl(QStringLiteral("http://fleet.example.test")));
    QVERIFY(!FleetLiveTransport::acceptableBaseUrl(QString()));
    QVERIFY(LiveSession::validLiveId(QStringLiteral("L1_ab-9")));
    QVERIFY(!LiveSession::validLiveId(QStringLiteral("../x")));
    QVERIFY(!LiveSession::validLiveId(QStringLiteral("a?b")));
}

void LiveSessionTest::_fleetStateLogic()
{
    using FC = FleetConnector;
    QVERIFY(VideoRelay::fleetAllows(FC::Connected, true, false));
    QVERIFY(VideoRelay::fleetAllows(FC::Unreachable, true, false));   // the session backs off itself
    QVERIFY(VideoRelay::fleetAllows(FC::FirstSync, true, false));
    QVERIFY(!VideoRelay::fleetAllows(FC::Connected, true, true));     // RotorLab Connector is the route
    QVERIFY(!VideoRelay::fleetAllows(FC::Revoked, true, false));
    QVERIFY(!VideoRelay::fleetAllows(FC::NotConnected, false, false));
    QVERIFY(!VideoRelay::fleetAllows(FC::Enrolling, false, false));
    QVERIFY(VideoRelay::fleetRevoked(FC::Revoked));
    QVERIFY(VideoRelay::fleetRevoked(FC::NotConnected));
    QVERIFY(!VideoRelay::fleetRevoked(FC::Unreachable));
    // Only a new enrollment ends a revocation stop.
    QVERIFY(VideoRelay::clearsRevocation(FC::Enrolling, FC::FirstSync));
    QVERIFY(VideoRelay::clearsRevocation(FC::Revoked, FC::Connected));
    QVERIFY(!VideoRelay::clearsRevocation(FC::Connected, FC::Connected));
    QVERIFY(!VideoRelay::clearsRevocation(FC::Unreachable, FC::Connected));

    QCOMPARE(VideoRelay::pausedText(15), QStringLiteral("Paused: Fleet unreachable, retrying in 15 s"));
    QCOMPARE(VideoRelay::forwardingText(QStringLiteral("fleet.local"), QStringLiteral("Medium"), 2.44, 0.42),
             QStringLiteral("Forwarding to fleet.local at Medium, 2.4 Mb/s sent, 0.4 s behind"));
}

void LiveSessionTest::_flightRefPatchedOnSameStream()
{
    const QString ref = QStringLiteral("b1946ac92492d2347c6235b4d2611184");

    Rig rig;
    QVERIFY(rig.startPlain());
    rig.server->fragDelayMs = 20;
    rig.session->push(frag(true, 1, 30000));
    QTRY_COMPARE_WITH_TIMEOUT(rig.server->fragCount(), 1, 5000);
    QCOMPARE(rig.server->flightRefs.value(QStringLiteral("L1")), QString());   // not armed at POST

    // Arm: the same stream is named, nothing restarts.
    rig.session->setFlightRef(ref);
    QTRY_COMPARE_WITH_TIMEOUT(rig.server->count("PATCH", "/api/gc/v1/live/L1"), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(rig.session->serverFlightRef(), ref, 5000);
    QCOMPARE(rig.server->flightRefs.value(QStringLiteral("L1")), ref);
    rig.session->push(frag(false, 1, 30000));
    QTRY_COMPARE_WITH_TIMEOUT(rig.server->fragCount(), 2, 5000);
    rig.session->setFlightRef(ref);     // the same ref again: no second PATCH
    QTest::qWait(150);
    QCOMPARE(rig.server->count("PATCH", "/live"), 1);

    // Landing: "" on the same stream.
    rig.session->setFlightRef(QString());
    QTRY_COMPARE_WITH_TIMEOUT(rig.server->count("PATCH", "/api/gc/v1/live/L1"), 2, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(rig.server->flightRefs.value(QStringLiteral("L1"), QStringLiteral("?")), QString(), 5000);
    // Something the server would refuse is never sent: it means "no flight".
    rig.session->setFlightRef(QStringLiteral("x"));
    rig.session->setFlightRef(QStringLiteral("../../etc"));
    QTest::qWait(150);
    QCOMPARE(rig.server->count("PATCH", "/live"), 2);
    QCOMPARE(rig.server->liveCount(), 1);
    QCOMPARE(rig.session->state(), LiveSession::State::Live);
    QCOMPARE(rig.server->maxOutstanding, 1);

    QStringList bodies;
    for (const MockRequest &r : rig.server->requests) {
        if (r.method == "PATCH") {
            QCOMPARE(r.headers.value("authorization"), QByteArray("Bearer ") + kToken.toLatin1());
            QVERIFY(r.headers.value("content-type").startsWith("application/json"));
            const QJsonObject o = QJsonDocument::fromJson(r.body).object();
            QCOMPARE(o.keys(), QStringList({QStringLiteral("flight_ref")}));
            bodies << o.value(QStringLiteral("flight_ref")).toString();
        }
    }
    QCOMPARE(bodies, QStringList({ref, QString()}));
    QCOMPARE(LiveSession::cleanFlightRef(ref), ref);
    QCOMPARE(LiveSession::cleanFlightRef(QStringLiteral("short")), QString());
    QCOMPARE(LiveSession::cleanFlightRef(QString(65, QLatin1Char('a'))), QString());

    // Armed before the stream opens: the POST carries the ref, no PATCH.
    Rig armed;
    QVERIFY(armed.startPlain());
    armed.session->setFlightRef(ref);
    armed.session->push(frag(true, 1, 30000));
    QTRY_COMPARE_WITH_TIMEOUT(armed.server->fragCount(), 1, 5000);
    const QJsonObject open = QJsonDocument::fromJson(armed.server->requests.first().body).object();
    QCOMPARE(open.value(QStringLiteral("flight_ref")).toString(), ref);
    QCOMPARE(armed.server->count("PATCH", "/live"), 0);
    QCOMPARE(armed.session->serverFlightRef(), ref);
}

void LiveSessionTest::_flightRefPatchRefusedOrEnded()
{
    const QString ref = QStringLiteral("b1946ac92492d2347c6235b4d2611184");

    // 404: the stream had ended. A new session, whose POST names the flight.
    {
        Rig rig;
        rig.server->override = [](const MockRequest &r, MockResponse &out) {
            if (r.method == "PATCH" && r.path() == "/api/gc/v1/live/L1") {
                out.status = 404;
                out.body = "{\"error\":\"That live stream has ended. Start a new one.\"}";
                return true;
            }
            return false;
        };
        QVERIFY(rig.startPlain());
        rig.session->push(frag(true, 1, 30000));
        QTRY_COMPARE_WITH_TIMEOUT(rig.server->fragCount(), 1, 5000);
        rig.session->setFlightRef(ref);
        QTRY_COMPARE_WITH_TIMEOUT(rig.server->count("PATCH", "/live"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(rig.session->state(), LiveSession::State::Waiting, 5000);
        rig.session->push(frag(true, 1, 30000));
        QTRY_COMPARE_WITH_TIMEOUT(rig.server->count("POST", "/api/gc/v1/live/L2/frag"), 1, 5000);
        QCOMPARE(rig.server->flightRefs.value(QStringLiteral("L2")), ref);
        QCOMPARE(rig.server->count("PATCH", "/live"), 1);
    }

    // 400: logged, not sent again for that value, and the video carries on.
    {
        Rig rig;
        rig.server->override = [](const MockRequest &r, MockResponse &out) {
            if (r.method == "PATCH") {
                out.status = 400;
                out.body = "{\"error\":\"Send a flight's ref.\"}";
                return true;
            }
            return false;
        };
        QVERIFY(rig.startPlain());
        rig.session->push(frag(true, 1, 30000));
        QTRY_COMPARE_WITH_TIMEOUT(rig.server->fragCount(), 1, 5000);
        rig.session->setFlightRef(ref);
        QTRY_COMPARE_WITH_TIMEOUT(rig.server->count("PATCH", "/live"), 1, 5000);
        rig.session->push(frag(false, 1, 30000));
        rig.session->push(frag(false, 1, 30000));
        QTRY_COMPARE_WITH_TIMEOUT(rig.server->fragCount(), 3, 5000);
        QTest::qWait(150);
        QCOMPARE(rig.server->count("PATCH", "/live"), 1);
        QCOMPARE(rig.server->liveCount(), 1);
        QCOMPARE(rig.session->state(), LiveSession::State::Live);
    }
}

void LiveSessionTest::_fragmentSentFeedsGuard()
{
    // The guard measures what goes on the wire: each fragment's bytes and media time.
    Rig rig;
    QVERIFY(rig.startPlain());
    QList<QPair<qint64, qint64>> sent;
    BitrateGuard guard;
    guard.setCap(VideoRelay::guardCap(RelayQuality::Low));
    QList<BitrateGuard::State> states;
    QObject::connect(rig.session.get(), &LiveSession::fragmentSent, rig.session.get(),
                     [&](qint64 bytes, qint64 duration90k) {
        sent << qMakePair(bytes, duration90k);
        if (guard.onSent(bytes, duration90k)) {
            states << guard.state();
        }
    });
    // 1 s fragments at 0.78 Mb/s (the scene measurement): under the cap, Normal.
    for (int i = 0; i < 3; ++i) {
        rig.session->push(frag(true, 1, 90000, 97500));
        QTRY_COMPARE_WITH_TIMEOUT(rig.server->fragCount(), i + 1, 5000);
    }
    QTRY_COMPARE_WITH_TIMEOUT(int(sent.size()), 3, 5000);
    QCOMPARE(sent.first().first, qint64(97500));
    QCOMPARE(sent.first().second, qint64(90000));
    QCOMPARE(guard.state(), BitrateGuard::State::Normal);
    QVERIFY(qAbs(guard.lastSentBps() - 780000.0) < 1.0);
    // 1.76 Mb/s (noise at Low): over the cap within one window, keyframes only.
    for (int i = 0; i < 2; ++i) {
        rig.session->push(frag(true, 1, 90000, 220000));
        QTRY_COMPARE_WITH_TIMEOUT(rig.server->fragCount(), 4 + i, 5000);
    }
    QTRY_COMPARE_WITH_TIMEOUT(int(states.size()), 1, 5000);
    QCOMPARE(states.first(), BitrateGuard::State::KeyframesOnly);
}

void LiveSessionTest::_guardTexts()
{
    QCOMPARE(VideoRelay::guardPausedText(QStringLiteral("Low")),
             QStringLiteral("Video forwarding paused: the picture needs more bandwidth than Low allows"));
    QCOMPARE(VideoRelay::guardPausedText(QStringLiteral("Medium")),
             QStringLiteral("Video forwarding paused: the picture needs more bandwidth than Medium allows"));
    QCOMPARE(VideoRelay::guardKeyframesText(QStringLiteral("Low")),
             QStringLiteral("Sending keyframes only: the picture needs more bandwidth than Low allows"));
    QCOMPARE(VideoRelay::guardCap(RelayQuality::Low), qint64(800000));
    QCOMPARE(VideoRelay::guardCap(RelayQuality::Medium), qint64(2500000));
    QCOMPARE(VideoRelay::guardCap(RelayQuality::Full), qint64(0));     // the design gives Full no cap
    QCOMPARE(VideoRelay::guardPass(BitrateGuard::State::Normal), int(RelayPipeline::Pass::All));
    QCOMPARE(VideoRelay::guardPass(BitrateGuard::State::KeyframesOnly), int(RelayPipeline::Pass::KeyframesOnly));
    QCOMPARE(VideoRelay::guardPass(BitrateGuard::State::Paused), int(RelayPipeline::Pass::Nothing));
}

void LiveSessionTest::_qualityChangeKeepsStream()
{
    // Low to Full while a viewer watches: the stream stays open, the new
    // level's init goes out on the same live_id, then its fragments, seq
    // counting on. No DELETE and no second POST /live, so the viewer never
    // sees "The live video has ended".
    Rig rig;
    QVERIFY(rig.startPlain());
    rig.session->push(frag(true, 1, 30000));
    rig.session->push(frag(false, 1, 30000));
    QTRY_COMPARE_WITH_TIMEOUT(rig.server->fragCount(), 2, 5000);
    const QString id = rig.session->liveId();
    QVERIFY(!id.isEmpty());

    rig.session->changeQuality(QStringLiteral("full"));
    QCOMPARE(rig.session->qualityKey(), QStringLiteral("full"));
    QCOMPARE(rig.session->liveId(), id);
    QCOMPARE(rig.session->state(), LiveSession::State::Live);
    const auto full = [](bool key) {
        LiveFragment f = frag(key, 2, 30000, 6000);
        f.init = QByteArray("INIT-2-FULL") + QByteArray(100, 'f');
        f.format.codec = QStringLiteral("avc1.640028");
        f.format.width = 1280;
        f.format.height = 720;
        f.format.fps = 30;
        return f;
    };
    rig.session->push(full(true));
    rig.session->push(full(false));
    QTRY_COMPARE_WITH_TIMEOUT(rig.server->fragCount(), 4, 5000);

    QCOMPARE(rig.server->liveCount(), 1);
    QCOMPARE(rig.session->liveId(), id);
    QCOMPARE(rig.server->count("DELETE", "/api/gc/v1/live/"), 0);
    QCOMPARE(rig.server->count("PUT", "/init"), 2);
    QStringList order;
    for (const MockRequest &r : rig.server->requests) {
        QVERIFY(r.method == "POST" && r.target == "/api/gc/v1/live" ? true : r.target.contains(id.toLatin1()));
        QByteArray what = r.target;
        if (r.target.contains("/init")) {
            what = r.body.startsWith("INIT-2-FULL") ? QByteArray("init full") : QByteArray("init low");
        } else if (r.target.contains("/frag")) {
            what = QByteArray("frag");
        }
        order << QString::fromLatin1(r.method + QByteArray(" ") + what);
    }
    QCOMPARE(order, QStringList({QStringLiteral("POST /api/gc/v1/live"), QStringLiteral("PUT init low"),
                                 QStringLiteral("POST frag"), QStringLiteral("POST frag"),
                                 QStringLiteral("PUT init full"), QStringLiteral("POST frag"), QStringLiteral("POST frag")}));
    const QList<MockRequest> f = rig.server->fragments();
    for (int i = 0; i < int(f.size()); ++i) {
        QCOMPARE(f[i].seq(), i);        // one stream, seq counting on across the change
    }
    QCOMPARE(rig.session->state(), LiveSession::State::Live);
}
