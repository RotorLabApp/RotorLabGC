/****************************************************************************
 *
 * RotorLab GC Plan view 3D (G64, MG2) - ground elevation from GC's terrain.
 *
 ****************************************************************************/

#include "Plan3DGround.h"

#include "FlightMapSettings.h"
#include "SettingsManager.h"
#include "TerrainQuery.h"
#include "TerrainTileCopernicus.h"

#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLoggingCategory>
#include <QtCore/QPointer>

#include <algorithm>
#include <cmath>

Q_LOGGING_CATEGORY(Plan3DGroundLog, "rotorlab.plan3d.ground")

namespace Plan3D {

// ---------------------------------------------------------------------------
// GroundGrid

GroundGrid GroundGrid::layout(const GeoBounds &bounds, double spacingM, int maxPerSide)
{
    GroundGrid g;
    g.requested = bounds;
    if (!bounds.valid() || !std::isfinite(bounds.west) || !std::isfinite(bounds.east) || bounds.east < bounds.west) {
        return g;
    }
    // G67 integration must-fix 1: never a node outside valid coordinates (a
    // margin past a pole or the antimeridian), which GC's terrain tiling
    // cannot index.
    GeoBounds c = bounds;
    c.south = std::clamp(c.south, -90.0, 90.0);
    c.north = std::clamp(c.north, -90.0, 90.0);
    c.west = std::clamp(c.west, -180.0, 180.0);
    c.east = std::clamp(c.east, -180.0, 180.0);
    const LocalFrame f{(c.south + c.north) / 2, (c.west + c.east) / 2, 0};
    const double heightM = f.north(c.north) - f.north(c.south);
    const double widthM = f.east(c.east) - f.east(c.west);
    if (!(heightM <= kMaxAreaSideM && widthM <= kMaxAreaSideM)) {
        return g;   // too large to load: unknown ground, no terrain or imagery requests
    }
    g.area = c;
    maxPerSide = std::max(2, maxPerSide);
    g.cols = std::clamp(int(std::ceil(widthM / spacingM)) + 1, 2, maxPerSide);
    g.rows = std::clamp(int(std::ceil(heightM / spacingM)) + 1, 2, maxPerSide);
    g.heights.fill(kNaN, g.cols * g.rows);
    return g;
}

bool GroundGrid::sameArea(const GeoBounds &b) const
{
    return requested.valid() && b.valid()
        && std::abs(requested.south - b.south) < 1e-5 && std::abs(requested.north - b.north) < 1e-5
        && std::abs(requested.west - b.west) < 1e-5 && std::abs(requested.east - b.east) < 1e-5;
}

double GroundGrid::nodeLat(int row) const
{
    return rows > 1 ? area.south + (area.north - area.south) * row / (rows - 1) : area.south;
}

double GroundGrid::nodeLon(int col) const
{
    return cols > 1 ? area.west + (area.east - area.west) * col / (cols - 1) : area.west;
}

double GroundGrid::elevation(double lat, double lon) const
{
    if (!valid() || !(lat >= area.south && lat <= area.north && lon >= area.west && lon <= area.east)) {
        return kNaN;   // also NaN inputs
    }
    const double spanLon = area.east - area.west, spanLat = area.north - area.south;
    const double fx = spanLon > 0 ? (lon - area.west) / spanLon * (cols - 1) : 0;
    const double fy = spanLat > 0 ? (lat - area.south) / spanLat * (rows - 1) : 0;
    const int c0 = std::clamp(int(std::floor(fx)), 0, cols - 2);
    const int r0 = std::clamp(int(std::floor(fy)), 0, rows - 2);
    const double tx = fx - c0, ty = fy - r0;
    const double h00 = node(c0, r0), h10 = node(c0 + 1, r0), h01 = node(c0, r0 + 1), h11 = node(c0 + 1, r0 + 1);
    if (!std::isfinite(h00) || !std::isfinite(h10) || !std::isfinite(h01) || !std::isfinite(h11)) {
        return kNaN;
    }
    return (h00 * (1 - tx) + h10 * tx) * (1 - ty) + (h01 * (1 - tx) + h11 * tx) * ty;
}

int GroundGrid::knownCount() const
{
    return int(std::count_if(heights.cbegin(), heights.cend(), [](double h) { return std::isfinite(h); }));
}

QList<GroundTile> groupByTile(const GroundGrid &grid, double tileDeg)
{
    QList<GroundTile> out;
    if (!grid.valid() || !(tileDeg > 0)) {
        return out;
    }
    // The same tiling as GC's elevation provider (floor((lon + 180) / size)),
    // so every node of a group reads from one cached terrain tile.
    QHash<qint64, int> index;
    for (int r = 0; r < grid.rows; r++) {
        for (int c = 0; c < grid.cols; c++) {
            const qint64 tx = qint64(std::floor((grid.nodeLon(c) + 180.0) / tileDeg));
            const qint64 ty = qint64(std::floor((grid.nodeLat(r) + 90.0) / tileDeg));
            const qint64 key = (ty << 32) | (tx & 0xffffffffLL);
            auto it = index.constFind(key);
            if (it == index.constEnd()) {
                it = index.insert(key, int(out.size()));
                GroundTile t;
                t.key = key;
                out.append(t);
            }
            out[*it].nodes.append(r * grid.cols + c);
        }
    }
    // Probe: the node nearest the middle of the tile's nodes. Order: nearest
    // the area's centre first, so the ground under the plan comes in first.
    const double midLat = (grid.area.south + grid.area.north) / 2, midLon = (grid.area.west + grid.area.east) / 2;
    const double cosLat = std::cos(midLat * 3.14159265358979323846 / 180.0);
    QList<std::pair<double, int>> order;
    for (int i = 0; i < out.size(); i++) {
        GroundTile &t = out[i];
        const int n = t.nodes[t.nodes.size() / 2];
        t.probe = QGeoCoordinate(grid.nodeLat(n / grid.cols), grid.nodeLon(n % grid.cols));
        const double dy = t.probe.latitude() - midLat, dx = (t.probe.longitude() - midLon) * cosLat;
        order.append({dx * dx + dy * dy, i});
    }
    std::stable_sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    QList<GroundTile> sorted;
    sorted.reserve(out.size());
    for (const auto &o : order) {
        sorted.append(out[o.second]);
    }
    return sorted;
}

// ---------------------------------------------------------------------------
// TileSchedule

void TileSchedule::reset(int tiles)
{
    _tiles.fill(Tile{}, std::max(0, tiles));
}

QList<int> TileSchedule::take(int maxInFlight, qint64 nowMs)
{
    QList<int> out;
    int inFlight = 0;
    bool retryInFlight = false;
    for (const Tile &t : _tiles) {
        if (t.state == State::InFlight) {
            inFlight++;
            retryInFlight |= t.attempts > 1;
        }
    }
    if (retryInFlight) {
        return out;     // a retry runs alone
    }
    for (int i = 0; i < _tiles.size() && inFlight < maxInFlight; i++) {
        Tile &t = _tiles[i];
        if (t.state != State::Waiting || t.dueMs > nowMs) {
            continue;
        }
        if (t.attempts > 0) {
            if (inFlight > 0 || !out.isEmpty()) {
                continue;   // wait until nothing else is in flight
            }
            t.state = State::InFlight;
            t.attempts++;
            out.append(i);
            return out;
        }
        t.state = State::InFlight;
        t.attempts++;
        out.append(i);
        inFlight++;
    }
    return out;
}

void TileSchedule::loaded(int tile)
{
    if (tile >= 0 && tile < _tiles.size()) {
        _tiles[tile].state = State::Loaded;
    }
}

int TileSchedule::failed(int tile, qint64 nowMs)
{
    if (tile < 0 || tile >= _tiles.size()) {
        return -1;
    }
    Tile &t = _tiles[tile];
    if (t.state == State::Loaded || t.state == State::Failed) {
        return -1;
    }
    if (t.attempts >= kMaxAttempts) {
        t.state = State::Failed;
        return -1;
    }
    const int delay = kBaseDelayMs << std::max(0, t.attempts - 1);
    t.state = State::Waiting;
    t.dueMs = nowMs + delay;
    return delay;
}

qint64 TileSchedule::nextDueInMs(qint64 nowMs) const
{
    qint64 best = -1;
    for (const Tile &t : _tiles) {
        if (t.state == State::Waiting) {
            const qint64 d = std::max<qint64>(0, t.dueMs - nowMs);
            best = best < 0 ? d : std::min(best, d);
        }
    }
    return best;
}

int TileSchedule::count(State s) const
{
    return int(std::count_if(_tiles.cbegin(), _tiles.cend(), [s](const Tile &t) { return t.state == s; }));
}

// ---------------------------------------------------------------------------
// Ground

Ground::Ground(QObject *parent)
    : QObject(parent)
{
    _clock.start();
    _notify.setSingleShot(true);
    connect(&_notify, &QTimer::timeout, this, &Ground::changed);
    _pumpTimer.setSingleShot(true);
    connect(&_pumpTimer, &QTimer::timeout, this, &Ground::_pump);
#ifdef QGC_DAILY_BUILD
    // Test only: refuse the tiles east of the area's middle, as a refused or
    // failed terrain tile would be, to show unknown ground (G64 shots).
    if (qEnvironmentVariableIntValue("RL_G64_AUTORUN") == 1) {
        _refuse = qEnvironmentVariable("RL_G64_GROUND_REFUSE");
        // Test only (G66 tier 2 shots): the shared terrain fixture G66-F1
        // instead of GC's terrain query, whole, so the view draws exactly the
        // scene Plan3DParityTest checks. NOT real terrain.
        const QString fixture = qEnvironmentVariable("RL_G66_GROUND_FIXTURE");
        if (!fixture.isEmpty()) {
            _loadFixture(fixture);
        }
    }
#endif
}

void Ground::_loadFixture(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qCWarning(Plan3DGroundLog) << "G66 fixture not readable:" << path;
        return;
    }
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    GroundGrid g;
    g.area.south = o[QStringLiteral("south")].toDouble(kNaN);
    g.area.north = o[QStringLiteral("north")].toDouble(kNaN);
    g.area.west = o[QStringLiteral("west")].toDouble(kNaN);
    g.area.east = o[QStringLiteral("east")].toDouble(kNaN);
    g.cols = o[QStringLiteral("cols")].toInt();
    g.rows = o[QStringLiteral("rows")].toInt();
    for (const QJsonValue &v : o[QStringLiteral("heights")].toArray()) {
        g.heights.append(v.isNull() ? kNaN : v.toDouble());
    }
    if (!g.valid() || o[QStringLiteral("schema")].toString() != QStringLiteral("g66-fixture/1")) {
        qCWarning(Plan3DGroundLog) << "G66 fixture not g66-fixture/1:" << path;
        return;
    }
    _fixture = g;
    _fixtureName = o[QStringLiteral("id")].toString() + QStringLiteral(" (test fixture, not real terrain)");
    qCWarning(Plan3DGroundLog) << "G66: ground is the test fixture" << _fixtureName;
}

