/****************************************************************************
 *
 * RotorLab GC TAK integration - manager implementation.
 *
 ****************************************************************************/

#include "TakManager.h"
#include "fleet/GuidedDispatch.h"
#include "CoTMessage.h"

#include <cmath>
#include <limits>

#include <algorithm>

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSet>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <QtCore/qmath.h>
#include <QtPositioning/QGeoCoordinate>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QSslCertificate>
#include <QtNetwork/QSslConfiguration>
#include <QtNetwork/QSslKey>
#include <QtNetwork/QSslSocket>

#include "Fact.h"
#include "FactGroup.h"
#include "MultiVehicleManager.h"
#include "PositionManager.h"
#include "QGCMAVLink.h"
#include "QmlObjectListModel.h"
#include "Vehicle.h"

Q_GLOBAL_STATIC(TakManager, _takManagerInstance)

TakManager *TakManager::instance()
{
    return _takManagerInstance();
}

TakManager::TakManager(QObject *parent)
    : QObject(parent)
{
    _publishTimer.setInterval(1000);
    connect(&_publishTimer, &QTimer::timeout, this, &TakManager::_publishTick);
    _presenceTimer.setInterval(kPresenceRefreshSec * 1000);
    connect(&_presenceTimer, &QTimer::timeout, this, &TakManager::_refreshPresence);

    // Debounced auto-save: every config edit (QML sets a MEMBER property, which
    // auto-emits configChanged) is persisted ~0.5 s later, coalescing bursts.
    // _persist() is silent (emits nothing) so this cannot loop.
    _autoSaveTimer.setSingleShot(true);
    _autoSaveTimer.setInterval(500);
    connect(&_autoSaveTimer, &QTimer::timeout, this, &TakManager::_persist);
    connect(this, &TakManager::configChanged, this, [this]() { _autoSaveTimer.start(); });
    connect(this, &TakManager::configChanged, this, &TakManager::trustStateChanged);
    connect(this, &TakManager::c2ArmedChanged, this, &TakManager::trustStateChanged);
    // A setting changed while connected: publishing turned on starts the
    // publisher, and presence that has just become needed (Read commands
    // turned on, publishing or the beacon turned off) goes out now, not at
    // the next refresh. Nothing on the live link is dropped or reopened.
    connect(this, &TakManager::configChanged, this, [this]() {
        if (!_connected) {
            return;
        }
        if (_publishEnabled && !_publishTimer.isActive()) {
            _publishTimer.start();
            _publishTick();
        }
        _presenceCheck();
    });

    load();
    // The GCS UID is this station's stable EUD identity on the TAK server. Guard
    // against an empty or truncated/hand-edited value (a valid one is
    // "rotorlab-" + 12 hex chars = 21 chars); regenerate so a bad value can't
    // break EUD registration and, with it, drone-marker attribution.
    if (_gcsUid.isEmpty() || _gcsUid.length() < 16) {
        _gcsUid = QStringLiteral("rotorlab-") + QUuid::createUuid().toString(QUuid::Id128).left(12);
        _persist();   // keep this identity stable across launches
    }
}

TakManager::~TakManager()
{
    disconnectServer();
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static QByteArray pemWrapCert(const QString &data)
{
    const QString d = data.trimmed();
    if (d.contains(QStringLiteral("-----BEGIN"))) {
        return d.toUtf8();
    }
    QString body = d;
    body.remove(QRegularExpression(QStringLiteral("\\s")));
    QString out = QStringLiteral("-----BEGIN CERTIFICATE-----\n");
    for (int i = 0; i < body.size(); i += 64) {
        out += body.mid(i, 64) + QLatin1Char('\n');
    }
    out += QStringLiteral("-----END CERTIFICATE-----\n");
    return out.toUtf8();
}

void TakManager::_setStatus(const QString &text)
{
    if (_statusText != text) {
        _statusText = text;
        emit statusTextChanged();
    }
}

void TakManager::_log(const QString &line)
{
    emit logMessage(line);
}

bool TakManager::enrolled() const
{
    return !_clientCertPem.isEmpty() && !_clientKeyPem.isEmpty();
}

void TakManager::setC2Armed(bool armed)
{
    if (_c2Armed != armed) {
        _c2Armed = armed;
        emit c2ArmedChanged();
        _log(armed ? QStringLiteral("\"Commands to aircraft on\" turned on")
                   : QStringLiteral("\"Commands to aircraft on\" turned off"));
    }
}

// ---------------------------------------------------------------------------
// Config + cert persistence
// ---------------------------------------------------------------------------

QString TakManager::_certDir() const
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return base + QStringLiteral("/tak");
}

void TakManager::save()
{
    _persist();
    _log(QStringLiteral("TAK settings saved"));
    emit configChanged();
}

void TakManager::_persist()
{
    QSettings s;
    s.beginGroup(QStringLiteral("TAK"));
    s.setValue(QStringLiteral("host"), _host);
    s.setValue(QStringLiteral("port"), _port);
    s.setValue(QStringLiteral("enrollPort"), _enrollPort);
    s.setValue(QStringLiteral("username"), _username);
    s.setValue(QStringLiteral("password"), _password);   // stored locally in app config so creds survive relaunch
    s.setValue(QStringLiteral("callsignPrefix"), _callsignPrefix);
    s.setValue(QStringLiteral("gcsUid"), _gcsUid);
    s.setValue(QStringLiteral("verifyPeer"), _verifyPeer);
    s.setValue(QStringLiteral("pinnedServerSha256"), _pinnedFingerprint);
    s.setValue(QStringLiteral("publishEnabled"), _publishEnabled);
    s.setValue(QStringLiteral("publishGcsBeacon"), _publishGcsBeacon);
    s.setValue(QStringLiteral("gcsManual"), _gcsManual);
    s.setValue(QStringLiteral("gcsManualLat"), _gcsManualLat);
    s.setValue(QStringLiteral("gcsManualLon"), _gcsManualLon);
    s.setValue(QStringLiteral("gcsManualAlt"), _gcsManualAlt);
    s.setValue(QStringLiteral("publishDirect"), _publishDirect);
    s.setValue(QStringLiteral("directDest"), _directDest);
    s.setValue(QStringLiteral("dronesAddressable"), _dronesAddressable);
    s.setValue(QStringLiteral("commandEnabled"), _commandEnabled);
    s.setValue(QStringLiteral("c2Allowed"), _c2Allowed);
    s.setValue(QStringLiteral("verbGoto"), _verbGoto);
    s.setValue(QStringLiteral("verbRtl"), _verbRtl);
    s.setValue(QStringLiteral("verbLand"), _verbLand);
    s.setValue(QStringLiteral("verbHold"), _verbHold);
    s.setValue(QStringLiteral("verbAltHold"), _verbAltHold);
    s.setValue(QStringLiteral("verbGuided"), _verbGuided);
    s.setValue(QStringLiteral("verbArm"), _verbArm);
    s.setValue(QStringLiteral("verbDisarm"), _verbDisarm);
    s.setValue(QStringLiteral("verbTakeoff"), _verbTakeoff);
    s.setValue(QStringLiteral("altWarnAboveFt"), _altWarnAboveFt);
    s.setValue(QStringLiteral("spdMin"), _spdMin);
    s.setValue(QStringLiteral("spdMax"), _spdMax);
    s.setValue(QStringLiteral("approvedOnly"), _approvedOnly);
    s.setValue(QStringLiteral("approvedPilots"), _approvedPilots);

    QJsonArray fleetArr;
    for (auto it = _fleet.constBegin(); it != _fleet.constEnd(); ++it) {
        QJsonObject o;
        o[QStringLiteral("sysid")]    = it.key();
        o[QStringLiteral("callsign")] = it.value().callsign;
        o[QStringLiteral("videoUrl")] = it.value().videoUrl;
        o[QStringLiteral("tail")]     = it.value().tail;
        fleetArr.append(o);
    }
    s.setValue(QStringLiteral("fleet"),
               QString::fromUtf8(QJsonDocument(fleetArr).toJson(QJsonDocument::Compact)));
    s.endGroup();
}

void TakManager::load()
{
    QSettings s;
    s.beginGroup(QStringLiteral("TAK"));
    _host             = s.value(QStringLiteral("host"), _host).toString();
    _port             = s.value(QStringLiteral("port"), _port).toInt();
    _enrollPort       = s.value(QStringLiteral("enrollPort"), _enrollPort).toInt();
    _username         = s.value(QStringLiteral("username"), _username).toString();
    _password         = s.value(QStringLiteral("password"), _password).toString();
    _callsignPrefix   = s.value(QStringLiteral("callsignPrefix"), _callsignPrefix).toString();
    _gcsUid           = s.value(QStringLiteral("gcsUid"), _gcsUid).toString();
    _verifyPeer       = s.value(QStringLiteral("verifyPeer"), TakDefaults::verifyPeer).toBool();
    _pinnedFingerprint = s.value(QStringLiteral("pinnedServerSha256")).toString();
    _publishEnabled   = s.value(QStringLiteral("publishEnabled"), _publishEnabled).toBool();
    _publishGcsBeacon = s.value(QStringLiteral("publishGcsBeacon"), _publishGcsBeacon).toBool();
    _gcsManual        = s.value(QStringLiteral("gcsManual"), _gcsManual).toBool();
    _gcsManualLat     = s.value(QStringLiteral("gcsManualLat"), _gcsManualLat).toDouble();
    _gcsManualLon     = s.value(QStringLiteral("gcsManualLon"), _gcsManualLon).toDouble();
    _gcsManualAlt     = s.value(QStringLiteral("gcsManualAlt"), _gcsManualAlt).toDouble();
    _publishDirect    = s.value(QStringLiteral("publishDirect"), _publishDirect).toBool();
    _directDest       = s.value(QStringLiteral("directDest"), _directDest).toString();
    _dronesAddressable = s.value(QStringLiteral("dronesAddressable"), _dronesAddressable).toBool();
    _commandEnabled   = s.value(QStringLiteral("commandEnabled"), _commandEnabled).toBool();
    _c2Allowed        = s.value(QStringLiteral("c2Allowed"), _c2Allowed).toBool();
    _verbGoto         = s.value(QStringLiteral("verbGoto"), TakDefaults::verbGoto).toBool();
    _verbRtl          = s.value(QStringLiteral("verbRtl"), TakDefaults::verbRtl).toBool();
    _verbLand         = s.value(QStringLiteral("verbLand"), TakDefaults::verbLand).toBool();
    _verbHold         = s.value(QStringLiteral("verbHold"), TakDefaults::verbHold).toBool();
    _verbAltHold      = s.value(QStringLiteral("verbAltHold"), TakDefaults::verbAltHold).toBool();
    _verbGuided       = s.value(QStringLiteral("verbGuided"), TakDefaults::verbGuided).toBool();
    _verbArm          = s.value(QStringLiteral("verbArm"), TakDefaults::verbArm).toBool();
    _verbDisarm       = s.value(QStringLiteral("verbDisarm"), TakDefaults::verbDisarm).toBool();
    _verbTakeoff      = s.value(QStringLiteral("verbTakeoff"), TakDefaults::verbTakeoff).toBool();
    _altWarnAboveFt   = s.value(QStringLiteral("altWarnAboveFt"), TakDefaults::altWarnAboveFt).toDouble();
    _spdMin           = s.value(QStringLiteral("spdMin"), _spdMin).toDouble();
    _spdMax           = s.value(QStringLiteral("spdMax"), _spdMax).toDouble();
    _approvedOnly     = s.value(QStringLiteral("approvedOnly"), TakDefaults::approvedOnly).toBool();
    _approvedPilots   = s.value(QStringLiteral("approvedPilots"), _approvedPilots).toStringList();

    _fleet.clear();
    const QJsonArray fleetArr =
        QJsonDocument::fromJson(s.value(QStringLiteral("fleet")).toString().toUtf8()).array();
    for (const QJsonValue &v : fleetArr) {
        const QJsonObject o = v.toObject();
        FleetEntry e;
        e.callsign = o.value(QStringLiteral("callsign")).toString();
        e.videoUrl = o.value(QStringLiteral("videoUrl")).toString();
        e.tail     = o.value(QStringLiteral("tail")).toString();
        _fleet.insert(o.value(QStringLiteral("sysid")).toInt(), e);
    }
    s.endGroup();
    _loadCerts();
    emit configChanged();
    emit enrolledChanged();
}

