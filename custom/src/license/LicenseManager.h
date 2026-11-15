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
 *   - stores a local activation record so the app opens offline once
 *     activated (online re-verify on each launch; offline = grace)
 *   - re-checks the license while running: every 24 hours, and when the
 *     network comes back. A re-check while running only notes the result. It
 *     never closes the session, never shows the license gate and never clears
 *     the stored activation, so it cannot interrupt a flight. The check at the
 *     next start applies the result.
 *
 * The license key is sent only in the JSON request body over verified HTTPS.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QObject>
#include <QtCore/QString>

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

signals:
    void activatedChanged();
    void busyChanged();
    void statusChanged();
    void logMessage(const QString &line);

private:
    friend class LicenseRecheckTest;

    enum class Op { Activate, Deactivate, Verify };

    void    _startBackgroundChecks();
    void    _backgroundRecheck();
    bool    _networkReachable(bool online);   ///< true when it asked for a re-check
    void    _noteInSession(const QString &state, const QString &error);
    static bool _anyVehicleConnected();

    void    _setActivated(bool on);
    void    _setBusy(bool on);
    void    _setStatus(const QString &text);

    QString _ensureDeviceId();          ///< load or create the persisted device id
    void    _loadRecord();              ///< read stored activation record
    void    _saveRecord(const QString &key);
    void    _clearRecord();

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
    QString  _deviceId;
    QString  _key;                      ///< current (plaintext) license key, if any
    bool     _activated = false;
    bool     _busy = false;
    QString  _statusText = QStringLiteral("Not activated");
};
