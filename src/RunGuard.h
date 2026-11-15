/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QLockFile>
#include <QtCore/QString>

/// Single-instance guard, held for the life of the process.
///
/// The guard is a lock file in a per-user directory. The operating system
/// drops the file's lock when the owning process ends for any reason (normal
/// exit, crash, kill -9, container stop), so a lock left behind by a process
/// that is gone is detected and cleared at the next start instead of blocking
/// it. A lock held by a live instance is never cleared: on Linux and macOS the
/// live owner holds a flock() on the file, and on Windows it keeps the file
/// open without delete sharing, so removing it fails.
class RunGuard
{
public:
    explicit RunGuard(const QString &key);
    ~RunGuard();

    /// True when another live instance holds the lock.
    bool isAnotherRunning();

    /// Take the lock. Returns false only when another live instance holds it.
    bool tryToRun();

    void release();

    /// True when the lock file could not be created at all and this run
    /// started without the single-instance check.
    bool startedWithoutGuard() const { return _withoutGuard; }

    /// The lock file for `key`, for this user on this machine.
    static QString lockFilePath(const QString &key);

private:
    bool _clearIfStale();

    const QString _lockPath;
    QLockFile     _lock;
    bool          _withoutGuard = false;

    Q_DISABLE_COPY(RunGuard)
};
