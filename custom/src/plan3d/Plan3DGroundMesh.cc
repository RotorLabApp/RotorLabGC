/****************************************************************************
 *
 * RotorLab GC Plan view 3D (G64, MG2) - the ground mesh and its imagery.
 *
 ****************************************************************************/

#include "Plan3DGroundMesh.h"

#include "Plan3DColors.h"
#include "Plan3DGround.h"

#include "FlightMapSettings.h"
#include "MapProvider.h"
#include "QGCMapUrlEngine.h"
#include "QGeoMapReplyQGC.h"
#include "QGeoTileFetcherQGC.h"
#include "SettingsManager.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QLoggingCategory>
#include <QtCore/QQueue>
#include <QtCore/QThreadPool>
#include <QtGui/QColor>
#include <QtGui/QPainter>
#include <QtLocation/private/qgeotilespec_p.h>
#include <QtNetwork/QNetworkAccessManager>
#include <QtQml/QQmlEngine>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <memory>

Q_DECLARE_LOGGING_CATEGORY(Plan3DGroundLog)

namespace Plan3D {

namespace {

constexpr double kPi = 3.14159265358979323846;

void grow(QVector3D &mn, QVector3D &mx, const QVector3D &v)
{
    mn = QVector3D(std::min(mn.x(), v.x()), std::min(mn.y(), v.y()), std::min(mn.z(), v.z()));
    mx = QVector3D(std::max(mx.x(), v.x()), std::max(mx.y(), v.y()), std::max(mx.z(), v.z()));
}

} // namespace

// ---------------------------------------------------------------------------
// GroundMesh

GroundMesh::GroundMesh(QQuick3DObject *parent)
    : QQuick3DGeometry(parent)
{
}

void GroundMesh::setGrid(int cols, int rows, const QList<QVector3D> &positions, const QList<QVector4D> &colors)
{
    clear();
    if (cols < 2 || rows < 2 || positions.size() != cols * rows || colors.size() != cols * rows) {
        update();
        return;
    }
    constexpr int kFloats = 12;   // position 3, normal 3, uv 2, color 4
    constexpr int kStride = kFloats * int(sizeof(float));
    QByteArray vbuf(cols * rows * kStride, Qt::Uninitialized);
    float *v = reinterpret_cast<float *>(vbuf.data());
    QVector3D mn(FLT_MAX, FLT_MAX, FLT_MAX), mx(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    auto at = [&](int c, int r) { return positions[std::clamp(r, 0, rows - 1) * cols + std::clamp(c, 0, cols - 1)]; };
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            const QVector3D p = at(c, r);
            const QVector3D dx = at(c + 1, r) - at(c - 1, r);
            const QVector3D dz = at(c, r - 1) - at(c, r + 1);   // row grows north, z grows south
            QVector3D n = QVector3D::crossProduct(dz, dx).normalized();
            if (n.y() < 0) {
                n = -n;
            }
            const QVector4D col = colors[r * cols + c];
            *v++ = p.x(); *v++ = p.y(); *v++ = p.z();
            *v++ = n.x(); *v++ = n.y(); *v++ = n.z();
            *v++ = float(c) / (cols - 1);
            *v++ = float(r) / (rows - 1);
            *v++ = col.x(); *v++ = col.y(); *v++ = col.z(); *v++ = col.w();
            grow(mn, mx, p);
        }
    }
    QByteArray ibuf((cols - 1) * (rows - 1) * 6 * int(sizeof(quint32)), Qt::Uninitialized);
    quint32 *idx = reinterpret_cast<quint32 *>(ibuf.data());
    for (int r = 0; r < rows - 1; r++) {
        for (int c = 0; c < cols - 1; c++) {
            const quint32 i0 = quint32(r * cols + c), i1 = i0 + 1, i2 = i0 + quint32(cols), i3 = i2 + 1;
            *idx++ = i0; *idx++ = i1; *idx++ = i2;
            *idx++ = i2; *idx++ = i1; *idx++ = i3;
        }
    }
    setStride(kStride);
    setPrimitiveType(QQuick3DGeometry::PrimitiveType::Triangles);
    addAttribute(QQuick3DGeometry::Attribute::PositionSemantic, 0, QQuick3DGeometry::Attribute::F32Type);
    addAttribute(QQuick3DGeometry::Attribute::NormalSemantic, 3 * sizeof(float), QQuick3DGeometry::Attribute::F32Type);
    addAttribute(QQuick3DGeometry::Attribute::TexCoord0Semantic, 6 * sizeof(float), QQuick3DGeometry::Attribute::F32Type);
    addAttribute(QQuick3DGeometry::Attribute::ColorSemantic, 8 * sizeof(float), QQuick3DGeometry::Attribute::F32Type);
    addAttribute(QQuick3DGeometry::Attribute::IndexSemantic, 0, QQuick3DGeometry::Attribute::U32Type);
    setVertexData(vbuf);
    setIndexData(ibuf);
    setBounds(mn, mx);
    update();
}

