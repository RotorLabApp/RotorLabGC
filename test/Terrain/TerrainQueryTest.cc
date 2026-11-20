/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "TerrainQueryTest.h"
#include "TerrainTileManager.h"
#include "TerrainQuery.h"
#include "FlightPathSegment.h"
#include "TerrainProfile.h"
#ifdef RL_PLAN3D
#include "Plan3DColors.h"
#include "Plan3DMath.h"
#endif

#include <QtGui/QColor>
#include <QtTest/QTest>
#include <QtTest/QSignalSpy>

/// Point Nemo is a point on Earth furthest from land
static const QGeoCoordinate pointNemo = QGeoCoordinate(-48.875556, -123.392500);

const UnitTestTerrainQuery::Flat10Region UnitTestTerrainQuery::flat10Region{{
    pointNemo,
    QGeoCoordinate{
        pointNemo.latitude() - UnitTestTerrainQuery::regionSizeDeg,
        pointNemo.longitude() + UnitTestTerrainQuery::regionSizeDeg
    }
}};

const UnitTestTerrainQuery::LinearSlopeRegion UnitTestTerrainQuery::linearSlopeRegion{{
    flat10Region.topRight(),
    QGeoCoordinate{
        flat10Region.topRight().latitude() - UnitTestTerrainQuery::regionSizeDeg,
        flat10Region.topRight().longitude() + UnitTestTerrainQuery::regionSizeDeg
    }
}};

const UnitTestTerrainQuery::HillRegion UnitTestTerrainQuery::hillRegion{{
    linearSlopeRegion.topRight(),
    QGeoCoordinate{
        linearSlopeRegion.topRight().latitude() - UnitTestTerrainQuery::regionSizeDeg,
        linearSlopeRegion.topRight().longitude() + UnitTestTerrainQuery::regionSizeDeg
    }
}};

UnitTestTerrainQuery::UnitTestTerrainQuery(QObject *parent)
    : TerrainQueryInterface(parent)
{

}

UnitTestTerrainQuery::~UnitTestTerrainQuery()
{

}

void UnitTestTerrainQuery::requestCoordinateHeights(const QList<QGeoCoordinate> &coordinates)
{
    const QList<double> result = _requestCoordinateHeights(coordinates);
    emit coordinateHeightsReceived(result.size() == coordinates.size(), result);
}

void UnitTestTerrainQuery::requestPathHeights(const QGeoCoordinate &fromCoord, const QGeoCoordinate &toCoord)
{
    const PathHeightInfo_t pathHeightInfo = _requestPathHeights(fromCoord, toCoord);
    emit pathHeightsReceived(
        !pathHeightInfo.rgHeights.isEmpty(),
        pathHeightInfo.distanceBetween,
        pathHeightInfo.finalDistanceBetween,
        pathHeightInfo.rgHeights
    );
}

void UnitTestTerrainQuery::requestCarpetHeights(const QGeoCoordinate &swCoord, const QGeoCoordinate &neCoord, bool statsOnly)
{
    QList<QList<double>> carpet;

    if ((swCoord.longitude() > neCoord.longitude()) || (swCoord.latitude() > neCoord.latitude())) {
        emit carpetHeightsReceived(false, qQNaN(), qQNaN(), carpet);
        return;
    }

    double min = std::numeric_limits<double>::max();
    double max = std::numeric_limits<double>::min();
    for (double lat = swCoord.latitude(); lat < neCoord.latitude(); lat++) {
        const QGeoCoordinate fromCoord(lat, swCoord.longitude());
        const QGeoCoordinate toCoord(lat, neCoord.longitude());

        const QList<double> row = _requestPathHeights(fromCoord, toCoord).rgHeights;
        if (row.isEmpty()) {
            emit carpetHeightsReceived(false, qQNaN(), qQNaN(), QList<QList<double>>());
            return;
        }

        for (const double val : row) {
            min = qMin(val, min);
            max = qMax(val, max);
        }
        (void) carpet.append(row);
    }

    emit carpetHeightsReceived(true, min, max, carpet);
}

UnitTestTerrainQuery::PathHeightInfo_t UnitTestTerrainQuery::_requestPathHeights(const QGeoCoordinate &fromCoord, const QGeoCoordinate &toCoord)
{
    PathHeightInfo_t pathHeights;
    pathHeights.rgCoords = TerrainTileManager::_pathQueryToCoords(fromCoord, toCoord, pathHeights.distanceBetween, pathHeights.finalDistanceBetween);
    pathHeights.rgHeights = _requestCoordinateHeights(pathHeights.rgCoords);
    return pathHeights;
}

QList<double> UnitTestTerrainQuery::_requestCoordinateHeights(const QList<QGeoCoordinate> &coordinates)
{
    QList<double> result;

    for (const auto &coordinate : coordinates) {
        if (flat10Region.contains(coordinate)) {
            (void) result.append(UnitTestTerrainQuery::Flat10Region::amslElevation);
        } else if (linearSlopeRegion.contains(coordinate)) {
            // cast to oneSecondDeg grid and round to int to emulate SRTM1 even better
            const double x = (coordinate.longitude() - linearSlopeRegion.topLeft().longitude()) / oneSecondDeg;
            const double dx = regionSizeDeg / oneSecondDeg;
            const double fraction = x / dx;
            (void) result.append(std::round(UnitTestTerrainQuery::LinearSlopeRegion::minAMSLElevation + (fraction * UnitTestTerrainQuery::LinearSlopeRegion::totalElevationChange)));
        } else if (hillRegion.contains(coordinate)) {
            const double arcSecondMeters = (earthsRadiusMts * oneSecondDeg) * (M_PI / 180.);
            const double x = (coordinate.latitude() - hillRegion.center().latitude()) * arcSecondMeters / oneSecondDeg;
            const double y = (coordinate.longitude() - hillRegion.center().longitude()) * arcSecondMeters / oneSecondDeg;
            const double x2y2 = pow(x, 2) + pow(y, 2);
            const double r2 = pow(UnitTestTerrainQuery::HillRegion::radius, 2);
            const double z = (x2y2 <= r2) ? sqrt(r2 - x2y2) : UnitTestTerrainQuery::Flat10Region::amslElevation;
            (void) result.append(z);
        } else {
            result.clear();
            break;
        }
    }

    return result;
}

/*===========================================================================*/

void TerrainQueryTest::_testRequestCoordinateHeights()
{
    UnitTestTerrainQuery* const query = new UnitTestTerrainQuery(this);
    QSignalSpy spy(query, &UnitTestTerrainQuery::coordinateHeightsReceived);
    QVERIFY(spy.isValid());

    const QList<QGeoCoordinate> coords = {
        pointNemo
    };
    query->requestCoordinateHeights(coords);

    const QVariantList arguments = spy.takeFirst();
    QVERIFY(arguments.at(0).toBool() == true);
    QCOMPARE(arguments.at(1).toList().size(), coords.size());
    QVERIFY(arguments.at(1).toList().at(0).toDouble() == UnitTestTerrainQuery::Flat10Region::amslElevation);
}

