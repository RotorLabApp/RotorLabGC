/****************************************************************************
 *
 * RotorLab GC Plan view 3D (G64, MG2) - ground elevation from GC's terrain.
 *
 * A grid of ground heights over the plan's area, filled through GC's own
 * terrain query (TerrainAtCoordinateQuery, the provider set in Settings,
 * Copernicus via terrain-ce by default), which reads GC's terrain tile cache
 * first and downloads only what is missing. A grid node that has no height
 * (a failed, refused or pending tile) stays NaN: the ground there is unknown
 * (PLAN-CONTRACT.md A-3, CL-2), never level with launch.
 *
 * Loading is per GC terrain tile (0.01 degree): the cache is read in time
 * slices so the UI never stalls, and only missing tiles go to the network, a
 * few at a time, each retried with backoff before it is given up as unknown.
 *
 ****************************************************************************/

#pragma once

#include "Plan3DBuilder.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QTimer>
#include <QtCore/QVector>
#include <QtPositioning/QGeoCoordinate>

namespace Plan3D {

/// The height grid, pure: layout, node positions and bilinear lookup.
/// Row 0 is the south edge, column 0 the west edge; NaN is unknown.
struct GroundGrid {
    GeoBounds       area;           ///< the area the nodes cover (inside -90..90, -180..180)
    GeoBounds       requested;      ///< the area asked for, before clamping (sameArea compares it)
    int             cols = 0;
    int             rows = 0;
    QVector<double> heights;

    static constexpr int    kMaxNodesPerSide = 129;
    static constexpr double kNodeSpacingM    = 30.0;   // Copernicus GLO-30 spacing
    /// G67 integration must-fix 1: an area wider or taller than this is not
    /// loaded at all (its ground stays unknown), so a plan that spans the
    /// antimeridian, or has no home to bound it, can never ask for thousands
    /// of terrain tiles. Rev 9 plans stay within about 520 km.
    static constexpr double kMaxAreaSideM    = 1000000.0;

    /// A grid over `bounds` with about `spacingM` between nodes, at least 2
    /// and at most `maxPerSide` nodes per side, every node unknown. `bounds`
    /// is clamped to valid latitudes and longitudes; an area with a side over
    /// kMaxAreaSideM gives an empty grid (not valid(), no area: unknown ground).
    static GroundGrid layout(const GeoBounds &bounds, double spacingM = kNodeSpacingM, int maxPerSide = kMaxNodesPerSide);

    bool valid() const { return area.valid() && cols >= 2 && rows >= 2 && heights.size() == cols * rows; }
    /// Same area to about a metre.
    bool sameArea(const GeoBounds &bounds) const;
    double nodeLat(int row) const;
    double nodeLon(int col) const;
    double node(int col, int row) const { return heights[row * cols + col]; }
    /// Bilinear; NaN outside the area or when any of the four nodes is unknown.
    double elevation(double lat, double lon) const;
    int knownCount() const;
};

/// The nodes of a grid grouped by terrain tile, nearest the area's centre first.
struct GroundTile {
    qint64          key = 0;        ///< (tileY << 32) | tileX in GC's terrain tiling
    QList<int>      nodes;          ///< row * cols + col
    QGeoCoordinate  probe;          ///< one node inside the tile, for the network request
};
QList<GroundTile> groupByTile(const GroundGrid &grid, double tileDeg);

/// Load state per tile, pure (no network, no clock of its own): which tiles to
/// request now, retries with backoff, and when a tile is given up as unknown.
class TileSchedule
{
public:
    enum class State { Waiting, InFlight, Loaded, Failed };

    static constexpr int kMaxAttempts  = 4;
    static constexpr int kBaseDelayMs  = 1000;   ///< 1 s, 2 s, 4 s between attempts

    void reset(int tiles);
    int size() const { return int(_tiles.size()); }
    State state(int tile) const { return _tiles[tile].state; }
    int attempts(int tile) const { return _tiles[tile].attempts; }

