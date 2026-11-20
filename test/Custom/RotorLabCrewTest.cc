#include "RotorLabCrewTest.h"

#include "rotorlab/PicLog.h"
#include "rotorlab/RotorLabClient.h"
#include "rotorlab/RotorLabRoster.h"
#include "rotorlab/RotorLabStore.h"
#include "net/HttpClient.h"

#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QRegularExpression>
#include <QtCore/QSysInfo>
#include <QtCore/QTimer>
#include <QtCore/QUuid>

#include <QtTest/QTest>

using RotorLabCrew::Open;

void RotorLabCrewTest::_crewScreenDeferredWhileArmed()
{
    // A78: the station's own ask (after a verify, or offline with a cached
    // roster) never opens over an armed aircraft. It waits for the disarm.
    QVERIFY(RotorLabCrew::whenToOpen(true, false, false) == Open::Now);
    QVERIFY(RotorLabCrew::whenToOpen(true, true,  false) == Open::AfterDisarm);
    // Connector off or no key: nothing to ask for.
    QVERIFY(RotorLabCrew::whenToOpen(false, false, false) == Open::No);
    QVERIFY(RotorLabCrew::whenToOpen(false, true,  false) == Open::No);
}

void RotorLabCrewTest::_changePilotOpensAnyTime()
{
    // A79: the operator's own Change pilot opens at once, also while flying.
    QVERIFY(RotorLabCrew::whenToOpen(true, false, true) == Open::Now);
    QVERIFY(RotorLabCrew::whenToOpen(true, true,  true) == Open::Now);
    QVERIFY(RotorLabCrew::whenToOpen(false, true, true) == Open::No);
}

void RotorLabCrewTest::_preflightOffer()
{
    const QString none;
    // A71: offered once, before the first arm, with an aircraft connected and
    // a pre-flight published.
    QVERIFY( RotorLabCrew::offerPreflight(true, false, true, true, false, none));
    // Never while armed: no offer sits over a crew that is flying.
    QVERIFY(!RotorLabCrew::offerPreflight(true, true,  true, true, false, none));
    // Nothing connected yet, or nothing published: no offer.
    QVERIFY(!RotorLabCrew::offerPreflight(true, false, false, true, false, none));
    QVERIFY(!RotorLabCrew::offerPreflight(true, false, true, false, false, none));
    // Once per sortie, and never over another offer.
    QVERIFY(!RotorLabCrew::offerPreflight(true, false, true, true, true, none));
    QVERIFY(!RotorLabCrew::offerPreflight(true, false, true, true, false,
                                          QStringLiteral("postflight")));
    // Connector off or no key: nothing.
    QVERIFY(!RotorLabCrew::offerPreflight(false, false, true, true, false, none));
}

void RotorLabCrewTest::_missionsEmptyText()
{
    // A72: a Device Key reads the missions published to the organization,
    // narrowed to their author when a pilot is set. Never "assigned".
    QCOMPARE(RotorLabCrew::missionsEmptyText(false, QString()),
             QStringLiteral("Not loaded yet."));
    QCOMPARE(RotorLabCrew::missionsEmptyText(true, QString()),
             QStringLiteral("No missions published to your organization."));
    QCOMPARE(RotorLabCrew::missionsEmptyText(true, QStringLiteral("Pat Current")),
             QStringLiteral("None made by Pat Current among the missions published to your organization."));
    QVERIFY(!RotorLabCrew::missionsEmptyText(true, QString()).contains(QStringLiteral("assigned")));
}

void RotorLabCrewTest::_armedKeyErrorClearsAtDisarm()
{
    const QString refusal = QStringLiteral("The key is not saved while an aircraft is armed. "
                                           "Enter it again after landing; the current key stays in use.");
    // Still the armed refusal at the disarm: it goes.
    QVERIFY( RotorLabCrew::clearErrorAtDisarm(refusal, refusal));
    // A later error replaced it (a rejected key): that one stays.
    QVERIFY(!RotorLabCrew::clearErrorAtDisarm(QStringLiteral("RotorLab rejected this Device Key."), refusal));
    // No refusal shown: nothing to clear.
    QVERIFY(!RotorLabCrew::clearErrorAtDisarm(QString(), QString()));
    QVERIFY(!RotorLabCrew::clearErrorAtDisarm(QStringLiteral("Some error"), QString()));
}

