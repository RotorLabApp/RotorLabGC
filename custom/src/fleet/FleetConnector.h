/****************************************************************************
 *
 * RotorLab GC Fleet Connector.
 *
 * One QObject, exposed to QML as `FleetConnector`. Talks to exactly one thing
 * - the RotorLab unified server - over HTTPS. It reports aircraft telemetry,
 * flight details, checklists, and logs, and fetches the station roster. This
 * connector never holds a RotorLab.app credential.
 *
 * THE RULE THAT OUTRANKS EVERYTHING ELSE HERE:
 *
 *   Nothing in this class may prevent a flight.
 *
 * Not a rejected enrollment code, not a revoked token, not an unreachable
 * server, not a TLS validation failure. In every state - including REVOKED -
 * the GC flies, arms, commands and shows video
 * exactly as it does with no Fleet Server configured. If this connector ever
 * refuses to arm, blocks a launch, greys out a control or holds up a
 * pre-flight because the Fleet Server said something, that is a defect.
 *
 * The GC flies aircraft. The connector reports about them. When the two are in
 * tension, the aircraft wins.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QVariantList>

#include "FleetClient.h"

class FleetTelemetry;
class FleetChecklists;
class FleetLogs;

class FleetConnector : public QObject
{
    Q_OBJECT

public:
    /// The connector is always in exactly one of these. The state is derived,
    /// never a flag set by hand.
    enum State {
        NotConnected,   ///< no token stored - the enrollment form
        Enrolling,      ///< enroll pressed, waiting
        FirstSync,      ///< enrolled; downloading roster/templates
        Connected,      ///< reporting
        Unreachable,    ///< network failure, server error, or missing server license
        Revoked,        ///< a call returned 401 - stopped until a human acts
    };
    Q_ENUM(State)

private:
    Q_PROPERTY(State   state           READ state           NOTIFY stateChanged)
    Q_PROPERTY(QString stateText       READ stateText       NOTIFY stateChanged)
    Q_PROPERTY(bool    enrolled        READ enrolled        NOTIFY stateChanged)

    // --- identity / connection, for the main screen ---
    Q_PROPERTY(QString baseUrl         READ baseUrl         NOTIFY identityChanged)
    Q_PROPERTY(QString stationName     READ stationName     NOTIFY identityChanged)
    Q_PROPERTY(QString machineName     READ machineName     CONSTANT)
    Q_PROPERTY(int     stationId       READ stationId       NOTIFY identityChanged)
    Q_PROPERTY(QString certInfo        READ certInfo        NOTIFY identityChanged)
    Q_PROPERTY(QString lastReportText  READ lastReportText  NOTIFY tickChanged)

    // --- what the operator is told ---
    Q_PROPERTY(QString statusText      READ statusText      NOTIFY statusChanged)
    Q_PROPERTY(QString lastError       READ lastError       NOTIFY statusChanged)
    Q_PROPERTY(bool    busy            READ busy            NOTIFY statusChanged)

    /// True while the RotorLab Connector is this station's route instead.
    /// The settings list uses it to decide which connector row to show.
    Q_PROPERTY(bool    suspended       READ suspended       NOTIFY stateChanged)

    // --- what the last report did, for the developer build and the log ---
    Q_PROPERTY(int     reportedVehicles READ reportedVehicles NOTIFY tickChanged)

    // --- roster + run context, for the crew screen ---
    Q_PROPERTY(QVariantList pilots      READ pilots         NOTIFY rosterChanged)
    Q_PROPERTY(QVariantList assets      READ assets         NOTIFY rosterChanged)
    Q_PROPERTY(QString rosterSourceText READ rosterSourceText NOTIFY rosterChanged)
    Q_PROPERTY(bool    rosterLoaded     READ rosterLoaded   NOTIFY rosterChanged)
    Q_PROPERTY(QStringList flightTypes  READ flightTypes    CONSTANT)
    Q_PROPERTY(QStringList promptFields READ promptFields   CONSTANT)
    Q_PROPERTY(QStringList recentSites  READ recentSites    NOTIFY runContextChanged)
    Q_PROPERTY(QStringList recentCrews  READ recentCrews    NOTIFY runContextChanged)

    // Current run context, for Panel 3 and the crew screen's prefill.
    Q_PROPERTY(int     pilotId          READ pilotId        NOTIFY runContextChanged)
    Q_PROPERTY(QString pilotName        READ pilotName      NOTIFY runContextChanged)
    Q_PROPERTY(int     assetId          READ assetId        NOTIFY runContextChanged)
    Q_PROPERTY(QString site             READ site           NOTIFY runContextChanged)
    Q_PROPERTY(QString flightType       READ flightType     NOTIFY runContextChanged)
    Q_PROPERTY(QString flightTypeOther  READ flightTypeOther NOTIFY runContextChanged)
    Q_PROPERTY(QString crew             READ crew           NOTIFY runContextChanged)
    Q_PROPERTY(bool    crewScreenPending READ crewScreenPending NOTIFY runContextChanged)

    // --- checklists ---
    Q_PROPERTY(QVariantList preflightTemplates  READ preflightTemplates  NOTIFY checklistsChanged)
    Q_PROPERTY(QVariantList postflightTemplates READ postflightTemplates NOTIFY checklistsChanged)
    Q_PROPERTY(QString checklistOffer   READ checklistOffer  NOTIFY checklistsChanged)
    Q_PROPERTY(int     outboxCount      READ outboxCount     NOTIFY checklistsChanged)

    // --- flight logs ---
    Q_PROPERTY(QVariantList flightLogs  READ flightLogs      NOTIFY flightLogsChanged)
    Q_PROPERTY(int  flightLogsWaiting   READ flightLogsWaiting NOTIFY flightLogsChanged)
    Q_PROPERTY(bool collectLogsAutomatically READ collectLogsAutomatically
                    WRITE setCollectLogsAutomatically NOTIFY flightLogsChanged)

public:
    explicit FleetConnector(QObject *parent = nullptr);
    ~FleetConnector() override;

    static FleetConnector *instance();

    State   state() const { return _state; }
    QString stateText() const;
    bool    enrolled() const;

    QString baseUrl() const;
    QString stationName() const;
    /// This machine's hostname - the default for the station name field, and
    /// what an admin uses to tell two identical laptops apart.
    QString machineName() const;
    int     stationId() const;
    QString certInfo() const;
    QString lastReportText() const;

    QString statusText() const { return _statusText; }
    QString lastError() const { return _lastError; }
    bool    busy() const { return _busy; }

    int         reportedVehicles() const { return _reportedVehicles; }

    /// Flight metadata captured for the next sortie.
    void setRunContext(const QJsonObject &ctx);
    QJsonObject runContext() const { return _runContext; }

    // --- roster ---
    QVariantList pilots() const { return _pilots; }
    QVariantList assets() const { return _assets; }
    QString      rosterSourceText() const;
    bool         rosterLoaded() const { return _rosterLoaded; }
    QStringList flightTypes() const
    {
        return {QStringLiteral("commercial"), QStringLiteral("training"),
                QStringLiteral("proficiency"), QStringLiteral("maintenance"),
                QStringLiteral("survey"), QStringLiteral("inspection"),
                QStringLiteral("public_safety"), QStringLiteral("test"),
                QStringLiteral("recreational"), QStringLiteral("other")};
    }
    QStringList promptFields() const
    {
        return {QStringLiteral("pilot_id"), QStringLiteral("asset_id"),
                QStringLiteral("site"), QStringLiteral("flight_type"),
                QStringLiteral("flight_type_other"), QStringLiteral("crew")};
    }
    QStringList  recentSites() const;
    QStringList  recentCrews() const;

    // --- current run context ---
    int     pilotId() const;
    QString pilotName() const;
    int     assetId() const;
    QString site() const;
    QString flightType() const;
    QString flightTypeOther() const;
    QString crew() const;

    /// True when the crew screen is due to open: after first sync, on every
    /// launch while connected, and once the session crosses local midnight or
    /// runs past twelve hours. Nothing else opens it.
    bool crewScreenPending() const { return _crewScreenPending; }

    /// Record who is flying. `pilotId` < 0 means "not set" - which is what a
    /// dismissal leaves behind, deliberately.
    Q_INVOKABLE void setCrew(int pilotId, const QString &pilotName, int assetId,
                             const QString &site, const QString &flightType,
                             const QString &crew);
    Q_INVOKABLE void setFlightTypeOther(const QString &value);

    /// The crew closed the screen without choosing. The pilot becomes unset,
    /// never "whatever it was last time".
    Q_INVOKABLE void dismissCrewScreen();

    /// Open it on demand, from Panel 3 or a "change pilot" action.
    Q_INVOKABLE void openCrewScreen();

    /// Fetch the roster now (also done at first sync and on the refresh timer).
    Q_INVOKABLE void refreshRoster();

    // --- checklists ---
    QVariantList preflightTemplates() const;
    QVariantList postflightTemplates() const;
    /// "preflight", "postflight" or empty. Set when a checklist is worth
    /// offering; the UI shows a card the crew can open or ignore. Never a
    /// modal, never a step to pass through.
    QString checklistOffer() const { return _checklistOffer; }
    int     outboxCount() const;

    /// Mint the reference for a run, once, as the crew opens it.
    Q_INVOKABLE QString newChecklistRef(const QString &kind, int sysid) const;
    Q_INVOKABLE QString flightRefForChecklist(const QString &kind, int sysid);

    /// File a completed run. `items` is a list of {text, ok} maps.
    Q_INVOKABLE void fileChecklist(const QString &externalRef, const QString &flightRef,
                                   int templateId, const QString &kind,
                                   const QString &name, const QString &aircraft,
                                   double started, bool passed,
                                   const QVariantList &items, const QString &notes);

    /// Dismiss the current offer without running it.
    Q_INVOKABLE void dismissChecklistOffer();

    // --- flight logs ---
    /// One row per flight the server has no log for: ref, callsign, when,
    /// duration, state, detail, progress, retryable, file.
    QVariantList flightLogs() const;
    int  flightLogsWaiting() const;
    bool collectLogsAutomatically() const;
    void setCollectLogsAutomatically(bool on);

    /// Hand a file to a flight the connector could not find one for.
    Q_INVOKABLE void attachFlightLog(const QString &externalRef, const QString &filePath);
    /// Stop the collection or upload in progress.
    Q_INVOKABLE void cancelFlightLogTransfer();
    /// Try a parked entry again.
    Q_INVOKABLE void retryFlightLog(const QString &externalRef);
    /// Stop asking about one flight.
    Q_INVOKABLE void forgetFlightLog(const QString &externalRef);

    // --- enrollment code (RotorLab Server 2.0 contract) ---

    /// What the operator typed in the code field, taken apart.
    ///
    /// The code is `secrets.token_urlsafe(12)`: 16 characters of A-Z, a-z,
    /// 0-9, `-` and `_`, compared by the server exactly as sent. Nothing here
    /// changes its case or removes a character; only whitespace around a paste
    /// is dropped, since a code never contains any.
    ///
    /// Three forms are accepted:
    ///  - `<code>`
    ///  - `<code>.<pin>`: everything before the first dot is the code
    ///  - `rotorlab-server://enroll?a=<host>:<port>&p=<pin>&c=<code>`
    struct EnrollCode {
        QString code;       ///< sent as-is in the request body
        QString pin;        ///< lowercase hex SPKI SHA-256, or empty
        QString address;    ///< from the link form only, as https://host:port
        bool    hasPin = false;     ///< a pin part was present, valid or not
    };
    static EnrollCode parseEnrollCode(const QString &typed);

    /// True when `typed` is a 16-character code, with or without a valid pin.
    /// Drives the Enroll button.
    static bool enrollCodeReady(const QString &typed);
    Q_INVOKABLE bool codeReady(const QString &typed) const { return enrollCodeReady(typed); }
    static bool isValidCode(const QString &code);
    static bool isValidPin(const QString &pin);

    /// The JSON body of POST /api/gc/v1/enroll.
    static QJsonObject enrollBody(const QString &code, const QString &name);

    /// The operator-facing sentence for a failed enroll reply.
    static QString enrollErrorText(const FleetReply &r, const QString &base);

    /// G40: the Fleet Server is this station's route to ATAK while enrolled
    /// and not suspended (the TAK page is hidden then). Returns +1 when it has
    /// just become the route, -1 when it has just stopped being it, else 0.
    static int takRouteEdge(bool wasRoute, bool enrolled, bool suspended);

    // --- QML actions ---

    /// Redeem an enrollment code. `address` is what the operator typed;
    /// `code` is the code field, in any form parseEnrollCode() accepts.
    Q_INVOKABLE void enroll(const QString &address, const QString &code,
                            const QString &name);

    /// Forget everything locally. Does not revoke the station server-side.
    Q_INVOKABLE void disconnectFromServer();

    /// Stand down because the RotorLab Connector is now this station's route.
    ///
    /// Deliberately NOT a disconnect: the enrollment and local caches stay in
    /// place, so turning the RotorLab Connector off again brings this one back
    /// with nothing to re-enter. It stops reporting and stops polling.
    void setSuspendedByRotorLab(bool suspended);
    bool suspended() const { return _suspended; }

    /// Ask the server for the roster now, rather than waiting for the refresh.
    Q_INVOKABLE void refreshNow();

signals:
    void stateChanged();
    void identityChanged();
    void statusChanged();
    void tickChanged();
    void rosterChanged();
    void runContextChanged();
    void checklistsChanged();
    void flightLogsChanged();
    void logMessage(const QString &line);

    /// First sync finished. Phase 4 opens the crew screen on this.
    void firstSyncComplete();

private slots:
    void _rosterTick();
    void _telemetryTick();

private:
    void _setState(State s);
    void _setStatus(const QString &text);
    void _setError(const QString &text);
    void _setBusy(bool on);
    void _log(const QString &line);
    void _flushFlightDetails();

    void _loadFromStore();
    void _startFirstSync();
    void _fetchRoster(bool duringFirstSync = false);

    /// Common handling for every authenticated reply: 401 stops reporting,
    /// 409 is quiet, invalid credentials stop reporting. Returns true when
    /// the caller should treat the reply as usable.
    bool _handleCommon(const FleetReply &r);

    void _scheduleReconnect();
    /// G40: hand the ATAK route over when the Fleet Server becomes it.
    void _takHandOff();
    void _clearReconnect();

    FleetClient   *_client = nullptr;
    FleetTelemetry  *_telemetry = nullptr;
    FleetChecklists *_checklists = nullptr;
    FleetLogs       *_logs = nullptr;
    bool             _suspended = false;   ///< the RotorLab Connector is active instead
    bool             _takRoute = false;    ///< G40: the Fleet Server is the route to ATAK
    State          _state = NotConnected;

    QTimer _rosterTimer;        ///< periodic roster refresh
    QTimer _reconnectTimer;     ///< backoff 1/2/5/15/30 s
    QTimer _telemetryTimer;     ///< periodic telemetry reporting

    void _applyRoster(const QJsonObject &body, bool persist);
    void _evaluateCrewScreenDue();

    QJsonObject  _runContext;
    int          _reportedVehicles = 0;
    bool         _telemetryInFlight = false;

    QVariantList _pilots;
    QVariantList _assets;
    QString      _rosterSource;
    bool         _rosterLoaded = false;
    QString      _pilotName;
    bool         _crewScreenPending = false;

    // Checklist offers are driven by the arm state: the pre-flight before the
    // first arm of a sortie, the post-flight once it has been disarmed and
    // stayed that way. Never on app start, never mid-flight.
    void    _watchArmState();
    QString _checklistOffer;
    bool    _anyArmed = false;
    bool    _preflightOfferedThisSortie = false;
    QTimer  _postflightTimer;   ///< 30 s after the last disarm

    int       _backoffIndex = 0;
    QDateTime _lastReportAt;
    bool      _busy = false;
    QString   _statusText;
    QString   _lastError;
};