Ground::~Ground() = default;

QString Ground::sourceName()
{
#ifdef QGC_DAILY_BUILD
    // G66 tier 2: the legend names the test fixture as its terrain (CL-6).
    if (qEnvironmentVariableIntValue("RL_G64_AUTORUN") == 1 && !qEnvironmentVariable("RL_G66_GROUND_FIXTURE").isEmpty()) {
        return QStringLiteral("G66-F1 test fixture, not real terrain");
    }
#endif
    return SettingsManager::instance()->flightMapSettings()->elevationMapProvider()->rawValue().toString();
}

bool Ground::needsReload(const GroundGrid &loaded, const QString &loadedProvider, const GeoBounds &area, const QString &provider)
{
    if (!area.valid()) {
        return false;
    }
    // Another provider's heights are another terrain: never kept (CL-6).
    return !loaded.sameArea(area) || loadedProvider != provider;
}

void Ground::setArea(const GeoBounds &bounds)
{
    setArea(bounds, sourceName());
}

void Ground::setArea(const GeoBounds &bounds, const QString &provider)
{
    if (_fixture.valid()) {
        // G66 tier 2: the fixture replaces the terrain query for any area.
        if (!_grid.sameArea(_fixture.area)) {
            _provider = _fixtureName;
            _grid = _fixture;
            _generation++;
            _known = _grid.knownCount();
            _tiles.clear();
            _schedule.reset(0);
            _cacheCursor = 0;
            _changed();
        }
        return;
    }
    if (!needsReload(_grid, _provider, bounds, provider)) {
        return;
    }
    if (_provider != provider && !_provider.isEmpty()) {
        qCDebug(Plan3DGroundLog) << "elevation provider" << _provider << "->" << provider << ": reloading the ground";
    }
    _provider = provider;
    _grid = GroundGrid::layout(bounds);
    _generation++;
    _known = 0;
    _tiles = groupByTile(_grid, TerrainTileCopernicus::kTileSizeDegrees);
    _schedule.reset(int(_tiles.size()));
    _cacheCursor = 0;
    qCDebug(Plan3DGroundLog) << "area" << _grid.cols << "x" << _grid.rows << "nodes," << _tiles.size() << "terrain tiles";
    _schedulePump(0);
    _changed();
}

