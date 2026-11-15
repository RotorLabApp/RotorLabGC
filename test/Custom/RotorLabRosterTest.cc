#include "RotorLabRosterTest.h"

#include "rotorlab/RotorLabRoster.h"
#include "net/HttpClient.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtTest/QTest>

namespace {

// /api/v1/health as RotorLab 3.5.1 sends it (CONTRACT-gc-roster.md section 1).
const char *kHealth = R"({"airworthiness": {
  "1":  {"state": "go", "readiness": "No flags", "limits": false, "advisory": false, "why": []},
  "2":  {"state": "advisory", "readiness": "No flags, 2 notes", "limits": false, "advisory": true,
         "why": ["Prop scuff noted", "Camera mount loose"]},
  "3":  {"state": "limits", "limits": true, "advisory": false, "why": ["Max wind 20 kt"]},
  "4":  {"state": "unknown", "readiness": "Condition unknown", "why": ["Battery log unreadable"]},
  "5":  {"state": "no_go", "readiness": "No go", "why": ["Grounding work order open"],
         "grounding": ["workorder"]},
  "6":  {"state": "future_word", "why": []},
  "7":  {"state": "advisory", "why": ["Compass calibration old"]},
  "8":  {"state": "go", "readiness": "Nothing judged", "why": []},
  "9":  {"state": "advisory", "why": []},
  "10": {"state": "advisory"}
}})";

// /api/v1/workorders (section 2, corrected 2026-11-13 09:10): each row carries
// RotorLab's own overdue, due_soon and days_to_due.
const char *kWork = R"({"workorders": [
  {"id": 11, "asset_id": 5, "title": "Wash airframe", "status": "open",
   "priority": "routine", "due_by": "2026-11-30", "closed": "", "grounding_hold": 0,
   "overdue": false, "due_soon": false, "days_to_due": 17},
  {"id": 12, "asset_id": 5, "title": "Replace port-aft motor bearing", "status": "in_progress",
   "priority": "grounding", "due_by": "2026-11-10", "closed": "", "grounding_hold": 1,
   "overdue": true, "due_soon": false, "days_to_due": -3},
  {"id": 13, "asset_id": 5, "title": "Prop hub torque check", "status": "open",
   "priority": "urgent", "due_by": "", "closed": "", "grounding_hold": 1,
   "overdue": false, "due_soon": false, "days_to_due": null},
  {"id": 14, "asset_id": 5, "title": "SB-2026-001: Gimbal firmware", "status": "open",
   "priority": "soon", "due_by": "2026-11-15", "closed": "", "grounding_hold": 0,
   "overdue": false, "due_soon": true, "days_to_due": 2},
  {"id": 15, "asset_id": 5, "title": "Old grounding item", "status": "done",
   "priority": "grounding", "due_by": "", "closed": "2026-11-01T10:00:00Z", "grounding_hold": 1,
   "overdue": false, "due_soon": false, "days_to_due": null},
  {"id": 16, "asset_id": 5, "title": "Cancelled item", "status": "cancelled",
   "priority": "urgent", "due_by": "", "closed": "", "grounding_hold": 0,
   "overdue": false, "due_soon": false, "days_to_due": null},
  {"id": 17, "asset_id": 3, "title": "Other aircraft", "status": "open",
   "priority": "grounding", "due_by": "", "closed": "", "grounding_hold": 1,
   "overdue": false, "due_soon": false, "days_to_due": null},
  {"id": 18, "asset_id": 5, "title": "Closed by timestamp", "status": "open",
   "priority": "routine", "due_by": "", "closed": "2026-11-02T08:00:00Z", "grounding_hold": 0,
   "overdue": false, "due_soon": false, "days_to_due": null},
  {"id": 19, "asset_id": 2, "title": "Check landing gear", "status": "open",
   "priority": "routine", "due_by": "2026-11-12T00:00:00Z", "closed": "",
   "overdue": true, "due_soon": false, "days_to_due": -1}
]})";

QJsonObject health()
{
    return QJsonDocument::fromJson(kHealth).object().value(QStringLiteral("airworthiness")).toObject();
}

QJsonArray work()
{
    return QJsonDocument::fromJson(kWork).object().value(QStringLiteral("workorders")).toArray();
}

QVariantMap ready(int id)
{
    return RotorLabRoster::readinessFrom(health(), work(), id);
}

