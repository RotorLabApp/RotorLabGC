/****************************************************************************
 *
 * RotorLab GC Plan view 3D editing (G65, MG3; contract section 4a).
 *
 * Every 3D edit goes through this class, and this class writes only the
 * objects a 2D edit writes (E-1): the SimpleMissionItem coordinate, its
 * altitude Fact, its command, its parameters and its camera section, and
 * MissionController's own insert and remove. There is no 3D copy of the
 * plan. The class has no Qt Quick in it, so the parity tests (E-9) drive it
 * directly; Plan3DSceneModel turns mouse drags into these calls.
 *
 ****************************************************************************/

#pragma once

#include "Plan3DMath.h"

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>

#include <functional>
#include <QtCore/QList>

class PlanMasterController;
class SimpleMissionItem;

namespace Plan3D {

class Editor : public QObject
{
    Q_OBJECT
    /// E-5: the Plan view's mission layer is the editing layer (set by Plan3DView from PlanView).
    Q_PROPERTY(bool missionLayer READ missionLayer WRITE setMissionLayer NOTIFY missionLayerChanged)
    Q_PROPERTY(bool dragging READ dragging NOTIFY draggingChanged)
    Q_PROPERTY(int dragVisualIndex READ dragVisualIndex NOTIFY draggingChanged)
    /// Why the last edit was not made, or a note about it (E-2, E-6); empty when there is none.
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    /// E-8 release gate: gimbal yaw is drawn and edited only after the yaw
    /// frame per firmware is checked in SITL. Off until that result is recorded.
    Q_PROPERTY(bool gimbalYawEditing READ gimbalYawEditing CONSTANT)

public:
    explicit Editor(QObject *parent = nullptr);

    void setPlanMasterController(PlanMasterController *pmc);
    PlanMasterController *planMasterController() const { return _pmc; }

    /// The references a height edit needs (E-2): G(p) from the loaded ground
    /// (NaN when unknown) and R from A-0 (NaN when unknown).
    void setReferences(GroundFn ground, std::function<double()> launchRef);

    bool missionLayer() const { return _missionLayer; }
    void setMissionLayer(bool missionLayer);
    bool dragging() const { return _dragVi >= 0; }
    int dragVisualIndex() const { return _dragVi; }
    QString message() const { return _message; }
    static constexpr bool kGimbalYawEditing = false;
    /// IB-6 (contract revision 9): plan extent from home, and drawn heights.
    static constexpr double kPlanExtentM = kMaxPlanExtentM;          // Plan3DMath.h, g64-bounds
    static constexpr double kDrawnHeightLimitM = kMaxDrawnHeightM;
    static bool gimbalYawEditing() { return kGimbalYawEditing; }
    /// The item being dragged (bound at beginDrag), null when none (M2).
    SimpleMissionItem *dragItem() const;

    // ---- pure rules (section 4a), tested without a plan

    /// E-3: an altitude in metres rounded to 0.1 m, through the same decimal
    /// text 2D's field holds (QString::number(x, 'f', 1)). Height edits use the
    /// user's own unit (revision 10): 0.1 ft in feet, via the altitude Fact.
    static double snapAltitude(double metres);
    /// E-3: a written latitude or longitude is rounded to 1e-7 degrees (same method, 'f', 7).
    static double snapDegrees(double degrees);
    /// E-3: a heading is rounded to 0.1 degree, in 0 up to but not including 360.
    static double snapHeading(double degrees);
    /// E-3: a gimbal angle is rounded to 0.1 degree and clamped to [min, max].
    static double snapGimbal(double degrees, double min, double max);
    /// E-2: the Altitude value an item in GC altitude mode `altitudeMode`
    /// (1 relative, 2 AMSL, 3 calculated above terrain, 4 terrain frame) gets
    /// for a point at `amsl`. NaN when the reference the mode needs is unknown.
    static double altitudeForAmsl(int altitudeMode, double amsl, double launchRef, double groundAtPoint);

    // ---- what may be edited (E-4, E-5)

    /// The item at visual index `vi` when it is a simple mission item, else null.
    SimpleMissionItem *simpleItem(int vi) const;
    /// Empty when `vi` may be moved horizontally, else why not.
    Q_INVOKABLE QString moveBlockedReason(int vi) const;
    /// Empty when the height of `vi` may be changed, else why not (E-2 references included).
    Q_INVOKABLE QString heightBlockedReason(int vi) const;
    Q_INVOKABLE bool canMove(int vi) const { return moveBlockedReason(vi).isEmpty(); }
    Q_INVOKABLE bool canChangeHeight(int vi) const { return heightBlockedReason(vi).isEmpty(); }

    // ---- edits