void RotorLabCrewTest::_verifyDoesNotReask()
{
    // First check of the session: nobody set yet, asked.
    QVERIFY( RotorLabCrew::askAfterVerify(true, false, false, false));
    // A pilot but no aircraft (or the reverse): asked, to fill the gap.
    QVERIFY( RotorLabCrew::askAfterVerify(true, true,  false, false));
    QVERIFY( RotorLabCrew::askAfterVerify(true, false, true,  false));
    // Pilot and aircraft set this session: a Verify or a reconnect does not ask.
    QVERIFY(!RotorLabCrew::askAfterVerify(true, true,  true,  false));
    // A new key: asked, as on the first check.
    QVERIFY( RotorLabCrew::askAfterVerify(true, true,  true,  true));
    // "Ask who is flying at start-up" off: never.
    QVERIFY(!RotorLabCrew::askAfterVerify(false, false, false, true));
}

void RotorLabCrewTest::_filingReconnects()
{
    // Retry now (or any filing) answered 200 while Offline: Connected again.
    QVERIFY( RotorLabCrew::reconnectOnFiled(true, true));
    // Not offline: nothing to change.
    QVERIFY(!RotorLabCrew::reconnectOnFiled(false, true));
    // Never verified (no server version yet): left to the Verify.
    QVERIFY(!RotorLabCrew::reconnectOnFiled(true, false));
}

void RotorLabCrewTest::_externalRefHasNoHostName()
{
    // The install id: 8 hex, made once, the same on every read (each read
    // goes to settings, as after a restart of the same install).
    const QString id = RotorLabStore::installId();
    QVERIFY(QRegularExpression(QStringLiteral("^[0-9a-f]{8}$")).match(id).hasMatch());
    QCOMPARE(RotorLabStore::installId(), id);
    QCOMPARE(RotorLabStore::installId(), id);

    const QString uuid = QUuid::createUuid().toString(QUuid::Id128).left(12);
    const QString ref = RotorLabStore::formatExternalRef(id, QDate(2026, 11, 14), uuid);
    QCOMPARE(ref, QStringLiteral("gc-%1-2026-11-14-%2").arg(id, uuid));

    // Never the machine's or the user's name.
    const QString host = QSysInfo::machineHostName();
    if (!host.isEmpty()) {
        QVERIFY(!ref.contains(host, Qt::CaseInsensitive));
        QVERIFY(!id.contains(host.left(8), Qt::CaseInsensitive));
    }
    const QString user = qEnvironmentVariable("USERNAME", qEnvironmentVariable("USER"));
    if (user.size() >= 3) {
        QVERIFY(!ref.contains(user, Qt::CaseInsensitive));
    }
    // Unique per record.
    QVERIFY(RotorLabStore::formatExternalRef(id, QDate(2026, 11, 14),
                                             QUuid::createUuid().toString(QUuid::Id128).left(12)) != ref);
}

