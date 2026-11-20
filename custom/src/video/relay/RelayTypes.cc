/****************************************************************************
 *
 * RotorLab GC - G109 live relay: log category and clock.
 *
 ****************************************************************************/

#include "RelayTypes.h"

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

#include "QGCLoggingCategory.h"

#include <chrono>

QGC_LOGGING_CATEGORY(VideoRelayLog, "gcs.custom.video.relay")

qint64 relayThreadCpuNs()
{
#if defined(Q_OS_WIN)
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
        return -1;
    }
    const auto ticks = [](const FILETIME &f) {
        return (qint64(f.dwHighDateTime) << 32) | qint64(f.dwLowDateTime);
    };
    return (ticks(kernel) + ticks(user)) * 100;     // 100 ns units
#elif defined(CLOCK_THREAD_CPUTIME_ID)
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) {
        return -1;
    }
    return qint64(ts.tv_sec) * 1'000'000'000LL + qint64(ts.tv_nsec);
#else
    return -1;
#endif
}

qint64 relaySteadyNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
