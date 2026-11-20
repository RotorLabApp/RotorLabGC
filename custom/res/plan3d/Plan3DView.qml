/****************************************************************************
 *
 * RotorLab GC Plan view 3D (G64, MG2). 3D view of the plan in Plan view,
 * Windows and Linux only. Loaded by PlanView.qml's "3D" toggle. G65: points
 * are edited here through the scene model's editor (contract section 4a).
 *
 * One line mesh for every leg, stem, fence and outline; one instanced draw
 * for every marker; labels only for the selected item and near the cursor;
 * MSAA. The ground layer is in Plan3DGroundLayer.qml, the legend in
 * Plan3DLegend.qml and the camera rig in Plan3DCameraRig.qml.
 *
 ****************************************************************************/

import QtQuick
import QtQuick.Controls
import QtQuick3D

import QGroundControl
import QGroundControl.Controls
import QGroundControl.Palette
import QGroundControl.ScreenTools

import RotorLab.Plan3D

Item {
    id: root

    property var    planMasterController:   null
    property var    planView:               null        ///< PlanView.qml root, for the autorun only
    property bool   open:                   false
    property bool   editingAllowed:         true        ///< E-5: PlanView's mission layer is the editing layer
    property real   leftInset:              0           ///< width covered on the left (PlanView's tool strip)
    property real   rightInset:             0           ///< width covered on the right (PlanView's right panel)

    // The part of the view not under the tool strip or the right panel. The
    // hint, the edit panel, the edit message and Fit / Top center in it.
    readonly property real _visibleWidth:   Math.max(width * 0.3, width - leftInset - rightInset)
    readonly property real _centerOffset:   width - leftInset - rightInset >= width * 0.3 ? (leftInset - rightInset) / 2 : 0

    readonly property var   sceneModel:     model
    readonly property var   view3d:         view
    readonly property var   cameraRig:      rig
    readonly property var   _missionController: planMasterController ? planMasterController.missionController : null

    property real   _mouseX:    -1
    property real   _mouseY:    -1
    property var    labelList:  []
    property bool   _fitted:    false

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    readonly property var editor: model.editor

    Binding {
        target:     model.editor
        property:   "missionLayer"
        value:      root.editingAllowed
    }

    Plan3DSceneModel {
        id:                     model
        planMasterController:   root.planMasterController
        active:                 root.visible || root.open
        darkTheme:              qgcPal.globalTheme === QGCPalette.Dark
        selectedVisualIndex:    root._missionController ? root._missionController.currentPlanViewVIIndex : -1
        onSceneChanged: {
            if (!root._fitted && model.markerCount > 0) {
                root.fit()
                root._fitted = true
            }
            root.updateLabels()
        }
    }

    Connections {
        target: root._missionController
        function onVisualItemsChanged() { root._fitted = false }
    }

    // Kept for the autorun and the buttons; the rig does the work.
    function fit()                  { rig.fit(model.center, model.radius) }
    function rotate(dYaw, dPitch)   { rig.rotate(dYaw, dPitch) }
    function pan(dx, dy)            { rig.pan(dx, dy) }
    function zoom(f)                { rig.zoom(f) }

    function updateLabels() {
        if (!visible) {
            return
        }
        var l = model.labels(rig.camera, view.width, view.height, _mouseX, _mouseY, 4)
        // Recreating label delegates costs a frame; skip when nothing is shown either way.
        if (l.length === 0 && labelList.length === 0) {
            return
        }
        labelList = l
    }

    Timer {
        id:             labelTimer
        interval:       0
        onTriggered:    root.updateLabels()
    }

    View3D {
        id:             view
        anchors.fill:   parent
        camera:         rig.camera

        environment: SceneEnvironment {
            backgroundMode:         SceneEnvironment.Color
            clearColor:             qgcPal.globalTheme === QGCPalette.Dark ? "#0d1117" : "#e9eef4"
            antialiasingMode:       SceneEnvironment.MSAA
            antialiasingQuality:    SceneEnvironment.High
        }

        Plan3DCameraRig {
            id:             rig
            sceneRadius:    model.radius
            viewHeight:     view.height
            onRevisionChanged: labelTimer.restart()
        }

        Plan3DGroundLayer {
            id:             groundLayer
            sceneModel:     model
            cameraDistance: rig.distance
        }

        // Every leg, stem, fence and outline: one mesh, one draw call.
        Model {
            id:         lineModel
            geometry:   model.lineGeometry
            materials: [
                CustomMaterial {
                    shadingMode:    CustomMaterial.Unshaded
                    cullMode:       Material.NoCulling
                    vertexShader:   "qrc:/rotorlab/plan3d/Plan3DLine.vert"
                    fragmentShader: "qrc:/rotorlab/plan3d/Plan3DLine.frag"
                    property size viewportSize: Qt.size(Math.max(1, view.width), Math.max(1, view.height))
                    property real dashLength:   Math.max(1.5, rig.distance / 120)
                    property real onTop:        0
                }
            ]
        }

        // Every marker: one instanced draw, a constant size on screen.
        Model {
            id:         markerModel
            source:     "#Sphere"
            instancing: model.markerInstancing
            materials: [
                CustomMaterial {
                    shadingMode:    CustomMaterial.Unshaded
                    vertexShader:   "qrc:/rotorlab/plan3d/Plan3DMarker.vert"
                    fragmentShader: "qrc:/rotorlab/plan3d/Plan3DMarker.frag"
                    property size viewportSize: Qt.size(Math.max(1, view.width), Math.max(1, view.height))
                    property real onTop:        0
                }
            ]
        }

        // G67 M2: the strike segments and strike markers again, drawn on top
        // of the terrain (onTop presses their depth toward the near plane),
        // so a strike under the ground is never hidden by it.
        Model {
            id:         strikeLineModel
            geometry:   model.strikeLineGeometry
            materials: [
                CustomMaterial {
                    shadingMode:    CustomMaterial.Unshaded
                    cullMode:       Material.NoCulling
                    vertexShader:   "qrc:/rotorlab/plan3d/Plan3DLine.vert"
                    fragmentShader: "qrc:/rotorlab/plan3d/Plan3DLine.frag"
                    property size viewportSize: Qt.size(Math.max(1, view.width), Math.max(1, view.height))
                    property real dashLength:   Math.max(1.5, rig.distance / 120)
                    property real onTop:        1
                }
            ]
        }
        Model {
            id:         strikeMarkerModel
            source:     "#Sphere"
            instancing: model.strikeMarkerInstancing
            materials: [
                CustomMaterial {
                    shadingMode:    CustomMaterial.Unshaded
                    vertexShader:   "qrc:/rotorlab/plan3d/Plan3DMarker.vert"
                    fragmentShader: "qrc:/rotorlab/plan3d/Plan3DMarker.frag"
                    property size viewportSize: Qt.size(Math.max(1, view.width), Math.max(1, view.height))
                    property real onTop:        1
                }
            ]
        }
    }

    // Left drag orbits, right drag (or Shift + left) pans, the wheel zooms;
    // a click selects the nearest item. G65: a left drag that starts on an
    // editable point moves it over the ground; with Ctrl (or on a takeoff or
    // a land) it changes its height.
    MouseArea {
        id:                 mouse
        anchors.fill:       parent
        hoverEnabled:       true
        acceptedButtons:    Qt.LeftButton | Qt.RightButton
        property real lastX: 0
        property real lastY: 0
        property real pressX: 0
        property real pressY: 0
        property bool panning: false
        property int  dragCandidate: -1     ///< visual index under the press, may become a drag
        property bool heightDrag: false
        property bool editDrag: false       ///< a G65 edit drag is running
        property string handleDrag: ""      ///< "heading" or "gimbal" while a handle is dragged
        property bool handleYaw: false
        property int  blockedVi: -1         ///< visual index under the press that cannot be edited (visual-ux M1)
        property bool blockedHeight: false

        onPressed: (m) => {
            root.forceActiveFocus()
            lastX = pressX = m.x
            lastY = pressY = m.y
            panning = (m.button === Qt.RightButton) || (m.modifiers & Qt.ShiftModifier)
            editDrag = false
            dragCandidate = -1
            blockedVi = -1
            handleDrag = ""
            if (!panning && m.button === Qt.LeftButton && root.editingAllowed) {
                handleDrag = model.pickHandle(rig.camera, view.width, view.height, m.x, m.y)
                handleYaw = (m.modifiers & Qt.ControlModifier) !== 0
                if (handleDrag !== "") {
                    return
                }
                var vi = model.pick(rig.camera, view.width, view.height, m.x, m.y)
                if (vi > 0 && (model.editor.canMove(vi) || model.editor.canChangeHeight(vi))) {
                    dragCandidate = vi
                    heightDrag = (m.modifiers & Qt.ControlModifier) || (!model.editor.canMove(vi) && model.editor.canChangeHeight(vi))
                } else if (vi > 0) {
                    // RotorLab 5.3.0 review (visual-ux M1): a point that
                    // cannot be edited still says why once the drag starts.
                    blockedVi = vi
                    blockedHeight = (m.modifiers & Qt.ControlModifier) !== 0
                }
            }
        }
        onPositionChanged: (m) => {
            root._mouseX = m.x
            root._mouseY = m.y
            if (pressed && handleDrag !== "") {
                model.dragHandleTo(rig.camera, view.width, view.height, m.x, m.y, handleDrag, handleYaw)
                messageTimer.restart()
                return
            }
            if (pressed && editDrag) {
                model.dragTo(rig.camera, view.width, view.height, m.x, m.y)
                labelTimer.restart()
                return
            }
            if (pressed && dragCandidate > 0) {
                if (Math.abs(m.x - pressX) + Math.abs(m.y - pressY) < 4) {
                    return
                }
                editDrag = model.beginDragAt(rig.camera, view.width, view.height, pressX, pressY, dragCandidate, heightDrag)
                dragCandidate = -1
                if (editDrag) {
                    model.dragTo(rig.camera, view.width, view.height, m.x, m.y)
                    messageTimer.restart()
                    return
                }
                messageTimer.restart()
                return      // not editable that way: the message says why
            }
            if (pressed && blockedVi > 0 && Math.abs(m.x - pressX) + Math.abs(m.y - pressY) >= 4) {
                model.editor.checkEdit(blockedVi, blockedHeight)    // sets the message: why not
                messageTimer.restart()
                blockedVi = -1
                // then orbit, as before
            }
            if (pressed) {
                var dx = m.x - lastX, dy = m.y - lastY
                lastX = m.x
                lastY = m.y
                if (panning) {
                    rig.pan(dx, dy)
                } else {
                    rig.rotate(-dx * 0.3, -dy * 0.3)
                }
            } else {
                labelTimer.restart()
            }
        }
        onExited: {
            root._mouseX = -1
            labelTimer.restart()
        }
        onReleased: (m) => {
            if (handleDrag !== "") {
                handleDrag = ""
                return
            }
            if (editDrag) {
                editDrag = false
                model.endDrag()
                return
            }
            dragCandidate = -1
            blockedVi = -1
            if (Math.abs(m.x - pressX) + Math.abs(m.y - pressY) < 4 && m.button === Qt.LeftButton && root._missionController) {
                var vi = model.pick(rig.camera, view.width, view.height, m.x, m.y)
                if (vi > 0 && vi < root._missionController.visualItems.count) {
                    root._missionController.setCurrentPlanViewSeqNum(root._missionController.visualItems.get(vi).sequenceNumber, false)
                }
            }
        }
        onWheel: (w) => rig.zoom(w.angleDelta.y > 0 ? 0.88 : 1.0 / 0.88)
        onCanceled: {
            blockedVi = -1
            if (editDrag) {
                editDrag = false
                model.endDrag()
            }
            dragCandidate = -1
        }
    }

    // G65: why an edit was not made (E-2, E-4, E-5), or a note about it.
    Timer {
        id:             messageTimer
        interval:       5000
        onTriggered:    model.editor.clearMessage()
    }

    // L7: a message from a panel button times out too.
    Connections {
        target: model.editor
        function onMessageChanged() {
            if (model.editor.message !== "") {
                messageTimer.restart()
            }
        }
    }

    Rectangle {
        id:                         editMessageBox
        anchors.horizontalCenter:   parent.horizontalCenter
        anchors.horizontalCenterOffset: root._centerOffset
        anchors.bottom:             parent.bottom
        anchors.bottomMargin:       root._bottomBase + (editPanel.visible ? editPanel.height + ScreenTools.defaultFontPixelHeight / 2 : 0)
        visible:                    model.editor.message !== ""
        width:                      Math.min(root._visibleWidth * 0.8, editMessage.implicitWidth + ScreenTools.defaultFontPixelWidth * 2)
        height:                     editMessage.implicitHeight + ScreenTools.defaultFontPixelHeight / 2
        radius:                     ScreenTools.defaultFontPixelWidth / 2
        color:                      Qt.rgba(qgcPal.window.r, qgcPal.window.g, qgcPal.window.b, 0.9)
        QGCLabel {
            id:                     editMessage
            anchors.centerIn:       parent
            width:                  Math.min(implicitWidth, root._visibleWidth * 0.8 - ScreenTools.defaultFontPixelWidth * 2)
            wrapMode:               Text.WordWrap
            text:                   model.editor.message
        }
    }

    // Touch: pinch zooms, twist turns the camera around the plan (one finger
    // drags as the mouse does).
    PinchHandler {
        target:     null
        property real _lastScale:    1
        property real _lastRotation: 0
        onActiveChanged: {
            _lastScale = 1
            _lastRotation = 0
        }
        onActiveScaleChanged: {
            if (activeScale > 0) {
                rig.zoom(_lastScale / activeScale)
                _lastScale = activeScale
            }
        }
        onActiveRotationChanged: {
            rig.rotate(activeRotation - _lastRotation, 0)
            _lastRotation = activeRotation
        }
    }

    // Keys: arrows orbit, + and - zoom, F fits, T looks straight down.
    Keys.onPressed: (e) => {
        if (e.key === Qt.Key_Left)       { rig.rotate(-5, 0) }
        else if (e.key === Qt.Key_Right) { rig.rotate(5, 0) }
        else if (e.key === Qt.Key_Up)    { rig.rotate(0, 3) }
        else if (e.key === Qt.Key_Down)  { rig.rotate(0, -3) }
        else if (e.key === Qt.Key_Plus || e.key === Qt.Key_Equal) { rig.zoom(0.88) }
        else if (e.key === Qt.Key_Minus) { rig.zoom(1 / 0.88) }
        else if (e.key === Qt.Key_F)     { root.fit() }
        else if (e.key === Qt.Key_T)     { rig.topView() }
        else { return }
        e.accepted = true
    }

    // Labels: the selected item and a few near the cursor (2D text over the view).
    Repeater {
        model: root.labelList
        delegate: Rectangle {
            x:          modelData.x + ScreenTools.defaultFontPixelWidth
            y:          modelData.y - height / 2
            width:      labelText.implicitWidth + ScreenTools.defaultFontPixelWidth
            height:     labelText.implicitHeight + 2
            radius:     2
            color:      modelData.selected ? "#f0b429" : Qt.rgba(qgcPal.window.r, qgcPal.window.g, qgcPal.window.b, 0.85)
            QGCLabel {
                id:                     labelText
                anchors.centerIn:       parent
                text:                   modelData.text
                color:                  modelData.selected ? "#111111" : qgcPal.text
                font.pointSize:         ScreenTools.smallFontPointSize
            }
        }
    }

    // Legend (block c): colors, terrain source, notes and warnings. Top left,
    // clear of the Plan view's terrain profile along the bottom.
    Plan3DLegend {
        id:                 legend3d
        anchors.left:       parent.left
        anchors.top:        parent.top
        anchors.margins:    ScreenTools.defaultFontPixelWidth
        // Below the Fit / Top buttons when they would cover the Less / More
        // button (narrow windows, large fonts).
        anchors.topMargin:  cameraRow.x < anchors.leftMargin + width + ScreenTools.defaultFontPixelWidth
                            ? cameraRow.height + ScreenTools.defaultFontPixelWidth * 2 : ScreenTools.defaultFontPixelWidth
        anchors.leftMargin: ScreenTools.defaultFontPixelWidth * 10
        width:              Math.min(parent.width * 0.42, ScreenTools.defaultFontPixelWidth * 52)
        // RotorLab 5.3.0 review (accessibility M1): no taller than the space
        // above the hint line, the edit panel and the edit message; the rest
        // of the legend scrolls.
        maxHeight:          parent.height - anchors.topMargin - root._bottomBase - ScreenTools.defaultFontPixelHeight
                            - (editPanel.visible ? editPanel.height : 0)
                            - (editMessageBox.visible ? editMessageBox.height + ScreenTools.defaultFontPixelHeight / 2 : 0)
        sceneModel:         model
        imagerySource:      groundLayer.imagerySource
        imageryLoading:     groundLayer.imageryLoading
    }

    // Camera buttons (camera: unassigned).
    Row {
        id:                 cameraRow
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.horizontalCenterOffset: root._centerOffset
        anchors.top:        parent.top
        anchors.margins:    ScreenTools.defaultFontPixelWidth
        spacing:            ScreenTools.defaultFontPixelWidth / 2
        QGCButton { text: qsTr("Fit"); onClicked: root.fit() }
        QGCButton { text: qsTr("Top"); onClicked: rig.topView() }
    }

    // G65: edits of the selected point that are not drags. Bindings re-read
    // the model on every scene revision (the editor's answers are not properties).
    readonly property int _sel: model.selectedVisualIndex
    property bool _splineConfirm: false
    // What the panel offers for the selected point. Plain conditions, not the
    // rows' own `visible`: a row inside a hidden panel always reads false.
    readonly property bool   _showSpline:   (model.revision, _sel > 0 && model.editor.splineBlockedReason(_sel) === "")
    readonly property bool   _showHeading:  (model.revision, _sel > 0 && model.editor.headingBlockedReason(_sel) === "")
    readonly property bool   _showGimbal:   (model.revision, _sel > 0 && model.editor.gimbalBlockedReason(_sel) === "")
    readonly property string _headingNote:  (model.revision, _sel > 0 ? model.editor.headingNote(_sel) : "")
    on_SelChanged: _splineConfirm = false

    Rectangle {
        id:                 editPanel
        // Bottom centre, above the hint line: clear of the legend (top left)
        // and of the Plan view's tool strip.
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.horizontalCenterOffset: root._centerOffset
        anchors.bottom:     parent.bottom
        anchors.bottomMargin: root._bottomBase
        width:              editColumn.width + ScreenTools.defaultFontPixelWidth * 2
        height:             editColumn.height + ScreenTools.defaultFontPixelHeight
        radius:             ScreenTools.defaultFontPixelWidth / 2
        color:              Qt.rgba(qgcPal.window.r, qgcPal.window.g, qgcPal.window.b, 0.9)
        visible:            root.editingAllowed && !model.dragging && (root._showSpline || root._showHeading || root._showGimbal || root._headingNote !== "")

        Column {
            id:                 editColumn
            anchors.centerIn:   parent
            spacing:            ScreenTools.defaultFontPixelHeight / 3

            // E-6: the leg into this point, straight or spline (ArduPilot multirotor only).
            Row {
                id:         splineRow
                spacing:    ScreenTools.defaultFontPixelWidth
                visible:    root._showSpline
                QGCLabel { anchors.verticalCenter: parent.verticalCenter; text: qsTr("Leg into this point:") }
                QGCButton {
                    text:       qsTr("Straight")
                    checkable:  true
                    checked:    (model.revision, !model.editor.isSpline(root._sel))
                    onClicked: {
                        checked = Qt.binding(function() { return (model.revision, !model.editor.isSpline(root._sel)) })
                        root._splineConfirm = false
                        model.editor.setSpline(root._sel, false)
                    }
                }
                QGCButton {
                    text:       qsTr("Spline")
                    checkable:  true
                    checked:    (model.revision, model.editor.isSpline(root._sel))
                    onClicked: {
                        checked = Qt.binding(function() { return (model.revision, model.editor.isSpline(root._sel)) })
                        if (!model.editor.isSpline(root._sel)) {
                            root._splineConfirm = true      // E-6: say so before the change
                        }
                    }
                }
            }
            // M3: a turn command 3D does not edit follows this point.
            QGCLabel {
                id:         headingNoteLabel
                property string note: root._headingNote
                visible:    note !== ""
                width:      ScreenTools.defaultFontPixelWidth * 40
                wrapMode:   Text.WordWrap
                text:       note
            }
            // E-7: the heading at this point; drag the blue handle to turn it.
            Row {
                id:         headingRow
                spacing:    ScreenTools.defaultFontPixelWidth
                visible:    root._showHeading
                property real heading: (model.revision, root._sel > 0 ? model.editor.heading(root._sel) : NaN)
                QGCLabel {
                    anchors.verticalCenter: parent.verticalCenter
                    text: isNaN(headingRow.heading) ? qsTr("Heading: none set") : qsTr("Heading: %1 deg").arg(headingRow.heading.toFixed(1))
                }
                QGCButton {
                    text:       qsTr("Set heading")
                    visible:    isNaN(headingRow.heading)
                    // Starts at north; the blue handle then turns it.
                    onClicked:  model.editor.setHeading(root._sel, 0)
                }
                QGCButton {
                    text:       qsTr("Remove heading")
                    visible:    !isNaN(headingRow.heading)
                    onClicked:  model.editor.clearHeading(root._sel)
                }
            }

            // E-8: the gimbal at this point; drag the grey handle for pitch,
            // Ctrl + drag for yaw (relative to the heading, known only when one is set).
            Row {
                id:         gimbalRow
                spacing:    ScreenTools.defaultFontPixelWidth
                visible:    root._showGimbal
                property real pitch:    (model.revision, root._sel > 0 ? model.editor.gimbalPitch(root._sel) : NaN)
                property real yaw:      (model.revision, root._sel > 0 ? model.editor.gimbalYaw(root._sel) : NaN)
                property bool yawKnown: (model.revision, root._sel > 0 && !isNaN(model.editor.heading(root._sel)))
                QGCLabel {
                    anchors.verticalCenter: parent.verticalCenter
                    width:                  Math.min(implicitWidth, ScreenTools.defaultFontPixelWidth * 40)
                    wrapMode:               Text.WordWrap
                    // RotorLab 5.3.0 review (visual-ux M2): says where the yaw is set.
                    text: isNaN(gimbalRow.pitch) ? qsTr("Gimbal: not set")
                                                 : !model.editor.gimbalYawEditing
                                                   ? qsTr("Gimbal pitch %1 deg down. Yaw is not shown in 3D; set it in the Camera section of the panel on the right.").arg((-gimbalRow.pitch).toFixed(1))
                                                   : qsTr("Gimbal pitch %1 deg down, yaw %2").arg((-gimbalRow.pitch).toFixed(1))
                                                       .arg(gimbalRow.yawKnown ? qsTr("%1 deg from the heading").arg(gimbalRow.yaw.toFixed(1)) : qsTr("not known (no heading set here)"))
                }
                QGCButton {
                    text:       qsTr("Set gimbal")
                    visible:    isNaN(gimbalRow.pitch)
                    onClicked:  model.editor.setGimbalPitch(root._sel, -45)
                }
                QGCButton {
                    text:       qsTr("Remove gimbal")
                    visible:    !isNaN(gimbalRow.pitch)
                    onClicked:  model.editor.clearGimbal(root._sel)
                }
            }

            Column {
                visible:    root._splineConfirm && root._showSpline
                spacing:    ScreenTools.defaultFontPixelHeight / 4
                QGCLabel {
                    width:      ScreenTools.defaultFontPixelWidth * 40
                    wrapMode:   Text.WordWrap
                    text:       model.editor.splineNote() + " " + qsTr("Spline legs are drawn approximately; the autopilot computes its own curve.")
                }
                Row {
                    spacing: ScreenTools.defaultFontPixelWidth
                    QGCButton {
                        text: qsTr("Change to spline")
                        onClicked: {
                            root._splineConfirm = false
                            model.editor.setSpline(root._sel, true)
                        }
                    }
                    QGCButton { text: qsTr("Cancel"); onClicked: root._splineConfirm = false }
                }
            }
        }
    }

    // Where the edit panel and the edit message start: six lines up, or above
    // the hint line when it wraps (narrow windows, large fonts).
    readonly property real _bottomBase: Math.max(ScreenTools.defaultFontPixelHeight * 6,
                                                 ScreenTools.defaultFontPixelHeight * 4.5 + (editHintBox.visible ? editHintBox.height : 0))

    // The hint sits on the same plate as the edit message, so it reads over
    // light imagery too, and stays clear of the right panel.
    Rectangle {
        id:                         editHintBox
        anchors.horizontalCenter:   parent.horizontalCenter
        anchors.horizontalCenterOffset: root._centerOffset
        anchors.bottom:             parent.bottom
        anchors.bottomMargin:       ScreenTools.defaultFontPixelHeight * 4
        visible:                    root.editingAllowed
        width:                      editHint.width + ScreenTools.defaultFontPixelWidth * 2
        height:                     editHint.implicitHeight + ScreenTools.defaultFontPixelHeight / 2
        radius:                     ScreenTools.defaultFontPixelWidth / 2
        color:                      Qt.rgba(qgcPal.window.r, qgcPal.window.g, qgcPal.window.b, 0.9)
        QGCLabel {
            id:                     editHint
            anchors.centerIn:       parent
            width:                  Math.min(implicitWidth, root._visibleWidth * 0.8 - ScreenTools.defaultFontPixelWidth * 2)
            wrapMode:               Text.WordWrap
            horizontalAlignment:    Text.AlignHCenter
            font.pointSize:         ScreenTools.smallFontPointSize
            // RotorLab 5.3.0 review (accessibility N1): the non-drag path is named.
            text:                   qsTr("Drag a point to move it. Ctrl + drag to change its height. Drag the blue handle to turn the heading, the grey one for gimbal pitch. Every edit can also be made in the mission list on the right: select the item there and edit its fields.")
        }
    }

    // G67 M1: a plan over the drawing limits says so on the view, not only in the legend.
    QGCLabel { anchors.horizontalCenter: parent.horizontalCenter; anchors.top: parent.top; anchors.topMargin: ScreenTools.defaultFontPixelHeight * 3.5; width: Math.min(parent.width * 0.5, implicitWidth); visible: model.limitWarning !== ""; text: model.limitWarning; wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter; color: model.stateColor("unknown"); style: Text.Outline; styleColor: qgcPal.window }

    // Test-only autorun (Daily builds, RL_G64_AUTORUN=1): timing and screenshots.
    Loader {
        active: typeof RLPlan3DProbe !== "undefined" && RLPlan3DProbe !== null
        source: "qrc:/rotorlab/plan3d/Plan3DAutorun.qml"
        onLoaded: item.view = root
    }
}
