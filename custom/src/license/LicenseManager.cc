/****************************************************************************
 *
 * RotorLab GC - license activation manager implementation.
 *
 ****************************************************************************/

#include "LicenseManager.h"
#include "net/HttpClient.h"
#include "rotorlab/RotorLabStore.h"
#include "MultiVehicleManager.h"
#include "QmlObjectListModel.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSettings>
#include <QtCore/QSysInfo>
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkInformation>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QSslConfiguration>

Q_GLOBAL_STATIC(LicenseManager, _licenseManagerInstance)

LicenseManager *LicenseManager::instance()
{
    return _licenseManagerInstance();
}

// A local record key that doesn't advertise its purpose in a settings dump.
static const char *kRecGroup  = "svc";
static const char *kRecDev    = "svc/di";
static const char *kRecBlob   = "svc/rk";

LicenseManager::LicenseManager(QObject *parent)
    : QObject(parent)
{
    _nam = new QNetworkAccessManager(this);
    _deviceId = _ensureDeviceId();
    _loadRecord();
    _startBackgroundChecks();
}

LicenseManager::~LicenseManager()
{
}

// ---------------------------------------------------------------------------
// State helpers
// ---------------------------------------------------------------------------

void LicenseManager::_setActivated(bool on)
{
    if (_activated != on) {
        _activated = on;
        emit activatedChanged();
    }
}

void LicenseManager::_setBusy(bool on)
{
    if (_busy != on) {
        _busy = on;
        emit busyChanged();
    }
}

void LicenseManager::_setStatus(const QString &text)
{
    _statusText = text;
    emit statusChanged();
    emit logMessage(text);
}

QString LicenseManager::licenseKey() const
{
    // Mask all but the last 4 characters.
    if (_key.isEmpty()) {
        return QString();
    }
    const QString tail = _key.right(4);
    return QStringLiteral("••••-••••-%1").arg(tail);
}

QString LicenseManager::maskedDeviceId() const
{
    if (_deviceId.length() < 8) {
        return _deviceId;
    }
    return _deviceId.left(4) + QStringLiteral("…") + _deviceId.right(4);
}

// ---------------------------------------------------------------------------
// Device id + local record
// ---------------------------------------------------------------------------

QString LicenseManager::_ensureDeviceId()
{
    QSettings s;
    QString id = s.value(QLatin1String(kRecDev)).toString();
    if (id.isEmpty()) {
        // Stable, opaque, per-install. Not derived from PII; just a random id
        // bound to this install so the license can device-lock.
        id = QUuid::createUuid().toString(QUuid::Id128);
        s.setValue(QLatin1String(kRecDev), id);
        // Flush now. QSettings normally writes on destruction/exit, so a crash
        // or a force-kill would lose this and the next launch would look like a
        // brand new device - burning another activation on the customer's key.
        s.sync();
    }
    return id;
}

// The stored record is an obfuscated (not encrypted) blob binding the key to
// this device id. It lets the app open offline once activated; it is NOT a
// security boundary (a local flag can always be flipped by the machine owner).
void LicenseManager::_loadRecord()
{
    QSettings s;
    const QByteArray blob = QByteArray::fromBase64(s.value(QLatin1String(kRecBlob)).toByteArray());
    if (blob.isEmpty()) {
        _setStatus(QStringLiteral("Not activated"));
        return;
    }
    // XOR with a device-bound keystream (deviceId hash) then parse JSON.
    const QByteArray ks = QCryptographicHash::hash(_deviceId.toUtf8(), QCryptographicHash::Sha256);
    QByteArray plain;
    plain.resize(blob.size());
    for (int i = 0; i < blob.size(); ++i) {
        plain[i] = blob[i] ^ ks[i % ks.size()];
    }
    const QJsonObject o = QJsonDocument::fromJson(plain).object();
    if (o.value(QStringLiteral("d")).toString() == _deviceId) {
        _key = o.value(QStringLiteral("k")).toString();
        _setActivated(true);
        _setStatus(QStringLiteral("Activated (offline record; re-checking…)"));
    }
}

void LicenseManager::_saveRecord(const QString &key)
{
    QJsonObject o;
    o[QStringLiteral("k")] = key;
    o[QStringLiteral("d")] = _deviceId;
    const QByteArray plain = QJsonDocument(o).toJson(QJsonDocument::Compact);
    const QByteArray ks = QCryptographicHash::hash(_deviceId.toUtf8(), QCryptographicHash::Sha256);
    QByteArray blob;
    blob.resize(plain.size());
    for (int i = 0; i < plain.size(); ++i) {
        blob[i] = plain[i] ^ ks[i % ks.size()];
    }
    QSettings s;
    s.setValue(QLatin1String(kRecBlob), blob.toBase64());
    s.sync();   // survive a crash: an unflushed activation costs the user a seat
}

