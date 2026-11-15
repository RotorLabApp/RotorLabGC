/****************************************************************************
 *
 * RotorLab GC - getting the aircraft's own recording off it.
 *
 ****************************************************************************/

#include "LogCollector.h"

#include <limits>

#include <QtCore/QApplicationStatic>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>

#include "AppSettings.h"
#include "LogDownloadController.h"
#include "LogEntry.h"
#include "MultiVehicleManager.h"
#include "QGCLoggingCategory.h"
#include "QmlObjectListModel.h"
#include "SettingsManager.h"
#include "Vehicle.h"

QGC_LOGGING_CATEGORY(LogCollectorLog, "gcs.custom.net.logcollector")

namespace {

/// The window a log's reported time must fall in, measured from when the
/// flight ENDED - because that number is a modification time, not a start.
constexpr qint64 kMatchBeforeEndS = 60;
constexpr qint64 kMatchAfterEndS  = 180;

/// How much a log may plausibly hold per minute of flight, with a floor so a
/// very short sortie is not held to an absurd number. Deliberately loose -
/// high-rate IMU logging is real - but no per-flight log is a hundred times
/// its own sortie.
constexpr double kMaxMbPerMinute = 30.0;
constexpr double kMinPlausibleMb = 20.0;

/// The controller can go quiet without either of its signals firing. Two of
/// these and the sequence is declared broken.
constexpr int kWatchdogMs = 15000;

bool plausibleForFlight(qint64 sizeBytes, double durationS)
{
    if (durationS <= 0) {
        return true;
    }
    const double allowedMb = qMax(kMinPlausibleMb, (durationS / 60.0) * kMaxMbPerMinute);
    return double(sizeBytes) / (1024.0 * 1024.0) <= allowedMb;
}

QString logDir()
{
    AppSettings *const app = SettingsManager::instance()
                                ? SettingsManager::instance()->appSettings() : nullptr;
    return app ? app->logSavePath() : QString();
}

/// Recover the log's own time from the name QGC gave it,
/// `log_<id>_<yyyy-M-d-hh-mm-ss>.bin`, falling back to the file's mtime.
QDateTime logTimeFromName(const QFileInfo &fi)
{
    const QString base = fi.completeBaseName();
    const int under = base.indexOf(QLatin1Char('_'), 4);
    if (base.startsWith(QLatin1String("log_")) && under > 0) {
        const QString stamp = base.mid(under + 1);
        const QDateTime whole = QDateTime::fromString(stamp, QStringLiteral("yyyy-M-d-hh-mm-ss"));
        if (whole.isValid()) {
            return whole;
        }
        // QGC appends _1, _2 ... when a name is already taken.
        const int dup = stamp.lastIndexOf(QLatin1Char('_'));
        if (dup > 0) {
            const QDateTime trimmed =
                QDateTime::fromString(stamp.left(dup), QStringLiteral("yyyy-M-d-hh-mm-ss"));
            if (trimmed.isValid()) {
                return trimmed;
            }
        }
    }
    return fi.lastModified();
}

} // namespace

Q_APPLICATION_STATIC(LogCollector, _logCollectorInstance);

LogCollector *LogCollector::instance()
{
    return _logCollectorInstance();
}

LogCollector::LogCollector(QObject *parent)
    : QObject(parent)
{
    _watchdogTimer.setInterval(kWatchdogMs);
    connect(&_watchdogTimer, &QTimer::timeout, this, &LogCollector::_watchdog);

    // Abandon a collection the moment anything arms. Pulling a log floods the
    // same link that carries control, and the crew flying again outranks any
    // file.
    MultiVehicleManager *const mvm = MultiVehicleManager::instance();
    if (!mvm) {
        return;
    }
    auto watch = [this](Vehicle *v) {
        if (!v) {
            return;
        }
        connect(v, &Vehicle::armedChanged, this, [this](bool armed) {
            if (armed && _busy) {
                emit logMessage(tr("aircraft armed - stopping the log download"));
                cancel();
            }
        });
    };
    connect(mvm, &MultiVehicleManager::vehicleAdded, this, watch);
    if (QmlObjectListModel *list = mvm->vehicles()) {
        for (int i = 0; i < list->count(); ++i) {
            watch(qobject_cast<Vehicle *>((*list)[i]));
        }
    }
}

LogCollector::~LogCollector() = default;

// ------------------------------------------------------------------- on disk