void GroundMesh::vertices(const GroundGrid &grid, const LocalFrame &F, bool dark, QList<QVector3D> &pos, QList<QVector4D> &col)
{
    pos.clear();
    col.clear();
    if (!grid.valid()) {
        return;
    }
    const int cols = grid.cols, rows = grid.rows, n = cols * rows;

    // Display heights: unknown nodes take the nearest known node's height
    // (breadth first from every known node), so unknown ground sits in the
    // terrain instead of as a wall. Display only: elevation() stays NaN there.
    QVector<double> shown = grid.heights;
    QQueue<int> todo;
    double lo = DBL_MAX, hi = -DBL_MAX;
    for (int i = 0; i < n; i++) {
        if (std::isfinite(shown[i])) {
            todo.enqueue(i);
            lo = std::min(lo, shown[i]);
            hi = std::max(hi, shown[i]);
        }
    }
    while (!todo.isEmpty()) {
        const int i = todo.dequeue();
        const int r = i / cols, c = i % cols;
        const int nb[4][2] = {{c - 1, r}, {c + 1, r}, {c, r - 1}, {c, r + 1}};
        for (const auto &p : nb) {
            if (p[0] < 0 || p[0] >= cols || p[1] < 0 || p[1] >= rows) {
                continue;
            }
            const int j = p[1] * cols + p[0];
            if (!std::isfinite(shown[j])) {
                shown[j] = shown[i];
                todo.enqueue(j);
            }
        }
    }

    const QColor violet = unknownColor(dark);
    const QColor low = dark ? QColor(QStringLiteral("#26332b")) : QColor(QStringLiteral("#d5dccb"));
    const QColor high = dark ? QColor(QStringLiteral("#5d6b55")) : QColor(QStringLiteral("#9fae8c"));
    pos.reserve(n);
    col.reserve(n);
    for (int r = 0; r < rows; r++) {
        const double lat = grid.nodeLat(r);
        for (int c = 0; c < cols; c++) {
            const int i = r * cols + c;
            const double h = grid.heights[i];
            // No known ground anywhere: launch level (y 0), display only.
            const double y = std::isfinite(shown[i]) ? shown[i] - F.alt0 : 0.0;
            pos.append(QVector3D(float(F.east(grid.nodeLon(c))), float(y) - 0.05f, float(-F.north(lat))));
            if (std::isfinite(h)) {
                const double t = hi > lo ? (h - lo) / (hi - lo) : 0.5;
                col.append(QVector4D(float(low.redF() + (high.redF() - low.redF()) * t),
                                     float(low.greenF() + (high.greenF() - low.greenF()) * t),
                                     float(low.blueF() + (high.blueF() - low.blueF()) * t), 1.0f));
            } else {
                col.append(QVector4D(float(violet.redF()), float(violet.greenF()), float(violet.blueF()), -1.0f));
            }
        }
    }
}

void GroundMesh::build(const Ground &ground, const LocalFrame &F, bool dark)
{
    QList<QVector3D> pos;
    QList<QVector4D> col;
    const GroundGrid &grid = ground.grid();
    vertices(grid, F, dark, pos, col);
    if (pos.isEmpty()) {
        _widthM = _depthM = 0;
        setGrid(0, 0, pos, col);
        return;
    }
    _widthM = F.east(grid.area.east) - F.east(grid.area.west);
    _depthM = F.north(grid.area.north) - F.north(grid.area.south);
    setGrid(grid.cols, grid.rows, pos, col);
}

