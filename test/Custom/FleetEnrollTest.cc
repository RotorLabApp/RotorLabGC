#include "FleetEnrollTest.h"

#include "fleet/FleetConnector.h"
#include "fleet/FleetStore.h"
#include "tak/TakManager.h"
#include "net/HttpClient.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QPointer>
#include <QtCore/QProcess>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QSslCertificate>
#include <QtNetwork/QSslConfiguration>
#include <QtNetwork/QSslKey>
#include <QtNetwork/QSslServer>
#include <QtNetwork/QSslSocket>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QTest>

#include <memory>

namespace {

// The openapi example code is "Zx3..."; this is a full 16-character code in
// the same alphabet, with both cases, a '-' and a '_'.
const QString kCode = QStringLiteral("Zx3-k_9QpL-mW_7a");

QByteArray headerValue(const QByteArray &raw, const QByteArray &name)
{
    const qsizetype end = raw.indexOf("\r\n\r\n");
    const QList<QByteArray> lines = raw.left(end < 0 ? raw.size() : end).split('\n');
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const qsizetype colon = line.indexOf(':');
        if (colon > 0 && line.left(colon).trimmed().compare(name, Qt::CaseInsensitive) == 0) {
            return line.mid(colon + 1).trimmed();
        }
    }
    return QByteArray();
}

QByteArray requestBody(const QByteArray &raw)
{
    const qsizetype end = raw.indexOf("\r\n\r\n");
    return end < 0 ? QByteArray() : raw.mid(end + 4);
}

/// A local HTTP or HTTPS server that records every application byte it
/// receives, answers POST /api/gc/v1/enroll as told, and anything else with an
/// empty roster.
class WireServer : public QObject
{
public:
    explicit WireServer(bool tls) : _tls(tls) {}

    QTcpServer       *server = nullptr;
    QByteArray        received;
    QList<QByteArray> requests;
    int               enrollStatus = 401;
    QByteArray        enrollReply =
        R"({"error":"That code is wrong or has expired. Ask for a new one."})";

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
        return server->listen(QHostAddress::LocalHost, 0);
    }

    QString url() const
    {
        return QStringLiteral("%1://127.0.0.1:%2")
            .arg(_tls ? QStringLiteral("https") : QStringLiteral("http"))
            .arg(server->serverPort());
    }

private:
    void _watch(QTcpSocket *s)
    {
        auto buf = std::make_shared<QByteArray>();
        QObject::connect(s, &QTcpSocket::readyRead, this, [this, s, buf]() {
            const QByteArray chunk = s->readAll();
            received += chunk;
            *buf += chunk;
            const qsizetype he = buf->indexOf("\r\n\r\n");
            if (he < 0) {
                return;
            }
            const qsizetype need = he + 4 + headerValue(*buf, "content-length").toLongLong();
            if (buf->size() < need) {
                return;
            }
            const QByteArray req = buf->left(need);
            requests << req;
            buf->clear();
            const bool enroll = req.startsWith("POST /api/gc/v1/enroll ");
            const int status = enroll ? enrollStatus : 200;
            const QByteArray body = enroll ? enrollReply
                                           : QByteArray(R"({"pilots":[],"aircraft":[]})");
            s->write("HTTP/1.1 " + QByteArray::number(status) + " X\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                     "Connection: close\r\n\r\n" + body);
            s->flush();
            s->disconnectFromHost();
        });
    }

    bool _tls;
};

QString findOpenssl()
{
    QString exe = QStandardPaths::findExecutable(QStringLiteral("openssl"));
    if (!exe.isEmpty()) {
        return exe;
    }
    for (const QString &c : {QStringLiteral("C:/Program Files/Git/usr/bin/openssl.exe"),
                             QStringLiteral("C:/Program Files/Git/mingw64/bin/openssl.exe"),
                             QStringLiteral("C:/Program Files (x86)/Git/usr/bin/openssl.exe")}) {
        if (QFile::exists(c)) {
            return c;
        }
    }
    return QString();
}

bool runOpenssl(const QString &ossl, const QStringList &args, QString &err)
{
    QProcess p;
    p.start(ossl, args);
    if (!p.waitForFinished(60000) || p.exitCode() != 0) {
        err = QString::fromLocal8Bit(p.readAllStandardError());
        return false;
    }
    return true;
}

