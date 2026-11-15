#include "RotorLabSpoolTest.h"

#include "rotorlab/FilingRules.h"
#include "rotorlab/FlightSpool.h"
#include "rotorlab/FlightWatcher.h"
#include "rotorlab/RotorLabConnector.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonObject>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>
#include <QtTest/QTest>

using namespace RotorLabFiling;

namespace {

QVariantMap modelRow(const FlightSpool &spool, const QString &id)
{
    for (const QVariant &v : spool.model()) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("id")).toString() == id) {
            return m;
        }
    }
    return {};
}

} // namespace

void RotorLabSpoolTest::init()
{
    UnitTest::init();
    QStandardPaths::setTestModeEnabled(true);
    QDir(FlightSpool::_dir()).removeRecursively();
    QFile::remove(FlightWatcher::pendingPath());
}

void RotorLabSpoolTest::cleanup()
{
    QDir(FlightSpool::_dir()).removeRecursively();
    QFile::remove(FlightWatcher::pendingPath());
    QStandardPaths::setTestModeEnabled(false);
    UnitTest::cleanup();
}

// A82: a 401 stops the drain; a key that verifies (setFilingAllowed(true))
// resumes it, with the item still queued.
void RotorLabSpoolTest::_rejectedKeyStopsAndResumes()
{
    FlightSpool spool(nullptr);
    const QString id = spool.addFlight(QStringLiteral("gc-t-1"),
                                       { { QStringLiteral("external_ref"), QStringLiteral("gc-t-1") } });
    spool._find(id)->state = FlightSpool::State::Sending;
    spool._sending = true;
    spool._onReply(id, 401, QStringLiteral("bad key"), {});
    QVERIFY(!spool._allowed);
    QCOMPARE(spool._find(id)->state, FlightSpool::State::Pending);
    QCOMPARE(spool.pendingCount(), 1);

    spool.setFilingAllowed(true);
    QVERIFY(spool._allowed);
    QCOMPARE(spool.pendingCount(), 1);
}

// A108: a log whose file is gone is parked with its reason, not retried.
void RotorLabSpoolTest::_missingLogParks()
{
    FlightSpool spool(nullptr);
    const QString path = QDir::temp().filePath(QStringLiteral("rotorlab-no-such-log.bin"));
    QFile::remove(path);
    const QString id = spool.addLog(QStringLiteral("gc-t-2-log"), path,
                                    QStringLiteral("log.bin"), QString());
    QCOMPARE(spool.pendingCount(), 1);

    spool._send(*spool._find(id));
    FlightSpool::Item *it = spool._find(id);
    QCOMPARE(it->state, FlightSpool::State::NeedsAttention);
    QCOMPARE(it->attempts, 0);
    QVERIFY(it->lastError.contains(QDir::toNativeSeparators(path)));
    QVERIFY(!spool._sending);
    QCOMPARE(spool.pendingCount(), 0);
    QCOMPARE(spool.needsAttentionCount(), 1);
    const QVariantMap row = modelRow(spool, id);
    QVERIFY(row.value(QStringLiteral("parked")).toBool());
    QVERIFY(!row.value(QStringLiteral("uncollected")).toBool());

    // Discard removes it.
    spool.forget(id);
    QCOMPARE(spool.needsAttentionCount(), 0);
}

// A85, A110, A136: a log that could not be collected is on the card with its
// reason, survives a restart, and queues once a file is attached.
void RotorLabSpoolTest::_uncollectedLog()
{
    QString id;
    {
        FlightSpool spool(nullptr);
        spool.addFlight(QStringLiteral("gc-t-3"),
                        { { QStringLiteral("external_ref"), QStringLiteral("gc-t-3") } });
        id = spool.addUncollectedLog(QStringLiteral("gc-t-3-log"), QStringLiteral("gc-t-3"),
                                     2, 1000.0, 120.0,
                                     QStringLiteral("Not collected yet: OSPREY-7 (vehicle 2) is "
                                                    "not the active aircraft."));
        QCOMPARE(spool.needsAttentionCount(), 1);
        QCOMPARE(spool.pendingCount(), 1);      // the flight; the log is parked
        const QVariantMap row = modelRow(spool, id);
        QVERIFY(row.value(QStringLiteral("uncollected")).toBool());
        QVERIFY(row.value(QStringLiteral("detail")).toString().contains(QStringLiteral("OSPREY-7")));

        // The spool never sends it by itself.
        spool.retry(id);
        QCOMPARE(spool._find(id)->state, FlightSpool::State::NeedsAttention);

        spool.setLogProblem(id, QStringLiteral("Not collected: the download did not finish."));
        QCOMPARE(spool._find(id)->lastError,
                 QStringLiteral("Not collected: the download did not finish."));
    }

    FlightSpool again(nullptr);
    FlightSpool::LogToCollect l;
    QVERIFY(again.logToCollect(id, &l));
    QCOMPARE(l.sysid, 2);
    QCOMPARE(l.flightRef, QStringLiteral("gc-t-3"));
    QCOMPARE(l.endedAt, 1000.0);
    QCOMPARE(l.durationS, 120.0);
    QCOMPARE(again.logsToCollect().size(), 1);

    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("log_1_2026-11-13-10-00-00.bin"));
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("x");
    f.close();
    again.attachCollectedLog(id, path);
    QCOMPARE(again._find(id)->state, FlightSpool::State::Pending);
    QCOMPARE(again._find(id)->filePath, path);
    QVERIFY(again._find(id)->lastError.isEmpty());
    QVERIFY(!again.logToCollect(id, nullptr));
}

