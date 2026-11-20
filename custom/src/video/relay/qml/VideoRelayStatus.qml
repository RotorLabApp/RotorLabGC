/****************************************************************************
 *
 * RotorLab GC - G109: live relay chip for the Fly view.
 *
 * Shown only when the station mode is forward or both. Read only: the chip
 * never takes input, so it can never get in the way of flying.
 * The Fly view sets maxWidth; a long state wraps inside it (G88 M2).
 *
 ****************************************************************************/

import QtQuick
import QtQuick.Controls

import QGroundControl
import QGroundControl.Controls
import QGroundControl.Palette
import QGroundControl.ScreenTools

Rectangle {
    id:         chip
    readonly property bool shown: VideoRelay.statusVisible && VideoRelay.chipText.length > 0
    visible:    shown
    width:      Math.min(label.implicitWidth, maxWidth - ScreenTools.defaultFontPixelWidth * 2) + ScreenTools.defaultFontPixelWidth * 2
    height:     label.implicitHeight + ScreenTools.defaultFontPixelHeight * 0.4
    // A two-line chip stays a rounded rectangle
    radius:     Math.min(height / 2, ScreenTools.defaultFontPixelHeight * 0.75)
    // G88 M3: the same gray fill as the record chip
    color:      VideoRelay.forwarding ? "#b03030" : Qt.rgba(0, 0, 0, 0.72)

    property real maxWidth: ScreenTools.defaultFontPixelWidth * 40

    // A status item for screen readers
    Accessible.role:        Accessible.StatusBar
    Accessible.name:        qsTr("Live relay: %1").arg(VideoRelay.chipText)
    Accessible.description: VideoRelay.statusText

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    QGCLabel {
        id:                     label
        anchors.centerIn:       parent
        width:                  Math.min(implicitWidth, chip.maxWidth - ScreenTools.defaultFontPixelWidth * 2)
        wrapMode:               Text.WordWrap
        horizontalAlignment:    Text.AlignHCenter
        color:                  "white"
        text:                   VideoRelay.chipText
    }

    // The full status line on hover, for the operator who wants the numbers.
    ToolTip.visible:    hover.hovered && VideoRelay.statusText.length > 0
    ToolTip.text:       VideoRelay.statusText
    HoverHandler { id: hover }
}
