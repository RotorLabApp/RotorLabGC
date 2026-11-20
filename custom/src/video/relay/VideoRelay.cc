/****************************************************************************
 *
 * RotorLab GC - G109 live relay: QML front.
 *
 ****************************************************************************/

#include "VideoRelay.h"

#include "FleetLiveTransport.h"
#include "LiveSession.h"
#include "RelayPipeline.h"

#include "VideoTapRegistry.h"

#include "fleet/FleetConnector.h"
#include "fleet/FleetStore.h"

#include "MultiVehicleManager.h"
#include "QmlObjectListModel.h"
#include "Vehicle.h"

#include <QtCore/QApplicationStatic>
#include <QtCore/QSettings>
#include <QtCore/QVariantMap>

Q_APPLICATION_STATIC(VideoRelay, _videoRelayInstance);

namespace {
const QString kGroup = QStringLiteral("VideoRelay");
const QString kModeKey = QStringLiteral("stationMode");
const QString kQualityKey = QStringLiteral("quality");
const QString kCameraKey = QStringLiteral("camera");

bool validMode(const QString &m)
{
    return m == QLatin1String("view") || m == QLatin1String("forward") || m == QLatin1String("both");
}

QString levelName(RelayQuality q)
{
    switch (q) {
    case RelayQuality::Low:    return VideoRelay::tr("Low");
    case RelayQuality::Medium: return VideoRelay::tr("Medium");
    case RelayQuality::Full:   break;
    }
    return VideoRelay::tr("Full");
}
} // namespace

VideoRelay *VideoRelay::instance()
{
    return _videoRelayInstance();
}