QString str(const QVariantMap &m, const char *key)
{
    return m.value(QLatin1String(key)).toString();
}

QString line(const QVariantList &w, int i)
{
    return w[i].toMap().value(QStringLiteral("line")).toString();
}

bool flag(const QVariantList &w, int i, const char *key)
{
    return w[i].toMap().value(QLatin1String(key)).toBool();
}

} // namespace

void RotorLabRosterTest::_stateWords()
{
    // Each state one to one; RotorLab's own `readiness` wins when present.
    QCOMPARE(str(ready(1), "readiness"), QStringLiteral("No flags"));
    QCOMPARE(str(ready(2), "readiness"), QStringLiteral("No flags, 2 notes"));
    QCOMPARE(str(ready(3), "readiness"), QStringLiteral("Limits on record"));
    QCOMPARE(str(ready(4), "readiness"), QStringLiteral("Condition unknown"));
    QCOMPARE(str(ready(5), "readiness"), QStringLiteral("No go"));
    QCOMPARE(str(ready(8), "readiness"), QStringLiteral("Nothing judged"));
    // advisory without `readiness`: RotorLab words it by note count.
    QCOMPARE(str(ready(7), "readiness"), QStringLiteral("No flags, 1 note"));
    QCOMPARE(str(ready(9), "readiness"), QStringLiteral("No flags"));
    QCOMPARE(str(ready(10), "readiness"), QStringLiteral("No flags, with notes"));
    // A word this build does not know is passed through as sent.
    QCOMPARE(str(ready(6), "readiness"), QStringLiteral("future_word"));

    // The key itself is never rewritten.
    QCOMPARE(str(ready(3), "state"), QStringLiteral("limits"));
    QCOMPARE(str(ready(5), "state"), QStringLiteral("no_go"));

    // Nothing on record for this id: no state, no word, no note.
    QVERIFY(str(ready(42), "state").isEmpty());
    QVERIFY(str(ready(42), "readiness").isEmpty());
    QVERIFY(str(ready(42), "note").isEmpty());
    QVERIFY(str(ready(-1), "note").isEmpty());
}

void RotorLabRosterTest::_notes()
{
    // go gets no note; advisory, limits, unknown and no_go each get one.
    QVERIFY(str(ready(1), "note").isEmpty());
    QVERIFY(str(ready(8), "note").isEmpty());
    QCOMPARE(str(ready(2), "note"),
             QStringLiteral("RotorLab lists this aircraft as No flags, 2 notes: "
                            "Prop scuff noted; Camera mount loose"));
    QCOMPARE(str(ready(3), "note"),
             QStringLiteral("RotorLab lists this aircraft as Limits on record: Max wind 20 kt"));
    QCOMPARE(str(ready(4), "note"),
             QStringLiteral("RotorLab lists this aircraft as Condition unknown: Battery log unreadable"));
    QCOMPARE(str(ready(5), "note"),
             QStringLiteral("RotorLab lists this aircraft as No go: Grounding work order open"));
    QCOMPARE(str(ready(7), "note"),
             QStringLiteral("RotorLab lists this aircraft as No flags, 1 note: Compass calibration old"));
    QCOMPARE(str(ready(9), "note"), QStringLiteral("RotorLab lists this aircraft as No flags"));
    QCOMPARE(str(ready(6), "note"), QStringLiteral("RotorLab lists this aircraft as future_word"));
    QCOMPARE(ready(2).value(QStringLiteral("why")).toStringList(),
             QStringList({ QStringLiteral("Prop scuff noted"), QStringLiteral("Camera mount loose") }));

    // Never relabeled, and never words of permission.
    for (int id = 1; id <= 10; ++id) {
        const QString note = str(ready(id), "note").toLower();
        QVERIFY2(!note.contains(QStringLiteral("caution")), qPrintable(note));
        for (const char *word : { "cleared", "approved", "safe", "authorized", "\xE2\x80\x94" }) {
            QVERIFY2(!note.contains(QString::fromUtf8(word)), qPrintable(note));
        }
    }
}

