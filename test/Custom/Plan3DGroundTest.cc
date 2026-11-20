#include "Plan3DGroundTest.h"

#include "plan3d/Plan3DColors.h"
#include "plan3d/Plan3DGround.h"
#include "plan3d/Plan3DGroundMesh.h"

#include <QtTest/QTest>

#include <cmath>

using namespace Plan3D;

namespace {

// The wp300 corpus plan's area (Golden Gate Park), as Builder::bounds gives it
// with its 150 m margin, rounded.
GeoBounds wp300Area()
{
    GeoBounds b;
    b.south = 37.7652;
    b.north = 37.7710;
    b.west = -122.4951;
    b.east = -122.4845;
    return b;
}

GroundGrid plane(const GeoBounds &b, int cols, int rows)
{
    GroundGrid g = GroundGrid::layout(b, 1e9);
    g.cols = cols;
    g.rows = rows;
    g.heights.resize(cols * rows);
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            g.heights[r * cols + c] = 100.0 + 2.0 * c + 3.0 * r;
        }
    }
    return g;
}

} // namespace

void Plan3DGroundTest::_gridLayout()
{
    const GeoBounds b = wp300Area();
    const GroundGrid g = GroundGrid::layout(b);
    QVERIFY(g.valid());
    // About 935 m east-west and 645 m north-south at 30 m spacing.
    QVERIFY(g.cols >= 31 && g.cols <= 34);
    QVERIFY(g.rows >= 21 && g.rows <= 24);
    QCOMPARE(g.heights.size(), g.cols * g.rows);
    QCOMPARE(g.knownCount(), 0);
    QCOMPARE(g.nodeLat(0), b.south);
    QCOMPARE(g.nodeLat(g.rows - 1), b.north);
    QCOMPARE(g.nodeLon(0), b.west);
    QCOMPARE(g.nodeLon(g.cols - 1), b.east);
    QVERIFY(g.sameArea(b));
    GeoBounds moved = b;
    moved.east += 0.001;
    QVERIFY(!g.sameArea(moved));

    // A large area is capped; a tiny one has at least 2 x 2 nodes.
    GeoBounds big;
    big.south = 37.0; big.north = 38.0; big.west = -123.0; big.east = -122.0;
    const GroundGrid gb = GroundGrid::layout(big);
    QCOMPARE(gb.cols, GroundGrid::kMaxNodesPerSide);
    QCOMPARE(gb.rows, GroundGrid::kMaxNodesPerSide);
    GeoBounds tiny;
    tiny.south = 37.0; tiny.north = 37.00001; tiny.west = -122.0; tiny.east = -121.99999;
    const GroundGrid gt = GroundGrid::layout(tiny);
    QCOMPARE(gt.cols, 2);
    QCOMPARE(gt.rows, 2);

    QVERIFY(!GroundGrid::layout(GeoBounds()).valid());
}

void Plan3DGroundTest::_elevationBilinear()
{
    const GeoBounds b = wp300Area();
    const GroundGrid g = plane(b, 5, 4);
    // Nodes exactly.
    for (int r = 0; r < g.rows; r++) {
        for (int c = 0; c < g.cols; c++) {
            QVERIFY(std::abs(g.elevation(g.nodeLat(r), g.nodeLon(c)) - (100.0 + 2.0 * c + 3.0 * r)) < 1e-6);
        }
    }
    // A plane is reproduced between nodes, and on the north and east edges.
    const double lat = (g.nodeLat(1) + g.nodeLat(2)) / 2, lon = (g.nodeLon(2) + g.nodeLon(3)) / 2;
    QVERIFY(std::abs(g.elevation(lat, lon) - (100.0 + 2.0 * 2.5 + 3.0 * 1.5)) < 1e-6);
    QVERIFY(std::abs(g.elevation(b.north, b.east) - (100.0 + 2.0 * 4 + 3.0 * 3)) < 1e-6);
    // Outside the area, or a NaN position: unknown.
    QVERIFY(std::isnan(g.elevation(b.south - 1e-4, lon)));
    QVERIFY(std::isnan(g.elevation(lat, b.east + 1e-4)));
    QVERIFY(std::isnan(g.elevation(kNaN, lon)));
    QVERIFY(std::isnan(GroundGrid().elevation(lat, lon)));
}

