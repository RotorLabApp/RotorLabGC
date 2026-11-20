/****************************************************************************
 *
 * RotorLab GC Plan view 3D (G64, MG2) - test-only autorun, Daily builds.
 *
 * Loaded only when the Daily build was started with RL_G64_AUTORUN=1 (the
 * RLPlan3DProbe context property exists only then). Drives the G63 spike's
 * frame-time method on Plan3DView and takes screenshots of corpus plans.
 *
 *   RL_G64_PLAN     plan to time (camera scenarios)
 *   RL_G64_STEPS    comma list of scenarios: idle, rotate, pan (default all three);
 *                   G65: drag (the point moves on a 60 m circle through the
 *                   editor, every frame) and dragheight (its height swings
 *                   20 m up and down, every frame)
 *   RL_G65_DRAG_VI  visual index to drag (default: the editable item nearest the middle)
 *   RL_G65_SHOTS    1: after RL_G64_PLAN loads, screenshots of the 3D edit
 *                   panel and handles (heading, gimbal pitch, spline note) on
 *                   item RL_G65_DRAG_VI (default 2)
 *   RL_G65_ROUNDTRIP plan for the SITL round trip (contract 4a): wait for the
 *                   connected vehicle and its own first download, load the
 *                   plan, make the 3D edits through the editor (move, height,
 *                   spline, heading, gimbal pitch, where offered), save it,
 *                   upload, download again and save what came back
 *                   (g65-edited.plan, g65-readback.plan in RL_G64_OUT)
 *   RL_G64_SECONDS  seconds per scenario (default 35)
 *   RL_G64_SHOTS    semicolon list of plans to screenshot in 3D, dark and light
 *   RL_G64_SHOT2D   1: also screenshot each plan in the 2D Plan view
 *   RL_R530_LEGEND  1: with RL_G64_SHOTS, also screenshot the legend scrolled
 *                   to its end, and collapsed ("Less"), in each theme
 *   RL_G64_TOGGLE2D plan for the 2D check: 2D, 3D, then 2D again (shots before and after)
 *   RL_G64_LABELS   1: screenshot the selected and near-cursor labels on RL_G64_PLAN
 *   RL_G64_QUIT     1: quit when done
 *   RL_G64_OUT      output folder
 *
 ****************************************************************************/

import QtQuick
import QtQuick.Window

import QGroundControl
import QGroundControl.Palette