void TerrainQueryTest::_testRequestPathHeights()
{
    UnitTestTerrainQuery* const query = new UnitTestTerrainQuery(this);
    QSignalSpy spy(query, &UnitTestTerrainQuery::pathHeightsReceived);
    QVERIFY(spy.isValid());

    const QGeoCoordinate from = pointNemo;
    const QGeoCoordinate to = QGeoCoordinate(pointNemo.latitude(), pointNemo.longitude() + UnitTestTerrainQuery::regionSizeDeg);
    query->requestPathHeights(from, to);

    const QVariantList arguments = spy.takeFirst();
    QVERIFY(arguments.at(0).toBool() == true);
    QVERIFY(arguments.at(1).toDouble() > 0.);
    QVERIFY(arguments.at(2).toDouble() > 0.);
    QVERIFY(arguments.at(3).toList().size() > 2);
    QVERIFY(arguments.at(3).toList().constFirst().toDouble() == UnitTestTerrainQuery::Flat10Region::amslElevation);
    QVERIFY(arguments.at(3).toList().constLast().toDouble() == UnitTestTerrainQuery::Flat10Region::amslElevation);
}

void TerrainQueryTest::_testRequestCarpetHeights()
{
    UnitTestTerrainQuery* const query = new UnitTestTerrainQuery(this);
    QSignalSpy spy(query, &UnitTestTerrainQuery::carpetHeightsReceived);
    QVERIFY(spy.isValid());

    const QGeoCoordinate sw = pointNemo;
    const QGeoCoordinate ne = QGeoCoordinate(pointNemo.latitude() + UnitTestTerrainQuery::regionSizeDeg, pointNemo.longitude() + UnitTestTerrainQuery::regionSizeDeg);
    query->requestCarpetHeights(sw, ne, false);

    const QVariantList arguments = spy.takeFirst();
    QVERIFY(arguments.at(0).toBool() == true);
    QVERIFY(arguments.at(1).toDouble() == UnitTestTerrainQuery::Flat10Region::amslElevation);
    QVERIFY(arguments.at(2).toDouble() == UnitTestTerrainQuery::Flat10Region::amslElevation);
    QVERIFY(!arguments.at(3).toList().isEmpty());
    QVERIFY(!arguments.at(3).toList().constFirst().toList().isEmpty());
    QVERIFY(arguments.at(3).toList().constFirst().toList().constFirst().toDouble() == UnitTestTerrainQuery::Flat10Region::amslElevation);
}

// Test Requires Internet, so disable by default.
// Or, check if internet and elevation server are available?
#if 0
void TerrainQueryTest::_testTerrainAtCoordinateQuery()
{
    const QGeoCoordinate coord{QRandomGenerator::global()->bounded(90.0), QRandomGenerator::global()->bounded(90.0), 0.0};
    QList<double> altitudes;
    QList<QGeoCoordinate> coordinates;
    (void) coordinates.append(pointNemo);
    (void) coordinates.append(coord);
    TerrainAtCoordinateQuery* query = new TerrainAtCoordinateQuery(true, this);
    query->requestData(coordinates);
    bool result = false;
    for (uint8_t attempts = 0; attempts < 20; attempts++) {
        bool error = false;
        const bool altAvailable = TerrainAtCoordinateQuery::getAltitudesForCoordinates(coordinates, altitudes, error);
        QVERIFY(!error);
        result |= altAvailable;
        if (result) {
            break;
        }
        QTest::qWait(500);
    }
    QVERIFY(result);
}
#endif

/*===========================================================================*/
// RotorLab GC G76: the 2D Plan view draws each leg in the terrain states of the 3D views
// (PLAN-CONTRACT.md section 4: a 2 m strike margin, CL-1a ground contact, CL-2 unknown).

namespace {

using Segment = FlightPathSegment;
using States = QList<FlightPathSegment::TerrainState>;

QList<double> constantList(int count, double value)
{
    return QList<double>(count, value);
}

// 11 samples every 10 m along a 100 m leg
constexpr int    kSamples   = 11;
constexpr double kStepM     = 10.0;
constexpr double kLegM      = 100.0;

States classify(Segment::SegmentType type, double alt1, double alt2, const QList<double>& ground)
{
    const QList<double> distances = Segment::sampleDistances(static_cast<int>(ground.count()), kStepM, kStepM);
    return Segment::classifySamples(type, alt1, alt2, kLegM, distances, ground);
}

} // namespace

void TerrainQueryTest::_testSegmentStateColors()
{
    // The 3D views' colors (custom/src/plan3d/Plan3DColors.h), the same in both themes except unknown and contact
    for (const bool dark: { true, false }) {
        QCOMPARE(Segment::terrainStateColor(Segment::TerrainStrike, dark), QColor(QStringLiteral("#ff5544")));
        QCOMPARE(Segment::terrainStateColor(Segment::TerrainNoStrike, dark), QColor(QStringLiteral("#2dd4bf")));
        QVERIFY(!Segment::terrainStateColor(Segment::TerrainNotChecked, dark).isValid());
    }
    QCOMPARE(Segment::terrainStateColor(Segment::TerrainUnknown, true), QColor(QStringLiteral("#b07cff")));
    QCOMPARE(Segment::terrainStateColor(Segment::TerrainUnknown, false), QColor(QStringLiteral("#7c3aed")));
    QCOMPARE(Segment::terrainStateColor(Segment::TerrainGroundContact, true), QColor(QStringLiteral("#94a3b8")));
    QCOMPARE(Segment::terrainStateColor(Segment::TerrainGroundContact, false), QColor(QStringLiteral("#64748b")));

    const double strikeMargin = Segment::strikeMarginM;
    const double contactBelow = Segment::contactBelowGroundM;
    QCOMPARE(strikeMargin, 2.0);
    QCOMPARE(contactBelow, 0.5);

    const QGeoCoordinate c2 = pointNemo.atDistanceAndAzimuth(kLegM, 90);
    Segment segment(Segment::SegmentTypeGeneric, pointNemo, 100, c2, 100, false /* queryTerrainData */, nullptr);
    QCOMPARE(segment.terrainStateColorName(Segment::TerrainUnknown, true), QStringLiteral("#b07cff"));
    QCOMPARE(segment.terrainStateColorName(Segment::TerrainStrike, false), QStringLiteral("#ff5544"));
    QVERIFY(segment.terrainStateColorName(Segment::TerrainNotChecked, true).isEmpty());
    QVERIFY(segment.terrainStateColorName(99, true).isEmpty());
    QVERIFY(segment.terrainStateColorName(-1, true).isEmpty());

#ifdef RL_PLAN3D
    // Same values as the 3D view, so the 2D and 3D views can never drift apart
    const QList<QPair<Segment::TerrainState, Plan3D::Clearance>> pairs = {
        { Segment::TerrainStrike,           Plan3D::Clearance::Strike },
        { Segment::TerrainNoStrike,         Plan3D::Clearance::NoStrike },
        { Segment::TerrainUnknown,          Plan3D::Clearance::Unknown },
        { Segment::TerrainGroundContact,    Plan3D::Clearance::GroundContact },
    };
    for (const auto& pair: pairs) {
        for (const bool dark: { true, false }) {
            QCOMPARE(Segment::terrainStateColor(pair.first, dark), Plan3D::clearanceColor(pair.second, dark));
        }
    }
    const double plan3DMargin = Plan3D::kStrikeMarginM;
    const double plan3DContact = Plan3D::kContactBelowGroundM;
    QCOMPARE(strikeMargin, plan3DMargin);
    QCOMPARE(contactBelow, plan3DContact);
#endif
}