void RotorLabRosterTest::_workCopy()
{
    const QVariantMap r = ready(5);
    const QVariantList w = r.value(QStringLiteral("work")).toList();
    QVERIFY(r.value(QStringLiteral("groundingWork")).toBool());

    // Done, cancelled, closed-by-timestamp and other aircraft are left out.
    QCOMPARE(w.size(), 4);

    // Worst first: grounding priority, grounding hold, soon, routine.
    QCOMPARE(w[0].toMap().value(QStringLiteral("text")).toString(),
             QStringLiteral("Replace port-aft motor bearing"));
    QCOMPARE(line(w, 0),
             QStringLiteral("Open grounding work order: Replace port-aft motor bearing "
                            "(in progress, due by 2026-11-10, past due)"));
    QVERIFY(flag(w, 0, "grounding"));

    QCOMPARE(line(w, 1),
             QStringLiteral("Open grounding work order: Prop hub torque check (urgent, grounding hold)"));
    QVERIFY(flag(w, 1, "grounding"));

    QCOMPARE(line(w, 2),
             QStringLiteral("Open work order: SB-2026-001: Gimbal firmware (soon, due by 2026-11-15, due soon)"));
    QCOMPARE(w[2].toMap().value(QStringLiteral("priority")).toString(), QStringLiteral("soon"));
    QVERIFY(!flag(w, 2, "grounding"));

    QCOMPARE(line(w, 3), QStringLiteral("Open work order: Wash airframe (due by 2026-11-30)"));

    // An aircraft with only routine work has no grounding work.
    const QVariantMap r2 = ready(2);
    QCOMPARE(r2.value(QStringLiteral("work")).toList().size(), 1);
    QVERIFY(!r2.value(QStringLiteral("groundingWork")).toBool());
    // No work at all.
    QVERIFY(ready(1).value(QStringLiteral("work")).toList().isEmpty());
}

void RotorLabRosterTest::_dueDates()
{
    // overdue and due_soon are RotorLab's, read as sent.
    const QVariantList w = RotorLabRoster::workFrom(work(), 5);
    QVERIFY(flag(w, 0, "overdue"));
    QVERIFY(!flag(w, 0, "dueSoon"));
    QVERIFY(!flag(w, 1, "overdue"));
    QVERIFY(!flag(w, 1, "dueSoon"));
    QVERIFY(!flag(w, 2, "overdue"));
    QVERIFY(flag(w, 2, "dueSoon"));
    QVERIFY(!flag(w, 3, "overdue"));
    QVERIFY(!flag(w, 3, "dueSoon"));

    // A date-time due_by is shown by its date part.
    const QVariantList w2 = RotorLabRoster::workFrom(work(), 2);
    QCOMPARE(w2.size(), 1);
    QVERIFY(flag(w2, 0, "overdue"));
    QCOMPARE(line(w2, 0),
             QStringLiteral("Open work order: Check landing gear (due by 2026-11-12, past due)"));
}

void RotorLabRosterTest::_assetRows()
{
    // The crew screen reads readiness and work from the asset row itself.
    const QByteArray aircraft = R"({"aircraft": [
      {"id": 5, "name": "Hex 5", "registration": "N555RL", "status": "active",
       "registration_expires": "2027-01-01"},
      {"id": 1, "name": "Quad 1", "registration": "", "status": "active",
       "registration_expires": "2027-02-01"}
    ]})";
    RotorLabRoster roster(nullptr);
    roster.loadFrom(QByteArray(), aircraft, QByteArray(),
                    QByteArray(R"({"airworthiness": )") +
                        QJsonDocument(health()).toJson(QJsonDocument::Compact) +
                        QByteArray(R"(, "workorders": )") +
                        QJsonDocument(work()).toJson(QJsonDocument::Compact) + "}");

    const QVariantList rows = roster.assets();
    QCOMPARE(rows.size(), 2);

    const QVariantMap hex = rows[0].toMap();
    QCOMPARE(hex.value(QStringLiteral("id")).toInt(), 5);
    QCOMPARE(hex.value(QStringLiteral("name")).toString(), QStringLiteral("Hex 5  (N555RL)"));
    QCOMPARE(hex.value(QStringLiteral("state")).toString(), QStringLiteral("no_go"));
    QCOMPARE(hex.value(QStringLiteral("readiness")).toString(), QStringLiteral("No go"));
    QCOMPARE(hex.value(QStringLiteral("note")).toString(),
             QStringLiteral("RotorLab lists this aircraft as No go: Grounding work order open"));
    QVERIFY(hex.value(QStringLiteral("groundingWork")).toBool());
    const QVariantList hexWork = hex.value(QStringLiteral("work")).toList();
    QCOMPARE(hexWork.size(), 4);
    QCOMPARE(line(hexWork, 0),
             QStringLiteral("Open grounding work order: Replace port-aft motor bearing "
                            "(in progress, due by 2026-11-10, past due)"));
    QCOMPARE(hex.value(QStringLiteral("registrationExpires")).toString(), QStringLiteral("2027-01-01"));

    // A go aircraft with a registration date gets no state note: the
    // registration line alone never brings up a state line.
    const QVariantMap quad = rows[1].toMap();
    QCOMPARE(quad.value(QStringLiteral("name")).toString(), QStringLiteral("Quad 1"));
    QCOMPARE(quad.value(QStringLiteral("state")).toString(), QStringLiteral("go"));
    QVERIFY(quad.value(QStringLiteral("note")).toString().isEmpty());
    QVERIFY(quad.value(QStringLiteral("work")).toList().isEmpty());
    QCOMPARE(quad.value(QStringLiteral("registrationExpires")).toString(), QStringLiteral("2027-02-01"));
}