void TakManager::_loadCerts()
{
    const QString dir = _certDir();
    auto readFile = [](const QString &path) -> QByteArray {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    };
    _clientCertPem = readFile(dir + QStringLiteral("/client_cert.pem"));
    _clientKeyPem  = readFile(dir + QStringLiteral("/client_key.pem"));
    _caCertPem     = readFile(dir + QStringLiteral("/ca_cert.pem"));
}

void TakManager::_setPendingFingerprint(const QString &fp, bool fromEnroll)
{
    _pendingFromEnroll = fromEnroll;
    if (_pendingFingerprint != fp) {
        _pendingFingerprint = fp;
        emit fingerprintChanged();
    }
}

void TakManager::acceptServerFingerprint()
{
    if (_pendingFingerprint.isEmpty() || _pendingFingerprint.startsWith(QLatin1Char('('))) {
        return;
    }
    _pinnedFingerprint = _pendingFingerprint;
    _pendingFingerprint.clear();
    _persist();
    emit fingerprintChanged();
    _log(QStringLiteral("Server certificate accepted, SHA-256 %1. Used from the next enrollment or connect.")
             .arg(_pinnedFingerprint));
    if (_pendingFromEnroll) {
        _pendingFromEnroll = false;
        enroll();
    }
}

void TakManager::forgetServerFingerprint()
{
    if (!_pinnedFingerprint.isEmpty()) {
        _pinnedFingerprint.clear();
        _persist();
        emit fingerprintChanged();
        _log(QStringLiteral("Accepted server certificate forgotten (applies at the next enrollment or connect)"));
    }
}

void TakManager::clearCerts()
{
    const QString dir = _certDir();
    for (const QString &n : { QStringLiteral("client_cert.pem"), QStringLiteral("client_key.pem"),
                              QStringLiteral("ca_cert.pem") }) {
        QFile::remove(dir + QLatin1Char('/') + n);
    }
    _clientCertPem.clear();
    _clientKeyPem.clear();
    _caCertPem.clear();
    _log(QStringLiteral("Enrolled TLS material cleared"));
    emit enrolledChanged();
}

QString TakManager::certInfo() const
{
    QStringList parts;
    const QList<QSslCertificate> cc = QSslCertificate::fromData(_clientCertPem, QSsl::Pem);
    if (!cc.isEmpty()) {
        parts << QStringLiteral("client: %1 (expires %2)")
                     .arg(cc.first().subjectDisplayName(),
                          cc.first().expiryDate().toString(QStringLiteral("yyyy-MM-dd")));
    }
    if (!_caCertPem.isEmpty()) {
        const QList<QSslCertificate> ca = QSslCertificate::fromData(_caCertPem, QSsl::Pem);
        parts << QStringLiteral("truststore: %1 CA cert(s)").arg(ca.size());
    }
    return parts.isEmpty() ? QStringLiteral("none") : parts.join(QStringLiteral("   |   "));
}

static void _writePem(const QString &path, const QByteArray &data)
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(data);
    }
}

bool TakManager::importTruststore(const QString &path, const QString &password)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        _log(QStringLiteral("truststore import: cannot open ") + path);
        return false;
    }
    QList<QSslCertificate> cas;
    // TAK truststores are usually PKCS12 (truststore-*.p12); fall back to PEM.
    QSslKey key;
    QSslCertificate cert;
    QList<QSslCertificate> chain;
    if (QSslCertificate::importPkcs12(&f, &key, &cert, &chain, password.toUtf8())) {
        if (!cert.isNull()) {
            cas << cert;
        }
        cas << chain;
    }
    if (cas.isEmpty()) {
        f.seek(0);
        cas = QSslCertificate::fromData(f.readAll(), QSsl::Pem);
    }
    f.close();
    if (cas.isEmpty()) {
        _log(QStringLiteral("truststore import: no certificates found (wrong password?)"));
        return false;
    }
    QByteArray pem;
    for (const QSslCertificate &c : cas) {
        pem += c.toPem();
    }
    _caCertPem = pem;
    QDir().mkpath(_certDir());
    _writePem(_certDir() + QStringLiteral("/ca_cert.pem"), _caCertPem);
    _log(QStringLiteral("truststore imported: %1 CA cert(s)").arg(cas.size()));
    emit enrolledChanged();
    return true;
}

bool TakManager::importClientCert(const QString &path, const QString &password)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        _log(QStringLiteral("client cert import: cannot open ") + path);
        return false;
    }
    QSslKey key;
    QSslCertificate cert;
    QList<QSslCertificate> chain;
    bool ok = QSslCertificate::importPkcs12(&f, &key, &cert, &chain, password.toUtf8())
              && !key.isNull() && !cert.isNull();
    if (!ok) {
        // PEM fallback: a single file carrying both a private key and a cert.
        f.seek(0);
        const QByteArray data = f.readAll();
        const QList<QSslCertificate> certs = QSslCertificate::fromData(data, QSsl::Pem);
        QSslKey k(data, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
        if (k.isNull()) {
            k = QSslKey(data, QSsl::Ec, QSsl::Pem, QSsl::PrivateKey);
        }
        if (!certs.isEmpty() && !k.isNull()) {
            cert = certs.first();
            key = k;
            for (int i = 1; i < certs.size(); ++i) {
                chain << certs.at(i);
            }
            ok = true;
        }
    }
    f.close();
    if (!ok) {
        _log(QStringLiteral("client cert import: need a key + certificate (wrong password?)"));
        return false;
    }
    _clientCertPem = cert.toPem();
    _clientKeyPem  = key.toPem();
    QByteArray ca;
    for (const QSslCertificate &c : chain) {
        ca += c.toPem();
    }
    if (!ca.isEmpty()) {
        _caCertPem = ca;
    }
    QDir().mkpath(_certDir());
    _writePem(_certDir() + QStringLiteral("/client_cert.pem"), _clientCertPem);
    _writePem(_certDir() + QStringLiteral("/client_key.pem"), _clientKeyPem);
    if (!_caCertPem.isEmpty()) {
        _writePem(_certDir() + QStringLiteral("/ca_cert.pem"), _caCertPem);
    }
    _log(QStringLiteral("client identity imported: ") + cert.subjectDisplayName());
    emit enrolledChanged();
    return true;
}

// ---------------------------------------------------------------------------
// OTS certificate enrollment (Marti signClient/v2)
// ---------------------------------------------------------------------------

QString TakManager::_findOpenssl() const
{
    QString exe = QStandardPaths::findExecutable(QStringLiteral("openssl"));
    if (!exe.isEmpty()) {
        return exe;
    }
    const QStringList candidates = {
        QStringLiteral("C:/Program Files/Git/usr/bin/openssl.exe"),
        QStringLiteral("C:/Program Files/Git/mingw64/bin/openssl.exe"),
        QStringLiteral("C:/Program Files (x86)/Git/usr/bin/openssl.exe"),
    };
    for (const QString &c : candidates) {
        if (QFile::exists(c)) {
            return c;
        }
    }
    return QString();
}

bool TakManager::_makeCsr(const QString &cn, QByteArray &keyPemOut, QString &csrB64Out, QString &err)
{
    const QString ossl = _findOpenssl();
    if (ossl.isEmpty()) {
        err = QStringLiteral("openssl not found (install OpenSSL, or it ships with Git for Windows)");
        return false;
    }
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        err = QStringLiteral("could not create temp dir");
        return false;
    }
    const QString keyPath = tmp.path() + QStringLiteral("/k.pem");
    const QString csrPath = tmp.path() + QStringLiteral("/c.pem");

    auto run = [&](const QStringList &args) -> bool {
        QProcess p;
        p.start(ossl, args);
        if (!p.waitForFinished(30000) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
            err = QStringLiteral("openssl ") + args.value(0) + QStringLiteral(" failed: ")
                + QString::fromUtf8(p.readAllStandardError());
            return false;
        }
        return true;
    };

    // Traditional RSA key so Qt's QSslKey parses it reliably, then a CSR from it.
    if (!run({ QStringLiteral("genrsa"), QStringLiteral("-out"), keyPath, QStringLiteral("2048") })) {
        return false;
    }
    if (!run({ QStringLiteral("req"), QStringLiteral("-new"), QStringLiteral("-key"), keyPath,
               QStringLiteral("-out"), csrPath, QStringLiteral("-subj"), QStringLiteral("/CN=") + cn })) {
        return false;
    }

    QFile kf(keyPath), cf(csrPath);
    if (!kf.open(QIODevice::ReadOnly) || !cf.open(QIODevice::ReadOnly)) {
        err = QStringLiteral("could not read generated key/csr");
        return false;
    }
    keyPemOut = kf.readAll();
    const QString csrPem = QString::fromUtf8(cf.readAll());
    QString body;
    for (const QString &line : csrPem.split(QLatin1Char('\n'))) {
        if (!line.contains(QStringLiteral("-----"))) {
            body += line.trimmed();
        }
    }
    csrB64Out = body;
    return true;
}

