/****************************************************************************
 *
 * RotorLab GC Fleet Connector - running a checklist.
 *
 * Each item has three states - ok, not ok, skipped - and starts UNANSWERED. A
 * checklist that arrives pre-ticked records nothing.
 *
 * The reference is minted once, when this opens, and is reused for the first
 * send and every retry. Filing a failed run is normal: a crew that finds a
 * cracked arm should record it, and that record is worth more than a clean
 * one. Nothing here blocks a launch.
 *
 ****************************************************************************/

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls
import QGroundControl.Palette
import QGroundControl.ScreenTools

Popup {
    id:      root
    modal:   false
    focus:   true
    closePolicy: Popup.NoAutoClose
    anchors.centerIn: Overlay.overlay
    width:   Math.min(ScreenTools.defaultFontPixelWidth * 96,
                      Overlay.overlay ? Overlay.overlay.width * 0.9 : 900)
    padding: 0

    /// "preflight" | "postflight"
    property var    backend:  FleetConnector
    property string kind:     "preflight"
    property var    template_: null
    /// Which published template of `kind` to open with. The crew can pick
    /// another in the runner while nothing is answered yet.
    property int    startIndex: 0
    property int    _templateIndex: 0
    property int    sysid:    1
    property string aircraft: ""

    property string _ref:     ""
    property string _flightRef: ""
    property double _started: 0
    property var    _answers: ({})      // index -> "ok" | "notok" | "skip"
    property int    _answered: 0

    function _templates() {
        return kind === "preflight" ? backend.preflightTemplates
                                    : backend.postflightTemplates
    }

    onOpened: {
        var list = _templates()
        _templateIndex = list.length > 0 ? Math.min(Math.max(startIndex, 0), list.length - 1) : 0
        template_ = list.length > 0 ? list[_templateIndex] : null
        // Minted ONCE, here. Every send and retry uses this exact value; a new
        // one would file a second copy of a run that happened once.
        _ref     = backend.newChecklistRef(kind, sysid)
        _flightRef = typeof backend.flightRefForChecklist === "function"
                   ? backend.flightRefForChecklist(kind, sysid) : ""
        _started = Date.now() / 1000
        _answers = ({})
        _answered = 0
    }

    function _set(i, v) {
        var a = _answers
        if (a[i] === undefined) { _answered++ }
        a[i] = v
        _answers = a
    }

    function _passed() {
        // True when nothing was marked not-ok. A skipped item is a gap, not a
        // failure.
        for (var k in _answers) { if (_answers[k] === "notok") return false }
        return true
    }

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    // Notes, captions and status lines stay readable in both themes, at
    // least 4.5:1 on the card. The stock gray, orange and green are too pale
    // on the light card, and the stock orange on the dark one.
    readonly property bool  _lightTheme: qgcPal.globalTheme === QGCPalette.Light
    readonly property color _noteColor:  _lightTheme ? "#39414b" : "#d4d4d4"   // same as RLPageLayout.noteColor
    readonly property color _warnColor:  _lightTheme ? "#8a4a00" : "#e89020"

    background: Rectangle {
        color:        qgcPal.window
        border.color: qgcPal.groupBorder
        border.width: 1
        radius:       ScreenTools.defaultFontPixelHeight / 2
    }

    contentItem: ColumnLayout {
        spacing: ScreenTools.defaultFontPixelHeight / 2

        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins:   ScreenTools.defaultFontPixelWidth * 2
            spacing:          ScreenTools.defaultFontPixelHeight / 4
            QGCLabel {
                text: template_ ? template_.name : qsTr("Checklist")
                textFormat: Text.PlainText
                font.bold: true
                font.pointSize: ScreenTools.defaultFontPointSize + 3
            }
            // Every published template of this kind, not only the first
            // (A107): an organization may publish one per aircraft type.
            RowLayout {
                Layout.fillWidth: true
                visible: _templates().length > 1
                QGCLabel {
                    text:  qsTr("Checklist")
                    color: _noteColor
                }
                QGCComboBox {
                    id: templatePicker
                    Layout.fillWidth: true
                    // A switch would drop answers already given, so the pick is
                    // made before the first answer.
                    enabled: _answered === 0
                    model: {
                        var names = []
                        var list = _templates()
                        for (var i = 0; i < list.length; i++) {
                            names.push(list[i].name)
                        }
                        return names
                    }
                    currentIndex: _templateIndex
                    onActivated: function(index) {
                        var list = _templates()
                        if (index < 0 || index >= list.length) {
                            return
                        }
                        _templateIndex = index
                        template_ = list[index]
                        _answers = ({})
                        _answered = 0
                    }
                }
            }
            QGCLabel {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: _noteColor
                text: qsTr("Records what was checked. You can close this at any point and fly. " +
                           "It never holds up a launch.")
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.leftMargin:  ScreenTools.defaultFontPixelWidth * 2
            Layout.rightMargin: ScreenTools.defaultFontPixelWidth * 2
            Layout.preferredHeight: ScreenTools.defaultFontPixelHeight * 16
            color:        qgcPal.windowShade
            border.color: qgcPal.groupBorder
            border.width: 1
            radius:       4

            ListView {
                id: itemList
                anchors.fill:    parent
                anchors.margins: 4
                clip:            true
                spacing:         2
                model:           template_ ? template_.items : []

                delegate: RowLayout {
                    required property string modelData
                    required property int    index
                    width:   itemList.width
                    spacing: ScreenTools.defaultFontPixelWidth

                    QGCLabel {
                        Layout.fillWidth: true
                        Layout.leftMargin: ScreenTools.defaultFontPixelWidth / 2
                        wrapMode: Text.WordWrap
                        textFormat: Text.PlainText
                        text:  modelData
                        color: _answers[index] === undefined ? qgcPal.text : _noteColor
                    }
                    // Three states, all explicit. Nothing is answered for the
                    // crew: an unanswered item stays unanswered.
                    QGCButton {
                        text:    qsTr("OK")
                        primary: _answers[index] === "ok"
                        onClicked: _set(index, "ok")
                    }
                    QGCButton {
                        text:    qsTr("Not OK")
                        primary: _answers[index] === "notok"
                        onClicked: _set(index, "notok")
                    }
                    QGCButton {
                        text:    qsTr("Skip")
                        primary: _answers[index] === "skip"
                        onClicked: _set(index, "skip")
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin:  ScreenTools.defaultFontPixelWidth * 2
            Layout.rightMargin: ScreenTools.defaultFontPixelWidth * 2
            QGCLabel {
                Layout.fillWidth: true
                color: _noteColor
                text: template_
                      ? qsTr("%1 of %2 answered").arg(_answered).arg(template_.items.length)
                      : ""
            }
            QGCLabel {
                visible: !_passed()
                color:   _warnColor
                text:    qsTr("will be filed as failed")
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.margins:   ScreenTools.defaultFontPixelWidth * 2
            QGCButton {
                text: qsTr("Close")
                onClicked: { backend.dismissChecklistOffer(); root.close() }
            }
            Item { Layout.fillWidth: true }
            QGCButton {
                text:    qsTr("File this checklist")
                primary: true
                enabled: template_ !== null && _answered > 0
                onClicked: {
                    var items = []
                    for (var i = 0; i < template_.items.length; i++) {
                        if (_answers[i] === undefined) { continue }
                        // `ok` preserves compatibility with the direct RotorLab
                        // connector; the Fleet Server records the explicit result.
                        items.push({
                            "text":   template_.items[i],
                            "ok":     _answers[i] === "ok",
                            "result": _answers[i] === "ok"    ? "pass"
                                    : _answers[i] === "notok" ? "fail"
                                                              : "skip"
                        })
                    }
                    if (_flightRef.length > 0) {
                        backend.fileChecklist(_ref, _flightRef, template_.id, kind,
                                              template_.name, aircraft,
                                              _started, _passed(), items, "")
                    } else {
                        backend.fileChecklist(_ref, template_.id, kind, aircraft,
                                              _started, _passed(), items, "")
                    }
                    root.close()
                }
            }
        }
    }
}