// A136: a 413 parks the log and names the file for attaching by hand.
void RotorLabSpoolTest::_tooLargeLog()
{
    FlightSpool spool(nullptr);
    const QString path = QDir::temp().filePath(QStringLiteral("big.bin"));
    const QString id = spool.addLog(QStringLiteral("gc-t-4-log"), path,
                                    QStringLiteral("big.bin"), QString());
    spool._find(id)->state = FlightSpool::State::Sending;
    spool._onReply(id, 413, QStringLiteral("Over the 512 MB limit."), {});
    QCOMPARE(spool._find(id)->state, FlightSpool::State::NeedsAttention);
    QVERIFY(spool._find(id)->lastError.contains(QDir::toNativeSeparators(path)));
}

// A83: Try again may replace a flight's body, never its external_ref.
void RotorLabSpoolTest::_retryKeepsReference()
{
    FlightSpool spool(nullptr);
    const QJsonObject stored{ { QStringLiteral("external_ref"), QStringLiteral("gc-t-5") },
                              { QStringLiteral("asset_name"), QStringLiteral("Alpha") } };
    const QString id = spool.addFlight(QStringLiteral("gc-t-5"), stored, 3);
    spool._find(id)->state = FlightSpool::State::Sending;
    spool._onReply(id, 400, QStringLiteral("refused"), {});
    QCOMPARE(spool._find(id)->state, FlightSpool::State::NeedsAttention);

    QJsonObject body;
    int sysid = 0;
    QVERIFY(spool.flightForRetry(id, &body, &sysid));
    QCOMPARE(body, stored);
    QCOMPARE(sysid, 3);

    QJsonObject other = stored;
    other[QStringLiteral("external_ref")] = QStringLiteral("gc-other");
    other[QStringLiteral("pilot_id")] = 9;
    spool.retry(id, other);
    QCOMPARE(spool._find(id)->body, stored);

    QJsonObject fixed = stored;
    fixed[QStringLiteral("pilot_id")] = 7;
    spool._onReply(id, 400, QStringLiteral("refused"), {});
    spool.retry(id, fixed);
    QCOMPARE(spool._find(id)->body, fixed);
    QCOMPARE(spool._find(id)->externalRef, QStringLiteral("gc-t-5"));
    QCOMPARE(spool._find(id)->state, FlightSpool::State::Pending);
}