void TakManager::enroll()
{
    if (_host.isEmpty()) {
        _log(QStringLiteral("enroll: set the server host first"));
        return;
    }
    _setStatus(QStringLiteral("enrolling..."));
    _log(QStringLiteral("OTS enroll: generating key + CSR"));

    QByteArray keyPem;
    QString csrB64, err;
    const QString cn = _username.isEmpty() ? QStringLiteral("rotorlab") : _username;
    if (!_makeCsr(cn, keyPem, csrB64, err)) {
        _setStatus(QStringLiteral("enroll failed"));
        _log(QStringLiteral("enroll: ") + err);
        return;
    }

    const QString url = QStringLiteral("https://%1:%2/Marti/api/tls/signClient/v2?clientUid=%3&version=3")
                            .arg(_host).arg(_enrollPort).arg(_gcsUid);
    QNetworkRequest req{ QUrl(url) };
    // HTTP/1.1 only, so the request (and its Authorization header) is written
    // after the TLS checks below and never negotiated ahead of them.
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("text/plain"));
    const QByteArray auth = (_username + QLatin1Char(':') + _password).toUtf8().toBase64();
    req.setRawHeader("Authorization", "Basic " + auth);

    // The TAK username and password go in this request, so it is sent only
    // over a verified connection (A76): the handshake always checks the
    // certificate (system trust store plus any imported CA), and the checks
    // below run after the handshake and BEFORE any request data is written.
    // An unverified server is shown on the TAK page by its SHA-256
    // fingerprint; the credentials are sent only once the operator accepts it.
    QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
    ssl.setPeerVerifyMode(QSslSocket::VerifyPeer);
    if (!_caCertPem.isEmpty()) {
        ssl.setCaCertificates(ssl.caCertificates()
                              + QSslCertificate::fromData(_caCertPem, QSsl::Pem));
    }
    req.setSslConfiguration(ssl);

    QNetworkAccessManager nam;
    QEventLoop loop;
    QNetworkReply *reply = nam.post(req, csrB64.toUtf8());
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    bool chainVerified = true;
    QString heldFingerprint;   // set when the request was stopped before sending
    const QMetaObject::Connection onSslErrors =
        connect(reply, &QNetworkReply::sslErrors, reply, [&](const QList<QSslError> &errors) {
        Q_UNUSED(errors)
        chainVerified = false;
        // Only the certificate the server presented counts. With none, fail
        // closed: nothing is sent and there is nothing to accept.
        const QSslCertificate peer = reply->sslConfiguration().peerCertificate();
        const QString fp = peer.isNull() ? QString() : TakPolicy::sha256Fingerprint(peer.toDer());
        if (!fp.isEmpty() && TakPolicy::fingerprintsMatch(fp, _pinnedFingerprint)) {
            reply->ignoreSslErrors();     // the certificate the operator accepted
        } else {
            heldFingerprint = fp.isEmpty() ? QStringLiteral("(no certificate)") : fp;
            reply->abort();
        }
    }, Qt::DirectConnection);
    const QMetaObject::Connection onEncrypted =
        connect(reply, &QNetworkReply::encrypted, reply, [&]() {
        const QString fp = TakPolicy::sha256Fingerprint(reply->sslConfiguration().peerCertificate().toDer());
        if (!TakPolicy::enrollMaySend(_verifyPeer, chainVerified, fp, _pinnedFingerprint)) {
            heldFingerprint = fp.isEmpty() ? QStringLiteral("(no certificate)") : fp;
            reply->abort();
        }
    }, Qt::DirectConnection);
    QTimer::singleShot(20000, &loop, &QEventLoop::quit);
    loop.exec();
    // The checks above capture locals: detach them before leaving, and stop a
    // request that is still running after the timeout.
    QObject::disconnect(onSslErrors);
    QObject::disconnect(onEncrypted);
    if (reply->isRunning()) {
        reply->abort();
    }

    if (!heldFingerprint.isEmpty()) {
        _setPendingFingerprint(heldFingerprint, true);
        _setStatus(QStringLiteral("enrollment waiting: server certificate not verified"));
        _log(QStringLiteral("enroll: the TAK username and password were not sent. The server certificate is "
                            "not verified. Its SHA-256 fingerprint is shown on the TAK page; accept it there "
                            "to enroll."));
        reply->deleteLater();
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        _setStatus(QStringLiteral("enroll failed"));
        _log(QStringLiteral("enroll: ") + reply->errorString());
        reply->deleteLater();
        return;
    }
    const QByteArray body = reply->readAll();
    reply->deleteLater();

    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) {
        _setStatus(QStringLiteral("enroll failed"));
        _log(QStringLiteral("enroll: unexpected response (not JSON)"));
        return;
    }
    const QJsonObject obj = doc.object();
    QString signed_ = obj.value(QStringLiteral("signedCert")).toString();
    if (signed_.isEmpty()) {
        signed_ = obj.value(QStringLiteral("cert")).toString();
    }
    if (signed_.isEmpty()) {
        _setStatus(QStringLiteral("enroll failed"));
        _log(QStringLiteral("enroll: no signed cert in response"));
        return;
    }
    QByteArray ca;
    QStringList caKeys;
    for (const QString &k : obj.keys()) {
        if (k.startsWith(QStringLiteral("ca"), Qt::CaseInsensitive)) {
            caKeys << k;
        }
    }
    caKeys.sort();
    for (const QString &k : caKeys) {
        const QString v = obj.value(k).toString();
        if (!v.isEmpty()) {
            ca += pemWrapCert(v);
        }
    }

    _clientCertPem = pemWrapCert(signed_);
    _clientKeyPem  = keyPem;
    if (!ca.isEmpty()) {
        _caCertPem = ca;
    }

    const QString dir = _certDir();
    QDir().mkpath(dir);
    auto writeFile = [](const QString &path, const QByteArray &data) {
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(data);
        }
    };
    writeFile(dir + QStringLiteral("/client_cert.pem"), _clientCertPem);
    writeFile(dir + QStringLiteral("/client_key.pem"), _clientKeyPem);
    if (!_caCertPem.isEmpty()) {
        writeFile(dir + QStringLiteral("/ca_cert.pem"), _caCertPem);
    }

    _setPendingFingerprint(QString(), false);
    _setStatus(QStringLiteral("enrolled"));
    _log(QStringLiteral("OTS enroll: client cert issued and stored"));
    emit enrolledChanged();
}

// ---------------------------------------------------------------------------
// mTLS CoT stream
// ---------------------------------------------------------------------------

void TakManager::connectServer()
{
    if (_socket) {
        disconnectServer();
    }
    if (_host.isEmpty()) {
        _log(QStringLiteral("connect: no host configured"));
        return;
    }
    _socket = new QSslSocket(this);

    QSslConfiguration cfg = QSslConfiguration::defaultConfiguration();
    if (enrolled()) {
        const QSslCertificate cert(_clientCertPem, QSsl::Pem);
        // The key may be RSA (our enrollment) or EC (an imported p12); try both.
        QSslKey key(_clientKeyPem, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
        if (key.isNull()) {
            key = QSslKey(_clientKeyPem, QSsl::Ec, QSsl::Pem, QSsl::PrivateKey);
        }
        if (cert.isNull() || key.isNull()) {
            _log(QStringLiteral("connect: enrolled cert/key failed to load"));
        } else {
            cfg.setLocalCertificate(cert);
            cfg.setPrivateKey(key);
        }
    }
    // Verify on: check the server against the system trust store plus any
    // imported CA, and accept the certificate the operator pinned on the TAK
    // page. With no CA imported this still verifies (it used to fall back to
    // VerifyNone silently); the page warns that no CA is imported. Verify off
    // is the operator's choice and the page says what it means. Either way
    // this applies to this new connection only: a live one is never dropped
    // because of a setting change.
    if (_verifyPeer) {
        if (!_caCertPem.isEmpty()) {
            cfg.setCaCertificates(cfg.caCertificates() + QSslCertificate::fromData(_caCertPem, QSsl::Pem));
        }
        cfg.setPeerVerifyMode(QSslSocket::VerifyPeer);
    } else {
        cfg.setPeerVerifyMode(QSslSocket::VerifyNone);
    }
    _socket->setSslConfiguration(cfg);

    connect(_socket, &QSslSocket::encrypted, this, &TakManager::_onEncrypted);
    connect(_socket, &QSslSocket::readyRead, this, &TakManager::_onReadyRead);
    connect(_socket, &QSslSocket::disconnected, this, &TakManager::_onDisconnected);
    connect(_socket, &QSslSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (_socket) {
            _log(QStringLiteral("socket error: ") + _socket->errorString());
        }
    });
    if (!_verifyPeer) {
        connect(_socket, &QSslSocket::sslErrors, _socket,
                qOverload<const QList<QSslError> &>(&QSslSocket::ignoreSslErrors));
    } else {
        // Runs during the handshake of this new connection, before it is up.
        connect(_socket, &QSslSocket::sslErrors, this, [this](const QList<QSslError> &errors) {
            if (!_socket) {
                return;
            }
            const QString fp = TakPolicy::sha256Fingerprint(_socket->peerCertificate().toDer());
            if (TakPolicy::fingerprintsMatch(fp, _pinnedFingerprint)) {
                _socket->ignoreSslErrors();
                return;
            }
            QStringList why;
            for (const QSslError &e : errors) {
                why << e.errorString();
            }
            _log(QStringLiteral("connect: server certificate not verified (%1). SHA-256 %2. Accept it on the "
                                "TAK page to use it, then connect again.")
                     .arg(why.join(QStringLiteral("; ")), fp));
            _setPendingFingerprint(fp, false);
        }, Qt::DirectConnection);
    }

    _setStatus(QStringLiteral("connecting..."));
    _log(QStringLiteral("connecting to %1:%2").arg(_host).arg(_port));
    _socket->connectToHostEncrypted(_host, static_cast<quint16>(_port));
}

