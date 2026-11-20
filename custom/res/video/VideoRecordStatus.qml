/****************************************************************************
 *
 * RotorLab GC - recording status chip for the Fly view (G95).
 *
 * "REC" with the elapsed time and the part size while recording;
 * "Recording stopped: <reason>" when recording stopped by itself; hidden when
 * recording is off. Never interactive, never in the way of a control.
 * The Fly view sets maxWidth; a long stop reason wraps inside it (G88 M2).
 *
 ****************************************************************************/

import QtQuick
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls
import QGroundControl.Palette
import QGroundControl.ScreenTools

Rectangle {
    id:         chip
    readonly property bool shown: VideoRecorder.available && VideoRecorder.chipVisible
    visible:    shown
    radius:     ScreenTools.defaultFontPixelHeight / 4
    // G88 M3: dark enough that the amber stop reason passes contrast over bright video
    color:      Qt.rgba(0, 0, 0, 0.72)
    width:      row.implicitWidth + ScreenTools.defaultFontPixelWidth * 1.5
    height:     row.implicitHeight + ScreenTools.defaultFontPixelHeight / 3

    property real maxWidth: ScreenTools.defaultFontPixelWidth * 40

    // A status item for screen readers; no running timer in the name, so it is not
    // announced again every second
    Accessible.role:    Accessible.StatusBar
    Accessible.name:    VideoRecorder.recording ? qsTr("Video recording") : VideoRecorder.stoppedText

    RowLayout {
        id:                 row
        anchors.centerIn:   parent
        spacing:            ScreenTools.defaultFontPixelWidth / 2

        Rectangle {
            visible:    VideoRecorder.recording
            width:      ScreenTools.defaultFontPixelHeight / 2
            height:     width
            radius:     width / 2
            color:      "#e5484d"
        }
        QGCLabel {
            visible:                VideoRecorder.recording
            text:                   qsTr("REC") + "  " + VideoRecorder.elapsedText + "  " + VideoRecorder.partSizeText
            color:                  "white"
            font.bold:              true
            // The part size goes first when the chip is narrow; "REC" never does
            Layout.maximumWidth:    chip.maxWidth - ScreenTools.defaultFontPixelWidth * 3
            elide:                  Text.ElideRight
        }
        QGCLabel {
            visible:                !VideoRecorder.recording && VideoRecorder.stoppedText.length > 0
            text:                   VideoRecorder.stoppedText
            color:                  "#ffcc66"
            Layout.maximumWidth:    chip.maxWidth - ScreenTools.defaultFontPixelWidth * 1.5
            wrapMode:               Text.WordWrap
        }
    }
}
