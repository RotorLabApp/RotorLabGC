/****************************************************************************
 *
 * RotorLab GC - getting the aircraft's own recording off it.
 *
 * Shared by both connectors, because there is exactly one
 * LogDownloadController in the application and because every awkward detail
 * below was measured the hard way. A second copy would be a second place to
 * relearn them.
 *
 * WHAT MAKES THIS HARDER THAN IT LOOKS
 *
 *   The log's reported time is its MODIFICATION time, not its start.
 *   ArduPilot fills LOG_ENTRY's `time_utc` from the file's mtime, so it marks
 *   a few seconds AFTER disarm. Measured: a flight ending 22:46:10 produced a
 *   log reporting 22:46:23. The match window is therefore anchored on the
 *   flight's end, not its start.
 *
 *   A still-open log looks like it belongs to whichever sortie just ended.
 *   With LOG_FILE_DSRMROT off the autopilot appends to one log all session and
 *   its mtime is always "now". A size-versus-duration sanity check is what
 *   stops a 124 MB multi-session file being attached to a one-minute flight.
 *
 *   The downloaded file is identified by being NEW, not by rebuilding the name.
 *   QGC renames on collision, so the directory is snapshotted first and diffed
 *   afterwards.
 *
 *   The Analyze view drives the same controller. If the operator is using it,
 *   they get it - this is the background job.
 *
 *   Collection floods the link that carries control, so it never runs while
 *   anything is armed and is abandoned the instant something arms.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QMetaObject>
#include <QtCore/QObject>
#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtCore/QTimer>

class LogCollector : public QObject
{
    Q_OBJECT

public:
    /// One per application, because LogDownloadController is.
    static LogCollector *instance();

    explicit LogCollector(QObject *parent = nullptr);
    ~LogCollector() override;

    bool busy() const { return _busy; }

    /// True when pulling a log off the aircraft is safe and sensible right now.
    /// `whyNot` gets a sentence fit to show an operator.
    bool canCollect(QString *whyNot = nullptr) const;

    /// A log already on disk that matches this flight, or empty.
    /// `endedAt` is epoch seconds; `maxBytes` of 0 means no limit.
    static QString findOnDisk(double endedAt, double durationS, qint64 maxBytes);

    /// Pull the matching log off `sysid`. Emits collected() or failed()
    /// exactly once. Only one collection runs at a time.
    void collect(int sysid, double endedAt, double durationS, qint64 maxBytes);

    /// Stop whatever is in progress. Emits failed().
    void cancel();

signals:
    /// The file is on disk at `path`.
    void collected(const QString &path);
    /// It did not happen. `retryable` is false only when trying again cannot
    /// possibly help - the log is too big, or the aircraft has no such log.
    void failed(const QString &why, bool retryable);
    void logMessage(const QString &line);

private slots:
    void _onListChanged();
    void _onDownloadChanged();
    void _watchdog();

private:
    void _finish(const QString &path, const QString &why, bool retryable);
    void _disconnectController();
    void _snapshot();
    QString _newFileInDir() const;

    bool    _busy = false;
    int     _sysid = 0;
    double  _endedAt = 0;
    double  _durationS = 0;
    qint64  _maxBytes = 0;
    QString _dir;
    /// A member rather than a heap allocation handed between two lambdas -
    /// the old shape leaked whenever the sequence broke between them.
    QSet<QString> _before;

    /// Held so a disconnect can be precise. `disconnect(obj, signal, this, nullptr)`
    /// drops EVERY slot this object has on that signal, which would tear down
    /// a second consumer living in the same object.
    QMetaObject::Connection _listConn;
    QMetaObject::Connection _dlConn;

    QTimer _watchdogTimer;
    int    _idleTicks = 0;
};