void RotorLabSpoolTest::_retryFlightBody()
{
    // Yesterday's flight: aircraft Bravo (id 9), pilot P1 (4), North field.
    const QJsonObject stored{
        { QStringLiteral("external_ref"), QStringLiteral("gc-t-6") },
        { QStringLiteral("pilot_id"), 4 },
        { QStringLiteral("asset_id"), 9 },
        { QStringLiteral("site"), QStringLiteral("North field") },
        { QStringLiteral("flight_type"), QStringLiteral("survey") },
        { QStringLiteral("crew"), QStringLiteral("A. Smith") },
        { QStringLiteral("jurisdiction"), QStringLiteral("US") },
        { QStringLiteral("takeoff_at"), QStringLiteral("2026-11-12T10:00:00Z") },
        { QStringLiteral("duration_min"), 12.5 },
        { QStringLiteral("remarks"), QStringLiteral("Filed by RotorLab GC.") } };

    // Today's settings: aircraft Alpha (id 11), pilot P2 (7), South field.
    const QJsonObject today{
        { QStringLiteral("external_ref"), QStringLiteral("gc-other") },
        { QStringLiteral("pilot_id"), 7 },
        { QStringLiteral("asset_id"), 11 },
        { QStringLiteral("site"), QStringLiteral("South field") },
        { QStringLiteral("flight_type"), QStringLiteral("other") },
        { QStringLiteral("flight_type_other"), QStringLiteral("demo") },
        { QStringLiteral("crew"), QStringLiteral("B. Jones") },
        { QStringLiteral("jurisdiction"), QStringLiteral("GB") },
        { QStringLiteral("takeoff_at"), QStringLiteral("2026-11-13T09:00:00Z") },
        { QStringLiteral("duration_min"), 3.0 } };

    // Stored values are kept: nothing of today's flight reaches yesterday's.
    QJsonObject out = retryFlightBody(stored, today);
    QCOMPARE(out, stored);

    // Missing values are filled.
    const QJsonObject sparse{
        { QStringLiteral("external_ref"), QStringLiteral("gc-t-7") },
        { QStringLiteral("asset_name"), QStringLiteral("OSPREY-7") },
        { QStringLiteral("site"), QStringLiteral("  ") },
        { QStringLiteral("takeoff_at"), QStringLiteral("2026-11-12T10:00:00Z") } };
    out = retryFlightBody(sparse, today);
    QCOMPARE(out.value(QStringLiteral("external_ref")).toString(), QStringLiteral("gc-t-7"));
    QCOMPARE(out.value(QStringLiteral("pilot_id")).toInt(), 7);
    QCOMPARE(out.value(QStringLiteral("asset_id")).toInt(), 11);
    QVERIFY(!out.contains(QStringLiteral("asset_name")));
    QCOMPARE(out.value(QStringLiteral("site")).toString(), QStringLiteral("South field"));
    QCOMPARE(out.value(QStringLiteral("flight_type")).toString(), QStringLiteral("other"));
    QCOMPARE(out.value(QStringLiteral("flight_type_other")).toString(), QStringLiteral("demo"));
    QCOMPARE(out.value(QStringLiteral("crew")).toString(), QStringLiteral("B. Jones"));
    QCOMPARE(out.value(QStringLiteral("jurisdiction")).toString(), QStringLiteral("GB"));
    QCOMPARE(out.value(QStringLiteral("takeoff_at")).toString(),
             QStringLiteral("2026-11-12T10:00:00Z"));

    // A stored name stays when nothing better is known now; nothing stored
    // takes today's name.
    const QJsonObject nameOnly{ { QStringLiteral("external_ref"), QStringLiteral("gc-t-8") },
                                { QStringLiteral("asset_name"), QStringLiteral("Bravo") } };
    const QJsonObject todayName{ { QStringLiteral("asset_name"), QStringLiteral("Alpha") } };
    QCOMPARE(retryFlightBody(nameOnly, todayName).value(QStringLiteral("asset_name")).toString(),
             QStringLiteral("Bravo"));
    const QJsonObject none{ { QStringLiteral("external_ref"), QStringLiteral("gc-t-9") } };
    QCOMPARE(retryFlightBody(none, todayName).value(QStringLiteral("asset_name")).toString(),
             QStringLiteral("Alpha"));

    // Nothing set now (a restart forgot the pilot): the body is unchanged.
    QCOMPARE(retryFlightBody(stored, QJsonObject{}), stored);
}

void RotorLabSpoolTest::_logLimit()
{
    QCOMPARE(kMaxLogBytes, qint64(512) * 1024 * 1024);
    QCOMPARE(maxLogBytesFrom({}), kMaxLogBytes);
    QCOMPARE(maxLogBytesFrom({ { QStringLiteral("max_file_bytes"), 0 } }), kMaxLogBytes);
    QCOMPARE(maxLogBytesFrom({ { QStringLiteral("max_file_bytes"), 1073741824.0 } }),
             qint64(1073741824));
    QCOMPARE(megabytesText(qint64(600) * 1024 * 1024), QStringLiteral("600 MB"));
    QCOMPARE(megabytesText(1), QStringLiteral("1 MB"));
    const QString s = logTooLargeText(qint64(600) * 1024 * 1024, kMaxLogBytes,
                                      QStringLiteral("C:\\logs\\big.bin"));
    QVERIFY(s.contains(QStringLiteral("600 MB")));
    QVERIFY(s.contains(QStringLiteral("512 MB")));
    QVERIFY(s.contains(QStringLiteral("C:\\logs\\big.bin")));
    QVERIFY(!s.contains(QChar(0x2014)));
}

