/****************************************************************************
 *
 * RotorLab GC - license activation manager.
 *
 * One QObject, exposed to QML as `LicenseManager`. Gates the application
 * behind a RotorLab device-bound license key.
 *
 *   - generates + persists a stable per-install device id (silent)
 *   - activate / deactivate / re-verify a license key against the store
 *   - device-locked: the license binds to this install's device id
 *   - keeps the RotorLab-signed offline activation record (G89,
 *     LicenseRecord.h) so the app opens offline once activated, until the
 *     record's offline_until. The record is checked against the RotorLab
 *     public keys compiled into GC: a settings file edit cannot open GC.
 *     After offline_until, or when the clock is set back, GC asks for a
 *     connection at the next start.
 *   - never opens on a 5.2.0-or-earlier (unsigned) record: it only sends
 *     that record's key to the online check, which brings a signed record
 *   - re-checks the license while running: every 24 hours, and when the
 *     network comes back. A re-check while running only notes the result. It
 *     never closes the session, never shows the license gate and never clears
 *     the stored activation, so it cannot interrupt a flight. The check at the
 *     next start applies the result.
 *   - never puts the gate over a connected vehicle: when the stored record has
 *     only run out of time (or the clock looks wrong) and a vehicle is
 *     connected at start, or connects while the gate is up, GC opens for that
 *     session and says the license needs a check.
 *   - no link behind the gate: linksAllowed() is the
 *     one C++ answer to "may GC open vehicle links". CustomPlugin::init feeds
 *     it to LinkManager, which then starts no auto-connect link and refuses a
 *     manual connect while it is false; the joystick sends nothing either.
 *     Deactivate is refused while a vehicle is connected, and turning links
 *     off also disconnects TAK.
 *
 * The license key is sent only in the JSON request body over verified HTTPS.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QString>

#include "LicenseRecord.h"

class QJsonObject;
class QNetworkAccessManager;
class QTimer;
class QNetworkReply;

class LicenseManager : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool    activated     READ activated     NOTIFY activatedChanged)
    Q_PROPERTY(bool    busy          READ busy          NOTIFY busyChanged)
    Q_PROPERTY(QString statusText    READ statusText    NOTIFY statusChanged)
    Q_PROPERTY(QString licenseKey    READ licenseKey    NOTIFY statusChanged)   ///< masked for display
    Q_PROPERTY(QString deviceId      READ maskedDeviceId NOTIFY statusChanged)  ///< masked for display
    /// A re-check while running found the license expired, revoked or no
    /// longer valid for this device. The session goes on; the settings page
    /// shows it.
    Q_PROPERTY(bool    attention     READ attention     NOTIFY statusChanged)
    /// GC may open vehicle links (linksAllowed()). The gate says so when not.
    Q_PROPERTY(bool    linksAllowed  READ linksAllowed  NOTIFY linksAllowedChanged)
    /// Why "Deactivate this device" is off while a vehicle is connected.
    Q_PROPERTY(QString deactivateVehicleText READ deactivateVehicleText CONSTANT)

public:
    explicit LicenseManager(QObject *parent = nullptr);
    ~LicenseManager() override;

    static LicenseManager *instance();

    bool    activated() const { return _activated; }
    bool    busy() const { return _busy; }
    bool    attention() const { return _attention; }
    QString statusText() const { return _statusText; }
    QString licenseKey() const;         ///< masked (e.g. ROTOR-****-****-AB12)
    QString maskedDeviceId() const;     ///< masked (e.g. 8f3c…a91d)

    /// May GC open vehicle links and send to a vehicle? True when GC is
    /// activated, or when this device holds a genuine signed record that
    /// verifies but only ran out of time (or the clock looks wrong): that
    /// install still opens for a vehicle that connects (_openForVehicle), so
    /// its links must be able to start. False with no signed record, with
    /// only a 5.2.0 record, or with a record that does not verify.
    bool    linksAllowed() const { return _activated || _timeLapsed; }

    /// Activate `key` for this device against the store. Async; updates
    /// activated/status on completion.
    Q_INVOKABLE void activate(const QString &key);

    /// Release this device's activation so the key can be used elsewhere.
    Q_INVOKABLE void deactivate();

    /// Online re-verify of the stored key for this device. The first one, at
    /// launch, can close the license gate or open it, unless a vehicle is
    /// connected when its reply arrives. Any later one (Re-check now, the
    /// 24-hour timer, network regain) only notes the result.
    /// Never downgrades an already-activated app while offline.
    Q_INVOKABLE void recheck();

    /// Text shown when the license server cannot be reached to deactivate.
    static QString offlineReleaseHint();

    static constexpr int kRecheckIntervalMs = 24 * 60 * 60 * 1000;
    static constexpr int kRegainMinSpacingMs = 10 * 60 * 1000;

    /// Gate texts, plain, saying what GC does.
    static QString expiredText();
    static QString clockText();
    /// Gate text for an offline start with only a 5.2.0-or-earlier record.
    static QString legacyText();
    /// Gate line while linksAllowed() is false.
    static QString noLinksText();
    /// Shown when a link is refused while linksAllowed() is false.
    static QString linkRefusedText();
    /// Deactivate refused: a vehicle is connected.
    static QString deactivateVehicleText();

signals:
    void activatedChanged();
    void linksAllowedChanged(bool allowed);
    void busyChanged();
    void statusChanged();
    void logMessage(const QString &line);

private:
    friend class LicenseRecheckTest;
    friend class LicenseRecordTest;
    friend class LicenseLinkGateTest;

    enum class Op { Activate, Deactivate, Verify };

    void    _startBackgroundChecks();
    void    _backgroundRecheck();
    bool    _networkReachable(bool online);   ///< true when it asked for a re-check
    void    _noteInSession(const QString &state, const QString &error);
    static bool _anyVehicleConnected();

    void    _setActivated(bool on);
    void    _setBusy(bool on);
    void    _setStatus(const QString &text);
    /// Emits linksAllowedChanged when linksAllowed() changed; on a change to
    /// false also disconnects TAK. Every state change ends in _setActivated
    /// or _setStatus, which both call it.
    void    _noteLinksAllowed();
    static void _disconnectTakNow();

    QString _ensureDeviceId();          ///< load or create the persisted device id
    void    _loadRecord();              ///< the start: check the stored record, set the gate
    void    _loadLegacy(qint64 now);   ///< 5.2.0 record: key for the online check only
    bool    _readLegacy(QString *key) const;   ///< 5.2.0 XOR record, if it is this device's
    void    _storeKey(const QString &key);
    QString _loadKey() const;
    void    _clearRecord();             ///< signed record, key and legacy record
    void    _noteTime(qint64 now, qint64 signedTime);   ///< raise the highest time seen
    qint64  _now() const;

    /// Checks the reply's offline_record against the pinned keys and stores it
    /// (with the key) when it is valid.
    LicenseRecord::Check _acceptRecord(const QJsonObject &reply, const QString &key);
    QString _textFor(LicenseRecord::Check c) const;
    QString _offlineUntilText() const;

    /// The stored activation ran out of time at start. Gate up, unless a
    /// vehicle is connected.
    void    _timeProblemAtStart(const QString &text);
    void    _openForVehicle();
    void    _vehicleAppeared();

    void    _send(Op op, const QString &key);
    void    _handleReply(Op op, const QString &key, const QByteArray &body,
                         bool networkOk, int status);

    QNetworkAccessManager *_nam = nullptr;
    QTimer  *_recheckTimer = nullptr;
    qint64   _lastAttemptMs = 0;    ///< QDateTime ms of the last verify sent
    bool     _wasOnline = true;
    bool     _inSession = false;    ///< past the launch check, or activated here
    bool     _attention = false;
    /// Any vehicle connected (armed or not). Replaceable for tests.
    bool   (*_vehicleConnected)() = &LicenseManager::_anyVehicleConnected;
    /// Closes an open TAK link. Replaceable for tests.
    void   (*_disconnectTak)() = &LicenseManager::_disconnectTakNow;
    bool     _linksAllowedNoted = false;   ///< last linksAllowed() reported
    QString  _deviceId;
    QString  _key;                      ///< current (plaintext) license key, if any
    QList<LicenseRecord::PinnedKey> _pinned;   ///< RotorLab public keys; replaceable for tests
    qint64   _nowOverride = 0;          ///< UTC seconds; tests only, 0 = the clock
    qint64   _offlineUntil = 0;         ///< UTC seconds, from the valid record in use
    bool     _timeLapsed = false;       ///< gate up only because time ran out or went wrong
    bool     _legacyGate = false;       ///< gate up with only a 5.2.0 record: a signed online reply opens it
    QString  _lapsedText;
    bool     _activated = false;
    bool     _busy = false;
    QString  _statusText = QStringLiteral("Not activated");
};