void RotorLabCrewTest::_everyTemplateListed()
{
    // A107: every active template of each kind, in the server's order, not
    // only the first. The settings card and the runner list all of them.
    const QByteArray templates = R"({"templates": [
      {"id": 1, "kind": "preflight",  "name": "Quad pre-flight",  "items": ["Props", "Arms"]},
      {"id": 2, "kind": "postflight", "name": "Quad post-flight", "items": ["Battery temp"]},
      {"id": 3, "kind": "preflight",  "name": "Fixed-wing pre-flight", "items": ["Control surfaces"]},
      {"id": 4, "kind": "preflight",  "name": "Retired list", "active": 0, "items": ["Old"]},
      {"id": 5, "kind": "postflight", "name": "Fixed-wing post-flight", "items": ["Skid wear"]},
      {"id": 6, "kind": "preflight",  "name": "Night pre-flight", "items": ["Strobes"]}
    ]})";
    RotorLabRoster roster(nullptr);
    roster.loadFrom(QByteArray(), QByteArray(), templates, QByteArray());

    const QVariantList pre = roster.templates(QStringLiteral("preflight"));
    QCOMPARE(pre.size(), 3);
    QCOMPARE(pre[0].toMap().value(QStringLiteral("name")).toString(), QStringLiteral("Quad pre-flight"));
    QCOMPARE(pre[1].toMap().value(QStringLiteral("name")).toString(), QStringLiteral("Fixed-wing pre-flight"));
    QCOMPARE(pre[2].toMap().value(QStringLiteral("name")).toString(), QStringLiteral("Night pre-flight"));
    QCOMPARE(pre[1].toMap().value(QStringLiteral("id")).toInt(), 3);
    QCOMPARE(pre[1].toMap().value(QStringLiteral("items")).toStringList(),
             QStringList{QStringLiteral("Control surfaces")});

    const QVariantList post = roster.templates(QStringLiteral("postflight"));
    QCOMPARE(post.size(), 2);
    QCOMPARE(post[1].toMap().value(QStringLiteral("id")).toInt(), 5);
}

// ------------------------------------------------------------ pilot in command

namespace {

QDateTime utc(const char *iso)
{
    return QDateTime::fromString(QLatin1String(iso), Qt::ISODate).toUTC();
}

RotorLabPic::Handoff change(const char *at, int sysid, int fromId, int toId,
                            const char *toName, const char *reason = "")
{
    RotorLabPic::Handoff h;
    h.at = utc(at);
    h.sysid = sysid;
    h.fromId = fromId;
    h.toId = toId;
    h.toName = QLatin1String(toName);
    h.reason = QLatin1String(reason);
    return h;
}

QString field(const QJsonArray &a, int i, const char *key)
{
    const QJsonValue v = a.at(i).toObject().value(QLatin1String(key));
    return v.isDouble() ? QString::number(v.toInt()) : v.toString();
}

// /api/v1/sorties as RotorLab's API shows it.
const char *kSorties = R"json({"day": "2026-11-20", "sorties": [
  {"id": 300, "day": "2026-11-20", "window": "08:00-09:00", "status": "planned",
   "asset_id": 9, "job_id": 80, "job_title": "Other aircraft", "operating_basis": "",
   "seats": [{"user_id": 90, "name": "Not this", "role": "remote_pic", "duty": "pic",
              "duty_label": "PIC (pilot in command)", "controls": true, "planned_pic": true}]},
  {"id": 310, "day": "2026-11-20", "window": "10:00-12:00", "status": "planned",
   "asset_id": 12, "job_id": 87, "job_title": "Earlier plan", "operating_basis": "part107",
   "seats": []},
  {"id": 311, "day": "2026-11-20", "window": "14:00-16:00", "status": "out",
   "asset_id": 12, "job_id": 88, "job_title": "Bridge inspection, span 4",
   "operating_basis": "part107",
   "seats": [
     {"user_id": 57, "name": "Sam Trainee", "role": "at_controls", "duty": "manip",
      "duty_label": "Person manipulating the flight controls", "controls": true},
     {"user_id": 41, "name": "Lee Smith", "role": "remote_pic", "duty": "pic",
      "duty_label": "PIC (pilot in command)", "controls": true,
      "planned_pic": true, "planned_by": "Jo Admin"},
     {"user_id": 72, "name": "Ana Ruiz", "role": "visual_observer", "duty": "vo",
      "duty_label": "VO (visual observer)", "controls": false}
   ]}
]})json";

} // namespace