// A85: a sortie inside the re-arm window is on disk and comes back marked
// recovered; an empty list clears the file.
void RotorLabSpoolTest::_pendingFlightsOnDisk()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("pending.json"));

    FlightWatcher::Flight f;
    f.sysid        = 2;
    f.takeoffAt    = QDateTime(QDate(2026, 11, 13), QTime(10, 0, 0, 250), Qt::UTC);
    f.landingAt    = QDateTime(QDate(2026, 11, 13), QTime(10, 12, 30), Qt::UTC);
    f.durationS    = 750.0;
    f.maxHeightM   = 61.5;
    f.maxDistanceM = 420.0;
    f.lat          = 51.5;
    f.lon          = -0.12;
    f.hasPosition  = true;
    QVERIFY(FlightWatcher::savePending(path, { f }));

    const QList<FlightWatcher::Flight> back = FlightWatcher::loadPending(path);
    QCOMPARE(back.size(), 1);
    const FlightWatcher::Flight &b = back.first();
    QCOMPARE(b.sysid, 2);
    QCOMPARE(b.takeoffAt, f.takeoffAt);
    QCOMPARE(b.landingAt, f.landingAt);
    QCOMPARE(b.durationS, 750.0);
    QCOMPARE(b.maxHeightM, 61.5);
    QCOMPARE(b.maxDistanceM, 420.0);
    QCOMPARE(b.lat, 51.5);
    QCOMPARE(b.lon, -0.12);
    QVERIFY(b.hasPosition);
    QVERIFY(b.recovered);
    QVERIFY(!f.recovered);

    QVERIFY(FlightWatcher::savePending(path, {}));
    QVERIFY(!QFile::exists(path));
    QVERIFY(FlightWatcher::loadPending(path).isEmpty());
}

// A110 (M2): while any aircraft is armed no log download starts; the log
// waits on the card with the reason instead.
void RotorLabSpoolTest::_noLogPullWhileArmed()
{
    RotorLabConnector *const rl = RotorLabConnector::instance();
    QVERIFY(rl);
    const int was = rl->_flights->_armedCount;
    rl->_flights->_armedCount = 1;

    // sysid 0 and a time with no log on disk, so the armed check decides.
    const QString ref = QStringLiteral("gc-t-armed");
    rl->_collectLogFor(ref, 0, 1000.0, 60.0);
    rl->_flights->_armedCount = was;

    QString id;
    for (const FlightSpool::LogToCollect &l : rl->_spool->logsToCollect()) {
        if (l.flightRef == ref) {
            id = l.id;
        }
    }
    QVERIFY(!id.isEmpty());
    QString reason;
    for (const QVariant &v : rl->_spool->model()) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("id")).toString() == id) {
            reason = m.value(QStringLiteral("detail")).toString();
        }
    }
    rl->_spool->forget(id);
    QVERIFY2(reason.startsWith(QStringLiteral("Waiting until no aircraft is armed")),
             qPrintable(reason));
}

