/****************************************************************************
 *
 * RotorLab GC Plan view 3D (G64, MG2) - the scene object Plan3DView.qml binds.
 *
 * Watches Plan view's PlanMasterController, rebuilds the scene content in
 * C++ when the plan or the loaded ground changes (debounced), and hands the
 * results to the GPU objects: one line mesh, one marker instance table, one
 * ground mesh. It never writes to the plan itself: every edit goes through
 * its Editor (G65, contract section 4a), which writes GC's own items.
 *
 * G65 drag fast path: while an item is dragged the scene is rebuilt at most
 * once per frame from a cached SourcePlan with only the dragged item patched,
 * on the ground already loaded (no new area, no new ground mesh); a full
 * rebuild follows on release.
 *
 ****************************************************************************/

#pragma once

#include "Plan3DBuilder.h"

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QVariantList>
#include <QtGui/QVector3D>

class QQuick3DGeometry;
class QQuick3DInstancing;
class PlanMasterController;
class SimpleMissionItem;

namespace Plan3D {

class Editor;
class Ground;
class GroundMesh;
class LineMesh;
class MarkerInstances;

class SceneModel : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *planMasterController READ planMasterController WRITE setPlanMasterController NOTIFY planMasterControllerChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool darkTheme READ darkTheme WRITE setDarkTheme NOTIFY darkThemeChanged)
    Q_PROPERTY(int selectedVisualIndex READ selectedVisualIndex WRITE setSelectedVisualIndex NOTIFY selectedVisualIndexChanged)
    Q_PROPERTY(QQuick3DGeometry *lineGeometry READ lineGeometry CONSTANT)
    Q_PROPERTY(QQuick3DInstancing *markerInstancing READ markerInstancing CONSTANT)
    Q_PROPERTY(QQuick3DGeometry *groundGeometry READ groundGeometry CONSTANT)
    /// G67 M2: strike segments and strike markers again, drawn on top of the
    /// terrain (Plan3DView.qml, onTop) so a strike under the ground stays visible.
    Q_PROPERTY(QQuick3DGeometry *strikeLineGeometry READ strikeLineGeometry CONSTANT)
    Q_PROPERTY(QQuick3DInstancing *strikeMarkerInstancing READ strikeMarkerInstancing CONSTANT)
    Q_PROPERTY(QVector3D center READ center NOTIFY sceneChanged)
    Q_PROPERTY(qreal radius READ radius NOTIFY sceneChanged)
    Q_PROPERTY(int revision READ revision NOTIFY sceneChanged)
    Q_PROPERTY(int samplesStrike READ samplesStrike NOTIFY sceneChanged)
    Q_PROPERTY(int samplesGroundContact READ samplesGroundContact NOTIFY sceneChanged)
    Q_PROPERTY(int samplesUnknown READ samplesUnknown NOTIFY sceneChanged)
    Q_PROPERTY(int samplesNoStrike READ samplesNoStrike NOTIFY sceneChanged)
    Q_PROPERTY(int pointsUnknownGround READ pointsUnknownGround NOTIFY sceneChanged)
    Q_PROPERTY(int markerCount READ markerCount NOTIFY sceneChanged)
    Q_PROPERTY(int segmentCount READ segmentCount NOTIFY sceneChanged)
    Q_PROPERTY(QStringList warnings READ warnings NOTIFY sceneChanged)
    Q_PROPERTY(QStringList strikePlaces READ strikePlaces NOTIFY sceneChanged)   ///< G67 M2: where the route meets the terrain
    Q_PROPERTY(bool hasFences READ hasFences NOTIFY sceneChanged)                ///< G67 a11y M3: fence legend rows
    /// G67 M1: drawing limits reached, shown on the view itself; empty when none.
    Q_PROPERTY(QString limitWarning READ limitWarning NOTIFY sceneChanged)
    Q_PROPERTY(bool hasSpline READ hasSpline NOTIFY sceneChanged)
    Q_PROPERTY(bool hasTerrainFrame READ hasTerrainFrame NOTIFY sceneChanged)
    Q_PROPERTY(qreal buildMs READ buildMs NOTIFY sceneChanged)
    Q_PROPERTY(qreal emitMs READ emitMs NOTIFY sceneChanged)     ///< G65: time spent in the last drag frame's sceneChanged handlers
    Q_PROPERTY(QString terrainSource READ terrainSource NOTIFY terrainSourceChanged)
    Q_PROPERTY(bool groundLoading READ groundLoading NOTIFY sceneChanged)
    Q_PROPERTY(int groundKnownPercent READ groundKnownPercent NOTIFY sceneChanged)
    Q_PROPERTY(qreal groundWidth READ groundWidth NOTIFY sceneChanged)
    Q_PROPERTY(qreal groundDepth READ groundDepth NOTIFY sceneChanged)
    Q_PROPERTY(QVariantList groundArea READ groundArea NOTIFY sceneChanged)   ///< [south, west, north, east]
    Q_PROPERTY(QObject *editor READ editorObject CONSTANT)                    ///< G65: Plan3D::Editor
    Q_PROPERTY(bool dragging READ dragging NOTIFY draggingChanged)