void RotorLabCrewTest::_picVersionGate()
{
    // One constant: the first RotorLab server version that takes the PIC
    // fields (3.5.4). Older servers get the pilot_id and crew string only.
    QVERIFY(!RotorLabPic::serverTakesPic(QString()));
    QVERIFY(!RotorLabPic::serverTakesPic(QStringLiteral("3.5.2")));
    QVERIFY(!RotorLabPic::serverTakesPic(QStringLiteral("3.5.3")));
    QVERIFY( RotorLabPic::serverTakesPic(QStringLiteral("3.5.4")));
    QVERIFY( RotorLabPic::serverTakesPic(QStringLiteral("v3.6.0+abc")));
    QVERIFY(!RotorLabPic::serverTakesPic(QStringLiteral("garbage")));
    // Or the sorties route answered 200 (a server ahead of its version).
    QVERIFY( RotorLabPic::picEnabled(QStringLiteral("3.5.3"), true));
    QVERIFY(!RotorLabPic::picEnabled(QStringLiteral("3.5.3"), false));
    QVERIFY( RotorLabPic::picEnabled(QStringLiteral("3.5.4"), false));
}

void RotorLabCrewTest::_picPeriods()
{
    const QDateTime takeoff = utc("2026-11-20T14:02:10Z");
    const QDateTime landing = utc("2026-11-20T14:40:00Z");
    const QList<RotorLabPic::Handoff> log = {
        change("2026-11-20T13:50:00Z", 1, -1, 41, "Lee Smith"),
        change("2026-11-20T14:20:44Z", 1, 41, 63, "Pat Current", "Battery swap"),
        // Another vehicle's change is not this flight's.
        change("2026-11-20T14:25:00Z", 2, 63, 99, "Other Vehicle"),
        // After the landing: the next flight's pilot.
        change("2026-11-20T14:45:00Z", 1, 63, 41, "Lee Smith"),
    };

    int id = -2;
    QString name;
    QVERIFY(RotorLabPic::pilotAt(log, takeoff, 1, &id, &name));
    QCOMPARE(id, 41);                                   // pilot_id: the pilot at takeoff
    QVERIFY(!RotorLabPic::pilotAt(log, utc("2026-11-20T13:00:00Z"), 1, &id, &name));

    const QJsonArray p = RotorLabPic::buildPeriods(log, takeoff, landing, 1);
    QCOMPARE(p.size(), 2);
    QCOMPARE(field(p, 0, "role"), QStringLiteral("remote_pic"));
    QCOMPARE(field(p, 0, "pilot_id"), QStringLiteral("41"));
    QCOMPARE(field(p, 0, "from"), QStringLiteral("2026-11-20T14:02:10Z"));
    QCOMPARE(field(p, 0, "to"), QStringLiteral("2026-11-20T14:20:44Z"));
    QCOMPARE(field(p, 0, "sysid"), QStringLiteral("1"));
    QCOMPARE(field(p, 1, "pilot_id"), QStringLiteral("63"));
    QCOMPARE(field(p, 1, "from"), QStringLiteral("2026-11-20T14:20:44Z"));
    QCOMPARE(field(p, 1, "reason"), QStringLiteral("Battery swap"));
    QCOMPARE(field(p, 1, "sent_at"), QStringLiteral("2026-11-20T14:20:44Z"));
    QVERIFY(!p.at(1).toObject().contains(QStringLiteral("to")));   // runs to the landing

    // Cleared in the air: the period ends and none starts.
    const QList<RotorLabPic::Handoff> cleared = {
        change("2026-11-20T13:50:00Z", -1, -1, 41, "Lee Smith"),
        change("2026-11-20T14:10:00Z", -1, 41, -1, ""),
    };
    const QJsonArray c = RotorLabPic::buildPeriods(cleared, takeoff, landing, 1);
    QCOMPARE(c.size(), 1);
    QCOMPARE(field(c, 0, "to"), QStringLiteral("2026-11-20T14:10:00Z"));

    // Nobody named at takeoff, one named in the air (M1): no pilot_id with
    // periods, and one period from the change. An older RotorLab with nothing
    // in the log before takeoff keeps today's pilot.
    const QList<RotorLabPic::Handoff> inAir = {
        change("2026-11-20T14:20:44Z", 1, -1, 63, "Pat Current", "Arrived late"),
    };
    QCOMPARE(RotorLabPic::filingPilot(inAir, takeoff, 1, 63, /*picEnabled*/ true), -1);
    const QJsonArray air = RotorLabPic::buildPeriods(inAir, takeoff, landing, 1);
    QCOMPARE(air.size(), 1);
    QCOMPARE(field(air, 0, "pilot_id"), QStringLiteral("63"));
    QCOMPARE(field(air, 0, "from"), QStringLiteral("2026-11-20T14:20:44Z"));
    QVERIFY(!air.at(0).toObject().contains(QStringLiteral("to")));
    QCOMPARE(RotorLabPic::filingPilot(inAir, takeoff, 1, 63, /*picEnabled*/ false), 63);
    QCOMPARE(RotorLabPic::filingPilot({}, takeoff, 1, 41, false), 41);
    QCOMPARE(RotorLabPic::filingPilot({}, takeoff, 1, 41, true), -1);
    // Named at takeoff: that pilot either way.
    QCOMPARE(RotorLabPic::filingPilot(log, takeoff, 1, 63, true), 41);
    QCOMPARE(RotorLabPic::filingPilot(log, takeoff, 1, 63, false), 41);

    // Nobody named at takeoff and no change: no periods at all.
    QVERIFY(RotorLabPic::buildPeriods({}, takeoff, landing, 1).isEmpty());

    // Never more than the server's 24; the first (takeoff) is kept.
    QList<RotorLabPic::Handoff> many = { change("2026-11-20T13:50:00Z", 1, -1, 1, "P1") };
    for (int i = 0; i < 30; ++i) {
        RotorLabPic::Handoff h = change("2026-11-20T14:03:00Z", 1, i, i + 2, "P");
        h.at = takeoff.addSecs(10 * (i + 1));
        many << h;
    }
    const QJsonArray capped = RotorLabPic::buildPeriods(many, takeoff, landing, 1);
    QCOMPARE(capped.size(), 24);
    QCOMPARE(field(capped, 0, "pilot_id"), QStringLiteral("1"));
    QCOMPARE(field(capped, 23, "pilot_id"), QStringLiteral("31"));
}