// A155: discarding a flight also discards the logs waiting on it, and never
// touches a log file.
void RotorLabSpoolTest::_discardFlightTakesItsLogs()
{
    QTemporaryDir dir;
    const QString logPath = dir.filePath(QStringLiteral("log_1_2026-11-14-05-00-00.bin"));
    QFile f(logPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("x");
    f.close();

    FlightSpool spool(nullptr);
    const QString flight = spool.addFlight(QStringLiteral("gc-f1"),
                                           { { QStringLiteral("external_ref"), QStringLiteral("gc-f1") } });
    const QString log = spool.addLog(QStringLiteral("gc-f1-log"), logPath,
                                     QStringLiteral("log.bin"), QStringLiteral("gc-f1"));
    const QString uncollected = spool.addUncollectedLog(QStringLiteral("gc-f1-log2"),
                                                        QStringLiteral("gc-f1"), 2, 1000.0, 60.0,
                                                        QStringLiteral("Not collected."));
    const QString other = spool.addFlight(QStringLiteral("gc-f2"),
                                          { { QStringLiteral("external_ref"), QStringLiteral("gc-f2") } });
    const QString otherLog = spool.addLog(QStringLiteral("gc-f2-log"), logPath,
                                          QStringLiteral("log.bin"), QStringLiteral("gc-f2"));

    QCOMPARE(spool.dependentCount(flight, FlightSpool::Kind::Log), 2);
    QCOMPARE(modelRow(spool, flight).value(QStringLiteral("logs")).toInt(), 2);

    spool.forget(flight);
    QVERIFY(!spool._find(flight));
    QVERIFY(!spool._find(log));
    QVERIFY(!spool._find(uncollected));
    QVERIFY(spool._find(other));
    QVERIFY(spool._find(otherLog));
    QVERIFY(!QFile::exists(QDir(FlightSpool::_dir()).filePath(log + QStringLiteral(".json"))));
    QVERIFY(QFile::exists(logPath));        // the log file itself is never deleted
    QCOMPARE(spool.pendingCount(), 2);      // gc-f2 and its log

    // Nothing is left from that flight after a restart either.
    FlightSpool again(nullptr);
    QVERIFY(!again._find(log));
    QVERIFY(!again._find(uncollected));
    QVERIFY(again._find(otherLog));
    QCOMPARE(again._find(otherLog)->state, FlightSpool::State::Pending);
}

// A155: a station that already has a pending log whose flight is gone
// shows it parked with Discard after a restart; it is not deleted.
void RotorLabSpoolTest::_orphanLogParkedOnLoad()
{
    const QString logPath = QDir::temp().filePath(QStringLiteral("rotorlab-orphan.bin"));
    QString id;
    {
        FlightSpool spool(nullptr);
        id = spool.addLog(QStringLiteral("gc-gone-log"), logPath, QStringLiteral("orphan.bin"),
                          QStringLiteral("gc-gone"));
        QCOMPARE(spool._find(id)->state, FlightSpool::State::Pending);   // the old stuck state
    }

    FlightSpool again(nullptr);
    FlightSpool::Item *it = again._find(id);
    QVERIFY(it);
    QCOMPARE(it->state, FlightSpool::State::NeedsAttention);
    QVERIFY(it->lastError.contains(QDir::toNativeSeparators(logPath)));
    QCOMPARE(again.pendingCount(), 0);
    QCOMPARE(again.needsAttentionCount(), 1);
    QVERIFY(again.summaryText().isEmpty());
    QVERIFY(modelRow(again, id).value(QStringLiteral("parked")).toBool());

    again.forget(id);
    QCOMPARE(again.needsAttentionCount(), 0);
}

// A155: checklist runs follow the same rule as logs. Discarding a flight
// removes its runs; a pending run whose flight is gone is parked on load.
void RotorLabSpoolTest::_checklistRunsFollowTheirFlight()
{
    const QJsonObject run{ { QStringLiteral("external_ref"), QStringLiteral("gc-run") } };
    QString orphanRun;
    QString waitingRun;
    {
        FlightSpool spool(nullptr);
        const QString flight = spool.addFlight(QStringLiteral("gc-c1"),
                                               { { QStringLiteral("external_ref"), QStringLiteral("gc-c1") } });
        const QString post = spool.addChecklist(QStringLiteral("gc-c1-post"), run,
                                                QStringLiteral("gc-c1"));
        const QString log = spool.addLog(QStringLiteral("gc-c1-log"),
                                         QDir::temp().filePath(QStringLiteral("c1.bin")),
                                         QStringLiteral("c1.bin"), QStringLiteral("gc-c1"));
        // A pre-flight run waiting for the next flight is nobody's orphan.
        waitingRun = spool.addChecklist(QStringLiteral("gc-pre"), run, FlightSpool::awaitNextFlight());
        // The stuck state an earlier build could leave.
        orphanRun = spool.addChecklist(QStringLiteral("gc-c9-post"), run, QStringLiteral("gc-c9"));

        QCOMPARE(spool.dependentCount(flight, FlightSpool::Kind::Checklist), 1);
        QCOMPARE(spool.dependentCount(flight, FlightSpool::Kind::Log), 1);
        const QVariantMap row = modelRow(spool, flight);
        QCOMPARE(row.value(QStringLiteral("checklists")).toInt(), 1);
        QCOMPARE(row.value(QStringLiteral("logs")).toInt(), 1);

        spool.forget(flight);
        QVERIFY(!spool._find(post));
        QVERIFY(!spool._find(log));
        QVERIFY(spool._find(waitingRun));
        QCOMPARE(spool._find(orphanRun)->state, FlightSpool::State::Pending);
    }

    FlightSpool again(nullptr);
    QCOMPARE(again._find(orphanRun)->state, FlightSpool::State::NeedsAttention);
    QVERIFY(again._find(orphanRun)->lastError.contains(QStringLiteral("checklist run")));
    QCOMPARE(again._find(waitingRun)->state, FlightSpool::State::Pending);
    QVERIFY(modelRow(again, orphanRun).value(QStringLiteral("parked")).toBool());
}
