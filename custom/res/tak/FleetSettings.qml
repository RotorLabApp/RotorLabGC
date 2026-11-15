/****************************************************************************
 *
 * RotorLab GC Fleet page: per-MAVLink-sysid metadata (callsign, video feed URL,
 * tail number) that enriches what RotorLab GC publishes to ATAK. Purely additive:
 * reads the live vehicle roster (read-only) and the native TAKManager fleet
 * registry; it does not touch QGC's own vehicle or video code.
 *
 * Laid out with the shared RLPageLayout (A156): full window width, fields up
 * to a readable maximum, the field under its label below about 1000 px.
 *
 ****************************************************************************/

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls
import QGroundControl.Palette
import QGroundControl.ScreenTools

Rectangle {
    id:             root
    color:          qgcPal.window
    anchors.fill:   parent

    property real _pad:        ScreenTools.defaultFontPixelWidth
    readonly property bool _narrow: page.narrow   // field under its label

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    // High-contrast white entry bar (matches the TAK page).
    component VField: TextField {
        Layout.preferredHeight: ScreenTools.defaultFontPixelHeight * 2
        color:                  page.inputTextColor
        font.pointSize:         ScreenTools.defaultFontPointSize
        verticalAlignment:      TextInput.AlignVCenter
        leftPadding:            ScreenTools.defaultFontPixelWidth
        rightPadding:           ScreenTools.defaultFontPixelWidth
        selectByMouse:          true
        selectionColor:         page.focusColor
        selectedTextColor:      "#0F1722"
        placeholderTextColor:   page.placeholderColor
        background: Rectangle {
            color:          page.inputColor
            radius:         4
            border.width:   parent.activeFocus ? 2 : 1
            border.color:   parent.activeFocus ? page.focusColor : page.inputBorderColor
        }
        cursorDelegate: Rectangle { width: 2; color: page.focusColor; visible: parent.cursorVisible }
    }

    // Labelled rows are placed by the shared rule in RLPageLayout: the label
    // column is 40% of the row up to about 40 character widths and wraps, the
    // field follows right after it up to 48 character widths, and the rest of
    // the row stays empty. On a narrow page the field goes under its label.
    // The field's size is a maximum, so it always shrinks with the window.
    // Entries use the theme's input colors with a blue focus outline.
    component LabeledField: GridLayout {
        id: _row
        property alias label: _l.text
        property alias text:  _tf.text
        Layout.fillWidth: true
        columns:          _narrow ? 1 : 3
        columnSpacing:    _pad * 2
        rowSpacing:       _pad / 2
        QGCLabel {
            id:                     _l
            Layout.fillWidth:       _narrow
            Layout.preferredWidth:  _narrow ? -1 : page.labelColumnWidth(_row.width)
            verticalAlignment:      Text.AlignVCenter
            wrapMode:               Text.WordWrap
        }
        VField {
            id:                     _tf
            Layout.fillWidth:       true
            Layout.minimumWidth:    ScreenTools.defaultFontPixelWidth * 8
            Layout.maximumWidth:    page.fieldMaxWidthFor(_row.width, _row.columnSpacing * 2)
            Layout.preferredWidth:  Layout.maximumWidth
        }
        Item { Layout.fillWidth: true; visible: !_narrow }
    }

    component Card: ColumnLayout {
        property alias title: _h.text
        default property alias content: _body.data
        Layout.fillWidth: true
        spacing:          ScreenTools.defaultFontPixelHeight / 3
        QGCLabel { id: _h; Layout.fillWidth: true; wrapMode: Text.WordWrap; font.bold: true; font.pointSize: ScreenTools.defaultFontPointSize + 1 }
        Rectangle {
            Layout.fillWidth: true
            color:            qgcPal.windowShade
            border.color:     qgcPal.groupBorder
            border.width:     1
            radius:           ScreenTools.defaultFontPixelHeight / 2
            implicitHeight:   _body.implicitHeight + _pad * 2
            ColumnLayout {
                id:      _body
                x:       _pad
                y:       _pad
                width:   parent.width - _pad * 2
                spacing: ScreenTools.defaultFontPixelHeight / 2
            }
        }
    }

    // The shared page layout (A156): full width minus the standard settings
    // padding, scrolls vertically, and says when the page is narrow.
    RLPageLayout {
        id:            page
        anchors.fill:  parent

        ColumnLayout {
            id:                 outer
            Layout.fillWidth:   true
            spacing:            ScreenTools.defaultFontPixelHeight

            Card {
                title: qsTr("Fleet: per-vehicle ATAK metadata")
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Assign a callsign, a video feed URL, and an optional tail number to each " +
                               "MAVLink system ID. RotorLab GC publishes these to ATAK: the callsign labels " +
                               "the aircraft, and the video URL is handed to ATAK as a stream it plays on " +
                               "the marker (RotorLab GC doesn't render extra feeds; ATAK does). Connected " +
                               "vehicles appear automatically; you can also pre-stage a sysid below.")
                }
                RowLayout {
                    Layout.fillWidth: true
                    QGCButton { text: qsTr("Rescan vehicles"); onClicked: TAKManager.refreshFleet() }
                    QGCLabel {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: TAKManager.fleetModel.length + qsTr(" vehicle(s) known")
                        color: qgcPal.text
                    }
                }
            }

            Repeater {
                model: TAKManager.fleetModel
                delegate: Card {
                    required property var modelData
                    property int    _sysid: modelData.sysid
                    property string _cs:    modelData.callsign
                    property string _vu:    modelData.videoUrl
                    property string _tl:    modelData.tail
                    title: (modelData.connected ? "●  " : "○  ") + qsTr("sysid ") + modelData.sysid +
                           (modelData.connected ? qsTr("  (connected)") : qsTr("  (offline)"))
                    LabeledField { label: qsTr("Callsign");     text: _cs; onTextChanged: _cs = text }
                    LabeledField { label: qsTr("Video feed URL"); text: _vu; onTextChanged: _vu = text }
                    LabeledField { label: qsTr("Tail number");   text: _tl; onTextChanged: _tl = text }

                    RowLayout {
                        Layout.fillWidth: true
                        QGCButton {
                            text: qsTr("Apply")
                            onClicked: TAKManager.setFleetEntry(_sysid, _cs, _vu, _tl)
                        }
                        QGCButton {
                            text: qsTr("Clear")
                            onClicked: TAKManager.removeFleetEntry(_sysid)
                        }
                        QGCLabel { Layout.fillWidth: true }
                    }
                }
            }

            Card {
                id: addCard
                title: qsTr("Add / pre-stage a sysid")
                property string _asysid: ""
                property string _acs: ""
                property string _avu: ""
                property string _atl: ""
                LabeledField { label: qsTr("System ID"); text: addCard._asysid; onTextChanged: addCard._asysid = text }
                LabeledField { label: qsTr("Callsign"); text: addCard._acs; onTextChanged: addCard._acs = text }
                LabeledField { label: qsTr("Video feed URL"); text: addCard._avu; onTextChanged: addCard._avu = text }
                LabeledField { label: qsTr("Tail number"); text: addCard._atl; onTextChanged: addCard._atl = text }
                QGCButton {
                    text: qsTr("Add to fleet")
                    onClicked: {
                        var sid = parseInt(addCard._asysid)
                        if (sid > 0) {
                            TAKManager.setFleetEntry(sid, addCard._acs, addCard._avu, addCard._atl)
                            addCard._asysid = ""; addCard._acs = ""; addCard._avu = ""; addCard._atl = ""
                        }
                    }
                }
            }
        }
    }
}
