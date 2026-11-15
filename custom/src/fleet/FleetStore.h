/****************************************************************************
 *
 * RotorLab GC Fleet Connector - persistence.
 *
 * Everything the connector remembers between runs, in one place, under the
 * QSettings group "FleetConnector".
 *
 * The station token is a bearer credential: anything holding it can report
 * telemetry as this crew. It is never logged and never placed in a URL. How
 * it is stored depends on the platform. On Windows it is sealed with DPAPI
 * (CryptProtectData, user-scoped) so a copy of the ini on another machine or
 * under another account is useless. On every other platform it is stored as
 * is (plaintext) in the settings file: Android keeps that file in app-private
 * storage, while on Linux and macOS it is protected only by the user
 * account's file permissions.
 *
 * Disconnect clears every key here. It is deliberately a local action - it
 * does not tell the Fleet Server anything, and the station stays in the
 * operator's console until they revoke it there.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

class FleetStore
{
public:
    // --- identity, from enrollment ---
    static QString baseUrl();                       ///< normalized https://host:port, no trailing slash
    static void    setBaseUrl(const QString &url);

    static QString token();                         ///< empty when not enrolled
    static void    setToken(const QString &token);

    static QString stationName();
    static void    setStationName(const QString &name);

    static int     stationId();
    static void    setStationId(int id);

    /// The server key pin from the enrollment code: lowercase hex SHA-256 of
    /// the server certificate's SubjectPublicKeyInfo. Empty when the code had
    /// none (loopback http, or a server validated by the system trust store).
    static QString serverPin();
    static void    setServerPin(const QString &pin);

    // --- caches that must survive a restart so a crew can work offline ---
    static QByteArray rosterJson();
    static void       setRosterJson(const QByteArray &json);

    static QByteArray checklistTemplatesJson();
    static void       setChecklistTemplatesJson(const QByteArray &json);

    // --- protocol state, preserved across reconnects and application restarts ---
    static bool       enqueueReportSamples(QJsonArray samples);
    static QJsonArray reportBatch(int maxSamples = 500);
    static void       acknowledgeReportSamples(int count);

    static QJsonObject flightState(int sysid);
    static void        setFlightState(int sysid, const QJsonObject &state);
    static QJsonArray  pendingFlightDetails();
    static void        enqueueFlightDetails(const QJsonObject &details);
    static void        completeFlightDetails(const QString &flightRef);

    static bool        flightRefReported(const QString &flightRef);
    static void        markFlightRefsReported(const QStringList &flightRefs);
    static void        resetStationState();

    // --- run context (who is flying), per sortie ---
    // pilot_id is deliberately NOT persisted: it must never be carried across a
    // restart or a dismissal, or a flight gets somebody else's name on it.
    static QString lastSite();
    static void    setLastSite(const QString &site);
    static QString lastFlightType();
    static void    setLastFlightType(const QString &type);
    static QString lastCrew();
    static void    setLastCrew(const QString &crew);
    static int     lastAssetId();                   ///< -1 when unset
    static void    setLastAssetId(int id);

    static QStringList recentSites();
    static void        noteSite(const QString &site);
    static QStringList recentCrews();
    static void        noteCrew(const QString &crew);

    /// Wall-clock of the last time the crew screen was shown, epoch seconds.
    /// Drives the "crossed midnight or ran 12 hours" reopen rule.
    static qint64 crewScreenShownAt();
    static void   setCrewScreenShownAt(qint64 epochSeconds);

    // --- checklist outbox: runs that could not be filed yet ---
    static QByteArray outboxJson();
    static void       setOutboxJson(const QByteArray &json);

    // --- flight logs ---
    /// The log queue, persisted for a reason the checklist outbox does not
    /// share: a dataflash log exists once. If the app dies with a file
    /// collected but not yet uploaded, and the queue only lived in memory, the
    /// card gets wiped and the only copy is gone.
    static QByteArray logQueueJson();
    static void       setLogQueueJson(const QByteArray &json);

    /// Whether to pull the aircraft's own log off it after a sortie. On by
    /// default - the log is the point - but a crew on a slow radio who pull
    /// cards by hand can turn it off.
    static bool collectLogsAutomatically();
    static void setCollectLogsAutomatically(bool on);

    /// True when a token is stored - i.e. the connector is enrolled.
    static bool enrolled();

    /// Forget everything. Local only; does not revoke the station server-side.
    static void clearAll();

private:
    /// Seal/unseal the token for storage. DPAPI on Windows; identity elsewhere,
    /// where the platform's app-private storage is the protection.
    static QByteArray _seal(const QString &plain);
    static QString    _unseal(const QByteArray &sealed);
};