void LicenseManager::_clearRecord()
{
    QSettings s;
    s.remove(QLatin1String(kRecBlob));
    s.sync();
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void LicenseManager::activate(const QString &key)
{
    const QString k = key.trimmed();
    if (k.isEmpty()) {
        _setStatus(QStringLiteral("Enter a license key"));
        return;
    }
    _send(Op::Activate, k);
}

void LicenseManager::deactivate()
{
    if (_key.isEmpty()) {
        _setStatus(QStringLiteral("No active license to release"));
        return;
    }
    _send(Op::Deactivate, _key);
}

void LicenseManager::recheck()
{
    if (_key.isEmpty()) {
        return;
    }
    _send(Op::Verify, _key);
}

QString LicenseManager::offlineReleaseHint()
{
    return tr("Without a connection, an organization admin can release this key in RotorLab: "
              "Organization, Billing, Product licenses, then Release on this device's activation.");
}

// ---------------------------------------------------------------------------
// Re-check while running
// ---------------------------------------------------------------------------

void LicenseManager::_startBackgroundChecks()
{
    _recheckTimer = new QTimer(this);
    _recheckTimer->setInterval(kRecheckIntervalMs);
    connect(_recheckTimer, &QTimer::timeout, this, &LicenseManager::_backgroundRecheck);
    _recheckTimer->start();

    // Network regain. Where the platform has no reachability backend there is
    // only the timer and Re-check now.
    if (QNetworkInformation::loadBackendByFeatures(QNetworkInformation::Feature::Reachability)) {
        QNetworkInformation *info = QNetworkInformation::instance();
        _wasOnline = info->reachability() == QNetworkInformation::Reachability::Online;
        connect(info, &QNetworkInformation::reachabilityChanged, this,
                [this](QNetworkInformation::Reachability r) {
                    _networkReachable(r == QNetworkInformation::Reachability::Online);
                });
    }
}

bool LicenseManager::_networkReachable(bool online)
{
    const bool regained = online && !_wasOnline;
    _wasOnline = online;
    if (!regained) {
        return false;
    }
    // A link that flaps should not turn into a stream of license requests.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (_lastAttemptMs != 0 && now - _lastAttemptMs < kRegainMinSpacingMs) {
        return false;
    }
    _backgroundRecheck();
    return true;
}

bool LicenseManager::_anyVehicleConnected()
{
    if (!QCoreApplication::instance()) {
        return false;
    }
    const QmlObjectListModel *list = MultiVehicleManager::instance()->vehicles();
    return list && list->count() > 0;
}

void LicenseManager::_backgroundRecheck()
{
    // Before the launch check has run, the gate is still up and does it.
    if (!_inSession || _key.isEmpty() || _busy) {
        return;
    }
    _send(Op::Verify, _key);
}

// A re-check while running found a problem. Note it and carry on: the
// activation, the stored record and the open session all stay as they are,
// so nothing here can stop a flight. The next start checks again and applies
// the result there.
void LicenseManager::_noteInSession(const QString &state, const QString &error)
{
    const QString when = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm"));
    QString what;
    if (state == QLatin1String("expired")) {
        what = tr("License expired");
    } else if (state == QLatin1String("revoked")) {
        what = tr("License revoked");
    } else if (state == QLatin1String("wrong_device") || state == QLatin1String("invalid")) {
        what = tr("License is no longer valid for this device");
    } else {
        // Nothing found against the license: same as offline.
        _setStatus(error.isEmpty() ? tr("License check inconclusive: using stored activation") : error);
        return;
    }
    _attention = true;
    _setStatus(tr("%1 (re-check at %2). RotorLab GC keeps running for this session; "
                  "the next start checks the license again.").arg(what, when));
}

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------

