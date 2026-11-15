#include "RunGuardTest.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QSysInfo>
#include <QtCore/QUuid>
#include <QtTest/QTest>

#if !defined(Q_OS_WIN) && !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS)
#include <sys/types.h>
#include <unistd.h>
#endif

#if !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS)
#include "RunGuard.h"

namespace {

QString freshKey()
{
    return QStringLiteral("RunGuardTest %1").arg(QUuid::createUuid().toString(QUuid::Id128));
}

/// Write a lock file the way QLockFile does, as if `pid` (running `app`) had
/// left it there.
void plantLock(const QString &path, qint64 pid, const QString &app = QCoreApplication::applicationName())
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(QByteArray::number(pid) + '\n'
            + app.toUtf8() + '\n'
            + QSysInfo::machineHostName().toUtf8() + '\n');
    f.close();
}

void ageLock(const QString &path, int seconds)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadWrite));
    QVERIFY(f.setFileTime(QDateTime::currentDateTimeUtc().addSecs(-seconds),
                          QFileDevice::FileModificationTime));
    f.close();
}

/// The process name QLockFile reads back for the stranger below.
QString strangerName()
{
#ifdef Q_OS_WIN
    return QStringLiteral("cmd");
#else
    return QStringLiteral("sleep");
#endif
}

/// A process that is not RotorLab GC and keeps running until killed.
void startStranger(QProcess &p)
{
#ifdef Q_OS_WIN
    p.start(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), QStringLiteral("ping -n 60 127.0.0.1 >nul")});
#else
    p.start(QStringLiteral("sleep"), {QStringLiteral("60")});
#endif
}

qint64 deadPid()
{
    QProcess p;
#ifdef Q_OS_WIN
    p.start(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), QStringLiteral("exit 0")});
#else
    p.start(QStringLiteral("true"), {});
#endif
    if (!p.waitForStarted(10000)) {
        return 0;
    }
    const qint64 pid = p.processId();
    p.waitForFinished(10000);
    return pid;
}

} // namespace
#endif

void RunGuardTest::_secondLiveGuardRefused()
{
#if !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS)
    const QString key = freshKey();
    RunGuard first(key);
    QVERIFY(first.tryToRun());
    QVERIFY(QFile::exists(RunGuard::lockFilePath(key)));

    {
        // Same pid in the file as this process, and a lock older than the
        // settle time: both make the guard try to clear it. The live holder
        // must still win.
        RunGuard second(key);
        QVERIFY(!second.tryToRun());
        QVERIFY(second.isAnotherRunning());
    }
    QVERIFY(QFile::exists(RunGuard::lockFilePath(key)));

    first.release();
    RunGuard third(key);
    QVERIFY(third.tryToRun());
    third.release();
#endif
}

void RunGuardTest::_deadOwnerCleared()
{
#if !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS)
    const QString key = freshKey();
    const QString path = RunGuard::lockFilePath(key);
    const qint64 pid = deadPid();
    QVERIFY(pid > 0);
    plantLock(path, pid);   // fresh: as if it crashed a moment ago

    RunGuard guard(key);
    QVERIFY(guard.tryToRun());
    guard.release();
    QVERIFY(!QFile::exists(path));
#endif
}

void RunGuardTest::_ownPidCleared()
{
#if !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS)
    // A container restart can hand the new process the same pid as the one
    // that left the lock. Nothing in this process holds it.
    const QString key = freshKey();
    const QString path = RunGuard::lockFilePath(key);
    plantLock(path, QCoreApplication::applicationPid());

    RunGuard guard(key);
    QVERIFY(guard.tryToRun());
    guard.release();
#endif
}

void RunGuardTest::_youngLockOfLiveStrangerKept()
{
#if !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS)
    // A lock written a moment ago by a live process running the program the
    // lock names may be an instance that is still starting: leave it. (A live
    // pid running another program is a reused pid, which QLockFile itself
    // treats as stale.)
    QProcess stranger;
    startStranger(stranger);
    QVERIFY(stranger.waitForStarted(10000));

    const QString key = freshKey();
    const QString path = RunGuard::lockFilePath(key);
    plantLock(path, stranger.processId(), strangerName());

    RunGuard guard(key);
    QVERIFY(!guard.tryToRun());

    stranger.kill();
    stranger.waitForFinished(10000);
    QFile::remove(path);
#endif
}

void RunGuardTest::_oldUnheldLockCleared()
{
#if !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS)
    // The pid in the file is live and runs the program the lock names, but
    // nobody holds the lock: once it is older than the settle time it is
    // cleared.
    QProcess stranger;
    startStranger(stranger);
    QVERIFY(stranger.waitForStarted(10000));

    const QString key = freshKey();
    const QString path = RunGuard::lockFilePath(key);
    plantLock(path, stranger.processId(), strangerName());
    ageLock(path, 60);

    RunGuard guard(key);
    QVERIFY(guard.tryToRun());
    guard.release();

    stranger.kill();
    stranger.waitForFinished(10000);
#endif
}

void RunGuardTest::_foreignLockKeptAndUnguarded()
{
#if defined(Q_OS_WIN) || defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
    QSKIP("Linux and macOS only");
#else
    // A lock owned by another account may be a live instance's, seen from
    // another account. It is never removed: GC starts without the guard and
    // says so.
    if (::geteuid() != 0) {
        QSKIP("needs root to plant a lock owned by another account");
    }
    // Owned by a live process of the program the lock names, so QLockFile's
    // own staleness check leaves it alone and the owner check decides.
    QProcess stranger;
    startStranger(stranger);
    QVERIFY(stranger.waitForStarted(10000));

    const QString key = freshKey();
    const QString path = RunGuard::lockFilePath(key);
    plantLock(path, stranger.processId(), strangerName());
    QCOMPARE(::chown(QFile::encodeName(path).constData(), 65534, 65534), 0);

    RunGuard guard(key);
    QVERIFY(guard.tryToRun());
    QVERIFY(guard.startedWithoutGuard());
    QVERIFY(QFile::exists(path));
    QCOMPARE(QFileInfo(path).ownerId(), 65534u);

    stranger.kill();
    stranger.waitForFinished(10000);
    QFile::remove(path);
#endif
}
