/****************************************************************************
 *
 * RotorLab GC - "Video recording and relay" settings page (G95, G109).
 *
 * The recording section (G95) and, when the relay is in this build, the live
 * relay section (G109), on the shared page layout.
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

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    RLPageLayout {
        id:             page
        anchors.fill:   parent

        ColumnLayout {
            Layout.fillWidth:   true
            spacing:            ScreenTools.defaultFontPixelHeight

            Loader {
                Layout.fillWidth:   true
                source:             "qrc:/custom/video/VideoRecordSettings.qml"
                onLoaded: {
                    item.noteColor = page.noteColor
                    item.warnColor = page.warnColor
                }
            }

            // G109: present once the relay module registers `VideoRelay`.
            Loader {
                Layout.fillWidth:   true
                active:             typeof VideoRelay !== "undefined"
                source:             active ? "qrc:/custom/video/VideoRelaySettings.qml" : ""
            }
        }
    }
}