void TerrainQueryTest::_testSegmentSampleDistances()
{
    QVERIFY(Segment::sampleDistances(0, 10, 7).isEmpty());
    QCOMPARE(Segment::sampleDistances(1, 10, 7), QList<double>({ 0 }));
    QCOMPARE(Segment::sampleDistances(2, 10, 7), QList<double>({ 0, 7 }));
    QCOMPARE(Segment::sampleDistances(5, 10, 7), QList<double>({ 0, 10, 20, 30, 37 }));
}

void TerrainQueryTest::_testSegmentStrikeMargin()
{
    const QList<double> flat10 = constantList(kSamples, 10);

    // Exactly 2 m above the ground is not a strike; less than 2 m is (decision 756, OQ7)
    QCOMPARE(Segment::legState(classify(Segment::SegmentTypeGeneric, 12.0, 12.0, flat10)), Segment::TerrainNoStrike);
    QCOMPARE(Segment::legState(classify(Segment::SegmentTypeGeneric, 11.99, 11.99, flat10)), Segment::TerrainStrike);
    QCOMPARE(Segment::legState(classify(Segment::SegmentTypeGeneric, 500, 500, flat10)), Segment::TerrainNoStrike);
    // Below the ground is always a strike
    QCOMPARE(Segment::legState(classify(Segment::SegmentTypeGeneric, 5, 5, flat10)), Segment::TerrainStrike);

    // A descent that ends 1 m above the ground: before G76 only terrain above the line counted, now the
    // last sample is a strike and the rest are not
    const States descent = classify(Segment::SegmentTypeGeneric, 50, 11, flat10);
    QCOMPARE(descent.count(), kSamples);
    QCOMPARE(descent.last(), Segment::TerrainStrike);
    QCOMPARE(descent.first(), Segment::TerrainNoStrike);
    QCOMPARE(Segment::legState(descent), Segment::TerrainStrike);
}

void TerrainQueryTest::_testSegmentUnknownGround()
{
    // No terrain data: unknown, never no strike (CL-2)
    QVERIFY(classify(Segment::SegmentTypeGeneric, 100, 100, {}).isEmpty());
    QCOMPARE(Segment::legState({}), Segment::TerrainUnknown);

    // A height that cannot be put in AMSL
    const States noHeight = classify(Segment::SegmentTypeGeneric, qQNaN(), 100, constantList(kSamples, 10));
    QCOMPARE(noHeight.count(Segment::TerrainUnknown), kSamples);
    QCOMPARE(Segment::legState(noHeight), Segment::TerrainUnknown);

    // One sample with no ground: the leg is unknown...
    QList<double> gap = constantList(kSamples, 10);
    gap[4] = qQNaN();
    const States gapStates = classify(Segment::SegmentTypeGeneric, 100, 100, gap);
    QCOMPARE(gapStates[4], Segment::TerrainUnknown);
    QCOMPARE(gapStates[3], Segment::TerrainNoStrike);
    QCOMPARE(Segment::legState(gapStates), Segment::TerrainUnknown);

    // ...unless a strike was found where the ground is known
    QList<double> gapAndRidge = gap;
    gapAndRidge[8] = 99;
    QCOMPARE(Segment::legState(classify(Segment::SegmentTypeGeneric, 100, 100, gapAndRidge)), Segment::TerrainStrike);
}

void TerrainQueryTest::_testSegmentContactSpan()
{
    const QList<double> flat10 = constantList(kSamples, 10);

    // Takeoff from the ground: the start is ground contact, not a strike (CL-1a)
    const States takeoff = classify(Segment::SegmentTypeTakeoff, 10, 60, flat10);
    QCOMPARE(takeoff.first(), Segment::TerrainGroundContact);
    QCOMPARE(takeoff[1], Segment::TerrainNoStrike);
    QCOMPARE(Segment::legState(takeoff), Segment::TerrainGroundContact);

    // The same line on a generic leg is a strike
    QCOMPARE(Segment::legState(classify(Segment::SegmentTypeGeneric, 10, 60, flat10)), Segment::TerrainStrike);

    // Down to 0.5 m below the ground is still contact; further below is a strike
    QCOMPARE(classify(Segment::SegmentTypeTakeoff, 9.5, 60, flat10).first(), Segment::TerrainGroundContact);
    QCOMPARE(classify(Segment::SegmentTypeTakeoff, 9.4, 60, flat10).first(), Segment::TerrainStrike);

    // Landing: the touchdown is ground contact
    const States land = classify(Segment::SegmentTypeLand, 60, 10, flat10);
    QCOMPARE(land.last(), Segment::TerrainGroundContact);
    QCOMPARE(land[kSamples - 2], Segment::TerrainNoStrike);
    QCOMPARE(Segment::legState(land), Segment::TerrainGroundContact);

    // A takeoff out of launch starts on the loaded ground there, whatever the plan's home altitude says (A-0,
    // A-11). The Boulder GUI plan had a home altitude 0.6 m or so below the 2D ground: its first sample was a strike.
    const QList<double> distances = Segment::sampleDistances(kSamples, kStepM, kStepM);
    QCOMPARE(Segment::classifySamples(Segment::SegmentTypeTakeoff, 9.0, 60, kLegM, distances, flat10, Segment::LaunchStartNone).first(), Segment::TerrainStrike);
    QCOMPARE(Segment::classifySamples(Segment::SegmentTypeTakeoff, 9.0, 60, kLegM, distances, flat10, Segment::LaunchStartOnGround).first(), Segment::TerrainGroundContact);
    // A stale home 16 m above the ground: the climb still starts on the ground
    const States staleHigh = Segment::classifySamples(Segment::SegmentTypeTakeoff, 26.0, 60, kLegM, distances, flat10, Segment::LaunchStartOnGround);
    QCOMPARE(staleHigh.first(), Segment::TerrainGroundContact);
    QCOMPARE(Segment::legState(staleHigh), Segment::TerrainGroundContact);
    // Without ground under the start the flag changes nothing
    QList<double> noStartGround = flat10;
    noStartGround.first() = qQNaN();
    QCOMPARE(Segment::classifySamples(Segment::SegmentTypeTakeoff, 9.0, 60, kLegM, distances, noStartGround, Segment::LaunchStartOnGround).first(), Segment::TerrainUnknown);

    // A dip below 2 m in the middle of a landing leg stays a strike: it is outside the contact span
    QList<double> ridge = flat10;
    ridge[5] = 34;  // the line is at 35 m there
    const States landRidge = classify(Segment::SegmentTypeLand, 60, 10, ridge);
    QCOMPARE(landRidge[5], Segment::TerrainStrike);
    QCOMPARE(landRidge.last(), Segment::TerrainGroundContact);
    QCOMPARE(Segment::legState(landRidge), Segment::TerrainStrike);
}

