/****************************************************************************
 *
 * RotorLab GC - shared HTTPS transport for the connectors.
 *
 ****************************************************************************/

#include "HttpClient.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QSslCertificate>
#include <QtNetwork/QSslConfiguration>
#include <QtNetwork/QSslError>

#include "QGCLoggingCategory.h"

QGC_LOGGING_CATEGORY(HttpClientLog, "gcs.custom.net.http")

HttpClient::HttpClient(TlsPolicy policy, QObject *parent)
    : QObject(parent)
    , _policy(policy)
    , _nam(new QNetworkAccessManager(this))
{
}

HttpClient::~HttpClient() = default;

void HttpClient::setBaseUrl(const QString &url) { _baseUrl = url; }
void HttpClient::setToken(const QString &token) { _token = token; }
void HttpClient::reset() { ++_generation; }

namespace {

/// The property a pin check leaves on a reply it stopped.
constexpr char kPinRefused[] = "rlPinRefused";

/// Read the DER tag and length at `pos`. `hdr` is the size of tag plus
/// length octets, `len` the content size. False when it runs off the end.
bool derItem(const QByteArray &d, qsizetype pos, quint8 &tag, qsizetype &hdr, qsizetype &len)
{
    if (pos < 0 || pos + 2 > d.size()) {
        return false;
    }
    tag = quint8(d.at(pos));
    const quint8 first = quint8(d.at(pos + 1));
    hdr = 2;
    if (first < 0x80) {
        len = first;
    } else {
        const int n = first & 0x7f;
        if (n == 0 || n > 4 || pos + 2 + n > d.size()) {
            return false;
        }
        len = 0;
        for (int i = 0; i < n; ++i) {
            len = (len << 8) | quint8(d.at(pos + 2 + i));
        }
        hdr += n;
    }
    return len >= 0 && pos + hdr + len <= d.size();
}

} // namespace

QByteArray HttpClient::spkiFromCertificateDer(const QByteArray &der)
{
    // Certificate ::= SEQUENCE { tbsCertificate SEQUENCE { [0] version OPTIONAL,
    //   serialNumber, signature, issuer, validity, subject,
    //   subjectPublicKeyInfo, ... }, ... }
    quint8 tag = 0;
    qsizetype hdr = 0, len = 0;
    if (!derItem(der, 0, tag, hdr, len) || tag != 0x30) {
        return {};
    }
    qsizetype pos = hdr;
    if (!derItem(der, pos, tag, hdr, len) || tag != 0x30) {
        return {};
    }
    pos += hdr;
    const qsizetype tbsEnd = pos + len;
    if (!derItem(der, pos, tag, hdr, len)) {
        return {};
    }
    if (tag == 0xA0) {
        pos += hdr + len;
    }
    for (int i = 0; i < 5; ++i) {
        if (!derItem(der, pos, tag, hdr, len) || pos + hdr + len > tbsEnd) {
            return {};
        }
        pos += hdr + len;
    }
    if (!derItem(der, pos, tag, hdr, len) || tag != 0x30 || pos + hdr + len > tbsEnd) {
        return {};
    }
    return der.mid(pos, hdr + len);
}

QString HttpClient::spkiSha256Hex(const QByteArray &certDer)
{
    const QByteArray spki = spkiFromCertificateDer(certDer);
    if (spki.isEmpty()) {
        return {};
    }
    return QString::fromLatin1(QCryptographicHash::hash(spki, QCryptographicHash::Sha256).toHex());
}

void HttpClient::setPinnedSpkiSha256(const QString &pin)
{
    const QString p = pin.trimmed().toLower();
    if (p == _pin) {
        return;
    }
    _pin = p;
    // A connection opened under the old pin (or none) must not be reused.
    _nam->clearConnectionCache();
}

bool HttpClient::_pinNeedsTls(HttpReply &fail) const
{
    if (_pin.isEmpty() || _baseUrl.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
        return false;
    }
    fail.pinRefused = true;
    fail.error = QStringLiteral("This code carries a server pin, so the address must start with "
                                "https://. Nothing was sent.");
    return true;
}

void HttpClient::_guardPin(QNetworkReply *reply) const
{
    if (_pin.isEmpty() || !reply) {
        return;
    }
    const QString pin = _pin;
    // Both checks run on the network thread's signal, before Qt writes the
    // request (HTTP/1.1 only, see _request()). sslErrors comes first for a
    // self-signed server; encrypted covers one the system trust store accepts.
    const auto matches = [reply, pin]() {
        const QSslCertificate peer = reply->sslConfiguration().peerCertificate();
        return !peer.isNull() && spkiSha256Hex(peer.toDer()) == pin;
    };
    connect(reply, &QNetworkReply::sslErrors, reply, [reply, matches](const QList<QSslError> &) {
        if (matches()) {
            reply->ignoreSslErrors();
        } else {
            reply->setProperty(kPinRefused, true);
            reply->abort();
        }
    }, Qt::DirectConnection);
    connect(reply, &QNetworkReply::encrypted, reply, [reply, matches]() {
        if (!matches()) {
            reply->setProperty(kPinRefused, true);
            reply->abort();
        }
    }, Qt::DirectConnection);
}

