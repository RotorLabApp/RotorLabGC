/****************************************************************************
 *
 * RotorLab GC License settings page. Activate / deactivate the app license
 * and see its status. Binds to the native LicenseManager (`LicenseManager`).
 * The device id is generated silently and shown masked; the store URL and
 * API key are never surfaced.
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
    property string _pending:  ""
    // Masked until the user presses Show, and not kept once the
    // key activates, so a later deactivate shows an empty field.
    property bool   _reveal:   false
    property bool   _vehicleConnected: QGroundControl.multiVehicleManager.vehicles.count > 0

    Connections {
        target: LicenseManager
        function onActivatedChanged() {
            if (LicenseManager.activated) {
                root._pending = ""
                root._reveal = false
                keyField.clear()
            }
        }
    }

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    // Note and status colors, and when the page is narrow, come from the
    // shared page layout (A156).
    readonly property color _noteColor:  page.noteColor
    readonly property color _warnColor:  page.warnColor
    readonly property color _okColor:    page.okColor

    component Card: ColumnLayout {
        property alias title: _h.text
        default property alias content: _body.data
        Layout.fillWidth:   true
        spacing:            ScreenTools.defaultFontPixelHeight / 3
        QGCLabel {
            id:                 _h
            Layout.fillWidth:   true
            wrapMode:           Text.WordWrap
            font.bold:          true
            font.pointSize:     ScreenTools.defaultFontPointSize + 1
        }
        Rectangle {
            Layout.fillWidth:   true
            color:              qgcPal.windowShade
            border.color:       qgcPal.groupBorder
            border.width:       1
            radius:             ScreenTools.defaultFontPixelHeight / 2
            implicitHeight:     _body.implicitHeight + _pad * 2
            ColumnLayout {
                id:         _body
                x:          _pad
                y:          _pad
                width:      parent.width - _pad * 2
                spacing:    ScreenTools.defaultFontPixelHeight / 2
            }
        }
    }

    // The shared page layout (A156): full width minus the standard settings
    // padding, scrolls vertically, and says when the page is narrow.
    RLPageLayout {
        id:             page
        anchors.fill:   parent

        ColumnLayout {
            id:                 outer
            Layout.fillWidth:   true
            spacing:            ScreenTools.defaultFontPixelHeight

            Card {
                title: qsTr("License status")
                RowLayout {
                    Layout.fillWidth: true
                    QGCLabel {
                        Layout.fillWidth: true
                        text: !LicenseManager.activated ? qsTr("Not activated")
                              : LicenseManager.attention ? qsTr("Activated for this session")
                              : qsTr("Activated")
                        color: LicenseManager.activated && !LicenseManager.attention ? _okColor : _warnColor
                        font.bold: true
                    }
                    BusyIndicator {
                        running: LicenseManager.busy
                        visible: LicenseManager.busy
                        implicitWidth:  ScreenTools.defaultFontPixelHeight * 1.5
                        implicitHeight: ScreenTools.defaultFontPixelHeight * 1.5
                    }
                }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: LicenseManager.statusText
                    color: LicenseManager.attention ? _warnColor : qgcPal.text
                }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible: LicenseManager.activated
                    color: _noteColor
                    text: qsTr("RotorLab GC re-checks the license every 24 hours and when the network comes back. " +
                               "A re-check while running never closes RotorLab GC or interrupts a flight; " +
                               "the next start applies its result.")
                }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WrapAnywhere
                    visible: LicenseManager.activated
                    text: qsTr("Key: ") + LicenseManager.licenseKey
                    font.family: ScreenTools.fixedFontFamily
                    color: qgcPal.text
                }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WrapAnywhere
                    text: qsTr("Device: ") + LicenseManager.deviceId
                    font.family: ScreenTools.fixedFontFamily
                    color: _noteColor
                }
            }

            Card {
                title: qsTr("Activate")
                visible: !LicenseManager.activated
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Enter your RotorLab GC license key to activate this device. " +
                               "The key is tied to this install; deactivate here before moving it to another device.")
                }
                // Placed by the shared rule in RLPageLayout: the field right
                // after its label, or under it on a narrow page.
                GridLayout {
                    id:                 keyRow
                    Layout.fillWidth:   true
                    columns:            page.narrow ? 2 : 4
                    columnSpacing:      _pad * 2
                    rowSpacing:         _pad / 2
                    QGCLabel {
                        text:                   qsTr("License key")
                        Layout.alignment:       Qt.AlignVCenter
                        Layout.fillWidth:       page.narrow
                        Layout.preferredWidth:  page.narrow ? -1 : page.labelColumnWidth(keyRow.width)
                        Layout.columnSpan:      page.narrow ? 2 : 1
                        wrapMode:               Text.WordWrap
                    }
                    TextField {
                        id:                     keyField
                        objectName:             "licenseSettingsKeyField"
                        Layout.fillWidth:       true
                        Layout.minimumWidth:    ScreenTools.defaultFontPixelWidth * 8
                        Layout.maximumWidth:    page.fieldMaxWidthFor(keyRow.width,
                                                                      keyRow.columnSpacing * 3 + revealButton.width)
                        Layout.preferredWidth:  Layout.maximumWidth
                        Layout.preferredHeight: ScreenTools.defaultFontPixelHeight * 2
                        color:                  page.inputTextColor
                        text:                   _pending
                        echoMode:               _reveal ? TextInput.Normal : TextInput.Password
                        inputMethodHints:       Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                        placeholderText:        qsTr("XXXX-XXXX-XXXX-XXXX")
                        Accessible.name:        qsTr("License key")
                        placeholderTextColor:   page.placeholderColor
                        selectByMouse:          true
                        selectionColor:         page.focusColor
                        selectedTextColor:      "#0F1722"
                        leftPadding:            ScreenTools.defaultFontPixelWidth
                        onTextChanged:          _pending = text
                        background: Rectangle {
                            color:        page.inputColor
                            radius:       4
                            border.width: keyField.activeFocus ? 2 : 1
                            border.color: keyField.activeFocus ? page.focusColor : page.inputBorderColor
                        }
                        cursorDelegate: Rectangle { width: 2; color: page.focusColor; visible: keyField.cursorVisible }
                    }
                    QGCButton {
                        id:        revealButton
                        text:      _reveal ? qsTr("Hide") : qsTr("Show")
                        // Reachable with Tab so a keyboard user can check the key.
                        focusPolicy:     Qt.StrongFocus
                        checkable:       true
                        checked:         root._reveal
                        Accessible.name: root._reveal ? qsTr("Hide license key") : qsTr("Show license key")
                        onClicked:       _reveal = !_reveal

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
                    Item { Layout.fillWidth: true; visible: !page.narrow }
                }
                RowLayout {
                    Layout.fillWidth: true
                    QGCButton {
                        text: qsTr("Activate")
                        enabled: !LicenseManager.busy && _pending.trim().length > 0
                        primary: true
                        onClicked: LicenseManager.activate(_pending)
                    }
                }
            }

            Card {
                title: qsTr("Manage")
                visible: LicenseManager.activated
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Deactivating releases this key so it can be activated on another device. " +
                               "You will need to re-enter the key to use RotorLab GC on this device again.")
                }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Without a connection, an organization admin can release this key in RotorLab: " +
                               "Organization, Billing, Product licenses, then Release on this device's activation.")
                }
                // Deactivating raises the gate, and GC opens no vehicle link
                // behind it, so it waits until no vehicle is connected.
                QGCLabel {
                    objectName: "licenseDeactivateBlocked"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: LicenseManager.deactivateVehicleText
                    visible: root._vehicleConnected
                }
                RowLayout {
                    Layout.fillWidth: true
                    QGCButton {
                        objectName: "licenseDeactivate"
                        text: qsTr("Deactivate this device")
                        enabled: !LicenseManager.busy && !root._vehicleConnected
                        onClicked: LicenseManager.deactivate()
                    }
                    QGCButton {
                        text: qsTr("Re-check now")
                        enabled: !LicenseManager.busy
                        onClicked: LicenseManager.recheck()
                    }
                }
            }
        }
    }
}