void TerrainQueryTest::_testSegmentTerrainFrame()
{
    // Ground rising 0 m to 100 m with a 300 m spike in the middle
    QList<double> ground;
    for (int i = 0; i < kSamples; i++) {
        ground.append(i * 10.0);
    }
    ground[5] = 300;

    // Terrain frame legs follow the ground (A-7): 30 m above it at both ends is 30 m above it all along.
    // Before G76 terrain frame legs were skipped.
    const States follow = classify(Segment::SegmentTypeTerrainFrame, 0 + 30, 100 + 30, ground);
    QCOMPARE(follow.count(Segment::TerrainNoStrike), kSamples);
    QCOMPARE(Segment::legState(follow), Segment::TerrainNoStrike);

    // The same ends on a straight leg meet the spike
    QCOMPARE(Segment::legState(classify(Segment::SegmentTypeGeneric, 30, 130, ground)), Segment::TerrainStrike);

    // 1 m above the ground at the far end: the height interpolates from 30 m to 1 m, the end is a strike
    const States low = classify(Segment::SegmentTypeTerrainFrame, 0 + 30, 100 + 1, ground);
    QCOMPARE(low.first(), Segment::TerrainNoStrike);
    QCOMPARE(low.last(), Segment::TerrainStrike);
    QCOMPARE(Segment::legState(low), Segment::TerrainStrike);

    // Unknown ground under a sample, or an end height that is not known, is unknown
    QList<double> gap = ground;
    gap[3] = qQNaN();
    QCOMPARE(Segment::legState(classify(Segment::SegmentTypeTerrainFrame, 30, 130, gap)), Segment::TerrainUnknown);
    QCOMPARE(Segment::legState(classify(Segment::SegmentTypeTerrainFrame, qQNaN(), 130, ground)), Segment::TerrainUnknown);
    QList<double> noEndGround = ground;
    noEndGround.last() = qQNaN();
    QCOMPARE(Segment::legState(classify(Segment::SegmentTypeTerrainFrame, 30, 130, noEndGround)), Segment::TerrainUnknown);
}

void TerrainQueryTest::_testSegmentRuns()
{
    // No samples: one unknown run over the whole leg
    QList<Segment::TerrainRun> runs = Segment::terrainRuns({}, {}, 250);
    QCOMPARE(runs.count(), 1);
    QCOMPARE(runs[0].state, Segment::TerrainUnknown);
    QCOMPARE(runs[0].startM, 0.0);
    QCOMPARE(runs[0].endM, 250.0);

    // Runs merge equal neighbors and change halfway between samples
    const States states = { Segment::TerrainNoStrike, Segment::TerrainNoStrike, Segment::TerrainUnknown, Segment::TerrainUnknown, Segment::TerrainStrike };
    runs = Segment::terrainRuns(states, { 0, 10, 20, 30, 40 }, 40);
    QCOMPARE(runs.count(), 3);
    QCOMPARE(runs[0].state, Segment::TerrainNoStrike);
    QCOMPARE(runs[0].startM, 0.0);
    QCOMPARE(runs[0].endM, 15.0);
    QCOMPARE(runs[1].state, Segment::TerrainUnknown);
    QCOMPARE(runs[1].startM, 15.0);
    QCOMPARE(runs[1].endM, 35.0);
    QCOMPARE(runs[2].state, Segment::TerrainStrike);
    QCOMPARE(runs[2].startM, 35.0);
    QCOMPARE(runs[2].endM, 40.0);

    // One sample covers the leg
    runs = Segment::terrainRuns({ Segment::TerrainNoStrike }, { 0 }, 80);
    QCOMPARE(runs.count(), 1);
    QCOMPARE(runs[0].endM, 80.0);
}

