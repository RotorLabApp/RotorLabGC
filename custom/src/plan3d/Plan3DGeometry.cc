/****************************************************************************
 *
 * RotorLab GC Plan view 3D (G64, MG2) - the scene's GPU data.
 *
 ****************************************************************************/

#include "Plan3DGeometry.h"

#include "Plan3DMath.h"

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <limits>

namespace Plan3D {

namespace {

void grow(QVector3D &mn, QVector3D &mx, const QVector3D &v)
{
    mn = QVector3D(std::min(mn.x(), v.x()), std::min(mn.y(), v.y()), std::min(mn.z(), v.z()));
    mx = QVector3D(std::max(mx.x(), v.x()), std::max(mx.y(), v.y()), std::max(mx.z(), v.z()));
}

} // namespace

// ---------------------------------------------------------------- LineMesh

LineMesh::LineMesh(QQuick3DObject *parent)
    : QQuick3DGeometry(parent)
{
    setSegments({});
}

void LineMesh::setSegments(const QList<Seg> &segsIn)
{
    // Per vertex: position (3), the segment's other end (3, NORMAL), side and
    // end flag (2, UV0), distance along and signed width in px (2, UV1; a
    // negative width means dashed), color (4).
    constexpr int kFloats = 14;
    constexpr qsizetype kStride = kFloats * qsizetype(sizeof(float));
    constexpr qsizetype kVertexBytesPerSeg = 4 * kStride;
    constexpr qsizetype kIndexBytesPerSeg = 6 * qsizetype(sizeof(quint32));
    // G67 M1: sizes in qsizetype, never past the scene budget (the builder
    // keeps a scene under it, so this only guards), and never a size that
    // overflows or an index past quint32. Over the budget nothing is drawn.
    static_assert(kMaxSceneSamples <= (std::numeric_limits<qsizetype>::max)() / kVertexBytesPerSeg);
    static_assert(kMaxSceneSamples * 4 <= qsizetype((std::numeric_limits<quint32>::max)()));
    static const QList<Seg> kNone;
    const bool over = segsIn.size() > kMaxSceneSamples;
    const QList<Seg> &segs = over ? kNone : segsIn;
    QByteArray vbuf(segs.size() * kVertexBytesPerSeg, Qt::Uninitialized);
    QByteArray ibuf(segs.size() * kIndexBytesPerSeg, Qt::Uninitialized);
    if (vbuf.size() != segs.size() * kVertexBytesPerSeg || ibuf.size() != segs.size() * kIndexBytesPerSeg) {
        vbuf.clear();
        ibuf.clear();
    }
    const qsizetype count = vbuf.isEmpty() ? 0 : segs.size();
    float *v = reinterpret_cast<float *>(vbuf.data());
    quint32 *idx = reinterpret_cast<quint32 *>(ibuf.data());

    QVector3D mn(FLT_MAX, FLT_MAX, FLT_MAX), mx(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    quint32 base = 0;
    for (qsizetype k = 0; k < count; k++) {
        const Seg &s = segs[k];
        const float len = (s.b - s.a).length();
        const float w = s.dashed ? -s.widthPx : s.widthPx;
        const float c[4] = {float(s.color.redF()), float(s.color.greenF()), float(s.color.blueF()), float(s.color.alphaF())};
        auto put = [&](const QVector3D &self, const QVector3D &other, float side, float isEnd, float along) {
            *v++ = self.x();  *v++ = self.y();  *v++ = self.z();
            *v++ = other.x(); *v++ = other.y(); *v++ = other.z();
            *v++ = side;      *v++ = isEnd;
            *v++ = along;     *v++ = w;
            *v++ = c[0]; *v++ = c[1]; *v++ = c[2]; *v++ = c[3];
        };
        put(s.a, s.b,  1.0f, 0.0f, 0.0f);
        put(s.a, s.b, -1.0f, 0.0f, 0.0f);
        put(s.b, s.a,  1.0f, 1.0f, len);
        put(s.b, s.a, -1.0f, 1.0f, len);
        *idx++ = base;     *idx++ = base + 1; *idx++ = base + 2;
        *idx++ = base + 2; *idx++ = base + 1; *idx++ = base + 3;
        base += 4;
        grow(mn, mx, s.a);
        grow(mn, mx, s.b);
    }
    if (count == 0) {
        mn = mx = QVector3D();
    }

    clear();
    setStride(kStride);
    setPrimitiveType(QQuick3DGeometry::PrimitiveType::Triangles);
    addAttribute(QQuick3DGeometry::Attribute::PositionSemantic, 0, QQuick3DGeometry::Attribute::F32Type);
    addAttribute(QQuick3DGeometry::Attribute::NormalSemantic, 3 * sizeof(float), QQuick3DGeometry::Attribute::F32Type);
    addAttribute(QQuick3DGeometry::Attribute::TexCoord0Semantic, 6 * sizeof(float), QQuick3DGeometry::Attribute::F32Type);
    addAttribute(QQuick3DGeometry::Attribute::TexCoord1Semantic, 8 * sizeof(float), QQuick3DGeometry::Attribute::F32Type);
    addAttribute(QQuick3DGeometry::Attribute::ColorSemantic, 10 * sizeof(float), QQuick3DGeometry::Attribute::F32Type);
    addAttribute(QQuick3DGeometry::Attribute::IndexSemantic, 0, QQuick3DGeometry::Attribute::U32Type);
    setVertexData(vbuf);
    setIndexData(ibuf);
    // Ribbons are widened on screen, so pad the bounds against culling.
    setBounds(mn - QVector3D(50, 50, 50), mx + QVector3D(50, 50, 50));
    _segments = int(count);
    update();
}

// --------------------------------------------------------- MarkerInstances

MarkerInstances::MarkerInstances(QQuick3DObject *parent)
    : QQuick3DInstancing(parent)
{
}

void MarkerInstances::setInstances(const QList<Instance> &instances)
{
    _instances = instances;
    _dirty = true;
    markDirty();
}

QByteArray MarkerInstances::getInstanceBuffer(int *instanceCount)
{
    if (_dirty) {
        _buffer.clear();
        _buffer.reserve(_instances.size() * qsizetype(sizeof(QQuick3DInstancing::InstanceTableEntry)));
        for (const Instance &in : _instances) {
            // Unit scale: Plan3DMarker.vert sizes the marker on screen from customData.x.
            // G67 visual M1: customData.yzw carries the declared sRGB color as is;
            // the instance color is stored linear (an approximation) and is not used.
            const auto e = calculateTableEntry(in.pos, QVector3D(1, 1, 1), QVector3D(), in.color,
                                               QVector4D(in.sizePx, float(in.color.redF()), float(in.color.greenF()), float(in.color.blueF())));
            _buffer.append(reinterpret_cast<const char *>(&e), sizeof(e));
        }
        _count = int(_instances.size());
        _dirty = false;
    }
    if (instanceCount) {
        *instanceCount = _count;
    }
    return _buffer;
}

} // namespace Plan3D
