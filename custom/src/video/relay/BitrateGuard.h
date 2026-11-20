/****************************************************************************
 *
 * RotorLab GC - G109: the output-bitrate guard.
 *
 * The Low and Medium settings exist for field bandwidth, so their
 * caps must hold even when the encoder's rate control cannot hold them (pure
 * noise: rain, foliage, sensor noise at night; Microsoft's software encoder
 * sends 1.76 Mb/s at Low on noise whatever its rate-control setting). Full is
 * the camera's own stream and has no cap: the design gives none.
 *
 *   cap       Low 800 kb/s, Medium 2500 kb/s (the level's own target)
 *   window    the last 2 s of media sent: about two 1 s fragments. Rates are
 *             bits over media seconds, so they do not depend on when the
 *             replies came back. A shorter window would judge single
 *             fragments (one keyframe-heavy second is not a trend); a longer
 *             one lets an overrun go on for longer before the guard acts,
 *             and the session's own backlog bound is 3 s.
 *   Normal -> KeyframesOnly
 *             a full window sent above cap x 1.10. The 10 percent is the
 *             room a CBR encoder needs around its own target (Low averages
 *             0.78 Mb/s on a scene with swings above 0.8 in single seconds);
 *             noise is 120 percent over and trips at once.
 *   KeyframesOnly -> Paused
 *             a full window of keyframes alone still above the cap itself.
 *   back to Normal
 *             the encoder's whole output (measured all the time, also while
 *             only keyframes or nothing is sent, in the pipeline's 0.5 s
 *             chunks) at or under the cap for a quiet period: 5 s, then 10,
 *             20, 40 and 60 s when it trips again (backoff). The quiet period
 *             starts once the 2 s window is at or under the cap (the overrun
 *             has left it) and is judged on its own rate, bits over the whole
 *             period. Single 2 s windows of a settled scene swing over the
 *             cap with how many keyframes they hold (a scene at 783 kb/s with
 *             a 1 s GOP reads 816 kb/s in a 2.13 s window of 3 keyframes), so
 *             judging each of them restarted the period whenever the phase
 *             lined up. A window back over the trip line (the overrun again)
 *             starts the period over, and so does a period whose own rate
 *             ends over the cap. The quiet periods start again from 5 s after
 *             60 s of media in Normal. Trip at 1.10 x cap and come back at
 *             1.00 x cap is the hysteresis.
 *
 *   start grace
 *             after every (re)start the first startGrace90k() of media sent
 *             is not judged and does not enter the window; the first
 *             judgement is on the 2 s after it. A (re)start is setCap (a new
 *             level), reset (the relay on again) and restartGrace(), which
 *             VideoRelay calls whenever the relay encoder opens again or the
 *             source starts a new generation (RelayPipeline::encoderRestarted):
 *             a source restart opens a new encoder whose rate control starts
 *             over, so its first seconds need the same grace as a new level.
 *             Before that, a restart on the same level was judged at once
 *             against a window still holding the old stream (P5 run on
 *             f295d7d: the guard tripped 0.3 s after the encoder opened, at
 *             903 kb/s).
 *             VideoRelay sets kStartGrace90k (6 s). The grace now starts when
 *             the encoder opens, but up to the session's backlog bound
 *             (kBacklog90k, 3 s) of fragments made before it can still go out
 *             after it and count against it (the P5 run's first Low start:
 *             the 4 s grace began at the level change, 2.2 s before the
 *             encoder opened, and was mostly spent on the Full stream's last
 *             fragments). 6 s is that 3 s plus three 1 s GOPs of the new
 *             encoder's own output, so the first judged window always comes
 *             after the opening keyframe, the session's forced one and three
 *             seconds of rate control. The cap still holds for sustained
 *             content: noise trips on the first window after the grace, 8 s
 *             of media in, and the session's backlog bound is unchanged.
 *
 *   encoder aim
 *             the grace covers the encoder's opening seconds; it cannot cover
 *             a rate control that runs over the cap for tens of seconds. The
 *             P5 run (Media Foundation, 720p30 test pattern at Low): 903 to
 *             908 kb/s on a normal picture against an 800 kb/s target for 30
 *             to 60 s before it settled at 763 to 783. So the encoders aim
 *             below the cap (relayEncoderBitRate: 85 percent, 680 kb/s at
 *             Low), and the guard keeps judging against the cap itself. The
 *             same 13 percent overshoot on 680 is 772 kb/s: under the cap, and
 *             12 percent under the trip line (880). Noise does not follow the
 *             aim (1.76 Mb/s at Low whatever the setting) and still trips.
 *
 * Pure logic with no clock of its own (media time only), so tests drive it
 * directly. Lives on the GUI thread, owned by VideoRelay.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QList>
#include <QtCore/QtGlobal>

#include <algorithm>
#include <deque>

class BitrateGuard
{
public:
    enum class State { Normal, KeyframesOnly, Paused };

    static constexpr qint64 kWindow90k = 2 * 90000;
    static constexpr double kTripFactor = 1.10;
    static constexpr double kHoldFactor = 1.00;
    static constexpr double kResumeFactor = 1.00;
    static constexpr qint64 kCalm90k = 60 * 90000;
    /// The grace VideoRelay gives every (re)started stream: 6 s of media
    /// (the 3 s backlog bound plus three 1 s GOPs of the new encoder).
    static constexpr qint64 kStartGrace90k = 6 * 90000;

    /// The quiet periods, in 90 kHz units: 5, 10, 20, 40, 60 s.
    static QList<qint64> quietSchedule90k()
    {
        return {5LL * 90000, 10LL * 90000, 20LL * 90000, 40LL * 90000, 60LL * 90000};
    }

    /// Bits per second; 0 turns the guard off (Full).
    void setCap(qint64 bitsPerSecond)
    {
        _cap = std::max<qint64>(0, bitsPerSecond);
        reset();
    }
    qint64 cap() const { return _cap; }

    /// Media sent after each (re)start that is not judged (0: none, the default).
    /// Takes effect now and at every reset().
    void setStartGrace90k(qint64 grace90k)
    {
        _startGrace90k = std::max<qint64>(0, grace90k);
        _graceLeft90k = _startGrace90k;
    }
    qint64 startGrace90k() const { return _startGrace90k; }
    /// Media still to send before the first judgement (0 once judging).
    qint64 graceLeft90k() const { return _graceLeft90k; }

    /// A new stream or a new level: Normal, windows and backoff cleared.
    void reset()
    {
        _state = State::Normal;
        _sent.clear();
        _content.clear();
        _clearQuiet();
        _normal90k = 0;
        _trips = 0;
        _quietNeeded90k = 0;
        _lastSentBps = 0;
        _lastContentBps = 0;
        _graceLeft90k = _startGrace90k;
    }

    /// The relay encoder opened again or the source started a new generation:
    /// the start grace again and both windows cleared (they hold the old
    /// stream). The state, the backoff and the cap stay: a restart is not the
    /// operator's choice and must not let a sustained overrun start over.
    void restartGrace()
    {
        _sent.clear();
        _content.clear();
        _clearQuiet();
        _graceLeft90k = _startGrace90k;
    }

    /// A fragment put on the wire. True when the state changed.
    bool onSent(qint64 bytes, qint64 duration90k)
    {
        if (_cap <= 0 || _state == State::Paused) {
            return false;
        }
        if (_graceLeft90k > 0) {
            // Just (re)started: the encoder settling and the opening keyframes
            // are not the picture's rate. Not judged, not in the window.
            _graceLeft90k = std::max<qint64>(0, _graceLeft90k - std::max<qint64>(0, duration90k));
            return false;
        }
        _push(_sent, bytes, duration90k);
        if (_state == State::Normal) {
            _normal90k += std::max<qint64>(0, duration90k);
            if (_normal90k >= kCalm90k) {
                _trips = 0;     // calm for a minute: the backoff starts over
            }
        }
        if (!_full(_sent)) {
            return false;
        }
        _lastSentBps = _rate(_sent);
        if (_state == State::Normal && _lastSentBps > _cap * kTripFactor) {
            const QList<qint64> q = quietSchedule90k();
            _quietNeeded90k = q.at(std::min(_trips, int(q.size()) - 1));
            ++_trips;
            _enter(State::KeyframesOnly);
            return true;
        }
        if (_state == State::KeyframesOnly && _lastSentBps > _cap * kHoldFactor) {
            _enter(State::Paused);
            return true;
        }
        return false;
    }

    /// The encoder's whole output (every access unit, before the keyframe
    /// filter and the pause). True when the state changed (back to Normal).
    bool onContent(qint64 bytes, qint64 duration90k)
    {
        if (_cap <= 0) {
            return false;
        }
        _push(_content, bytes, duration90k);
        if (!_full(_content)) {
            return false;
        }
        _lastContentBps = _rate(_content);
        if (_state == State::Normal) {
            return false;
        }
        const qint64 b = std::max<qint64>(0, bytes);
        const qint64 d = std::max<qint64>(0, duration90k);
        if (!_quiet) {
            // The quiet period starts once the overrun has left the window.
            if (_lastContentBps <= _cap * kResumeFactor) {
                _quiet = true;
                _settledBytes = b;
                _settled90k = d;
            }
        } else if (_lastContentBps > _cap * kTripFactor) {
            _clearQuiet();      // over the trip line again: start over
        } else {
            _settledBytes += b;
            _settled90k += d;
        }
        if (_quiet && _settled90k >= _quietNeeded90k) {
            if (rateBps(_settledBytes, _settled90k) <= _cap * kResumeFactor) {
                _enter(State::Normal);
                return true;
            }
            _clearQuiet();      // the period as a whole was over the cap: another one
        }
        return false;
    }

    State state() const { return _state; }
    bool keyframesOnly() const { return _state == State::KeyframesOnly; }
    bool paused() const { return _state == State::Paused; }
    /// Times it has tripped since the last calm minute (drives the backoff).
    int trips() const { return _trips; }
    /// The quiet period the content must stay under the cap before Normal.
    qint64 quietNeeded90k() const { return _quietNeeded90k; }
    /// Media time counted so far in the running quiet period (0 when none runs).
    qint64 settled90k() const { return _settled90k; }
    double lastSentBps() const { return _lastSentBps; }
    double lastContentBps() const { return _lastContentBps; }

    /// Bits per second over a run of (bytes, duration) entries.
    static double rateBps(qint64 bytes, qint64 duration90k)
    {
        return duration90k > 0 ? double(bytes) * 8.0 * 90000.0 / double(duration90k) : 0.0;
    }

private:
    struct Entry
    {
        qint64 bytes = 0;
        qint64 duration90k = 0;
    };
    struct Window
    {
        std::deque<Entry> entries;
        qint64 bytes = 0;
        qint64 duration90k = 0;
        void clear()
        {
            entries.clear();
            bytes = 0;
            duration90k = 0;
        }
    };

    static void _push(Window &w, qint64 bytes, qint64 duration90k)
    {
        const Entry e{std::max<qint64>(0, bytes), std::max<qint64>(0, duration90k)};
        w.entries.push_back(e);
        w.bytes += e.bytes;
        w.duration90k += e.duration90k;
        // Keep the shortest tail that still covers the window.
        while (w.entries.size() > 1 && w.duration90k - w.entries.front().duration90k >= kWindow90k) {
            w.bytes -= w.entries.front().bytes;
            w.duration90k -= w.entries.front().duration90k;
            w.entries.pop_front();
        }
    }
    static bool _full(const Window &w) { return w.duration90k >= kWindow90k; }
    static double _rate(const Window &w) { return rateBps(w.bytes, w.duration90k); }

    void _clearQuiet()
    {
        _quiet = false;
        _settledBytes = 0;
        _settled90k = 0;
    }

    void _enter(State s)
    {
        _state = s;
        _sent.clear();
        _clearQuiet();
        if (s == State::Normal) {
            _normal90k = 0;
            _content.clear();
        }
    }

    qint64 _cap = 0;
    qint64 _startGrace90k = 0;
    qint64 _graceLeft90k = 0;
    State _state = State::Normal;
    Window _sent;
    Window _content;
    bool _quiet = false;            ///< a quiet period is running
    qint64 _settledBytes = 0;       ///< content bytes in the quiet period
    qint64 _settled90k = 0;         ///< content media time in the quiet period
    qint64 _normal90k = 0;
    int _trips = 0;
    qint64 _quietNeeded90k = 0;
    double _lastSentBps = 0;
    double _lastContentBps = 0;
};
