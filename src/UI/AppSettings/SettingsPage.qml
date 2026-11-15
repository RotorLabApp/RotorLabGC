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
import QtQuick.Dialogs
import QtQuick.Layouts

import QGroundControl
import QGroundControl.FactSystem
import QGroundControl.FactControls
import QGroundControl.Controls
import QGroundControl.ScreenTools
import QGroundControl.MultiVehicleManager
import QGroundControl.Palette

// RotorLab: A156. A thin wrapper around RLPageLayout, so every settings page
// fills the window and scrolls instead of sitting in a narrow centered column.
Item {
    id: root

    default property alias contentItem: _page.contentItem

    // RotorLab: A156. True below about 1000 px, for a page whose own body
    // sets two columns side by side.
    readonly property bool narrow: _page.narrow

    RLPageLayout {
        id:             _page
        anchors.fill:   parent
    }
}