    /// Tiles to request now, marked in flight. New tiles go up to `maxInFlight`
    /// together; a retry goes alone, so one bad tile cannot fail its neighbours
    /// (GC fails a whole terrain batch when one tile in it fails).
    QList<int> take(int maxInFlight, qint64 nowMs);
    /// Loaded, from the cache or the network.
    void loaded(int tile);
    /// A request failed: back to waiting with a delay, or Failed after
    /// kMaxAttempts. Returns the delay in ms, or -1 when given up.
    int failed(int tile, qint64 nowMs);
    /// Milliseconds until the next waiting tile is due (0 = now), -1 when none waits.
    qint64 nextDueInMs(qint64 nowMs) const;

    bool busy() const { return count(State::Waiting) + count(State::InFlight) > 0; }
    int count(State s) const;

private:
    struct Tile {
        State   state = State::Waiting;
        int     attempts = 0;
        qint64  dueMs = 0;
    };
    QVector<Tile> _tiles;
};

class Ground : public QObject
{
    Q_OBJECT

public:
    explicit Ground(QObject *parent = nullptr);
    ~Ground() override;

    /// Sets the area. Keeps the loaded heights when the area and the
    /// elevation provider are unchanged; drops them and reloads otherwise.
    void setArea(const GeoBounds &bounds);
    /// Same, with the provider given (setArea() reads it from Settings).
    void setArea(const GeoBounds &bounds, const QString &provider);

    /// Pure: whether a grid loaded over `loadedArea` from `loadedProvider`
    /// must be dropped and reloaded for `area` from `provider`.
    static bool needsReload(const GroundGrid &loaded, const QString &loadedProvider, const GeoBounds &area, const QString &provider);
    /// The elevation provider the loaded grid came from.
    const QString &provider() const { return _provider; }
    const GeoBounds &area() const { return _grid.area; }

    /// Ground AMSL by bilinear interpolation of the grid, NaN outside the
    /// area or where any of the four nodes is unknown.
    double elevation(double lat, double lon) const { return _grid.elevation(lat, lon); }

    int columns() const { return _grid.cols; }
    int rows() const { return _grid.rows; }
    /// Node height (NaN when unknown); row 0 is the south edge, column 0 the west edge.
    double node(int col, int row) const { return _grid.node(col, row); }
    double nodeLat(int row) const { return _grid.nodeLat(row); }
    double nodeLon(int col) const { return _grid.nodeLon(col); }
    const GroundGrid &grid() const { return _grid; }

    int knownNodes() const { return _known; }
    int totalNodes() const { return _grid.cols * _grid.rows; }
    bool loading() const { return _cacheCursor < _tiles.size() || _schedule.busy(); }
    /// Name of GC's elevation provider, for the legend (CL-6).
    static QString sourceName();

signals:
    /// Heights or the loading state changed (throttled).
    void changed();

private:
    void _pump();
    void _changed();
    void _schedulePump(qint64 delayMs);
    bool _readFromCache(int tile);
    void _request(const QList<int> &tiles);
    void _onReply(int generation, const QList<int> &tiles, bool success);
    bool _refused(int tile) const;
    void _loadFixture(const QString &path);

    GroundGrid          _grid;
    QString             _provider;          ///< elevation provider the grid is loaded from
    QList<GroundTile>   _tiles;
    TileSchedule        _schedule;
    int                 _cacheCursor = 0;   ///< first pass over the cache, tile by tile
    int                 _known = 0;
    int                 _generation = 0;
    QTimer              _pumpTimer;
    QTimer              _notify;
    QElapsedTimer       _clock;
    QString             _refuse;            ///< test only (Daily, RL_G64_AUTORUN): "east" refuses the east half's tiles
    GroundGrid          _fixture;           ///< test only (Daily, RL_G64_AUTORUN): G66 fixture from RL_G66_GROUND_FIXTURE
    QString             _fixtureName;

    static constexpr int    kMaxInFlight    = 4;
    static constexpr qint64 kSliceMs        = 6;     ///< cache reads per event loop turn
};

} // namespace Plan3D