void RotorLabCrewTest::_picSorties()
{
    const QJsonObject reply = QJsonDocument::fromJson(kSorties).object();
    QVariantMap info;
    const QVariantList seats = RotorLabPic::seatsFor(reply, 12, &info);
    // The sortie that is out wins over an earlier planned one.
    QCOMPARE(info.value(QStringLiteral("id")).toInt(), 311);
    QCOMPARE(info.value(QStringLiteral("jobTitle")).toString(), QStringLiteral("Bridge inspection, span 4"));
    QCOMPARE(info.value(QStringLiteral("operatingBasis")).toString(), QStringLiteral("part107"));
    QCOMPARE(seats.size(), 3);
    // The planned remote pilot first.
    QCOMPARE(seats[0].toMap().value(QStringLiteral("id")).toInt(), 41);
    QCOMPARE(seats[0].toMap().value(QStringLiteral("dutyLabel")).toString(),
             QStringLiteral("PIC (pilot in command)"));
    QCOMPARE(RotorLabPic::plannedPic(seats), 41);

    // No sortie on that aircraft: nothing, and the roster is the list.
    QVERIFY(RotorLabPic::seatsFor(reply, 77, &info).isEmpty());
    QVERIFY(info.isEmpty());

    // Seats first, then every other pilot; nobody filtered.
    QVariantList pilots;
    for (const auto &p : { std::pair<int, const char *>{41, "Lee Smith"},
                           std::pair<int, const char *>{63, "Pat Current"} }) {
        QVariantMap m;
        m[QStringLiteral("id")] = p.first;
        m[QStringLiteral("name")] = QLatin1String(p.second);
        m[QStringLiteral("currency")] = QStringLiteral("lapsed");
        pilots << m;
    }
    const QVariantList crew = RotorLabPic::mergeCrew(seats, pilots);
    QCOMPARE(crew.size(), 4);
    QCOMPARE(crew[0].toMap().value(QStringLiteral("id")).toInt(), 41);
    QVERIFY(crew[0].toMap().value(QStringLiteral("seat")).toBool());
    QCOMPARE(crew[0].toMap().value(QStringLiteral("currency")).toString(), QStringLiteral("lapsed"));
    QCOMPARE(crew[3].toMap().value(QStringLiteral("id")).toInt(), 63);
    QVERIFY(!crew[3].toMap().value(QStringLiteral("seat")).toBool());

    // crew_rows: everyone but the pilot in command, by duty.
    const QJsonArray rows = RotorLabPic::crewRows(seats, 41);
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows[0].toObject().value(QStringLiteral("user_id")).toInt(), 57);
    QCOMPARE(rows[0].toObject().value(QStringLiteral("duty")).toString(), QStringLiteral("manip"));
    QCOMPARE(rows[1].toObject().value(QStringLiteral("duty")).toString(), QStringLiteral("vo"));
}