VideoRelay::VideoRelay(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<LiveFragment>("LiveFragment");
    qRegisterMetaType<EncoderChoice>("EncoderChoice");
    qRegisterMetaType<EncoderProbeReport>("EncoderProbeReport");

    _loadSettings();
    _guard.setStartGrace90k(BitrateGuard::kStartGrace90k);

    _transport = new FleetLiveTransport(this);
    _session = new LiveSession(_transport, this);
    _session->setCamera(_camera);
    _session->setFlightRefProvider([this]() { return _flightRef(); });

    _thread.setObjectName(QStringLiteral("VideoRelay"));
    _pipeline = new RelayPipeline();
    _pipeline->moveToThread(&_thread);
    connect(&_thread, &QThread::finished, _pipeline, &QObject::deleteLater);
    _thread.start(QThread::LowPriority);

    connect(_pipeline, &RelayPipeline::probeDone, this, &VideoRelay::_onProbe, Qt::QueuedConnection);
    connect(_pipeline, &RelayPipeline::fragmentReady, this, [this](const LiveFragment &f) {
        _videoSeen = true;
        if (_running && !_guard.paused()) {
            _session->push(f);
        }
    }, Qt::QueuedConnection);
    connect(_pipeline, &RelayPipeline::contentMeasured, this, [this](qint64 bytes, qint64 duration90k) {
        if (_running && _guard.onContent(bytes, duration90k)) {
            _applyGuard();
        }
    }, Qt::QueuedConnection);
    connect(_pipeline, &RelayPipeline::encoderRestarted, this, [this]() {
        // A new encoder or a new source: its rate control starts over. The
        // start grace again, judged from here (BitrateGuard::restartGrace).
        if (_running) {
            _guard.restartGrace();
        }
    }, Qt::QueuedConnection);
    connect(_session, &LiveSession::fragmentSent, this, [this](qint64 bytes, qint64 duration90k) {
        if (_running && _guard.onSent(bytes, duration90k)) {
            _applyGuard();
        }
    }, Qt::QueuedConnection);
    connect(_pipeline, &RelayPipeline::codecSeen, this, [this](int codec) {
        _videoSeen = true;
        _codec = VideoTapCodec(codec);
        _codecKnown = true;
        _updateOffer();
    }, Qt::QueuedConnection);
    connect(_pipeline, &RelayPipeline::streamStarted, this, [this](int codec, bool known) {
        if (known) {
            _codec = VideoTapCodec(codec);
            _codecKnown = true;
            _updateOffer();
        }
        // The stream is back: a new live session at its first keyframe.
        if (_running && !_guard.paused() && _session->state() == LiveSession::State::Idle) {
            _session->start();
        }
        _updateStatus();
    }, Qt::QueuedConnection);
    connect(_pipeline, &RelayPipeline::streamStopped, this, [this]() {
        _videoSeen = false;
        if (_running) {
            _session->stop();   // DELETE, best effort
        }
        _updateStatus();
    }, Qt::QueuedConnection);
    connect(_pipeline, &RelayPipeline::encoderOverloaded, this, &VideoRelay::_onOverloaded, Qt::QueuedConnection);
    connect(_pipeline, &RelayPipeline::encoderFailed, this, [this](const QString &detail) {
        _encoderFail = detail;
        qCWarning(VideoRelayLog) << "relay encoder produced nothing:" << detail;
        _onOverloaded();
    }, Qt::QueuedConnection);

    connect(_session, &LiveSession::keyframeWanted, this, [this]() {
        RelayPipeline *p = _pipeline;
        QMetaObject::invokeMethod(p, [p]() { p->requestKeyframe(); }, Qt::QueuedConnection);
    });
    connect(_session, &LiveSession::stateChanged, this, &VideoRelay::_onSessionState);
    connect(_session, &LiveSession::statsChanged, this, &VideoRelay::_updateStatus);

    if (FleetConnector *fc = FleetConnector::instance()) {
        connect(fc, &FleetConnector::stateChanged, this, &VideoRelay::_onFleetState);
    }

    _statusTimer.setInterval(1000);
    connect(&_statusTimer, &QTimer::timeout, this, [this]() {
        // At arm the stream is named with the flight's ref; after landing, "".
        _session->setFlightRef(_flightRef());
        _updateStatus();
    });

    // The main stream's tap, as CustomPlugin::createVideoReceiver records it
    // (VideoTapRegistry). Looked up again every 2 s: receivers come later than
    // this object, and a receiver can be replaced.
    _tapTimer.setInterval(2000);
    connect(&_tapTimer, &QTimer::timeout, this, &VideoRelay::_findTap);
    _tapTimer.start();
    QTimer::singleShot(0, this, &VideoRelay::_findTap);

    _updateOffer();
    _onFleetState();

    if (EncoderProbe::buildHasEncoders()) {
        RelayPipeline *p = _pipeline;
        QMetaObject::invokeMethod(p, [p]() { p->probe(); }, Qt::QueuedConnection);
    } else {
        _probed = true;
    }
}

VideoRelay::~VideoRelay()
{
    _stopRelay(false);
    RelayPipeline *p = _pipeline;
    QMetaObject::invokeMethod(p, [p]() { p->stop(); }, Qt::BlockingQueuedConnection);
    _thread.quit();
    _thread.wait();
}

void VideoRelay::_loadSettings()
{
    QSettings s;
    s.beginGroup(kGroup);
    const QString mode = s.value(kModeKey, QStringLiteral("view")).toString();
    _mode = validMode(mode) ? mode : QStringLiteral("view");
    RelayQuality q;
    _qualitySet = relayQualityFromKey(s.value(kQualityKey).toString(), q);
    _quality = _qualitySet ? q : RelayQuality::Low;
    const QString cam = s.value(kCameraKey, QStringLiteral("main")).toString().left(32);
    _camera = cam.isEmpty() ? QStringLiteral("main") : cam;
    s.endGroup();
}

void VideoRelay::setPacketTap(VideoPacketTap *tap, QObject *owner)
{
    if (_tap == tap) {
        return;
    }
    if (_running) {
        _stopRelay(false);
    }
    _tap = tap;
    _tapOwner = owner;
    if (owner) {
        connect(owner, &QObject::destroyed, this, [this, tap]() {
            if (_tap == tap) {
                RelayPipeline *p = _pipeline;
                // Let go of the tap before it is freed.
                QMetaObject::invokeMethod(p, [p]() { p->stop(); }, Qt::BlockingQueuedConnection);
                _tap = nullptr;
                _running = false;
                _session->stop();
                emit changed();
                _updateStatus();
            }
        }, Qt::DirectConnection);
    }
    emit changed();
    _evaluate();
}

