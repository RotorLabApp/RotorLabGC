#include "RotorLabFilingTest.h"

#include "rotorlab/FilingRules.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonObject>
#include <QtCore/QSet>
#include <QtTest/QTest>

using namespace RotorLabFiling;

void RotorLabFilingTest::_publishedList()
{
    const QStringList expected = {
        QStringLiteral("inspection"), QStringLiteral("survey"), QStringLiteral("training"),
        QStringLiteral("test"), QStringLiteral("commercial"), QStringLiteral("recreational"),
        QStringLiteral("proficiency"), QStringLiteral("maintenance"),
        QStringLiteral("public_safety"), QStringLiteral("other") };
    QCOMPARE(flightTypeCodes(), expected);
    QCOMPARE(flightTypeLabel(QStringLiteral("public_safety")), QStringLiteral("Public safety"));
    for (const QString &code : expected) {
        const FlightType f = normalizeFlightType(code, {}, false);
        QCOMPARE(f.type, code);
        QVERIFY(f.other.isEmpty());
    }
    QVERIFY(normalizeFlightType({}, QStringLiteral("x"), true).type.isEmpty());
}

void RotorLabFilingTest::_legacyMapping()
{
    FlightType f = normalizeFlightType(QStringLiteral("mapping"), {}, true);
    QCOMPARE(f.type, QStringLiteral("survey"));
    QVERIFY(f.other.isEmpty());

    for (const char *word : { "patrol", "search", "delivery", "crop dusting" }) {
        f = normalizeFlightType(QLatin1String(word), {}, true);
        QCOMPARE(f.type, QStringLiteral("other"));
        QCOMPARE(f.other, QLatin1String(word));
    }
}

void RotorLabFilingTest::_otherText()
{
    FlightType f = normalizeFlightType(QStringLiteral("other"), {}, true);
    QCOMPARE(f.other, QStringLiteral("Not specified"));
    f = normalizeFlightType(QStringLiteral("other"), {}, false);
    QVERIFY(f.other.isEmpty());

    QCOMPARE(cleanFlightTypeOther(QStringLiteral("  line one\nline\ttwo\x01 ")),
             QStringLiteral("line one line two"));
    QCOMPARE(cleanFlightTypeOther(QString(200, QLatin1Char('a'))).size(), kFlightTypeOtherMax);

    // Anything Python's isprintable() rejects is gone: zero-width space (Cf)
    // dropped, NBSP and U+2028 turned into one ordinary space.
    QCOMPARE(cleanFlightTypeOther(QStringLiteral(u"bridge\u200B check\u00A0north\u2028east")),
             QStringLiteral("bridge check north east"));
    // The 80 is code points: an emoji at position 80 stays whole, never half
    // a surrogate pair.
    const QString emoji = QString(79, QLatin1Char('a')) + QStringLiteral(u"\U0001F681") +
                          QStringLiteral("tail");
    const QString cut = cleanFlightTypeOther(emoji);
    QCOMPARE(cut.toUcs4().size(), kFlightTypeOtherMax);
    QVERIFY(cut.endsWith(QStringLiteral(u"\U0001F681")));

    // Matched like the server's _kind_words: case, spaces and hyphens.
    QCOMPARE(normalizeFlightType(QStringLiteral("Public safety"), {}, true).type,
             QStringLiteral("public_safety"));
    QCOMPARE(normalizeFlightType(QStringLiteral("public-safety"), {}, true).type,
             QStringLiteral("public_safety"));

    // Never with any type but other.
    f = normalizeFlightType(QStringLiteral("survey"), QStringLiteral("stale"), true);
    QVERIFY(f.other.isEmpty());
}

void RotorLabFilingTest::_bodyRewrite()
{
    QJsonObject body{ { QStringLiteral("external_ref"), QStringLiteral("gc-x-1") },
                      { QStringLiteral("flight_type"),  QStringLiteral("patrol") } };
    QVERIFY(normalizeFlightBody(body));
    QCOMPARE(body.value(QStringLiteral("flight_type")).toString(), QStringLiteral("other"));
    QCOMPARE(body.value(QStringLiteral("flight_type_other")).toString(), QStringLiteral("patrol"));
    QCOMPARE(body.value(QStringLiteral("external_ref")).toString(), QStringLiteral("gc-x-1"));
    QVERIFY(!normalizeFlightBody(body));    // already good: unchanged

    QJsonObject ok{ { QStringLiteral("flight_type"), QStringLiteral("inspection") } };
    QVERIFY(!normalizeFlightBody(ok));

    QJsonObject stray{ { QStringLiteral("flight_type"), QStringLiteral("survey") },
                       { QStringLiteral("flight_type_other"), QStringLiteral("x") } };
    QVERIFY(normalizeFlightBody(stray));
    QVERIFY(!stray.contains(QStringLiteral("flight_type_other")));

    QJsonObject none{ { QStringLiteral("site"), QStringLiteral("Northgate") } };
    QVERIFY(!normalizeFlightBody(none));
}