namespace {

HttpReply reply(int status, const char *json)
{
    HttpReply r;
    r.netOk  = status != 0;
    r.status = status;
    r.body   = QJsonDocument::fromJson(json).object();
    return r;
}

const char *kPilots   = R"({"pilots": [{"id": 3, "name": "Pat Current", "currency": "current"}]})";
const char *kAircraft = R"({"aircraft": [{"id": 1, "name": "Quad 1", "registration": "N1RL"}]})";

} // namespace

void RotorLabRosterTest::_loadedNeedsPilotsAndAircraft()
{
    // "Roster loaded" means the crew screen has both lists. The pilots reply
    // usually lands first; on its own it must not report loaded while there
    // are no aircraft to pick from.
    RotorLabRoster roster(nullptr);
    roster.loadFrom(QByteArray(), QByteArray(), QByteArray(), QByteArray());
    QVERIFY(!roster.loaded());

    roster.applyPilotsReply(reply(200, kPilots));
    QCOMPARE(roster.pilots().size(), 1);
    QVERIFY(roster.assets().isEmpty());
    QVERIFY(!roster.loaded());

    // A failed aircraft fetch changes nothing and is not "loaded".
    roster.applyAircraftReply(reply(503, "{}"));
    QVERIFY(!roster.loaded());
    QVERIFY(roster.assets().isEmpty());

    roster.applyAircraftReply(reply(200, kAircraft));
    QVERIFY(roster.loaded());
    QCOMPARE(roster.assets().size(), 1);

    // A later failed refresh keeps serving what was read: still loaded,
    // nothing dropped.
    roster.applyPilotsReply(reply(0, ""));
    roster.applyAircraftReply(reply(500, "{}"));
    QVERIFY(roster.loaded());
    QCOMPARE(roster.pilots().size(), 1);
    QCOMPARE(roster.assets().size(), 1);

    // The other order: aircraft first is not loaded either.
    RotorLabRoster other(nullptr);
    other.loadFrom(QByteArray(), QByteArray(), QByteArray(), QByteArray());
    other.applyAircraftReply(reply(200, kAircraft));
    QVERIFY(!other.loaded());
    other.applyPilotsReply(reply(200, kPilots));
    QVERIFY(other.loaded());

    // An organization with no aircraft still loads once the reply is in:
    // an empty list read is not a list missing.
    RotorLabRoster empty(nullptr);
    empty.loadFrom(QByteArray(), QByteArray(), QByteArray(), QByteArray());
    empty.applyPilotsReply(reply(200, kPilots));
    empty.applyAircraftReply(reply(200, R"({"aircraft": []})"));
    QVERIFY(empty.loaded());
    QVERIFY(empty.assets().isEmpty());
}

void RotorLabRosterTest::_loadedFromCache()
{
    RotorLabRoster roster(nullptr);

    // A cache with pilots and no aircraft list is not a loaded roster.
    roster.loadFrom(kPilots, QByteArray(), QByteArray(), QByteArray());
    QCOMPARE(roster.pilots().size(), 1);
    QVERIFY(!roster.loaded());

    roster.loadFrom(QByteArray(), kAircraft, QByteArray(), QByteArray());
    QVERIFY(!roster.loaded());

    // Both cached: loaded with no network at all, so a station in a dead spot
    // still gets its crew screen.
    roster.loadFrom(kPilots, kAircraft, QByteArray(), QByteArray());
    QVERIFY(roster.loaded());
    QCOMPARE(roster.assets().size(), 1);
}