void RotorLabCrewTest::_picWording()
{
    // The regime's own term (US, UK, EU and Canada wording).
    QCOMPARE(RotorLabPic::pilotWord(QStringLiteral("part107"), QString()), QStringLiteral("Remote pilot in command"));
    QCOMPARE(RotorLabPic::pilotWord(QString(), QStringLiteral("US")), QStringLiteral("Remote pilot in command"));
    QCOMPARE(RotorLabPic::pilotWord(QStringLiteral("public_aircraft"), QStringLiteral("US")), QStringLiteral("Pilot"));
    QCOMPARE(RotorLabPic::pilotWord(QString(), QStringLiteral("GB")), QStringLiteral("Remote pilot"));
    QCOMPARE(RotorLabPic::pilotWord(QString(), QStringLiteral("EU")), QStringLiteral("Remote pilot"));
    QCOMPARE(RotorLabPic::pilotWord(QString(), QStringLiteral("CA")), QStringLiteral("Pilot"));

    const QDateTime from = QDateTime(QDate(2026, 11, 20), QTime(14, 2, 10));
    QCOMPARE(RotorLabPic::picLine(QStringLiteral("Lee Smith"), from, QStringLiteral("part107"), QString()),
             QStringLiteral("Remote pilot in command: Lee Smith (from 14:02)"));
    QVERIFY(RotorLabPic::picLine(QString(), from, QString(), QString()).isEmpty());
    for (const char *word : { "approved", "cleared", "authorized", "qualified" }) {
        QVERIFY(!RotorLabPic::picLine(QStringLiteral("Lee Smith"), from, QStringLiteral("part107"), QString())
                     .contains(QLatin1String(word), Qt::CaseInsensitive));
    }
}