// ---------------------------------------------------------------------------
// ImageryPlan

double ImageryPlan::tileX(double lon, int zoom)
{
    return (lon + 180.0) / 360.0 * double(1 << zoom);
}

double ImageryPlan::tileY(double lat, int zoom)
{
    lat = std::clamp(lat, -85.05112878, 85.05112878);
    const double r = lat * kPi / 180.0;
    return (1.0 - std::log(std::tan(r) + 1.0 / std::cos(r)) / kPi) / 2.0 * double(1 << zoom);
}

ImageryPlan ImageryPlan::make(const GeoBounds &a, int maxPx, int maxZoom, int minZoom, int maxTiles)
{
    ImageryPlan best;
    if (!a.valid() || !std::isfinite(a.west) || !std::isfinite(a.east) || a.east <= a.west || a.north <= a.south) {
        return best;
    }
    // G67 integration must-fix 1: Web Mercator tiles exist only within
    // +-85.0511 degrees and -180..180; outside, tile indexes are infinite.
    if (a.south < -85.0511 || a.north > 85.0511 || a.west < -180.0 || a.east > 180.0) {
        return best;    // no imagery: shaded ground
    }
    for (int z = std::clamp(maxZoom, 0, 22); z >= std::max(0, minZoom); z--) {
        const double fx0 = tileX(a.west, z), fx1 = tileX(a.east, z);
        const double fy0 = tileY(a.north, z), fy1 = tileY(a.south, z);
        const double wPx = (fx1 - fx0) * kTilePx, hPx = (fy1 - fy0) * kTilePx;
        ImageryPlan p;
        p.zoom = z;
        p.x0 = int(std::floor(fx0));
        p.x1 = std::min(int(std::floor(fx1)), (1 << z) - 1);
        p.y0 = int(std::floor(fy0));
        p.y1 = std::min(int(std::floor(fy1)), (1 << z) - 1);
        p.width = std::max(1, int(std::lround(wPx)));
        p.height = std::max(1, int(std::lround(hPx)));
        if ((wPx <= maxPx && hPx <= maxPx && p.tileCount() <= maxTiles) || z == std::max(0, minZoom)) {
            best = p;
            break;
        }
    }
    if (best.valid()) {
        best.width = std::min(best.width, maxPx);
        best.height = std::min(best.height, maxPx);
    }
    return best;
}

