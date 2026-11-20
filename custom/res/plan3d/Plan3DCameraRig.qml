/****************************************************************************
 *
 * RotorLab GC Plan view 3D (G64, MG2) - orbit camera around the plan.
 *
 ****************************************************************************/

import QtQuick
import QtQuick3D

Node {
    id: rig

    property vector3d target:   Qt.vector3d(0, 0, 0)
    property real   yaw:        30
    property real   pitch:      -40
    property real   distance:   800
    property real   fov:        45
    property real   sceneRadius: 500
    property real   viewHeight: 600
    property int    revision:   0       ///< bumps on every camera move
    readonly property alias camera: cam

    position:       target
    eulerRotation:  Qt.vector3d(pitch, yaw, 0)

    PerspectiveCamera {
        id:             cam
        position:       Qt.vector3d(0, 0, rig.distance)
        fieldOfView:    rig.fov
        clipNear:       Math.max(0.5, rig.distance / 2000)
        clipFar:        Math.max(5000, rig.distance * 20 + rig.sceneRadius * 8)
    }

    function fit(center, radius) {
        target = center
        distance = Math.max(60, radius / Math.tan(fov * Math.PI / 360) * 1.1)
        revision++
    }

    function topView() {
        pitch = -89
        revision++
    }

    function rotate(dYaw, dPitch) {
        yaw = (yaw + dYaw) % 360
        pitch = Math.max(-89, Math.min(-3, pitch + dPitch))
        revision++
    }

    /// Moves the target with the ground under the cursor, dx, dy in px.
    function pan(dx, dy) {
        var worldPerPx = distance * 2 * Math.tan(fov * Math.PI / 360) / Math.max(1, viewHeight)
        var yr = yaw * Math.PI / 180
        var right = Qt.vector3d(Math.cos(yr), 0, -Math.sin(yr))
        var fwd   = Qt.vector3d(-Math.sin(yr), 0, -Math.cos(yr))
        target = target.minus(right.times(dx * worldPerPx)).plus(fwd.times(dy * worldPerPx))
        revision++
    }

    function zoom(factor) {
        distance = Math.max(20, Math.min(200000, distance * factor))
        revision++
    }
}
