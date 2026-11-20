/****************************************************************************
 *
 * RotorLab GC TAK integration - manager implementation.
 *
 ****************************************************************************/

#include "TakManager.h"
#include "fleet/GuidedDispatch.h"
#include "rotorlab/Credential.h"
#include "CoTMessage.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include <algorithm>

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
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
#include <QtCore/QTimeZone>
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

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#endif

Q_GLOBAL_STATIC(TakManager, _takManagerInstance)

// The route as FleetConnector last reported it, so a TakManager created later
// starts in the right state (S4).
static bool s_fleetIsRoute = false;

TakManager *TakManager::instance()
{
    return _takManagerInstance();
}

TakManager::TakManager(QObject *parent)
    : QObject(parent)
    , _fleetIsRoute(s_fleetIsRoute)
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
    // Read before anything below saves: _persist() writes the prefix key, so
    // a check made after it would never see a new install (G47).
    const bool prefixSaved = QSettings().contains(QStringLiteral("TAK/callsignPrefix"));
    // The GCS UID is this station's stable EUD identity on the TAK server. Guard
    // against an empty or truncated/hand-edited value (a valid one is
    // "rotorlab-" + 12 hex chars = 21 chars); regenerate so a bad value can't
    // break EUD registration and, with it, drone-marker attribution.
    if (_gcsUid.isEmpty() || _gcsUid.length() < 16) {
        _gcsUid = QStringLiteral("rotorlab-") + QUuid::createUuid().toString(QUuid::Id128).left(12);
        _persist();   // keep this identity stable across launches
    }
    // G47 (security M1): no shared default prefix. A new install gets one
    // from its random station uid; it still reads no commands until the
    // operator sets or confirms a prefix here (prefixSet). An upgraded
    // install saved with ROTOR, the old shared default, is offered the same
    // per-install default, never ROTOR.
    if (!prefixSaved || TakPolicy::isSharedDefaultPrefix(_callsignPrefix)) {
        _callsignPrefix = TakPolicy::defaultPrefixFor(_gcsUid);
        _prefixSetFor.clear();
        _persist();
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
    // S4: while the Fleet Server is the route to ATAK the direct link's
    // commands stay off, whoever asks. The switch is told it stayed off.
    if (armed && _fleetIsRoute) {
        _log(QStringLiteral("\"Commands to aircraft on\" not turned on: the Fleet Server is this station's "
                            "route to ATAK"));
        emit c2ArmedChanged();
        return;
    }
    // G47 (security M1): not before this station has its own prefix.
    if (armed && !_prefixUnusableReason().isEmpty()) {
        _log(TakPolicy::prefixNotSetArmLine());
        emit c2ArmedChanged();
        return;
    }
    if (_c2Armed != armed) {
        _c2Armed = armed;
        emit c2ArmedChanged();
        _log(armed ? QStringLiteral("\"Commands to aircraft on\" turned on")
                   : QStringLiteral("\"Commands to aircraft on\" turned off"));
    }
}

// Verify off stays the operator's choice (default on). Every change is
// logged, and turning it off is also written to the application log (G40).
void TakManager::setVerifyPeer(bool on)
{
    if (_verifyPeer == on) {
        return;
    }
    _verifyPeer = on;
    if (on) {
        _log(QStringLiteral("\"Verify server certificate\" turned on"));
    } else {
        const QString line = QStringLiteral("\"Verify server certificate\" turned off. From the next connect "
                                            "the TAK link accepts any server certificate and does not check "
                                            "which server it is talking to.");
        _log(line);
        qWarning().noquote() << "TAK:" << line;
    }
    emit configChanged();
}

// G47: the prefix names this station in every marker. A value that cannot
// name it is not saved, and neither is one another station used on the link
// in the last 2 minutes (security M4). Saving the shown value again confirms
// it for this install (security M1). A change turns "Commands to aircraft
// on" off, and a marker placed before it is not run. A prefix conflict
// already found is kept, not cleared, so a change opens no window in which
// commands are read.
void TakManager::setCallsignPrefix(const QString &prefix)
{
    const QString t = prefix.trimmed();
    if (t == _callsignPrefix && prefixSet()) {
        if (!_prefixEntryError.isEmpty()) {
            _prefixEntryError.clear();
            emit prefixNoticeChanged();
        }
        return;
    }
    auto refuse = [this, &t](const QString &text, const QString &why) {
        _prefixEntryError = text;
        _log(QStringLiteral("Callsign prefix \"%1\" not saved: %2").arg(t.left(kMaxMarkerField), why));
        emit prefixNoticeChanged();
        emit configChanged();   // the page shows the saved value again
    };
    if (!TakPolicy::prefixProblem(t).isEmpty()) {
        refuse(TakPolicy::isSharedDefaultPrefix(t) ? TakPolicy::sharedDefaultPrefixRefusal()
                                                   : TakPolicy::prefixEntryRefusal(),
               TakPolicy::prefixProblem(t));
        return;
    }
    if (_prefixLastSeenElsewhere(t) > 0) {
        refuse(TakPolicy::prefixSeenRefusal(t), QStringLiteral("another station on this TAK server used it in the "
                                                               "last 2 minutes"));
        return;
    }
    if (t == _callsignPrefix) {   // confirmed for this install
        _prefixSetFor = t;
        _prefixEntryError.clear();
        _log(QStringLiteral("Callsign prefix %1 set for this station").arg(t));
        emit prefixNoticeChanged();
        emit configChanged();
        return;
    }
    const QString from = _callsignPrefix;
    _callsignPrefix = t;
    _prefixSetFor = t;
    _prefixEntryError.clear();
    _prefixFloorUtc = QDateTime::currentDateTimeUtc();
    setC2Armed(false);
    _prefixChangedNotice = TakPolicy::prefixChangedLine(from.isEmpty() ? QStringLiteral("(empty)") : from, t);
    _log(_prefixChangedNotice);
    emit prefixNoticeChanged();
    emit configChanged();
    _updateConflictNotice();
}

QString TakManager::prefixNotSetNotice() const
{
    if (_callsignPrefix.isEmpty() || !TakPolicy::prefixProblem(_callsignPrefix).isEmpty() || prefixSet()) {
        return QString();
    }
    return TakPolicy::prefixNotSetNotice(_callsignPrefix);
}

QString TakManager::_prefixUnusableReason() const
{
    if (_callsignPrefix.isEmpty()) {
        return TakPolicy::emptyPrefixReason();
    }
    if (!TakPolicy::prefixProblem(_callsignPrefix).isEmpty()) {
        return TakPolicy::prefixInvalidReason();
    }
    if (!prefixSet()) {
        return TakPolicy::prefixNotSetReason();
    }
    return QString();
}

QString TakManager::prefixProblemNotice() const
{
    if (_callsignPrefix.isEmpty() || TakPolicy::prefixProblem(_callsignPrefix).isEmpty()) {
        return QString();
    }
    return TakPolicy::prefixInvalidNotice(_callsignPrefix);
}

// ---------------------------------------------------------------------------
// Fleet Server hand-off (G40)
// ---------------------------------------------------------------------------

void TakManager::fleetRouteChanged(bool fleetIsRoute)
{
    s_fleetIsRoute = fleetIsRoute;
    // Never create the manager here: if it does not exist yet there is no
    // TAK link and no armed switch to turn off; it reads s_fleetIsRoute when
    // it is created.
    if (!_takManagerInstance.exists() || _takManagerInstance.isDestroyed()) {
        return;
    }
    if (fleetIsRoute) {
        instance()->handOffToFleet();
    } else {
        instance()->clearFleetHandOff();
    }
}