void TakManager::disconnectServer()
{
    _publishTimer.stop();
    _presenceTimer.stop();
    if (_socket) {
        _socket->disconnect(this);
        _socket->abort();
        _socket->deleteLater();
        _socket = nullptr;
    }
    if (_connected) {
        _connected = false;
        emit connectedChanged();
    }
    _setStatus(QStringLiteral("idle"));
}

void TakManager::_onEncrypted()
{
    _connected = true;
    emit connectedChanged();
    _setPendingFingerprint(QString(), false);
    _setStatus(QStringLiteral("connected (mTLS)"));
    _log(QStringLiteral("TLS established with TAK server"));
    if (_cotReceived) {
        _cotReceived = false;
        emit cotReceivedChanged();
    }
    _presenceSent = false;
    _presenceWasNeeded = false;
    _lastPresence.clear();
    _lastBeaconPos = QGeoCoordinate();
    _ownUids.clear();
    if (_publishEnabled) {
        _publishTimer.start();
        _publishTick();          // register the GCS EUD first, before any drone track
    }
    // A TAK server relays CoT only to clients that announced themselves. With
    // "Read commands from ATAK markers" on and the server group not getting
    // the beacon, announce the station now and keep it fresh, so commands
    // reach it.
    _presenceCheck();
    _presenceTimer.start();
}

void TakManager::_onDisconnected()
{
    if (_connected) {
        _connected = false;
        emit connectedChanged();
    }
    _publishTimer.stop();
    _presenceTimer.stop();
    _setStatus(QStringLiteral("disconnected"));
    _log(QStringLiteral("TAK server link closed"));
}

void TakManager::_onReadyRead()
{
    if (!_socket) {
        return;
    }
    _rxBuf += _socket->readAll();
    // Split concatenated <event>...</event> chunks (TAK streams XML back to back).
    for (;;) {
        const int start = _rxBuf.indexOf("<event");
        if (start < 0) {
            _rxBuf.clear();
            break;
        }
        const int end = _rxBuf.indexOf("</event>", start);
        if (end < 0) {
            _rxBuf = _rxBuf.mid(start);        // keep partial for next read
            break;
        }
        const QByteArray chunk = _rxBuf.mid(start, end + 8 - start);
        _rxBuf = _rxBuf.mid(end + 8);
        _noteReceived(chunk);
        if (_commandEnabled) {
            _handleMarker(chunk);
        }
    }
}

// ---------------------------------------------------------------------------
// Publish: vehicle roster + GCS beacon -> CoT
// ---------------------------------------------------------------------------

// The position the GCS beacon broadcasts. Manual (a fixed lat/lon set by the
// operator) wins when enabled and set; otherwise QGC's ground-station position
// (system location, or an NMEA GPS device such as a USB dongle configured in
// General settings); finally the active vehicle's position as a last resort so
// the GCS still registers as the reporting EUD.
QGeoCoordinate TakManager::_effectiveGcsPosition() const
{
    if (_gcsManual && (_gcsManualLat != 0.0 || _gcsManualLon != 0.0)) {
        const QGeoCoordinate m(_gcsManualLat, _gcsManualLon, _gcsManualAlt);
        if (m.isValid()) {
            return m;
        }
    }
    const QGeoCoordinate g = QGCPositionManager::instance()->gcsPosition();
    if (g.isValid()) {
        return g;
    }
    if (Vehicle *av = MultiVehicleManager::instance()->activeVehicle()) {
        if (av->coordinate().isValid()) {
            return av->coordinate();
        }
    }
    return QGeoCoordinate();
}

bool TakManager::captureCurrentLocation()
{
    const QGeoCoordinate g = QGCPositionManager::instance()->gcsPosition();
    if (!g.isValid()) {
        _log(QStringLiteral("No location fix available to capture (set a GPS source in General settings, or enter it manually)"));
        return false;
    }
    _gcsManualLat = g.latitude();
    _gcsManualLon = g.longitude();
    _gcsManualAlt = g.altitude();
    _gcsManual    = true;
    _persist();
    emit configChanged();
    _log(QStringLiteral("Captured GCS location %1, %2").arg(_gcsManualLat, 0, 'f', 6).arg(_gcsManualLon, 0, 'f', 6));
    return true;
}

QString TakManager::gcsPositionText() const
{
    const QGeoCoordinate g = _effectiveGcsPosition();
    if (!g.isValid()) {
        return QStringLiteral("no position (GCS will not beacon until a location is available)");
    }
    const QString src = (_gcsManual && (_gcsManualLat != 0.0 || _gcsManualLon != 0.0))
        ? QStringLiteral("manual")
        : (QGCPositionManager::instance()->gcsPosition().isValid()
               ? QStringLiteral("QGC location / GPS")
               : QStringLiteral("vehicle fallback"));
    return QStringLiteral("%1: %2, %3  (%4 m)")
        .arg(src)
        .arg(g.latitude(), 0, 'f', 6)
        .arg(g.longitude(), 0, 'f', 6)
        .arg(g.altitude(), 0, 'f', 1);
}

// ---------------------------------------------------------------------------
// Fleet registry (per MAVLink sysid metadata published to ATAK)
// ---------------------------------------------------------------------------

QVariantList TakManager::fleetModel() const
{
    // Merge saved sysids with currently-connected vehicles so a new aircraft
    // shows up as a row to configure, and saved rows persist when offline.
    QList<int> sysids = _fleet.keys();
    QSet<int> connected;
    if (QmlObjectListModel *list = MultiVehicleManager::instance()->vehicles()) {
        for (int i = 0; i < list->count(); ++i) {
            if (Vehicle *v = qobject_cast<Vehicle *>((*list)[i])) {
                connected.insert(v->id());
                if (!sysids.contains(v->id())) {
                    sysids << v->id();
                }
            }
        }
    }
    std::sort(sysids.begin(), sysids.end());
    QVariantList rows;
    for (int sysid : sysids) {
        const FleetEntry e = _fleet.value(sysid);
        QVariantMap row;
        row[QStringLiteral("sysid")]     = sysid;
        row[QStringLiteral("callsign")]  = e.callsign;
        row[QStringLiteral("videoUrl")]  = e.videoUrl;
        row[QStringLiteral("tail")]      = e.tail;
        row[QStringLiteral("connected")] = connected.contains(sysid);
        rows << row;
    }
    return rows;
}

void TakManager::setFleetEntry(int sysid, const QString &callsign,
                               const QString &videoUrl, const QString &tail)
{
    if (sysid <= 0) {
        return;
    }
    FleetEntry e;
    e.callsign = callsign.trimmed();
    e.videoUrl = videoUrl.trimmed();
    e.tail     = tail.trimmed();
    if (e.callsign.isEmpty() && e.videoUrl.isEmpty() && e.tail.isEmpty()) {
        _fleet.remove(sysid);
    } else {
        _fleet.insert(sysid, e);
    }
    _persist();
    emit fleetChanged();
    _log(QStringLiteral("Fleet sysid %1 -> %2").arg(sysid).arg(e.callsign.isEmpty() ? QStringLiteral("(cleared)") : e.callsign));
}

void TakManager::removeFleetEntry(int sysid)
{
    if (_fleet.remove(sysid) > 0) {
        _persist();
        emit fleetChanged();
    }
}

void TakManager::refreshFleet()
{
    emit fleetChanged();
}

void TakManager::_noteReceived(const QByteArray &eventXml)
{
    if (_cotReceived) {
        return;
    }
    // Only this station's exact uids are its own. Another station using the
    // same callsign prefix is CoT received.
    const QString uid = CoT::parse(eventXml).uid;
    if (uid.compare(_gcsUid, Qt::CaseInsensitive) == 0 || _ownUids.contains(uid)) {
        return;
    }
    _cotReceived = true;
    emit cotReceivedChanged();
}

void TakManager::_sendEvent(const QByteArray &xml)
{
    if (_recordSent) {
        _sentEvents << xml;
    }
    if (_socket) {
        _socket->write(xml);
        _socket->write("\n");
    }
}

void TakManager::_publishTick()
{
    if (!_connected || !_publishEnabled) {
        return;
    }
    auto send = [this](const QByteArray &xml) { _sendEvent(xml); };

    const QStringList dest = _publishDest();
    const QString gcsCallsign = QStringLiteral("%1-GCS").arg(_callsignPrefix);

    // GCS self-beacon: a self-describing EUD. Its uid == the enrolled clientUid,
    // so OTS binds the TLS session to this EUD. This MUST be published (and be
    // the first identifying event on the link): TAK servers attribute every CoT
    // on a connection to the connection's EUD, so a registered GCS is what lets
    // drone air-tracks be stored/rendered as markers parented to it. If the GCS
    // has no position fix, fall back to the primary vehicle's position so the
    // GCS still registers rather than letting a drone become the link identity.
    // With no position at all, the last real beacon of this link is sent again.
    _sendBeacon(dest);
    // Presence (no position) before any drone track, when it is needed.
    _presenceCheck();

    // Drones: air tracks reported by (and linked to) the GCS EUD.
    QmlObjectListModel *list = MultiVehicleManager::instance()->vehicles();
    for (int i = 0; list && i < list->count(); ++i) {
        Vehicle *v = qobject_cast<Vehicle *>((*list)[i]);
        if (!v) {
            continue;
        }
        const QGeoCoordinate c = v->coordinate();
        if (!c.isValid()) {
            continue;
        }
        double course = 0.0, speed = 0.0;
        if (FactGroup *fg = v->vehicleFactGroup()) {
            if (Fact *f = fg->getFact(QStringLiteral("heading")))     course = f->rawValue().toDouble();
            if (Fact *f = fg->getFact(QStringLiteral("groundSpeed"))) speed  = f->rawValue().toDouble();
        }
        // Fleet metadata for this sysid (operator-assigned callsign / video / tail).
        // The uid stays PREFIX-<sysid> (stable identity + loop-prevention); the
        // callsign is the display label ATAK shows, so renaming doesn't respawn.
        const FleetEntry fe = _fleet.value(v->id());
        const QString uid = QStringLiteral("%1-%2").arg(_callsignPrefix).arg(v->id());
        const QString cs  = fe.callsign.isEmpty() ? uid : fe.callsign;
        CoT::Entity e;
        e.uid            = uid;
        e.callsign       = cs;
        e.cotType        = CoT::typeForVehicle(static_cast<int>(v->vehicleType()));
        e.lat            = c.latitude();
        e.lon            = c.longitude();
        e.hae            = c.altitude();
        e.course         = course;
        e.speed          = speed;
        e.hasTrack       = true;
        e.addressable    = _dronesAddressable;     // send-to-able for send-to-drone commands
        e.selfEud        = false;                  // a drone is NOT a TAK device
        e.parentUid      = _gcsUid;                // association to the reporting EUD
        e.parentCallsign = gcsCallsign;
        e.videoUrl       = fe.videoUrl;            // ATAK plays this feed on the marker (if set)
        e.videoAlias     = cs;
        e.remark         = fe.tail.isEmpty()
            ? QStringLiteral("RotorLab sysid %1 (via %2)").arg(v->id()).arg(gcsCallsign)
            : QStringLiteral("RotorLab sysid %1  tail %2  (via %3)").arg(v->id()).arg(fe.tail, gcsCallsign);
        e.dest           = dest;
        send(CoT::build(e));
        _ownUids.insert(uid);
    }
    if (_socket) {
        _socket->flush();
    }
}

