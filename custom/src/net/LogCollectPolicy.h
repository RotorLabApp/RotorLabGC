/****************************************************************************
 *
 * RotorLab GC - when to ask the aircraft for its log, and when to stop.
 *
 * Pure rules, no QObject and no vehicle, so they can be tested on their own.
 * LogCollector and FleetLogs hold the state; these functions decide.
 *
 * WHAT THESE RULES ANSWER (G98 F-3, measured on SITL ArduCopter)
 *
 *   ArduPilot refuses LOG_REQUEST_LIST while armed: no LOG_ENTRY, only the
 *   STATUSTEXT "Disarm for log download". An attempt made then says nothing
 *   about the log, so it must not use up one of the few attempts.
 *
 *   The open log keeps growing for about 30 s after disarm. A log listed and
 *   downloaded too early is cut short, so the size is listed twice about 10 s
 *   apart and the download starts once it holds still (or after a cap).
 *
 *   A real failure (a radio drop) is retried, spaced out so the attempts are
 *   not all spent inside the same bad half minute.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QString>

namespace LogCollectPolicy {

/// Attempts at pulling one log off the aircraft that may fail for real.
/// An attempt refused because the aircraft is armed is not one of them.
constexpr int kMaxCollectAttempts = 4;

/// Time between the two listings that decide whether the log is still growing.
constexpr int kSettleIntervalMs = 10 * 1000;

/// Stop waiting for the log to hold still after this long, and take what is
/// listed.
constexpr int kSettleCapMs = 60 * 1000;

/// Wait after an armed refusal before asking again. Not an attempt.
constexpr int kArmedRetryMs = 15 * 1000;

/// How long to wait before the next attempt, after `failedAttempts` real
/// failures: 30 s, then 2 min, then 10 min.
qint64 retryDelayMs(int failedAttempts);

/// True when `text` is the autopilot refusing a log listing because it is armed.
bool isDisarmRefusal(const QString &text);

/// The operator's sentence once every attempt is used up. `why` is the last
/// failure reason.
QString gaveUpText(const QString &why);

/// The attempt bookkeeping for one flight's log.
struct Attempts {
    int     used = 0;           ///< real failures so far
    qint64  nextTryMs = 0;      ///< epoch ms; 0 means now
    QString lastReason;         ///< the last failure, kept for the operator
};

/// True when a collection may start: the aircraft reports disarmed, the
/// flight's end (epoch seconds) has passed, attempts remain, and any retry
/// wait is over. An end more than ten minutes in the future is a clock that
/// disagrees with ours and is not waited for.
bool mayStart(const Attempts &a, qint64 nowMs, double flightEndedS, bool aircraftArmed);

/// Record one failed collection at `nowMs`. `aircraftArmed` is true when the
/// aircraft refused because it was armed: that schedules a short wait and
/// uses up no attempt. Returns true when no attempts are left.
bool recordFailure(Attempts &a, const QString &why, bool aircraftArmed, qint64 nowMs);

/// What to do after a listing during the settle wait.
enum class SettleStep {
    ListAgain,      ///< the size moved (or this is the first listing): wait and list again
    Download,       ///< the size held still, or the cap is reached
};

/// `previousSize` is the size at the last listing, or -1 for the first one.
/// `elapsedMs` is the time since the first listing.
SettleStep settle(qint64 previousSize, qint64 currentSize, qint64 elapsedMs);

} // namespace LogCollectPolicy
