/****************************************************************************
 *
 * RotorLab GC - shared HTTPS transport for the connectors.
 *
 * Everything two connectors genuinely have in common on the wire: the bearer
 * header, JSON verbs, a streamed file upload, decoding a reply, and abandoning
 * requests that belong to a session that has since been torn down.
 *
 * It owns none of the things that differ. No endpoint paths, no state machine,
 * no spool. The two connectors share almost nothing above this line.
 *
 * The TLS policy is explicit at every construction site. The current Fleet
 * Server and RotorLab APIs use ordinary certificate-authority validation.
 * RotorLab Server 2.0 can also hand out a key pin with an enrollment code
 * (the SHA-256 of the server's SubjectPublicKeyInfo); a client given one
 * checks it on every new connection before any request is written.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtNetwork/QNetworkRequest>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

/// One completed call. `netOk` false means the request never produced an HTTP
/// answer (DNS, connect, timeout); `status` is then 0.
struct HttpReply {
    bool        netOk = false;
    int         status = 0;
    QJsonObject body;
    QByteArray  raw;
    QString     error;          ///< transport error, or the server's own sentence
    int         retryAfter = -1;    ///< Retry-After seconds, when the server sent one
    bool        pinRefused = false; ///< stopped before sending: the server key did not match the pin
};

class HttpClient : public QObject
{
    Q_OBJECT

public:
    /// How this client decides whether to trust the certificate it is served.
    enum class TlsPolicy {
        PublicCa,           ///< ordinary validation against the system trust store
    };

    /// `policy` is mandatory on purpose - see the header comment.
    explicit HttpClient(TlsPolicy policy, QObject *parent = nullptr);
    ~HttpClient() override;

    using Handler  = std::function<void(const HttpReply &)>;
    using Progress = std::function<void(qint64 sent, qint64 total)>;

    TlsPolicy tlsPolicy() const { return _policy; }

    void setBaseUrl(const QString &url);
    QString baseUrl() const { return _baseUrl; }

    void setToken(const QString &token);

    /// Pin the server's key: lowercase hex SHA-256 of the certificate's
    /// SubjectPublicKeyInfo (DER). Empty means no pin, which leaves ordinary
    /// certificate-authority validation in charge.
    ///
    /// With a pin, every new TLS connection is checked after the handshake and
    /// before any request byte is written. A match is trusted even when the
    /// certificate is self-signed; a mismatch aborts the request, which then
    /// answers with `pinRefused` set. A pin over plain http is refused outright,
    /// since there is no key to check.
    void setPinnedSpkiSha256(const QString &pin);
    QString pinnedSpkiSha256() const { return _pin; }

    /// The DER SubjectPublicKeyInfo inside a DER certificate, or empty when it
    /// cannot be parsed. Parsed here rather than taken from QSslKey so the
    /// bytes are the same on every TLS backend.
    static QByteArray spkiFromCertificateDer(const QByteArray &certDer);

    /// Lowercase hex SHA-256 of a DER certificate's SubjectPublicKeyInfo.
    static QString spkiSha256Hex(const QByteArray &certDer);

    /// Abandon everything already on the wire.
    ///
    /// A request does not stop existing because the operator disconnected: it
    /// finishes, up to a timeout later, and its handler runs against a
    /// connector that has no credentials any more. Bumping the generation here
    /// means those replies are dropped instead of being answered.
    void reset();

    /// Normalize what an operator typed: force https:// when no scheme is
    /// given, strip a trailing slash, keep an explicit port. Returns an empty
    /// string when it cannot be parsed as a host.
    ///
    /// `allowLocalhostHttp` exists for one honest case - a developer or a
    /// self-hosted instance on this machine. Everywhere else plain http is
    /// upgraded, because it is a bearer credential on the wire.
    static QString normalizeBaseUrl(const QString &typed, bool allowLocalhostHttp = false);

    /// GET `path`. `timeoutMs` must exceed any long-poll `wait` in the path.
    void get(const QString &path, Handler done, int timeoutMs = 15000);

    /// POST `body` as JSON to `path`.
    void post(const QString &path, const QJsonObject &body, Handler done, int timeoutMs = 15000);

    /// PUT `body` as JSON to `path`.
    void put(const QString &path, const QJsonObject &body, Handler done, int timeoutMs = 15000);

    /// POST without a bearer token - for the one call some connectors make
    /// before a credential exists.
    void postUnauthenticated(const QString &path, const QJsonObject &body,
                             Handler done, int timeoutMs = 20000);

    /// POST the raw bytes of `filePath` as `application/octet-stream`.
    ///
    /// Streamed from disk, never read into a QByteArray: one log may be
    /// hundreds of megabytes, and holding that in memory on a field laptop
    /// that is also flying an aircraft is not worth doing. The other way this
    /// call is reliably got wrong is putting the file through something that
    /// transforms it - no base64, no JSON string, no multipart wrapper, and
    /// the file is opened in binary.
    ///
    /// The timeout is an IDLE timeout, not a deadline, and that distinction is
    /// the whole point. A 400 MB upload over cellular is minutes, not seconds;
    /// a deadline tuned for JSON calls would abort every large log, and any
    /// deadline generous enough for the worst link would be far too slow to
    /// notice a dead one. Qt resets this on every chunk that moves, so a
    /// slow-but-alive upload runs as long as it needs and a stalled one still
    /// ends.
    ///
    /// Returns the reply so a caller can watch it or abort it. Hold it in a
    /// QPointer - it is deleted once `done` has run.
    QNetworkReply *postFile(const QString &path, const QString &filePath,
                            Handler done, Progress progress = {},
                            int idleTimeoutMs = 60000);

    /// PUT raw file bytes with the digest required by the Fleet Server contract.
    QNetworkReply *putFile(const QString &path, const QString &filePath,
                           const QByteArray &sha256, Handler done,
                           Progress progress = {}, int idleTimeoutMs = 60000);

private:
    void _send(const QString &verb, const QString &path, const QJsonObject *body,
               bool authenticated, Handler done, int timeoutMs);
    QNetworkReply *_sendFile(const QString &verb, const QString &path, const QString &filePath,
                             const QByteArray &sha256, Handler done, Progress progress,
                             int idleTimeoutMs);

    /// Build a request for `path` with the bearer header attached. Every call
    /// that leaves this class goes through here, so the token can only ever be
    /// applied one way.
    QNetworkRequest _request(const QString &path, bool authenticated) const;

    /// Decode every network reply through the same status, payload, and
    /// session-generation handling.
    void _wire(QNetworkReply *reply, Handler done);

    /// Hook the pin check onto `reply`. No-op without a pin.
    void _guardPin(QNetworkReply *reply) const;

    /// True when a pin is set but the base URL is not https; fills `fail`.
    bool _pinNeedsTls(HttpReply &fail) const;

    const TlsPolicy _policy;
    QNetworkAccessManager *_nam = nullptr;
    QString  _baseUrl;
    QString  _token;
    QString  _pin;                  ///< lowercase hex SPKI SHA-256, or empty
    quint64  _generation = 0;       ///< bumped by reset(); stale replies are dropped
};