void Plan3DGroundTest::_unknownIsNaN()
{
    const GeoBounds b = wp300Area();
    GroundGrid g = plane(b, 5, 4);
    QCOMPARE(g.knownCount(), 20);
    // Node (2, 1) unknown: the four cells around it are unknown, never level
    // with launch or with a neighbour (A-3); the rest stays known.
    g.heights[1 * g.cols + 2] = kNaN;
    QCOMPARE(g.knownCount(), 19);
    auto mid = [&](int c, int r) { return g.elevation((g.nodeLat(r) + g.nodeLat(r + 1)) / 2, (g.nodeLon(c) + g.nodeLon(c + 1)) / 2); };
    QVERIFY(std::isnan(mid(1, 0)));
    QVERIFY(std::isnan(mid(2, 0)));
    QVERIFY(std::isnan(mid(1, 1)));
    QVERIFY(std::isnan(mid(2, 1)));
    QVERIFY(std::isnan(g.elevation(g.nodeLat(1), g.nodeLon(2))));
    QVERIFY(std::isfinite(mid(0, 2)));
    QVERIFY(std::isfinite(mid(3, 2)));
    QVERIFY(std::isfinite(mid(0, 0)));
}

void Plan3DGroundTest::_providerChangeReloads()
{
    const GeoBounds b = wp300Area();
    const QString cop = QStringLiteral("Copernicus");
    const QString other = QStringLiteral("OtherElevation");
    GroundGrid loaded = plane(b, 5, 4);

    // Same area, same provider: the loaded heights are kept.
    QVERIFY(!Ground::needsReload(loaded, cop, b, cop));
    // Same area, another provider: the old provider's heights are dropped.
    QVERIFY(Ground::needsReload(loaded, cop, b, other));
    QVERIFY(Ground::needsReload(loaded, other, b, cop));
    // Nothing loaded yet (no provider recorded): load.
    QVERIFY(Ground::needsReload(GroundGrid(), QString(), b, cop));
    QVERIFY(Ground::needsReload(loaded, QString(), b, cop));
    // Another area: reload whatever the provider.
    GeoBounds moved = b;
    moved.north += 0.001;
    QVERIFY(Ground::needsReload(loaded, cop, moved, cop));
    // An invalid area never replaces what is loaded.
    QVERIFY(!Ground::needsReload(loaded, cop, GeoBounds(), other));
}

void Plan3DGroundTest::_groupByTile()
{
    const double tileDeg = 0.01;
    const GroundGrid g = GroundGrid::layout(wp300Area());
    const QList<GroundTile> tiles = groupByTile(g, tileDeg);
    // The area crosses 37.77 N and -122.49 E: four terrain tiles.
    QCOMPARE(tiles.size(), 4);
    QVector<int> seen(g.cols * g.rows, 0);
    for (const GroundTile &t : tiles) {
        QVERIFY(!t.nodes.isEmpty());
        const qint64 tx = t.key & 0xffffffffLL, ty = t.key >> 32;
        for (int n : t.nodes) {
            seen[n]++;
            QCOMPARE(qint64(std::floor((g.nodeLon(n % g.cols) + 180.0) / tileDeg)), tx);
            QCOMPARE(qint64(std::floor((g.nodeLat(n / g.cols) + 90.0) / tileDeg)), ty);
        }
        // The probe reads from the same tile.
        QCOMPARE(qint64(std::floor((t.probe.longitude() + 180.0) / tileDeg)), tx);
        QCOMPARE(qint64(std::floor((t.probe.latitude() + 90.0) / tileDeg)), ty);
    }
    for (int s : seen) {
        QCOMPARE(s, 1);
    }
    // Nearest the middle first.
    const double midLat = 37.7681, midLon = -122.4898;
    auto d = [&](const GroundTile &t) { return std::hypot(t.probe.latitude() - midLat, (t.probe.longitude() - midLon) * std::cos(midLat * 3.14159265358979323846 / 180.0)); };
    for (int i = 1; i < tiles.size(); i++) {
        QVERIFY(d(tiles[i - 1]) <= d(tiles[i]) + 1e-9);
    }
    QVERIFY(groupByTile(GroundGrid(), tileDeg).isEmpty());
}