void VideoRelay::_findTap()
{
    VideoPacketTap *tap = VideoTapRegistry::tap();
    if (tap != _tap) {
        setPacketTap(tap, nullptr);
    }
}

bool VideoRelay::available() const
{
    // The relay exists only on GC's own FFmpeg receiver (Windows and Android).
#ifdef QGC_FFMPEG_STREAMING
    return true;
#else
    return false;
#endif
}

// ---------------------------------------------------------------- Fleet state

bool VideoRelay::fleetAllows(int state, bool enrolled, bool suspended)
{
    return enrolled && !suspended && state != FleetConnector::NotConnected
           && state != FleetConnector::Enrolling && state != FleetConnector::Revoked;
}

bool VideoRelay::fleetRevoked(int state)
{
    return state == FleetConnector::Revoked || state == FleetConnector::NotConnected;
}

bool VideoRelay::clearsRevocation(int previous, int state)
{
    const bool working = state == FleetConnector::Connected || state == FleetConnector::FirstSync;
    const bool wasEnrolling = previous == FleetConnector::NotConnected || previous == FleetConnector::Enrolling
                              || previous == FleetConnector::Revoked;
    return working && wasEnrolling;
}

void VideoRelay::_setFleetReady(bool ready)
{
    if (ready != _fleetReady) {
        _fleetReady = ready;
        emit changed();
    }
}

void VideoRelay::_onFleetState()
{
    FleetConnector *fc = FleetConnector::instance();
    const int state = fc ? int(fc->state()) : int(FleetConnector::NotConnected);
    const bool enrolled = fc && fc->enrolled();
    const bool suspended = fc && fc->suspended();

    if (fleetRevoked(state)) {
        if (_running || _session->state() != LiveSession::State::Idle) {
            // Abort what is on the wire and drop the backlog; no DELETE.
            _stopRelay(true);
        }
        _revoked = (state == FleetConnector::Revoked);
    } else if (clearsRevocation(_lastFleetState, state)) {
        // A new enrollment after a revocation. A 401 seen only by the relay,
        // with the connector still Connected, stays until such a transition.
        if (_revoked || _session->state() == LiveSession::State::Revoked) {
            _revoked = false;
            _session->stop();
        }
    }
    _lastFleetState = state;
    _setFleetReady(fleetAllows(state, enrolled, suspended) && !_revoked);
    _evaluate();
}

// ---------------------------------------------------------------- offer

void VideoRelay::_onProbe(const EncoderProbeReport &report)
{
    _probe = report;
    _probed = true;
    if (!report.chosen.ok) {
        qCInfo(VideoRelayLog) << "no H.264 encoder opened; Low and Medium are not offered";
    }
    _updateOffer();
}

void VideoRelay::_updateOffer()
{
    _offer = QualityOffer::compute(_codec, _codecKnown, _probe.chosen.ok);
    RelayQuality want = _qualitySet ? _quality : _offer.lowest();
    if (!_offer.offers(want)) {
        want = _offer.lowest();
    }
    if (_steppedDown && want == RelayQuality::Medium) {
        want = RelayQuality::Low;
    }
    const bool qualityChanged = want != _effective;
    _effective = want;
    emit changed();
    if (qualityChanged && _running) {
        // A new picture on the same stream: new init, same live_id.
        _switchQuality();
    }
    _evaluate();
}

void VideoRelay::_switchQuality()
{
    _guard.setCap(guardCap(_effective));    // a new level: its cap, the start grace again
    _session->changeQuality(relayQualityKey(_effective));
    RelayPipeline *p = _pipeline;
    const int q = int(_effective);
    const EncoderChoice choice = _probe.chosen;
    QMetaObject::invokeMethod(p, [p, q, choice]() { p->setQuality(q, choice); }, Qt::QueuedConnection);
    if (_session->state() == LiveSession::State::Idle) {
        _session->start();      // the guard had paused (and ended) the stream
    }
    qCInfo(VideoRelayLog) << "live relay quality" << relayQualityKey(_effective) << "on the same stream";
    emit changed();
    _updateStatus();
}

