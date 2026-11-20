/****************************************************************************
 *
 * RotorLab GC: fly-view line while ATAK command markers are not read or not
 * run (rate limit, repeat check or marker time). Same counts as the notice on
 * the TAK page, updated once a second, gone a minute after the last drop.
 * Informs only; no mouse area, so the map under it still takes input.
 *
 ****************************************************************************/

import QtQuick

import QGroundControl
import QGroundControl.Controls
import QGroundControl.Palette
import QGroundControl.ScreenTools

Rectangle {
    id: line

    readonly property real _pad:  ScreenTools.defaultFontPixelWidth
    readonly property real _maxW: ScreenTools.defaultFontPixelWidth * 56

    width:          Math.min(_maxW, label.implicitWidth + _pad * 4)
    height:         label.implicitHeight + _pad * 2
    radius:         ScreenTools.defaultFontPixelHeight / 3
    color:          qgcPal.windowShade
    border.color:   qgcPal.colorOrange
    border.width:   2

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    QGCLabel {
        id:         label
        x:          _pad * 2
        y:          _pad
        width:      line.width - _pad * 4
        wrapMode:   Text.WordWrap
        textFormat: Text.PlainText
        font.bold:  true
        text:       TAKManager.flyDropNotice
    }
}