void TerrainQueryTest::_testSegmentMapPieces()
{
    QCOMPARE(Segment::dashPeriodM(0), 0.0);
    QCOMPARE(Segment::dashPeriodM(10), 2.5);            // at least 4 dashes
    QCOMPARE(Segment::dashPeriodM(400), 40.0);
    QCOMPARE(Segment::dashPeriodM(100000), 2000.0);     // at most 50 dashes

    const QGeoCoordinate c1 = pointNemo;
    const QGeoCoordinate c2 = c1.atDistanceAndAzimuth(400, 90);
    const double total = c1.distanceTo(c2);

    // Unknown ground is dashed: 10 dashes on a 400 m leg, all unknown
    QVariantList pieces = Segment::buildMapPieces(c1, c2, { { Segment::TerrainUnknown, 0, total } });
    QCOMPARE(pieces.count(), 10);
    for (const QVariant& piece: pieces) {
        const QVariantMap map = piece.toMap();
        QCOMPARE(map[QStringLiteral("state")].toInt(), static_cast<int>(Segment::TerrainUnknown));
        QVERIFY(map[QStringLiteral("dashed")].toBool());
        QCOMPARE(map[QStringLiteral("path")].toList().count(), 2);
    }
    QVERIFY(pieces.first().toMap()[QStringLiteral("path")].toList().first().value<QGeoCoordinate>() == c1);

    // Checked ground is one solid piece per run
    pieces = Segment::buildMapPieces(c1, c2, { { Segment::TerrainNoStrike, 0, total / 2 }, { Segment::TerrainUnknown, total / 2, total } });
    QCOMPARE(pieces.count(), 1 + 5);
    QVariantMap first = pieces.first().toMap();
    QCOMPARE(first[QStringLiteral("state")].toInt(), static_cast<int>(Segment::TerrainNoStrike));
    QVERIFY(!first[QStringLiteral("dashed")].toBool());

    // A strike run is one solid line plus tick marks across it, one per dash period (10 on 400 m)
    pieces = Segment::buildMapPieces(c1, c2, { { Segment::TerrainStrike, 0, total } });
    QCOMPARE(pieces.count(), 1 + 10);
    const QVariantMap strikeLine = pieces.first().toMap();
    QVERIFY(!strikeLine[QStringLiteral("tick")].toBool());
    QVERIFY(!strikeLine[QStringLiteral("dashed")].toBool());
    const QVariantList path = strikeLine[QStringLiteral("path")].toList();
    QVERIFY(path.first().value<QGeoCoordinate>() == c1);
    QVERIFY(path.last().value<QGeoCoordinate>() == c2);
    for (int i = 1; i < pieces.count(); i++) {
        const QVariantMap tick = pieces[i].toMap();
        QVERIFY(tick[QStringLiteral("tick")].toBool());
        QCOMPARE(tick[QStringLiteral("state")].toInt(), static_cast<int>(Segment::TerrainStrike));
        const QVariantList tickPath = tick[QStringLiteral("path")].toList();
        QCOMPARE(tickPath.count(), 2);
        // 10 m each side of the line (a quarter of the 40 m period), across it
        const QGeoCoordinate a = tickPath[0].value<QGeoCoordinate>();
        const QGeoCoordinate b = tickPath[1].value<QGeoCoordinate>();
        QVERIFY(qAbs(a.distanceTo(b) - 20.0) < 0.1);
        const double across = std::fmod(std::fabs(a.azimuthTo(b) - c1.azimuthTo(c2)), 180.0);
        QVERIFY(qAbs(across - 90.0) < 1.0);
    }
    // A strike run shorter than a period still gets one tick
    pieces = Segment::buildMapPieces(c1, c2, { { Segment::TerrainNoStrike, 0, 200 }, { Segment::TerrainStrike, 200, 210 }, { Segment::TerrainNoStrike, 210, total } });
    int ticks = 0;
    for (const QVariant& piece: pieces) {
        ticks += piece.toMap()[QStringLiteral("tick")].toBool() ? 1 : 0;
    }
    QCOMPARE(ticks, 1);
    // No other state gets ticks
    for (const QVariant& piece: Segment::buildMapPieces(c1, c2, { { Segment::TerrainGroundContact, 0, 100 }, { Segment::TerrainNoStrike, 100, total } })) {
        QVERIFY(!piece.toMap()[QStringLiteral("tick")].toBool());
    }

    // A long leg follows the great circle in pieces of at most 50 km
    const QGeoCoordinate far = c1.atDistanceAndAzimuth(120000, 45);
    pieces = Segment::buildMapPieces(c1, far, { { Segment::TerrainNoStrike, 0, c1.distanceTo(far) } });
    QCOMPARE(pieces.count(), 1);
    QCOMPARE(pieces.first().toMap()[QStringLiteral("path")].toList().count(), 4);

    // Nothing to draw without both ends
    QVERIFY(Segment::buildMapPieces(QGeoCoordinate(), c2, { { Segment::TerrainNoStrike, 0, 1 } }).isEmpty());
}

void TerrainQueryTest::_testSegmentNotChecked()
{
    // The Fly view makes no terrain query: its legs keep their look from before G76
    const QGeoCoordinate c2 = pointNemo.atDistanceAndAzimuth(kLegM, 90);
    Segment segment(Segment::SegmentTypeGeneric, pointNemo, 100, c2, 100, false /* queryTerrainData */, nullptr);
    QCOMPARE(segment.terrainState(), Segment::TerrainNotChecked);
    QVERIFY(!segment.terrainCollision());
    QCOMPARE(segment.mapPieces().count(), 1);
    QVariantMap piece = segment.mapPieces().first().toMap();
    QCOMPARE(piece[QStringLiteral("state")].toInt(), static_cast<int>(Segment::TerrainNotChecked));
    QVERIFY(!piece[QStringLiteral("dashed")].toBool());

    QSignalSpy piecesSpy(&segment, &Segment::mapPiecesChanged);
    const QGeoCoordinate c3 = pointNemo.atDistanceAndAzimuth(2 * kLegM, 0);
    segment.setCoordinate2(c3);
    QCOMPARE(segment.terrainState(), Segment::TerrainNotChecked);
    QCOMPARE(piecesSpy.count(), 1);
    piece = segment.mapPieces().first().toMap();
    QVERIFY(piece[QStringLiteral("path")].toList().last().value<QGeoCoordinate>() == c3);
}

void TerrainQueryTest::_testSegmentMergeShortRuns()
{
    using Runs = QList<Segment::TerrainRun>;
    auto check = [](const Runs& runs, double start, double end) {
        // Contiguous, no empty run, no two equal neighbors, same span
        QVERIFY(!runs.isEmpty());
        QCOMPARE(runs.first().startM, start);
        QCOMPARE(runs.last().endM, end);
        for (int i = 0; i < runs.count(); i++) {
            QVERIFY(runs[i].endM > runs[i].startM);
            if (i > 0) {
                QCOMPARE(runs[i].startM, runs[i - 1].endM);
                QVERIFY(runs[i].state != runs[i - 1].state);
            }
        }
    };

    // Long runs are left alone
    Runs runs = Segment::mergeShortRuns({ { Segment::TerrainNoStrike, 0, 100 }, { Segment::TerrainStrike, 100, 300 }, { Segment::TerrainNoStrike, 300, 400 } }, 40);
    QCOMPARE(runs.count(), 3);
    QCOMPARE(runs[1].startM, 100.0);
    QCOMPARE(runs[1].endM, 300.0);

    // Near a strike edge: strike, a short teal gap, strike, a short teal gap, strike. The gaps join the strike, so
    // the line never alternates red and teal in a pattern that reads as dashes.
    runs = Segment::mergeShortRuns({ { Segment::TerrainNoStrike, 0, 100 }, { Segment::TerrainStrike, 100, 110 }, { Segment::TerrainNoStrike, 110, 115 },
                                     { Segment::TerrainStrike, 115, 125 }, { Segment::TerrainNoStrike, 125, 130 }, { Segment::TerrainStrike, 130, 140 },
                                     { Segment::TerrainNoStrike, 140, 400 } }, 40);
    check(runs, 0, 400);
    QCOMPARE(runs.count(), 3);
    QCOMPARE(runs[1].state, Segment::TerrainStrike);
    QVERIFY(runs[1].startM <= 100.0);    // the strike only grows
    QVERIFY(runs[1].endM >= 140.0);
    QVERIFY(runs[1].endM - runs[1].startM >= 40.0 - 1e-9);

    // A lone short strike is widened to the minimum, never dropped
    runs = Segment::mergeShortRuns({ { Segment::TerrainNoStrike, 0, 200 }, { Segment::TerrainStrike, 200, 205 }, { Segment::TerrainNoStrike, 205, 400 } }, 40);
    check(runs, 0, 400);
    QCOMPARE(runs.count(), 3);
    QCOMPARE(runs[1].state, Segment::TerrainStrike);
    QVERIFY(qAbs((runs[1].endM - runs[1].startM) - 40.0) < 1e-6);
    QVERIFY(runs[1].startM <= 200.0 && runs[1].endM >= 205.0);

    // A short strike at the end of a leg widens into the leg
    runs = Segment::mergeShortRuns({ { Segment::TerrainNoStrike, 0, 395 }, { Segment::TerrainStrike, 395, 400 } }, 40);
    check(runs, 0, 400);
    QCOMPARE(runs.last().state, Segment::TerrainStrike);
    QVERIFY(qAbs(runs.last().startM - 360.0) < 1e-6);

    // A short teal run between unknown runs becomes unknown (unknown outranks no strike)
    runs = Segment::mergeShortRuns({ { Segment::TerrainUnknown, 0, 200 }, { Segment::TerrainNoStrike, 200, 210 }, { Segment::TerrainUnknown, 210, 400 } }, 40);
    QCOMPARE(runs.count(), 1);
    QCOMPARE(runs[0].state, Segment::TerrainUnknown);

    // A short ground contact run at the start of a takeoff next to no strike is kept (contact outranks no strike)
    runs = Segment::mergeShortRuns({ { Segment::TerrainGroundContact, 0, 10 }, { Segment::TerrainNoStrike, 10, 400 } }, 40);
    check(runs, 0, 400);
    QCOMPARE(runs.first().state, Segment::TerrainGroundContact);
    QVERIFY(qAbs(runs.first().endM - 40.0) < 1e-6);

    // A leg shorter than the minimum is one run in its highest state
    runs = Segment::mergeShortRuns({ { Segment::TerrainNoStrike, 0, 10 }, { Segment::TerrainStrike, 10, 12 }, { Segment::TerrainUnknown, 12, 20 } }, 40);
    QCOMPARE(runs.count(), 1);
    QCOMPARE(runs[0].state, Segment::TerrainStrike);
    QCOMPARE(runs[0].endM, 20.0);

    // Priorities: strike, unknown, contact, no strike
    QVERIFY(Segment::statePriority(Segment::TerrainStrike) > Segment::statePriority(Segment::TerrainUnknown));
    QVERIFY(Segment::statePriority(Segment::TerrainUnknown) > Segment::statePriority(Segment::TerrainGroundContact));
    QVERIFY(Segment::statePriority(Segment::TerrainGroundContact) > Segment::statePriority(Segment::TerrainNoStrike));
}

