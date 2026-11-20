/****************************************************************************
 *
 * RotorLab GC - G109: "Live relay" section of the video settings page.
 *
 * An embeddable section of the video recording page. Binds only
 * to the VideoRelay context property. Nothing here blocks or gates flying.
 *
 ****************************************************************************/

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls
import QGroundControl.Palette
import QGroundControl.ScreenTools

ColumnLayout {
    id:         root
    spacing:    ScreenTools.defaultFontPixelHeight * 0.5

    property real keyWidth:   ScreenTools.defaultFontPixelWidth * 22
    property color noteColor: qgcPal.text

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    readonly property var _modes: [
        { key: "view",    text: qsTr("View: the Fly view only, nothing is sent") },
        { key: "forward", text: qsTr("Forward: send to the Fleet Server, hide the Fly view video") },
        { key: "both",    text: qsTr("Both: send to the Fleet Server and show the Fly view video") }
    ]

    function _modeIndex() {
        for (var i = 0; i < _modes.length; i++) {
            if (_modes[i].key === VideoRelay.stationMode) {
                return i
            }
        }
        return 0
    }

    function _qualityIndex() {
        var opts = VideoRelay.qualityOptions
        for (var i = 0; i < opts.length; i++) {
            if (opts[i].key === VideoRelay.effectiveQuality) {
                return i
            }
        }
        return 0
    }

    QGCLabel {
        text:           qsTr("Live relay")
        font.bold:      true
    }

    QGCLabel {
        Layout.fillWidth:   true
        visible:            !VideoRelay.available
        wrapMode:           Text.WordWrap
        text:               qsTr("Live relay needs this version's built-in video receiver, which this build does not include.")
    }

    QGCLabel {
        Layout.fillWidth:   true
        visible:            VideoRelay.available && VideoRelay.enrollmentNote.length > 0
        wrapMode:           Text.WordWrap
        color:              root.noteColor
        text:               VideoRelay.enrollmentNote
    }

    RowLayout {
        Layout.fillWidth:   true
        visible:            VideoRelay.available

        QGCLabel {
            Layout.preferredWidth:  root.keyWidth
            text:                   qsTr("Station mode")
        }
        QGCComboBox {
            id:                     modeCombo
            Layout.fillWidth:       true
            Accessible.name:        qsTr("Station mode")
            enabled:                VideoRelay.fleetReady
            model:                  root._modes
            textRole:               "text"
            currentIndex:           VideoRelay.fleetReady ? root._modeIndex() : 0
            onActivated: (index) => { VideoRelay.stationMode = root._modes[index].key }
        }
    }

    RowLayout {
        Layout.fillWidth:   true
        visible:            VideoRelay.available && VideoRelay.fleetReady && VideoRelay.relayAllowed

        QGCLabel {
            Layout.preferredWidth:  root.keyWidth
            text:                   qsTr("Forward quality")
        }
        QGCComboBox {
            Layout.fillWidth:       true
            Accessible.name:        qsTr("Forward quality")
            model:                  VideoRelay.qualityOptions
            textRole:               "text"
            currentIndex:           root._qualityIndex()
            onActivated: (index) => { VideoRelay.quality = VideoRelay.qualityOptions[index].key }
        }
    }

    QGCLabel {
        Layout.fillWidth:   true
        visible:            VideoRelay.available && VideoRelay.fleetReady && VideoRelay.qualityReason.length > 0
        wrapMode:           Text.WordWrap
        color:              root.noteColor
        text:               VideoRelay.qualityReason
    }

    QGCLabel {
        Layout.fillWidth:   true
        visible:            VideoRelay.available && VideoRelay.encoderText.length > 0
        wrapMode:           Text.WordWrap
        color:              root.noteColor
        text:               qsTr("Encoder for Low and Medium: %1").arg(VideoRelay.encoderText)
    }

    RowLayout {
        Layout.fillWidth:   true
        visible:            VideoRelay.statusVisible && VideoRelay.statusText.length > 0

        QGCLabel {
            Layout.preferredWidth:  root.keyWidth
            text:                   qsTr("Status")
        }
        QGCLabel {
            Layout.fillWidth:       true
            wrapMode:               Text.WordWrap
            text:                   VideoRelay.statusText
        }
        QGCButton {
            visible:                VideoRelay.canRetry
            text:                   qsTr("Try again")
            onClicked:              VideoRelay.retry()
        }
    }
}