/// A self-signed certificate for 127.0.0.1, its key, and the pin openssl
/// itself computes for it (SHA-256 of `pkey -pubin -outform DER`).
struct TestCert {
    QSslCertificate cert;
    QSslKey         key;
    QString         opensslPin;
    QString         error;
};

TestCert makeCert(const QString &ossl, const QString &dir)
{
    TestCert out;
    const QString keyPath  = dir + QStringLiteral("/key.pem");
    const QString certPath = dir + QStringLiteral("/cert.pem");
    const QString pubPath  = dir + QStringLiteral("/pub.pem");
    const QString derPath  = dir + QStringLiteral("/pub.der");
    if (!runOpenssl(ossl, {QStringLiteral("req"), QStringLiteral("-x509"), QStringLiteral("-newkey"),
                           QStringLiteral("rsa:2048"), QStringLiteral("-nodes"), QStringLiteral("-keyout"),
                           keyPath, QStringLiteral("-out"), certPath, QStringLiteral("-days"),
                           QStringLiteral("1"), QStringLiteral("-subj"), QStringLiteral("/CN=127.0.0.1")},
                    out.error)
        || !runOpenssl(ossl, {QStringLiteral("x509"), QStringLiteral("-in"), certPath,
                              QStringLiteral("-pubkey"), QStringLiteral("-noout"), QStringLiteral("-out"),
                              pubPath}, out.error)
        || !runOpenssl(ossl, {QStringLiteral("pkey"), QStringLiteral("-pubin"), QStringLiteral("-in"),
                              pubPath, QStringLiteral("-outform"), QStringLiteral("DER"),
                              QStringLiteral("-out"), derPath}, out.error)) {
        return out;
    }
    QFile kf(keyPath), cf(certPath), df(derPath);
    if (!kf.open(QIODevice::ReadOnly) || !cf.open(QIODevice::ReadOnly) || !df.open(QIODevice::ReadOnly)) {
        out.error = QStringLiteral("could not read the openssl output");
        return out;
    }
    out.key  = QSslKey(kf.readAll(), QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
    out.cert = QSslCertificate(cf.readAll(), QSsl::Pem);
    out.opensslPin = QString::fromLatin1(
        QCryptographicHash::hash(df.readAll(), QCryptographicHash::Sha256).toHex());
    return out;
}

} // namespace

#define NEED_TLS_CERT(tc)                                                         \
    if (!QSslSocket::supportsSsl()) {                                             \
        QSKIP("no TLS backend");                                                  \
    }                                                                             \
    const QString ossl = findOpenssl();                                           \
    if (ossl.isEmpty()) {                                                         \
        QSKIP("openssl not found; it is needed to make the test certificate");    \
    }                                                                             \
    QTemporaryDir tmpDir;                                                         \
    QVERIFY(tmpDir.isValid());                                                    \
    const TestCert tc = makeCert(ossl, tmpDir.path());                            \
    QVERIFY2(tc.error.isEmpty(), qPrintable(tc.error));                           \
    QVERIFY(!tc.cert.isNull());                                                   \
    QVERIFY(!tc.key.isNull());

void FleetEnrollTest::init()
{
    UnitTest::init();
    FleetStore::clearAll();
}

void FleetEnrollTest::cleanup()
{
    FleetStore::clearAll();
    UnitTest::cleanup();
}