void LicenseManager::_send(Op op, const QString &key)
{
    if (_busy) {
        return;
    }
    _setBusy(true);
    if (op == Op::Verify) {
        _lastAttemptMs = QDateTime::currentMSecsSinceEpoch();
    }

    const char *verb = op == Op::Activate ? "activate"
                     : op == Op::Deactivate ? "deactivate"
                     : "verify";

    QJsonObject payload{
        {QStringLiteral("key"), key},
        {QStringLiteral("device_id"), _deviceId}
    };
    if (op == Op::Activate) {
        payload.insert(QStringLiteral("label"), QStringLiteral("RotorLab GC"));
        payload.insert(QStringLiteral("platform"), QSysInfo::productType());
    }

#ifdef QGC_DAILY_BUILD
    const QString base = HttpClient::normalizeBaseUrl(RotorLabStore::baseUrl(), true);
#else
    const QString base = RotorLabStore::baseUrl();
#endif
    if (base.isEmpty()) {
        _setBusy(false);
        _setStatus(QStringLiteral("The license server address is not valid."));
        return;
    }
    QNetworkRequest req{QUrl(base + QStringLiteral("/api/license/%1").arg(QLatin1String(verb)))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("RotorLabGC"));
    req.setSslConfiguration(QSslConfiguration::defaultConfiguration());
    req.setTransferTimeout(15000);

    QNetworkReply *reply = _nam->post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));

    connect(reply, &QNetworkReply::finished, this, [this, reply, op, key]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool netOk = status > 0;
        const QByteArray body = reply->readAll();
        reply->deleteLater();
        _handleReply(op, key, body, netOk, status);
    });
}

void LicenseManager::_handleReply(Op op, const QString &key, const QByteArray &body,
                                  bool networkOk, int status)
{
    _setBusy(false);

    // The first verify reply of the run is the launch check. From then on the
    // session is open and a verify only notes what it finds. A launch-check
    // reply that arrives while a vehicle is connected (GC restarted mid-flight,
    // or a slow reply) only notes too: the gate never covers a live station.
    const bool launchCheck = op == Op::Verify && !_inSession && !_vehicleConnected();
    if (op == Op::Verify) {
        _inSession = true;
    }

    if (!networkOk) {
        // Offline. Do not revoke an already-good activation; just report.
        if (op == Op::Verify && _activated) {
            _setStatus(QStringLiteral("Offline: using stored activation"));
        } else if (op == Op::Activate) {
            _setStatus(QStringLiteral("Could not reach the license server. Check your connection."));
        } else if (op == Op::Deactivate) {
            _setStatus(QStringLiteral("Could not reach the license server to deactivate. ")
                       + offlineReleaseHint());
        }
        return;
    }

    if (status == 429) {
        _setStatus(QStringLiteral("Too many license requests. Try again later."));
        return;
    }

    const QJsonObject o = QJsonDocument::fromJson(body).object();
    const QString state = o.value(QStringLiteral("state")).toString();
    const QString error = o.value(QStringLiteral("error")).toString();

    if (op == Op::Activate) {
        if (state == QLatin1String("activated") || state == QLatin1String("valid")) {
            _key = key;
            _saveRecord(key);
            _inSession = true;
            _attention = false;
            _setActivated(true);
            _setStatus(QStringLiteral("License activated"));
        } else if (state == QLatin1String("max_reached")) {
            _setStatus(QStringLiteral("This key has reached its activation limit"));
        } else if (state == QLatin1String("expired")) {
            _setStatus(QStringLiteral("This license key has expired"));
        } else if (state == QLatin1String("revoked")) {
            _setStatus(QStringLiteral("This license key has been revoked"));
        } else {
            _setStatus(error.isEmpty() ? QStringLiteral("Activation failed") : error);
        }
    } else if (op == Op::Deactivate) {
        if (state == QLatin1String("deactivated")) {
            _clearRecord();
            _key.clear();
            _setActivated(false);
            _setStatus(QStringLiteral("License deactivated on this device"));
        } else {
            _setStatus(error.isEmpty() ? QStringLiteral("Deactivation failed") : error);
        }
    } else if (!launchCheck) { // Verify while running: note only, never close
        if (state == QLatin1String("valid")) {
            _attention = false;
            _setStatus(QStringLiteral("License valid (re-check at %1)")
                           .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm"))));
        } else {
            _noteInSession(state, error);
        }
    } else { // Verify at launch
        if (state == QLatin1String("valid")) {
            _setActivated(true);
            _setStatus(QStringLiteral("License valid"));
        } else if (state == QLatin1String("expired") || state == QLatin1String("revoked")) {
            _clearRecord();
            _key.clear();
            _setActivated(false);
            _setStatus(state == QLatin1String("expired")
                           ? QStringLiteral("License expired")
                           : QStringLiteral("License revoked"));
        } else if (state == QLatin1String("wrong_device") || state == QLatin1String("invalid")) {
            _clearRecord();
            _key.clear();
            _setActivated(false);
            _setStatus(QStringLiteral("License is no longer valid for this device"));
        } else {
            if (_activated) {
                _setStatus(error.isEmpty()
                               ? QStringLiteral("License check inconclusive: using stored activation")
                               : error);
            } else if (!error.isEmpty()) {
                _setStatus(error);
            }
        }
    }
}