QString HttpClient::normalizeBaseUrl(const QString &typed, bool allowLocalhostHttp)
{
    QString t = typed.trimmed();
    if (t.isEmpty()) {
        return {};
    }

    // Work out the host before deciding what to do about the scheme, so the
    // localhost exemption can be applied to what the operator actually meant.
    const QUrl probe(t.contains(QStringLiteral("://")) ? t
                                                       : QStringLiteral("https://") + t);
    const QString host = probe.host().toLower();
    const bool isLocal = (host == QLatin1String("localhost")
                          || host == QLatin1String("127.0.0.1")
                          || host == QLatin1String("::1"));

    const bool keepHttp = allowLocalhostHttp && isLocal
                          && t.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive);

    // Default the scheme rather than letting a plain host fail, and upgrade
    // plaintext: it is a bearer credential on the wire, and a server that
    // answers http will usually redirect anyway.
    if (!t.contains(QStringLiteral("://"))) {
        t.prepend(keepHttp ? QStringLiteral("http://") : QStringLiteral("https://"));
    } else if (!keepHttp && t.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)) {
        t.replace(0, 7, QStringLiteral("https://"));
    }

    const QUrl u(t);
    if (!u.isValid() || u.host().isEmpty()) {
        return {};
    }
    QString out = QStringLiteral("%1://%2").arg(u.scheme(), u.host());
    if (u.port() > 0) {
        out += QStringLiteral(":%1").arg(u.port());
    }
    return out;
}

void HttpClient::get(const QString &path, Handler done, int timeoutMs)
{
    _send(QStringLiteral("GET"), path, nullptr, true, std::move(done), timeoutMs);
}

void HttpClient::post(const QString &path, const QJsonObject &body, Handler done, int timeoutMs)
{
    _send(QStringLiteral("POST"), path, &body, true, std::move(done), timeoutMs);
}

void HttpClient::put(const QString &path, const QJsonObject &body, Handler done, int timeoutMs)
{
    _send(QStringLiteral("PUT"), path, &body, true, std::move(done), timeoutMs);
}

void HttpClient::postUnauthenticated(const QString &path, const QJsonObject &body,
                                     Handler done, int timeoutMs)
{
    _send(QStringLiteral("POST"), path, &body, false, std::move(done), timeoutMs);
}

QNetworkRequest HttpClient::_request(const QString &path, bool authenticated) const
{
    QNetworkRequest req{QUrl(_baseUrl + path)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    if (!_pin.isEmpty()) {
        // HTTP/1.1, so nothing is negotiated or written ahead of the pin check.
        req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    }
    if (authenticated && !_token.isEmpty()) {
        // Header only. Never a query string, never a log line.
        req.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + _token.toUtf8());
    }
    return req;
}

QNetworkReply *HttpClient::postFile(const QString &path, const QString &filePath,
                                    Handler done, Progress progress, int idleTimeoutMs)
{
    return _sendFile(QStringLiteral("POST"), path, filePath, {}, std::move(done),
                     std::move(progress), idleTimeoutMs);
}

QNetworkReply *HttpClient::putFile(const QString &path, const QString &filePath,
                                   const QByteArray &sha256, Handler done,
                                   Progress progress, int idleTimeoutMs)
{
    return _sendFile(QStringLiteral("PUT"), path, filePath, sha256, std::move(done),
                     std::move(progress), idleTimeoutMs);
}