void TakManager::handOffToFleet()
{
    _fleetIsRoute = true;
    // Up, or still connecting. A connect that already failed is not a link.
    const bool linkOpen = _connected
        || (_socket && _socket->state() != QAbstractSocket::UnconnectedState);
    const bool wasArmed = _c2Armed;
    if (!linkOpen && !wasArmed) {
        return;
    }
    setC2Armed(false);
    if (linkOpen) {
        disconnectServer();
        _setStatus(QStringLiteral("disconnected: the Fleet Server is this station's route to ATAK"));
    }
    const QString what = linkOpen
        ? tr("The TAK link was closed and \"Commands to aircraft on\" was turned off.")
        : tr("\"Commands to aircraft on\" was turned off.");
    _fleetHandOffNotice = what + QLatin1Char(' ')
        + tr("The Fleet Server is now this station's route to ATAK, so the TAK / ATAK page is hidden. "
             "Your TAK settings are kept.");
    _log(QStringLiteral("Fleet Server joined: %1").arg(what));
    qWarning().noquote() << "TAK: Fleet Server joined:" << what;
    emit fleetHandOffNoticeChanged();
}

void TakManager::clearFleetHandOff()
{
    _fleetIsRoute = false;
    if (!_fleetHandOffNotice.isEmpty()) {
        _fleetHandOffNotice.clear();
        emit fleetHandOffNoticeChanged();
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
    // The password is never written to the settings file (G41): it goes to
    // the platform key store, or stays in memory where there is none.
    _savePassword();
    if (!_plainPasswordPending) {   // S6: kept until the key store has it
        s.remove(QStringLiteral("password"));
    }
    s.setValue(QStringLiteral("callsignPrefix"), _callsignPrefix);
    s.setValue(QStringLiteral("prefixSetFor"), _prefixSetFor);
    s.setValue(QStringLiteral("gcsUid"), _gcsUid);
    s.setValue(QStringLiteral("verifyPeer"), _verifyPeer);
    s.setValue(QStringLiteral("pinnedServerSha256"), _pinnedFingerprint);
    s.setValue(QStringLiteral("publishEnabled"), _publishEnabled);
    s.setValue(QStringLiteral("publishGcsBeacon"), _publishGcsBeacon);
    s.setValue(QStringLiteral("shareStationPosition"), _shareStationPosition);
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
    _loadPassword(s);
    _callsignPrefix   = s.value(QStringLiteral("callsignPrefix"), _callsignPrefix).toString();
    // Older versions never wrote this key, so an upgraded install has no
    // prefix set (security M1). One saved with the old shared default ROTOR
    // is moved to its own default in the constructor.
    _prefixSetFor     = s.value(QStringLiteral("prefixSetFor")).toString();
    _gcsUid           = s.value(QStringLiteral("gcsUid"), _gcsUid).toString();
    _verifyPeer       = s.value(QStringLiteral("verifyPeer"), TakDefaults::verifyPeer).toBool();
    _pinnedFingerprint = s.value(QStringLiteral("pinnedServerSha256")).toString();
    _publishEnabled   = s.value(QStringLiteral("publishEnabled"), _publishEnabled).toBool();
    _publishGcsBeacon = s.value(QStringLiteral("publishGcsBeacon"), _publishGcsBeacon).toBool();
    // Not saved by older versions: an upgraded install starts with sharing off.
    _shareStationPosition = s.value(QStringLiteral("shareStationPosition"),
                                    TakDefaults::shareStationPosition).toBool();
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

// ---------------------------------------------------------------------------
// TAK password in the platform key store (G41)
//
// The same store as the RotorLab Device Key (rotorlab/Credential.h): Windows
// Credential Manager, the Android Keystore, or the Secret Service on Linux.
// Where Credential would fall back to base64 in the settings file (macOS, or
// Linux with no Secret Service) the password is NOT saved at all: it is kept
// in memory for the session and asked for again. A plain value an older
// version left in the settings file is moved into the store once and always
// removed from the file.
// ---------------------------------------------------------------------------

QString TakManager::passwordAccount()
{
#ifdef QGC_UNITTEST_BUILD
    if (QCoreApplication::applicationName().endsWith(QLatin1String("_unittest"))) {
        return QStringLiteral("Unit test TAK password %1").arg(QCoreApplication::applicationPid());
    }
#endif
    return QStringLiteral("TAK password");
}

void TakManager::_loadPassword(QSettings &s)
{
    const QString plain = s.value(QStringLiteral("password")).toString();
    if (!plain.isEmpty()) {
        _password = plain;
        if (!Credential::inOsStore()) {
            // No system key store: never kept in plain text again.
            s.remove(QStringLiteral("password"));
            s.sync();
            const QString line = QStringLiteral("TAK password removed from the settings file and kept for this "
                                                "session only: it could not be saved to the system key store");
            _log(line);
            qWarning().noquote() << "TAK:" << line;
            _setPasswordStore(PasswordStore::SessionOnly);
            return;
        }
        if (Credential::store(passwordAccount(), plain)) {
            _passwordSaved = plain;
            _plainPasswordPending = false;
            s.remove(QStringLiteral("password"));
            s.sync();
            _log(QStringLiteral("TAK password moved from the settings file to %1").arg(Credential::storeName()));
            _setPasswordStore(PasswordStore::Stored, true);
        } else {
            // S6: the store refused it (for example while an aircraft is
            // armed). The plain value stays in the settings file until a
            // later save puts it in the store, so it is not lost.
            _plainPasswordPending = true;
            _plainPasswordLoaded  = plain;
            _storeRefusedArmed    = Credential::heldWhileArmed();
            const QString line = QStringLiteral("TAK password not moved yet: %1 did not accept it; it stays in the "
                                                "settings file until the next save to %1 succeeds")
                                     .arg(Credential::storeName());
            _log(line);
            qWarning().noquote() << "TAK:" << line;
            _setPasswordStore(PasswordStore::Pending);
        }
        return;
    }
    if (!Credential::inOsStore()) {
        _setPasswordStore(_password.isEmpty() ? PasswordStore::None : PasswordStore::SessionOnly);
        return;
    }
    if (_passwordStore == PasswordStore::NotSaved || _passwordStore == PasswordStore::Pending) {
        return;   // the session value is newer than anything stored
    }
    const bool moved = _passwordMoved;
    _password      = Credential::load(passwordAccount());
    _passwordSaved = _password;
    _setPasswordStore(_password.isEmpty() ? PasswordStore::None : PasswordStore::Stored,
                      moved && !_password.isEmpty());
}

void TakManager::_savePassword()
{
    // R2: the plain copy an older version left is kept only while it is
    // still the password. Changed or cleared, it is dropped at this save.
    if (_plainPasswordPending && _password != _plainPasswordLoaded) {
        _plainPasswordPending = false;
        _plainPasswordLoaded.clear();
        if (_password.isEmpty() && _passwordSaved.isEmpty()) {
            _setPasswordStore(PasswordStore::None);
        }
    }
    if (_password == _passwordSaved) {
        return;
    }
    if (!Credential::inOsStore()) {
        _passwordSaved = _password;   // nothing is written anywhere
        _setPasswordStore(_password.isEmpty() ? PasswordStore::None : PasswordStore::SessionOnly);
        return;
    }
    if (_password.isEmpty()) {
        Credential::erase(passwordAccount());
        _passwordSaved.clear();
        _setPasswordStore(PasswordStore::None);
        return;
    }
    if (Credential::store(passwordAccount(), _password)) {
        _passwordSaved = _password;
        const bool moved = _plainPasswordPending;
        _plainPasswordPending = false;   // the next settings write removes the plain copy
        _plainPasswordLoaded.clear();
        if (moved) {
            _log(QStringLiteral("TAK password moved from the settings file to %1").arg(Credential::storeName()));
        }
        _setPasswordStore(PasswordStore::Stored, moved);
    } else {
        // The note names the armed rule only when that is why the store
        // refused; the cause can change between saves.
        _storeRefusedArmed = Credential::heldWhileArmed();
        if (_plainPasswordPending) {
            _setPasswordStore(PasswordStore::Pending);   // still the loaded password
        } else {
            if (_passwordStore != PasswordStore::NotSaved) {
                const QString line = QStringLiteral("TAK password could not be saved to %1; kept for this session "
                                                    "only").arg(Credential::storeName());
                _log(line);
                qWarning().noquote() << "TAK:" << line;
            }
            _setPasswordStore(PasswordStore::NotSaved);
        }
    }
}

void TakManager::_setPasswordStore(PasswordStore st, bool movedFromSettings)
{
    _passwordStore = st;
    _passwordMoved = movedFromSettings;
    QString text;
    switch (st) {
    case PasswordStore::None:
        break;
    case PasswordStore::Stored:
        text = movedFromSettings
            ? tr("Password saved in %1. The readable copy that an earlier version kept in the settings file has "
                 "been deleted.")
                  .arg(Credential::storeName())
            : tr("Password saved in %1.").arg(Credential::storeName());
        break;
    case PasswordStore::SessionOnly:
        text = tr("Password kept for this session only (%1), so it is not saved. Enter it again after a "
                  "restart before you enroll.")
                   .arg(Credential::storeName());
        break;
    case PasswordStore::Pending:
        text = _storeRefusedArmed
            ? tr("Password not moved to %1 yet. GC does not write to %1 while an aircraft is armed. Until it is "
                 "moved, the password stays in the settings file, where the earlier version saved it. It is "
                 "moved the next time settings are saved after all aircraft are disarmed.")
                  .arg(Credential::storeName())
            : tr("Password not moved to %1 yet: %1 did not accept it. Until it is moved, the password stays in "
                 "the settings file, where the earlier version saved it. GC tries again the next time settings "
                 "are saved.").arg(Credential::storeName());
        break;
    case PasswordStore::NotSaved:
        if (!_storeRefusedArmed) {
            text = tr("Password could not be saved to %1: %1 did not accept it, so it is kept for this session "
                      "only. GC tries again the next time settings are saved.").arg(Credential::storeName());
            break;
        }
        text = tr("Password could not be saved to %1, so it is kept for this session only. GC does not write "
                  "to %1 while an aircraft is armed. If one is armed now, the password is saved the next time "
                  "settings are saved after all aircraft are disarmed.").arg(Credential::storeName());
        break;
    }
    if (_passwordStorageText != text) {
        _passwordStorageText = text;
        emit passwordStorageChanged();
    }
}

// ---------------------------------------------------------------------------
// Client key file: readable and writable by the current user only (G41)
// ---------------------------------------------------------------------------

bool TakManager::_restrictToOwner(const QString &path)
{
#ifdef QGC_UNITTEST_BUILD
    if (s_failRestrictForTest) {
        return false;
    }
#endif
#ifdef Q_OS_WIN
    // A protected DACL with one entry: full control for the current user.
    // Nothing is inherited from the profile folder.
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    DWORD len = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &len);
    QByteArray buf(static_cast<qsizetype>(len), '\0');
    const bool haveUser = len > 0 && ::GetTokenInformation(token, TokenUser, buf.data(), len, &len);
    ::CloseHandle(token);
    if (!haveUser) {
        return false;
    }
    PSID user = reinterpret_cast<TOKEN_USER *>(buf.data())->User.Sid;

    EXPLICIT_ACCESSW ea{};
    ea.grfAccessPermissions = FILE_ALL_ACCESS;
    ea.grfAccessMode        = SET_ACCESS;
    ea.grfInheritance       = NO_INHERITANCE;
    ea.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType  = TRUSTEE_IS_USER;
    ea.Trustee.ptstrName    = reinterpret_cast<LPWSTR>(user);
    PACL acl = nullptr;
    if (::SetEntriesInAclW(1, &ea, nullptr, &acl) != ERROR_SUCCESS) {
        return false;
    }
    std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    const DWORD rc = ::SetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT,
                                             DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                             nullptr, nullptr, acl, nullptr);
    ::LocalFree(acl);
    return rc == ERROR_SUCCESS;
#else
    return QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
#endif
}