// Broadcast (empty) or direct to the configured EUD callsigns.
QStringList TakManager::_publishDest() const
{
    return _publishDirect ? _directDest.split(QLatin1Char(','), Qt::SkipEmptyParts) : QStringList();
}

CoT::Entity TakManager::_stationEntity(const QGeoCoordinate &pos, int staleSeconds,
                                       const QStringList &dest) const
{
    CoT::Entity e;
    e.uid          = _gcsUid;
    e.callsign     = QStringLiteral("%1-GCS").arg(_callsignPrefix);
    e.cotType      = QStringLiteral("a-f-G-U-C");
    e.staleSeconds = staleSeconds;
    e.positionKnown = pos.isValid();
    if (e.positionKnown) {
        e.lat = pos.latitude();
        e.lon = pos.longitude();
        e.hae = pos.altitude();
    }
    e.hasTrack     = false;
    e.addressable  = true;
    e.selfEud      = true;                 // the GCS is the TAK EUD
    e.remark       = e.positionKnown ? QStringLiteral("RotorLab ground station")
                                     : QStringLiteral("RotorLab ground station (position not shared)");
    e.dest         = dest;
    return e;
}

// The station position, else the last real position a beacon carried on
// this link. A vehicle link drop or a lost fix keeps the last real point
// fresh; the beacon never switches to the unknown point.
QGeoCoordinate TakManager::_beaconPosition() const
{
    const QGeoCoordinate g = _effectiveGcsPosition();
    return g.isValid() ? g : _lastBeaconPos;
}

// The beacon goes out from _publishTick only with publishing on, "Publish GCS
// beacon" on and a real position now or earlier on this link. Short-circuits
// so the position is read only then.
bool TakManager::_beaconAnnounces() const
{
    return _publishEnabled && _publishGcsBeacon && _beaconPosition().isValid();
}

bool TakManager::_sendBeacon(const QStringList &dest)
{
    if (!_connected || !_publishEnabled || !_publishGcsBeacon) {
        return false;
    }
    const QGeoCoordinate g = _effectiveGcsPosition();
    if (g.isValid()) {
        _lastBeaconPos = g;
    }
    if (!_lastBeaconPos.isValid()) {
        return false;
    }
    _sendEvent(CoT::build(_stationEntity(_lastBeaconPos, 30, dest)));
    return true;
}

// The server group must get the beacon or the presence. A beacon sent only to
// direct recipients does not count.
bool TakManager::_presenceNeeded() const
{
    return _commandEnabled && (!_beaconAnnounces() || !_publishDest().isEmpty());
}

void TakManager::_refreshPresence()
{
    if (_beaconAnnounces() && !_publishDest().isEmpty()) {
        return;
    }
    _announcePresence();
}

void TakManager::_presenceCheck()
{
    if (!_connected) {
        return;
    }
    const bool need = _presenceNeeded();
    if (need && !_presenceWasNeeded) {
        _announcePresence();
    }
    _presenceWasNeeded = need;
}

// Presence: who this station is, never where. Sharing the station's location
// is the GCS beacon's job, which runs only with publishing on. No drone
// marker is ever part of it. It carries no <marti><dest>: addressed only to
// the direct recipients, the TAK server might never register the station
// and commands would again not arrive.
QByteArray TakManager::_presenceEvent() const
{
    return CoT::build(_stationEntity(QGeoCoordinate(), kPresenceStaleSec, QStringList()));
}

void TakManager::_announcePresence()
{
    if (!_presenceNeeded()) {
        return;
    }
    _lastPresence = _presenceEvent();
    if (!_connected) {
        return;
    }
    _sendEvent(_lastPresence);
    // Direct mode with the beacon on (sent once per connect or change, see
    // _refreshPresence): the beacon follows at once, so its recipients end
    // on the real position.
    if (_beaconAnnounces()) {
        _sendBeacon(_publishDest());
    }
    if (_socket) {
        _socket->flush();
    }
    if (!_presenceSent) {
        _presenceSent = true;
        _log(QStringLiteral("Station presence sent to the TAK server (uid %1, no position) so it relays "
                            "ATAK commands to this station").arg(_gcsUid));
    }
}

// ---------------------------------------------------------------------------
// Command By ATAK: marker -> parse -> guard -> dispatch
// ---------------------------------------------------------------------------

void TakManager::_handleMarker(const QByteArray &eventXml)
{
    const CoT::Marker m = CoT::parse(eventXml);
    if (!m.valid) {
        return;
    }
    // Loop prevention: ignore our own published entities (their uid carries our prefix).
    if (m.uid.startsWith(_callsignPrefix, Qt::CaseInsensitive)
        || m.uid.compare(_gcsUid, Qt::CaseInsensitive) == 0) {
        return;
    }

    // A named marker whose callsign carries a verb, e.g. "GOTO_2_ALT_150".
    if (!m.callsign.isEmpty()) {
        Tak::Command cmd;
        QString err;
        if (Tak::parseCommand(m.callsign, cmd, err)) {
            const QString result = _receiveCommand(cmd, m.lat, m.lon, m.callsign,
                                                   m.senderCallsign, m.senderUid, false, eventXml);
            _log(QStringLiteral("ATAK %1 -> %2").arg(m.callsign, result));
            return;
        }
    }
    // A plain point "sent to" one of our drones: <marti><dest callsign="ROTOR-3"/>.
    if (!m.destCallsign.isEmpty()) {
        const QRegularExpression re(QStringLiteral("^%1-(\\d+)$").arg(QRegularExpression::escape(_callsignPrefix)),
                                    QRegularExpression::CaseInsensitiveOption);
        const auto mo = re.match(m.destCallsign.trimmed());
        if (mo.hasMatch()) {
            Tak::Command cmd;
            cmd.verb = Tak::Verb::Goto;
            cmd.target = Tak::Target::Ids;
            cmd.ids = { mo.captured(1).toInt() };
            const QString result = _receiveCommand(cmd, m.lat, m.lon,
                                                   QStringLiteral("point sent to ") + m.destCallsign,
                                                   m.senderCallsign, m.senderUid, false, eventXml);
            _log(QStringLiteral("ATAK send-to %1 -> %2").arg(m.destCallsign, result));
        }
    }
}

// Every parsed command, from a marker or from the station's test button, goes
// through here: the same station checks in the same order, then the shared
// dispatcher, then the station alert. The alert is written after dispatch and
// never holds, delays or cancels the command.
QString TakManager::_receiveCommand(const Tak::Command &cmd, double lat, double lon,
                                    const QString &markerText, const QString &senderCallsign,
                                    const QString &senderUid, bool test, const QByteArray &payload)
{
    const QString receivedUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    // What the station had set when the command arrived, for the command log.
    QJsonObject settings;
    settings[QStringLiteral("readCommands")]   = _commandEnabled;
    settings[QStringLiteral("c2On")]           = _c2Allowed;
    settings[QStringLiteral("c2UplinkArmed")]  = _c2Armed;
    settings[QStringLiteral("approvedOnly")]   = _approvedOnly;
    settings[QStringLiteral("verbEnabled")]    = _verbEnabled(cmd.verb);
    settings[QStringLiteral("altWarnAboveFt")] = _altWarnAboveFt;

    TakPolicy::RemoteCommand rc;
    rc.verb           = Tak::verbName(cmd.verb);
    rc.markerText     = markerText;
    rc.senderCallsign = senderCallsign;
    rc.senderUid      = senderUid;
    rc.test           = test;
    if (cmd.verb == Tak::Verb::Goto) {
        rc.position = TakPolicy::positionText(lat, lon, cmd.altFt);
    }

    DispatchResult r;
    QString who;
    if (!_commandEnabled) {
        r.msg = TakPolicy::notSentResult(QStringLiteral("\"Read commands from ATAK markers\" is off"));
    } else if (!test && TakPolicy::claimsTestSender(senderCallsign, senderUid)) {
        r.msg = TakPolicy::testIdentityResult();
    } else if (!_senderApproved(senderCallsign, senderUid, who)) {
        r.msg = TakPolicy::notOnListResult();
        if (test) {
            r.msg += QStringLiteral(". To test, add device UID %1").arg(TakPolicy::testSenderUid());
        }
    } else {
        r = _execute(cmd, lat, lon, test ? QStringLiteral("test-inject") : QStringLiteral("atak ") + markerText);
    }

    rc.targets = r.targets.isEmpty() ? QStringList{ TakPolicy::intendedTargetText(cmd) } : r.targets;
    rc.result  = r.alertMsg.isEmpty() ? r.msg : r.alertMsg;
    rc.sent    = r.sent;
    rc.warning = TakPolicy::sentAltitudeWarning(r.sent, cmd.altFt, _altWarnAboveFt);

    QJsonObject ctx;
    ctx[QStringLiteral("receivedUtc")]   = receivedUtc;
    ctx[QStringLiteral("dispatchedUtc")] = r.attempted
        ? QJsonValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)) : QJsonValue();
    ctx[QStringLiteral("source")]        = test ? QStringLiteral("station test button")
                                                : QStringLiteral("TAK server %1:%2 (TLS CoT stream)").arg(_host).arg(_port);
    ctx[QStringLiteral("rawSha256")]     =
        QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());
    QJsonObject params;
    params[QStringLiteral("lat")] = lat;
    params[QStringLiteral("lon")] = lon;
    if (cmd.altFt >= 0) {
        params[QStringLiteral("alt")]      = cmd.altFt;
        params[QStringLiteral("altUnit")]  = QStringLiteral("ft");
        params[QStringLiteral("altFrame")] = QStringLiteral("above launch");
    }
    if (cmd.spd >= 0) {
        params[QStringLiteral("spd")] = cmd.spd;
    }
    ctx[QStringLiteral("params")] = params;
    // QGC's vehicle id is the MAVLink system id; both are written so the
    // record does not depend on knowing that.
    QJsonArray aircraft;
    for (int id : r.sysids) {
        QJsonObject a;
        a[QStringLiteral("vehicleId")] = id;
        a[QStringLiteral("sysid")]     = id;
        aircraft.append(a);
    }
    ctx[QStringLiteral("aircraft")] = aircraft;
    ctx[QStringLiteral("settings")]       = settings;

    _recordRemoteCommand(rc, r.attempted && (TakPolicy::isProminentVerb(cmd.verb) || !rc.warning.isEmpty()), ctx);
    return r.msg;
}

