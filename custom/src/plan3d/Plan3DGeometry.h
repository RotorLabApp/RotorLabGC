/****************************************************************************
 *
 * RotorLab GC Plan view 3D (G64, MG2) - the scene's GPU data.
 *
 * LineMesh: every leg, stem, fence and outline of the plan in one
 * QQuick3DGeometry, drawn as screen-space ribbons by Plan3DLine.vert, so the
 * whole route is one draw call however long the plan is.
 * MarkerInstances: every point marker as one instanced draw.
 * The ground mesh is in Plan3DGroundMesh.h.
 *
 ****************************************************************************/

#pragma once

#include <QtGui/QColor>
#include <QtGui/QVector3D>
#include <QtGui/QVector4D>
#include <QtQuick3D/QQuick3DGeometry>
#include <QtQuick3D/QQuick3DInstancing>

namespace Plan3D {

class LineMesh : public QQuick3DGeometry
{
    Q_OBJECT

public:
    explicit LineMesh(QQuick3DObject *parent = nullptr);

    struct Seg {
        QVector3D   a, b;
        QColor      color;
        float       widthPx = 2.0f;
        bool        dashed = false;
    };
    void setSegments(const QList<Seg> &segs);
    int segmentCount() const { return _segments; }

private:
    int _segments = 0;
};

class MarkerInstances : public QQuick3DInstancing
{
    Q_OBJECT

public:
    explicit MarkerInstances(QQuick3DObject *parent = nullptr);

    struct Instance {
        QVector3D   pos;
        QColor      color;
        float       sizePx = 10.0f;
    };
    void setInstances(const QList<Instance> &instances);

protected:
    QByteArray getInstanceBuffer(int *instanceCount) override;

private:
    QByteArray  _buffer;
    int         _count = 0;
    bool        _dirty = true;
    QList<Instance> _instances;
};

} // namespace Plan3D
