/****************************************************************************
 *
 * RotorLab GC - G109: the S20 client (the Fleet Server's live video API).
 *
 *   POST   /api/gc/v1/live                      -> {"live_id"}
 *   PUT    /api/gc/v1/live/<id>/init            init segment, before the first
 *                                               fragment and after every change
 *   POST   /api/gc/v1/live/<id>/frag?seq=N      one fragment, seq from 0
 *   PATCH  /api/gc/v1/live/<id>                 {"flight_ref"}: names the flight
 *                                               at arm, "" after landing, on the
 *                                               same stream
 *   DELETE /api/gc/v1/live/<id>                 on stop, best effort
 *
 * The flight ref goes in POST /live when it is known then; when it changes
 * while a stream is open, a PATCH goes out next, in the same one-at-a-time
 * order (a PATCH refused with 400 is logged and not sent again for that
 * value; 404 starts a new session, whose POST carries the ref).
 *
 * A quality change on an open stream keeps it (changeQuality): the same
 * live_id and seq, and the new level's first fragment brings its own init,
 * which goes out as PUT /live/<id>/init again before it (the contract: init
 * "before the first fragment and after every change", a codec or size change
 * included). A viewer showing the stream keeps it; a new live_id would end
 * the viewer's stream ("The live video has ended").
 *
 * One request at a time, in order, so nothing can reorder on the wire. A
 * session starts at a fragment that begins with a keyframe. The backlog
 * waiting to be sent is bounded at 3 s of media: past that, fragments are
 * dropped up to the next one that starts with a keyframe, and seq keeps
 * counting over the dropped ones (a gap is allowed, a repeat is not).
 *
 * Answers:
 *   no answer, 5xx  pause, back off 1, 2, 5, 15, 30 s (or Retry-After), then a
 *                   new session (new live_id, seq from 0, init again)
 *   409             on a fragment or init: new session at once; on POST /live
 *                   (no Fleet license): back off as above
 *   404             on a fragment or init (the server ended the stream): new session
 *   401             revoked: abort what is on the wire, drop the backlog, send
 *                   nothing more (no DELETE: the token no longer works)
 *   415             stop: live relay needs H.264
 *   413             log (a GC bug), drop that fragment, continue
 *   400             log the server's sentence, new session once, then stop
 *   pin mismatch    stop: nothing was sent
 *
 ****************************************************************************/

#pragma once

#include "LiveTransport.h"
#include "RelayTypes.h"

#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QTimer>

#include <deque>
#include <functional>
#include <optional>

class LiveSession : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Idle,       ///< not relaying
        Waiting,    ///< relaying, waiting for a fragment that starts with a keyframe
        Opening,    ///< POST /live on the wire
        Live,       ///< sending fragments
        Paused,     ///< Fleet unreachable; retrying after a delay
        Stopped,    ///< stopped with a reason; needs the operator
        Revoked,    ///< 401: stopped until a new enrollment
    };
    Q_ENUM(State)

    explicit LiveSession(LiveTransport *transport, QObject *parent = nullptr);
    ~LiveSession() override;

    void setCamera(const QString &camera) { _camera = camera; }
    void setQualityKey(const QString &key) { _quality = key; }
    QString qualityKey() const { return _quality; }
    /// The relay changed level while relaying: keep the open stream (same
    /// live_id, seq counting on); the new level's fragments carry a new init,
    /// sent again on this stream before them. No DELETE, no new POST /live.
    void changeQuality(const QString &key);
    void setFlightRefProvider(std::function<QString()> provider) { _flightRef = std::move(provider); }
    /// The flight to name on the stream: the armed flight's ref, or empty once
    /// it has landed. Sent with POST /live, or as a PATCH on an open stream.
    void setFlightRef(const QString &ref);
    /// What the server was last told for this stream (empty when none is open).
    QString serverFlightRef() const { return _serverRef; }
    /// A flight ref as the server takes it, `[A-Za-z0-9_-]{8,64}`; anything else is "".
    static QString cleanFlightRef(const QString &ref);
    /// For tests.
    void setBackoffScheduleMs(const QList<int> &ms) { _backoffMs = ms; }
    void setBacklogLimit90k(qint64 limit) { _backlogLimit90k = limit; }

    void start();
    /// End the stream: DELETE best effort, then Idle.
    void stop();
    /// 401 or the Fleet Connector going to Revoked or NotConnected: abort,
    /// drop the backlog, no DELETE.
    void revoke();

    void push(const LiveFragment &fragment);

    State state() const { return _state; }
    QString stopReason() const { return _stopReason; }
    QString liveId() const { return _liveId; }
    int retryInSeconds() const;
    double sentMbps() const;
    double behindSeconds() const { return _behindSeconds; }
    quint64 sentFragments() const { return _sentFragments; }
    quint64 droppedFragments() const { return _droppedFragments; }
    int backlogFragments() const { return int(_queue.size()); }
    qint64 backlog90k() const { return _backlog90k; }
    QString serverLabel() const { return _transport ? _transport->serverLabel() : QString(); }

    static QString h265Text();
    static QString revokedText();
    static bool validLiveId(const QString &id);

signals:
    void stateChanged();
    void statsChanged();
    /// The next fragment sent must start with a keyframe (new session or a
    /// backlog drop). The encoder path forces one; Full waits for the camera's.
    void keyframeWanted();
    /// A fragment went on the wire: its size and media duration (the bitrate
    /// guard's measure of what this station sends).
    void fragmentSent(qint64 bytes, qint64 duration90k);

private:
    enum class Call { Open, Init, Fragment, Patch };

    void _setState(State s);
    void _kick();
    void _open();
    void _sendInit();
    void _sendFragment();
    void _sendPatch();
    bool _handle(const HttpReply &r, Call call);
    void _newSession();
    void _pause(int retryAfterSeconds);
    void _stopWith(const QString &reason);
    void _endWire(bool sendDelete);
    void _drop(int count);
    void _dropToKeyframe();
    void _enforceBacklog();
    QString _path(const QString &tail = QString()) const;

    LiveTransport *_transport = nullptr;
    State _state = State::Idle;
    QString _stopReason;
    QString _camera = QStringLiteral("main");
    QString _quality = QStringLiteral("full");
    std::function<QString()> _flightRef;
    QString _wantRef;               ///< the flight to name
    QString _serverRef;             ///< what this stream's server was told
    /// A value the server refused with 400. Optional, not a QString with ""
    /// for "none": "" (the flight has landed) is a value PATCH sends too.
    std::optional<QString> _refusedRef;

    std::deque<LiveFragment> _queue;
    qint64 _backlog90k = 0;
    qint64 _backlogLimit90k = VideoRelayLimits::kBacklog90k;
    bool _waitKey = true;
    bool _busy = false;
    quint64 _epoch = 0;             ///< bumped on every abort; late handlers check it

    QString _liveId;
    quint64 _nextSeq = 0;
    quint64 _sentInitId = 0;
    bool _had400 = false;
    int _okSince400 = 0;

    QList<int> _backoffMs = {1000, 2000, 5000, 15000, 30000};
    int _backoffIndex = 0;
    QTimer _retryTimer;
    qint64 _retryAtNs = 0;

    std::deque<std::pair<qint64, qint64>> _sentWindow;     ///< (steady ns, bytes)
    double _behindSeconds = 0;
    quint64 _sentFragments = 0;
    quint64 _droppedFragments = 0;
};
