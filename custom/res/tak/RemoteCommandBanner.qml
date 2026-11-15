/****************************************************************************
 *
 * RotorLab GC: station alert for an ARM, TKOFF or GOTO received from ATAK
 * (or any command above the station's altitude warning level).
 *
 * Informs only. The command has already gone to the aircraft when this
 * shows: there is no cancel window and nothing here holds or changes it.
 * The operator dismisses the banner; the entry stays in the "Remote commands
 * received" list on the TAK page. Plain rectangle with no mouse area except
 * the Dismiss button, so the map under it still takes input.
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
    id: banner

    readonly property var  _alerts: TAKManager.bannerAlerts
    readonly property var  _alert:  _alerts.length > 0 ? _alerts[0] : null
    readonly property real _pad:    ScreenTools.defaultFontPixelWidth
    readonly property real _maxW:   ScreenTools.defaultFontPixelWidth * 56

    visible:        _alert !== null
    width:          Math.min(_maxW, col.implicitWidth + _pad * 4)
    height:         col.implicitHeight + _pad * 2
    radius:         ScreenTools.defaultFontPixelHeight / 3
    color:          qgcPal.windowShade
    border.color:   qgcPal.colorBlue
    border.width:   2

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    // Read at a glance: the title block (its own fill and a blue border, so
    // orange stays for the altitude warning only) holds the title, the verb
    // in large bold type (at least 24 px), the reported sender, and the
    // aircraft on its own line under it.
    // The altitude warning goes directly under it.
    ColumnLayout {
        id:         col
        x:          _pad * 2
        y:          _pad
        width:      banner.width - _pad * 4
        spacing:    ScreenTools.defaultFontPixelHeight / 4

        Rectangle {
            Layout.fillWidth:   true
            implicitHeight:     head.implicitHeight + _pad * 2
            radius:             ScreenTools.defaultFontPixelHeight / 4
            color:              qgcPal.windowShadeDark
            border.color:       qgcPal.colorBlue
            border.width:       2

            ColumnLayout {
                id:         head
                x:          _pad
                y:          _pad
                width:      parent.width - _pad * 2
                spacing:    0

                RowLayout {
                    Layout.fillWidth: true
                    QGCLabel {
                        Layout.fillWidth:   true
                        font.bold:          true
                        wrapMode:           Text.WordWrap
                        textFormat:         Text.PlainText
                        text:               _alert ? _alert.title : ""
                    }
                    QGCButton {
                        text:       qsTr("Dismiss")
                        onClicked:  if (_alert) TAKManager.dismissBanner(_alert.id)
                    }
                }
                QGCLabel {
                    Layout.fillWidth:   true
                    font.bold:          true
                    font.pixelSize:     Math.max(24, Math.round(ScreenTools.defaultFontPixelHeight * 1.75))
                    wrapMode:           Text.WordWrap
                    textFormat:         Text.PlainText
                    text:               _alert ? _alert.verb : ""
                }
                QGCLabel {
                    Layout.fillWidth:   true
                    wrapMode:           Text.WordWrap
                    textFormat:         Text.PlainText
                    text: _alert ? qsTr("Reported sender: %1").arg(_alert.sender) : ""
                }
                QGCLabel {
                    Layout.fillWidth:   true
                    wrapMode:           Text.WordWrap
                    textFormat:         Text.PlainText
                    text: _alert ? qsTr("Aircraft: %1").arg(_alert.targets) : ""
                }
            }
        }
        QGCLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            visible: _alert !== null && _alert.warning !== ""
            font.bold: true
            color: qgcPal.colorOrange
            text: _alert ? _alert.warning : ""
        }
        QGCLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            visible: _alert !== null && _alert.position !== ""
            text: _alert ? qsTr("Point: %1").arg(_alert.position) : ""
        }
        QGCLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            text: _alert ? qsTr("At: %1").arg(_alert.time) : ""
        }
        QGCLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            text: _alert ? qsTr("Result: %1").arg(_alert.result) : ""
        }
        QGCLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            visible: _alert !== null && _alert.test
            text: qsTr("Test command from this station.")
        }
        // When a command went to an aircraft. Otherwise the footer below says
        // that this was an alert only.
        QGCLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            visible: _alert !== null && _alert.sent === true
            text: qsTr("Sent to the aircraft when received. This alert does not hold or cancel it. " +
                       "Sender identity is as reported by ATAK and is not verified by this station. " +
                       "Sent means transmitted to the aircraft. The aircraft's response is not shown here.")
        }
        QGCLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            visible: _alert !== null && _alert.sent !== true
            text: qsTr("Alert only. Nothing was sent to the aircraft.")
        }
        QGCLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            visible: TAKManager.commandLogError
            color: qgcPal.colorRed
            text: qsTr("Command log could not be written")
        }
        QGCLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            visible: _alerts.length > 1
            text: qsTr("%1 more not dismissed. All received commands are listed on the TAK page.").arg(_alerts.length - 1)
        }
    }
}
