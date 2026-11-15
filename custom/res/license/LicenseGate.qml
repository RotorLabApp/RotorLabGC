/****************************************************************************
 *
 * RotorLab GC startup license gate. A full-window modal overlay shown until
 * the app is activated. Blocks all interaction with the app behind it.
 * Binds to the native LicenseManager (`LicenseManager`).
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
    id:         gate
    anchors.fill: parent
    visible:    !LicenseManager.activated
    color:      "#0d1117"
    z:          10000

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    // Swallow every mouse / key event so the app underneath can't be used.
    MouseArea { anchors.fill: parent; hoverEnabled: true; preventStealing: true; onWheel: (w)=> w.accepted = true }
    Keys.onPressed: (e)=> e.accepted = true

    property string _pending: ""

    ColumnLayout {
        anchors.centerIn: parent
        width:  Math.min(parent.width * 0.8, ScreenTools.defaultFontPixelWidth * 46)
        spacing: ScreenTools.defaultFontPixelHeight

        Image {
            source: "qrc:/custom/img/rotorlab-original.svg"
            sourceSize.height: ScreenTools.defaultFontPixelHeight * 6
            fillMode: Image.PreserveAspectFit
            Layout.alignment: Qt.AlignHCenter
        }

        QGCLabel {
            text: qsTr("RotorLab GC")
            font.pointSize: ScreenTools.largeFontPointSize
            font.bold: true
            color: "#e6edf3"
            Layout.alignment: Qt.AlignHCenter
        }

        QGCLabel {
            text: qsTr("Activate this device to continue")
            color: "#8b97a6"
            Layout.alignment: Qt.AlignHCenter
        }

        TextField {
            id:                     keyField
            Layout.fillWidth:       true
            Layout.preferredHeight: ScreenTools.defaultFontPixelHeight * 2.4
            horizontalAlignment:    TextInput.AlignHCenter
            color:                  "#0F1722"
            text:                   _pending
            placeholderText:        qsTr("Enter license key")
            placeholderTextColor:   "#8A93A0"
            enabled:                !LicenseManager.busy
            selectByMouse:          true
            selectionColor:         "#4D8BFF"
            selectedTextColor:      "#0F1722"
            onTextChanged:          _pending = text
            onAccepted:             if (_pending.trim().length > 0) LicenseManager.activate(_pending)
            background: Rectangle {
                color:        "#FFFFFF"
                radius:       6
                border.width: keyField.activeFocus ? 2 : 1
                border.color: keyField.activeFocus ? "#4D8BFF" : "#3a4657"
            }
            cursorDelegate: Rectangle { width: 2; color: "#4D8BFF"; visible: keyField.cursorVisible }
        }

        QGCButton {
            text: LicenseManager.busy ? qsTr("Activating…") : qsTr("Activate")
            primary: true
            Layout.fillWidth: true
            enabled: !LicenseManager.busy && _pending.trim().length > 0
            onClicked: LicenseManager.activate(_pending)
        }

        QGCLabel {
            Layout.fillWidth:   true
            horizontalAlignment: Text.AlignHCenter
            wrapMode:           Text.WordWrap
            text:               LicenseManager.statusText
            color:              "#c9a24d"
            visible:            text.length > 0
        }

        QGCLabel {
            Layout.alignment:   Qt.AlignHCenter
            text:               qsTr("Device: ") + LicenseManager.deviceId
            font.family:        ScreenTools.fixedFontFamily
            // Body size and about 6:1 on this background, like the line above.
            color:              "#8b97a6"
            font.pointSize:     ScreenTools.defaultFontPointSize
        }
    }

    // On first show, kick a silent re-check (covers the offline-record case).
    Component.onCompleted: LicenseManager.recheck()
}