void Plan3DGroundTest::_scheduleRetries()
{
    using S = TileSchedule::State;
    TileSchedule s;
    s.reset(3);
    QVERIFY(s.busy());
    QCOMPARE(s.take(4, 0), QList<int>({0, 1, 2}));
    QCOMPARE(s.count(S::InFlight), 3);
    QVERIFY(s.take(4, 0).isEmpty());

    // GC failed the batch: all three back to waiting, 1 s later.
    QCOMPARE(s.failed(0, 0), 1000);
    QCOMPARE(s.failed(1, 0), 1000);
    s.loaded(2);
    QCOMPARE(s.state(2), S::Loaded);
    QCOMPARE(s.nextDueInMs(500), qint64(500));
    QVERIFY(s.take(4, 999).isEmpty());

    // Retries go one at a time.
    QCOMPARE(s.take(4, 1000), QList<int>({0}));
    QVERIFY(s.take(4, 1000).isEmpty());
    s.loaded(0);
    QCOMPARE(s.take(4, 1000), QList<int>({1}));

    // Tile 1 never comes: 1 s, 2 s, 4 s, then given up and unknown.
    QCOMPARE(s.attempts(1), 2);
    QCOMPARE(s.failed(1, 1000), 2000);
    QCOMPARE(s.take(4, 3000), QList<int>({1}));
    QCOMPARE(s.failed(1, 3000), 4000);
    QCOMPARE(s.take(4, 7000), QList<int>({1}));
    QCOMPARE(s.attempts(1), TileSchedule::kMaxAttempts);
    QCOMPARE(s.failed(1, 7000), -1);
    QCOMPARE(s.state(1), S::Failed);
    QVERIFY(!s.busy());
    QCOMPARE(s.nextDueInMs(7000), qint64(-1));
    QCOMPARE(s.count(S::Loaded), 2);
    QCOMPARE(s.count(S::Failed), 1);

    // Late answers change nothing.
    QCOMPARE(s.failed(0, 8000), -1);
    QCOMPARE(s.state(0), S::Loaded);
}

void Plan3DGroundTest::_scheduleRetryAlone()
{
    using S = TileSchedule::State;
    TileSchedule s;
    s.reset(6);
    QCOMPARE(s.take(2, 0), QList<int>({0, 1}));
    QCOMPARE(s.failed(0, 0), 1000);
    // New tiles fill the free slot; the retry waits until nothing is in flight.
    QCOMPARE(s.take(2, 2000), QList<int>({2}));
    s.loaded(1);
    s.loaded(2);
    QCOMPARE(s.take(2, 2000), QList<int>({0}));
    // While a retry is in flight, nothing else goes.
    QVERIFY(s.take(2, 2000).isEmpty());
    s.loaded(0);
    QCOMPARE(s.take(2, 2000), QList<int>({3, 4}));
    QCOMPARE(s.state(5), S::Waiting);
}