Vehicle *TakManager::_vehicleById(int sysid) const
{
    QmlObjectListModel *list = MultiVehicleManager::instance()->vehicles();
    for (int i = 0; list && i < list->count(); ++i) {
        Vehicle *v = qobject_cast<Vehicle *>((*list)[i]);
        if (v && v->id() == sysid) {
            return v;
        }
    }
    return nullptr;
}

QList<Vehicle *> TakManager::_resolveTargets(const Tak::Command &cmd) const
{
    QList<Vehicle *> out;
    MultiVehicleManager *mvm = MultiVehicleManager::instance();
    if (cmd.target == Tak::Target::All) {
        QmlObjectListModel *list = mvm->vehicles();
        for (int i = 0; list && i < list->count(); ++i) {
            if (Vehicle *v = qobject_cast<Vehicle *>((*list)[i])) {
                out << v;
            }
        }
    } else if (cmd.target == Tak::Target::Sel) {
        QmlObjectListModel *list = mvm->selectedVehicles();
        for (int i = 0; list && i < list->count(); ++i) {
            if (Vehicle *v = qobject_cast<Vehicle *>((*list)[i])) {
                out << v;
            }
        }
    } else {
        for (int id : cmd.ids) {
            if (Vehicle *v = _vehicleById(id)) {
                out << v;
            }
        }
    }
    return out;
}

void TakManager::addApprovedPilot(const QString &callsign)
{
    addApprovedPilotAs(callsign, QStringLiteral("any"));
}

void TakManager::addApprovedPilotAs(const QString &value, const QString &kind)
{
    const TakPolicy::PilotKind k = TakPolicy::pilotKindFromName(kind);
    const QString stored = TakPolicy::formatPilotEntry(k, value);
    if (value.trimmed().isEmpty()) {
        return;
    }
    for (const QString &e : _approvedPilots) {
        if (e.compare(stored, Qt::CaseInsensitive) == 0) {
            return;
        }
    }
    _approvedPilots << stored;
    _log(QStringLiteral("ATAK command senders: added %1").arg(stored));
    emit configChanged();
}

QVariantList TakManager::approvedPilotRows() const
{
    QVariantList rows;
    for (const QString &stored : _approvedPilots) {
        const TakPolicy::PilotEntry e = TakPolicy::parsePilotEntry(stored);
        QVariantMap row;
        row[QStringLiteral("entry")] = stored;
        row[QStringLiteral("kind")]  = TakPolicy::pilotKindName(e.kind);
        row[QStringLiteral("value")] = e.value;
        rows << row;
    }
    return rows;
}

void TakManager::removeApprovedPilot(const QString &callsign)
{
    for (int i = 0; i < _approvedPilots.size(); ++i) {
        if (_approvedPilots.at(i).compare(callsign.trimmed(), Qt::CaseInsensitive) == 0) {
            _approvedPilots.removeAt(i);
            _log(QStringLiteral("ATAK command senders: removed %1").arg(callsign.trimmed()));
            emit configChanged();
            return;
        }
    }
}

// Sender allowlist. The sender is the EUD that placed the marker: ATAK stamps
// command markers with a <link relation="p-p"> carrying the device uid and the
// operator callsign. Both are what the sending device reports about itself;
// nothing on the client socket authenticates them (there is no per-sender
// server identity here). A "uid:" entry matches the reported device UID only,
// a "callsign:" entry the reported callsign only, an older unprefixed entry
// either, all case-insensitively. A marker with no parent link is an unknown
// sender and is refused while the allowlist is enforced.
bool TakManager::_senderApproved(const CoT::Marker &m, QString &whoOut) const
{
    return _senderApproved(m.senderCallsign, m.senderUid, whoOut);
}

bool TakManager::_senderApproved(const QString &senderCallsign, const QString &senderUid,
                                 QString &whoOut) const
{
    whoOut = TakPolicy::senderText(senderCallsign, senderUid);
    if (!_approvedOnly) {
        return true;
    }
    return TakPolicy::senderOnList(_approvedPilots, senderCallsign, senderUid);
}

bool TakManager::_verbEnabled(Tak::Verb v) const
{
    switch (v) {
    case Tak::Verb::Goto:    return _verbGoto;
    case Tak::Verb::Rtl:     return _verbRtl;
    case Tak::Verb::Land:    return _verbLand;
    case Tak::Verb::Hold:    return _verbHold;
    case Tak::Verb::AltHold: return _verbAltHold;
    case Tak::Verb::Guided:  return _verbGuided;
    case Tak::Verb::Arm:     return _verbArm;
    case Tak::Verb::Disarm:  return _verbDisarm;
    case Tak::Verb::Takeoff: return _verbTakeoff;
    case Tak::Verb::Invalid: return false;
    }
    return false;
}

// Dispatch a single vehicle through the SAME guided-mode entry points the GCS
// UI uses. No parallel geofence / arm / mode gate here: whatever QGC and the
// vehicle enforce for an operator action applies identically to an ATAK one.
TakManager::DispatchResult TakManager::_dispatchOne(Vehicle *veh, const Tak::Command &cmd,
                                                    double lat, double lon)
{
    // Convert at this boundary, then hand off to the one shared dispatcher.
    //
    // The marker grammar THIS path parses is `ALT <ft>` and `SPD <1-10>`, so
    // feet and a notch. The Fleet Server route receives the same order already
    // normalised to meters HAE and m/s. Both end up in Guided::dispatch, which
    // speaks SI only - so an operator gets identical behaviour whichever route
    // their order took, and a fix to the flight logic cannot land in one path
    // and miss the other.
    // ALT here is height above the LAUNCH point in feet, which is what an
    // operator means by "ALT 400" over a marker. Launch AMSL is the aircraft's
    // AMSL minus its height above launch, so this needs no geoid model and no
    // ellipsoid reading - exactly as this path has always worked.
    Guided::Alt alt = Guided::Alt::none();
    if (cmd.altFt >= 0) {
        const double curAMSL = veh->altitudeAMSL()     ? veh->altitudeAMSL()->rawValue().toDouble()     : 0.0;
        const double curRel  = veh->altitudeRelative() ? veh->altitudeRelative()->rawValue().toDouble() : 0.0;
        alt = Guided::Alt::amsl((curAMSL - curRel) + cmd.altFt * 0.3048);
    }

    const double speedMs = (cmd.spd >= 0)
        ? Tak::spdToMetersPerSecond(cmd.spd, _spdMin, _spdMax)
        : std::nan("");

    const Guided::Verb v = _guidedVerb(cmd.verb);
    if (v != Guided::Verb::Invalid) {
        const Guided::Result r = Guided::dispatch(veh, v, lat, lon, alt, speedMs);
        return { r.ok, r.detail };
    }
    return { false, QStringLiteral("invalid verb") };
}

Guided::Verb TakManager::_guidedVerb(Tak::Verb v)
{
    switch (v) {
    case Tak::Verb::Goto:    return Guided::Verb::Goto;
    case Tak::Verb::Takeoff: return Guided::Verb::Takeoff;
    case Tak::Verb::Rtl:     return Guided::Verb::Rtl;
    case Tak::Verb::Land:    return Guided::Verb::Land;
    case Tak::Verb::Hold:    return Guided::Verb::Hold;
    case Tak::Verb::Guided:  return Guided::Verb::Guided;
    case Tak::Verb::Arm:     return Guided::Verb::Arm;
    case Tak::Verb::Disarm:  return Guided::Verb::Disarm;
    case Tak::Verb::AltHold: return Guided::Verb::AltHold;
    case Tak::Verb::Invalid: break;
    }
    return Guided::Verb::Invalid;
}


TakManager::DispatchResult TakManager::_execute(const Tak::Command &cmd, double lat, double lon,
                                                const QString &source)
{
    auto refused = [](const QString &why) {
        DispatchResult r;
        r.msg      = why;
        r.alertMsg = TakPolicy::notSentResult(why);
        return r;
    };
    if (!_c2Allowed) {
        return refused(QStringLiteral("\"Command By ATAK on\" is off (TAK settings)"));
    }
    if (!_c2Armed) {
        return refused(QStringLiteral("\"Commands to aircraft on\" is off (TAK settings)"));
    }
    // The trust boundary: only verbs the operator has enabled are honoured.
    if (!_verbEnabled(cmd.verb)) {
        _log(QStringLiteral("refused %1 (verb disabled) from %2").arg(Tak::verbName(cmd.verb), source));
        return refused(Tak::verbName(cmd.verb) + QStringLiteral(" is disabled"));
    }

    const QList<Vehicle *> targets = _resolveTargets(cmd);
    if (targets.isEmpty()) {
        return refused(QStringLiteral("no matching tracks"));
    }

    QStringList notes;
    QStringList sent;
    int sentCount = 0;
    QStringList labels;
    QList<int>  sysids;
    bool anyOk = false;
    bool anySent = false;
    for (Vehicle *veh : targets) {
        // The one state the dispatcher reads back: ARM on an aircraft that
        // already reports armed sends nothing.
        const bool alreadyArmed = cmd.verb == Tak::Verb::Arm && veh->armed();
        const DispatchResult r = _dispatchOne(veh, cmd, lat, lon);
        anyOk = anyOk || r.ok;
        anySent = anySent || (r.ok && !alreadyArmed);
        notes << QStringLiteral("%1 %2").arg(veh->id()).arg(r.msg);
        sentCount += (r.ok && !alreadyArmed) ? 1 : 0;
        sent << TakPolicy::sentResultText(cmd.verb, r.ok, r.msg, alreadyArmed && r.ok);
        labels << _targetLabel(veh);
        sysids << veh->id();
    }
    DispatchResult out;
    out.ok        = anyOk;
    out.msg       = notes.join(QStringLiteral("; "));
    out.alertMsg  = TakPolicy::dispatchResultText(sysids, sent, sentCount);
    out.attempted = true;
    out.targets   = labels;
    out.sysids    = sysids;
    out.sent      = anySent;
    return out;
}