bool TakManager::_writePemFile(const QString &path, const QByteArray &data, bool privateKey, QString *error)
{
    auto fail = [error](const QString &why) {
        if (error) {
            *error = why;
        }
        return false;
    };
    if (privateKey) {
        // S5: a new file next to the target, restricted to the current user
        // while still empty. Only when that worked does the key go in, and
        // the restricted file then replaces the old one. Fails closed: if the
        // file cannot be restricted, no key is written and the old file is
        // left as it was.
        const QString tmp = path + QStringLiteral(".new");
        QFile::remove(tmp);
        QFile f(tmp);
        if (!f.open(QIODevice::WriteOnly | QIODevice::NewOnly,
                    QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
            return fail(QStringLiteral("could not create the file (%1)").arg(f.errorString()));
        }
        f.close();
        if (!_restrictToOwner(tmp)) {
            QFile::remove(tmp);
            const QString line = QStringLiteral("client key not saved: could not restrict the file to the current "
                                                "user (%1)").arg(QDir::toNativeSeparators(path));
            qWarning().noquote() << "TAK:" << line;
            return fail(QStringLiteral("could not restrict the file to the current user"));
        }
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(data) != data.size()
            || !f.flush()) {
            const QString why = f.errorString();
            f.close();
            QFile::remove(tmp);
            return fail(QStringLiteral("could not write the file (%1)").arg(why));
        }
        f.close();
        // R4: one step that replaces the old file, so there is never a moment
        // with no key file, and a failed replace leaves no copy behind.
#ifdef Q_OS_WIN
        const std::wstring from = QDir::toNativeSeparators(tmp).toStdWString();
        const std::wstring to   = QDir::toNativeSeparators(path).toStdWString();
        if (!::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD code = ::GetLastError();
            QFile::remove(tmp);
            return fail(QStringLiteral("could not replace the key file (Windows error %1)").arg(code));
        }
#else
        if (::rename(QFile::encodeName(tmp).constData(), QFile::encodeName(path).constData()) != 0) {
            const int code = errno;
            QFile::remove(tmp);
            return fail(QStringLiteral("could not replace the key file (%1)").arg(QString::fromLocal8Bit(strerror(code))));
        }
#endif
        return true;
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return fail(QStringLiteral("could not write the file (%1)").arg(f.errorString()));
    }
    return f.write(data) == data.size();
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
    // A key file written by an older version has the folder's default
    // permissions: restrict it now. One that cannot be restricted is not
    // used (S5).
    if (!_clientKeyPem.isEmpty() && !_restrictToOwner(dir + QStringLiteral("/client_key.pem"))) {
        _clientKeyPem.clear();
        const QString line = QStringLiteral("client key file not used: could not restrict it to the current user "
                                            "(%1)").arg(QDir::toNativeSeparators(dir + QStringLiteral("/client_key.pem")));
        _log(line);
        qWarning().noquote() << "TAK:" << line;
    }
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
    // An empty password means the TAK default pass phrase "atakatak" (the
    // settings page leaves the field empty and names the default as a hint);
    // a p12 with a truly empty pass phrase is tried next.
    const QList<QByteArray> passwords = password.isEmpty()
        ? QList<QByteArray>{ QByteArrayLiteral("atakatak"), QByteArray() }
        : QList<QByteArray>{ password.toUtf8() };
    for (const QByteArray &pass : passwords) {
        f.seek(0);
        if (QSslCertificate::importPkcs12(&f, &key, &cert, &chain, pass)) {
            if (!cert.isNull()) {
                cas << cert;
            }
            cas << chain;
            break;
        }
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
    _writePemFile(_certDir() + QStringLiteral("/ca_cert.pem"), _caCertPem, false);
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
    // The key goes first: if it cannot be written nothing on disk has
    // changed, so the old key and certificate still match (R4).
    QString keyError;
    if (!_writePemFile(_certDir() + QStringLiteral("/client_key.pem"), _clientKeyPem, true, &keyError)) {
        // S5: the key is not kept, on disk or in memory.
        _clientKeyPem.clear();
        _log(QStringLiteral("client key not saved: %1").arg(keyError));
        _loadCerts();
        emit enrolledChanged();
        return false;
    }
    QString certError;
    if (!_writePemFile(_certDir() + QStringLiteral("/client_cert.pem"), _clientCertPem, false, &certError)) {
        _log(QStringLiteral("client certificate not saved: %1. Import the identity again.").arg(certError));
    }
    if (!_caCertPem.isEmpty()) {
        _writePemFile(_certDir() + QStringLiteral("/ca_cert.pem"), _caCertPem, false);
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
    // The key goes first: if it cannot be written nothing on disk has
    // changed, so the old key and certificate still match (R4).
    QString keyError;
    if (!_writePemFile(dir + QStringLiteral("/client_key.pem"), _clientKeyPem, true, &keyError)) {
        // S5: the key is not kept, on disk or in memory.
        _loadCerts();
        _setStatus(QStringLiteral("enroll failed"));
        _log(QStringLiteral("enroll: client key not saved: %1").arg(keyError));
        emit enrolledChanged();
        return;
    }
    QString certError;
    if (!_writePemFile(dir + QStringLiteral("/client_cert.pem"), _clientCertPem, false, &certError)) {
        _log(QStringLiteral("enroll: client certificate not saved: %1. Enroll again.").arg(certError));
    }
    if (!_caCertPem.isEmpty()) {
        _writePemFile(dir + QStringLiteral("/ca_cert.pem"), _caCertPem, false);
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
    if (_fleetIsRoute) {   // S4: one route to the TAK server at a time
        _setStatus(QStringLiteral("not connected: the Fleet Server is this station's route to ATAK"));
        _log(QStringLiteral("connect: not opened, the Fleet Server is this station's route to ATAK"));
        return;
    }
    if (_host.isEmpty()) {
        _log(QStringLiteral("connect: no host configured"));
        return;
    }
    _socket = new QSslSocket(this);
    _connectError.clear();
    _rxBuf.clear();

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
    // S2: Qt holds at most this much unread; past it the server waits.
    _socket->setReadBufferSize(kMaxRxBufferBytes);

    connect(_socket, &QSslSocket::encrypted, this, &TakManager::_onEncrypted);
    connect(_socket, &QSslSocket::readyRead, this, &TakManager::_onReadyRead);
    connect(_socket, &QSslSocket::disconnected, this, &TakManager::_onDisconnected);
    connect(_socket, &QSslSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (_socket) {
            _onSocketError(_socket->errorString());
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

    if (!_verifyPeer) {
        const QString line = QStringLiteral("connecting with \"Verify server certificate\" off: the server's "
                                            "identity is not checked, and any device on the network path can "
                                            "pose as the TAK server");
        _log(line);
        qWarning().noquote() << "TAK:" << line;
    }
    _setStatus(QStringLiteral("connecting..."));
    _log(QStringLiteral("connecting to %1:%2").arg(_host).arg(_port));
    _socket->connectToHostEncrypted(_host, static_cast<quint16>(_port));
}

void TakManager::disconnectServer()
{
    _publishTimer.stop();
    _presenceTimer.stop();
    // R5: nothing received on a closed link is handled after it closes
    // (this is also the Fleet hand-off path). A drain already scheduled
    // finds an empty buffer and no socket.
    _rxBuf.clear();
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

void TakManager::_onSocketError(const QString &error)
{
    _log(QStringLiteral("socket error: ") + error);
    if (_connected) {
        return;   // a live link that drops ends in _onDisconnected
    }
    // The link never came up (refused, unreachable, timed out, or the TLS
    // handshake failed). Qt may not emit disconnected for this, so the status
    // would stay at "connecting..." (G45).
    _connectError = error;
    _publishTimer.stop();
    _presenceTimer.stop();
    _setStatus(QStringLiteral("connect failed: %1").arg(error));
}

void TakManager::_onDisconnected()
{
    const bool wasUp = _connected;
    if (_connected) {
        _connected = false;
        emit connectedChanged();
    }
    _publishTimer.stop();
    _presenceTimer.stop();
    // Keep "connect failed: ..." when the link never came up.
    if (wasUp || _connectError.isEmpty()) {
        _setStatus(QStringLiteral("disconnected"));
    }
    _log(QStringLiteral("TAK server link closed"));
}

void TakManager::_onReadyRead()
{
    _drainRx();
}

void TakManager::_drainRx()
{
    _rxDrainScheduled = false;
    // Read only what fits under the bound; the rest waits in the socket
    // (and, past its read buffer, at the server) until this pass is done.
    QByteArray in;
    if (_socket && _rxBuf.size() < kMaxRxBufferBytes) {
        in = _socket->read(kMaxRxBufferBytes - _rxBuf.size());
    }
    _consumeRx(in);
}

void TakManager::_consumeRx(const QByteArray &bytes)
{
    _rxBuf += bytes;
    // Split concatenated <event>...</event> chunks (TAK streams XML back to
    // back). S2: walk with an offset and compact once, and handle at most
    // kMaxRxEventsPerPass events; the rest is handled from the event loop.
    qsizetype pos = 0;
    int handled = 0;
    bool moreEvents = false;
    QElapsedTimer passTime;   // R5: a time budget per pass as well
    passTime.start();
    for (;;) {
        const qsizetype start = _rxBuf.indexOf("<event", pos);
        if (start < 0) {
            pos = _rxBuf.size();
            break;
        }
        const qsizetype end = _rxBuf.indexOf("</event>", start);
        if (end < 0) {
            pos = start;                       // keep partial for next read
            break;
        }
        if (handled >= kMaxRxEventsPerPass || (handled > 0 && passTime.elapsed() >= kRxPassBudgetMs)) {
            pos = start;
            moreEvents = true;
            break;
        }
        const QByteArray chunk = _rxBuf.mid(start, end + 8 - start);
        pos = end + 8;
        ++handled;
        _noteReceived(chunk);
        if (_commandEnabled) {
            _handleMarker(chunk);
        }
    }
    _rxBuf.remove(0, pos);
    // R5: schedule again only after progress, so a read that returns
    // nothing cannot spin; new data raises readyRead anyway.
    const bool progress = handled > 0 || !bytes.isEmpty();
    if (progress && (moreEvents || (_socket && _socket->bytesAvailable() > 0))) {
        if (!_rxDrainScheduled) {
            _rxDrainScheduled = true;
            QTimer::singleShot(0, this, &TakManager::_drainRx);
        }
    }
    // A partial event that never ends (a broken or hostile server) is not
    // kept without limit (G46): drop it and carry on with the next event.
    if (!moreEvents && _rxBuf.size() >= kMaxRxBufferBytes) {
        const QString line = QStringLiteral("TAK receive buffer over %1 KB with no complete event: %2 bytes "
                                            "dropped")
                                 .arg(kMaxRxBufferBytes / 1024).arg(_rxBuf.size());
        _rxBuf.clear();
        _log(line);
        qWarning().noquote() << "TAK:" << line;
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
    if (!_shareStationPosition) {
        return QStringLiteral("position not shared (the GCS beacon carries no location)");
    }
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
    const QHash<QString, int> names = _fleetNames();
    QVariantList rows;
    for (int sysid : sysids) {
        const FleetEntry e = _fleet.value(sysid);
        QVariantMap row;
        row[QStringLiteral("sysid")]     = sysid;
        row[QStringLiteral("callsign")]  = e.callsign;
        row[QStringLiteral("videoUrl")]  = e.videoUrl;
        row[QStringLiteral("tail")]      = e.tail;
        row[QStringLiteral("connected")] = connected.contains(sysid);
        // G47 (T15): a Fleet callsign markers cannot name.
        const bool nameable = !e.callsign.isEmpty()
            && names.value(Tak::normalizeName(e.callsign), -1) == sysid;
        row[QStringLiteral("nameNote")]  = (e.callsign.isEmpty() || nameable)
            ? QString() : TakPolicy::fleetNotNameableNote(e.callsign, _callsignPrefix, sysid);
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
    // With "Share this station's position" off (the default) the beacon still
    // goes out, carrying the CoT unknown point and no location.
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

// With position sharing on: the station position, else the last real
// position a beacon carried on this link. A vehicle link drop or a lost fix
// keeps the last real point fresh; the beacon never switches to the unknown
// point. With sharing off: no position, ever.
QGeoCoordinate TakManager::_beaconPosition() const
{
    if (!_shareStationPosition) {
        return QGeoCoordinate();
    }
    const QGeoCoordinate g = _effectiveGcsPosition();
    return g.isValid() ? g : _lastBeaconPos;
}

// The beacon goes out from _publishTick only with publishing on and "Publish
// GCS beacon" on. With position sharing on it also needs a real position now
// or earlier on this link; with sharing off it always goes out, carrying the
// CoT unknown point, so the station still registers and other stations still
// see its <PREFIX>-GCS for conflict detection. Short-circuits so the position
// is read only when it is shared.
bool TakManager::_beaconAnnounces() const
{
    return _publishEnabled && _publishGcsBeacon
        && (!_shareStationPosition || _beaconPosition().isValid());
}

bool TakManager::_sendBeacon(const QStringList &dest)
{
    if (!_connected || !_publishEnabled || !_publishGcsBeacon) {
        return false;
    }
    if (!_shareStationPosition) {
        // Forget any real point from when sharing was on, so turning it on
        // again later starts from the current position only.
        _lastBeaconPos = QGeoCoordinate();
        _sendEvent(CoT::build(_stationEntity(QGeoCoordinate(), 30, dest)));
        return true;
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
    _startMarkerTimers();
    // Bounded work and memory per event from the stream (5.1.2 review M1):
    // an oversize event or field is not read at all. Its line is subject to
    // the per-second line limit (R6).
    if (eventXml.size() > kMaxCommandEventBytes) {
        _noteIgnoredMarker(CoT::Marker(), QString(),
                           QStringLiteral("event larger than %1 KB").arg(kMaxCommandEventBytes / 1024),
                           QString::number(QDateTime::currentSecsSinceEpoch()));
        return;
    }
    const CoT::Marker m = CoT::parse(eventXml);
    if (!m.valid) {
        return;
    }
    for (const QString *f : { &m.uid, &m.callsign, &m.destCallsign, &m.senderUid, &m.senderCallsign }) {
        if (f->size() > kMaxMarkerField) {
            _noteIgnoredMarker(m, m.callsign, QStringLiteral("a name or UID is longer than %1 characters")
                                                  .arg(kMaxMarkerField));
            return;
        }
    }
    // G47: other stations and their aircraft, and another station using this
    // prefix, from device reports only. Before loop prevention, which drops
    // every uid that starts with this prefix.
    _noteAddressSignals(m);

    // Loop prevention: ignore our own published entities (their uid is
    // <prefix>-<sysid>). An empty prefix reads no command; that is said on
    // the TAK page and in a line below (G50).
    const bool prefixEmpty = _callsignPrefix.isEmpty();
    if ((!prefixEmpty && m.uid.startsWith(_callsignPrefix + QLatin1Char('-'), Qt::CaseInsensitive))
        || m.uid.compare(_gcsUid, Qt::CaseInsensitive) == 0) {
        return;
    }

    // What the event would command: a named marker whose callsign carries a
    // verb ("GOTO_2_ALT_150"), or a plain point "sent to" one of our drones
    // (<marti><dest callsign="ROTOR-3"/>). Anything else is not a command and
    // is left alone without a log line.
    Tak::Command cmd;
    QString markerText;
    QString malformed;     // a name that starts with a verb but does not parse
    const QString name = m.callsign.isEmpty() ? m.destCallsign : m.callsign;
    if (!m.callsign.isEmpty()) {
        QString err;
        if (Tak::parseCommand(m.callsign, cmd, err)) {
            markerText = m.callsign;
        } else if (cmd.verb != Tak::Verb::Invalid) {
            malformed = err;
        }
    }
    if (markerText.isEmpty() && !m.destCallsign.isEmpty()) {
        const QRegularExpression re(QStringLiteral("^%1-(\\d+)$").arg(QRegularExpression::escape(_callsignPrefix)),
                                    QRegularExpression::CaseInsensitiveOption);
        const auto mo = re.match(m.destCallsign.trimmed());
        if (mo.hasMatch()) {
            // G40: only the names this station publishes are addressable.
            const QString ignored = _sendToIgnoredReason(mo.captured(1).toInt());
            if (!ignored.isEmpty()) {
                _noteIgnoredMarker(m, name, QStringLiteral("point sent to %1 is not read: %2")
                                                .arg(m.destCallsign, ignored));
                return;
            }
            cmd = Tak::Command();
            cmd.verb = Tak::Verb::Goto;
            cmd.target = Tak::Target::Ids;
            cmd.ids = { mo.captured(1).toInt() };
            // The send-to names this station's aircraft by its published uid.
            cmd.name = Tak::normalizeName(_callsignPrefix);
            markerText = QStringLiteral("point sent to ") + m.destCallsign;
        }
    } else if (!markerText.isEmpty() && !m.destCallsign.isEmpty()) {
        // A command marker also sent to one of this station's aircraft: the
        // two must name the same aircraft (G47, T11).
        const QString dest = Tak::normalizeName(m.destCallsign);
        const QRegularExpression ownUid(QStringLiteral("^%1-([0-9]+)$")
                                            .arg(QRegularExpression::escape(Tak::normalizeName(_callsignPrefix))));
        const QRegularExpressionMatch dm = ownUid.match(dest);
        int destId = dm.hasMatch() ? dm.captured(1).toInt() : -1;
        if (destId < 0) {
            for (auto it = _fleet.constBegin(); it != _fleet.constEnd(); ++it) {
                if (!it.value().callsign.isEmpty() && Tak::normalizeName(it.value().callsign) == dest) {
                    destId = it.key();
                    break;
                }
            }
        }
        if (destId >= 0) {
            cmd.sentTo   = dest;
            cmd.sentToId = destId;
        }
    }
    if (markerText.isEmpty() && malformed.isEmpty()) {
        return;
    }
    if (prefixEmpty) {
        _noteIgnoredMarker(m, name, TakPolicy::emptyPrefixReason());
        return;
    }
    if (!TakPolicy::prefixProblem(_callsignPrefix).isEmpty()) {
        _noteIgnoredMarker(m, name, TakPolicy::prefixInvalidReason());
        return;
    }
    if (!prefixSet()) {
        _noteIgnoredMarker(m, name, TakPolicy::prefixNotSetReason());
        return;
    }

    // Commands come only from a marker a person placed (G38): never from a
    // device's own position report, a machine-generated event, an event
    // with no creating device, or a stale one. Nor from a marker placed
    // before this station started (review S3).
    const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
    const QString notMarker = TakPolicy::markerNotCommandReason(
        m, nowUtc, _sessionStartUtc.addSecs(-kSessionSkewSecs), _repeatFloorUtc);
    if (!notMarker.isEmpty()) {
        if (TakPolicy::isMarkerTimeReason(notMarker)) {
            _noteDrop(DropTime);
        }
        _noteIgnoredMarker(m, name, notMarker);
        return;
    }
    // G47: a marker placed before the prefix last changed is not run.
    if (_prefixFloorUtc.isValid() && m.time < _prefixFloorUtc) {
        _noteIgnoredMarker(m, name, TakPolicy::prefixFloorReason());
        return;
    }
    if (markerText.isEmpty()) {
        _noteIgnoredMarker(m, name, QStringLiteral("command not understood (%1)").arg(malformed));
        return;
    }

    // One command per marker (G43, G36). ATAK re-sends a marker on every
    // edit with the same uid and a new time, and a server can replay recent
    // CoT on connect: neither runs the command again. A new command needs a
    // new marker.
    const QString time = m.time.toString(Qt::ISODateWithMs);
    const QString seenKey = m.uid.trimmed().toLower();
    const auto seen = _markersSeen.constFind(seenKey);
    if (seen != _markersSeen.constEnd()) {
        if (seen->time == time) {
            _noteIgnoredMarker(m, name, QStringLiteral("already received; a repeated marker is not run again"), time);
        } else {
            _noteIgnoredMarker(m, name, QStringLiteral("changed after its command was received; an edited "
                                                       "marker is not run again. Place a new marker"), time);
        }
        return;
    }
    // A marker dropped earlier this session (rate limit or full repeat list)
    // is not run if it is delivered again (L1).
    const QByteArray uidHash = QCryptographicHash::hash(seenKey.toUtf8(), QCryptographicHash::Sha1);
    if (_droppedUids.contains(uidHash)) {
        _noteIgnoredMarker(m, name, TakPolicy::droppedEarlierReason());
        return;
    }
    // G47: a marker for another station seen on this link is not this
    // station's. One line, nothing listed, and it does not use this
    // station's rate budget or repeat list. An edit or repeat of it is not
    // run here either (T5, T6).
    if (_otherStationUids.contains(uidHash)) {
        _noteIgnoredMarker(m, name, TakPolicy::otherStationEarlierReason());
        return;
    }
    {
        const TakPolicy::AddressResult a = TakPolicy::classifyAddress(cmd, _addressInput());
        if (a.kind == TakPolicy::Address::OtherStation) {
            _noteIgnoredMarker(m, name, TakPolicy::otherStationReason(a.name, a.station, _callsignPrefix));
            _rememberOtherStation(uidHash);
            return;
        }
    }
    // At most a few command markers per second and per minute, counted after
    // the checks above so repeats and edits do not use the budget, and with
    // a separate budget for listed senders (reviews S1, R1).
    QString who;
    const bool listed = _approvedOnly && _senderApproved(m, who);
    if (!_commandRateAllows(listed)) {
        _noteDrop(DropRate);
        _rememberDropped(seenKey);
        return;
    }
    // Full repeat list (E1): drop the oldest entry only if its marker time is
    // at least 30 minutes old, and raise the floor to that time so a replay
    // of it is refused. Otherwise refuse the new marker (fail closed).
    while (_markersSeen.size() >= _maxMarkersSeen && !_seenOrder.isEmpty()) {
        const QPair<QString, quint64> &front = _seenOrder.first();
        const auto it = _markersSeen.find(front.first);
        if (it == _markersSeen.end() || it->seq != front.second) {
            _seenOrder.removeFirst();          // pruned or re-inserted since
            continue;
        }
        if (it->cotTime > nowUtc.addSecs(-kRepeatFloorAgeSecs)) {
            break;
        }
        if (!_repeatFloorUtc.isValid() || it->cotTime > _repeatFloorUtc) {
            _repeatFloorUtc = it->cotTime;
        }
        // An edit of the evicted marker (same UID, new time) passes the
        // floor, so its UID is refused for the session too.
        _rememberDropped(front.first);
        _markersSeen.erase(it);
        _seenOrder.removeFirst();
    }
    if (_markersSeen.size() >= _maxMarkersSeen) {
        // Fail closed: a refused command never moves an aircraft. The timer
        // prunes entries whose (clamped) stale time has passed.
        _noteDrop(DropFull);
        _rememberDropped(seenKey);
        _noteIgnoredMarker(m, name, TakPolicy::repeatListFullReason());
        return;
    }
    // The stored stale time is clamped, so an entry always leaves within
    // kMarkerKeepSecs. A replay after that is older than 24 hours and is
    // refused by markerNotCommandReason.
    const quint64 seq = ++_seenSeq;
    _markersSeen.insert(seenKey, MarkerSeen{ time, std::min(m.stale, nowUtc.addSecs(kMarkerKeepSecs)), m.time, seq });
    _seenOrder.append(qMakePair(seenKey, seq));

    _coalesceListSignal = true;
    const QString result = _receiveCommand(cmd, m.lat, m.lon, markerText,
                                           m.senderCallsign, m.senderUid, false, eventXml);
    _coalesceListSignal = false;
    _log(QStringLiteral("ATAK %1 -> %2").arg(markerText, result));
}

void TakManager::_noteIgnoredMarker(const CoT::Marker &m, const QString &name, const QString &reason,
                                    const QString &onceKey)
{
    const QByteArray key = QCryptographicHash::hash(
        (m.uid + QLatin1Char('|') + reason + QLatin1Char('|') + onceKey).toUtf8(), QCryptographicHash::Sha1);
    if (_ignoredNoted.contains(key)) {
        return;
    }
    if (_ignoredNoted.size() >= 5000) {
        _ignoredNoted.clear();
    }
    _ignoredNoted.insert(key);
    const qint64 sec = QDateTime::currentSecsSinceEpoch();
    if (sec != _ignoredLineSecond) {
        _ignoredLineSecond = sec;
        _ignoredLineCount = 0;
    }
    if (++_ignoredLineCount > kMaxIgnoredLinesPerSecond) {
        ++_ignoredLinesDropped;
        return;
    }
    _log(TakPolicy::markerIgnoredLine(reason, name.left(kMaxMarkerField), m.uid.left(kMaxMarkerField)));
}

void TakManager::_rememberDropped(const QString &seenKey)
{
    const QByteArray h = QCryptographicHash::hash(seenKey.toUtf8(), QCryptographicHash::Sha1);
    if (_droppedUids.contains(h)) {
        return;
    }
    if (_droppedOrder.size() >= kMaxDroppedUids) {
        _droppedUids.remove(_droppedOrder.takeFirst());
    }
    _droppedUids.insert(h);
    _droppedOrder.append(h);
}

void TakManager::_noteDrop(DropKind kind)
{
    ++_dropCount[kind];
    _dropLastMs[kind] = QDateTime::currentMSecsSinceEpoch();
}

void TakManager::_updateDropNotice()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int k = 0; k < DropKinds; ++k) {
        if (_dropCount[k] > 0 && now - _dropLastMs[k] > 60 * 1000) {
            _dropCount[k] = 0;   // no drop of this kind for a minute
        }
    }
    const QString notice = TakPolicy::commandDropNotice(_dropCount[DropRate], _dropCount[DropFull],
                                                        _dropCount[DropTime], _maxCommandsPerSecond,
                                                        _maxCommandsPerMinute);
    _updateConflictNotice();
    QString fly = TakPolicy::flyDropNotice(_dropCount[DropRate] + _dropCount[DropFull] + _dropCount[DropTime]);
    if (_prefixConflictActive()) {   // G47 (T9)
        const QString line = TakPolicy::prefixConflictFlyLine(_prefixConflictPrefix);
        fly = fly.isEmpty() ? line : line + QLatin1Char(' ') + fly;
    }
    if (notice != _commandDropNotice || fly != _flyDropNotice) {
        _commandDropNotice = notice;
        _flyDropNotice     = fly;
        emit commandDropNoticeChanged();
    }
}

void TakManager::_startMarkerTimers()
{
    if (_markerTimerReady) {
        return;
    }
    _markerTimerReady = true;
    _markerTimer.setInterval(1000);
    _markerTimer.callOnTimeout(this, &TakManager::_markerTimerTick);
    _markerTimer.start();
    _listTimer.setSingleShot(true);
    _listTimer.setInterval(250);
    _listTimer.callOnTimeout(this, [this]() { emit remoteCommandsChanged(); });
}

bool TakManager::_commandRateAllows(bool listedSender)
{
    const int b = listedSender ? 0 : 1;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 sec = nowMs / 1000;
    const qint64 min = nowMs / 60000;
    if (sec != _rateSecond[b]) {
        _rateSecond[b] = sec;
        _rateSecondCount[b] = 0;
    }
    if (min != _rateMinute[b]) {
        _rateMinute[b] = min;
        _rateMinuteCount[b] = 0;
    }
    if (_rateSecondCount[b] >= _maxCommandsPerSecond || _rateMinuteCount[b] >= _maxCommandsPerMinute) {
        ++_rateDropped;
        return false;
    }
    ++_rateSecondCount[b];
    ++_rateMinuteCount[b];
    return true;
}

void TakManager::_markerTimerTick()
{
    _flushRateSummary();
    _updateDropNotice();
    if (++_markerTimerTicks >= 60) {
        _markerTimerTicks = 0;
        _pruneMarkersSeen();
        _pruneAddressSeen();
    }
}

void TakManager::_pruneMarkersSeen()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (auto it = _markersSeen.begin(); it != _markersSeen.end();) {
        it = (it->stale <= now) ? _markersSeen.erase(it) : std::next(it);
    }
    // Drop eviction keys whose entries are gone.
    _seenOrder.removeIf([this](const QPair<QString, quint64> &p) {
        const auto it = _markersSeen.constFind(p.first);
        return it == _markersSeen.constEnd() || it->seq != p.second;
    });
}

void TakManager::_flushRateSummary()
{
    if (_rateDropped > 0) {
        _log(TakPolicy::rateLimitLine(_rateDropped, _maxCommandsPerSecond, _maxCommandsPerMinute));
        _rateDropped = 0;
    }
    if (_ignoredLinesDropped > 0) {
        _log(TakPolicy::ignoredLinesDroppedLine(_ignoredLinesDropped, kMaxIgnoredLinesPerSecond));
        _ignoredLinesDropped = 0;
    }
}

// ---------------------------------------------------------------------------
// Addressing (G47). Routing, not authentication: every name read here is
// written by the sending device, so a peer on the server can fake any of it.
// A faked conflict stops commands (fails closed); it never sends one.
// ---------------------------------------------------------------------------

QHash<QString, int> TakManager::_fleetNames() const
{
    QHash<QString, int> names;
    QSet<QString> shared;
    for (auto it = _fleet.constBegin(); it != _fleet.constEnd(); ++it) {
        const QString cs = it.value().callsign;
        if (cs.isEmpty() || !TakPolicy::fleetCallsignNameable(cs, _callsignPrefix)) {
            continue;
        }
        const QString n = Tak::normalizeName(cs);
        if (names.contains(n)) {
            shared.insert(n);
        }
        names.insert(n, it.key());
    }
    for (const QString &n : std::as_const(shared)) {
        names.remove(n);
    }
    return names;
}

TakPolicy::AddressInput TakManager::_addressInput() const
{
    TakPolicy::AddressInput in;
    in.ownPrefix  = _callsignPrefix;
    in.fleetNames = _fleetNames();
    const qint64 since = QDateTime::currentMSecsSinceEpoch() - TakPolicy::kAddressSeenSecs * 1000LL;
    for (auto it = _fleetConflicts.constBegin(); it != _fleetConflicts.constEnd(); ++it) {
        if (it.value() > since) {
            in.fleetConflicts.insert(it.key());
        }
    }
    for (auto it = _seenStations.constBegin(); it != _seenStations.constEnd(); ++it) {
        if (it.value() > since) {
            in.otherStations.insert(it.key());
        }
    }
    for (auto it = _seenAircraft.constBegin(); it != _seenAircraft.constEnd(); ++it) {
        if (it.value().second > since) {
            in.otherAircraft.insert(it.key(), it.value().first);
        }
    }
    return in;
}

void TakManager::_noteAddressSignals(const CoT::Marker &m)
{
    // A placed marker never counts: only a device's own report does.
    if (!m.isEud) {
        return;
    }
    if (m.uid.compare(_gcsUid, Qt::CaseInsensitive) == 0
        || (m.hasParentLink && m.senderUid.compare(_gcsUid, Qt::CaseInsensitive) == 0)) {
        return;   // this station's own event, relayed back
    }
    const QString own = Tak::normalizeName(_callsignPrefix);
    if (own.isEmpty()) {
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QString cs = Tak::normalizeName(m.callsign);
    // A linked aircraft track's callsign is the Fleet name its station gave
    // it, not a station: "BRAVO GCS" or "BRAVO 1" on ALPHA's aircraft says
    // nothing about station BRAVO. Only a device's own report,
    // with no parent link, names a station by its callsign.
    const QString station = m.hasParentLink ? QString() : TakPolicy::stationPrefixOf(m.callsign);
    const QString parentStation = m.hasParentLink ? TakPolicy::stationPrefixOf(m.senderCallsign) : QString();
    const QRegularExpression ownAircraft(QStringLiteral("^%1-[0-9]+$").arg(QRegularExpression::escape(own)));

    // Another station using this prefix: its station event <prefix>-GCS, an
    // aircraft track whose uid is <prefix>-<n>, or a track it reports for
    // <prefix>-GCS. A callsign alone never counts: a Fleet name such as
    // "ROTOR 1" on another station's aircraft is not this prefix in use.
    if (station == own || parentStation == own || ownAircraft.match(Tak::normalizeName(m.uid)).hasMatch()) {
        const bool wasActive = _prefixConflictActive();
        _prefixConflictMs = now;
        _prefixConflictPrefix = own;
        if (!wasActive) {
            _log(TakPolicy::prefixConflictRefusal(_callsignPrefix)
                 + QStringLiteral(" (seen: \"%1\", UID %2)").arg(m.callsign.left(kMaxMarkerField),
                                                                  m.uid.left(kMaxMarkerField)));
        }
        _updateConflictNotice();
        return;
    }
    auto boundedInsert = [this](auto &table, const QString &key, const auto &value) {
        if (!table.contains(key) && table.size() >= kMaxAddressSeen) {
            _pruneAddressSeen();
            if (table.size() >= kMaxAddressSeen) {
                return;   // full of fresh names: this one counts as not seen
            }
        }
        table.insert(key, value);
    };
    if (!station.isEmpty()) {
        boundedInsert(_seenStations, station, now);
        return;
    }
    // Another station's aircraft: a track linked to its <prefix>-GCS.
    if (!parentStation.isEmpty()) {
        boundedInsert(_seenStations, parentStation, now);
        // A Fleet name shaped like another station's aircraft name ("BRAVO 1"
        // on ALPHA's aircraft) is not kept as ALPHA's aircraft: BRAVO-1 is
        // the name station BRAVO publishes. It is never nameable (T15).
        static const QRegularExpression stationAircraft(QStringLiteral("^([A-Z][A-Z0-9]{1,11})-[0-9]+$"));
        const QRegularExpressionMatch shaped = stationAircraft.match(cs);
        if (!cs.isEmpty() && (!shaped.hasMatch() || shaped.captured(1) == parentStation)) {
            boundedInsert(_seenAircraft, cs, qMakePair(parentStation, now));
        }
        const QString uidName = Tak::normalizeName(m.uid);
        if (!uidName.isEmpty() && uidName != cs) {
            boundedInsert(_seenAircraft, uidName, qMakePair(parentStation, now));
        }
    }
    // One of this station's Fleet callsigns on someone else's report.
    if (!cs.isEmpty() && _fleetNames().contains(cs)) {
        _fleetConflicts.insert(cs, now);
    }
}

qint64 TakManager::_prefixLastSeenElsewhere(const QString &prefix) const
{
    const QString p = Tak::normalizeName(prefix);
    const qint64 since = QDateTime::currentMSecsSinceEpoch() - TakPolicy::kAddressSeenSecs * 1000LL;
    qint64 last = 0;
    const qint64 st = _seenStations.value(p, 0);
    if (st > since) {
        last = st;
    }
    for (auto it = _seenAircraft.constBegin(); it != _seenAircraft.constEnd(); ++it) {
        if (it.value().first == p && it.value().second > since) {
            last = std::max(last, it.value().second);
        }
    }
    // This station's own prefix in conflict counts too.
    if (p == Tak::normalizeName(_callsignPrefix) && _prefixConflictActive()) {
        last = std::max(last, _prefixConflictMs);
    }
    return last;
}

bool TakManager::_prefixConflictActive() const
{
    return _prefixConflictMs > 0
        && QDateTime::currentMSecsSinceEpoch() - _prefixConflictMs < TakPolicy::kAddressSeenSecs * 1000LL;
}

void TakManager::_updateConflictNotice()
{
    QString notice;
    if (_prefixConflictActive()) {
        notice = TakPolicy::prefixConflictNotice(
            _prefixConflictPrefix,
            QDateTime::fromMSecsSinceEpoch(_prefixConflictMs, QTimeZone::UTC).toString(QStringLiteral("HH:mm:ss")));
    }
    if (notice != _prefixConflictNotice) {
        _prefixConflictNotice = notice;
        emit addressingChanged();
    }
}

void TakManager::_pruneAddressSeen()
{
    const qint64 since = QDateTime::currentMSecsSinceEpoch() - TakPolicy::kAddressSeenSecs * 1000LL;
    _seenStations.removeIf([since](const QHash<QString, qint64>::iterator it) { return it.value() <= since; });
    _fleetConflicts.removeIf([since](const QHash<QString, qint64>::iterator it) { return it.value() <= since; });
    _seenAircraft.removeIf([since](const QHash<QString, QPair<QString, qint64>>::iterator it) {
        return it.value().second <= since;
    });
}

void TakManager::_rememberOtherStation(const QByteArray &uidHash)
{
    if (_otherStationUids.contains(uidHash)) {
        return;
    }
    if (_otherStationOrder.size() >= kMaxDroppedUids) {
        _otherStationUids.remove(_otherStationOrder.takeFirst());
    }
    _otherStationUids.insert(uidHash);
    _otherStationOrder.append(uidHash);
}

QList<Vehicle *> TakManager::_addressedTargets(const Tak::Command &cmd) const
{
    const TakPolicy::AddressResult a = TakPolicy::classifyAddress(cmd, _addressInput());
    if (a.kind != TakPolicy::Address::Own) {
        return {};
    }
    Tak::Command resolved = cmd;
    resolved.target = a.all ? Tak::Target::All : Tak::Target::Ids;
    resolved.ids    = a.all ? QList<int>() : a.ids;
    return _resolveTargets(resolved);
}

QString TakManager::_sendToIgnoredReason(int sysid) const
{
    if (!_dronesAddressable) {
        return QStringLiteral("\"Let ATAK address drones\" is off");
    }
    const QString fleetCallsign = _fleet.value(sysid).callsign;
    if (!fleetCallsign.isEmpty()) {
        return QStringLiteral("aircraft %1 is published as %2, not %3-%1")
            .arg(sysid).arg(fleetCallsign, _callsignPrefix);
    }
    return QString();
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
    settings[QStringLiteral("callsignPrefix")] = _callsignPrefix;
    settings[QStringLiteral("prefixConflict")] = _prefixConflictActive();

    TakPolicy::RemoteCommand rc;
    rc.verb           = Tak::verbName(cmd.verb);
    rc.markerText     = markerText;
    rc.senderCallsign = senderCallsign;
    rc.senderUid      = senderUid;
    rc.test           = test;
    if (cmd.verb == Tak::Verb::Goto) {
        rc.position = TakPolicy::pointKnown(lat, lon) ? TakPolicy::positionText(lat, lon, cmd.altFt)
                                                      : TakPolicy::unknownPointText(cmd.altFt);
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
    params[QStringLiteral("lat")] = std::isfinite(lat) ? QJsonValue(lat) : QJsonValue();
    params[QStringLiteral("lon")] = std::isfinite(lon) ? QJsonValue(lon) : QJsonValue();
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

// SEL (G48) and a name alone resolve to no aircraft here: the selection on
// this station is never read for a command from ATAK.
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
    } else if (cmd.target == Tak::Target::Ids) {
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

    // G47, G48: the command must name this station's aircraft. SEL, a marker
    // that names no station, and a name no station here publishes are not
    // run. This is routing, not authentication: the name is written by the
    // sending device. The test button follows the same rule.
    if (const QString why = _prefixUnusableReason(); !why.isEmpty()) {
        return refused(why);
    }
    const TakPolicy::AddressResult addr = TakPolicy::classifyAddress(cmd, _addressInput());
    if (addr.kind != TakPolicy::Address::Own) {
        _log(QStringLiteral("refused %1 (%2) from %3").arg(Tak::verbName(cmd.verb),
                                                          TakPolicy::intendedTargetText(cmd), source));
        return refused(TakPolicy::addressRefusal(addr, cmd, _callsignPrefix));
    }
    if (_prefixConflictActive()) {
        return refused(TakPolicy::prefixConflictRefusal(_prefixConflictPrefix));
    }
    Tak::Command resolved = cmd;
    resolved.target = addr.all ? Tak::Target::All : Tak::Target::Ids;
    resolved.ids    = addr.all ? QList<int>() : addr.ids;

    // A GOTO goes to one aircraft and one known point (G39). Several aircraft
    // sent to the same point would meet there, and an unknown point reads as
    // 0, 0. RTL, LAND, HOLD and the others act at each aircraft's own place
    // and may name several.
    if (resolved.verb == Tak::Verb::Goto) {
        if (resolved.target == Tak::Target::All || resolved.ids.size() > 1) {
            return refused(TakPolicy::gotoMultipleRefusal());
        }
        if (!TakPolicy::pointKnown(lat, lon)) {
            return refused(TakPolicy::gotoUnknownPointRefusal());
        }
    }

    const QList<Vehicle *> targets = _resolveTargets(resolved);
    if (targets.isEmpty()) {
        return refused(TakPolicy::noAircraftConnected(addr.name, addr.all || addr.ids.size() > 1));
    }
    if (cmd.verb == Tak::Verb::Goto && targets.size() > 1) {   // backstop: one aircraft per GOTO
        return refused(TakPolicy::gotoMultipleRefusal());
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
        // TKOFF arms an aircraft that is not armed, so it is held to the ARM
        // switch (G49): with ARM off it is never sent to an aircraft that
        // telemetry reports not armed.
        const bool takeoffNeedsArm = cmd.verb == Tak::Verb::Takeoff && !_verbArm && !veh->armed();
        DispatchResult r;
        if (takeoffNeedsArm) {
            r.msg = TakPolicy::takeoffNeedsArmRefusal();
            _log(QStringLiteral("refused TKOFF to aircraft %1 (ARM off, aircraft not armed) from %2")
                     .arg(veh->id()).arg(source));
        } else {
            r = _dispatchOne(veh, cmd, lat, lon);
        }
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
    // As this station publishes it to TAK (T16): "aircraft ROTOR-2",
    // "aircraft ROTOR-2 (HAWK)".
    const QString cs = _fleet.value(veh->id()).callsign;
    const QString uid = QStringLiteral("%1-%2").arg(Tak::normalizeName(_callsignPrefix)).arg(veh->id());
    return cs.isEmpty() ? QStringLiteral("aircraft %1").arg(uid)
                        : QStringLiteral("aircraft %1 (%2)").arg(uid, cs);
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
    if (_coalesceListSignal && _markerTimerReady) {
        if (!_listTimer.isActive()) {
            _listTimer.start();
        }
    } else {
        emit remoteCommandsChanged();
    }
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
        _lastTestSent = false;
        _recordRemoteCommand(rc, false, ctx);
        const QString line = QStringLiteral("TEST %1 -> %2").arg(markerCallsign, rc.result);
        _log(line);
        return line;
    }
    // Place the synthetic point ~80 m north of the first target so a GOTO
    // visibly moves the aircraft. Runs the real receive -> checks -> dispatch
    // path, as the station's test sender (see TakPolicy::testSenderUid).
    // With no aircraft position there is no test point: the point stays
    // unknown and a GOTO is refused (G39), never sent to 0, 0.
    double lat = std::nan(""), lon = std::nan("");
    const QList<Vehicle *> targets = _addressedTargets(cmd);
    if (!targets.isEmpty() && targets.first()->coordinate().isValid()) {
        const QGeoCoordinate c = targets.first()->coordinate();
        lat = c.latitude() + 80.0 / 111320.0;
        lon = c.longitude();
    }
    const QString result = _receiveCommand(cmd, lat, lon, markerCallsign,
                                           TakPolicy::testSenderCallsign(),
                                           TakPolicy::testSenderUid(), true, markerCallsign.toUtf8());
    _lastTestSent = !_remoteCommands.isEmpty()
                    && _remoteCommands.first().toMap().value(QStringLiteral("sent")).toBool();
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
        check(QStringLiteral("GOTO ROTOR-2 ALT 150 names ROTOR, aircraft 2 (G47)"),
              Tak::parseCommand(QStringLiteral("GOTO ROTOR-2 ALT 150"), c, e)
              && c.name == QLatin1String("ROTOR") && c.ids == QList<int>{ 2 } && c.altFt == 150);
        TakPolicy::AddressInput in;
        in.ownPrefix = QStringLiteral("ROTOR");
        check(QStringLiteral("a marker that names no station is not run (G47)"),
              Tak::parseCommand(QStringLiteral("GOTO 2"), c, e)
              && TakPolicy::classifyAddress(c, in).kind == TakPolicy::Address::Unaddressed);
        check(QStringLiteral("SEL is never run (G48)"),
              Tak::parseCommand(QStringLiteral("HOLD ROTOR-SEL"), c, e)
              && TakPolicy::classifyAddress(c, in).kind == TakPolicy::Address::Sel);
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