void FleetEnrollTest::_parseKeepsCodeExactly()
{
    // As typed, and with whitespace around a paste.
    for (const QString &typed : {kCode, QStringLiteral("  ") + kCode + QStringLiteral("\r\n"),
                                 QStringLiteral("\t") + kCode}) {
        const FleetConnector::EnrollCode c = FleetConnector::parseEnrollCode(typed);
        QCOMPARE(c.code, kCode);
        QVERIFY(!c.hasPin);
        QVERIFY(c.pin.isEmpty());
        QVERIFY(FleetConnector::enrollCodeReady(typed));
    }

    // Every character class token_urlsafe uses is accepted.
    QVERIFY(FleetConnector::enrollCodeReady(QStringLiteral("abcdefghABCDEF09")));
    QVERIFY(FleetConnector::enrollCodeReady(QStringLiteral("----____aaaaZZZZ")));

    // Not a 16-character code: the old 8-character form, too long, too short,
    // a space inside, characters outside the alphabet.
    for (const QString &bad : {QStringLiteral("ABCD2345"), kCode + QStringLiteral("x"), kCode.left(15),
                               QStringLiteral("Zx3-k_9Q pL-mW_7"), QStringLiteral("Zx3-k_9QpL+mW_7a"),
                               QStringLiteral("Zx3-k_9QpL/mW_7a"), QString()}) {
        QVERIFY2(!FleetConnector::enrollCodeReady(bad), qPrintable(bad));
    }

    // code.pin: everything before the first dot is the code.
    const QString pin = QString(64, QLatin1Char('a')).replace(0, 6, QStringLiteral("0f9e3c"));
    const FleetConnector::EnrollCode withPin = FleetConnector::parseEnrollCode(kCode + QLatin1Char('.') + pin);
    QCOMPARE(withPin.code, kCode);
    QCOMPARE(withPin.pin, pin);
    QVERIFY(withPin.hasPin);
    QVERIFY(FleetConnector::enrollCodeReady(kCode + QLatin1Char('.') + pin));
    QVERIFY(!FleetConnector::enrollCodeReady(kCode + QStringLiteral(".")));
    QVERIFY(!FleetConnector::enrollCodeReady(kCode + QStringLiteral(".abc")));
    QVERIFY(!FleetConnector::enrollCodeReady(kCode + QLatin1Char('.') + pin + QStringLiteral(".x")));

    // The console link carries the address and pin with the code.
    const QString link = QStringLiteral("rotorlab-server://enroll?a=127.0.0.1:5443&p=%1&c=%2").arg(pin, kCode);
    const FleetConnector::EnrollCode fromLink = FleetConnector::parseEnrollCode(link);
    QCOMPARE(fromLink.code, kCode);
    QCOMPARE(fromLink.pin, pin);
    QCOMPARE(fromLink.address, QStringLiteral("https://127.0.0.1:5443"));
    QVERIFY(FleetConnector::enrollCodeReady(link));

    // The request body carries the code untouched.
    const QJsonObject body = FleetConnector::enrollBody(kCode, QStringLiteral(" Truck 4 tablet "));
    QCOMPARE(body.value(QStringLiteral("code")).toString(), kCode);
    QCOMPARE(body.value(QStringLiteral("label")).toString(), QStringLiteral("Truck 4 tablet"));
    QVERIFY(!body.value(QStringLiteral("platform")).toString().isEmpty());
    QVERIFY(body.value(QStringLiteral("platform")).toString().size() <= 40);
    QCOMPARE(FleetConnector::enrollBody(kCode, QString(120, QLatin1Char('n')))
                 .value(QStringLiteral("label")).toString().size(), 80);
}

void FleetEnrollTest::_errorTexts()
{
    const QString base = QStringLiteral("https://fleet:5443");
    auto reply = [](int status, const QString &error) {
        FleetReply r;
        r.netOk = status > 0;
        r.status = status;
        r.error = error;
        return r;
    };

    QVERIFY(FleetConnector::enrollErrorText(reply(400, QStringLiteral("Send the enrollment code.")), base)
                .contains(QStringLiteral("did not get an enrollment code")));
    QVERIFY(FleetConnector::enrollErrorText(reply(401, QString()), base)
                .contains(QStringLiteral("wrong or has expired")));
    QVERIFY(FleetConnector::enrollErrorText(reply(409, QString()), base)
                .contains(QStringLiteral("no Fleet Server license")));

    // 429: the server names the wait in its sentence.
    const QString wait = FleetConnector::enrollErrorText(
        reply(429, QStringLiteral("Too many tries. Wait 37 seconds.")), base);
    QVERIFY2(wait.contains(QStringLiteral("Wait 37 seconds")), qPrintable(wait));
    // A Retry-After header, when there is one, is taken first.
    FleetReply ra = reply(429, QStringLiteral("Too many tries. Wait 37 seconds."));
    ra.retryAfter = 12;
    QVERIFY(FleetConnector::enrollErrorText(ra, base).contains(QStringLiteral("Wait 12 seconds")));

    FleetReply pinned;
    pinned.pinRefused = true;
    QVERIFY(FleetConnector::enrollErrorText(pinned, base).contains(QStringLiteral("does not match the pin")));

    QVERIFY(FleetConnector::enrollErrorText(reply(0, QStringLiteral("Connection refused")), base)
                .contains(QStringLiteral("Could not reach")));
    QVERIFY(FleetConnector::enrollErrorText(reply(503, QString()), base).contains(QStringLiteral("503")));
}