void TerrainQueryTest::_testTerrainProfileLegStates()
{
    // Legs with no terrain query (Fly view, rover) are not counted; nothing else is a strike or unknown
    const QGeoCoordinate c2 = pointNemo.atDistanceAndAzimuth(kLegM, 90);
    Segment notChecked(Segment::SegmentTypeGeneric, pointNemo, 100, c2, 100, false /* queryTerrainData */, nullptr);
    TerrainProfile::LegStateCounts counts = TerrainProfile::countLegStates({ &notChecked, nullptr });
    QCOMPARE(counts.strike, 0);
    QCOMPARE(counts.unknown, 0);
    QCOMPARE(counts.noStrike, 0);

    // The state line uses the map's colors
    TerrainProfile profile;
    QCOMPARE(profile.terrainStateColorName(Segment::TerrainStrike, true), QStringLiteral("#ff5544"));
    QCOMPARE(profile.terrainStateColorName(Segment::TerrainUnknown, false), QStringLiteral("#7c3aed"));
    QCOMPARE(profile.terrainStateColorName(Segment::TerrainNoStrike, true), QStringLiteral("#2dd4bf"));
    QVERIFY(profile.terrainStateColorName(Segment::TerrainNotChecked, true).isEmpty());
    QVERIFY(profile.terrainStateColorName(42, true).isEmpty());
    QCOMPARE(profile.strikeLegCount(), 0);
    QCOMPARE(profile.unknownLegCount(), 0);
}

void TerrainQueryTest::_testSegmentItemPoint()
{
    // A-9: a waypoint less than 2 m above the ground, or below it, is a strike, the same margin as its legs
    QCOMPARE(Segment::itemTerrainState(112.0, 110.0, false), Segment::TerrainNoStrike);
    QCOMPARE(Segment::itemTerrainState(111.9, 110.0, false), Segment::TerrainStrike);
    QCOMPARE(Segment::itemTerrainState(100.0, 110.0, false), Segment::TerrainStrike);
    // Before G76 only a point below the ground was a collision; 1 m above it is now a strike
    QCOMPARE(Segment::itemTerrainState(111.0, 110.0, false), Segment::TerrainStrike);

    // CL-1a: a takeoff or landing point may touch the ground, down to 0.5 m below it
    QCOMPARE(Segment::itemTerrainState(110.0, 110.0, true), Segment::TerrainGroundContact);
    QCOMPARE(Segment::itemTerrainState(109.5, 110.0, true), Segment::TerrainGroundContact);
    QCOMPARE(Segment::itemTerrainState(109.4, 110.0, true), Segment::TerrainStrike);
    QCOMPARE(Segment::itemTerrainState(140.0, 110.0, true), Segment::TerrainNoStrike);

    // Unknown ground or an unknown height is never a strike or no strike
    QCOMPARE(Segment::itemTerrainState(100.0, qQNaN(), false), Segment::TerrainUnknown);
    QCOMPARE(Segment::itemTerrainState(qQNaN(), 110.0, true), Segment::TerrainUnknown);
}

namespace {

// g76-boulder-mixed.plan, leg from the planned home (39.999, -105.281) to waypoint 2 (40.0, -105.265): the 47
// Copernicus heights GC loaded for it in the da510a9 GUI run (the path query's 30 m walk over the cached tiles),
// with the query's spacing. The plan's home altitude H is 1745.0 m, 16 to 17 m above this ground (1728 m here,
// 1729.1 m in the 3D view): a stale home. The ground rises 3 m in the first 30 m.
const QList<double> kBoulderLegHeights = {
    1728, 1731, 1730, 1730, 1730, 1728, 1726, 1724, 1723, 1721, 1720, 1717, 1715, 1713, 1710, 1707,
    1705, 1704, 1699, 1696, 1696, 1693, 1691, 1687, 1685, 1680, 1678, 1676, 1675, 1673, 1671, 1669,
    1669, 1668, 1666, 1663, 1662, 1661, 1660, 1659, 1656, 1656, 1656, 1654, 1653, 1652, 1651,
};
constexpr double kBoulderBetweenM   = 29.72682331610165;
constexpr double kBoulderFinalM     = 29.726400256073703;
constexpr double kBoulderLegM       = 1367.42414034476;
constexpr double kBoulderHomeAMSL   = 1745.0;               // H
constexpr double kBoulderTakeoffRel = 40.0;                 // the takeoff (command 22, lat/lon 0), above launch
constexpr double kBoulderWp2Rel     = 60.0;                 // waypoint 2, above launch

} // namespace

