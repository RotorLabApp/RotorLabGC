/****************************************************************************
 *
 * RotorLab GC startup license gate. A full-window modal overlay shown until
 * the app is activated. Blocks all interaction with the app behind it.
 * Binds to the native LicenseManager (`LicenseManager`).
 *
 * Shown only while LicenseManager.activated is false. LicenseManager decides
 * that at start from the RotorLab-signed offline record (G89) and never sets
 * it false during a session, except when the user deactivates. It never
 * shows this gate over a connected vehicle for an activation that only ran
 * out of time: GC stays open for that session (LicenseManager::_openForVehicle).
 * The status line carries the reason, for example "Activation expired.
 * Connect to the internet so RotorLab GC can check the license."
 *
 * No link behind the gate: while LicenseManager.linksAllowed is
 * false GC opens no vehicle link, and the gate says so.
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
    // The key field is masked like every other screen that holds the key;
    // the user presses Show to see it, as on the RotorLab
    // Connector page's Device Key field.
    property bool   _reveal:  false

    // A key typed here is not kept once it activates, so after "Deactivate
    // this device" the gate comes back with an empty, masked field.
    function _forgetKey() {
        _pending = ""
        _reveal = false
        keyField.clear()
        keyField.forceActiveFocus()
    }
    Connections {
        target: LicenseManager
        function onActivatedChanged() { if (LicenseManager.activated) gate._forgetKey() }
    }
    // The field takes focus whenever the gate shows, so the key can be typed
    // without a click first.
    onVisibleChanged: if (visible) keyField.forceActiveFocus()

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

        // A visible caption: the placeholder goes away at the first character,
        // and the masked dots alone do not say what the field holds.
        QGCLabel {
            id:                 keyCaption
            text:               qsTr("License key")
            color:              "#e6edf3"
            Layout.bottomMargin: -ScreenTools.defaultFontPixelHeight / 2
        }

        RowLayout {
            Layout.fillWidth:   true
            spacing:            ScreenTools.defaultFontPixelWidth

            TextField {
                id:                     keyField
                objectName:             "licenseGateKeyField"
                Layout.fillWidth:       true
                Layout.preferredHeight: ScreenTools.defaultFontPixelHeight * 2.4
                horizontalAlignment:    TextInput.AlignHCenter
                color:                  "#0F1722"
                text:                   _pending
                echoMode:               _reveal ? TextInput.Normal : TextInput.Password
                inputMethodHints:       Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                placeholderText:        qsTr("Enter license key")
                Accessible.name:        keyCaption.text
                placeholderTextColor:   "#8A93A0"
                enabled:                !LicenseManager.busy
                selectByMouse:          true
                selectionColor:         "#4D8BFF"
                selectedTextColor:      "#0F1722"
                onTextChanged:          _pending = text
                onAccepted:             if (_pending.trim().length > 0) LicenseManager.activate(_pending)
                // Tab stays inside the gate: the field and Show are the only
                // Tab stops (Activate takes click focus only), so focus never
                // moves to the hidden app behind the gate.
                KeyNavigation.tab:      revealButton
                KeyNavigation.backtab:  revealButton
                background: Rectangle {
                    color:        "#FFFFFF"
                    radius:       6
                    border.width: keyField.activeFocus ? 2 : 1
                    border.color: keyField.activeFocus ? "#4D8BFF" : "#3a4657"
                }
                cursorDelegate: Rectangle { width: 2; color: "#4D8BFF"; visible: keyField.cursorVisible }
            }

            QGCButton {
                id:                     revealButton
                objectName:             "licenseGateReveal"
                Layout.preferredHeight: keyField.Layout.preferredHeight
                text:                   _reveal ? qsTr("Hide") : qsTr("Show")
                // Reachable with Tab, unlike other QGCButtons, so a keyboard
                // user can check the typed key. Space toggles it.
                focusPolicy:            Qt.StrongFocus
                checkable:              true
                checked:                gate._reveal
                Accessible.name:        gate._reveal ? qsTr("Hide license key") : qsTr("Show license key")
                onClicked:              _reveal = !_reveal
                KeyNavigation.tab:      keyField
                KeyNavigation.backtab:  keyField

                // Visible focus mark for Tab users (QGCButton draws none).
                Rectangle {
                    anchors.fill:   parent
                    z:              1
                    color:          "transparent"
                    radius:         revealButton.backRadius
                    border.width:   2
                    border.color:   "#4D8BFF"
                    visible:        revealButton.activeFocus
                }
            }
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
            objectName:         "licenseGateNoLinks"
            Layout.fillWidth:   true
            horizontalAlignment: Text.AlignHCenter
            wrapMode:           Text.WordWrap
            text:               qsTr("RotorLab GC does not connect to vehicles until this device is activated.")
            color:              "#c9d1d9"
            visible:            !LicenseManager.linksAllowed
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
    Component.onCompleted: {
        LicenseManager.recheck()
        if (visible) keyField.forceActiveFocus()
    }
}
