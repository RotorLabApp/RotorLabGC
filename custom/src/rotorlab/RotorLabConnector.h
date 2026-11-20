/****************************************************************************
 *
 * RotorLab GC - the RotorLab Connector.
 *
 * Files this ground station's flights, checklists and logs straight to
 * RotorLab.app over HTTPS.
 *
 * This is NOT a second route for data that a Fleet Server would otherwise
 * carry. The two connectors serve two different customers: a Fleet Server is
 * an enterprise deployment, and this is the direct customer who has no Fleet
 * Server at all. That is why exactly one is ever active, why neither forwards
 * to the other, and why there is no de-duplication between them - there is
 * never one flight travelling both paths to reconcile. Anything that tries to
 * bridge the two is solving a problem this product does not have.
 *
 * It is a FILING client, not a live picture. There is no telemetry endpoint,
 * no command channel, and nothing RotorLab returns is an instruction. During a
 * flight this connector does nothing at all.
 *
 * THE RULE THAT OUTRANKS EVERYTHING ELSE IN THIS FILE
 *
 * Nothing here may prevent a flight. Not a rejected key, not a revoked
 * credential, not an unreachable rotorlab.app, not a lapsed subscription, not
 * a pilot whose currency has run out. In every state, including the red one,
 * the GC flies, arms, commands and shows video exactly as it does with no
 * connector configured. A state machine that can reach a state where the
 * aircraft is affected is the wrong state machine.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariantMap>
#include <QtCore/QTimer>
#include <QtQmlIntegration/QtQmlIntegration>

#include "Credential.h"
#include "FlightWatcher.h"

class RotorLabClient;
class MissionLibrary;

class RotorLabConnector : public QObject
{
    Q_OBJECT

    /// Five states plus the not-configured one. Everything the screen shows is
    /// a function of the state, and the state is derived - never a flag set by
    /// hand.
    Q_PROPERTY(State   state           READ state           NOTIFY stateChanged)
    Q_PROPERTY(QString stateText       READ stateText       NOTIFY stateChanged)
    Q_PROPERTY(bool    enabled         READ enabled         WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(bool    configured      READ configured      NOTIFY stateChanged)

    Q_PROPERTY(QString baseUrl         READ baseUrl         NOTIFY identityChanged)
    Q_PROPERTY(QString keyLabel        READ keyLabel        NOTIFY identityChanged)
    Q_PROPERTY(QString keyStoreName    READ keyStoreName    CONSTANT)
    Q_PROPERTY(QString keyStorageText  READ keyStorageText  NOTIFY identityChanged)
    Q_PROPERTY(QString maskedKey       READ maskedKey       NOTIFY identityChanged)

    Q_PROPERTY(QString statusText      READ statusText      NOTIFY statusChanged)
    Q_PROPERTY(QString filingNotice    READ filingNotice    NOTIFY statusChanged)
    Q_PROPERTY(QStringList filingNotices READ filingNotices NOTIFY statusChanged)
    Q_PROPERTY(int     filingPilotNoticeCount READ filingPilotNoticeCount NOTIFY statusChanged)
    Q_PROPERTY(bool    anyArmed        READ anyArmed        NOTIFY statusChanged)
    Q_PROPERTY(QString lastError       READ lastError       NOTIFY statusChanged)
    Q_PROPERTY(bool    busy            READ busy            NOTIFY statusChanged)

    // --- behaviour ---
    Q_PROPERTY(bool fileFlightsAutomatically READ fileFlightsAutomatically
                    WRITE setFileFlightsAutomatically NOTIFY settingsChanged)
    Q_PROPERTY(bool uploadLogs         READ uploadLogs      WRITE setUploadLogs   NOTIFY settingsChanged)
    Q_PROPERTY(bool askWhoIsFlying     READ askWhoIsFlying  WRITE setAskWhoIsFlying NOTIFY settingsChanged)

    // --- compliance defaults, sent on every flight and omitted when unset ---
    Q_PROPERTY(QString jurisdiction      READ jurisdiction      WRITE setJurisdiction      NOTIFY settingsChanged)
    Q_PROPERTY(QString operatingCategory READ operatingCategory WRITE setOperatingCategory NOTIFY settingsChanged)
    Q_PROPERTY(QString visualMode        READ visualMode        WRITE setVisualMode        NOTIFY settingsChanged)
    /// "GB / Open A2 / VLOS", or empty when nothing is set. The crew screen
    /// shows this so a default chosen in January is not silently stamped onto
    /// a June sortie that was flown differently.
    Q_PROPERTY(QString filingAsText      READ filingAsText      NOTIFY settingsChanged)
    /// The categories valid for the chosen jurisdiction. Eleven frameworks,
    /// each with its own vocabulary.
    Q_PROPERTY(QStringList jurisdictions      READ jurisdictions      CONSTANT)
    Q_PROPERTY(QStringList operatingCategories READ operatingCategories NOTIFY settingsChanged)
    Q_PROPERTY(QStringList visualModes        READ visualModes        CONSTANT)

    // --- the crew screen drives these; the Fleet Connector presents the same
    //     names, which is why one screen serves both ---
    Q_PROPERTY(QVariantList pilots      READ pilots      NOTIFY rosterChanged)
    Q_PROPERTY(QVariantList assets      READ assets      NOTIFY rosterChanged)
    Q_PROPERTY(bool    rosterLoaded     READ rosterLoaded     NOTIFY rosterChanged)
    Q_PROPERTY(QString rosterSourceText READ rosterSourceText NOTIFY rosterChanged)
    Q_PROPERTY(QStringList flightTypes  READ flightTypes  CONSTANT)
    Q_PROPERTY(QStringList flightTypeLabels READ flightTypeLabels CONSTANT)
    Q_PROPERTY(QStringList promptFields READ promptFields CONSTANT)
    /// True when the verified RotorLab is 3.5.2 or later, which records a
    /// flight with no pilot named as "Pilot not named" and lists it for
    /// naming (A54). False on an older or not yet verified server.
    Q_PROPERTY(bool serverNamesPilots READ serverNamesPilots NOTIFY stateChanged)

    Q_PROPERTY(int     pilotId          READ pilotId    NOTIFY runContextChanged)
    Q_PROPERTY(QString pilotName        READ pilotName  NOTIFY runContextChanged)
    Q_PROPERTY(int     assetId          READ assetId    NOTIFY runContextChanged)
    Q_PROPERTY(QString site             READ site       NOTIFY runContextChanged)
    Q_PROPERTY(QString flightType       READ flightType NOTIFY runContextChanged)
    Q_PROPERTY(QString flightTypeOther  READ flightTypeOther NOTIFY runContextChanged)
    Q_PROPERTY(QString crew             READ crew       NOTIFY runContextChanged)
    Q_PROPERTY(bool    crewScreenPending READ crewScreenPending NOTIFY runContextChanged)
    /// True when the crew screen was opened with Change pilot. The screen then
    /// offers Cancel, which leaves the pilot as it was.
    Q_PROPERTY(bool    crewScreenOnDemand READ crewScreenOnDemand NOTIFY runContextChanged)

    // --- pilot in command (PIC design, gated on RotorLab 3.5.4 and later) ---
    /// True when the verified RotorLab takes pic_periods, crew_rows and
    /// operating_basis: GET /api/v1/sorties answered 200, or its version is
    /// at or above RotorLabPic's release constant.
    Q_PROPERTY(bool    picSupported     READ picSupported NOTIFY stateChanged)
    /// The crew screen's pilot list: the sortie's seats first (each with its
    /// duty label), then every other pilot. Nobody is filtered or refused.
    Q_PROPERTY(QVariantList crewPilots  READ crewPilots   NOTIFY sortieChanged)
    /// The sortie planned on the aircraft the crew screen asked about:
    /// "Bridge inspection, span 4, 14:00-16:00", or empty.
    Q_PROPERTY(QString sortieText       READ sortieText   NOTIFY sortieChanged)
    /// The planned remote pilot of that sortie, or -1. The crew screen
    /// preselects it; the crew can pick anyone else or skip.
    Q_PROPERTY(int     plannedPicId     READ plannedPicId NOTIFY sortieChanged)
    /// The regime's term: "Remote pilot in command" (Part 107), "Remote
    /// pilot" (GB, EU) or "Pilot".
    Q_PROPERTY(QString pilotWord        READ pilotWord    NOTIFY runContextChanged)
    /// The fly view line: "Remote pilot in command: Lee Smith (from 14:02)",
    /// or empty.
    Q_PROPERTY(QString picText          READ picText      NOTIFY runContextChanged)

    // --- checklists ---
    Q_PROPERTY(QVariantList preflightTemplates  READ preflightTemplates  NOTIFY rosterChanged)
    Q_PROPERTY(QVariantList postflightTemplates READ postflightTemplates NOTIFY rosterChanged)
    Q_PROPERTY(QString checklistOffer   READ checklistOffer  NOTIFY checklistsChanged)

    // --- missions planned in RotorLab, pulled as native .plan files ---
    Q_PROPERTY(QVariantList missions      READ missions      NOTIFY missionsChanged)
    Q_PROPERTY(bool    missionsLoaded     READ missionsLoaded NOTIFY missionsChanged)
    Q_PROPERTY(bool    missionsBusy       READ missionsBusy   NOTIFY missionsChanged)
    Q_PROPERTY(QString missionsError      READ missionsError  NOTIFY missionsChanged)
    /// The pilot the list was narrowed to when it was asked for (the crew
    /// screen's pilot), or empty for the organization's whole published list.
    Q_PROPERTY(QString missionsPilotName  READ missionsPilotName NOTIFY missionsChanged)
    /// What an empty list says: what the list is, never "assigned" (A72).
    Q_PROPERTY(QString missionsEmptyText  READ missionsEmptyText NOTIFY missionsChanged)

    // --- the queue: one line on one screen, no toast, no modal, no badge ---
    Q_PROPERTY(QString queueText        READ queueText        NOTIFY queueChanged)
    Q_PROPERTY(QString queueLastAttempt READ queueLastAttempt NOTIFY queueChanged)
    Q_PROPERTY(int     queuePending     READ queuePending     NOTIFY queueChanged)
    Q_PROPERTY(int     queueNeedsAttention READ queueNeedsAttention NOTIFY queueChanged)
    Q_PROPERTY(QVariantList queueModel  READ queueModel       NOTIFY queueChanged)

public:
    enum State {
        NotConfigured = 0,  ///< no key stored
        Verifying,          ///< Verify pressed
        Connected,          ///< GET /version returned 200
        Offline,            ///< no route: timeout, DNS failure - keeps spooling
        KeyRejected,        ///< 401 - stops, and stays stopped until a human acts
        AccountBlocked,     ///< 403 - keeps spooling, shows the server's sentence
        Unavailable,        ///< RotorLab answered 5xx - keeps spooling
    };
    Q_ENUM(State)

    explicit RotorLabConnector(QObject *parent = nullptr);
    ~RotorLabConnector() override;

    static RotorLabConnector *instance();

    State   state() const { return _state; }
    QString stateText() const;
    bool    enabled() const;
    void    setEnabled(bool on);
    bool    configured() const;

    QString baseUrl() const;
    QString keyLabel() const;
    QString keyStoreName() const;
    QString maskedKey() const;
    /// Where the Device Key is kept, when that needs saying (G82): kept for
    /// this session only, moved from the settings file, or removed from it.
    /// Empty when the key is simply saved in the store.
    QString keyStorageText() const;
    /// The text keyStorageText() shows, from its inputs (for tests).
    static QString keyStorageTextFor(Credential::FileCopy fileCopy, bool inOsStore, bool haveKey,
                                     const QString &storeName);

    QString statusText() const { return _statusText; }
    bool serverNamesPilots() const;
    /// What the operator should see about flights already filed (no pilot
    /// named, a voided re-send). Kept until dismissed, so it is still there
    /// after landing; never a gate.
    QString filingNotice() const { return _filingNotice; }
    /// The same notices one per entry, pilot notices first, each prefixed
    /// with the flight's takeoff time and aircraft.
    QStringList filingNotices() const { return _filingPilotNotices + _filingOtherNotices; }
    int  filingPilotNoticeCount() const { return int(_filingPilotNotices.size()); }
    /// True while any vehicle is armed; the station card hides, the text stays.
    bool anyArmed() const { return _anyArmed; }
    Q_INVOKABLE void dismissFilingNotice();
    /// The station card's Dismiss: clears only the notices it showed; the
    /// rest stay, on the card and in settings.
    Q_INVOKABLE void dismissFilingNotices(const QStringList &shown);
    QString lastError() const  { return _lastError; }
    bool    busy() const       { return _busy; }

    bool fileFlightsAutomatically() const;
    void setFileFlightsAutomatically(bool on);
    bool uploadLogs() const;
    void setUploadLogs(bool on);
    bool askWhoIsFlying() const;
    void setAskWhoIsFlying(bool on);

    QString jurisdiction() const;
    void    setJurisdiction(const QString &code);
    QString operatingCategory() const;
    void    setOperatingCategory(const QString &code);
    QString visualMode() const;
    void    setVisualMode(const QString &mode);
    QString filingAsText() const;
    QStringList jurisdictions() const;
    QStringList operatingCategories() const;
    QStringList visualModes() const;

    /// The compliance fields to merge into a filed flight. Only ever contains
    /// what the operator actually set - a field nobody chose is absent, because
    /// nothing validates these server-side and a wrong value reads as blank
    /// while looking like an answer.
    QJsonObject complianceFields() const;

    // --- QML actions ---

    /// Store the key and verify it. `key` is rejected before any request when
    /// it does not look like a Device Key.
    Q_INVOKABLE void setKeyAndVerify(const QString &key);

    // --- missions ---
    QVariantList missions() const;
    bool    missionsLoaded() const;
    bool    missionsBusy() const;
    QString missionsError() const;
    QString missionsPilotName() const { return _missionsPilotName; }
    QString missionsEmptyText() const;
    /// Refresh the list. Narrowed to the crew screen's pilot when one is set.
    Q_INVOKABLE void refreshMissions();
    /// Fetch one mission's `.plan`. On success the connector emits
    /// missionPlanReady() and the QML loads it into the Plan view.
    Q_INVOKABLE void loadMission(int missionId);

    /// What RotorLab says about an airframe's fitness. Advisory: shown to the
    /// crew, never used to stop anything.
    Q_INVOKABLE QVariantMap readinessFor(int assetId) const;

    /// Re-run the verify against the stored key.
    Q_INVOKABLE void verifyNow();

    /// Forget the key and the caches. Keeps the spool, and does not revoke
    /// anything in RotorLab.
    Q_INVOKABLE void disconnectFromRotorLab();

    /// True when `typed` looks like a Device Key. The field uses this so a
    /// customer who pastes their password is told immediately rather than
    /// after a round trip.
    Q_INVOKABLE static bool looksLikeDeviceKey(const QString &typed);

    // --- roster and run context ---
    QVariantList pilots() const;
    QVariantList assets() const;
    bool         rosterLoaded() const;
    QString      rosterSourceText() const;
    QStringList  flightTypes() const;
    QStringList  flightTypeLabels() const;
    QStringList  promptFields() const;

    int     pilotId() const   { return _pilotId; }
    QString pilotName() const { return _pilotName; }
    int     assetId() const   { return _assetId; }
    QString site() const;
    QString flightType() const;
    QString flightTypeOther() const;
    QString crew() const;
    bool    crewScreenPending() const { return _crewScreenPending; }
    bool    crewScreenOnDemand() const { return _crewScreenOnDemand; }

    bool         picSupported() const;
    QVariantList crewPilots() const;
    QString      sortieText() const;
    int          plannedPicId() const;
    QString      pilotWord() const;
    QString      picText() const;
    /// Read the sortie planned today on `assetId` (the crew screen's aircraft).
    /// Only against a RotorLab that serves it; otherwise the seats stay empty
    /// and the screen lists the roster as before.
    Q_INVOKABLE void loadSortie(int assetId);
    /// Why the pilot is changing, in the crew's words. Optional; taken by the
    /// next setCrew and written to the handoff log.
    Q_INVOKABLE void setHandoffReason(const QString &reason);

    /// The crew answered. `pilotId` of -1 means they skipped the question,
    /// which is a valid answer and is recorded as one.
    Q_INVOKABLE void setCrew(int pilotId, const QString &pilotName, int assetId,
                             const QString &site, const QString &flightType,
                             const QString &crew);
    /// The text for flight type "other". Remembered like the site.
    Q_INVOKABLE void setFlightTypeOther(const QString &text);
    /// Pack serials for this sortie, as typed or scanned. Held in memory like
    /// the pilot and written to the station log when set. Not sent to RotorLab
    /// until a release takes packs on the flight write (A55), and the crew
    /// screen does not offer the field until then.
    Q_INVOKABLE void setPackSerials(const QString &text);

    /// Dismissed in one action, with no confirmation. `pilot_id` is left unset
    /// - never the previous pilot.
    Q_INVOKABLE void dismissCrewScreen();
    /// Change pilot: open the crew screen now, at any time, also while
    /// flying. It is non-modal and never sits over a flight control.
    Q_INVOKABLE void openCrewScreen();
    /// Close the crew screen and change nothing: the pilot, aircraft and the
    /// rest stay as they were. Cancel, and any other way the screen closes
    /// without an answer.
    Q_INVOKABLE void closeCrewScreen();
    Q_INVOKABLE void refreshRoster();

    // --- checklists ---
    QVariantList preflightTemplates() const;
    QVariantList postflightTemplates() const;
    /// "preflight", "postflight" or empty. Set when one is worth offering; the
    /// UI shows a card the crew can open or ignore. Never a modal, never a step
    /// to pass through, never while anything is armed.
    QString checklistOffer() const { return _checklistOffer; }

    /// Mint the reference for a run, once, as the crew opens it.
    Q_INVOKABLE QString newChecklistRef(const QString &kind, int sysid) const;

    /// File a completed run. `items` is a list of {text, ok, result} maps.
    Q_INVOKABLE void fileChecklist(const QString &externalRef, int templateId,
                                   const QString &kind, const QString &aircraft,
                                   double started, bool passed,
                                   const QVariantList &items, const QString &notes);

    Q_INVOKABLE void dismissChecklistOffer();

    // --- the queue ---
    QString queueText() const;
    QString queueLastAttempt() const;
    int     queuePending() const;
    int     queueNeedsAttention() const;
    QVariantList queueModel() const;
    Q_INVOKABLE void retryQueueNow();
    Q_INVOKABLE void retryQueueItem(const QString &id);
    Q_INVOKABLE void forgetQueueItem(const QString &id);

signals:
    void stateChanged();
    void enabledChanged();
    void identityChanged();
    void statusChanged();
    void settingsChanged();
    void rosterChanged();
    void runContextChanged();
    void queueChanged();
    void missionsChanged();
    /// A downloaded `.plan` is on disk. The QML opens the Plan view and calls
    /// PlanMasterController::loadFromFile() with this path - the C++ side never
    /// reaches into the plan itself.
    void missionPlanReady(const QString &filePath, const QString &missionName);
    void checklistsChanged();
    void sortieChanged();
    void logMessage(const QString &line);

private slots:
    void _refreshTick();
    void _onFlightComplete(const FlightWatcher::Flight &flight);
    void _onArmedChanged(bool anyArmed);
    /// Maintenance the server raised when a flight filed. Advisory only.
    void _onWorkRaised(const QStringList &items);
    void _onFilingNotice(const QJsonObject &flightBody, const QStringList &pilotNotes,
                         const QStringList &otherNotes);
    void _offerPostflight();
    /// Find or pull the log for a flight. With `itemId`, it is a spool item
    /// whose log was not collected before (Try again, or its aircraft became
    /// the active one); without, a new item is made only when collection
    /// cannot happen, so the spool card says why.
    void _collectLogFor(const QString &flightRef, int sysid,
                        double endedAt, double durationS,
                        const QString &itemId = QString());
    /// The active aircraft changed: collect a waiting log for it.
    void _onActiveVehicleChanged();

private:
    friend class RotorLabSpoolTest;

    void _setState(State s);
    void _setStatus(const QString &text);
    void _setError(const QString &text);
    void _setBusy(bool on);
    void _log(const QString &line);

    void _applyCredentials();
    void _fetchUsage();

    /// Classify a reply the way every call should. Returns true when the
    /// caller may read the body.
    bool _handleCommon(const struct HttpReply &r);

    /// Build the flight body from a completed sortie plus the run context.
    /// `external_ref` is minted here, once, and never again for this flight.
    QJsonObject _flightBody(const FlightWatcher::Flight &flight,
                            const QString &externalRef) const;
    /// Globally unique, opaque, and stable across a GC restart.
    static QString _mintExternalRef();

    /// Try again on a parked flight: the stored body with the pilot,
    /// aircraft, site, type, crew and compliance set on this station now
    /// (RotorLabFiling::retryFlightBody()). Same external_ref.
    QJsonObject _retryFlightBody(const QJsonObject &stored, int sysid) const;
    /// Record on the spool card why a flight's log was not collected.
    void _logNotCollected(const QString &itemId, const QString &flightRef, int sysid,
                          double endedAt, double durationS, const QString &why);
    /// "Not collected yet: OSPREY-7 (vehicle 2) is not the active aircraft..."
    QString _makeActiveText(int sysid) const;

    RotorLabClient   *_client = nullptr;
    class RotorLabRoster    *_roster = nullptr;
    class AirframeResolver  *_airframes = nullptr;
    class FlightSpool       *_spool = nullptr;
    FlightWatcher           *_flights = nullptr;

    State   _state = NotConfigured;
    MissionLibrary *_missions = nullptr;
    QString _statusText;
    QString _serverVersion;     ///< from GET /api/v1/version; empty until verified
    QStringList _filingPilotNotices;    ///< persisted until dismissed
    QStringList _filingOtherNotices;    ///< persisted until dismissed
    QString _filingNotice;
    bool    _anyArmed = false;
    void    _publishFilingNotices(bool save);
    QString _lastError;
    /// The "not saved while an aircraft is armed" refusal, while it is shown.
    /// Cleared from the page when every aircraft has disarmed.
    QString _armedKeyRefusal;
    /// A key was just entered: the next verify asks who is flying even when a
    /// pilot and aircraft are set.
    bool    _newKey = false;
    /// A filing answered 200 while Offline: back to Connected.
    void    _onServerAnswered();
    bool    _busy = false;

    // Run context. The pilot is held in memory ONLY - never persisted, so it
    // cannot survive a restart and put last week's name on today's flight.
    int     _pilotId = -1;
    QString _pilotName;
    int     _assetId = -1;
    QStringList _packSerials;   ///< per sortie, memory only, like the pilot
    bool    _crewScreenPending = false;
    bool    _crewScreenOnDemand = false;
    /// The station asked who is flying while armed; it asks at the disarm.
    bool    _crewScreenDeferred = false;
    /// The station's own ask (after a verify, or offline with a cached
    /// roster). Deferred until disarm while anything is armed.
    void    _askWhoIsFlying();

    QString _checklistOffer;
    bool    _preflightOfferedThisSortie = false;
    /// Offer the pre-flight when it is due (A71): an aircraft connected and
    /// disarmed, a pre-flight published, not yet offered this sortie.
    void    _maybeOfferPreflight();

    /// Ask for the missions, narrowed to the crew screen's pilot when one is
    /// set, and remember which pilot that was so the screen says so.
    void    _fetchMissions();
    QString _missionsPilotName;

    // --- pilot in command ---
    /// Write a change of pilot to the handoff log, when it is one.
    void    _recordHandoff(int fromId, const QString &fromName, int toId, const QString &toName);
    /// The vehicle a change is made for: the active one, or -1.
    static int _currentSysid();
    class PicLog *_picLog = nullptr;
    QVariantList _sortieSeats;      ///< the last sortie read, for _sortieAssetId
    QVariantMap  _sortieInfo;
    int          _sortieAssetId = -1;
    QVariantList _crewSeats;        ///< the seats in force when the crew answered
    QString      _crewOperatingBasis;
    QString      _handoffReason;
    QDateTime    _picFrom;          ///< when the current pilot was named
    /// GET /api/v1/sorties answered 200 after the last verify.
    bool         _sortiesAnswered = false;
    void         _probeSorties();
    /// The most recent flight queued, so a post-flight run can be tied to it.
    QString _lastFiledFlightRef;

    QTimer  _refreshTimer;      ///< roster / aircraft / templates, 15 min
    QTimer  _postflightTimer;   ///< 30 s after the last disarm
    /// RotorLab's limit for one log file (RotorLabFiling::kMaxLogBytes, or
    /// max_file_bytes from /api/v1/usage when a release publishes it).
    qint64  _maxLogBytes = 0;
};