QVariantList VideoRelay::qualityOptions() const
{
    QVariantList out;
    for (RelayQuality q : _offer.levels) {
        out << QVariantMap{{QStringLiteral("key"), relayQualityKey(q)},
                           {QStringLiteral("text"), QualityOffer::levelText(q)}};
    }
    return out;
}

QString VideoRelay::qualityReason() const
{
    if (!_probed && !_offer.offers(RelayQuality::Low)) {
        return tr("Checking for a video encoder...");
    }
    return _offer.reason;
}

QString VideoRelay::encoderText() const
{
    return _probe.chosen.ok ? _probe.chosen.label() : QString();
}

QString VideoRelay::enrollmentNote() const
{
    if (_revoked) {
        return LiveSession::revokedText();
    }
    return _fleetReady ? QString() : notEnrolledText();
}

// ---------------------------------------------------------------- settings

void VideoRelay::setStationMode(const QString &mode)
{
    if (!validMode(mode) || mode == _mode) {
        return;
    }
    _mode = mode;
    QSettings s;
    s.setValue(kGroup + QLatin1Char('/') + kModeKey, mode);
    _capStopped = false;
    _steppedDown = false;
    emit changed();
    _updateOffer();
}

void VideoRelay::setQuality(const QString &key)
{
    RelayQuality q;
    if (!relayQualityFromKey(key, q)) {
        return;
    }
    _quality = q;
    _qualitySet = true;
    QSettings s;
    s.setValue(kGroup + QLatin1Char('/') + kQualityKey, key);
    // The operator's choice clears the CPU cap's step-down and stop.
    _steppedDown = false;
    _capStopped = false;
    _encoderFail.clear();
    if (_session->state() == LiveSession::State::Stopped) {
        _session->stop();
    }
    _updateOffer();
}

bool VideoRelay::hideFlyViewVideo() const
{
    // Only when forwarding is actually in effect: never hide the picture for a
    // relay that cannot run.
    return _mode == QLatin1String("forward") && available() && _tap && _fleetReady && _offer.relayAllowed;
}

bool VideoRelay::forwarding() const
{
    return _running && _session->state() == LiveSession::State::Live && _session->sentFragments() > 0;
}

bool VideoRelay::canRetry() const
{
    return _capStopped || _session->state() == LiveSession::State::Stopped;
}

void VideoRelay::retry()
{
    _capStopped = false;
    _steppedDown = false;
    _encoderFail.clear();
    if (_session->state() == LiveSession::State::Stopped) {
        _session->stop();
    }
    _updateOffer();
}

// ---------------------------------------------------------------- run

void VideoRelay::_evaluate()
{
    const bool want = statusVisible() && available() && _tap && _fleetReady && _offer.relayAllowed
                      && _offer.offers(_effective) && !_capStopped
                      && _session->state() != LiveSession::State::Stopped
                      && _session->state() != LiveSession::State::Revoked;
    if (want && !_running) {
        _startRelay();
    } else if (!want && _running) {
        _stopRelay(false);
    }
    _updateStatus();
}

void VideoRelay::_startRelay()
{
    if (!_tap) {
        return;
    }
    _running = true;
    _guard.setCap(guardCap(_effective));
    _session->setQualityKey(relayQualityKey(_effective));
    _session->setFlightRef(_flightRef());
    _session->start();
    RelayPipeline *p = _pipeline;
    VideoPacketTap *tap = _tap;
    const int q = int(_effective);
    const EncoderChoice choice = _probe.chosen;
    QMetaObject::invokeMethod(p, [p, tap, q, choice]() { p->start(tap, q, choice); }, Qt::QueuedConnection);
    _statusTimer.start();
    qCInfo(VideoRelayLog) << "live relay on, quality" << relayQualityKey(_effective) << "mode" << _mode;
    emit changed();
}