void FleetEnrollTest::_openapiExampleReachesBodyUnchanged()
{
#ifndef QGC_DAILY_BUILD
    QSKIP("loopback http is a daily-build allowance");
#else
    WireServer srv(false);
    QVERIFY(srv.listen());

    FleetConnector conn;
    QCOMPARE(conn.state(), FleetConnector::NotConnected);
    conn.enroll(srv.url(), QStringLiteral("  ") + kCode + QStringLiteral("\n"),
                QStringLiteral("Truck 4 tablet"));
    QTRY_COMPARE_WITH_TIMEOUT(srv.requests.size(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!conn.busy(), 10000);

    const QByteArray req = srv.requests.first();
    QVERIFY2(req.startsWith("POST /api/gc/v1/enroll HTTP/1.1"), req.left(60).constData());
    QVERIFY(headerValue(req, "authorization").isEmpty());
    const QJsonObject body = QJsonDocument::fromJson(requestBody(req)).object();
    QCOMPARE(body.value(QStringLiteral("code")).toString(), kCode);
    QCOMPARE(body.value(QStringLiteral("label")).toString(), QStringLiteral("Truck 4 tablet"));
    QVERIFY(body.contains(QStringLiteral("platform")));

    // 401 from the server: a plain sentence, not enrolled, nothing stored.
    QCOMPARE(conn.state(), FleetConnector::NotConnected);
    QVERIFY(!conn.enrolled());
    QVERIFY(conn.lastError().contains(QStringLiteral("wrong or has expired")));

    // 429 shows the wait the server names.
    srv.enrollStatus = 429;
    srv.enrollReply = R"({"error":"Too many tries. Wait 41 seconds."})";
    conn.enroll(srv.url(), kCode, QStringLiteral("Truck 4 tablet"));
    QTRY_COMPARE_WITH_TIMEOUT(srv.requests.size(), 2, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!conn.busy(), 10000);
    QVERIFY2(conn.lastError().contains(QStringLiteral("Wait 41 seconds")), qPrintable(conn.lastError()));

    // 200 with no station_id is refused rather than half-enrolled.
    srv.enrollStatus = 200;
    srv.enrollReply = R"({"token":"rlgs_abc"})";
    conn.enroll(srv.url(), kCode, QStringLiteral("Truck 4 tablet"));
    QTRY_COMPARE_WITH_TIMEOUT(srv.requests.size(), 3, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!conn.busy(), 10000);
    QVERIFY(!conn.enrolled());
    QVERIFY(conn.lastError().contains(QStringLiteral("missing")));
#endif
}

void FleetEnrollTest::_spkiMatchesOpenssl()
{
    NEED_TLS_CERT(tc)
    QCOMPARE(HttpClient::spkiSha256Hex(tc.cert.toDer()), tc.opensslPin);
    QVERIFY(HttpClient::spkiFromCertificateDer(QByteArray("not a certificate")).isEmpty());
    QVERIFY(HttpClient::spkiSha256Hex(QByteArray()).isEmpty());
}

void FleetEnrollTest::_codeDotPinSendsOnlyCodeAndKeepsPin()
{
    NEED_TLS_CERT(tc)
    WireServer srv(true);
    QVERIFY(srv.listen(tc.cert, tc.key));
    srv.enrollStatus = 200;
    srv.enrollReply = R"({"station_id":7,"token":"rlgs_0123456789abcdefghijABCDEFGHIJ-_0123456789"})";

    FleetConnector conn;
    conn.enroll(srv.url(), kCode + QLatin1Char('.') + tc.opensslPin, QStringLiteral("Truck 4 tablet"));
    QTRY_VERIFY_WITH_TIMEOUT(conn.enrolled(), 15000);
    QCOMPARE(conn.stationId(), 7);

    // Only the part before the dot went out as the code; the pin never did.
    QVERIFY(!srv.requests.isEmpty());
    const QJsonObject body = QJsonDocument::fromJson(requestBody(srv.requests.first())).object();
    QCOMPARE(body.value(QStringLiteral("code")).toString(), kCode);
    QVERIFY(!srv.received.contains(tc.opensslPin.toLatin1()));

    // The pin is kept, and the next connection (the roster, with the token)
    // is checked against it and goes through.
    QCOMPARE(FleetStore::serverPin(), tc.opensslPin);
    QVERIFY(conn.certInfo().contains(tc.opensslPin.left(16)));
    QTRY_COMPARE_WITH_TIMEOUT(conn.state(), FleetConnector::Connected, 15000);
    QVERIFY(srv.requests.size() >= 2);
    QVERIFY(srv.requests.at(1).startsWith("GET /api/gc/v1/roster"));
    QVERIFY(headerValue(srv.requests.at(1), "authorization").startsWith("Bearer rlgs_"));

    conn.disconnectFromServer();
    QVERIFY(FleetStore::serverPin().isEmpty());
}

void FleetEnrollTest::_wrongPinRefusesBeforeSending()
{
    NEED_TLS_CERT(tc)
    WireServer srv(true);
    QVERIFY(srv.listen(tc.cert, tc.key));
    srv.enrollStatus = 200;
    srv.enrollReply = R"({"station_id":8,"token":"rlgs_x"})";

    const QString wrongPin = QString::fromLatin1(
        QCryptographicHash::hash("some other key", QCryptographicHash::Sha256).toHex());
    QVERIFY(wrongPin != tc.opensslPin);

    FleetConnector conn;
    conn.enroll(srv.url(), kCode + QLatin1Char('.') + wrongPin, QStringLiteral("Truck 4 tablet"));
    QVERIFY(conn.busy());
    QTRY_VERIFY_WITH_TIMEOUT(!conn.busy(), 15000);

    // The handshake may happen; not one byte of the request may follow it.
    QVERIFY2(srv.received.isEmpty(), srv.received.left(80).constData());
    QVERIFY(srv.requests.isEmpty());
    QVERIFY(!conn.enrolled());
    QCOMPARE(conn.state(), FleetConnector::NotConnected);
    QVERIFY2(conn.lastError().contains(QStringLiteral("does not match the pin")), qPrintable(conn.lastError()));
    QVERIFY(FleetStore::serverPin().isEmpty());
}

void FleetEnrollTest::_pinOverPlainHttpRefused()
{
#ifndef QGC_DAILY_BUILD
    QSKIP("loopback http is a daily-build allowance");
#else
    WireServer srv(false);
    QVERIFY(srv.listen());
    FleetConnector conn;
    conn.enroll(srv.url(), kCode + QLatin1Char('.') + QString(64, QLatin1Char('a')),
                QStringLiteral("Truck 4 tablet"));
    QVERIFY(!conn.busy());
    QVERIFY(conn.lastError().contains(QStringLiteral("https://")));
    QTest::qWait(300);
    QVERIFY(srv.received.isEmpty());
#endif
}

void FleetEnrollTest::_liveEnroll()
{
    const QString url = qEnvironmentVariable("RLGC_FLEET_LIVE_URL");
    const QString codeFile = qEnvironmentVariable("RLGC_FLEET_LIVE_CODE_FILE");
    if (url.isEmpty() || codeFile.isEmpty()) {
        QSKIP("live enroll: RLGC_FLEET_LIVE_URL and RLGC_FLEET_LIVE_CODE_FILE are not set");
    }
    QFile f(codeFile);
    QVERIFY2(f.open(QIODevice::ReadOnly), "cannot read the code file");
    const QString code = QString::fromUtf8(f.readAll());   // never printed

    FleetConnector conn;
    int status = -1;
    connect(&conn, &FleetConnector::logMessage, this, [&status](const QString &line) {
        if (line.startsWith(QStringLiteral("enroll: HTTP "))) {
            status = line.mid(13).toInt();
        } else if (line.startsWith(QStringLiteral("enroll: refused"))) {
            status = 0;
        }
    });
    conn.enroll(url, code, QStringLiteral("GC G19 live check"));
    QTRY_VERIFY_WITH_TIMEOUT(!conn.busy(), 20000);

    const int stationId = conn.enrolled() ? conn.stationId() : 0;
    const QString resultPath = qEnvironmentVariable("RLGC_FLEET_LIVE_RESULT");
    if (!resultPath.isEmpty()) {
        QFile out(resultPath);
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            out.write(QStringLiteral("status=%1 station_id=%2 state=%3 error=%4\n")
                          .arg(status).arg(stationId).arg(conn.stateText(), conn.lastError()).toUtf8());
        }
    }
    QCOMPARE(status, 200);
    QVERIFY(stationId > 0);

    // Let the first sync reach the server with the new token, then forget it
    // locally. The station stays in the server console.
    QTRY_COMPARE_WITH_TIMEOUT(conn.state(), FleetConnector::Connected, 15000);
    conn.disconnectFromServer();
}