    /// Marks the start of a drag of `vi` (the scene switches to its fast path).
    Q_INVOKABLE bool beginDrag(int vi);
    Q_INVOKABLE void endDrag();
    /// Horizontal move (E-3 snapping). False, with `message`, when not allowed.
    Q_INVOKABLE bool moveTo(int vi, double lat, double lon);
    /// Height edit: the item's Altitude in its own mode for a point at `amsl` (E-2, E-3).
    Q_INVOKABLE bool setHeightAmsl(int vi, double amsl);

    // ---- spline (E-6), ArduPilot multirotor only (S-2, S-3)

    /// E-6's note, shown before a change to a spline waypoint.
    Q_INVOKABLE QString splineNote() const;
    /// Empty when the leg into `vi` may be switched between straight and spline.
    Q_INVOKABLE QString splineBlockedReason(int vi) const;
    Q_INVOKABLE bool isSpline(int vi) const;
    /// Sets the leg into `vi` straight (16) or spline (82) with GC's own
    /// command change, then writes back the altitude, its mode and the hold.
    Q_INVOKABLE bool setSpline(int vi, bool spline);

    // ---- heading (E-7)

    /// Empty when a heading may be set at `vi`.
    Q_INVOKABLE QString headingBlockedReason(int vi) const;
    /// The heading set at `vi` in degrees (PX4: parameter 4; ArduPilot: the
    /// CONDITION_YAW right after it), NaN when none is set.
    Q_INVOKABLE double heading(int vi) const;
    /// Sets (or adds) the heading at `vi` (E-3: 0.1 degree, 0 to under 360).
    Q_INVOKABLE bool setHeading(int vi, double degrees);
    /// Removes the heading at `vi`.
    Q_INVOKABLE bool clearHeading(int vi);
    /// ArduPilot: the CONDITION_YAW (115) right after `vi` when it is the
    /// heading E-7 writes (absolute, rate 0, direction 0), else null.
    SimpleMissionItem *yawItemAfter(int vi) const;
    /// ArduPilot: a CONDITION_YAW right after `vi` that is not E-7's form
    /// (relative, or with a rate or direction), else null. 3D never edits it.
    SimpleMissionItem *otherTurnAfter(int vi) const;
    /// A note for the panel when a turn command 3D does not edit follows `vi`, else empty.
    Q_INVOKABLE QString headingNote(int vi) const;

    // ---- gimbal (E-8), through the waypoint's camera section

    /// Empty when the gimbal may be set at `vi`.
    Q_INVOKABLE QString gimbalBlockedReason(int vi) const;
    Q_INVOKABLE bool hasGimbal(int vi) const;
    /// Degrees, NaN when no gimbal setting is on the waypoint.
    Q_INVOKABLE double gimbalPitch(int vi) const;
    Q_INVOKABLE double gimbalYaw(int vi) const;
    /// Pitch, as 2D's camera section writes it (DO_MOUNT_CONTROL, 205).
    Q_INVOKABLE bool setGimbalPitch(int vi, double degrees);
    /// Yaw, relative to the vehicle heading at `vi`; refused while no heading
    /// is set there, and while the E-8 SITL gate is closed (kGimbalYawEditing).
    Q_INVOKABLE bool setGimbalYaw(int vi, double degrees);
    /// Turns the waypoint's gimbal setting off (2D's Gimbal check box).
    Q_INVOKABLE bool clearGimbal(int vi);

    Q_INVOKABLE void clearMessage() { _setMessage(QString()); }
    /// True when `vi` may be edited that way (`height`: its height, else its
    /// position); otherwise sets `message` to the reason and returns false.
    Q_INVOKABLE bool checkEdit(int vi, bool height);

signals:
    void missionLayerChanged();
    void draggingChanged();
    void messageChanged();
    /// After every write to item `vi`. `drawnAmsl` is the AMSL the edit put
    /// the point at (NaN for a horizontal move), for E-2's mode 3 hold.
    void itemEdited(int vi, double drawnAmsl);

private:
    QString _commonBlockedReason(int vi) const;
    /// A-1 (M1): the reason when the item's altitude mode and frame disagree, else empty.
    QString _datumBlockedReason(SimpleMissionItem *item) const;
    /// M2: while a drag runs, an edit must land on the dragged item itself.
    bool _dragTargetOk(int vi);
    double _groundAt(double lat, double lon) const;
    void _setMessage(const QString &message);
    void _connectPlan();

    QPointer<PlanMasterController> _pmc;
    QList<QMetaObject::Connection> _connections;
    GroundFn                    _ground;
    std::function<double()>     _launchRef;
    bool                        _missionLayer = true;
    int                         _dragVi = -1;
    QPointer<SimpleMissionItem> _dragItem;
    QString                     _message;
    bool                        _limitMessage = false;  ///< the message is a drawing-limit warning
};

} // namespace Plan3D