void RotorLabFilingTest::_pilotNotice()
{
    // RotorLab 3.5.2 and later: pilot_named and pilot_note are read.
    const QString note = QStringLiteral("Filed with no pilot named.");
    QVERIFY(pilotNotice({ { QStringLiteral("pilot_named"), true } }, false, true).isEmpty());
    QCOMPARE(pilotNotice({ { QStringLiteral("pilot_named"), false },
                           { QStringLiteral("pilot_note"), note } }, false, true), note);
    QCOMPARE(pilotNotice({ { QStringLiteral("pilot_named"), false } }, false, true),
             noPilotFallback());
    QCOMPARE(pilotNotice({}, false, true), noPilotFallback());
    QVERIFY(pilotNotice({}, true, true).isEmpty());

    // Older or unknown server: both fields are ignored, and the sentence says
    // only what this station did.
    QCOMPARE(noPilotNeutral(), QStringLiteral("This station filed this flight with no pilot named."));
    QCOMPARE(pilotNotice({}, false, false), noPilotNeutral());
    QCOMPARE(pilotNotice({ { QStringLiteral("pilot_named"), false },
                           { QStringLiteral("pilot_note"), note } }, false, false),
             noPilotNeutral());
    QVERIFY(pilotNotice({ { QStringLiteral("pilot_note"), note } }, true, false).isEmpty());

    // Which flight: local takeoff time, the date only when not today.
    const QDateTime takeoff(QDate(2026, 11, 11), QTime(14, 5));
    const QString iso = takeoff.toString(Qt::ISODate);
    QCOMPARE(flightNoticePrefix(iso, QStringLiteral("Alpha 2"), takeoff.date()),
             QStringLiteral("14:05, Alpha 2"));
    QCOMPARE(flightNoticePrefix(iso, {}, takeoff.date().addDays(1)),
             QStringLiteral("11 Nov 14:05"));
    QVERIFY(flightNoticePrefix({}, {}, takeoff.date()).isEmpty());
}

void RotorLabFilingTest::_packSerials()
{
    QCOMPARE(parsePackSerials(QStringLiteral(" A1, B2  A1\nC3,,")),
             (QStringList{ QStringLiteral("A1"), QStringLiteral("B2"), QStringLiteral("C3") }));
    QVERIFY(parsePackSerials(QStringLiteral(" , ")).isEmpty());
    // Any Unicode space separates; format characters vanish.
    QCOMPARE(parsePackSerials(QStringLiteral(u"A1\u00A0B2\u2028C\u200B3")),
             (QStringList{ QStringLiteral("A1"), QStringLiteral("B2"), QStringLiteral("C3") }));
    QVERIFY(parsePackSerials(QString(65, QLatin1Char('x'))).isEmpty());
    QCOMPARE(parsePackSerials(QString(64, QLatin1Char('x'))).size(), 1);

    QStringList many;
    for (int i = 0; i < 20; ++i) {
        many << QStringLiteral("P%1").arg(i);
    }
    const QStringList kept = parsePackSerials(many.join(QLatin1Char(' ')));
    QCOMPARE(kept.size(), kPackSerialsMax);
    QCOMPARE(kept.first(), QStringLiteral("P0"));
}

void RotorLabFilingTest::_serverFlightTypeSet()
{
    // A56: the same ten codes as the server's FLIGHT_TYPES list and its
    // OpenAPI flight type enum, compared as a set; order is GC's own.
    const QStringList server = {
        QStringLiteral("commercial"), QStringLiteral("training"), QStringLiteral("proficiency"),
        QStringLiteral("maintenance"), QStringLiteral("survey"), QStringLiteral("inspection"),
        QStringLiteral("public_safety"), QStringLiteral("test"), QStringLiteral("recreational"),
        QStringLiteral("other") };
    const QStringList ours = flightTypeCodes();
    QCOMPARE(ours.size(), server.size());
    QCOMPARE(QSet<QString>(ours.cbegin(), ours.cend()),
             QSet<QString>(server.cbegin(), server.cend()));
}