void Plan3DGroundTest::_meshUnknownViolet()
{
    const GeoBounds b = wp300Area();
    GroundGrid g = plane(b, 4, 3);
    // The east column unknown, as a refused tile would leave it.
    for (int r = 0; r < g.rows; r++) {
        g.heights[r * g.cols + 3] = kNaN;
    }
    const LocalFrame F{(b.south + b.north) / 2, (b.west + b.east) / 2, 90.0};
    for (bool dark : {true, false}) {
        QList<QVector3D> pos;
        QList<QVector4D> col;
        GroundMesh::vertices(g, F, dark, pos, col);
        QCOMPARE(pos.size(), 12);
        QCOMPARE(col.size(), 12);
        const QColor violet = unknownColor(dark);
        for (int r = 0; r < g.rows; r++) {
            for (int c = 0; c < g.cols; c++) {
                const int i = r * g.cols + c;
                if (c == 3) {
                    // Unknown: marked for the shader, in the section 4 violet.
                    QVERIFY(col[i].w() < 0);
                    QVERIFY(std::abs(col[i].x() - violet.redF()) < 1e-6);
                    QVERIFY(std::abs(col[i].y() - violet.greenF()) < 1e-6);
                    QVERIFY(std::abs(col[i].z() - violet.blueF()) < 1e-6);
                    // Shown at the nearest known ground's height (display only).
                    QVERIFY(std::abs(pos[i].y() - pos[r * g.cols + 2].y()) < 1e-4);
                } else {
                    QCOMPARE(col[i].w(), 1.0f);
                    QVERIFY(std::abs(pos[i].y() - float(g.heights[i] - 90.0 - 0.05)) < 1e-3);
                }
                // x east, z south.
                QVERIFY(std::abs(pos[i].x() - float(F.east(g.nodeLon(c)))) < 1e-3);
                QVERIFY(std::abs(pos[i].z() + float(F.north(g.nodeLat(r)))) < 1e-3);
            }
        }
    }
    // Dark and light violets are the contract's (#b07cff, #7c3aed).
    QCOMPARE(unknownColor(true), QColor(0xb0, 0x7c, 0xff));
    QCOMPARE(unknownColor(false), QColor(0x7c, 0x3a, 0xed));
}

void Plan3DGroundTest::_meshAllUnknown()
{
    const GeoBounds b = wp300Area();
    const GroundGrid g = GroundGrid::layout(b);   // nothing loaded yet
    const LocalFrame F{(b.south + b.north) / 2, (b.west + b.east) / 2, 30.0};
    QList<QVector3D> pos;
    QList<QVector4D> col;
    GroundMesh::vertices(g, F, true, pos, col);
    QCOMPARE(pos.size(), g.cols * g.rows);
    for (int i = 0; i < pos.size(); i++) {
        QVERIFY(col[i].w() < 0);
        QVERIFY(std::abs(pos[i].y() + 0.05f) < 1e-6);   // launch level
    }
    GroundMesh::vertices(GroundGrid(), F, true, pos, col);
    QVERIFY(pos.isEmpty());
}

void Plan3DGroundTest::_imageryPlan()
{
    QCOMPARE(ImageryPlan::tileX(-180.0, 3), 0.0);
    QCOMPARE(ImageryPlan::tileX(0.0, 3), 4.0);
    QVERIFY(std::abs(ImageryPlan::tileY(0.0, 3) - 4.0) < 1e-9);
    QVERIFY(ImageryPlan::tileY(60.0, 3) < ImageryPlan::tileY(10.0, 3));   // y grows south

    const GeoBounds b = wp300Area();
    const ImageryPlan p = ImageryPlan::make(b);
    QVERIFY(p.valid());
    // About 1 km at 37.8 N: zoom 17 (about 0.95 m per pixel) fits 1536 px;
    // zoom 18 would need 1976 px.
    QCOMPARE(p.zoom, 17);
    QVERIFY(p.width <= 1536 && p.height <= 1536);
    QVERIFY(p.width > 900 && p.height > 600);
    QVERIFY(p.tileCount() <= 64);
    QCOMPARE(ImageryPlan::make(b, 2048, 18, 1, 100).zoom, 18);
    QVERIFY(p.x0 <= int(std::floor(ImageryPlan::tileX(b.west, p.zoom))));
    QVERIFY(p.x1 >= int(std::floor(ImageryPlan::tileX(b.east, p.zoom))));
    QVERIFY(p.y0 <= int(std::floor(ImageryPlan::tileY(b.north, p.zoom))));
    QVERIFY(p.y1 >= int(std::floor(ImageryPlan::tileY(b.south, p.zoom))));

    // A smaller cap gives a shallower zoom.
    const ImageryPlan q = ImageryPlan::make(b, 512);
    QVERIFY(q.zoom < p.zoom);
    QVERIFY(q.width <= 512 && q.height <= 512);

    QVERIFY(!ImageryPlan::make(GeoBounds()).valid());
    QCOMPARE(ImageryPlan::make(GeoBounds()).tileCount(), 0);
}

