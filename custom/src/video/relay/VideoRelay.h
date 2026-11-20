/****************************************************************************
 *
 * RotorLab GC - G109 live relay: the QML front (context property VideoRelay).
 *
 * Station mode (QSettings group VideoRelay, key stationMode):
 *   view     the Fly view only; nothing is sent (default)
 *   forward  relay to the Fleet Server, and the Fly view hides its video
 *            (hideFlyViewVideo; the receiver keeps decoding, G109 never
 *            touches it)
 *   both     relay and the Fly view
 *
 * Forward quality (key quality): low, medium, full; the default is the lowest
 * level offered. Low and Medium are offered only when EncoderProbe opened an
 * encoder; Full only for an H.264 camera.
 *
 * Output-bitrate guard (BitrateGuard.h): over the level's cap
 * (Low 0.8, Medium 2.5 Mb/s; none for Full) the relay first sends keyframes
 * only, then pauses with "Video forwarding paused: the picture needs more
 * bandwidth than Low allows", and comes back when the picture settles. It
 * filters only the relay's own tap consumer: local recording is never touched.
 *
 * A quality change while relaying (the operator's, or the CPU cap's step
 * down) keeps the open stream: the pipeline changes level and the new init
 * goes out on the same live_id (LiveSession::changeQuality), so a viewer
 * keeps watching. Every (re)start gives the bitrate guard its start grace
 * (BitrateGuard::kStartGrace90k): a new level, the relay on again, and every
 * time the relay encoder opens again or the source starts a new generation
 * (RelayPipeline::encoderRestarted, BitrateGuard::restartGrace).
 *
 * The armed flight's ref is named on the open stream (PATCH /live/<id>) at
 * arm, and cleared after landing; the stream is never restarted for it.
 *
 * Relaying happens only while the Fleet Connector is enrolled, not suspended
 * (the RotorLab Connector is not the route) and not revoked. Revocation (a 401,
 * or the connector going to Revoked or NotConnected) stops it at once and it
 * stays stopped until a new enrollment. Nothing here can stop, delay or gate a
 * flight, the display, or local recording.
 *
 ****************************************************************************/

#pragma once

#include "BitrateGuard.h"
#include "EncoderProbe.h"
#include "RelayTypes.h"

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtCore/QVariantList>

class FleetLiveTransport;
class LiveSession;
class RelayPipeline;

class VideoRelay : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(bool fleetReady READ fleetReady NOTIFY changed)
    Q_PROPERTY(QString stationMode READ stationMode WRITE setStationMode NOTIFY changed)
    Q_PROPERTY(QString quality READ quality WRITE setQuality NOTIFY changed)
    Q_PROPERTY(QString effectiveQuality READ effectiveQuality NOTIFY changed)
    Q_PROPERTY(QVariantList qualityOptions READ qualityOptions NOTIFY changed)
    Q_PROPERTY(QString qualityReason READ qualityReason NOTIFY changed)
    Q_PROPERTY(bool relayAllowed READ relayAllowed NOTIFY changed)
    Q_PROPERTY(bool hideFlyViewVideo READ hideFlyViewVideo NOTIFY changed)
    Q_PROPERTY(bool forwarding READ forwarding NOTIFY statusChanged)
    Q_PROPERTY(bool statusVisible READ statusVisible NOTIFY changed)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(QString chipText READ chipText NOTIFY statusChanged)
    Q_PROPERTY(QString encoderText READ encoderText NOTIFY changed)
    Q_PROPERTY(QString enrollmentNote READ enrollmentNote NOTIFY changed)
    Q_PROPERTY(bool canRetry READ canRetry NOTIFY statusChanged)