Item {
    id: runner

    property var    view:       null
    property var    probe:      RLPlan3DProbe
    property var    queue:      []
    property string scenario:   ""
    property double t0:         0
    property var    last:       ({})
    property int    groundWaits: 0
    property int    stableWaits: 0
    property int    lastRevision: -1

    QGCPalette { id: pal }

    function pmc() { return view.planMasterController }
    function log(s) { probe.log(s) }
    function baseName(p) { var n = p.replace(/\\/g, "/"); n = n.substring(n.lastIndexOf("/") + 1); return n.replace(/\.plan$/, "") }
    function themeName() { return pal.globalTheme === QGCPalette.Dark ? "dark" : "light" }

    function push(fn, arg) { queue.push({ fn: fn, arg: arg }) }

    function next() {
        if (queue.length === 0) {
            log("ALL DONE")
            if (probe.env("RL_G64_QUIT") === "1") {
                if (!edited) {
                    pmc().dirty = false     // nothing was edited: as G64
                }
                // After an edit the plan stays dirty; GC may ask to save, and the
                // run script closes GC by its pid (F7).
                Qt.quit()
            }
            return
        }
        var a = queue.shift()
        var wait = a.fn(a.arg)
        if (wait >= 0) {
            stepTimer.interval = wait
            stepTimer.start()
        }
    }

    Timer { id: stepTimer; onTriggered: runner.next() }

    // ---- actions: each returns ms to wait before the next one, or -1 if it calls next() itself

    function aLoad(path) {
        log("load " + path + " window=" + view.Window.window.width + "x" + view.Window.window.height)
        pmc().loadFromFile(path)
        mainWindow.showPlanView()
        pmc().fitViewportToItems()
        view.planView.setPlan3DOpen(true)
        view._fitted = false
        groundWaits = 0
        return 1500
    }

    function aWaitGround() {
        var m = view.sceneModel
        // Wait for the ground, and for the scene to settle: GC may still set
        // the planned home height from terrain, which moves every relative
        // height (G64 stats). Settled = no rebuild for 3 s.
        if (m.revision !== lastRevision) {
            lastRevision = m.revision
            stableWaits = 0
        } else {
            stableWaits++
        }
        // Block (c) warns "Waiting for GC to set the launch altitude from
        // terrain" until the planned home has its height; shots before that
        // show every relative height as unknown.
        var waitingHome = false
        for (var i = 0; i < m.warnings.length; i++) {
            if (m.warnings[i].indexOf("Waiting for GC") === 0) {
                waitingHome = true
            }
        }
        if ((m.groundLoading || waitingHome || stableWaits < 6) && groundWaits < 120) {
            groundWaits++
            queue.unshift({ fn: aWaitGround, arg: null })
            return 500
        }
        view.fit()
        log("scene markers=" + m.markerCount + " segments=" + m.segmentCount + " build_ms=" + m.buildMs.toFixed(1)
            + " ground_known=" + m.groundKnownPercent + "% loading=" + m.groundLoading
            + " home_alt=" + pmc().missionController.plannedHomePosition.altitude
            + " strike=" + m.samplesStrike + " contact=" + m.samplesGroundContact + " unknown=" + m.samplesUnknown + " nostrike=" + m.samplesNoStrike
            + " warnings=" + JSON.stringify(m.warnings))
        return 2000
    }

    // 2D check: the plan loaded with 3D never opened.
    function aLoad2D(path) {
        log("load (2D) " + path)
        pmc().loadFromFile(path)
        mainWindow.showPlanView()
        view.planView.setPlan3DOpen(false)
        pmc().fitViewportToItems()
        return 20000    // map tiles and the terrain profile settle
    }

    function aTheme(dark) {
        QGroundControl.settingsManager.appSettings.indoorPalette.rawValue = dark ? 1 : 0
        return 1500
    }

    // ---- 5.3.0 review shots: the legend's scroll area and "Less"
    function legendItem() {
        var c = view.children
        for (var i = 0; i < c.length; i++) {
            if (c[i].maxHeight !== undefined && c[i].expanded !== undefined && c[i].sceneModel !== undefined) {
                return c[i]
            }
        }
        return null
    }
    function legendFlick(lg) {
        if (lg.scrollArea !== undefined) {
            return lg.scrollArea
        }
        var c = lg.children
        for (var i = 0; i < c.length; i++) {
            if (c[i].contentY !== undefined && c[i].contentHeight !== undefined) {
                return c[i]
            }
        }
        return null
    }
    function aLegend(step) {
        var lg = legendItem()
        if (!lg) {
            log("legend not found")
            return 300
        }
        var f = legendFlick(lg)
        if (step === "end" && f) {
            f.contentY = Math.max(0, f.contentHeight - f.height)
        } else if (f) {
            f.contentY = 0
        }
        if (step === "collapse") {
            lg.expanded = false
        } else if (step === "expand") {
            lg.expanded = true
        }
        log("legend " + step + " expanded=" + lg.expanded + " height=" + lg.height.toFixed(0) + " maxHeight=" + lg.maxHeight.toFixed(0)
            + (f ? " scroll=" + f.height.toFixed(0) + "/" + f.contentHeight.toFixed(0) + " contentY=" + f.contentY.toFixed(0) : ""))
        return 1000
    }

    function aShot(name) {
        probe.saveScreenshot(name.replace("%THEME%", themeName()))
        return 300
    }

    function a2D(open3d) {
        view.planView.setPlan3DOpen(open3d)
        return 2500
    }

    // Labels check: select a mid-plan item and put the cursor at the view center.
    function aLabels() {
        var mc = pmc().missionController
        var vi = Math.floor(mc.visualItems.count / 2)
        mc.setCurrentPlanViewSeqNum(mc.visualItems.get(vi).sequenceNumber, false)
        view._mouseX = view.width / 2
        view._mouseY = view.height / 2
        view.updateLabels()
        log("labels shown=" + view.labelList.length + " selectedVisualIndex=" + vi)
        return 1500
    }

    function aClearLabels() {
        pmc().missionController.setCurrentPlanViewSeqNum(0, false)
        view._mouseX = -1
        view.updateLabels()
        return 500
    }

    // G65: the item the drag scenarios edit.
    property int    dragVi:     -1
    property var    dragStart:  null
    property var    editMs:     []      ///< time inside each editor write (GC's own signal cascade)
    property var    emitMs:     []      ///< time inside the scene's sceneChanged (QML handlers)
    function median(a) { if (a.length === 0) return NaN; var b = a.slice().sort(function(x, y) { return x - y }); return b[Math.floor(b.length / 2)] }
    function p95(a) { if (a.length === 0) return NaN; var b = a.slice().sort(function(x, y) { return x - y }); return b[Math.floor(b.length * 0.95)] }
    property bool   edited:     false
    property double dragAmsl:   NaN

    function pickDragItem() {
        var mc = pmc().missionController
        var want = parseInt(probe.env("RL_G65_DRAG_VI"))
        if (!isNaN(want)) {
            return want
        }
        var mid = Math.floor(mc.visualItems.count / 2)
        for (var k = 0; k < mc.visualItems.count; k++) {
            var cand = [mid + k, mid - k]
            for (var j = 0; j < 2; j++) {
                if (cand[j] > 0 && cand[j] < mc.visualItems.count && view.editor.canMove(cand[j]) && view.editor.canChangeHeight(cand[j])) {
                    return cand[j]
                }
            }
        }
        return -1
    }

    // ---- G65: wait (never act) until the device is activated through the
    // license gate by the run script, so shots and timings show the plan view.
    property int licenseWaits: 0
    function aWaitLicense() {
        if (!LicenseManager.activated && licenseWaits++ < 240) {
            queue.unshift({ fn: aWaitLicense, arg: null })
            return 500
        }
        log("license gate " + (LicenseManager.activated ? "passed" : "still shown after 120 s"))
        return 3000
    }

    // ---- G65 edit panel shots
    function aEditPanel(step) {
        var mc = pmc().missionController
        var vi = parseInt(probe.env("RL_G65_DRAG_VI"))
        if (isNaN(vi)) {
            vi = 2
        }
        var ed = view.editor
        if (step === "select") {
            mc.setCurrentPlanViewSeqNum(mc.visualItems.get(vi).sequenceNumber, false)
            log("panel select vi=" + vi + " spline=" + ed.splineBlockedReason(vi) + "| heading=" + ed.headingBlockedReason(vi) + "| gimbal=" + ed.gimbalBlockedReason(vi) + "|")
        } else if (step === "handles") {
            edit("heading vi=" + vi, ed.setHeading(vi, 60))
            edit("gimbal pitch vi=" + vi, ed.setGimbalPitch(vi, -35))
            edited = true
        } else if (step === "splinenote") {
            view._splineConfirm = true
        } else if (step === "refuse") {
            view._splineConfirm = false
            edit("move takeoff (refused)", ed.moveTo(1, 0, 0))
        }
        view.updateLabels()
        return 1500
    }

    // ---- G65 SITL round trip
    property int vehicleWaits: 0
    function aWaitVehicle() {
        var v = QGroundControl.multiVehicleManager.activeVehicle
        var ready = v && v.initialConnectComplete && !pmc().syncInProgress
        if (!ready && vehicleWaits++ < 240) {
            queue.unshift({ fn: aWaitVehicle, arg: null })
            return 500
        }
        log("roundtrip vehicle " + (v ? ("ready=" + ready + " firmware apm=" + v.apmFirmware + " px4=" + v.px4Firmware + " multirotor=" + v.multiRotor + " fixedWing=" + v.fixedWing) : "none"))
        return 3000
    }

    function edit(name, ok) {
        log("edit " + name + " -> " + ok + (view.editor.message !== "" ? (" message=\"" + view.editor.message + "\"") : ""))
    }

    function aRoundtripEdits() {
        var ed = view.editor
        var mc = pmc().missionController
        var n = mc.visualItems.count
        var moved = false, raised = false, splined = false, headed = false, gimbaled = false
        for (var vi = 1; vi < n; vi++) {
            if (!moved && ed.canMove(vi) && ed.canChangeHeight(vi)) {
                var c = mc.visualItems.get(vi).coordinate
                ed.beginDrag(vi)
                edit("move vi=" + vi, ed.moveTo(vi, c.latitude + 0.0004, c.longitude + 0.0005))
                ed.endDrag()
                moved = true
                continue
            }
            if (!raised && ed.canChangeHeight(vi)) {
                var amsl = view.sceneModel.markerAmsl(vi)
                edit("height vi=" + vi + " from amsl=" + amsl, ed.setHeightAmsl(vi, (isNaN(amsl) ? 50 : amsl) + 7.3))
                raised = true
                continue
            }
            if (!splined && ed.splineBlockedReason(vi) === "") {
                edit("spline vi=" + vi + " to " + !ed.isSpline(vi), ed.setSpline(vi, !ed.isSpline(vi)))
                splined = true
                continue
            }
        }
        for (vi = 1; vi < mc.visualItems.count; vi++) {
            if (!gimbaled && ed.gimbalBlockedReason(vi) === "") {
                edit("gimbal pitch vi=" + vi, ed.setGimbalPitch(vi, -30))
                edit("gimbal yaw vi=" + vi + " (hidden until SITL check)", ed.setGimbalYaw(vi, 20))
                gimbaled = true
            }
            if (!headed && ed.headingBlockedReason(vi) === "") {
                edit("heading vi=" + vi, ed.setHeading(vi, 123.4))
                headed = true
            }
        }
        edited = true       // never cleared after an edit (F7)
        log("roundtrip offered: move=" + moved + " height=" + raised + " spline=" + splined + " heading=" + headed + " gimbal=" + gimbaled)
        return 1500
    }

    function aSavePlan(name) {
        pmc().saveToFile(probe.outDir() + "/" + name)
        log("saved " + name)
        return 500
    }

    function aUpload() {
        log("upload start, items=" + pmc().missionController.visualItems.count)
        pmc().sendToVehicle()
        vehicleWaits = 0
        return 1000
    }

    function aWaitSync(label) {
        if (pmc().syncInProgress && vehicleWaits++ < 240) {
            queue.unshift({ fn: aWaitSync, arg: label })
            return 500
        }
        log(label + " done, syncInProgress=" + pmc().syncInProgress + " dirty=" + pmc().dirty)
        return 1500
    }

    function aDownload() {
        log("download start")
        pmc().loadFromVehicle()
        vehicleWaits = 0
        return 1000
    }

    function aMeasure(label) {
        scenario = label
        last = {}
        if (label === "drag" || label === "dragheight") {
            dragVi = pickDragItem()
            var item = pmc().missionController.visualItems.get(dragVi)
            // Numbers, not the coordinate: a value read from a QObject property
            // can follow the property, and the circle would walk away.
            dragStart = { latitude: item.coordinate.latitude, longitude: item.coordinate.longitude }
            editMs = []
            emitMs = []
            dragAmsl = view.sceneModel.markerAmsl(dragVi)
            log("drag item vi=" + dragVi + " command=" + item.command + " altitudeMode=" + item.altitudeMode
                + " start=" + dragStart.latitude + "," + dragStart.longitude + " amsl=" + dragAmsl
                + " begin=" + view.editor.beginDrag(dragVi) + " message=" + view.editor.message)
        }
        t0 = probe.nowMs()
        var s = parseInt(probe.env("RL_G64_SECONDS"))
        probe.start(label, isNaN(s) ? 35 : s)
        return -1
    }

    Connections {
        target: runner.probe
        function onFrame() {
            var t = (runner.probe.nowMs() - runner.t0) / 1000.0
            if (runner.scenario === "rotate") {
                // As the spike: +-15 degrees yaw, +-8 degrees pitch.
                var a = Math.sin(2 * Math.PI * t / 6), b = Math.sin(2 * Math.PI * t / 9)
                runner.view.rotate(15 * (a - (runner.last.a || 0)), 8 * (b - (runner.last.b || 0)))
                runner.last = { a: a, b: b }
            } else if (runner.scenario === "drag") {
                // G65: a 60 m circle in 6 s, written through the editor every frame.
                var r = 60, w = 2 * Math.PI * t / 6
                var lat0 = runner.dragStart.latitude, lon0 = runner.dragStart.longitude
                var e0 = runner.probe.nowMs()
                // The scene's drag entry point, as a mouse drag: drawn every
                // frame, written to GC's item at most every 100 ms.
                runner.view.sceneModel.dragToLatLon(
                                          lat0 + r * Math.sin(w) / 111320,
                                          lon0 + r * (Math.cos(w) - 1) / (111320 * Math.cos(lat0 * Math.PI / 180)))
                runner.editMs.push(runner.probe.nowMs() - e0)
                runner.emitMs.push(runner.view.sceneModel.emitMs)
            } else if (runner.scenario === "dragheight") {
                var h0 = runner.probe.nowMs()
                runner.view.sceneModel.dragToAmsl(runner.dragAmsl + 20 * Math.sin(2 * Math.PI * t / 4))
                runner.editMs.push(runner.probe.nowMs() - h0)
                runner.emitMs.push(runner.view.sceneModel.emitMs)
            } else if (runner.scenario === "pan") {
                // As the spike: 400 by 250 px back and forth.
                var c = 400 * Math.sin(2 * Math.PI * t / 6), d = 250 * Math.sin(2 * Math.PI * t / 8)
                runner.view.pan(c - (runner.last.c || 0), d - (runner.last.d || 0))
                runner.last = { c: c, d: d }
            }
        }
        function onFinished(label) {
            runner.probe.saveScreenshot("g64-" + label + ".png")
            if (label === "drag" || label === "dragheight") {
                runner.log("drag done vi=" + runner.dragVi + " last build_ms=" + runner.view.sceneModel.buildMs.toFixed(2)
                           + " revision=" + runner.view.sceneModel.revision
                           + " edit_ms median=" + runner.median(runner.editMs).toFixed(2) + " p95=" + runner.p95(runner.editMs).toFixed(2)
                           + " scene_emit_ms median=" + runner.median(runner.emitMs).toFixed(2) + " p95=" + runner.p95(runner.emitMs).toFixed(2))
                runner.view.sceneModel.endDrag()       // writes the last position
                // F7: put the point back where it started; the plan stays marked
                // changed (never cleared after an edit).
                runner.view.editor.moveTo(runner.dragVi, runner.dragStart.latitude, runner.dragStart.longitude)
                if (!isNaN(runner.dragAmsl)) {
                    runner.view.editor.setHeightAmsl(runner.dragVi, runner.dragAmsl)
                }
                runner.edited = true
            }
            runner.scenario = ""
            stepTimer.interval = 2000
            stepTimer.start()
        }
    }

    property bool _started: false
    onViewChanged: {
        if (!view || _started) {
            return
        }
        _started = true
        probe.window = view.Window.window
        push(aWaitLicense, null)
        var rt = probe.env("RL_G65_ROUNDTRIP")
        if (rt !== "") {
            push(aWaitVehicle, null)
            push(aLoad, rt)
            push(aWaitGround, null)
            push(aRoundtripEdits, null)
            push(aShot, "g65-roundtrip-edited-%THEME%.png")
            push(aSavePlan, "g65-edited.plan")
            push(aUpload, null)
            push(aWaitSync, "upload")
            push(aDownload, null)
            push(aWaitSync, "download")
            push(aSavePlan, "g65-readback.plan")
            push(aShot, "g65-roundtrip-readback-%THEME%.png")
        }
        var plan = probe.env("RL_G64_PLAN")
        if (plan !== "") {
            push(aLoad, plan)
            push(aWaitGround, null)
            push(aShot, "g64-timing-start-%THEME%.png")
            if (probe.env("RL_G65_SHOTS") === "1") {
                push(aEditPanel, "select")
                push(aShot, "g65-panel-selected-%THEME%.png")
                push(aEditPanel, "handles")
                push(aShot, "g65-panel-handles-%THEME%.png")
                push(aEditPanel, "splinenote")
                push(aShot, "g65-panel-spline-note-%THEME%.png")
                push(aEditPanel, "refuse")
                push(aShot, "g65-panel-refused-%THEME%.png")
            }
            if (probe.env("RL_G64_LABELS") === "1") {
                push(aLabels, null)
                push(aShot, "g64-labels-%THEME%.png")
                push(aClearLabels, null)
            }
            var steps = (probe.env("RL_G64_STEPS") || "idle,rotate,pan").split(",")
            for (var i = 0; i < steps.length; i++) {
                push(aMeasure, steps[i])
            }
        }
        // RL_G64_TOGGLE2D=<plan>: 2D shot with 3D never opened, then 3D, then
        // 2D again after closing 3D; the two 2D shots must match.
        var t2d = probe.env("RL_G64_TOGGLE2D")
        if (t2d !== "") {
            push(aLoad2D, t2d)
            push(aShot, "g64-2d-before-3d-%THEME%.png")
            push(a2D, true)
            push(aWaitGround, null)
            push(aShot, "g64-2d-toggle-3d-%THEME%.png")
            push(a2D, false)
            push(aShot, "g64-2d-after-3d-%THEME%.png")
        }
        var shots = probe.env("RL_G64_SHOTS")
        if (shots !== "") {
            var plans = shots.split(";")
            for (var j = 0; j < plans.length; j++) {
                var n = baseName(plans[j])
                push(aLoad, plans[j])
                push(aWaitGround, null)
                var themes = [true, false]
                for (var k = 0; k < themes.length; k++) {
                    push(aTheme, themes[k])
                    push(aShot, "g64-" + n + "-%THEME%.png")
                    if (probe.env("RL_R530_LEGEND") === "1") {
                        push(aLegend, "end")
                        push(aShot, "r530-" + n + "-expanded-end-%THEME%.png")
                        push(aLegend, "collapse")
                        push(aShot, "r530-" + n + "-collapsed-%THEME%.png")
                        push(aLegend, "expand")
                    }
                }
                if (probe.env("RL_G64_SHOT2D") === "1") {
                    push(a2D, false)
                    push(aShot, "g64-" + n + "-2d-%THEME%.png")
                }
                push(aTheme, true)
            }
        }
        stepTimer.interval = 8000
        stepTimer.start()
    }
}
