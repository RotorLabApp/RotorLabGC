#include "LogCollectPolicyTest.h"

#include <QtTest/QTest>

#include "net/LogCollectPolicy.h"

using namespace LogCollectPolicy;

namespace {

constexpr qint64 kSec = 1000;
constexpr qint64 kMin = 60 * kSec;

/// A fixed flight end (epoch seconds), standing in for 06:04:44 in G98 F-3.
constexpr double kEndedS = 1794463484.0;
constexpr qint64 kEndedMs = qint64(kEndedS) * kSec;

} // namespace

void LogCollectPolicyTest::_armedUsesNoAttempt()
{
    Attempts a;
    const qint64 now = kEndedMs + 5 * kSec;
    for (int i = 0; i < 10; ++i) {
        const bool gaveUp = recordFailure(a, QStringLiteral("the aircraft is armed"), true,
                                          now + i * kArmedRetryMs);
        QVERIFY(!gaveUp);
    }
    QCOMPARE(a.used, 0);
    // A short wait, not a spaced-out retry.
    QCOMPARE(a.nextTryMs, now + 9 * kArmedRetryMs + kArmedRetryMs);
    // Still armed: no start, however long.
    QVERIFY(!mayStart(a, a.nextTryMs + 10 * kMin, kEndedS, true));
    // Disarmed and the wait is over: go.
    QVERIFY(mayStart(a, a.nextTryMs, kEndedS, false));
    QVERIFY(!mayStart(a, a.nextTryMs - 1, kEndedS, false));
}

void LogCollectPolicyTest::_notBeforeDisarmOrFlightEnd()
{
    const Attempts fresh;
    // F-3 attempt 1 was at 06:04:43, a second before the recorded end.
    QVERIFY(!mayStart(fresh, kEndedMs - 1 * kSec, kEndedS, false));
    QVERIFY(mayStart(fresh, kEndedMs, kEndedS, false));
    QVERIFY(!mayStart(fresh, kEndedMs + kMin, kEndedS, true));
    // An end far in the future is a clock that disagrees with ours.
    QVERIFY(mayStart(fresh, kEndedMs - 11 * kMin, kEndedS, false));
    // No end known at all.
    QVERIFY(mayStart(fresh, kEndedMs, 0.0, false));
}

void LogCollectPolicyTest::_retriesSpacedOut()
{
    QCOMPARE(retryDelayMs(1), 30 * kSec);
    QCOMPARE(retryDelayMs(2), 2 * kMin);
    QCOMPARE(retryDelayMs(3), 10 * kMin);

    Attempts a;
    qint64 now = kEndedMs + 20 * kSec;
    const qint64 expected[] = { 30 * kSec, 2 * kMin, 10 * kMin };
    for (int i = 0; i < kMaxCollectAttempts - 1; ++i) {
        QVERIFY(mayStart(a, now, kEndedS, false));
        QVERIFY(!recordFailure(a, QStringLiteral("the download did not finish"), false, now));
        QCOMPARE(a.used, i + 1);
        QCOMPARE(a.nextTryMs - now, expected[i]);
        QVERIFY(!mayStart(a, a.nextTryMs - 1, kEndedS, false));
        now = a.nextTryMs;
    }
    QVERIFY(mayStart(a, now, kEndedS, false));
    QVERIFY(recordFailure(a, QStringLiteral("the download did not finish"), false, now));
    QCOMPARE(a.used, kMaxCollectAttempts);
    // Used up: nothing starts again on its own.
    QVERIFY(!mayStart(a, now + 24 * 60 * kMin, kEndedS, false));
    // The whole run spans minutes, not the 30 s F-3 spent all of them in.
    QVERIFY(now - (kEndedMs + 20 * kSec) >= 12 * kMin);
}

void LogCollectPolicyTest::_reasonKeptInGaveUpText()
{
    QCOMPARE(gaveUpText(QStringLiteral("the aircraft has no log for this flight")),
             QStringLiteral("could not download it from the aircraft: the aircraft has no log "
                            "for this flight. Attach it by hand."));
    // No doubled period.
    QCOMPARE(gaveUpText(QStringLiteral("the download did not finish.")),
             QStringLiteral("could not download it from the aircraft: the download did not "
                            "finish. Attach it by hand."));
    QCOMPARE(gaveUpText(QString()),
             QStringLiteral("could not download it from the aircraft. Attach it by hand."));

    // The last reason survives, including one from an armed refusal in between.
    Attempts a;
    recordFailure(a, QStringLiteral("the log download stopped responding"), false, kEndedMs);
    QCOMPARE(a.lastReason, QStringLiteral("the log download stopped responding"));
    recordFailure(a, QStringLiteral("the aircraft is armed"), true, kEndedMs + kMin);
    QCOMPARE(a.lastReason, QStringLiteral("the aircraft is armed"));
    QCOMPARE(a.used, 1);

    const QString text = gaveUpText(QStringLiteral("the download did not finish"));
    QVERIFY(!text.contains(QChar(0x2014)));     // no em dash
}

void LogCollectPolicyTest::_disarmRefusalRecognized()
{
    QVERIFY(isDisarmRefusal(QStringLiteral("Disarm for log download")));
    QVERIFY(isDisarmRefusal(QStringLiteral("disarm for log download")));
    QVERIFY(!isDisarmRefusal(QStringLiteral("Disarming motors")));
    QVERIFY(!isDisarmRefusal(QString()));
}

void LogCollectPolicyTest::_settleWaitsForSizeToHold()
{
    // First listing: always look again.
    QVERIFY(settle(-1, 3000000, 0) == SettleStep::ListAgain);
    // Still growing 10 s later.
    QVERIFY(settle(3000000, 3300000, kSettleIntervalMs) == SettleStep::ListAgain);
    // Unchanged: download.
    QVERIFY(settle(3300000, 3300000, 2 * kSettleIntervalMs) == SettleStep::Download);
}

void LogCollectPolicyTest::_settleCapTakesWhatIsListed()
{
    QVERIFY(settle(3000000, 3100000, kSettleCapMs - 1) == SettleStep::ListAgain);
    QVERIFY(settle(3000000, 3100000, kSettleCapMs) == SettleStep::Download);
    QVERIFY(settle(-1, 3100000, kSettleCapMs) == SettleStep::Download);
    QVERIFY(kSettleIntervalMs == 10 * kSec);
    QVERIFY(kSettleCapMs == 60 * kSec);
}

void LogCollectPolicyTest::_f3Replay()
{
    // G98 F-3: sweeps every 15 s from 06:04:43. The aircraft refuses while it
    // still counts itself armed; GC must wait instead of spending attempts.
    Attempts a;
    const qint64 sweep0 = kEndedMs - 1 * kSec;
    int started = 0;
    for (int i = 0; i < 3; ++i) {
        const qint64 now = sweep0 + i * 15 * kSec;
        if (!mayStart(a, now, kEndedS, false)) {
            continue;
        }
        ++started;
        recordFailure(a, QStringLiteral("the aircraft said \"Disarm for log download\""),
                      true, now);
    }
    QCOMPARE(started, 2);           // 06:04:43 is before the end and does not start
    QCOMPARE(a.used, 0);            // the two refusals cost nothing
    QVERIFY(mayStart(a, a.nextTryMs, kEndedS, false));
}