void VideoRelay::_stopRelay(bool revoked)
{
    RelayPipeline *p = _pipeline;
    QMetaObject::invokeMethod(p, [p]() { p->stop(); }, Qt::QueuedConnection);
    if (revoked) {
        _session->revoke();
    } else if (_session->state() != LiveSession::State::Stopped
               && _session->state() != LiveSession::State::Revoked) {
        _session->stop();
    }
    if (_running) {
        qCInfo(VideoRelayLog) << "live relay off";
    }
    _running = false;
    _guard.reset();
    _statusTimer.stop();
    emit changed();
    _updateStatus();
}

void VideoRelay::_onOverloaded()
{
    if (!_running) {
        return;
    }
    if (capAction(_effective, _steppedDown) == CapAction::StepToLow && _encoderFail.isEmpty()) {
        // Once, and never back up by itself.
        _steppedDown = true;
        qCWarning(VideoRelayLog) << "live relay stepped down from Medium to Low (CPU cap)";
        _updateOffer();
        return;
    }
    qCWarning(VideoRelayLog) << "live relay stopped: this computer cannot encode fast enough";
    _capStopped = true;
    _stopRelay(false);
}

void VideoRelay::_onSessionState()
{
    const LiveSession::State s = _session->state();
    if (s == LiveSession::State::Revoked) {
        // A 401 on a relay call: the same as the connector going to Revoked.
        _revoked = true;
        _setFleetReady(false);
        if (_running) {
            RelayPipeline *p = _pipeline;
            QMetaObject::invokeMethod(p, [p]() { p->stop(); }, Qt::QueuedConnection);
            _running = false;
            _statusTimer.stop();
        }
    } else if (s == LiveSession::State::Stopped && _running) {
        RelayPipeline *p = _pipeline;
        QMetaObject::invokeMethod(p, [p]() { p->stop(); }, Qt::QueuedConnection);
        _running = false;
        _statusTimer.stop();
    }
    emit changed();
    _updateStatus();
}

qint64 VideoRelay::guardCap(RelayQuality q)
{
    // Full is the camera's own stream: the design gives it no cap.
    return q == RelayQuality::Full ? 0 : relayQualityTarget(q).bitRate;
}

int VideoRelay::guardPass(BitrateGuard::State s)
{
    switch (s) {
    case BitrateGuard::State::KeyframesOnly: return int(RelayPipeline::Pass::KeyframesOnly);
    case BitrateGuard::State::Paused:        return int(RelayPipeline::Pass::Nothing);
    case BitrateGuard::State::Normal:        break;
    }
    return int(RelayPipeline::Pass::All);
}

void VideoRelay::_applyGuard()
{
    const BitrateGuard::State s = _guard.state();
    RelayPipeline *p = _pipeline;
    const int pass = guardPass(s);
    QMetaObject::invokeMethod(p, [p, pass]() { p->setPass(pass); }, Qt::QueuedConnection);
    const QString level = relayQualityKey(_effective);
    switch (s) {
    case BitrateGuard::State::KeyframesOnly:
        qCWarning(VideoRelayLog) << "bitrate guard: over the" << level << "cap at"
                                 << qRound64(_guard.lastSentBps() / 1000) << "kb/s; sending keyframes only";
        break;
    case BitrateGuard::State::Paused:
        qCWarning(VideoRelayLog) << "bitrate guard: keyframes alone are over the" << level << "cap at"
                                 << qRound64(_guard.lastSentBps() / 1000) << "kb/s; forwarding paused";
        // End the stream (DELETE, best effort); a new one starts when the picture settles.
        _session->stop();
        break;
    case BitrateGuard::State::Normal:
        qCInfo(VideoRelayLog) << "bitrate guard: the picture settled at"
                              << qRound64(_guard.lastContentBps() / 1000) << "kb/s; forwarding everything again";
        if (_running && _session->state() == LiveSession::State::Idle) {
            _session->start();
        }
        break;
    }
    _updateStatus();
}

QString VideoRelay::_flightRef() const
{
    // The armed vehicle's flight ref, when exactly one vehicle is armed and has one.
    QString ref;
    int armed = 0;
    MultiVehicleManager *mvm = MultiVehicleManager::instance();
    if (QmlObjectListModel *list = mvm ? mvm->vehicles() : nullptr) {
        for (int i = 0; i < list->count(); ++i) {
            const Vehicle *v = qobject_cast<Vehicle *>((*list)[i]);
            if (v && v->armed()) {
                ++armed;
                ref = FleetStore::flightState(v->id()).value(QStringLiteral("ref")).toString();
            }
        }
    }
    return armed == 1 ? ref : QString();
}

