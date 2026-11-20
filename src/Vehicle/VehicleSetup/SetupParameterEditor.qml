/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/


import QtQuick
import QtQuick.Controls

import QGroundControl.Controls
import QGroundControl.ScreenTools
import QGroundControl.Palette

// RotorLab G97: the parameter editor inside GC's page padding (A156,
// RLPageLayout: two character widths each side, a one-line top inset).
// Padding only: the editor itself is unchanged.
Item {
    RLPageLayout {
        id:         _rlPage
        visible:    false
        width:      parent.width
        height:     0
    }

    ParameterEditor {
        anchors.fill:           parent
        anchors.leftMargin:     _rlPage.pagePadding
        anchors.rightMargin:    _rlPage.pagePadding
        anchors.topMargin:      ScreenTools.defaultFontPixelHeight
    }
}