void TerrainQueryTest::_testSegmentBoulderLaunchClimb()
{
    QCOMPARE(kBoulderLegHeights.count(), 47);
    const QList<double> distances = Segment::sampleDistances(static_cast<int>(kBoulderLegHeights.count()), kBoulderBetweenM, kBoulderFinalM);
    const double wp2AMSL        = kBoulderHomeAMSL + kBoulderWp2Rel;
    const double takeoffAMSL    = kBoulderHomeAMSL + kBoulderTakeoffRel;

    // The cause, as drawn by c691fe7: the leg was a slant from the loaded ground at launch (1728 m) to waypoint 2.
    // 30 m out the line is at 1729.7 m and the ground at 1731 m: 1.3 m below the ground, inside the contact span
    // but more than 0.5 m below it, so a strike. The map drew the start red with one tick, then slate, then teal,
    // and the profile, which colors a whole leg, drew all of it red.
    const States slant = Segment::classifySamples(Segment::SegmentTypeTakeoff, kBoulderHomeAMSL, wp2AMSL, kBoulderLegM, distances,
                                                  kBoulderLegHeights, Segment::LaunchStartOnGround);
    QCOMPARE(slant[0], Segment::TerrainGroundContact);
    QCOMPARE(slant[1], Segment::TerrainStrike);
    QCOMPARE(Segment::legState(slant), Segment::TerrainStrike);

    // A-11: the takeoff has no position, so it climbs straight up at launch to its own height (H + 40 m in GC's
    // datum) and the leg to waypoint 2 starts there. The first sample is the foot of the climb: ground contact.
    const States climb = Segment::classifySamples(Segment::SegmentTypeTakeoff, takeoffAMSL, wp2AMSL, kBoulderLegM, distances,
                                                  kBoulderLegHeights, Segment::LaunchStartClimbAtLaunch);
    QCOMPARE(climb.count(), kBoulderLegHeights.count());
    QCOMPARE(climb[0], Segment::TerrainGroundContact);
    QCOMPARE(climb.count(Segment::TerrainStrike), 0);
    QCOMPARE(climb.count(Segment::TerrainNoStrike), kBoulderLegHeights.count() - 1);
    QCOMPARE(Segment::legState(climb), Segment::TerrainGroundContact);

    // The same with the ground at home as the launch reference (A-0, as the 3D view draws it): no strike either
    const States climbFromGround = Segment::classifySamples(Segment::SegmentTypeTakeoff, kBoulderLegHeights.first() + kBoulderTakeoffRel,
                                                            kBoulderLegHeights.first() + kBoulderWp2Rel, kBoulderLegM, distances,
                                                            kBoulderLegHeights, Segment::LaunchStartClimbAtLaunch);
    QCOMPARE(climbFromGround[0], Segment::TerrainGroundContact);
    QCOMPARE(climbFromGround.count(Segment::TerrainStrike), 0);

    // The drawn segment: the map starts in the contact color and has no strike piece, and the leg is not a
    // terrain collision, so the profile does not draw it red
    const QGeoCoordinate home(39.999, -105.281);
    const QGeoCoordinate wp2(40.0, -105.265);
    Segment segment(Segment::SegmentTypeTakeoff, home, takeoffAMSL, wp2, wp2AMSL, true /* queryTerrainData */, nullptr);
    segment.setLaunchStart(Segment::LaunchStartClimbAtLaunch);
    segment.applyTerrainDataForTest(kBoulderLegHeights, kBoulderBetweenM, kBoulderFinalM);
    QCOMPARE(segment.terrainState(), Segment::TerrainGroundContact);
    QVERIFY(!segment.terrainCollision());
    QVERIFY(!segment.mapPieces().isEmpty());
    QCOMPARE(segment.mapPieces().first().toMap()[QStringLiteral("state")].toInt(), static_cast<int>(Segment::TerrainGroundContact));
    QVERIFY(segment.mapPieces().first().toMap()[QStringLiteral("path")].toList().first().value<QGeoCoordinate>() == home);
    for (const QVariant& piece: segment.mapPieces()) {
        QVERIFY(piece.toMap()[QStringLiteral("state")].toInt() != static_cast<int>(Segment::TerrainStrike));
    }
    // c691fe7's shape on the same segment: a strike, which the profile draws red over the whole leg
    segment.setCoord1AMSLAlt(kBoulderHomeAMSL);
    segment.setLaunchStart(Segment::LaunchStartOnGround);
    QCOMPARE(segment.terrainState(), Segment::TerrainStrike);
    QVERIFY(segment.terrainCollision());

    // Edge cases of the climb at launch: an unknown takeoff height or ground stays unknown (never guessed), and a
    // climb whose top is more than 0.5 m below the ground at launch is a strike
    QCOMPARE(Segment::classifySamples(Segment::SegmentTypeTakeoff, qQNaN(), wp2AMSL, kBoulderLegM, distances,
                                      kBoulderLegHeights, Segment::LaunchStartClimbAtLaunch).first(), Segment::TerrainUnknown);
    QList<double> noGroundAtLaunch = kBoulderLegHeights;
    noGroundAtLaunch.first() = qQNaN();
    QCOMPARE(Segment::classifySamples(Segment::SegmentTypeTakeoff, takeoffAMSL, wp2AMSL, kBoulderLegM, distances,
                                      noGroundAtLaunch, Segment::LaunchStartClimbAtLaunch).first(), Segment::TerrainUnknown);
    QCOMPARE(Segment::classifySamples(Segment::SegmentTypeTakeoff, 1727.0, wp2AMSL, kBoulderLegM, distances,
                                      kBoulderLegHeights, Segment::LaunchStartClimbAtLaunch).first(), Segment::TerrainStrike);
    // Only a takeoff leg climbs at launch
    QCOMPARE(Segment::classifySamples(Segment::SegmentTypeGeneric, 1729.0, wp2AMSL, kBoulderLegM, distances,
                                      kBoulderLegHeights, Segment::LaunchStartClimbAtLaunch).first(), Segment::TerrainStrike);
}