// ---------------------------------------------------------------- status

VideoRelay::CapAction VideoRelay::capAction(RelayQuality effective, bool alreadySteppedDown)
{
    return (effective == RelayQuality::Medium && !alreadySteppedDown) ? CapAction::StepToLow : CapAction::Stop;
}

QString VideoRelay::capStoppedText()
{
    return tr("Live relay stopped: this computer cannot encode fast enough. Choose Full or turn relay off.");
}

QString VideoRelay::capSteppedText()
{
    return tr("Stepped down to Low: this computer could not encode Medium fast enough.");
}

QString VideoRelay::notEnrolledText()
{
    return tr("Live relay goes to the Fleet Server this station is enrolled with.");
}

QString VideoRelay::noReceiverText()
{
    return tr("Live relay needs this version's built-in video receiver, which this build does not include.");
}

QString VideoRelay::forwardingText(const QString &server, const QString &level, double mbps, double behind)
{
    return tr("Forwarding to %1 at %2, %3 Mb/s sent, %4 s behind")
        .arg(server, level)
        .arg(mbps, 0, 'f', 1)
        .arg(behind, 0, 'f', 1);
}

QString VideoRelay::pausedText(int seconds)
{
    return tr("Paused: Fleet unreachable, retrying in %1 s").arg(seconds);
}

QString VideoRelay::waitingFleetText()
{
    return tr("Waiting for Fleet");
}

QString VideoRelay::waitingVideoText()
{
    return tr("Waiting for video");
}

QString VideoRelay::guardPausedText(const QString &level)
{
    return tr("Video forwarding paused: the picture needs more bandwidth than %1 allows").arg(level);
}

QString VideoRelay::guardKeyframesText(const QString &level)
{
    return tr("Sending keyframes only: the picture needs more bandwidth than %1 allows").arg(level);
}

void VideoRelay::_updateStatus()
{
    QString text;
    QString chip;
    const LiveSession::State s = _session->state();
    if (!statusVisible()) {
        // View only: nothing to say.
    } else if (!available()) {
        text = noReceiverText();
    } else if (_revoked || s == LiveSession::State::Revoked) {
        text = LiveSession::revokedText();
        chip = tr("Relay stopped");
    } else if (!_fleetReady) {
        text = notEnrolledText();
    } else if (!_offer.relayAllowed) {
        text = _offer.reason;
        chip = tr("Relay off");
    } else if (_capStopped) {
        text = capStoppedText();
        chip = tr("Relay stopped");
    } else if (s == LiveSession::State::Stopped) {
        text = _session->stopReason();
        chip = tr("Relay stopped");
    } else if (_running && _guard.paused()) {
        text = guardPausedText(levelName(_effective));
        chip = tr("Relay paused");
    } else if (s == LiveSession::State::Paused) {
        text = pausedText(_session->retryInSeconds());
        chip = tr("Relay paused");
    } else if (forwarding()) {
        text = forwardingText(_session->serverLabel(), levelName(_effective),
                              _session->sentMbps(), _session->behindSeconds());
        if (_steppedDown) {
            text += QStringLiteral(" (") + capSteppedText() + QLatin1Char(')');
        }
        if (_guard.keyframesOnly()) {
            text += QStringLiteral(" (") + guardKeyframesText(levelName(_effective)) + QLatin1Char(')');
            chip = tr("LIVE %1, keyframes only").arg(levelName(_effective));
        } else {
            chip = tr("LIVE %1").arg(levelName(_effective));
        }
    } else if (!_videoSeen) {
        text = waitingVideoText();
        chip = tr("Relay waiting");
    } else {
        text = waitingFleetText();
        chip = tr("Relay waiting");
    }
    if (text != _statusText || chip != _chipText) {
        _statusText = text;
        _chipText = chip;
        emit statusChanged();
    }
}
