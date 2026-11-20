/****************************************************************************
 *
 * RotorLab GC - license activation manager implementation.
 *
 ****************************************************************************/

#include "LicenseManager.h"
#include "LicenseRecord.h"
#include "net/HttpClient.h"
#include "rotorlab/RotorLabStore.h"
#include "tak/TakManager.h"
#include "MultiVehicleManager.h"
#include "QmlObjectListModel.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
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

// Local keys that don't advertise their purpose in a settings dump. None of
// them can open GC on its own: only a record with RotorLab's signature does.
static const char *kRecDev     = "svc/di";   ///< device id
static const char *kRecBlob    = "svc/rk";   ///< 5.2.0 and earlier: unsigned record (read for migration only)
static const char *kRecSigned  = "svc/sr";   ///< the signed offline record envelope (compact JSON)
static const char *kRecKey     = "svc/lk";   ///< the license key, obfuscated, for the online checks
static const char *kHighWater  = "svc/hw";   ///< highest UTC second seen (clock rollback check)
static const char *kSignedSeen = "svc/sv";   ///< a signed record was stored here once: unsigned ones never count again

LicenseManager::LicenseManager(QObject *parent)
    : QObject(parent)
{
    _nam = new QNetworkAccessManager(this);
    _deviceId = _ensureDeviceId();
    _pinned = LicenseRecord::pinnedKeys();
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
    _noteLinksAllowed();
}

void LicenseManager::_noteLinksAllowed()
{
    const bool allowed = linksAllowed();
    if (allowed == _linksAllowedNoted) {
        return;
    }
    _linksAllowedNoted = allowed;
    if (!allowed && _disconnectTak) {
        // Integration nit 9: an open TAK link does not stay open behind the
        // gate, so no TAK command reaches a vehicle from under it.
        _disconnectTak();
    }
    emit linksAllowedChanged(allowed);
}