void TerrainQueryTest::_testSegmentTerrainRetry()
{
    // 2, 5, 15, then 30 s, and no more after maxTerrainRetries
    QCOMPARE(Segment::terrainRetryDelayMs(0), 0);
    QCOMPARE(Segment::terrainRetryDelayMs(1), 2000);
    QCOMPARE(Segment::terrainRetryDelayMs(2), 5000);
    QCOMPARE(Segment::terrainRetryDelayMs(3), 15000);
    QCOMPARE(Segment::terrainRetryDelayMs(4), 30000);
    QCOMPARE(Segment::terrainRetryDelayMs(Segment::maxTerrainRetries), 30000);
    QCOMPARE(Segment::terrainRetryDelayMs(Segment::maxTerrainRetries + 1), 0);

    // No event loop runs in this test, so no real terrain answer can arrive between the steps
    const QGeoCoordinate c2 = pointNemo.atDistanceAndAzimuth(kLegM, 90);
    Segment segment(Segment::SegmentTypeGeneric, pointNemo, 100, c2, 100, true /* queryTerrainData */, nullptr);
    const QList<double> flat10 = constantList(kSamples, 10);
    // Start from a known answer, whatever the constructor's real query did
    segment.applyTerrainDataForTest(flat10, kStepM, kStepM);
    QCOMPARE(segment.terrainRetryCount(), 0);
    QVERIFY(!segment.terrainRetryPending());

    // A failed query: the leg stays unknown and a retry is scheduled in 2 s
    segment.applyTerrainFailureForTest();
    QCOMPARE(segment.terrainState(), Segment::TerrainUnknown);
    QVERIFY(!segment.terrainCollision());
    QVERIFY(segment.terrainRetryPending());
    QCOMPARE(segment.terrainRetryIntervalMs(), 2000);
    QCOMPARE(segment.terrainRetryCount(), 1);

    // The retry sends a new query; when that one fails too, the next retry waits longer
    segment.fireTerrainRetryForTest();
    QVERIFY(!segment.terrainRetryPending());
    segment.applyTerrainFailureForTest();
    QCOMPARE(segment.terrainRetryIntervalMs(), 5000);
    QCOMPARE(segment.terrainRetryCount(), 2);

    // A query that never answers is dropped after terrainQueryTimeoutMs and counted as failed
    segment.fireTerrainRetryForTest();
    if (segment.terrainQueryPending()) {
        segment.expireTerrainQueryForTest();
        QVERIFY(!segment.terrainQueryPending());
    } else {
        segment.applyTerrainFailureForTest();   // the tile manager answered at once
    }
    QCOMPARE(segment.terrainState(), Segment::TerrainUnknown);
    QVERIFY(segment.terrainRetryPending());
    QCOMPARE(segment.terrainRetryIntervalMs(), 15000);
    QCOMPARE(segment.terrainRetryCount(), 3);

    // The ground arrives: the leg is checked and the retries stop
    segment.applyTerrainDataForTest(flat10, kStepM, kStepM);
    QCOMPARE(segment.terrainState(), Segment::TerrainNoStrike);
    QVERIFY(!segment.terrainRetryPending());
    QCOMPARE(segment.terrainRetryCount(), 0);

    // The retries are bounded: after maxTerrainRetries failed retries the leg stays unknown with no more retry
    for (int attempt = 1; attempt <= Segment::maxTerrainRetries; attempt++) {
        segment.applyTerrainFailureForTest();
        QVERIFY(segment.terrainRetryPending());
        QCOMPARE(segment.terrainRetryIntervalMs(), Segment::terrainRetryDelayMs(attempt));
        segment.fireTerrainRetryForTest();
    }
    segment.applyTerrainFailureForTest();
    QVERIFY(!segment.terrainRetryPending());
    QCOMPARE(segment.terrainState(), Segment::TerrainUnknown);

    // Moving the leg starts over: a new query and a fresh set of retries
    segment.setCoordinate2(pointNemo.atDistanceAndAzimuth(2 * kLegM, 90));
    QCOMPARE(segment.terrainRetryCount(), 0);
    segment.applyTerrainFailureForTest();
    QVERIFY(segment.terrainRetryPending());
    QCOMPARE(segment.terrainRetryIntervalMs(), 2000);

    // The Fly view makes no query, so it never retries
    Segment notChecked(Segment::SegmentTypeGeneric, pointNemo, 100, c2, 100, false /* queryTerrainData */, nullptr);
    notChecked.applyTerrainFailureForTest();
    QVERIFY(!notChecked.terrainRetryPending());
    QCOMPARE(notChecked.terrainState(), Segment::TerrainNotChecked);
}

void TerrainQueryTest::_testSegmentRtlUnknown()
{
    // The leg back to launch of an RTL, as GC drew it before: from the last item down to the home altitude on
    // the ground at launch, which is a strike at its end
    const QGeoCoordinate c2 = pointNemo.atDistanceAndAzimuth(kLegM, 90);
    const QList<double> flat10 = constantList(kSamples, 10);
    Segment segment(Segment::SegmentTypeGeneric, pointNemo, 50, c2, 10, true /* queryTerrainData */, nullptr);
    segment.applyTerrainDataForTest(flat10, kStepM, kStepM);
    QCOMPARE(segment.terrainState(), Segment::TerrainStrike);
    QVERIFY(segment.terrainCollision());

    // A-10, CL-2: its height is the aircraft's RTL setting, not in the plan, so it is unknown as in the 3D view:
    // violet and dashed, never red and never counted as a strike
    QSignalSpy stateSpy(&segment, &Segment::terrainStateChanged);
    segment.setHeightNotInPlan(true);
    QVERIFY(segment.heightNotInPlan());
    QCOMPARE(stateSpy.count(), 1);
    QCOMPARE(segment.terrainState(), Segment::TerrainUnknown);
    QVERIFY(!segment.terrainCollision());
    QVERIFY(!segment.mapPieces().isEmpty());
    for (const QVariant& piece: segment.mapPieces()) {
        const QVariantMap map = piece.toMap();
        QCOMPARE(map[QStringLiteral("state")].toInt(), static_cast<int>(Segment::TerrainUnknown));
        QVERIFY(map[QStringLiteral("dashed")].toBool());
        QVERIFY(!map[QStringLiteral("tick")].toBool());
    }
    TerrainProfile::LegStateCounts counts = TerrainProfile::countLegStates({ &segment });
    QCOMPARE(counts.unknown, 1);
    QCOMPARE(counts.strike, 0);

    // Also unknown before any terrain has loaded, and unknown whatever its heights say
    Segment noTerrain(Segment::SegmentTypeGeneric, pointNemo, 500, c2, 500, true /* queryTerrainData */, nullptr);
    noTerrain.setHeightNotInPlan(true);
    QCOMPARE(noTerrain.terrainState(), Segment::TerrainUnknown);
    noTerrain.applyTerrainDataForTest(flat10, kStepM, kStepM);
    QCOMPARE(noTerrain.terrainState(), Segment::TerrainUnknown);

    // The profile's distance axis covers every drawn leg, never less than the mission total
    QCOMPARE(TerrainProfile::profileDistanceFor(1200.0, 1000.0), 1200.0);
    QCOMPARE(TerrainProfile::profileDistanceFor(900.0, 1000.0), 1000.0);
    QCOMPARE(TerrainProfile::profileDistanceFor(900.0, qQNaN()), 900.0);
}