void Ground::_changed()
{
    // Throttled, not debounced: every change reaches the scene within a
    // second while tiles stream in (each one costs the scene a rebuild), and
    // within 100 ms once loading is over.
    const int ms = loading() ? 800 : 100;
    if (_notify.isActive() && _notify.remainingTime() <= ms) {
        return;
    }
    _notify.start(ms);
}

void Ground::_schedulePump(qint64 delayMs)
{
    const int ms = int(std::clamp<qint64>(delayMs, 0, 60000));
    if (_pumpTimer.isActive() && _pumpTimer.remainingTime() <= ms) {
        return;
    }
    _pumpTimer.start(ms);
}

bool Ground::_refused(int tile) const
{
    if (_refuse != QLatin1String("east")) {
        return false;
    }
    return _tiles[tile].probe.longitude() > (_grid.area.west + _grid.area.east) / 2;
}

bool Ground::_readFromCache(int tile)
{
    // GC's terrain tile cache, read directly: one coordinate first, so a miss
    // costs nothing (GC then starts that tile's download by itself).
    QList<double> probe;
    bool error = false;
    if (!TerrainAtCoordinateQuery::getAltitudesForCoordinates({_tiles[tile].probe}, probe, error)) {
        return false;
    }
    const GroundTile &t = _tiles[tile];
    QList<QGeoCoordinate> coords;
    coords.reserve(t.nodes.size());
    for (int n : t.nodes) {
        coords.append(QGeoCoordinate(_grid.nodeLat(n / _grid.cols), _grid.nodeLon(n % _grid.cols)));
    }
    QList<double> heights;
    if (!TerrainAtCoordinateQuery::getAltitudesForCoordinates(coords, heights, error) || heights.size() != t.nodes.size()) {
        return false;
    }
    // A node the tile has no value for stays unknown (error is set then).
    for (int i = 0; i < t.nodes.size(); i++) {
        const double h = heights[i];
        double &slot = _grid.heights[t.nodes[i]];
        if (std::isfinite(h)) {
            if (!std::isfinite(slot)) {
                _known++;
            }
            slot = h;
        }
    }
    return true;
}

