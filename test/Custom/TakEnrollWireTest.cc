#include "TakEnrollWireTest.h"

#include "tak/TakManager.h"
#include "tak/TakPolicy.h"
#include "rotorlab/Credential.h"

#include <QtCore/QFile>
#include <QtCore/QPointer>
#include <QtCore/QProcess>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QSslCertificate>
#include <QtNetwork/QSslConfiguration>
#include <QtNetwork/QSslKey>
#include <QtNetwork/QSslServer>
#include <QtNetwork/QSslSocket>
#include <QtTest/QTest>

static void clearTakSettings()
{
    QSettings s;
    s.remove(QStringLiteral("TAK"));
    s.sync();
}

void TakEnrollWireTest::init()
{
    UnitTest::init();
    clearTakSettings();
}

void TakEnrollWireTest::cleanup()
{
    clearTakSettings();
    // Accepting a fingerprint saves the settings, and with them the TAK
    // password, under the per-process unit-test key-store name (G41).
    Credential::erase(TakManager::passwordAccount());
    UnitTest::cleanup();
}

namespace {

/// A TLS server that records every byte it receives and answers the first
/// complete HTTP request with an empty 500 so the client finishes at once.
class RecordingServer : public QObject
{
public:
    QSslServer       server;
    QByteArray       received;
    QList<QPointer<QSslSocket>> sockets;

    bool listen(const QSslCertificate &cert, const QSslKey &key)
    {
        QSslConfiguration cfg = QSslConfiguration::defaultConfiguration();
        cfg.setLocalCertificate(cert);
        cfg.setPrivateKey(key);
        cfg.setPeerVerifyMode(QSslSocket::VerifyNone);
        server.setSslConfiguration(cfg);
        QObject::connect(&server, &QSslServer::startedEncryptionHandshake, this, [this](QSslSocket *s) {
            sockets << s;
            QObject::connect(s, &QSslSocket::readyRead, this, [this, s]() {
                received += s->readAll();
                if (received.contains("\r\n\r\n")) {
                    s->write("HTTP/1.1 500 Test\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    s->flush();
                    s->disconnectFromHost();
                }
            });
        });
        // Drain accepted connections so the server keeps accepting.
        QObject::connect(&server, &QTcpServer::pendingConnectionAvailable, this, [this]() {
            while (server.hasPendingConnections()) {
                server.nextPendingConnection();
            }
        });
        return server.listen(QHostAddress::LocalHost, 0);
    }
};

/// The value of HTTP header `name` in a raw request, or a null array. Header
/// names are case-insensitive and Qt 6 writes them in lowercase
/// ("authorization:"), so the name is matched without case.
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

} // namespace

void TakEnrollWireTest::_noCredentialsBeforeAccept()
{
    if (!QSslSocket::supportsSsl()) {
        QSKIP("no TLS backend");
    }

    TakManager tak;
    const QString ossl = tak._findOpenssl();
    if (ossl.isEmpty()) {
        QSKIP("openssl not found; it is needed to make the test certificate");
    }

    // A self-signed server certificate made here, for 127.0.0.1.
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString keyPath  = tmp.path() + QStringLiteral("/server_key.pem");
    const QString certPath = tmp.path() + QStringLiteral("/server_cert.pem");
    QProcess p;
    p.start(ossl, { QStringLiteral("req"), QStringLiteral("-x509"), QStringLiteral("-newkey"),
                    QStringLiteral("rsa:2048"), QStringLiteral("-nodes"), QStringLiteral("-keyout"), keyPath,
                    QStringLiteral("-out"), certPath, QStringLiteral("-days"), QStringLiteral("1"),
                    QStringLiteral("-subj"), QStringLiteral("/CN=127.0.0.1") });
    QVERIFY(p.waitForFinished(60000));
    QVERIFY2(p.exitCode() == 0, p.readAllStandardError().constData());
    QFile kf(keyPath), cf(certPath);
    QVERIFY(kf.open(QIODevice::ReadOnly) && cf.open(QIODevice::ReadOnly));
    const QSslKey key(kf.readAll(), QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
    const QSslCertificate cert(cf.readAll(), QSsl::Pem);
    QVERIFY(!key.isNull());
    QVERIFY(!cert.isNull());
    const QString serverFp = TakPolicy::sha256Fingerprint(cert.toDer());

    RecordingServer srv;
    QVERIFY(srv.listen(cert, key));

    // Verify on (the default), no CA imported, no pin yet.
    tak._host       = QStringLiteral("127.0.0.1");
    tak._enrollPort = srv.server.serverPort();
    tak._username   = QStringLiteral("wire-user");
    tak._password   = QStringLiteral("wire-secret");
    tak._caCertPem.clear();
    QVERIFY(tak._verifyPeer);
    QVERIFY(tak.pinnedFingerprint().isEmpty());

    // 1. Before accepting: the handshake happens, nothing else.
    tak.enroll();
    QTest::qWait(500);   // let any stray bytes land
    for (const QPointer<QSslSocket> &s : srv.sockets) {
        if (s) {
            srv.received += s->readAll();
        }
    }
    QVERIFY2(srv.received.isEmpty(), srv.received.left(200).constData());
    QVERIFY(!srv.received.contains("POST"));
    QVERIFY(!srv.received.toLower().contains("authorization"));
    QCOMPARE(tak.pendingFingerprint(), serverFp);
    QVERIFY(!tak.enrolled());

    // 2. Accept on the page: enrollment runs again and now sends.
    tak.acceptServerFingerprint();
    QCOMPARE(tak.pinnedFingerprint(), serverFp);
    QTRY_VERIFY_WITH_TIMEOUT(srv.received.contains("\r\n\r\n"), 10000);
    QVERIFY2(srv.received.startsWith("POST /Marti/api/tls/signClient/v2"), srv.received.left(200).constData());
    const QByteArray expected = "Basic " + QByteArray("wire-user:wire-secret").toBase64();
    QCOMPARE(headerValue(srv.received, "Authorization"), expected);
}
