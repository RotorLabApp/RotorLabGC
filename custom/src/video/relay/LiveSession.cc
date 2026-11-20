/****************************************************************************
 *
 * RotorLab GC - G109: S20 live session client.
 *
 ****************************************************************************/

#include "LiveSession.h"

#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>

#include <algorithm>

namespace {

const QString kLiveRoot = QStringLiteral("/api/gc/v1/live");
constexpr int kOpenTimeoutMs = 15000;
constexpr int kSendTimeoutMs = 10000;
constexpr int kDeleteTimeoutMs = 5000;
constexpr qint64 kRateWindowNs = 5'000'000'000LL;

} // namespace

QString LiveSession::h265Text()
{
    return tr("Live relay needs an H.264 camera on this computer.");
}

QString LiveSession::revokedText()
{
    return tr("Live relay stopped: this ground station was removed from the Fleet Server.");
}

bool LiveSession::validLiveId(const QString &id)
{
    static const QRegularExpression ok(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    return ok.match(id).hasMatch();
}

QString LiveSession::cleanFlightRef(const QString &ref)
{
    // The Fleet Server's rule for a flight ref.
    static const QRegularExpression ok(QStringLiteral("^[A-Za-z0-9_-]{8,64}$"));
    return ok.match(ref).hasMatch() ? ref : QString();
}

void LiveSession::setFlightRef(const QString &ref)
{
    const QString clean = cleanFlightRef(ref);
    if (clean == _wantRef) {
        return;
    }
    _wantRef = clean;
    _kick();
}

LiveSession::LiveSession(LiveTransport *transport, QObject *parent)
    : QObject(parent)
    , _transport(transport)
{
    _retryTimer.setSingleShot(true);
    connect(&_retryTimer, &QTimer::timeout, this, [this]() {
        if (_state == State::Paused) {
            _setState(State::Waiting);
            _kick();
        }
    });
}

LiveSession::~LiveSession()
{
    if (_transport) {
        _transport->abortAll();
    }
}

void LiveSession::_setState(State s)
{
    if (s == _state) {
        return;
    }
    _state = s;
    emit stateChanged();
}

QString LiveSession::_path(const QString &tail) const
{
    return tail.isEmpty() ? kLiveRoot + QLatin1Char('/') + _liveId
                          : kLiveRoot + QLatin1Char('/') + _liveId + tail;
}

int LiveSession::retryInSeconds() const
{
    if (_state != State::Paused) {
        return 0;
    }
    const qint64 left = _retryAtNs - relaySteadyNowNs();
    return left <= 0 ? 0 : int((left + 999'999'999LL) / 1'000'000'000LL);
}

double LiveSession::sentMbps() const
{
    if (_sentWindow.empty()) {
        return 0;
    }
    const qint64 now = relaySteadyNowNs();
    qint64 bytes = 0;
    for (const auto &e : _sentWindow) {
        if (now - e.first <= kRateWindowNs) {
            bytes += e.second;
        }
    }
    return double(bytes) * 8.0 / (double(kRateWindowNs) / 1e9) / 1e6;
}

void LiveSession::changeQuality(const QString &key)
{
    if (key == _quality) {
        return;
    }
    _quality = key;     // for the next POST /live, if this stream ever has to be opened again
    if (!_liveId.isEmpty()) {
        qCInfo(VideoRelayLog) << "live stream" << _liveId << "now at quality" << key
                              << "on the same stream; its init follows";
    }
}

void LiveSession::start()
{
    _retryTimer.stop();
    _queue.clear();
    _backlog90k = 0;
    _waitKey = true;
    _busy = false;
    _liveId.clear();
    _serverRef.clear();
    _refusedRef.reset();
    _sentInitId = 0;
    _nextSeq = 0;
    _had400 = false;
    _okSince400 = 0;
    _backoffIndex = 0;
    _stopReason.clear();
    _sentWindow.clear();
    _behindSeconds = 0;
    _setState(State::Waiting);
    emit keyframeWanted();
    emit statsChanged();
}

void LiveSession::_endWire(bool sendDelete)
{
    ++_epoch;
    _retryTimer.stop();
    const QString id = _liveId;
    if (_transport) {
        _transport->abortAll();
        if (sendDelete && !id.isEmpty()) {
            // Best effort: the server also ends a stream after 10 s without a fragment.
            _transport->deleteResource(kLiveRoot + QLatin1Char('/') + id, {}, kDeleteTimeoutMs);
        }
    }
    _queue.clear();
    _backlog90k = 0;
    _busy = false;
    _liveId.clear();
    _serverRef.clear();
    _sentInitId = 0;
    _sentWindow.clear();
}

void LiveSession::stop()
{
    if (_state == State::Idle) {
        return;
    }
    _endWire(_state != State::Revoked);
    _stopReason.clear();
    _setState(State::Idle);
    emit statsChanged();
}

void LiveSession::revoke()
{
    if (_state == State::Revoked) {
        return;
    }
    _endWire(false);
    _stopReason = revokedText();
    qCWarning(VideoRelayLog) << "live relay stopped: the station is no longer enrolled (401 or revoked)";
    _setState(State::Revoked);
    emit statsChanged();
}

void LiveSession::_stopWith(const QString &reason)
{
    _endWire(true);
    _stopReason = reason;
    qCWarning(VideoRelayLog) << "live relay stopped:" << reason;
    _setState(State::Stopped);
    emit statsChanged();
}

void LiveSession::_drop(int count)
{
    for (int i = 0; i < count && !_queue.empty(); ++i) {
        _backlog90k -= _queue.front().duration90k;
        _queue.pop_front();
        ++_droppedFragments;
        if (!_liveId.isEmpty()) {
            ++_nextSeq;     // a gap on the wire, never a repeat
        }
    }
    if (_queue.empty()) {
        _backlog90k = 0;
    }
}

void LiveSession::_dropToKeyframe()
{
    int lead = 0;
    while (lead < int(_queue.size()) && !_queue[size_t(lead)].keyframeStart) {
        ++lead;
    }
    _drop(lead);
    _waitKey = _queue.empty();
    if (_waitKey) {
        emit keyframeWanted();
    }
}

void LiveSession::_enforceBacklog()
{
    while (_backlog90k > _backlogLimit90k && !_queue.empty()) {
        int next = -1;
        for (int i = 1; i < int(_queue.size()); ++i) {
            if (_queue[size_t(i)].keyframeStart) {
                next = i;
                break;
            }
        }
        if (next < 0) {
            _drop(int(_queue.size()));
            _waitKey = true;
            emit keyframeWanted();
            break;
        }
        _drop(next);
    }
}

void LiveSession::push(const LiveFragment &fragment)
{
    switch (_state) {
    case State::Idle:
    case State::Stopped:
    case State::Revoked:
        return;
    default:
        break;
    }
    if (_waitKey) {
        if (!fragment.keyframeStart) {
            ++_droppedFragments;
            if (!_liveId.isEmpty()) {
                ++_nextSeq;
            }
            return;
        }
        _waitKey = false;
    }
    _queue.push_back(fragment);
    _backlog90k += fragment.duration90k;
    _enforceBacklog();
    _kick();
}

void LiveSession::_kick()
{
    if (_busy) {
        return;
    }
    if (_state != State::Waiting && _state != State::Live) {
        return;
    }
    if (!_liveId.isEmpty() && _wantRef != _serverRef && _refusedRef != _wantRef) {
        // Name (or un-name) the flight on the open stream before the next fragment.
        _sendPatch();
        return;
    }
    if (_queue.empty()) {
        return;
    }
    if (_liveId.isEmpty()) {
        _open();
    } else if (_queue.front().initId != _sentInitId) {
        _sendInit();
    } else {
        _sendFragment();
    }
}

void LiveSession::_open()
{
    if (!_queue.front().keyframeStart) {
        _dropToKeyframe();
        if (_queue.empty()) {
            return;
        }
    }
    if (!_transport || !_transport->prepare()) {
        // Not enrolled any more, or no usable address: wait and look again.
        _pause(-1);
        return;
    }
    const LiveFormat &fmt = _queue.front().format;
    if (_flightRef) {
        _wantRef = cleanFlightRef(_flightRef());
    }
    const QString flightRef = _wantRef;
    QJsonObject body{
        {QStringLiteral("camera"), _camera},
        {QStringLiteral("quality"), _quality},
        {QStringLiteral("codec"), fmt.codec},
        {QStringLiteral("width"), fmt.width},
        {QStringLiteral("height"), fmt.height},
        // The server takes 1 to 240. A fragment of one sample (keyframes only)
        // gives no rate of its own: 1 is what such a stream shows.
        {QStringLiteral("fps"), std::clamp(fmt.fps, 1, 240)},
        {QStringLiteral("flight_ref"), flightRef},
    };
    _busy = true;
    _setState(State::Opening);
    const quint64 epoch = _epoch;
    _transport->postJson(kLiveRoot, body, [this, epoch, flightRef](const HttpReply &r) {
        if (epoch != _epoch) {
            return;
        }
        _busy = false;
        if (!_handle(r, Call::Open)) {
            return;
        }
        const QString id = r.body.value(QStringLiteral("live_id")).toString();
        if (!validLiveId(id)) {
            qCWarning(VideoRelayLog) << "POST /live answered without a usable live_id";
            _pause(-1);
            return;
        }
        _liveId = id;
        _serverRef = flightRef;
        _nextSeq = 0;
        _sentInitId = 0;
        qCInfo(VideoRelayLog) << "live session opened" << id << "quality" << _quality;
        _setState(State::Live);
        _kick();
    }, kOpenTimeoutMs);
}

void LiveSession::_sendInit()
{
    const LiveFragment &f = _queue.front();
    const quint64 initId = f.initId;
    _busy = true;
    const quint64 epoch = _epoch;
    _transport->putBytes(_path(QStringLiteral("/init")), f.init, [this, epoch, initId](const HttpReply &r) {
        if (epoch != _epoch) {
            return;
        }
        _busy = false;
        if (!_handle(r, Call::Init)) {
            return;
        }
        _sentInitId = initId;
        _kick();
    }, kSendTimeoutMs);
}

void LiveSession::_sendFragment()
{
    LiveFragment f = std::move(_queue.front());
    _queue.pop_front();
    _backlog90k = std::max<qint64>(0, _backlog90k - f.duration90k);
    const quint64 seq = _nextSeq++;
    const qint64 bytes = f.bytes.size();
    const qint64 receiveNs = f.lastReceiveSteadyNs;
    _busy = true;
    const quint64 epoch = _epoch;
    const qint64 duration90k = f.duration90k;
    _transport->postBytes(_path(QStringLiteral("/frag?seq=%1").arg(seq)), f.bytes,
                          [this, epoch, bytes, receiveNs](const HttpReply &r) {
        if (epoch != _epoch) {
            return;
        }
        _busy = false;
        if (!_handle(r, Call::Fragment)) {
            return;
        }
        const qint64 now = relaySteadyNowNs();
        _sentWindow.emplace_back(now, bytes);
        while (!_sentWindow.empty() && now - _sentWindow.front().first > kRateWindowNs) {
            _sentWindow.pop_front();
        }
        if (receiveNs > 0 && now > receiveNs) {
            _behindSeconds = double(now - receiveNs) / 1e9;
        }
        ++_sentFragments;
        if (_had400 && ++_okSince400 >= 10) {
            _had400 = false;
        }
        emit statsChanged();
        _kick();
    }, kSendTimeoutMs);
    // After the call, so a listener that stops the session (the bitrate
    // guard pausing) aborts a request that exists rather than racing it.
    emit fragmentSent(bytes, duration90k);
}

void LiveSession::_sendPatch()
{
    const QString ref = _wantRef;
    _busy = true;
    const quint64 epoch = _epoch;
    _transport->patchJson(_path(), QJsonObject{{QStringLiteral("flight_ref"), ref}},
                          [this, epoch, ref](const HttpReply &r) {
        if (epoch != _epoch) {
            return;
        }
        _busy = false;
        if (r.netOk && r.status == 400) {
            // The server refused this value: say so once, never loop on it, keep streaming.
            qCWarning(VideoRelayLog) << "PATCH flight_ref refused:" << r.error.left(300);
            _refusedRef = ref;
            _kick();
            return;
        }
        if (!_handle(r, Call::Patch)) {
            return;
        }
        _serverRef = ref;
        qCInfo(VideoRelayLog) << (ref.isEmpty() ? "live stream: flight ended" : "live stream named its flight")
                              << _liveId;
        _kick();
    }, kSendTimeoutMs);
}

bool LiveSession::_handle(const HttpReply &r, Call call)
{
    if (r.pinRefused) {
        _stopWith(tr("Live relay stopped: the server's key does not match this station's enrollment. Nothing was sent."));
        return false;
    }
    if (!r.netOk || r.status >= 500) {
        _pause(r.retryAfter);
        return false;
    }
    if (r.status >= 200 && r.status < 300) {
        _backoffIndex = 0;
        return true;
    }
    switch (r.status) {
    case 401:
        revoke();
        return false;
    case 409:
        if (call == Call::Open) {
            // The server has no Fleet license right now: retry later, quietly.
            _pause(r.retryAfter);
        } else {
            qCInfo(VideoRelayLog) << "409 from the Fleet Server: starting a new live session";
            _newSession();
        }
        return false;
    case 404:
        if (call != Call::Open) {
            qCInfo(VideoRelayLog) << "the Fleet Server ended the live stream: starting a new live session";
            _newSession();
            return false;
        }
        break;
    case 415:
        _stopWith(h265Text());
        return false;
    case 413:
        if (call != Call::Fragment) {
            break;
        }
        qCWarning(VideoRelayLog) << "413 for a live fragment (GC should never send one over 4 MB); dropped";
        _kick();
        return false;
    case 400:
        qCWarning(VideoRelayLog) << "400 from the Fleet Server:" << r.error.left(300);
        if (!_had400) {
            _had400 = true;
            _okSince400 = 0;
            _newSession();
        } else {
            _stopWith(tr("Live relay stopped: %1").arg(r.error.isEmpty() ? tr("the Fleet Server refused the video.") : r.error.left(300)));
        }
        return false;
    default:
        break;
    }
    _stopWith(tr("Live relay stopped: the Fleet Server answered %1.").arg(r.status));
    return false;
}

void LiveSession::_newSession()
{
    ++_epoch;
    if (_transport) {
        _transport->abortAll();
    }
    _busy = false;
    _liveId.clear();
    _serverRef.clear();
    _sentInitId = 0;
    _nextSeq = 0;
    _dropToKeyframe();
    _setState(State::Waiting);
    emit statsChanged();
    _kick();
}

void LiveSession::_pause(int retryAfterSeconds)
{
    ++_epoch;
    if (_transport) {
        _transport->abortAll();
    }
    _busy = false;
    _liveId.clear();
    _serverRef.clear();
    _sentInitId = 0;
    _nextSeq = 0;
    int delayMs = 0;
    if (retryAfterSeconds > 0) {
        delayMs = std::min(retryAfterSeconds, 300) * 1000;
    } else if (!_backoffMs.isEmpty()) {
        delayMs = _backoffMs.at(std::min(_backoffIndex, int(_backoffMs.size()) - 1));
    }
    ++_backoffIndex;
    _dropToKeyframe();
    _retryAtNs = relaySteadyNowNs() + qint64(delayMs) * 1'000'000;
    qCInfo(VideoRelayLog) << "Fleet unreachable for live relay; retrying in" << delayMs << "ms";
    _setState(State::Paused);
    _retryTimer.start(delayMs);
    emit statsChanged();
}