void Ground::_pump()
{
    const qint64 now = _clock.elapsed();
    QElapsedTimer slice;
    slice.start();
    bool dirty = false;
    const bool inFlight = _schedule.count(TileSchedule::State::InFlight) > 0;
    if (_cacheCursor >= _tiles.size() && inFlight) {
        _cacheCursor = 0;   // look again while GC works on a request
    }

    // The cache first, in slices, so a large area never stalls the UI.
    while (_cacheCursor < _tiles.size()) {
        const int t = _cacheCursor++;
        // In flight too: another user's download (the 2D terrain profile) may
        // have brought the tile in before GC answers this request.
        const TileSchedule::State st = _schedule.state(t);
        if ((st == TileSchedule::State::Waiting || st == TileSchedule::State::InFlight) && !_refused(t) && _readFromCache(t)) {
            _schedule.loaded(t);
            dirty = true;
        }
        if (slice.elapsed() >= kSliceMs) {
            break;
        }
    }
    if (_cacheCursor < _tiles.size()) {
        if (dirty) {
            _changed();
        }
        _schedulePump(0);
        return;
    }

    // Then the network, for the tiles still missing.
    const QList<int> take = _schedule.take(kMaxInFlight, now);
    if (!take.isEmpty()) {
        _request(take);
        dirty = true;
    }
    const qint64 due = _schedule.nextDueInMs(now);
    if (due > 0) {
        _schedulePump(due);
    }
    if (_schedule.count(TileSchedule::State::InFlight) > 0) {
        _schedulePump(2000);
    }
    if (dirty || !loading()) {
        _changed();
    }
}