QString LogCollector::findOnDisk(double endedAt, double durationS, qint64 maxBytes)
{
    const QString dir = logDir();
    if (dir.isEmpty()) {
        return {};
    }
    const qint64 ended = qint64(endedAt);
    const qint64 from  = ended - kMatchBeforeEndS;
    const qint64 to    = ended + kMatchAfterEndS;

    // Nearest to the flight's end, not merely the first inside the window:
    // two sorties minutes apart both fall in it, and attaching the wrong log
    // is worse than attaching none.
    QString best;
    qint64  bestGap = std::numeric_limits<qint64>::max();

    const QStringList patterns{ QStringLiteral("*.bin"), QStringLiteral("*.BIN"),
                                QStringLiteral("*.ulg"), QStringLiteral("*.tlog") };
    const QFileInfoList files = QDir(dir).entryInfoList(patterns, QDir::Files);
    for (const QFileInfo &fi : files) {
        if (fi.size() <= 0 || !plausibleForFlight(fi.size(), durationS)) {
            continue;
        }
        if (maxBytes > 0 && fi.size() > maxBytes) {
            continue;
        }
        const qint64 t = logTimeFromName(fi).toSecsSinceEpoch();
        if (t < from || t > to) {
            continue;
        }
        const qint64 gap = qAbs(t - ended);
        if (gap < bestGap) {
            bestGap = gap;
            best    = fi.absoluteFilePath();
        }
    }
    return best;
}

// ----------------------------------------------------------------- preflight

bool LogCollector::canCollect(QString *whyNot) const
{
    auto no = [whyNot](const QString &why) {
        if (whyNot) {
            *whyNot = why;
        }
        return false;
    };

    LogDownloadController *const ldc = LogDownloadController::instance();
    if (!ldc) {
        return no(tr("no log download available"));
    }
    if (_busy) {
        return no(tr("already collecting a log"));
    }
    // The Analyze view drives this same controller. If the operator is using
    // it, they get it - we are the background job here.
    if (ldc->property("downloadingLogs").toBool() || ldc->property("requestingList").toBool()) {
        return no(tr("the Log Download page is busy"));
    }

    MultiVehicleManager *const mvm = MultiVehicleManager::instance();
    Vehicle *const v = mvm ? mvm->activeVehicle() : nullptr;
    if (!v) {
        return no(tr("no aircraft connected"));
    }
    // Never over a link that is carrying a flight.
    if (v->armed() || v->flying()) {
        return no(tr("the aircraft is flying"));
    }
    // Nor while any other aircraft is armed: a bulk download on a shared link
    // competes with the one that is flying.
    if (QmlObjectListModel *const list = mvm->vehicles()) {
        for (int i = 0; i < list->count(); ++i) {
            Vehicle *const other = qobject_cast<Vehicle *>((*list)[i]);
            if (other && (other->armed() || other->flying())) {
                return no(tr("waiting until no aircraft is armed"));
            }
        }
    }
    return true;
}

// ------------------------------------------------------------------ collect

void LogCollector::_snapshot()
{
    _before.clear();
    const QFileInfoList existing = QDir(_dir).entryInfoList(QDir::Files);
    for (const QFileInfo &fi : existing) {
        _before.insert(fi.absoluteFilePath());
    }
}

QString LogCollector::_newFileInDir() const
{
    const QFileInfoList now = QDir(_dir).entryInfoList(QDir::Files);
    for (const QFileInfo &fi : now) {
        if (!_before.contains(fi.absoluteFilePath()) && fi.size() > 0) {
            return fi.absoluteFilePath();
        }
    }
    return {};
}

void LogCollector::collect(int sysid, double endedAt, double durationS, qint64 maxBytes)
{
    QString whyNot;
    if (!canCollect(&whyNot)) {
        emit failed(whyNot, true);
        return;
    }
    LogDownloadController *const ldc = LogDownloadController::instance();
    MultiVehicleManager *const mvm = MultiVehicleManager::instance();
    Vehicle *const v = mvm ? mvm->activeVehicle() : nullptr;
    if (!ldc || !v) {
        emit failed(tr("no aircraft connected"), true);
        return;
    }
    // The controller only ever talks to the active vehicle, so a log for a
    // different SysID has to wait until that aircraft is the active one.
    if (sysid != 0 && v->id() != sysid) {
        emit failed(tr("vehicle %1 is not the active aircraft").arg(sysid), true);
        return;
    }

    _dir = logDir();
    if (_dir.isEmpty()) {
        emit failed(tr("no log folder is set in General settings"), true);
        return;
    }

    _busy      = true;
    _sysid     = sysid;
    _endedAt   = endedAt;
    _durationS = durationS;
    _maxBytes  = maxBytes;
    _idleTicks = 0;
    _snapshot();

    // Armed BEFORE refresh(), which starts the listing synchronously.
    _listConn = connect(ldc, &LogDownloadController::requestingListChanged,
                        this, &LogCollector::_onListChanged);
    _watchdogTimer.start();
    ldc->refresh();
}