QNetworkReply *HttpClient::_sendFile(const QString &verb, const QString &path,
                                     const QString &filePath, const QByteArray &sha256,
                                     Handler done, Progress progress, int idleTimeoutMs)
{
    HttpReply fail;
    if (_baseUrl.isEmpty()) {
        fail.error = QStringLiteral("no server address configured");
        if (done) done(fail);
        return nullptr;
    }

    if (_pinNeedsTls(fail)) {
        if (done) done(fail);
        return nullptr;
    }

    auto *file = new QFile(filePath);
    if (!file->open(QIODevice::ReadOnly)) {
        // QIODevice::ReadOnly and nothing else. QIODevice::Text would translate
        // newlines and quietly corrupt a binary log.
        fail.error = QStringLiteral("cannot read %1: %2").arg(filePath, file->errorString());
        delete file;
        if (done) done(fail);
        return nullptr;
    }

    QNetworkRequest req = _request(path, true);
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/octet-stream"));
    req.setHeader(QNetworkRequest::ContentLengthHeader, file->size());
    if (!sha256.isEmpty()) {
        req.setRawHeader("X-Content-SHA256", sha256);
    }
    // Idle timeout: reset by Qt on every chunk that moves, so a slow-but-alive
    // upload is never cut off, and a dead one still ends.
    req.setTransferTimeout(idleTimeoutMs);

    QNetworkReply *reply = verb == QLatin1String("PUT")
        ? _nam->put(req, file)
        : _nam->post(req, file);
    if (!reply) {
        delete file;
        fail.error = QStringLiteral("could not create request");
        if (done) done(fail);
        return nullptr;
    }
    file->setParent(reply);         // closed and freed with the reply
    _guardPin(reply);

    if (progress) {
        connect(reply, &QNetworkReply::uploadProgress, this,
                [progress = std::move(progress)](qint64 sent, qint64 total) {
            progress(sent, total);
        });
    }

    _wire(reply, std::move(done));
    return reply;
}

void HttpClient::_send(const QString &verb, const QString &path, const QJsonObject *body,
                       bool authenticated, Handler done, int timeoutMs)
{
    HttpReply fail;
    if (_baseUrl.isEmpty()) {
        fail.error = QStringLiteral("no server address configured");
        if (done) done(fail);
        return;
    }

    if (_pinNeedsTls(fail)) {
        if (done) done(fail);
        return;
    }

    QNetworkRequest req = _request(path, authenticated);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    QNetworkReply *reply = nullptr;
    if (verb == QLatin1String("GET")) {
        reply = _nam->get(req);
    } else if (verb == QLatin1String("PUT")) {
        const QByteArray payload = body
            ? QJsonDocument(*body).toJson(QJsonDocument::Compact)
            : QByteArrayLiteral("{}");
        reply = _nam->put(req, payload);
    } else {
        const QByteArray payload = body
            ? QJsonDocument(*body).toJson(QJsonDocument::Compact)
            : QByteArrayLiteral("{}");
        reply = _nam->post(req, payload);
    }
    if (!reply) {
        fail.error = QStringLiteral("could not create request");
        if (done) done(fail);
        return;
    }
    _guardPin(reply);

    // A wall-clock timeout, longer than any long-poll wait carried in the path,
    // or every poll looks like a broken server. Uploads use an idle timeout
    // instead - see postFile().
    auto *timer = new QTimer(reply);
    timer->setSingleShot(true);
    timer->setInterval(timeoutMs);
    connect(timer, &QTimer::timeout, reply, [reply]() {
        if (reply->isRunning()) {
            reply->abort();
        }
    });
    timer->start();

    _wire(reply, std::move(done));
}

void HttpClient::_wire(QNetworkReply *reply, Handler done)
{
    const quint64 gen = _generation;
    connect(reply, &QNetworkReply::finished, this,
            [this, gen, reply, done = std::move(done)]() {
        // Belongs to a session that has since been torn down. Free it and say
        // nothing: answering would let a dead session move the state machine.
        if (gen != _generation) {
            reply->deleteLater();
            return;
        }

        HttpReply out;
        out.raw    = reply->readAll();
        out.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        out.netOk  = (out.status > 0);
        if (!out.raw.isEmpty()) {
            const QJsonDocument doc = QJsonDocument::fromJson(out.raw);
            if (doc.isObject()) {
                out.body = doc.object();
            }
        }
        if (reply->error() != QNetworkReply::NoError && !out.netOk) {
            out.error = reply->errorString();
        }
        if (reply->property(kPinRefused).toBool()) {
            out.netOk = false;
            out.status = 0;
            out.pinRefused = true;
            out.error = QStringLiteral("The server's key does not match the pin in the enrollment "
                                       "code. Nothing was sent.");
            reply->deleteLater();
            if (done) {
                done(out);
            }
            return;
        }
        bool retryOk = false;
        const int retry = reply->rawHeader("Retry-After").trimmed().toInt(&retryOk);
        if (retryOk && retry >= 0) {
            out.retryAfter = retry;
        }
        // Prefer the server's own sentence when it sent one: a refusal is
        // meant to be shown to the operator verbatim.
        if (out.body.contains(QStringLiteral("error"))) {
            out.error = out.body.value(QStringLiteral("error")).toString();
        }

        reply->deleteLater();
        if (done) {
            done(out);
        }
    });
}
