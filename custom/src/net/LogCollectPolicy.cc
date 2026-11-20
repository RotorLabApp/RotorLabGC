/****************************************************************************
 *
 * RotorLab GC - when to ask the aircraft for its log, and when to stop.
 *
 ****************************************************************************/

#include "LogCollectPolicy.h"

namespace LogCollectPolicy {

qint64 retryDelayMs(int failedAttempts)
{
    if (failedAttempts <= 0) {
        return 0;
    }
    if (failedAttempts == 1) {
        return 30 * 1000;
    }
    if (failedAttempts == 2) {
        return 2 * 60 * 1000;
    }
    return 10 * 60 * 1000;
}

bool isDisarmRefusal(const QString &text)
{
    return text.contains(QLatin1String("Disarm for log download"), Qt::CaseInsensitive);
}

QString gaveUpText(const QString &why)
{
    QString reason = why.trimmed();
    while (reason.endsWith(QLatin1Char('.'))) {
        reason.chop(1);
    }
    if (reason.isEmpty()) {
        return QStringLiteral("could not download it from the aircraft. Attach it by hand.");
    }
    return QStringLiteral("could not download it from the aircraft: %1. Attach it by hand.")
        .arg(reason);
}

bool mayStart(const Attempts &a, qint64 nowMs, double flightEndedS, bool aircraftArmed)
{
    if (aircraftArmed || a.used >= kMaxCollectAttempts) {
        return false;
    }
    if (a.nextTryMs > 0 && nowMs < a.nextTryMs) {
        return false;
    }
    const qint64 endedMs = qint64(flightEndedS * 1000.0);
    if (endedMs > 0 && nowMs < endedMs && endedMs - nowMs <= 10 * 60 * 1000) {
        return false;
    }
    return true;
}

bool recordFailure(Attempts &a, const QString &why, bool aircraftArmed, qint64 nowMs)
{
    a.lastReason = why;
    if (aircraftArmed) {
        a.nextTryMs = nowMs + kArmedRetryMs;
        return a.used >= kMaxCollectAttempts;
    }
    ++a.used;
    a.nextTryMs = nowMs + retryDelayMs(a.used);
    return a.used >= kMaxCollectAttempts;
}

SettleStep settle(qint64 previousSize, qint64 currentSize, qint64 elapsedMs)
{
    if (elapsedMs >= kSettleCapMs) {
        return SettleStep::Download;
    }
    if (previousSize < 0) {
        return SettleStep::ListAgain;
    }
    return (currentSize == previousSize) ? SettleStep::Download : SettleStep::ListAgain;
}

} // namespace LogCollectPolicy