void Plan3DGroundTest::_imageryCompose()
{
    const GeoBounds b = wp300Area();
    const ImageryPlan p = ImageryPlan::make(b, 512);
    QVERIFY(p.valid());
    // One flat color per tile: red north-west, blue south-east, the rest
    // missing except the north-east, which is green.
    QHash<int, QImage> tiles;
    auto tile = [](QColor c) { QImage i(256, 256, QImage::Format_ARGB32); i.fill(c); return i; };
    tiles.insert(0, tile(Qt::red));
    tiles.insert(p.columns() - 1, tile(Qt::green));
    tiles.insert(p.tileCount() - 1, tile(Qt::blue));
    const QImage out = p.compose(b, tiles);
    QCOMPARE(out.size(), QSize(p.width, p.height));
    QCOMPARE(out.format(), QImage::Format_RGBA8888);
    // Row 0 is north, column 0 west.
    QCOMPARE(out.pixelColor(0, 0), QColor(Qt::red));
    QCOMPARE(out.pixelColor(out.width() - 1, out.height() - 1), QColor(Qt::blue));
    if (p.columns() > 1) {
        QCOMPARE(out.pixelColor(out.width() - 1, 0), QColor(Qt::green));
    }
    if (p.rows() > 1 && p.columns() > 1) {
        // South-west tile missing: transparent, the shaded ground shows.
        QCOMPARE(out.pixelColor(0, out.height() - 1).alpha(), 0);
    }
    QVERIFY(ImageryPlan().compose(b, tiles).isNull());
}

void Plan3DGroundTest::_areaClampedAndCapped()
{
    // G67 integration must-fix 1: the ground area GC loads is bounded, and
    // never has a node outside valid coordinates.
    auto bounds = [](double s, double w, double n, double e) { GeoBounds b; b.south = s; b.west = w; b.north = n; b.east = e; return b; };

    // Across the antimeridian (home at 179.99, a waypoint at -179.99): the
    // bounds span the whole longitude band. Not loaded: no tiles, no area.
    const GeoBounds band = bounds(-17.1, -179.995, -16.9, 179.995);
    const GroundGrid gb = GroundGrid::layout(band);
    QVERIFY(!gb.valid());
    QVERIFY(!gb.area.valid());
    QVERIFY(gb.sameArea(band));         // asked for again: no reload loop
    QVERIFY(groupByTile(gb, 0.01).isEmpty());
    QVERIFY(std::isnan(gb.elevation(-17.0, 179.99)));

    // The pre-rev 9 crash case (a home 12,800 km from wp300): one area over
    // both. Not loaded (it was 129 x 129 nodes, 16,641 tiles).
    QVERIFY(!GroundGrid::layout(bounds(-40.0, -122.6, 37.9, 151.3)).valid());

    // The largest area rev 9 can ask for (200 km around home plus margin) loads.
    const GroundGrid big = GroundGrid::layout(bounds(37.77 - 2.4, -122.49 - 3.0, 37.77 + 2.4, -122.49 + 3.0));
    QVERIFY(big.valid());
    QVERIFY(groupByTile(big, 0.01).size() <= GroundGrid::kMaxNodesPerSide * GroundGrid::kMaxNodesPerSide);

    // Past a pole or the antimeridian by a margin: clamped, every node valid.
    const GeoBounds polar = bounds(89.5, 179.5, 90.4, 180.6);
    const GroundGrid gp = GroundGrid::layout(polar);
    QVERIFY(gp.valid());
    QVERIFY(gp.sameArea(polar));
    QCOMPARE(gp.area.north, 90.0);
    QCOMPARE(gp.area.east, 180.0);
    for (const GroundTile &t : groupByTile(gp, 0.01)) {
        QVERIFY2(t.probe.isValid(), qPrintable(QStringLiteral("%1, %2").arg(t.probe.latitude()).arg(t.probe.longitude())));
    }
    // Imagery: Web Mercator has no tiles there; none are planned.
    QVERIFY(!ImageryPlan::make(gp.area).valid());
    QVERIFY(ImageryPlan::make(bounds(37.7652, -122.4951, 37.7710, -122.4845)).valid());
}
