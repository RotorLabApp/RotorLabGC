/****************************************************************************
 *
 * RotorLab GC Plan view 3D (G64, MG2) - the ground mesh and its imagery.
 *
 * GroundMesh: the terrain grid of Plan3D::Ground as one mesh, with
 * per-vertex color: known ground shaded by height (map imagery on top when
 * GC has the tiles), unknown ground violet (PLAN-CONTRACT.md A-3, CL-2),
 * drawn at the height of the nearest known ground, display only. Vertex
 * color alpha below 0 marks unknown ground for Plan3DGround.frag.
 *
 * GroundImagery: the 2D map's imagery (the map provider and type set in
 * Settings) over the ground area, from GC's own map tile cache, downloading
 * only what is missing through GC's tile reply, and so cached for offline use
 * like any 2D map tile. Rows are linear in latitude (row 0 north) and columns
 * linear in longitude, matching the mesh's UV0. QML: Plan3DGroundImagery,
 * the `imagery` texture of the ground material in Plan3DGroundLayer.qml.
 *
 *
 ****************************************************************************/

#pragma once

#include "Plan3DBuilder.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtCore/QVariantList>
#include <QtGui/QImage>
#include <QtGui/QVector3D>
#include <QtGui/QVector4D>
#include <QtQuick3D/QQuick3DGeometry>
#include <QtQuick3D/QQuick3DTextureData>

class QNetworkAccessManager;

namespace Plan3D {

class Ground;
struct GroundGrid;

class GroundMesh : public QQuick3DGeometry
{
    Q_OBJECT

public:
    explicit GroundMesh(QQuick3DObject *parent = nullptr);

    /// Rebuilds the mesh from the loaded grid, in the scene's local frame.
    /// Vertex color alpha < 0 marks unknown ground for the shader.
    void build(const Ground &ground, const LocalFrame &frame, bool darkTheme);

    /// The vertices of a grid, pure: positions in the scene frame (unknown
    /// nodes at the nearest known node's height, or launch level when none
    /// is known) and colors (alpha -1 for unknown, in the unknown color).
    static void vertices(const GroundGrid &grid, const LocalFrame &frame, bool darkTheme,
                         QList<QVector3D> &positions, QList<QVector4D> &colors);

    /// cols x rows grid of positions (row major, row 0 south), UV0 in 0..1
    /// over the area (u east, v north), per-vertex colors.
    void setGrid(int cols, int rows, const QList<QVector3D> &positions, const QList<QVector4D> &colors);

    /// Ground size in metres (east-west, north-south), for the imagery.
    double widthM() const { return _widthM; }
    double depthM() const { return _depthM; }

private:
    double _widthM = 0;
    double _depthM = 0;
};

/// Which map tiles cover an area and how big the composed image is, pure.
/// Web Mercator tiles of 256 px (XYZ, y grows south).
struct ImageryPlan {
    int zoom = -1;
    int x0 = 0, x1 = -1, y0 = 0, y1 = -1;   ///< tile range, inclusive; y0 is the north row
    int width = 0, height = 0;              ///< composed image, pixels

    static constexpr int kTilePx = 256;

    /// The deepest zoom (minZoom..maxZoom) at which the area fits in maxPx on
    /// both sides and needs at most maxTiles tiles. Invalid (zoom -1) for an
    /// invalid area.
    static ImageryPlan make(const GeoBounds &area, int maxPx = 1536, int maxZoom = 18, int minZoom = 1, int maxTiles = 64);
    static double tileX(double lon, int zoom);   ///< fractional
    static double tileY(double lat, int zoom);   ///< fractional, grows south

    bool valid() const { return zoom >= 0 && x1 >= x0 && y1 >= y0 && width > 0 && height > 0; }
    int columns() const { return x1 - x0 + 1; }
    int rows() const { return y1 - y0 + 1; }
    int tileCount() const { return valid() ? columns() * rows() : 0; }

    /// Composes tiles (keyed by (ty - y0) * columns() + (tx - x0); missing
    /// tiles stay transparent) into a width x height RGBA8888 image over
    /// `area`, rows linear in latitude (row 0 north), columns in longitude.
    QImage compose(const GeoBounds &area, const QHash<int, QImage> &tiles) const;
};

class GroundImagery : public QQuick3DTextureData
{
    Q_OBJECT
    Q_PROPERTY(QVariantList area READ area WRITE setArea NOTIFY areaChanged)   ///< [south, west, north, east]
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY progressChanged)
    Q_PROPERTY(int loadedTiles READ loadedTiles NOTIFY progressChanged)
    Q_PROPERTY(int totalTiles READ totalTiles NOTIFY progressChanged)
    Q_PROPERTY(QString sourceName READ sourceName NOTIFY progressChanged)

public:
    explicit GroundImagery(QQuick3DObject *parent = nullptr);
    ~GroundImagery() override;

    QVariantList area() const;
    void setArea(const QVariantList &area);
    bool ready() const { return _ready; }
    bool loading() const { return _inFlight > 0 || !_queue.isEmpty(); }
    int loadedTiles() const { return int(_tiles.size()); }
    int totalTiles() const { return _plan.tileCount(); }
    QString sourceName() const { return _provider; }

signals:
    void areaChanged();
    void readyChanged();
    void progressChanged();

private:
    void _start();
    void _next();
    void _fetch(int index);
    void _scheduleCompose();
    void _compose();

    GeoBounds               _area;
    ImageryPlan             _plan;
    QString                 _provider;
    int                     _mapId = -1;
    int                     _generation = 0;
    QList<int>              _queue;
    QHash<int, int>         _attempts;
    QElapsedTimer           _clock;
    QHash<int, QImage>      _tiles;
    int                     _inFlight = 0;
    bool                    _ready = false;
    bool                    _composing = false;
    bool                    _composeAgain = false;
    QTimer                  _startTimer;
    QTimer                  _composeTimer;
    QNetworkAccessManager   *_network = nullptr;

    static constexpr int kMaxInFlight = 8;
    static constexpr int kMaxAttempts = 3;
};

/// Registers Plan3DGroundImagery (RotorLab.Plan3D 1.0). Windows and Linux only.
void registerGroundQmlTypes();

} // namespace Plan3D