public:
    explicit SceneModel(QObject *parent = nullptr);
    ~SceneModel() override;

    QObject *planMasterController() const;
    void setPlanMasterController(QObject *pmc);
    bool active() const { return _active; }
    void setActive(bool active);
    bool darkTheme() const { return _dark; }
    void setDarkTheme(bool dark);
    int selectedVisualIndex() const { return _selected; }
    void setSelectedVisualIndex(int index);

    QQuick3DGeometry *lineGeometry() const;
    QQuick3DInstancing *markerInstancing() const;
    QQuick3DGeometry *groundGeometry() const;
    QQuick3DGeometry *strikeLineGeometry() const;
    QQuick3DInstancing *strikeMarkerInstancing() const;

    QVector3D center() const { return _center; }
    qreal radius() const { return _radius; }
    int revision() const { return _revision; }
    int samplesStrike() const { return _scene.stats.samplesStrike; }
    int samplesGroundContact() const { return _scene.stats.samplesGroundContact; }
    int samplesUnknown() const { return _scene.stats.samplesUnknown; }
    int samplesNoStrike() const { return _scene.stats.samplesNoStrike; }
    int pointsUnknownGround() const { return _scene.stats.pointsUnknownGround; }
    int markerCount() const { return int(_scene.markers.size()); }
    int segmentCount() const;
    QStringList warnings() const { return _scene.warnings; }
    QStringList strikePlaces() const { return _strikePlaces; }
    bool hasFences() const { return _hasFences; }
    QString limitWarning() const { return _scene.limitWarnings.join(QLatin1Char('\n')); }
    bool hasSpline() const { return _scene.hasSpline; }
    bool hasTerrainFrame() const { return _scene.hasTerrainFrame; }
    qreal buildMs() const { return _buildMs; }
    qreal emitMs() const { return _emitMs; }
    QString terrainSource() const;
    bool groundLoading() const;
    int groundKnownPercent() const;
    qreal groundWidth() const { return _groundWidth; }
    qreal groundDepth() const { return _groundDepth; }
    QVariantList groundArea() const;
    Editor *editor() const { return _editor; }
    QObject *editorObject() const;
    bool dragging() const { return _dragging; }

    /// G65: starts a drag of item `vi` under the cursor at (x, y): on the
    /// horizontal plane at the point's height, or, with `height`, up and down
    /// on the vertical plane through the point that faces the camera. False
    /// (with the editor's message) when the item cannot be edited that way.
    Q_INVOKABLE bool beginDragAt(QObject *camera, qreal width, qreal height, qreal x, qreal y, int vi, bool heightDrag);
    /// Moves the dragged item to follow the cursor.
    Q_INVOKABLE void dragTo(QObject *camera, qreal width, qreal height, qreal x, qreal y);
    Q_INVOKABLE void endDrag();
    /// The drag's new position (scene drag, autorun): drawn at once, written to
    /// GC's item at most every kCommitMs, and always on endDrag.
    Q_INVOKABLE void dragToLatLon(double lat, double lon);
    Q_INVOKABLE void dragToAmsl(double amsl);
    static constexpr int kCommitMs = 100;
    /// The AMSL item `vi` is drawn at (NaN when unknown or not drawn).
    Q_INVOKABLE double markerAmsl(int vi) const;

    /// G65: the selected item's heading or gimbal handle under (x, y)
    /// within 14 px: "heading", "gimbal" or "".
    Q_INVOKABLE QString pickHandle(QObject *camera, qreal width, qreal height, qreal x, qreal y) const;
    /// Drags a handle of the selected item: "heading" (horizontal), "gimbal"
    /// (pitch, or with `yaw` the gimbal yaw). Not a point drag: no fast path.
    Q_INVOKABLE void dragHandleTo(QObject *camera, qreal width, qreal height, qreal x, qreal y, const QString &handle, bool yaw);

    /// Labels to draw this frame: the selected item and up to `maxNear`
    /// markers within 80 px of the cursor (mouseX < 0: none). Each entry is
    /// {x, y, text, selected} in view pixels. `camera` is the View3D's camera.
    Q_INVOKABLE QVariantList labels(QObject *camera, qreal width, qreal height, qreal mouseX, qreal mouseY, int maxNear) const;

    /// Visual item index of the marker nearest to (x, y) within 24 px, or -1.
    Q_INVOKABLE int pick(QObject *camera, qreal width, qreal height, qreal x, qreal y) const;

    /// Section 4 colors, by theme (for the legend).
    Q_INVOKABLE QColor stateColor(const QString &state) const;