void LicenseManager::_disconnectTakNow()
{
    if (QCoreApplication::instance()) {
        TakManager::instance()->disconnectServer();
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
    _noteLinksAllowed();
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

namespace {

/// The obfuscation the 5.2.0 record used, kept for reading it and for the
/// stored key. It is NOT a security boundary and nothing relies on it: the
/// key is the customer's own, and the gate only trusts a signed record.
QByteArray xorWithDevice(const QByteArray &in, const QString &deviceId)
{
    const QByteArray ks = QCryptographicHash::hash(deviceId.toUtf8(), QCryptographicHash::Sha256);
    QByteArray out;
    out.resize(in.size());
    for (int i = 0; i < in.size(); ++i) {
        out[i] = in[i] ^ ks[i % ks.size()];
    }
    return out;
}

QString localTime(qint64 utcSecs)
{
    return QDateTime::fromSecsSinceEpoch(utcSecs).toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

} // namespace

QString LicenseManager::expiredText()
{
    return tr("Activation expired. Connect to the internet so RotorLab GC can check the license.");
}

QString LicenseManager::clockText()
{
    return tr("The date or time on this device is set earlier than RotorLab GC has already seen. "
              "Set the correct date and time, then connect to the internet so RotorLab GC can check the license.");
}

QString LicenseManager::legacyText()
{
    return tr("RotorLab GC must connect to the internet once to activate. "
              "Your activation from an earlier RotorLab GC version is checked online, "
              "then RotorLab GC opens without a connection as usual.");
}

QString LicenseManager::noLinksText()
{
    return tr("RotorLab GC does not connect to vehicles until this device is activated.");
}

QString LicenseManager::linkRefusedText()
{
    return tr("Activate RotorLab GC to connect to a vehicle.");
}

QString LicenseManager::deactivateVehicleText()
{
    return tr("Deactivate this device is off while a vehicle is connected.");
}

qint64 LicenseManager::_now() const
{
    return _nowOverride != 0 ? _nowOverride : QDateTime::currentSecsSinceEpoch();
}

void LicenseManager::_noteTime(qint64 now, qint64 signedTime)
{
    QSettings s;
    const qint64 hw = s.value(QLatin1String(kHighWater)).toLongLong();
    const qint64 next = qMax(hw, qMax(now, signedTime));
    if (next != hw) {
        s.setValue(QLatin1String(kHighWater), next);
        s.sync();
    }
}

void LicenseManager::_storeKey(const QString &key)
{
    QSettings s;
    s.setValue(QLatin1String(kRecKey), xorWithDevice(key.toUtf8(), _deviceId).toBase64());
    s.sync();
}

QString LicenseManager::_loadKey() const
{
    QSettings s;
    const QByteArray blob = QByteArray::fromBase64(s.value(QLatin1String(kRecKey)).toByteArray());
    if (blob.isEmpty()) {
        return QString();
    }
    return QString::fromUtf8(xorWithDevice(blob, _deviceId));
}

bool LicenseManager::_readLegacy(QString *key) const
{
    QSettings s;
    const QByteArray blob = QByteArray::fromBase64(s.value(QLatin1String(kRecBlob)).toByteArray());
    if (blob.isEmpty()) {
        return false;
    }
    const QJsonObject o = QJsonDocument::fromJson(xorWithDevice(blob, _deviceId)).object();
    if (o.value(QStringLiteral("d")).toString() != _deviceId) {
        return false;
    }
    const QString k = o.value(QStringLiteral("k")).toString();
    if (k.isEmpty()) {
        return false;
    }
    *key = k;
    return true;
}

void LicenseManager::_clearRecord()
{
    // The high-water time, the migration time and the signed-seen flag stay:
    // they only ever make GC stricter.
    QSettings s;
    s.remove(QLatin1String(kRecSigned));
    s.remove(QLatin1String(kRecKey));
    s.remove(QLatin1String(kRecBlob));
    s.sync();
    _offlineUntil = 0;
    _timeLapsed = false;
}

QString LicenseManager::_offlineUntilText() const
{
    return tr("RotorLab GC can open without a connection until %1.").arg(localTime(_offlineUntil));
}

QString LicenseManager::_textFor(LicenseRecord::Check c) const
{
    using LicenseRecord::Check;
    switch (c) {
    case Check::Valid:
        return tr("Activated.") + QLatin1Char(' ') + _offlineUntilText();
    case Check::Expired:
        return expiredText();
    case Check::IssuedInFuture:
    case Check::ClockRolledBack:
        return clockText();
    case Check::WrongDevice:
        return tr("The stored activation is for a different device, so RotorLab GC does not use it. "
                  "Enter the license key to activate this device.");
    case Check::Missing:
        return tr("Not activated");
    case Check::Malformed:
    case Check::UnknownKey:
    case Check::BadSignature:
    case Check::WrongProduct:
    case Check::WrongKey:
        break;
    }
    return tr("RotorLab GC could not verify the stored activation, so it does not use it. "
              "Connect to the internet so RotorLab GC can check the license, or enter the license key again.");
}

// The start. Only a record with RotorLab's signature, for this device and
// product, inside its time window, opens GC. Anything else in the settings
// file is ignored for the gate.
void LicenseManager::_loadRecord()
{
    QSettings s;
    const qint64 now = _now();
    const qint64 highWater = s.value(QLatin1String(kHighWater)).toLongLong();
    _key = _loadKey();
    _offlineUntil = 0;
    _timeLapsed = false;
    _legacyGate = false;

    const QByteArray envelope = s.value(QLatin1String(kRecSigned)).toByteArray();
    if (envelope.isEmpty()) {
        _loadLegacy(now);
        return;
    }

    LicenseRecord::Record r;
    bool signatureOk = false;
    const LicenseRecord::Check c = LicenseRecord::check(envelope, _pinned, _deviceId, _key,
                                                        now, highWater, &r, &signatureOk);
    // A genuine record for this device is a trusted time: the clock cannot be
    // set back before it was issued without GC noticing.
    const bool genuine = signatureOk && r.deviceId == _deviceId && r.product == QLatin1String("gc");
    _noteTime(now, genuine ? r.issued : 0);

    if (c == LicenseRecord::Check::Valid) {
        _offlineUntil = r.offlineUntil;
        _setActivated(true);
        _setStatus(_textFor(c));
    } else if (genuine && LicenseRecord::isTimeProblem(c)) {
        _timeProblemAtStart(_textFor(c));
    } else {
        // Not RotorLab's, or not for this device: never used. The key, if
        // any, still lets the gate's re-check ask RotorLab.
        _setActivated(false);
        _setStatus(_textFor(c));
    }
}

// Migration from 5.2.0 and earlier. The old record has no signature and
// anyone can write one, so it never opens GC offline, with or without a
// vehicle connected. Its only
// job is to hand its key to the online check: the gate's re-check sends it,
// and the signed record RotorLab returns replaces it. A 5.2.0 customer who
// updates must be online once.
void LicenseManager::_loadLegacy(qint64 now)
{
    QString legacyKey;
    if (!_readLegacy(&legacyKey)) {
        _setActivated(false);
        _setStatus(tr("Not activated"));
        return;
    }
    if (_key.isEmpty()) {
        _key = legacyKey;
    }
    _noteTime(now, 0);
    // The gate is up only for want of a signed record. A verified signed
    // record from the online check opens it in this session, as a typed key
    // does, whether or not a vehicle is connected.
    _legacyGate = true;

    // _timeLapsed stays false, so neither the start check nor
    // _vehicleAppeared opens GC on this record.
    _setActivated(false);
    if (QSettings().value(QLatin1String(kSignedSeen)).toBool()) {
        _setStatus(tr("Connect to the internet so RotorLab GC can check the license."));
    } else {
        _setStatus(legacyText());
    }
}

void LicenseManager::_timeProblemAtStart(const QString &text)
{
    _timeLapsed = true;
    _lapsedText = text;
    if (_vehicleConnected()) {
        _openForVehicle();
        return;
    }
    _setActivated(false);
    _setStatus(text);
}

// GC never covers a connected vehicle with the gate. A stored activation that
// only ran out of time (this device held a genuine one) opens for this session
// when a vehicle is connected, and says the license needs a check. The
// session then behaves like any other: re-checks only note, and the next
// start with no vehicle connected applies the result.
void LicenseManager::_openForVehicle()
{
    _timeLapsed = false;
    _inSession = true;
    _attention = true;
    _setActivated(true);
    _setStatus(_lapsedText + QLatin1Char(' ')
               + tr("RotorLab GC stays open for this session because a vehicle is connected."));
}

void LicenseManager::_vehicleAppeared()
{
    if (!_activated && _timeLapsed) {
        _openForVehicle();
    }
}

LicenseRecord::Check LicenseManager::_acceptRecord(const QJsonObject &reply, const QString &key)
{
    const QJsonValue v = reply.value(QStringLiteral("offline_record"));
    if (!v.isObject()) {
        return LicenseRecord::Check::Missing;
    }
    const QByteArray envelope = QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact);
    const qint64 now = _now();
    LicenseRecord::Record r;
    // Online, RotorLab's issued time is the reference. A high-water mark left
    // by a clock that was wrong earlier is not held against a fresh record;
    // a clock that is wrong now still fails the issued check.
    const LicenseRecord::Check c = LicenseRecord::check(envelope, _pinned, _deviceId, key,
                                                        now, 0, &r);
    if (c != LicenseRecord::Check::Valid) {
        return c;
    }
    QSettings s;
    s.setValue(QLatin1String(kRecSigned), envelope);
    s.setValue(QLatin1String(kSignedSeen), true);
    s.setValue(QLatin1String(kHighWater), qMax(now, r.issued));
    s.remove(QLatin1String(kRecBlob));
    s.sync();   // survive a crash: an unflushed activation costs the user a seat
    _storeKey(key);
    _offlineUntil = r.offlineUntil;
    _timeLapsed = false;
    return c;
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
    // Deactivating raises the gate, and no link stays open behind it. Never
    // take the screen, or the links, from a connected vehicle.
    if (_vehicleConnected()) {
        _setStatus(deactivateVehicleText());
        return;
    }
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

    // A vehicle connecting while the gate is up for a time-only problem lifts
    // it for the session (_vehicleAppeared). Connected once the event loop
    // runs, so the vehicle manager exists by then.
    if (QCoreApplication::instance()) {
        QTimer::singleShot(0, this, [this]() {
            (void) connect(MultiVehicleManager::instance(), &MultiVehicleManager::vehicleAdded,
                           this, [this]() { _vehicleAppeared(); });
        });
    }

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
    // Keep the highest time seen current through a long session, so a clock
    // set back after it shows at the next start.
    _noteTime(_now(), 0);

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
    if (op != Op::Deactivate) {
        // G89: the signing keys this GC can check, so RotorLab signs the
        // offline record with one of them (key rotation never strands an
        // older GC). Public key ids only.
        QJsonArray kids;
        for (const LicenseRecord::PinnedKey &k : _pinned) {
            kids.append(k.kid);
        }
        payload.insert(QStringLiteral("record_kids"), kids);
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
    const bool good = state == QLatin1String("activated") || state == QLatin1String("valid");

    // A good answer counts only with a signed record for this device. Without
    // one GC changes nothing, since such a reply could come from anywhere.
    LicenseRecord::Check rec = LicenseRecord::Check::Missing;
    if (good && op != Op::Deactivate) {
        rec = _acceptRecord(o, key);
    }
    const bool recordOk = rec == LicenseRecord::Check::Valid;
    const QString notVerified = LicenseRecord::isTimeProblem(rec)
        ? clockText()
        : tr("RotorLab GC could not verify the license server's reply. Try again later. "
             "If this keeps happening, contact RotorLab support.");

    if (op == Op::Activate) {
        if (good && recordOk) {
            _key = key;
            _inSession = true;
            _attention = false;
            _setActivated(true);
            _setStatus(tr("License activated.") + QLatin1Char(' ') + _offlineUntilText());
        } else if (good) {
            _setStatus(notVerified);   // an open session stays open; a closed gate stays closed
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
        if (state == QLatin1String("deactivated") && _activated && _vehicleConnected()) {
            // A vehicle connected while the request was out. The key is
            // released and the record gone, so the next start shows the gate,
            // but the gate never covers a connected vehicle: GC stays open
            // for this session.
            _clearRecord();
            _key.clear();
            _inSession = true;
            _attention = true;
            _setStatus(tr("License deactivated on this device. "
                          "RotorLab GC stays open for this session because a vehicle is connected."));
        } else if (state == QLatin1String("deactivated")) {
            _clearRecord();
            _key.clear();
            _setActivated(false);
            _setStatus(QStringLiteral("License deactivated on this device"));
        } else {
            _setStatus(error.isEmpty() ? QStringLiteral("Deactivation failed") : error);
        }
    } else if (!launchCheck) { // Verify while running: note only, never close
        if (good && recordOk && !_activated && _legacyGate) {
            // The gate is up for a 5.2.0 record only, and RotorLab just sent a
            // signed record for this device: open now, not at the next start.
            _legacyGate = false;
            _attention = false;
            _setActivated(true);
            _setStatus(tr("License activated.") + QLatin1Char(' ') + _offlineUntilText());
        } else if (good && recordOk) {
            _attention = false;
            _setStatus(QStringLiteral("License valid (re-check at %1). ")
                           .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm")))
                       + _offlineUntilText());
        } else if (good) {
            _setStatus(notVerified + QLatin1Char(' ')
                       + tr("RotorLab GC keeps running for this session."));
        } else {
            _noteInSession(state, error);
        }
    } else { // Verify at launch
        if (good && recordOk) {
            _setActivated(true);
            _setStatus(QStringLiteral("License valid. ") + _offlineUntilText());
        } else if (good) {
            // The stored record decided the gate at start; a reply GC cannot
            // verify neither opens nor closes it.
            _setStatus(_activated ? notVerified + QLatin1Char(' ') + tr("Using the stored activation.")
                                  : notVerified);
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