QString TakManager::_targetLabel(Vehicle *veh) const
{
    const QString cs = _fleet.value(veh->id()).callsign;
    return cs.isEmpty() ? QStringLiteral("aircraft %1").arg(veh->id())
                        : QStringLiteral("aircraft %1 (%2)").arg(veh->id()).arg(cs);
}

// ---------------------------------------------------------------------------
// Remote commands received: the station alert (A80). Informs only.
//
// Every entry is written AFTER the command has been handed to the shared
// dispatcher (or refused by a station check), so recording it can never hold,
// delay or cancel what the remote pilot sent. There is no cancel window and
// no modal. ARM, TKOFF and GOTO that reached the dispatcher (and any command
// with an altitude warning) also raise a banner over the fly view, which the
// operator dismisses.
//
// Each entry is also appended to the command log, one JSON object per line.
// Nothing in the app deletes or rewrites that file; "Clear view" on the TAK
// page empties the on-screen list only.
// ---------------------------------------------------------------------------

QString TakManager::commandLogPath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
         + QStringLiteral("/tak/remote-commands.jsonl");
}

// One line per event. Each line carries the SHA-256 of the line before it
// (prevHash), so an edit or a removed line is detectable by re-hashing; the
// file is still an ordinary file, not a tamper-proof record. A write that
// fails is reported on the TAK page and never touches the command itself,
// which has already gone to the dispatcher.
void TakManager::_appendCommandLog(QJsonObject record)
{
    const QString path = commandLogPath();
    if (!_logHashLoaded) {
        _logHashLoaded = true;
        QFile in(path);
        if (in.open(QIODevice::ReadOnly)) {
            const qint64 tail = 256 * 1024;
            if (in.size() > tail) {
                in.seek(in.size() - tail);
            }
            const QList<QByteArray> lines = in.readAll().split('\n');
            for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
                if (!it->trimmed().isEmpty()) {
                    _lastLogHash = TakPolicy::lineHash(it->trimmed());
                    break;
                }
            }
        }
    }
    record[QStringLiteral("id")]         = QUuid::createUuid().toString(QUuid::WithoutBraces);
    record[QStringLiteral("appVersion")] = QCoreApplication::applicationVersion();
    record[QStringLiteral("stationId")]  = _gcsUid;   // this install's stable TAK identity
    record[QStringLiteral("clock")]      = QStringLiteral("station system clock");
    const QByteArray line = TakPolicy::chainRecord(record, _lastLogHash);

    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    const bool ok = f.open(QIODevice::WriteOnly | QIODevice::Append)
                    && f.write(line + '\n') == line.size() + 1
                    && f.flush();
    if (ok) {
        _lastLogHash = TakPolicy::lineHash(line);
    } else {
        _log(QStringLiteral("Command log could not be written: ") + path);
    }
    if (_commandLogError == ok) {   // error state changed
        _commandLogError = !ok;
        emit commandLogErrorChanged();
    }
}

void TakManager::_recordRemoteCommand(const TakPolicy::RemoteCommand &c, bool banner,
                                      const QJsonObject &context)
{
    const QDateTime now = QDateTime::currentDateTime();
    QVariantMap e;
    e[QStringLiteral("id")]             = _nextRemoteId++;
    e[QStringLiteral("title")]          = TakPolicy::alertTitle();
    e[QStringLiteral("verb")]           = c.verb;
    e[QStringLiteral("marker")]         = c.markerText;
    e[QStringLiteral("senderCallsign")] = c.senderCallsign;
    e[QStringLiteral("senderUid")]      = c.senderUid;
    e[QStringLiteral("sender")]         = TakPolicy::senderText(c.senderCallsign, c.senderUid);
    e[QStringLiteral("targets")]        = c.targets.join(QStringLiteral(", "));
    e[QStringLiteral("position")]       = c.position;
    e[QStringLiteral("result")]         = c.result;
    e[QStringLiteral("warning")]        = c.warning;
    e[QStringLiteral("test")]           = c.test;
    e[QStringLiteral("sent")]           = c.sent;
    e[QStringLiteral("time")]           = now.toString(QStringLiteral("HH:mm:ss"));
    e[QStringLiteral("date")]           = now.toString(QStringLiteral("yyyy-MM-dd"));
    e[QStringLiteral("timeUtc")]        = now.toUTC().toString(Qt::ISODate);
    e[QStringLiteral("summary")]        = TakPolicy::summaryLine(c);
    e[QStringLiteral("banner")]         = banner;     // shown as a banner until dismissed
    QJsonObject rec = TakPolicy::logRecord(c);
    for (auto it = context.constBegin(); it != context.constEnd(); ++it) {
        rec.insert(it.key(), it.value());
    }
    rec[QStringLiteral("timeUtc")]   = e.value(QStringLiteral("timeUtc")).toString();
    rec[QStringLiteral("timeLocal")] = now.toString(Qt::ISODate);
    rec[QStringLiteral("listId")]    = e.value(QStringLiteral("id")).toInt();
    rec[QStringLiteral("banner")]    = banner;
    _appendCommandLog(rec);
    _remoteCommands.prepend(e);
    while (_remoteCommands.size() > kMaxRemoteCommands) {
        _remoteCommands.removeLast();
    }
    _log(QStringLiteral("%1 %2: %3").arg(e.value(QStringLiteral("time")).toString(),
                                        TakPolicy::alertTitle(),
                                        e.value(QStringLiteral("summary")).toString()));
    if (!c.warning.isEmpty()) {
        _log(QStringLiteral("  warning: ") + c.warning);
    }
    emit remoteCommandsChanged();
    emit remoteCommandReceived(e);
}

QVariantList TakManager::bannerAlerts() const
{
    QVariantList out;
    for (const QVariant &v : _remoteCommands) {
        if (v.toMap().value(QStringLiteral("banner")).toBool()) {
            out << v;
        }
    }
    return out;
}