signals:
    void planMasterControllerChanged();
    void activeChanged();
    void darkThemeChanged();
    void selectedVisualIndexChanged();
    void sceneChanged();
    void terrainSourceChanged();
    void draggingChanged();

private:
    /// E-2 and CL-2: a mode 3 point whose AMSL GC is still querying is drawn
    /// at its last height, as unknown, until GC answers; after 10 s without an
    /// answer it stays unknown, labelled "terrain height not available".
    struct Hold {
        QPointer<SimpleMissionItem> item;
        double  amsl = 0;
        qint64  untilMs = 0;            ///< 0 while the drag runs
        bool    expired = false;        ///< no answer in time: stays unknown (F4)
    };

    void _scheduleRebuild();
    void _rebuild();
    void _upload();
    void _uploadLinesAndMarkers();
    void _uploadGround();
    void _onEditorDragging();
    void _onItemEdited(int vi, double drawnAmsl);
    void _patchDragSource();
    Hold *_holdFor(SimpleMissionItem *item);
    void _applyHolds(SourcePlan &plan);
    void _dragRebuild();
    void _scheduleDragFrame();
    void _commitDrag();
    bool _ray(QObject *camera, qreal width, qreal height, qreal x, qreal y, QVector3D &origin, QVector3D &dir) const;
    const Marker *_markerFor(int vi) const;
    double _handleLength() const;
    void _connectPlan();
    void _connectItems();

    QColor _lineColor(const LineSeg &s) const;
    QColor _markerColor(const Marker &m) const;

    QPointer<PlanMasterController> _pmc;
    QList<QMetaObject::Connection> _connections;
    QList<QMetaObject::Connection> _itemConnections;
    bool        _active = false;
    bool        _dark = true;
    int         _selected = -1;
    bool        _pending = false;
    QTimer      _rebuildTimer;

    // G65 drag fast path
    bool        _dragging = false;
    bool        _dragDirty = false;
    SourcePlan  _dragSource;
    QTimer      _dragTimer;
    QTimer      _commitTimer;
    bool        _pendingMove = false;
    bool        _pendingHeight = false;
    double      _pendLat = 0, _pendLon = 0, _pendAmsl = 0;
    QList<Hold> _holds;
    QTimer      _holdTimer;
    int         _dragVi = -1;
    bool        _dragHeight = false;
    double      _dragPlaneY = 0;        ///< scene y of the horizontal drag plane
    double      _grabDx = 0, _grabDz = 0;   ///< marker minus cursor hit at the start, scene metres
    double      _grabDy = 0;
    // Handles of the selected item, in scene coordinates (valid when set).
    bool        _headingTipValid = false;
    QVector3D   _headingTip;
    bool        _gimbalTipValid = false;
    QVector3D   _gimbalTip;

    Ground          *_ground = nullptr;
    LineMesh        *_lines = nullptr;
    MarkerInstances *_markers = nullptr;
    LineMesh        *_strikeLines = nullptr;
    MarkerInstances *_strikeMarkers = nullptr;
    GroundMesh      *_groundMesh = nullptr;
    Editor          *_editor = nullptr;     ///< G65; after _groundMesh, as initialised

    Scene       _scene;
    QStringList _strikePlaces;
    bool        _hasFences = false;
    QVector3D   _center;
    qreal       _radius = 100;
    int         _revision = 0;
    qreal       _buildMs = 0;
    qreal       _emitMs = 0;
    qreal       _groundWidth = 0;
    qreal       _groundDepth = 0;
};

/// G67 M2: each item, leg, pattern or rally point with a strike (a red
/// sample or a red marker), in route order, for the legend: "item 5 (1.8 m
/// below the loaded ground)", "leg 4 to 5", "Rally 1". Never empty when the
/// scene has a strike. Text only; it reads the scene and changes nothing.
QStringList strikePlacesOf(const Scene &scene);

/// G67 M2: a marker's hover and selected label, with its clearance state in
/// words (CL-5), so a strike is never shown only by red.
QString markerLabelText(const Marker &m);

/// Registers the QML types (RotorLab.Plan3D 1.0). Windows and Linux only.
void registerQmlTypes();

} // namespace Plan3D
