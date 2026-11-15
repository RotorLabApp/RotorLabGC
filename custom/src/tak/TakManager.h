/****************************************************************************
 *
 * RotorLab GC TAK integration - manager (enrollment, mTLS stream, publish, C2).
 *
 * One QObject, exposed to QML as `TAKManager`. Owns:
 *   - config (persisted via QSettings) and enrolled TLS material (PEM files)
 *   - OTS certificate enrollment over the Marti signClient endpoint
 *   - a persistent mutual-TLS CoT stream to the TAK server
 *   - a 1 Hz publisher turning the vehicle roster + GCS into CoT markers
 *   - the inbound "Command By ATAK" path: parse marker -> dispatch
 *
 * Design intent: a command from an ATAK marker must behave *exactly* as the
 * same action taken from the RotorLab GC controls. So dispatch calls the same
 * Vehicle guided-mode entry points the GCS UI uses (guidedModeGotoLocation,
 * pauseVehicle, guidedModeRTL, ...) rather than re-implementing flight logic
 * or adding a parallel geofence. The only ATAK-specific layer is the trust
 * boundary: which verbs are enabled, plus a master permit + arm. Fence,
 * altitude and mode enforcement are left to QGC and the vehicle, exactly as
 * they are for a human operator.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QMap>
#include <QtCore/QObject>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtCore/QVariantList>
#include <QtPositioning/QGeoCoordinate>

#include "TakCommand.h"
#include "TakDefaults.h"
#include "TakPolicy.h"
#include "fleet/GuidedDispatch.h"

class QSslSocket;
class Vehicle;
namespace CoT { struct Marker; struct Entity; }

class TakManager : public QObject
{
    Q_OBJECT
    friend class TakManagerTest;
    friend class TakEnrollWireTest;

    // --- connection / identity ---
    Q_PROPERTY(QString host            MEMBER _host            NOTIFY configChanged)
    Q_PROPERTY(int     port            MEMBER _port            NOTIFY configChanged)
    Q_PROPERTY(int     enrollPort      MEMBER _enrollPort      NOTIFY configChanged)
    Q_PROPERTY(QString username        MEMBER _username        NOTIFY configChanged)
    Q_PROPERTY(QString password        MEMBER _password        NOTIFY configChanged)
    Q_PROPERTY(QString callsignPrefix  MEMBER _callsignPrefix  NOTIFY configChanged)
    Q_PROPERTY(QString gcsUid          MEMBER _gcsUid          NOTIFY configChanged)
    Q_PROPERTY(bool    verifyPeer      MEMBER _verifyPeer      NOTIFY configChanged)
    // Server certificate the operator accepted on the TAK page (SHA-256), and
    // one waiting to be accepted. Used for enrollment and the CoT stream.
    Q_PROPERTY(QString pinnedFingerprint  READ pinnedFingerprint  NOTIFY fingerprintChanged)
    Q_PROPERTY(QString pendingFingerprint READ pendingFingerprint NOTIFY fingerprintChanged)

    // --- publish ---
    Q_PROPERTY(bool    publishEnabled  MEMBER _publishEnabled  NOTIFY configChanged)

    Q_PROPERTY(bool    publishGcsBeacon MEMBER _publishGcsBeacon NOTIFY configChanged)

    // --- GCS position source ---
    // gcsManual=false: use QGC's ground-station position (system location or an
    // NMEA GPS device configured in General settings, e.g. a USB GPS dongle).
    // gcsManual=true: broadcast the fixed lat/lon/alt below.
    Q_PROPERTY(bool    gcsManual       MEMBER _gcsManual       NOTIFY configChanged)
    Q_PROPERTY(double  gcsManualLat    MEMBER _gcsManualLat    NOTIFY configChanged)
    Q_PROPERTY(double  gcsManualLon    MEMBER _gcsManualLon    NOTIFY configChanged)
    Q_PROPERTY(double  gcsManualAlt    MEMBER _gcsManualAlt    NOTIFY configChanged)
    Q_PROPERTY(QString gcsPositionText READ gcsPositionText    NOTIFY configChanged)

    // --- fleet registry: per-MAVLink-sysid callsign / video URL / tail no. ---
    // Rows are the merge of saved entries and currently-connected vehicles.
    Q_PROPERTY(QVariantList fleetModel READ fleetModel NOTIFY fleetChanged)
    Q_PROPERTY(bool    publishDirect   MEMBER _publishDirect   NOTIFY configChanged)   ///< direct (addressed) vs broadcast
    Q_PROPERTY(QString directDest      MEMBER _directDest      NOTIFY configChanged)   ///< EUD callsigns for direct delivery
    Q_PROPERTY(bool    dronesAddressable MEMBER _dronesAddressable NOTIFY configChanged) ///< drones send-to-able (needed for send-to-drone commands)

    // --- command (C2) ---
    Q_PROPERTY(bool    commandEnabled  MEMBER _commandEnabled  NOTIFY configChanged)
    Q_PROPERTY(bool    c2Allowed       MEMBER _c2Allowed       NOTIFY configChanged)
    Q_PROPERTY(bool    c2Armed         READ c2Armed WRITE setC2Armed NOTIFY c2ArmedChanged)

    // --- per-verb enable (the trust boundary) ---
    Q_PROPERTY(bool    verbGoto        MEMBER _verbGoto        NOTIFY configChanged)
    Q_PROPERTY(bool    verbRtl         MEMBER _verbRtl         NOTIFY configChanged)
    Q_PROPERTY(bool    verbLand        MEMBER _verbLand        NOTIFY configChanged)
    Q_PROPERTY(bool    verbHold        MEMBER _verbHold        NOTIFY configChanged)
    Q_PROPERTY(bool    verbAltHold     MEMBER _verbAltHold     NOTIFY configChanged)
    Q_PROPERTY(bool    verbGuided      MEMBER _verbGuided      NOTIFY configChanged)
    Q_PROPERTY(bool    verbArm         MEMBER _verbArm         NOTIFY configChanged)
    Q_PROPERTY(bool    verbDisarm      MEMBER _verbDisarm      NOTIFY configChanged)
    Q_PROPERTY(bool    verbTakeoff     MEMBER _verbTakeoff     NOTIFY configChanged)

    // --- ATAK command senders (sender allowlist, part of the trust boundary) ---
    // UI: "Send commands only from listed senders". Settings keys stay
    // "approvedOnly" / "approvedPilots" so saved values load.
    Q_PROPERTY(bool        approvedOnly   MEMBER _approvedOnly NOTIFY configChanged)
    Q_PROPERTY(QStringList approvedPilots READ approvedPilots  NOTIFY configChanged)
    /// The list as rows {entry, kind ("uid", "callsign", "any"), value} for the TAK page.
    Q_PROPERTY(QVariantList approvedPilotRows READ approvedPilotRows NOTIFY configChanged)
    /// Non-empty while C2 is on and armed with the list off (A74).
    Q_PROPERTY(QString     openSenderWarning READ openSenderWarning NOTIFY trustStateChanged)

    // --- commanded altitude warning (informs only; 0 = off) ---
    // Feet above the launch point, the datum of the marker's ALT. A command
    // above it still goes to the aircraft; the station alert adds a warning.
    Q_PROPERTY(double  altWarnAboveFt  MEMBER _altWarnAboveFt  NOTIFY configChanged)

    // --- the identity the station's test command uses as its sender ---
    Q_PROPERTY(QString testSenderCallsign READ testSenderCallsign CONSTANT)
    Q_PROPERTY(QString testSenderUid      READ testSenderUid      CONSTANT)

    // --- SPD 1..10 -> m/s scale (grammar interpretation, not a gate) ---
    Q_PROPERTY(double  spdMin          MEMBER _spdMin          NOTIFY configChanged)
    Q_PROPERTY(double  spdMax          MEMBER _spdMax          NOTIFY configChanged)

    // --- status (read only) ---
    Q_PROPERTY(bool    connected       READ connected       NOTIFY connectedChanged)
    Q_PROPERTY(bool    enrolled        READ enrolled        NOTIFY enrolledChanged)
    Q_PROPERTY(bool    haveTruststore  READ haveTruststore  NOTIFY enrolledChanged)
    Q_PROPERTY(QString certInfo        READ certInfo        NOTIFY enrolledChanged)
    Q_PROPERTY(QString statusText      READ statusText      NOTIFY statusTextChanged)
    /// Any CoT event (other than this station's own) arrived on the current link.
    Q_PROPERTY(bool    cotReceivedSinceConnect READ cotReceivedSinceConnect NOTIFY cotReceivedChanged)

    // --- remote commands received (station alert; informs only) ---
    // Commands received from ATAK since start, newest first, for review on
    // the TAK page. bannerAlerts holds the ARM / TKOFF / GOTO entries (and any
    // with an altitude warning) the operator has not dismissed yet; the fly
    // view shows the first one as a banner. Every entry is also appended to
    // the command log at commandLogPath, which the app never deletes.
    Q_PROPERTY(QVariantList remoteCommands READ remoteCommands NOTIFY remoteCommandsChanged)
    Q_PROPERTY(QVariantList bannerAlerts   READ bannerAlerts   NOTIFY remoteCommandsChanged)
    Q_PROPERTY(QString      commandLogPath READ commandLogPath CONSTANT)
    Q_PROPERTY(bool         commandLogError READ commandLogError NOTIFY commandLogErrorChanged)

public:
    explicit TakManager(QObject *parent = nullptr);
    ~TakManager() override;

    static TakManager *instance();

    bool    c2Armed() const { return _c2Armed; }
    void    setC2Armed(bool armed);

    bool    connected() const { return _connected; }
    bool    enrolled() const;
    bool    haveTruststore() const { return !_caCertPem.isEmpty(); }
    QString certInfo() const;                ///< subject + expiry of the loaded client cert / CA
    QString statusText() const { return _statusText; }
    bool    cotReceivedSinceConnect() const { return _cotReceived; }

    /// Presence refresh period and the stale time the presence event carries.
    /// The event is sent again before it goes stale.
    static constexpr int kPresenceRefreshSec = 30;
    static constexpr int kPresenceStaleSec   = 60;

    // --- QML actions ---
    Q_INVOKABLE void save();                 ///< persist config to QSettings
    Q_INVOKABLE void load();                 ///< reload config + TLS material
    Q_INVOKABLE void enroll();               ///< OTS cert enrollment
    Q_INVOKABLE void clearCerts();           ///< forget enrolled TLS material

    QString pinnedFingerprint() const  { return _pinnedFingerprint; }
    QString pendingFingerprint() const { return _pendingFingerprint; }
    /// Accept (pin) the server certificate now shown on the page. Applies to
    /// the next enrollment or connect; when it came from enrollment, enrollment
    /// runs again now.
    Q_INVOKABLE void acceptServerFingerprint();
    Q_INVOKABLE void forgetServerFingerprint();

    /// Import a CA truststore (TAK truststore-*.p12 or a PEM bundle). The
    /// password is the p12 pass phrase (TAK default is "atakatak"); ignored
    /// for PEM. Returns true and stores ca_cert.pem on success.
    Q_INVOKABLE bool importTruststore(const QString &path, const QString &password);

    /// Import a client identity (personal key) as a p12/pfx, or a PEM that
    /// carries both a PRIVATE KEY and a CERTIFICATE. Stores the client key +
    /// cert (+ any CA chain). Returns true on success.
    Q_INVOKABLE bool importClientCert(const QString &path, const QString &password);
    Q_INVOKABLE void connectServer();        ///< open the mTLS CoT stream
    Q_INVOKABLE void disconnectServer();
    Q_INVOKABLE QString runSelfTest();       ///< offline build/parse/verb checks

    /// Capture the current live position (QGC system location / GPS) into the
    /// manual lat/lon/alt fields and switch to manual mode. This is the
    /// "use my current location" action. Returns true if a fix was available.
    Q_INVOKABLE bool captureCurrentLocation();

    /// Human-readable description of the position the GCS beacon will use.
    QString gcsPositionText() const;

    // --- fleet registry ---
    QVariantList fleetModel() const;         ///< rows: {sysid, callsign, videoUrl, tail, connected}
    /// Assign metadata to a MAVLink sysid. Empty callsign/videoUrl/tail clears that field.
    Q_INVOKABLE void setFleetEntry(int sysid, const QString &callsign,
                                   const QString &videoUrl, const QString &tail);
    Q_INVOKABLE void removeFleetEntry(int sysid);   ///< forget a saved sysid
    Q_INVOKABLE void refreshFleet();                ///< re-emit fleetModel (e.g. after a vehicle connects)

    // --- approved ATAK pilots ---
    QStringList approvedPilots() const { return _approvedPilots; }
    QString     openSenderWarning() const
    {
        return TakPolicy::openSenderWarning(_c2Allowed, _c2Armed, _approvedOnly);
    }
    Q_INVOKABLE void addApprovedPilot(const QString &callsign);   ///< unprefixed entry (matches either)
    /// Add an entry of a kind: "uid" (matches the reported device UID only) or
    /// "callsign" (matches the reported callsign only).
    Q_INVOKABLE void addApprovedPilotAs(const QString &value, const QString &kind);
    QVariantList approvedPilotRows() const;
    Q_INVOKABLE void removeApprovedPilot(const QString &callsign);

    /// Validation hook: run a marker callsign (e.g. "GOTO 1") through the REAL
    /// receive -> parse -> dispatch path, as if it had arrived over the wire.
    /// The point is placed ~80 m north of the target's current position so a
    /// GOTO visibly moves the aircraft. Returns a human-readable result.
    ///
    /// It passes the same station checks as a real marker, in the same order:
    /// "Read commands from ATAK markers", the ATAK command senders list (as the
    /// test sender below), "Command By ATAK on" (c2Allowed), "Commands to
    /// aircraft on" (c2Armed) and the verb enable.
    /// A command it cannot read is not sent; it is still listed and logged
    /// with the result "Not sent: command not understood (<reason>)".
    Q_INVOKABLE QString injectTestCommand(const QString &markerCallsign);

    QString testSenderCallsign() const { return TakPolicy::testSenderCallsign(); }
    QString testSenderUid() const      { return TakPolicy::testSenderUid(); }

    // --- remote commands received ---
    QVariantList remoteCommands() const { return _remoteCommands; }
    QVariantList bannerAlerts() const;
    Q_INVOKABLE void dismissBanner(int id);     ///< hide one banner; the entry stays in the list
    Q_INVOKABLE void clearRemoteCommands();     ///< empty the on-screen list; the command log is kept
    QString commandLogPath() const;             ///< append-only JSON-lines log of every received command
    bool    commandLogError() const { return _commandLogError; }   ///< the last log write failed

signals:
    void configChanged();
    void fleetChanged();
    void c2ArmedChanged();
    void connectedChanged();
    void enrolledChanged();
    void statusTextChanged();
    void cotReceivedChanged();
    void logMessage(const QString &line);
    void remoteCommandsChanged();
    void commandLogErrorChanged();
    void trustStateChanged();      ///< C2, uplink arm or the approved list changed
    void fingerprintChanged();
    /// Emitted after a received command has been handled (dispatched or not).
    void remoteCommandReceived(const QVariantMap &entry);

private slots:
    void _onEncrypted();
    void _onReadyRead();
    void _onDisconnected();
    void _publishTick();

private:
    void    _setStatus(const QString &text);
    void    _log(const QString &line);
    QString _certDir() const;
    void    _setPendingFingerprint(const QString &fp, bool fromEnroll);
    void    _loadCerts();
    void    _persist();                      ///< write config to QSettings (silent, no signal)

    bool    _makeCsr(const QString &cn, QByteArray &keyPemOut, QString &csrB64Out, QString &err);
    QString _findOpenssl() const;

    void    _handleMarker(const QByteArray &eventXml);
    bool    _verbEnabled(Tak::Verb v) const;
    bool    _senderApproved(const CoT::Marker &m, QString &whoOut) const;
    bool    _senderApproved(const QString &senderCallsign, const QString &senderUid,
                            QString &whoOut) const;
    QGeoCoordinate _effectiveGcsPosition() const;   ///< manual, else QGC location, else vehicle fallback

    /// The station's own TAK identity event (the GCS EUD): uid, callsign,
    /// type a-f-G-U-C, takv. With `pos` invalid no position is shared and the
    /// point carries the CoT unknown values.
    CoT::Entity _stationEntity(const QGeoCoordinate &pos, int staleSeconds,
                               const QStringList &dest) const;
    /// The position the GCS beacon sends now: the station position, else the
    /// last real position a beacon carried on this link (kept fresh with a
    /// new time and stale until a position returns), else invalid.
    QGeoCoordinate _beaconPosition() const;
    /// True when the GCS beacon (publishing on, beacon on, a real position
    /// now or earlier on this link) goes out on this link.
    bool       _beaconAnnounces() const;
    /// True when "Read commands from ATAK markers" is on and the server group
    /// does not get the beacon: the beacon is off, publishing is off, no real
    /// position has existed on this link yet, or the beacon goes only to
    /// direct recipients. The presence event is then needed so the TAK server
    /// relays CoT to the station.
    bool       _presenceNeeded() const;
    /// The presence event: the station identity with no position. Publishing
    /// off means the station shares no location. Always sent to the server
    /// group, never only to the direct recipients, so the TAK server
    /// registers the station whatever "Direct" is set to.
    QByteArray _presenceEvent() const;
    /// Send the presence event when needed. Never sends drone markers. In
    /// direct mode with the beacon on, the beacon follows in the same write
    /// so its recipients keep the real position.
    void       _announcePresence();
    /// The presence refresh (every kPresenceRefreshSec). In direct mode with
    /// the beacon on, presence is registration only: it went out once when it
    /// became needed, and the beacon to the recipients keeps the station
    /// fresh, so no periodic group presence moves them to the unknown point.
    void       _refreshPresence();
    /// Send the beacon when it announces: returns false when it does not.
    bool       _sendBeacon(const QStringList &dest);
    /// Announce at once when presence has just become needed.
    void       _presenceCheck();
    QStringList _publishDest() const;
    /// Write one event to the link (and to _sentEvents while recording).
    void       _sendEvent(const QByteArray &xml);
    /// Rx bookkeeping for one received event: any uid that is not one of this
    /// station's own (its TAK uid, or a drone uid it published on this link)
    /// counts as CoT received.
    void       _noteReceived(const QByteArray &eventXml);

    struct DispatchResult {
        bool        ok = false;
        QString     msg;
        QString     alertMsg;            ///< what the station alert says: what was sent, or why not
        bool        attempted = false;   ///< passed the station checks and went to the dispatcher
        QStringList targets;             ///< labels of the aircraft it went to
        QList<int>  sysids;              ///< MAVLink system ids of those aircraft
        bool        sent = false;        ///< a command went to at least one aircraft
    };
    DispatchResult _execute(const Tak::Command &cmd, double lat, double lon, const QString &source);
    DispatchResult _dispatchOne(Vehicle *veh, const Tak::Command &cmd, double lat, double lon);
    /// Map this path's verb enum onto the shared dispatcher's.
    static Guided::Verb _guidedVerb(Tak::Verb v);
    QList<Vehicle *> _resolveTargets(const Tak::Command &cmd) const;
    Vehicle *_vehicleById(int sysid) const;
    QString  _targetLabel(Vehicle *veh) const;

    /// One parsed command from a marker or the test button: station checks,
    /// dispatch, then the station alert. Returns the result text.
    QString _receiveCommand(const Tak::Command &cmd, double lat, double lon,
                            const QString &markerText, const QString &senderCallsign,
                            const QString &senderUid, bool test, const QByteArray &payload);

    /// Add a received command to the list and the command log (and the banner
    /// when `banner`). Called after dispatch, never before.
    void _recordRemoteCommand(const TakPolicy::RemoteCommand &c, bool banner,
                              const QJsonObject &context = QJsonObject());
    void _appendCommandLog(QJsonObject record);
    static constexpr int kMaxRemoteCommands = 200;

    // --- config ---
    QString _host;
    int     _port = 8089;
    int     _enrollPort = 8446;
    QString _username;
    QString _password;
    QString _callsignPrefix = QStringLiteral("ROTOR");
    QString _gcsUid;
    bool    _verifyPeer = TakDefaults::verifyPeer;
    QString _pinnedFingerprint;          ///< SHA-256 of the server certificate the operator accepted
    QString _pendingFingerprint;         ///< shown on the page, waiting for the operator
    bool    _pendingFromEnroll = false;  ///< accepting it re-runs enrollment
    bool    _publishEnabled = false;
    bool    _publishGcsBeacon = true;
    bool    _gcsManual = false;          ///< true = broadcast the fixed lat/lon/alt below
    double  _gcsManualLat = 0.0;
    double  _gcsManualLon = 0.0;
    double  _gcsManualAlt = 0.0;

    // Fleet registry: MAVLink sysid -> operator-assigned metadata used to enrich
    // what we publish to ATAK (callsign, a video feed URL for ATAK to play, tail).
    struct FleetEntry {
        QString callsign;
        QString videoUrl;
        QString tail;
    };
    QMap<int, FleetEntry> _fleet;
    bool    _publishDirect = false;      ///< false = broadcast to the group
    QString _directDest;                 ///< comma list of EUD callsigns (direct mode)
    bool    _dronesAddressable = true;   ///< publish drones as send-to-able contacts
    bool    _commandEnabled = false;
    bool    _c2Allowed = false;
    bool    _c2Armed = false;

    // Per-verb enable. Defaults come from TakDefaults (every verb opt in).
    bool    _verbGoto    = TakDefaults::verbGoto;
    bool    _verbRtl     = TakDefaults::verbRtl;
    bool    _verbLand    = TakDefaults::verbLand;
    bool    _verbHold    = TakDefaults::verbHold;
    bool    _verbAltHold = TakDefaults::verbAltHold;
    bool    _verbGuided  = TakDefaults::verbGuided;
    bool    _verbArm     = TakDefaults::verbArm;
    bool    _verbDisarm  = TakDefaults::verbDisarm;
    bool    _verbTakeoff = TakDefaults::verbTakeoff;

    /// Commanded altitude warning level, ft above launch; 0 = no warning.
    double  _altWarnAboveFt = TakDefaults::altWarnAboveFt;

    // Sender allowlist. On by default (TakDefaults); when on, only
    // markers placed by a listed ATAK callsign (or EUD uid) are dispatched.
    bool        _approvedOnly = TakDefaults::approvedOnly;
    QStringList _approvedPilots;

    double  _spdMin = 2.0;
    double  _spdMax = 18.0;

    // --- runtime ---
    QSslSocket *_socket = nullptr;
    QTimer      _publishTimer;
    QTimer      _presenceTimer;        ///< keeps the presence event fresh while commands are read
    QByteArray  _lastPresence;         ///< the last presence event built for the link
    bool        _presenceSent = false; ///< presence went out on the current link
    bool        _presenceWasNeeded = false; ///< _presenceNeeded() at the last check on this link
    QGeoCoordinate _lastBeaconPos;     ///< last real position a beacon carried on this link
    QSet<QString> _ownUids;            ///< uids this station sent on this link (its own, never "received")
    bool        _cotReceived = false;  ///< any CoT from the server on the current link
    bool        _recordSent = false;   ///< tests: keep every event written in _sentEvents
    QList<QByteArray> _sentEvents;
    QTimer      _autoSaveTimer;   ///< debounced persistence: any config edit is saved automatically
    QByteArray  _rxBuf;
    bool        _connected = false;
    QString     _statusText = QStringLiteral("idle");
    QVariantList _remoteCommands;      ///< newest first, capped at kMaxRemoteCommands
    int         _nextRemoteId = 1;
    QString     _lastLogHash;          ///< SHA-256 of the command log's last line
    bool        _logHashLoaded = false;
    bool        _commandLogError = false;

    QByteArray  _clientCertPem;
    QByteArray  _clientKeyPem;
    QByteArray  _caCertPem;
};