void TakManager::dismissBanner(int id)
{
    for (int i = 0; i < _remoteCommands.size(); ++i) {
        QVariantMap e = _remoteCommands.at(i).toMap();
        if (e.value(QStringLiteral("id")).toInt() == id) {
            if (e.value(QStringLiteral("banner")).toBool()) {
                e[QStringLiteral("banner")] = false;
                _remoteCommands[i] = e;
                emit remoteCommandsChanged();
                QJsonObject rec;
                rec[QStringLiteral("kind")]    = QStringLiteral("banner dismissed");
                rec[QStringLiteral("timeUtc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
                rec[QStringLiteral("listId")]  = id;
                rec[QStringLiteral("verb")]    = e.value(QStringLiteral("verb")).toString();
                _appendCommandLog(rec);
            }
            return;
        }
    }
}

void TakManager::clearRemoteCommands()
{
    if (!_remoteCommands.isEmpty()) {
        _remoteCommands.clear();
        emit remoteCommandsChanged();
    }
}

QString TakManager::injectTestCommand(const QString &markerCallsign)
{
    Tak::Command cmd;
    QString err;
    if (!Tak::parseCommand(markerCallsign, cmd, err)) {
        // Listed and logged like any other command, so a test the station
        // could not read does not vanish silently. Nothing is dispatched.
        TakPolicy::RemoteCommand rc;
        if (cmd.verb != Tak::Verb::Invalid) {
            rc.verb = Tak::verbName(cmd.verb);
        } else {
            const QStringList toks = markerCallsign.trimmed().toUpper()
                                         .split(QRegularExpression(QStringLiteral("[\\s_-]+")), Qt::SkipEmptyParts);
            rc.verb = toks.isEmpty() ? QStringLiteral("(empty)") : toks.first();
        }
        rc.markerText     = markerCallsign;
        rc.senderCallsign = TakPolicy::testSenderCallsign();
        rc.senderUid      = TakPolicy::testSenderUid();
        rc.targets        = { QStringLiteral("no aircraft named") };
        rc.result         = TakPolicy::parseFailedResult(err);
        rc.test           = true;
        QJsonObject ctx;
        ctx[QStringLiteral("receivedUtc")]   = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        ctx[QStringLiteral("dispatchedUtc")] = QJsonValue();
        ctx[QStringLiteral("source")]        = QStringLiteral("station test button");
        ctx[QStringLiteral("rawSha256")]     = QString::fromLatin1(
            QCryptographicHash::hash(markerCallsign.toUtf8(), QCryptographicHash::Sha256).toHex());
        _recordRemoteCommand(rc, false, ctx);
        const QString line = QStringLiteral("TEST %1 -> %2").arg(markerCallsign, rc.result);
        _log(line);
        return line;
    }
    // Place the synthetic point ~80 m north of the first target so a GOTO
    // visibly moves the aircraft. Runs the real receive -> checks -> dispatch
    // path, as the station's test sender (see TakPolicy::testSenderUid).
    double lat = 0.0, lon = 0.0;
    const QList<Vehicle *> targets = _resolveTargets(cmd);
    if (!targets.isEmpty() && targets.first()->coordinate().isValid()) {
        const QGeoCoordinate c = targets.first()->coordinate();
        lat = c.latitude() + 80.0 / 111320.0;
        lon = c.longitude();
    }
    const QString result = _receiveCommand(cmd, lat, lon, markerCallsign,
                                           TakPolicy::testSenderCallsign(),
                                           TakPolicy::testSenderUid(), true, markerCallsign.toUtf8());
    const QString line = QStringLiteral("TEST %1 -> %2").arg(markerCallsign, result);
    _log(line);
    return line;
}

// ---------------------------------------------------------------------------
// Offline self-test (mirrors validate_cot.py's offline + verb checks)
// ---------------------------------------------------------------------------

QString TakManager::runSelfTest()
{
    QStringList lines;
    int pass = 0, total = 0;
    auto check = [&](const QString &name, bool ok) {
        ++total;
        if (ok) ++pass;
        lines << QStringLiteral("  %1 %2").arg(ok ? QStringLiteral("[ok]  ")
                                                  : QStringLiteral("[FAIL]"), name);
    };

    // CoT type mapping
    check(QStringLiteral("fixed-wing -> a-f-A-M-F-Q"),
          CoT::typeForVehicle(1) == QLatin1String("a-f-A-M-F-Q"));
    check(QStringLiteral("quad -> a-f-A-M-H-Q"),
          CoT::typeForVehicle(2) == QLatin1String("a-f-A-M-H-Q"));

    // Build then parse a drone track CoT round-trip, and check TAK correctness:
    // a drone links to its reporting GCS EUD and does NOT claim takv.
    CoT::Entity drone;
    drone.uid = drone.callsign = QStringLiteral("ROTOR-1");
    drone.cotType = CoT::typeForVehicle(2);
    drone.lat = -35.36; drone.lon = 149.16; drone.hae = 100.0;
    drone.course = 90.0; drone.speed = 12.0;
    drone.parentUid = QStringLiteral("rotorlab-gcs");
    drone.parentCallsign = QStringLiteral("ROTOR-GCS");
    const QByteArray droneCot = CoT::build(drone);
    const CoT::Marker rt = CoT::parse(droneCot);
    check(QStringLiteral("drone track is well-formed and re-parses"),
          rt.valid && qAbs(rt.lat + 35.36) < 1e-6 && rt.callsign == QLatin1String("ROTOR-1"));
    check(QStringLiteral("drone track links to the reporting GCS EUD (association)"),
          droneCot.contains("<link") && droneCot.contains("rotorlab-gcs")
          && droneCot.contains("relation=\"p-p\""));
    check(QStringLiteral("drone track does NOT claim takv (not a TAK device)"),
          !droneCot.contains("<takv"));

    // The GCS self-beacon IS a TAK EUD (carries takv), broadcast by default.
    CoT::Entity gcs;
    gcs.uid = QStringLiteral("rotorlab-gcs"); gcs.callsign = QStringLiteral("ROTOR-GCS");
    gcs.cotType = QStringLiteral("a-f-G-U-C"); gcs.hasTrack = false; gcs.selfEud = true;
    const QByteArray gcsCot = CoT::build(gcs);
    check(QStringLiteral("GCS beacon is a self EUD (carries takv)"), gcsCot.contains("<takv"));
    check(QStringLiteral("broadcast beacon has no <marti> dest"), !gcsCot.contains("<marti"));

    // Direct delivery adds <marti><dest>.
    CoT::Entity direct = gcs;
    direct.dest = QStringList{ QStringLiteral("ALPHA"), QStringLiteral("BRAVO") };
    const QByteArray directCot = CoT::build(direct);
    check(QStringLiteral("direct delivery addresses named EUDs via <marti><dest>"),
          directCot.contains("<marti") && directCot.contains("ALPHA") && directCot.contains("BRAVO"));

    // Grammar
    auto parses = [](const QString &s, Tak::Verb v, int firstId, int altFt, int spd) {
        Tak::Command c; QString e;
        if (!Tak::parseCommand(s, c, e)) return false;
        if (c.verb != v) return false;
        if (firstId >= 0 && (c.ids.isEmpty() || c.ids.first() != firstId)) return false;
        return c.altFt == altFt && c.spd == spd;
    };
    check(QStringLiteral("GOTO_2_ALT_150_SPD_5 parses"),
          parses(QStringLiteral("GOTO_2_ALT_150_SPD_5"), Tak::Verb::Goto, 2, 150, 5));
    check(QStringLiteral("VECTOR-2 alias -> GOTO"),
          parses(QStringLiteral("VECTOR-2"), Tak::Verb::Goto, 2, -1, -1));
    {
        Tak::Command c; QString e;
        check(QStringLiteral("RTL_ALL -> RTL / target ALL"),
              Tak::parseCommand(QStringLiteral("RTL_ALL"), c, e)
              && c.verb == Tak::Verb::Rtl && c.target == Tak::Target::All);
    }
    {
        Tak::Command c; QString e;
        check(QStringLiteral("our own beacon callsign is NOT a command"),
              !Tak::parseCommand(QStringLiteral("ROTOR-2"), c, e));
    }
    {
        Tak::Command c; QString e;
        check(QStringLiteral("garbage marker is rejected"),
              !Tak::parseCommand(QStringLiteral("HELLO WORLD"), c, e));
    }

    // Approved-pilots allowlist: parse recovers the placing EUD from the p-p
    // link, and the gate honours callsign / uid entries.
    {
        const QByteArray cmdMarker =
            "<event version=\"2.0\" uid=\"a1b2c3\" type=\"a-u-G\" how=\"h-g-i-g-o\""
            " time=\"2026-01-01T00:00:00.0Z\" start=\"2026-01-01T00:00:00.0Z\" stale=\"2026-01-01T00:01:00.0Z\">"
            "<point lat=\"-35.36\" lon=\"149.16\" hae=\"0\" ce=\"9\" le=\"9\"/>"
            "<detail><contact callsign=\"GOTO 1\"/>"
            "<link uid=\"ANDROID-123456\" parent_callsign=\"WOLF\" relation=\"p-p\"/>"
            "</detail></event>";
        const CoT::Marker pm = CoT::parse(cmdMarker);
        check(QStringLiteral("command marker recovers placing EUD (uid + callsign)"),
              pm.valid && pm.senderCallsign == QLatin1String("WOLF")
              && pm.senderUid == QLatin1String("ANDROID-123456"));

        const bool prevOnly = _approvedOnly;
        const QStringList prevList = _approvedPilots;
        QString who;
        _approvedOnly = false; _approvedPilots.clear();
        check(QStringLiteral("allowlist off: any sender may command"), _senderApproved(pm, who));
        _approvedOnly = true;
        check(QStringLiteral("allowlist on + empty: every sender refused"), !_senderApproved(pm, who));
        _approvedPilots = QStringList{ QStringLiteral("wolf") };
        check(QStringLiteral("callsign entry matches case-insensitively"), _senderApproved(pm, who));
        _approvedPilots = QStringList{ QStringLiteral("android-123456") };
        check(QStringLiteral("device-uid entry matches"), _senderApproved(pm, who));
        _approvedPilots = QStringList{ QStringLiteral("uid:ANDROID-123456") };
        check(QStringLiteral("uid: entry matches the reported device UID"), _senderApproved(pm, who));
        _approvedPilots = QStringList{ QStringLiteral("uid:WOLF") };
        check(QStringLiteral("uid: entry does not match a callsign"), !_senderApproved(pm, who));
        _approvedPilots = QStringList{ QStringLiteral("callsign:ANDROID-123456") };
        check(QStringLiteral("callsign: entry does not match a device UID"), !_senderApproved(pm, who));
        _approvedPilots = QStringList{ QStringLiteral("OTHER") };
        check(QStringLiteral("unlisted sender is refused"), !_senderApproved(pm, who));
        const CoT::Marker anon = CoT::parse(QByteArray(cmdMarker).replace("<link", "<unlink"));
        check(QStringLiteral("marker without a parent link is refused while enforced"),
              !_senderApproved(anon, who));
        _approvedOnly = prevOnly; _approvedPilots = prevList;
    }

    // New-install verb defaults (TakDefaults): every verb is opt in. This
    // checks the shipped table, not this station's saved choices.
    check(QStringLiteral("default off: GOTO/RTL/LAND/HOLD/ALTHOLD"),
          !TakDefaults::verbGoto && !TakDefaults::verbRtl && !TakDefaults::verbLand
          && !TakDefaults::verbHold && !TakDefaults::verbAltHold);
    check(QStringLiteral("default on: send commands only from listed senders"), TakDefaults::approvedOnly);
    check(QStringLiteral("default on: verify server certificate"), TakDefaults::verifyPeer);
    check(QStringLiteral("enrollment never sends credentials unverified"),
          !TakPolicy::enrollMaySend(false, true, QStringLiteral("AA"), QString())
          && !TakPolicy::enrollMaySend(true, false, QStringLiteral("AA"), QString())
          && TakPolicy::enrollMaySend(true, true, QStringLiteral("AA"), QString())
          && TakPolicy::enrollMaySend(false, false, QStringLiteral("AA"), QStringLiteral("aa")));
    check(QStringLiteral("default off: ARM/TKOFF/DISARM/GUIDED"),
          !TakDefaults::verbArm && !TakDefaults::verbTakeoff
          && !TakDefaults::verbDisarm && !TakDefaults::verbGuided);

    // The station's test command carries a marked test sender, so the
    // allowlist refuses it unless that identity is listed.
    {
        const bool prevOnly = _approvedOnly;
        const QStringList prevList = _approvedPilots;
        QString who;
        _approvedOnly = true; _approvedPilots = QStringList{ QStringLiteral("WOLF") };
        check(QStringLiteral("allowlist on: station test sender refused unless listed"),
              !_senderApproved(TakPolicy::testSenderCallsign(), TakPolicy::testSenderUid(), who));
        _approvedPilots << TakPolicy::testSenderUid();
        check(QStringLiteral("allowlist on: station test sender accepted when its UID is listed"),
              _senderApproved(TakPolicy::testSenderCallsign(), TakPolicy::testSenderUid(), who));
        _approvedOnly = prevOnly; _approvedPilots = prevList;
    }

    // Commanded altitude warning informs only (warning text, never a refusal).
    check(QStringLiteral("altitude above warning level adds a warning line"),
          !TakPolicy::altitudeWarning(520, 400).isEmpty()
          && TakPolicy::altitudeWarning(400, 400).isEmpty()
          && TakPolicy::altitudeWarning(520, 0).isEmpty());

    lines.prepend(QStringLiteral("RotorLab GC TAK self-test  (%1/%2 passed)").arg(pass).arg(total));
    return lines.join(QLatin1Char('\n'));
}
