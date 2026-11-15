/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "RunGuard.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QStandardPaths>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

// The guard used to be a QSharedMemory segment plus a QSystemSemaphore. On
// Linux both are named kernel objects that outlive a process killed with
// SIGKILL or a crash: the next start then found the old segment and refused to
// run, or waited forever on a semaphore the dead process still "held". A lock
// file whose lock the kernel drops with the process does not have that failure.

namespace
{

/// A lock younger than this may belong to an instance that is starting right
/// now and has not finished writing it, so it is left alone unless its owner
/// is known to be gone.
constexpr qint64 kSettleMs = 3000;

bool processIsGone(qint64 pid)
{
    if (pid <= 0) {
        return false;   // unknown owner: decide by age instead
    }
#ifdef Q_OS_WIN
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) {
        // No such process. Access denied means it exists (another account).
        return ::GetLastError() == ERROR_INVALID_PARAMETER;
    }
    DWORD code = 0;
    const bool gone = ::GetExitCodeProcess(h, &code) && code != STILL_ACTIVE;
    ::CloseHandle(h);
    return gone;
#else
    // EPERM means the process exists under another account: not gone.
    return ::kill(static_cast<pid_t>(pid), 0) == -1 && errno == ESRCH;
#endif
}

} // namespace

QString RunGuard::lockFilePath(const QString &key)
{
    QString dir;
#ifdef Q_OS_WIN
    // The per-user temp directory.
    dir = QDir::tempPath();
#else
    // Per-user, never a shared directory: a lock in a shared /tmp could be
    // planted by another account, or be left there by one, where this user
    // could not remove it. $XDG_RUNTIME_DIR first (Qt checks it is this
    // user's and 0700), then the user's cache directory, then home.
    dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty()) {
        dir = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    }
    if (dir.isEmpty()) {
        dir = QDir::home().filePath(QStringLiteral(".cache"));
    }
#endif
    QDir().mkpath(dir);

    const QByteArray hash = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex();
    return QDir(dir).filePath(QString::fromLatin1(hash) + QStringLiteral(".runguard.lock"));
}

RunGuard::RunGuard(const QString &key)
    : _lockPath(lockFilePath(key))
    , _lock(_lockPath)
{
    // Staleness is decided in _clearIfStale(), from the owner's process and
    // the OS lock, never from the lock's age alone.
    _lock.setStaleLockTime(0);
}

RunGuard::~RunGuard()
{
    release();
}

bool RunGuard::isAnotherRunning()
{
    if (_lock.isLocked()) {
        return false;
    }
    if (tryToRun()) {
        release();
        return false;
    }
    return true;
}

bool RunGuard::tryToRun()
{
    if (_lock.isLocked()) {
        return true;
    }
    if (_lock.tryLock(0)) {
        return true;
    }

    if (_lock.error() != QLockFile::LockFailedError) {
        // The lock file could not be written at all (no runtime directory, no
        // permission, disk full). There is no other instance to protect
        // against that we can see, and a ground station that will not open is
        // the worse failure, so start without the guard and say so.
        qWarning() << "RunGuard: could not create" << _lockPath << "error" << _lock.error()
                   << "; starting without the single-instance check";
        _withoutGuard = true;
        return true;
    }

#ifndef Q_OS_WIN
    // A lock this user does not own was not made by this user's GC. Never
    // remove it (it may be a live instance's, seen from another account):
    // start without the guard, as when the lock cannot be created, and say so.
    const QFileInfo owner(_lockPath);
    if (owner.exists() && owner.ownerId() != static_cast<uint>(::getuid())) {
        qWarning() << "RunGuard: the lock file is owned by another account; starting without the single-instance check:"
                   << _lockPath;
        _withoutGuard = true;
        return true;
    }
#endif

    if (_clearIfStale() && _lock.tryLock(0)) {
        qInfo() << "RunGuard: cleared a lock left by an instance that is no longer running";
        return true;
    }
    return false;
}

void RunGuard::release()
{
    if (_lock.isLocked()) {
        _lock.unlock();
    }
}

bool RunGuard::_clearIfStale()
{
    qint64 pid = 0;
    QString host;
    QString app;
    const bool haveInfo = _lock.getLockInfo(&pid, &host, &app);

    // Our own pid in the file means a previous process with this pid (a
    // container restart reuses low pids) left it: this process holds no lock.
    const bool ownPid = haveInfo && pid == QCoreApplication::applicationPid();
    const bool ownerGone = haveInfo && processIsGone(pid);

    const QDateTime mtime = QFileInfo(_lockPath).lastModified().toUTC();
    const qint64 ageMs = mtime.isValid() ? qAbs(mtime.msecsTo(QDateTime::currentDateTimeUtc())) : 0;

    if (!ownPid && !ownerGone && ageMs < kSettleMs) {
        return false;
    }

    // The removal itself is the liveness test: it fails while a live instance
    // holds the lock (flock on Linux and macOS, an open handle without delete
    // sharing on Windows), so a running instance is never displaced, even if
    // its pid looked reused.
    return _lock.removeStaleLockFile();
}