public:
    explicit VideoRelay(QObject *parent = nullptr);
    ~VideoRelay() override;

    static VideoRelay *instance();

    /// The video code wires the receiver's tap (VideoReceiver::packetTap(); null for
    /// GStreamer and Qt Multimedia). `owner` is the object that owns the tap:
    /// when it is destroyed the relay lets go of the tap first.
    void setPacketTap(VideoPacketTap *tap, QObject *owner);

    bool available() const;
    bool fleetReady() const { return _fleetReady; }
    QString stationMode() const { return _mode; }
    void setStationMode(const QString &mode);
    QString quality() const { return relayQualityKey(_quality); }
    void setQuality(const QString &key);
    QString effectiveQuality() const { return relayQualityKey(_effective); }
    QVariantList qualityOptions() const;
    QString qualityReason() const;
    bool relayAllowed() const { return _offer.relayAllowed; }
    bool hideFlyViewVideo() const;
    bool forwarding() const;
    bool statusVisible() const { return _mode != QLatin1String("view"); }
    QString statusText() const { return _statusText; }
    QString chipText() const { return _chipText; }
    QString encoderText() const;
    QString enrollmentNote() const;
    bool canRetry() const;

    /// The operator's "Try again" after a stop (cap, 415, 400, pin).
    Q_INVOKABLE void retry();

    // --- logic, static for the tests ---------------------------------------
    /// Fleet Connector state (FleetConnector::State as int) to "may relay".
    static bool fleetAllows(int state, bool enrolled, bool suspended);
    /// True when that state means the station's token is gone or refused.
    static bool fleetRevoked(int state);
    /// True when a transition from `previous` to `state` is a new working
    /// enrollment, which ends a revocation stop.
    static bool clearsRevocation(int previous, int state);
    /// What the CPU cap does: Medium steps down to Low once; Low (or a second
    /// overload) stops forwarding. Never a step up.
    enum class CapAction { StepToLow, Stop };
    static CapAction capAction(RelayQuality effective, bool alreadySteppedDown);
    static QString capStoppedText();
    static QString capSteppedText();
    static QString notEnrolledText();
    static QString noReceiverText();
    static QString forwardingText(const QString &server, const QString &level, double mbps, double behind);
    static QString pausedText(int seconds);
    static QString waitingFleetText();
    static QString waitingVideoText();
    /// The bitrate guard (BitrateGuard.h), with the level's name ("Low", "Medium").
    static QString guardPausedText(const QString &level);
    static QString guardKeyframesText(const QString &level);
    /// The guard's cap for a level, bits per second: the level's own target; 0 (none) for Full.
    static qint64 guardCap(RelayQuality q);
    /// What the pipeline lets through in a guard state (RelayPipeline::Pass as int).
    static int guardPass(BitrateGuard::State s);

signals:
    void changed();
    void statusChanged();

private:
    void _loadSettings();
    void _findTap();
    void _onFleetState();
    void _onProbe(const EncoderProbeReport &report);
    void _updateOffer();
    void _evaluate();
    void _startRelay();
    void _stopRelay(bool revoked);
    void _switchQuality();
    void _onOverloaded();
    void _onSessionState();
    void _updateStatus();
    void _applyGuard();
    QString _flightRef() const;
    void _setFleetReady(bool ready);

    QThread _thread;
    RelayPipeline *_pipeline = nullptr;
    FleetLiveTransport *_transport = nullptr;
    LiveSession *_session = nullptr;

    VideoPacketTap *_tap = nullptr;
    QPointer<QObject> _tapOwner;

    QString _mode = QStringLiteral("view");
    RelayQuality _quality = RelayQuality::Low;
    bool _qualitySet = false;
    RelayQuality _effective = RelayQuality::Low;
    QString _camera = QStringLiteral("main");

    EncoderProbeReport _probe;
    bool _probed = false;
    VideoTapCodec _codec = VideoTapCodec::H264;
    bool _codecKnown = false;
    QualityOffer _offer;

    bool _fleetReady = false;
    int _lastFleetState = 0;        ///< FleetConnector::NotConnected
    bool _revoked = false;          ///< sticky until a new enrollment
    bool _running = false;
    bool _steppedDown = false;
    bool _capStopped = false;
    bool _videoSeen = false;
    QString _encoderFail;
    BitrateGuard _guard;

    QTimer _statusTimer;
    QTimer _tapTimer;
    QString _statusText;
    QString _chipText;
};