void LogCollector::_onListChanged()
{
    LogDownloadController *const ldc = LogDownloadController::instance();
    if (!ldc || ldc->property("requestingList").toBool()) {
        return;             // still listing
    }
    QObject::disconnect(_listConn);
    _idleTicks = 0;

    const qint64 ended = qint64(_endedAt);
    const qint64 from  = ended - kMatchBeforeEndS;
    const qint64 to    = ended + kMatchAfterEndS;

    QGCLogEntry *pick = nullptr;
    qint64 bestGap = std::numeric_limits<qint64>::max();
    QmlObjectListModel *const rows =
        qobject_cast<QmlObjectListModel *>(ldc->property("model").value<QObject *>());
    for (int i = 0; rows && i < rows->count(); ++i) {
        QGCLogEntry *const le = qobject_cast<QGCLogEntry *>((*rows)[i]);
        if (!le || le->size() == 0 || !plausibleForFlight(le->size(), _durationS)) {
            continue;
        }
        const QDateTime t = le->time();
        // A board with no GPS time reports a nonsense date; QGC calls it
        // "UnknownDate". We cannot match that to a flight, so it is left for
        // the operator rather than uploading a guess.
        if (!t.isValid() || t.date().year() < 2010) {
            continue;
        }
        const qint64 secs = t.toSecsSinceEpoch();
        if (secs < from || secs > to) {
            continue;
        }
        const qint64 gap = qAbs(secs - ended);
        if (gap < bestGap) {
            bestGap = gap;
            pick    = le;
        }
    }

    if (!pick) {
        _finish({}, tr("the aircraft has no log for this flight"), true);
        return;
    }
    if (_maxBytes > 0 && qint64(pick->size()) > _maxBytes) {
        // Named with both sizes, so the crew knows to fetch it from the
        // aircraft by hand and attach it in RotorLab.
        const qint64 mb    = (qint64(pick->size()) + 1024 * 1024 - 1) / (1024 * 1024);
        const qint64 maxMb = _maxBytes / (1024 * 1024);
        _finish({}, tr("the log on the aircraft is %1 MB and RotorLab takes up to %2 MB for "
                       "one file; download it on the Log Download page and attach it in "
                       "RotorLab by hand").arg(mb).arg(maxMb), false);
        return;
    }

    _dlConn = connect(ldc, &LogDownloadController::downloadingLogsChanged,
                      this, &LogCollector::_onDownloadChanged);
    pick->setSelected(true);
    ldc->download(_dir);
}

void LogCollector::_onDownloadChanged()
{
    LogDownloadController *const ldc = LogDownloadController::instance();
    if (!ldc || ldc->property("downloadingLogs").toBool()) {
        return;
    }
    QObject::disconnect(_dlConn);

    const QString written = _newFileInDir();
    if (written.isEmpty()) {
        // Cancelled, or the transfer failed. A radio drops; this is worth
        // trying again.
        _finish({}, tr("the download did not finish"), true);
        return;
    }
    _finish(written, {}, true);
}

void LogCollector::_watchdog()
{
    LogDownloadController *const ldc = LogDownloadController::instance();
    const bool active = ldc && (ldc->property("downloadingLogs").toBool()
                                || ldc->property("requestingList").toBool());
    if (active) {
        _idleTicks = 0;
        return;
    }
    // The sequence hangs off two signals from a controller we do not own and
    // that the Analyze view can also drive. If it has gone idle without either
    // handler running, release rather than wedging for the rest of the session.
    if (++_idleTicks >= 2) {
        _finish({}, tr("the log download stopped responding"), true);
    }
}

void LogCollector::cancel()
{
    if (!_busy) {
        return;
    }
    if (LogDownloadController *ldc = LogDownloadController::instance()) {
        ldc->cancel();          // deletes the partial file
    }
    _finish({}, tr("the log download was stopped"), true);
}

void LogCollector::_disconnectController()
{
    // Precise: only the two connections this object made, never every slot it
    // happens to have on those signals.
    QObject::disconnect(_listConn);
    QObject::disconnect(_dlConn);
}

void LogCollector::_finish(const QString &path, const QString &why, bool retryable)
{
    if (!_busy) {
        return;                 // exactly one of collected()/failed(), ever
    }
    _busy = false;
    _watchdogTimer.stop();
    _idleTicks = 0;
    _disconnectController();
    _before.clear();

    if (!path.isEmpty()) {
        emit collected(path);
    } else {
        emit failed(why, retryable);
    }
}