void RotorLabCrewTest::_picLogFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    PicLog log(dir.path(), nullptr);
    log.append(change("2026-11-20T14:20:44Z", 1, 41, 63, "Pat Current", "Battery swap"));
    log.append(change("2026-11-20T14:30:00Z", 1, 63, -1, ""));
    QCOMPARE(log.entries().size(), 2);

    QFile f(dir.path() + QStringLiteral("/2026-11-20.jsonl"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QList<QByteArray> lines = f.readAll().trimmed().split('\n');
    QCOMPARE(lines.size(), 2);                              // append-only, one line each
    RotorLabPic::Handoff back;
    QVERIFY(RotorLabPic::fromJson(QJsonDocument::fromJson(lines[0]).object(), &back));
    QCOMPARE(back.toId, 63);
    QCOMPARE(back.fromId, 41);
    QCOMPARE(back.reason, QStringLiteral("Battery swap"));
    QCOMPARE(back.at, utc("2026-11-20T14:20:44Z"));

    // Never read back at start: a pilot named before a restart never lands
    // on a flight after it.
    PicLog again(dir.path(), nullptr);
    QVERIFY(again.entries().isEmpty());

    // A folder that cannot be written: logged, never thrown, kept in memory.
    PicLog bad(dir.path() + QStringLiteral("/2026-11-20.jsonl/not-a-dir"), nullptr);
    bad.append(change("2026-11-20T14:20:44Z", 1, 41, 63, "Pat Current"));
    QCOMPARE(bad.entries().size(), 1);
}

namespace {

/// One request, waited for. The reply as the connector would see it.
template <typename Call>
HttpReply waitFor(Call call)
{
    HttpReply out;
    bool done = false;
    QEventLoop loop;
    call([&](const HttpReply &r) { out = r; done = true; loop.quit(); });
    if (!done) {
        QTimer::singleShot(20000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return out;     // a timeout comes back as netOk false, status 0
}

} // namespace

void RotorLabCrewTest::_picLiveServer()
{
    const QString path = qEnvironmentVariable("ROTORLAB_PIC_CONNECTION");
    if (path.isEmpty()) {
        QSKIP("ROTORLAB_PIC_CONNECTION not set: no local PIC RotorLab to check against");
    }
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QJsonObject conn = QJsonDocument::fromJson(f.readAll()).object();
    const int assetId = conn.value(QStringLiteral("aircraft_id")).toInt(-1);
    QVERIFY(assetId >= 0);

    RotorLabClient client;
    client.setBaseUrl(HttpClient::normalizeBaseUrl(conn.value(QStringLiteral("base_url")).toString(),
                                                   /*allowLocalhostHttp*/ true));
    client.setToken(conn.value(QStringLiteral("device_key")).toString());   // never printed

    // The gate: the sorties route answers 200 (this server reports 3.5.3).
    const HttpReply version = waitFor([&](auto cb) { client.getVersion(cb); });
    QCOMPARE(version.status, 200);
    const HttpReply probe = waitFor([&](auto cb) { client.getSorties(QString(), -1, cb); });
    QCOMPARE(probe.status, 200);
    QVERIFY(RotorLabPic::picEnabled(version.body.value(QStringLiteral("version")).toString(),
                                    probe.status == 200));

    // Crew listed first, planned remote pilot first and preselected.
    const HttpReply sorties = waitFor([&](auto cb) { client.getSorties(QString(), assetId, cb); });
    QCOMPARE(sorties.status, 200);
    QVariantMap info;
    const QVariantList seats = RotorLabPic::seatsFor(sorties.body, assetId, &info);
    qInfo() << "sortie" << info.value(QStringLiteral("id")).toInt()
            << info.value(QStringLiteral("jobTitle")).toString() << "seats" << seats.size();
    QCOMPARE(seats.size(), 3);
    const int lee = RotorLabPic::plannedPic(seats);
    QCOMPARE(seats[0].toMap().value(QStringLiteral("id")).toInt(), lee);
    QCOMPARE(seats[0].toMap().value(QStringLiteral("role")).toString(), QStringLiteral("remote_pic"));

    const HttpReply pilots = waitFor([&](auto cb) { client.getPilots(cb); });
    QCOMPARE(pilots.status, 200);
    QVariantList roster;
    for (const QJsonValue &v : pilots.body.value(QStringLiteral("pilots")).toArray()) {
        QVariantMap m;
        m[QStringLiteral("id")]   = v.toObject().value(QStringLiteral("id")).toInt();
        m[QStringLiteral("name")] = v.toObject().value(QStringLiteral("name")).toString();
        roster << m;
    }
    const QVariantList crew = RotorLabPic::mergeCrew(seats, roster);
    QVERIFY(crew.size() >= 3);
    for (int i = 0; i < 3; ++i) {
        QVERIFY(crew[i].toMap().value(QStringLiteral("seat")).toBool());
    }
    QCOMPARE(crew[0].toMap().value(QStringLiteral("id")).toInt(), lee);

    int pat = -1;
    for (const QVariant &s : seats) {
        if (s.toMap().value(QStringLiteral("role")).toString() == QLatin1String("at_controls")) {
            pat = s.toMap().value(QStringLiteral("id")).toInt();
        }
    }
    QVERIFY(pat >= 0);

    // A flight with a handoff from Lee to Pat mid-flight, built the way the
    // connector builds it, filed to the server.
    const QDateTime takeoff = QDateTime::currentDateTimeUtc().addSecs(-1800).addMSecs(
        -QDateTime::currentDateTimeUtc().time().msec());
    const QDateTime handoff = takeoff.addSecs(600);
    const QDateTime landing = takeoff.addSecs(1200);
    RotorLabPic::Handoff named;
    named.at = takeoff.addSecs(-120); named.sysid = 1;
    named.toId = lee; named.toName = seats[0].toMap().value(QStringLiteral("name")).toString();
    RotorLabPic::Handoff swap;
    swap.at = handoff; swap.sysid = 1; swap.fromId = lee; swap.toId = pat;
    swap.toName = QStringLiteral("Pat Current"); swap.reason = QStringLiteral("Battery swap");
    const QList<RotorLabPic::Handoff> log = { named, swap };

    const int atTakeoff = RotorLabPic::filingPilot(log, takeoff, 1, -1, true);
    QCOMPARE(atTakeoff, lee);

    const QString ref = QStringLiteral("gc-pic-check-%1").arg(QDateTime::currentSecsSinceEpoch());
    QJsonObject body;
    body[QStringLiteral("external_ref")] = ref;
    body[QStringLiteral("asset_id")]     = assetId;
    body[QStringLiteral("pilot_id")]     = atTakeoff;
    body[QStringLiteral("flown_at")]     = takeoff.toString(QStringLiteral("yyyy-MM-dd"));
    body[QStringLiteral("duration_min")] = 20.0;
    body[QStringLiteral("takeoff_at")]   = takeoff.toString(Qt::ISODate);
    body[QStringLiteral("landing_at")]   = landing.toString(Qt::ISODate);
    body[QStringLiteral("pic_periods")]  = RotorLabPic::buildPeriods(log, takeoff, landing, 1);
    body[QStringLiteral("crew_rows")]    = RotorLabPic::crewRows(seats, atTakeoff);
    body[QStringLiteral("remarks")]      = QStringLiteral("GC PIC lane check");
    QCOMPARE(body.value(QStringLiteral("pic_periods")).toArray().size(), 2);
    QCOMPARE(body.value(QStringLiteral("crew_rows")).toArray().size(), 2);

    const HttpReply filed = waitFor([&](auto cb) { client.postFlight(body, cb); });
    qInfo() << "filed" << filed.status << "pic_recorded" << filed.body.value(QStringLiteral("pic_recorded")).toInt()
            << "pic_note" << filed.body.value(QStringLiteral("pic_note")).toString()
            << "crew_note" << filed.body.value(QStringLiteral("crew_note")).toString();
    QCOMPARE(filed.status, 200);
    QCOMPARE(filed.body.value(QStringLiteral("pic_recorded")).toInt(), 2);
    QVERIFY(filed.body.value(QStringLiteral("pic_note")).toString().isEmpty());
    QVERIFY(filed.body.value(QStringLiteral("crew_note")).toString().isEmpty());

    // A period naming nobody in the organization: recorded with no member
    // named, and pic_note says so in RotorLab's own sentence.
    QJsonObject odd = body;
    odd[QStringLiteral("external_ref")] = ref + QStringLiteral("-b");
    QJsonArray periods = odd.value(QStringLiteral("pic_periods")).toArray();
    QJsonObject second = periods.at(1).toObject();
    second[QStringLiteral("pilot_id")] = 999999;
    periods[1] = second;
    odd[QStringLiteral("pic_periods")] = periods;
    const HttpReply noted = waitFor([&](auto cb) { client.postFlight(odd, cb); });
    qInfo() << "filed" << noted.status << "pic_recorded" << noted.body.value(QStringLiteral("pic_recorded")).toInt()
            << "pic_note" << noted.body.value(QStringLiteral("pic_note")).toString();
    QCOMPARE(noted.status, 200);
    QVERIFY(!noted.body.value(QStringLiteral("pic_note")).toString().isEmpty());
}