QImage ImageryPlan::compose(const GeoBounds &a, const QHash<int, QImage> &tiles) const
{
    if (!valid()) {
        return QImage();
    }
    QImage out(width, height, QImage::Format_RGBA8888);
    out.fill(Qt::transparent);
    // Source pixel column per output column: longitude is linear in tile x.
    QVector<int> srcX(width);
    for (int i = 0; i < width; i++) {
        const double lon = a.west + (a.east - a.west) * (i + 0.5) / width;
        srcX[i] = int(std::floor((tileX(lon, zoom) - x0) * kTilePx));
    }
    QHash<int, QImage> conv;
    for (auto it = tiles.cbegin(); it != tiles.cend(); ++it) {
        if (!it.value().isNull()) {
            QImage img = it.value().convertToFormat(QImage::Format_RGBA8888);
            if (img.size() != QSize(kTilePx, kTilePx)) {
                img = img.scaled(kTilePx, kTilePx, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            }
            conv.insert(it.key(), img);
        }
    }
    for (int j = 0; j < height; j++) {
        // Rows are linear in latitude; the tiles are Mercator.
        const double lat = a.north - (a.north - a.south) * (j + 0.5) / height;
        const int sy = int(std::floor((tileY(lat, zoom) - y0) * kTilePx));
        const int ty = sy / kTilePx, py = sy % kTilePx;
        if (sy < 0 || ty >= rows()) {
            continue;
        }
        quint32 *dst = reinterpret_cast<quint32 *>(out.scanLine(j));
        int lastTx = -1;
        const quint32 *src = nullptr;
        for (int i = 0; i < width; i++) {
            const int sx = srcX[i];
            if (sx < 0) {
                continue;
            }
            const int tx = sx / kTilePx;
            if (tx >= columns()) {
                break;
            }
            if (tx != lastTx) {
                lastTx = tx;
                auto it = conv.constFind(ty * columns() + tx);
                src = it == conv.constEnd() ? nullptr : reinterpret_cast<const quint32 *>(it.value().constScanLine(py));
            }
            if (src) {
                dst[i] = src[sx % kTilePx];
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// GroundImagery

GroundImagery::GroundImagery(QQuick3DObject *parent)
    : QQuick3DTextureData(parent)
    , _network(new QNetworkAccessManager(this))
{
    _startTimer.setSingleShot(true);
    _startTimer.setInterval(250);
    connect(&_startTimer, &QTimer::timeout, this, &GroundImagery::_start);
    _composeTimer.setSingleShot(true);
    _composeTimer.setInterval(300);
    connect(&_composeTimer, &QTimer::timeout, this, &GroundImagery::_compose);
    setFormat(QQuick3DTextureData::RGBA8);
    setHasTransparency(true);
}

GroundImagery::~GroundImagery()
{
    _generation++;
}

QVariantList GroundImagery::area() const
{
    if (!_area.valid()) {
        return {};
    }
    return {_area.south, _area.west, _area.north, _area.east};
}

void GroundImagery::setArea(const QVariantList &v)
{
    GeoBounds b;
    if (v.size() == 4) {
        b.south = v[0].toDouble();
        b.west = v[1].toDouble();
        b.north = v[2].toDouble();
        b.east = v[3].toDouble();
    }
    auto same = [](double x, double y) { return (std::isnan(x) && std::isnan(y)) || std::abs(x - y) < 1e-6; };
    if (same(b.south, _area.south) && same(b.west, _area.west) && same(b.north, _area.north) && same(b.east, _area.east)) {
        return;
    }
    _area = b;
    emit areaChanged();
    _startTimer.start();    // debounced: the area moves with every plan edit
}

void GroundImagery::_start()
{
    _generation++;
    _queue.clear();
    _attempts.clear();
    _tiles.clear();
    _inFlight = 0;
    if (_ready) {
        // The old image belongs to the old area: shaded ground until the new one is in.
        _ready = false;
        emit readyChanged();
    }

    FlightMapSettings *s = SettingsManager::instance()->flightMapSettings();
    _provider = s->mapProvider()->rawValue().toString() + QLatin1Char(' ') + s->mapType()->rawValue().toString();
    const SharedMapProvider provider = UrlFactory::getMapProviderFromProviderType(_provider);
    _mapId = provider && !provider->isElevationProvider() ? UrlFactory::getQtMapIdFromProviderType(_provider) : -1;
    _plan = _mapId >= 0 ? ImageryPlan::make(_area) : ImageryPlan();
    if (!_plan.valid()) {
        qCDebug(Plan3DGroundLog) << "no imagery for" << _provider;
        emit progressChanged();
        return;
    }
    // Nearest the middle first.
    const double cx = (_plan.columns() - 1) / 2.0, cy = (_plan.rows() - 1) / 2.0;
    for (int i = 0; i < _plan.tileCount(); i++) {
        _queue.append(i);
    }
    std::stable_sort(_queue.begin(), _queue.end(), [&](int a, int b) {
        const double ax = a % _plan.columns() - cx, ay = a / _plan.columns() - cy;
        const double bx = b % _plan.columns() - cx, by = b / _plan.columns() - cy;
        return ax * ax + ay * ay < bx * bx + by * by;
    });
    qCDebug(Plan3DGroundLog) << "imagery" << _provider << "zoom" << _plan.zoom << _plan.tileCount() << "tiles" << _plan.width << "x" << _plan.height;
    _clock.start();
    emit progressChanged();
    _next();
}

void GroundImagery::_next()
{
    while (_inFlight < kMaxInFlight && !_queue.isEmpty()) {
        _fetch(_queue.takeFirst());
    }
}

void GroundImagery::_fetch(int index)
{
    const int gen = _generation;
    QGeoTileSpec spec;
    spec.setMapId(_mapId);
    spec.setZoom(_plan.zoom);
    spec.setX(_plan.x0 + index % _plan.columns());
    spec.setY(_plan.y0 + index / _plan.columns());
    const QNetworkRequest request = QGeoTileFetcherQGC::getNetworkRequest(spec.mapId(), spec.x(), spec.y(), spec.zoom());
    if (request.url().isEmpty()) {
        return;
    }
    _inFlight++;
    _attempts[index]++;
    // GC's own tile reply: its map tile cache first, the network only on a
    // miss, and what comes in is cached as a 2D map tile would be.
    auto *reply = new QGeoTiledMapReplyQGC(_network, request, spec, this);
    auto done = std::make_shared<bool>(false);
    auto finish = [this, gen, index, done](QGeoTiledMapReplyQGC *r, bool timedOut) {
        if (*done) {
            return;
        }
        *done = true;
        if (gen != _generation) {
            if (r) {
                r->deleteLater();
            }
            return;
        }
        _inFlight--;
        bool ok = false;
        if (!timedOut && r && r->error() == QGeoTiledMapReply::NoError) {
            QImage img;
            if (img.loadFromData(r->mapImageData())) {
                _tiles.insert(index, img);
                ok = true;
            }
        }
        if (r) {
            r->deleteLater();
        }
        qCDebug(Plan3DGroundLog) << "imagery tile" << index << (ok ? "ok" : "failed") << (timedOut ? "timed out" : "")
                                 << (r ? r->errorString() : QString()) << (r && r->isCached() ? "cached" : "") << _clock.elapsed() << "ms";
        if (ok) {
            _scheduleCompose();
            if (_tiles.size() == _plan.tileCount()) {
                qCDebug(Plan3DGroundLog) << "imagery complete" << _tiles.size() << "tiles in" << _clock.elapsed() << "ms";
            }
        } else if (_attempts.value(index) < kMaxAttempts) {
            const int delay = 1000 * _attempts.value(index);
            QTimer::singleShot(delay, this, [this, gen, index]() {
                if (gen == _generation) {
                    _queue.append(index);
                    _next();
                }
            });
        } else {
            qCDebug(Plan3DGroundLog) << "imagery tile given up" << index;
        }
        emit progressChanged();
        _next();
    };
    connect(reply, &QGeoTiledMapReply::finished, this, [reply, finish]() { finish(reply, false); });
    QPointer<QGeoTiledMapReplyQGC> guard(reply);
    QTimer::singleShot(30000, this, [guard, finish]() {
        if (guard) {
            guard->abort();
        }
        finish(guard.data(), true);
    });
}

void GroundImagery::_scheduleCompose()
{
    if (!_composeTimer.isActive()) {
        _composeTimer.start(loading() ? 300 : 0);
    }
}

void GroundImagery::_compose()
{
    if (_composing) {
        _composeAgain = true;
        return;
    }
    if (_tiles.isEmpty()) {
        return;
    }
    _composing = true;
    _composeAgain = false;
    const int gen = _generation;
    const ImageryPlan plan = _plan;
    const GeoBounds area = _area;
    const QHash<int, QImage> tiles = _tiles;
    QPointer<GroundImagery> self(this);
    // Off the UI thread: a 2048 px image is a few tens of milliseconds.
    QThreadPool::globalInstance()->start([self, gen, plan, area, tiles]() {
        const QImage img = plan.compose(area, tiles);
        QMetaObject::invokeMethod(qApp, [self, gen, img]() {
            if (!self) {
                return;
            }
            self->_composing = false;
            if (gen == self->_generation && !img.isNull()) {
                self->setSize(img.size());
                self->setTextureData(QByteArray(reinterpret_cast<const char *>(img.constBits()), img.sizeInBytes()));
                if (!self->_ready) {
                    self->_ready = true;
                    emit self->readyChanged();
                }
            }
            if (self->_composeAgain) {
                self->_compose();
            }
        }, Qt::QueuedConnection);
    });
}

void registerGroundQmlTypes()
{
    qmlRegisterType<GroundImagery>("RotorLab.Plan3D", 1, 0, "Plan3DGroundImagery");
}

} // namespace Plan3D
