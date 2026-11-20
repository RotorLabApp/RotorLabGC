/****************************************************************************
 *
 * RotorLab GC - "Video recording" settings section (G95).
 *
 * The mode, the folder (QGC's video save path, read only), upload on or off,
 * and the recording queue. Guidance only: nothing here describes a recording
 * as evidence, certified or admissible.
 *
 ****************************************************************************/

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls
import QGroundControl.Palette
import QGroundControl.ScreenTools

ColumnLayout {
    id:         section
    spacing:    ScreenTools.defaultFontPixelHeight / 3

    property color noteColor:  qgcPal.text
    property color warnColor:  qgcPal.warningText

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    QGCLabel {
        Layout.fillWidth:   true
        text:               qsTr("Video recording")
        font.bold:          true
        font.pointSize:     ScreenTools.defaultFontPointSize + 1
    }

    // Builds without GC's own receiver: say so, and nothing else.
    QGCLabel {
        Layout.fillWidth:   true
        visible:            !VideoRecorder.available
        wrapMode:           Text.WordWrap
        text:               VideoRecorder.unavailableText
        color:              section.noteColor
    }

    ColumnLayout {
        Layout.fillWidth:   true
        visible:            VideoRecorder.available
        spacing:            ScreenTools.defaultFontPixelHeight / 3

        RowLayout {
            Layout.fillWidth: true
            QGCLabel { text: qsTr("Record video"); Layout.preferredWidth: ScreenTools.defaultFontPixelWidth * 22 }
            QGCComboBox {
                Layout.fillWidth:   true
                Accessible.name:    qsTr("Record video")
                model:              [ qsTr("Off"), qsTr("While armed"), qsTr("Always while video is received") ]
                currentIndex:       VideoRecorder.mode
                onActivated:        (index) => { VideoRecorder.mode = index }
            }
        }

        QGCLabel {
            Layout.fillWidth:   true
            wrapMode:           Text.WordWrap
            color:              section.noteColor
            text:               qsTr("Saves the video exactly as the camera sends it, with a SHA-256 made while it is written and a time map file next to it. While armed: from the first arm until 10 s after the last disarm.")
        }

        RowLayout {
            Layout.fillWidth: true
            QGCLabel { text: qsTr("Folder"); Layout.preferredWidth: ScreenTools.defaultFontPixelWidth * 22 }
            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WrapAnywhere
                text:               VideoRecorder.folder.length > 0 ? VideoRecorder.folder
                                                                    : qsTr("Not set. Set the application load and save path in General settings.")
            }
            QGCButton {
                text:       qsTr("Open folder")
                enabled:    VideoRecorder.folder.length > 0
                onClicked:  VideoRecorder.openFolder()
            }
        }

        QGCCheckBox {
            text:       qsTr("Upload recordings to the Fleet Server this station is enrolled with")
            checked:    VideoRecorder.uploadToFleet
            onClicked:  VideoRecorder.uploadToFleet = checked
        }

        QGCLabel {
            Layout.fillWidth:   true
            visible:            VideoRecorder.uploadStatusText.length > 0
            wrapMode:           Text.WordWrap
            color:              section.warnColor
            text:               VideoRecorder.uploadStatusText
        }

        QGCLabel {
            Layout.fillWidth:   true
            visible:            VideoRecorder.stoppedText.length > 0
            wrapMode:           Text.WordWrap
            color:              section.warnColor
            text:               VideoRecorder.stoppedText
        }

        QGCLabel {
            Layout.fillWidth:   true
            text:               qsTr("Recordings")
            font.bold:          true
        }

        QGCLabel {
            Layout.fillWidth:   true
            visible:            VideoRecorder.queue.length === 0
            color:              section.noteColor
            text:               qsTr("No recordings yet.")
        }

        Repeater {
            model: VideoRecorder.queue

            delegate: ColumnLayout {
                Layout.fillWidth:   true
                spacing:            0

                required property var modelData

                RowLayout {
                    Layout.fillWidth: true
                    QGCLabel {
                        Layout.fillWidth:   true
                        elide:              Text.ElideMiddle
                        text:               modelData.name
                    }
                    QGCLabel { text: modelData.size }
                    QGCLabel {
                        text:   modelData.stateText
                        color:  modelData.state === "parked" ? section.warnColor : qgcPal.text
                    }
                    QGCButton {
                        text:       qsTr("Try again")
                        visible:    modelData.canRetry
                        onClicked:  VideoRecorder.retry(modelData.id)
                    }
                    QGCButton {
                        text:       qsTr("Check again")
                        visible:    modelData.canCheck
                        onClicked:  VideoRecorder.checkRecords(modelData.id)
                    }
                    QGCButton {
                        text:       qsTr("Send to this server")
                        visible:    modelData.canSend
                        onClicked:  VideoRecorder.sendToThisServer(modelData.id)
                    }
                    QGCButton {
                        text:       qsTr("Delete")
                        visible:    modelData.id.length > 0 && modelData.canDelete
                        onClicked:  section.confirmDelete(modelData.id)
                    }
                }
                QGCLabel {
                    Layout.fillWidth:   true
                    visible:            modelData.detail.length > 0
                    wrapMode:           Text.WordWrap
                    color:              section.noteColor
                    font.pointSize:     ScreenTools.smallFontPointSize
                    text:               modelData.detail
                }
            }
        }
    }

    // The application's own dialog: a bare QtQuick Dialog is unreadable on the dark theme.
    function confirmDelete(id) {
        mainWindow.showMessageDialog(
            qsTr("Delete recording?"),
            VideoRecorder.deleteQuestion(id),
            Dialog.Cancel | Dialog.Ok,
            function() { VideoRecorder.deleteRecording(id) })
    }
}
