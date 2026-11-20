/****************************************************************************
 *
 * RotorLab GC Fleet Connector - the aircraft's own flight log.
 *
 * A dataflash log is the one artifact in this chain that cannot be
 * reconstructed. A flight record can be re-derived from telemetry; the .bin
 * exists once, on a card, and if nobody collects it before that card is wiped
 * it is gone. Everything here is shaped by that.
 *
 * Three steps, deliberately separate:
 *
 *   QUEUE      The connector records its own stable flight reference as the
 *              vehicle transitions from armed to disarmed.
 *
 *   COLLECT    Find the file. Already on disk, or pulled off the aircraft over
 *              MAVLink through QGC's own LogDownloadController.
 *
 *   UPLOAD     PUT the raw bytes with their SHA-256. Queued, persisted, retried; the server
 *              dedupes on the SHA-256 of the body, so a blind retry after a
 *              timeout is free and stores nothing twice.
 *
 * Two rules this module must not break, both from §0:
 *
 *   - Nothing here may prevent a flight. Collection is abandoned the instant
 *     the aircraft arms, and no part of this shows a modal or gates a control.
 *   - None of it runs in the telemetry loop. A 40 MB upload must not sit in the
 *     path of a 1 Hz report.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QVariantList>

#include "net/LogCollectPolicy.h"

class FleetClient;
class QNetworkReply;
class QTimer;
class Vehicle;

class FleetLogs : public QObject
{
    Q_OBJECT

public:
    explicit FleetLogs(FleetClient *client, QObject *parent = nullptr);
    ~FleetLogs() override;

    /// Add a completed local flight to the upload queue.
    void queueFlight(const QJsonObject &flight);
    void reset();

    /// Rows for the settings page. Each is a map of
    /// ref/callsign/when/duration/state/detail/progress.
    QVariantList model() const;

    bool busy() const { return _collecting || _uploading; }
    int  waitingCount() const;

    /// Attach a file the connector could not find itself - a card read on
    /// another machine, or a log the operator downloaded by hand.
    Q_INVOKABLE void attachFile(const QString &externalRef, const QString &filePath);

    /// Stop whatever is in flight. Collection resumes on the next sweep;
    /// an upload is safe to restart because dedupe is on the body hash.
    Q_INVOKABLE void cancelCurrent();

    /// Give up on one entry permanently, at the operator's word.
    Q_INVOKABLE void forget(const QString &externalRef);

    /// Try a parked entry again - for the ones parked on something the operator
    /// can fix, like a full server.
    Q_INVOKABLE void retry(const QString &externalRef);

signals:
    void modelChanged();
    void logMessage(const QString &line);

private slots:
    void _sweep();

private:
    /// What we know about one flight the server is missing a log for.
    struct Entry {
        QString  ref;               ///< external_ref, always the server's
        QString  callsign;
        int      sysid = 0;
        QString  uid;
        double   started = 0.0;     ///< epoch seconds, from the server
        double   ended = 0.0;
        double   durationS = 0.0;

        QString  path;              ///< local file, once found
        QString  name;              ///< basename sent as ?name=

        enum class State { Waiting, Collecting, Ready, Uploading, Done, Parked };
        State    state = State::Waiting;
        QString  detail;            ///< the server's own sentence, or ours
        double   progress = 0.0;    ///< 0..1 while collecting or uploading
        bool     retryable = false; ///< parked on something the operator can clear
        /// Parked because the file is over the server's per-file limit. Tracked
        /// separately so raising that limit releases it automatically - the
        /// operator has already answered, and making them find the row and
        /// press a button again is asking twice.
        bool     parkedOnSize = false;
        /// Pulls off the aircraft: real failures, when to try next, and the
        /// last reason. An armed refusal is not counted (LogCollectPolicy).
        LogCollectPolicy::Attempts collectTries;
        /// Uploads attempted for this entry. A dropped connection mid-body is
        /// expected on a big file and is retried, but a refusal we have
        /// misread as retryable must not loop for ever in front of the crew.
        int      uploadAttempts = 0;
    };

    Entry *_entry(const QString &ref);
    void   _save() const;
    void   _load();

    /// Look for a file that matches this flight among logs already on disk.
    /// Matching is on the log's own start time against the server's flight
    /// window, because that is what both sides actually key on.
    QString _findLocalLog(const Entry &e) const;

    /// True when pulling a log off the aircraft is safe and sensible right now.
    bool _canCollect(QString *whyNot = nullptr) const;

    void _startCollect(Entry &e);
    void _onCollectFinished();
    void _startUpload(Entry &e);

    /// Put an entry back in the queue after a failure that a retry can fix.
    void _retryUpload(Entry &e, const QString &why);
    void _park(Entry &e, const QString &why, bool retryable, bool onSize = false);

    FleetClient *_client = nullptr;
    QTimer      *_timer  = nullptr;

    QList<Entry> _entries;

    bool _collecting = false;
    bool _uploading  = false;
    QString _currentRef;                    ///< the entry being worked on
    QPointer<QNetworkReply> _upload;

    int  _maxMb    = 512;

};