void RotorLabFilingTest::_versionGate()
{
    QVERIFY(serverNamesPilots(QStringLiteral("3.5.2")));
    QVERIFY(serverNamesPilots(QStringLiteral("v3.5.2+abc123")));
    QVERIFY(serverNamesPilots(QStringLiteral("3.6")));
    QVERIFY(serverNamesPilots(QStringLiteral("4.0.0")));
    QVERIFY(serverNamesPilots(QStringLiteral("3.5.10")));
    QVERIFY(!serverNamesPilots(QStringLiteral("3.5.1")));
    QVERIFY(!serverNamesPilots(QStringLiteral("3.5")));
    QVERIFY(!serverNamesPilots(QStringLiteral("2.134.0")));
    QVERIFY(!serverNamesPilots({}));
    QVERIFY(!serverNamesPilots(QStringLiteral("unknown")));
}

void RotorLabFilingTest::_serverTextClip()
{
    QCOMPARE(clipServerText(QStringLiteral("  short  ")), QStringLiteral("short"));
    const QString exact(kServerTextMax, QLatin1Char('a'));
    QCOMPARE(clipServerText(exact), exact);
    const QString cut = clipServerText(QString(kServerTextMax + 50, QLatin1Char('a')));
    QCOMPARE(cut.toUcs4().size(), kServerTextMax);
    QVERIFY(cut.endsWith(QChar(0x2026)));
    // Code points, never half a surrogate pair.
    QString emoji;
    for (int i = 0; i < kServerTextMax + 10; ++i) {
        emoji += QStringLiteral(u"\U0001F681");
    }
    const QString cutEmoji = clipServerText(emoji);
    QCOMPARE(cutEmoji.toUcs4().size(), kServerTextMax);
    QVERIFY(cutEmoji.chopped(1).back().isLowSurrogate());
}

void RotorLabFilingTest::_flightTypeRequeue()
{
    // I1, with RotorLab 3.5.1's own refusals (flight_type, and the text
    // for other). The sentence is never matched: a parked
    // flight goes back only when its type is corrected after a 400.
    struct Parked {
        QJsonObject body;
        int         status;
        const char *sentence;
        bool        requeue;
    };
    const Parked cases[] = {
        // A word from an earlier GC build.
        { { { QStringLiteral("flight_type"), QStringLiteral("patrol") } }, 400,
          "What for must be one of commercial, training, proficiency, maintenance, survey, "
          "inspection, public safety, test, recreational, or Other with a description "
          "(got \"patrol\")", true },
        // other with no description.
        { { { QStringLiteral("flight_type"), QStringLiteral("other") } }, 400,
          "Say what the flight was for: Other needs a description", true },
        // A description with a listed type.
        { { { QStringLiteral("flight_type"), QStringLiteral("survey") },
            { QStringLiteral("flight_type_other"), QStringLiteral("north block") } }, 400,
          "A description goes with Other only: choose Other, or leave the description blank",
          true },
        // Saved by an earlier build, with no status.
        { { { QStringLiteral("flight_type"), QStringLiteral("mapping") } }, 0,
          "What for must be one of commercial, training, proficiency, maintenance, survey, "
          "inspection, public safety, test, recreational, or Other with a description "
          "(got \"mapping\")", true },
        // A 400 for another reason, type already good: stays parked.
        { { { QStringLiteral("flight_type"), QStringLiteral("survey") } }, 400,
          "The log does not record when it was flown. Enter the date flown.", false },
        // No type at all: stays parked.
        { { { QStringLiteral("site"), QStringLiteral("Northgate") } }, 400,
          "The log does not record when it was flown. Enter the date flown.", false },
        // Not a 400: stays parked even when the type changes.
        { { { QStringLiteral("flight_type"), QStringLiteral("patrol") } }, 404, "Not found", false },
        { { { QStringLiteral("flight_type"), QStringLiteral("patrol") } }, 413, "Too large", false },
    };
    for (const Parked &c : cases) {
        QJsonObject body = c.body;
        const bool changed = normalizeFlightBody(body);
        QVERIFY2(requeueAfterTypeFix(c.status, changed) == c.requeue, c.sentence);
    }
    QVERIFY(!requeueAfterTypeFix(400, false));
    QVERIFY(!requeueAfterTypeFix(0, false));
    QVERIFY(requeueAfterTypeFix(0, true));
}