void Ground::_request(const QList<int> &tiles)
{
    const int gen = _generation;
    QList<int> net;
    for (int t : tiles) {
        if (_refused(t)) {
            // Test only: answered as a refused tile, without the network.
            QTimer::singleShot(50, this, [this, gen, t]() { _onReply(gen, {t}, false); });
        } else {
            net.append(t);
        }
    }
    if (net.isEmpty()) {
        return;
    }
    QList<QGeoCoordinate> coords;
    for (int t : net) {
        coords.append(_tiles[t].probe);
    }
    qCDebug(Plan3DGroundLog) << "requesting" << net.size() << "terrain tiles";

    // One reply per request: from GC's terrain query, or the watchdog if GC
    // never answers (a stalled connection), whichever comes first.
    auto answered = std::make_shared<bool>(false);
    auto *q = new TerrainAtCoordinateQuery(true /* autoDelete */);
    connect(q, &TerrainAtCoordinateQuery::terrainDataReceived, this, [this, gen, net, answered](bool success, const QList<double> &) {
        if (!*answered) {
            *answered = true;
            _onReply(gen, net, success);
        }
    });
    QTimer::singleShot(45000, this, [this, gen, net, answered]() {
        if (!*answered) {
            *answered = true;
            qCDebug(Plan3DGroundLog) << "terrain request timed out";
            _onReply(gen, net, false);
        }
    });
    q->requestData(coords);
}

void Ground::_onReply(int generation, const QList<int> &tiles, bool success)
{
    if (generation != _generation) {
        return;     // a reply for an earlier area
    }
    const qint64 now = _clock.elapsed();
    for (int t : tiles) {
        if (t < 0 || t >= _tiles.size() || _schedule.state(t) != TileSchedule::State::InFlight) {
            continue;
        }
        if (success && !_refused(t) && _readFromCache(t)) {
            _schedule.loaded(t);
        } else if (_schedule.failed(t, now) < 0) {
            qCDebug(Plan3DGroundLog) << "terrain tile given up, its ground stays unknown" << _tiles[t].probe;
        }
    }
    // A batch that failed part way still cached the tiles before the failure,
    // and other users' downloads fill the cache too: look again.
    _cacheCursor = 0;
    _schedulePump(0);
    _changed();
}

} // namespace Plan3D
