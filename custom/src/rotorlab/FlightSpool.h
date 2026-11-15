/****************************************************************************
 *
 * RotorLab GC RotorLab Connector - the spool.
 *
 * A ground station lives in a field, in a truck, on a hotspot, in a basement.
 * "No network" is the normal case and "filed immediately" is the happy
 * accident, so every record goes to disk BEFORE the first attempt - even when
 * the link is up. A connector that only spools after a failure loses the flight
 * that was in flight when the laptop lid closed.
 *
 * One directory, one file per pending item, written durably: contents flushed
 * and fsynced, then renamed into place. A record that exists only in a page
 * cache is a record a power cut takes.
 *
 * Three rules that are not preferences:
 *
 *   external_ref is generated ONCE, at creation, and reused on every retry
 *   for ever. Regenerating on retry is how one flight becomes four in a
 *   customer's logbook after a bad night on a hotspot.
 *
 *   The body stored here is the COMPLETE, FINAL payload, and it is what gets
 *   sent on every attempt. A checklist run that retries with a partial body
 *   replaces the stored run and destroys the failure note that was the most
 *   valuable thing in it.
 *
 *   A `pending` item is never deleted on a timer. If the disk is filling, say
 *   so; do not quietly throw away somebody's flight records.
 *
 * Ordering: a flight goes before its checklists and its log, because the log
 * endpoint needs the numeric flight id and a checklist wants it. The dependency
 * is explicit in the record rather than relying on how filenames happen to sort.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QVariantList>

class RotorLabClient;
class QNetworkReply;

class FlightSpool : public QObject
{
    Q_OBJECT

public:
    explicit FlightSpool(RotorLabClient *client, QObject *parent = nullptr);
    ~FlightSpool() override;

    enum class Kind    { Flight, Checklist, Log };
    enum class State   { Pending, Sending, Done, NeedsAttention };

    struct Item {
        QString id;                 ///< the file's stem, unique for ever
        Kind    kind = Kind::Flight;
        QString externalRef;        ///< minted once, reused on every retry
        QJsonObject body;           ///< the complete final payload
        QString filePath;           ///< logs only - the file on disk
        QString displayName;        ///< logs only - `?name=`
        QString dependsOn;          ///< external_ref of the flight this needs
        int     sysid = 0;          ///< the vehicle that flew it; 0 when not known
        int     flightId = -1;      ///< filled in when the flight is accepted
        double  created = 0;
        int     attempts = 0;
        double  lastAttempt = 0;
        QString lastError;
        int     lastStatus = 0;         ///< HTTP status of the last reply; 0 when none or unknown
        State   state = State::Pending;
    };

    /// Queue a flight. Returns the item id. `sysid` is kept so Try again can
    /// look the aircraft up again.
    QString addFlight(const QString &externalRef, const QJsonObject &body, int sysid = 0);

    /// A pre-flight run is completed BEFORE the flight it belongs to exists.
    /// Passing this as `dependsOn` holds the run until the next flight is
    /// filed, at which point bindWaitingChecklists() gives it the real
    /// reference. A run that floats free of its flight is worth much less.
    static QString awaitNextFlight() { return QStringLiteral("\x01next"); }

    /// Queue a checklist run against a flight. `dependsOn` is that flight's
    /// external_ref - or awaitNextFlight() when the flight has not happened yet.
    QString addChecklist(const QString &externalRef, const QJsonObject &body,
                         const QString &dependsOn);

    /// A flight was filed. Anything waiting for "the next flight" is now its.
    void bindWaitingChecklists(const QString &flightRef, int flightId);

    /// Queue a log upload against a flight.
    QString addLog(const QString &externalRef, const QString &filePath,
                   const QString &displayName, const QString &dependsOn);

    /// A flight's log that could not be collected (no aircraft, not the
    /// active one, over the size limit, the download failed). Parked as
    /// needs attention with `reason`, so the spool card says why and offers
    /// Try again and Discard; nothing is sent until a file is attached.
    QString addUncollectedLog(const QString &externalRef, const QString &dependsOn,
                              int sysid, double endedAt, double durationS,
                              const QString &reason);
    /// The log for an uncollected item is now on disk; queue it to send.
    void attachCollectedLog(const QString &id, const QString &filePath);
    /// A new reason for an uncollected log (a retry failed again).
    void setLogProblem(const QString &id, const QString &reason);

    /// What the connector needs to try collecting a log again.
    struct LogToCollect {
        QString id;
        QString flightRef;
        int     sysid = 0;
        double  endedAt = 0;
        double  durationS = 0;
    };
    /// True when `id` is a log that has not been collected yet.
    bool logToCollect(const QString &id, LogToCollect *out) const;
    QList<LogToCollect> logsToCollect() const;

    /// A parked flight's stored body and vehicle, so Try again can rebuild
    /// it from the current settings. False for anything else.
    bool flightForRetry(const QString &id, QJsonObject *body, int *sysid) const;
    /// True when a flight with this takeoff_at is already in the spool.
    bool hasFlightTakenOffAt(const QString &takeoffIso) const;

    /// Try now. Safe to call at any time; does nothing when something is
    /// already in flight.
    void drainNow();

    /// The connector is not in a state that may file. Stops the drain without
    /// losing anything.
    void setFilingAllowed(bool allowed);

    /// The RotorLab version from GET /api/v1/version, or empty when not
    /// known. Decides how a flight with no pilot named is described
    /// (RotorLabFiling::serverNamesPilots()).
    void setServerVersion(const QString &version);

    int pendingCount() const;
    int needsAttentionCount() const;
    /// "3 flights and 1 log waiting to file", or empty when there is nothing.
    QString summaryText() const;
    QString lastAttemptText() const;
    QVariantList model() const;

    /// Forget an item the operator has dealt with by hand. Forgetting a
    /// flight also forgets the logs and checklist runs waiting on it, so
    /// nothing is left whose flight is gone. Only spool records are removed,
    /// never a log file.
    void forget(const QString &id);
    /// How many logs or checklist runs (`kind`) wait on this flight; the
    /// Discard confirm names them.
    int dependentCount(const QString &id, Kind kind) const;
    /// Put a parked item back in the queue. For a flight, a non-empty
    /// `newBody` with the same external_ref replaces the stored body first.
    void retry(const QString &id, const QJsonObject &newBody = QJsonObject());

signals:
    void changed();
    void logMessage(const QString &line);
    /// A flight was accepted and now has a numeric id. Anything depending on
    /// it can proceed.
    void flightFiled(const QString &externalRef, int flightId);
    /// RotorLab answered a filing, a checklist or a log with 200: it is
    /// reachable again.
    void serverAnswered();
    /// Maintenance the server raised from the flight's own data - a component
    /// reaching a limit, and so on. Advisory only: it is never a failure of the
    /// filing and never a reason to hold an aircraft.
    void workRaised(const QStringList &items);
    /// Sentences about a filed flight the operator should see after landing:
    /// no pilot named, a voided re-send. Advisory, never a
    /// failure. `flightBody` is the body as sent, so the station can say
    /// which flight; pilot notes are apart because they are shown first.
    void filingNotice(const QJsonObject &flightBody, const QStringList &pilotNotes,
                      const QStringList &otherNotes);

private slots:
    void _tick();

private:
    friend class RotorLabSpoolTest;

    static bool _uncollected(const Item &it) { return it.kind == Kind::Log && it.filePath.isEmpty(); }
    /// Park an item as needs attention with a sentence for the spool card.
    void _park(Item &it, const QString &reason);
    /// A log or checklist run waiting on a flight that is no longer in the
    /// spool: it can never be sent, so it is parked with a reason and
    /// Discard, never left pending and never deleted unseen.
    bool _orphan(const Item &it) const;
    void _parkOrphan(Item &it);

    static QString _dir();
    static bool    _writeDurable(const QString &path, const QByteArray &bytes);

    void  _load();
    void  _save(const Item &it) const;
    void  _remove(const QString &id);
    Item *_find(const QString &id);
    Item *_findByRef(const QString &ref);

    /// Give an item that depends on a flight that has ALREADY been filed its
    /// numeric flight id. _onReply() back-fills dependents that exist when the
    /// flight is accepted; this is the other half, for the far more common case
    /// where the dependent is created afterwards - a log is collected seconds
    /// or minutes after the flight it belongs to has filed. Without it such an
    /// item waits on a flight id that will never arrive.
    void _resolveDependency(Item &it);

    /// A flight stored with a flight type the server no longer takes (a word
    /// from an earlier GC build) is rewritten onto the published list, with
    /// external_ref untouched so a re-send can never file a second copy. A
    /// parked one goes back in the queue only when its last reply was a 400
    /// (or unknown, from an earlier build) and its type was just changed
    /// (RotorLabFiling::requeueAfterTypeFix()). True when it changed.
    bool _correctFlightType(Item &it);

    /// The next item that may be sent right now, or nullptr.
    Item *_next();
    /// Per-item backoff: 1 s, 5 s, 30 s, 2 min, 10 min, then every 10 min.
    bool  _dueNow(const Item &it) const;

    void _send(Item &it);
    void _onReply(const QString &id, int status, const QString &error,
                  const QJsonObject &body);

    RotorLabClient *_client = nullptr;
    QList<Item> _items;
    QTimer  _timer;
    bool    _sending = false;
    bool    _allowed = true;
    bool    _namesPilots = false;   ///< server 3.5.2 or later (A54)
    QPointer<QNetworkReply> _inFlight;
};