void FleetEnrollTest::_takRouteEdge()
{
    // Becomes the route: enrolled and not suspended.
    QCOMPARE(FleetConnector::takRouteEdge(false, true, false), 1);
    // Already the route: nothing again.
    QCOMPARE(FleetConnector::takRouteEdge(true, true, false), 0);
    // The RotorLab Connector takes over (suspended), or the station leaves.
    QCOMPARE(FleetConnector::takRouteEdge(true, true, true), -1);
    QCOMPARE(FleetConnector::takRouteEdge(true, false, false), -1);
    // Not enrolled, or enrolled but suspended: never the route.
    QCOMPARE(FleetConnector::takRouteEdge(false, false, false), 0);
    QCOMPARE(FleetConnector::takRouteEdge(false, true, true), 0);
}

void FleetEnrollTest::_joinClosesTakLink()
{
    TakManager *tak = TakManager::instance();
    tak->disconnectServer();
    tak->clearFleetHandOff();
    tak->_onEncrypted();      // a live TAK link (no socket in the test)
    tak->_prefixSetFor = tak->_callsignPrefix;   // set on this install (security M1)
    tak->setC2Armed(true);
    QVERIFY(tak->c2Armed());
    QVERIFY(tak->connected());

    // An enrolled station (a token is stored) whose server does not answer.
    FleetStore::setBaseUrl(QStringLiteral("http://127.0.0.1:9"));
    FleetStore::setToken(QStringLiteral("unit-test-token-not-real"));
    QVERIFY(FleetStore::enrolled());
    {
        FleetConnector conn;
        QVERIFY(!tak->connected());
        QVERIFY(!tak->c2Armed());
        QVERIFY(!tak->fleetHandOffNotice().isEmpty());

        // S4: a TakManager created after the route changed knows it.
        {
            TakManager late;
            QVERIFY(late._fleetIsRoute);
            late.setC2Armed(true);
            QVERIFY(!late.c2Armed());
        }

        // The RotorLab Connector takes over: the direct link is wanted back,
        // so nothing is closed and the notice goes.
        conn.setSuspendedByRotorLab(true);
        QVERIFY(tak->fleetHandOffNotice().isEmpty());
        tak->_onEncrypted();
        tak->_prefixSetFor = tak->_callsignPrefix;   // set on this install (security M1)
        tak->setC2Armed(true);
        QVERIFY(tak->c2Armed());
        conn.setSuspendedByRotorLab(true);    // no change
        QVERIFY(tak->connected());

        // The RotorLab Connector is switched off: the Fleet Server is the
        // route again, so the TAK link closes again.
        conn.setSuspendedByRotorLab(false);
        QVERIFY(!tak->connected());
        QVERIFY(!tak->c2Armed());
        QVERIFY(!tak->fleetHandOffNotice().isEmpty());
        conn.setSuspendedByRotorLab(true);    // stop its retries before it goes
    }
    tak->clearFleetHandOff();
    tak->disconnectServer();
}
