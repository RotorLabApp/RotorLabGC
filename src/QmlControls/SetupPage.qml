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
import QGroundControl.Palette
import QGroundControl.Controls
import QGroundControl.ScreenTools
import QGroundControl.Controllers

/// Base view control for all Setup pages
// RotorLab G97: GC's page frame (A156, RLPageLayout) on every setup page:
// two character widths of side padding, a one-line top inset, scroll bars
// that stay on while the page is larger than the window, the heading in the
// settings style and notes in the readable note color. The page bodies, the
// armed/flying lockout and the page start hook are unchanged.
Item {
    id:             setupView
    enabled:        !_disableDueToArmed && !_disableDueToFlying

    property alias  pageComponent:          pageLoader.sourceComponent
    property string pageName:               vehicleComponent ? vehicleComponent.name : ""
    property string pageDescription:        vehicleComponent ? vehicleComponent.description : ""
    property real   availableWidth:         width - pageLoader.x - _pagePadding // RotorLab G97: padding on both sides
    property real   availableHeight:        height - pageLoader.y - ScreenTools.defaultFontPixelHeight // RotorLab G97: bottom inset, so a full-height page does not scroll
    property bool   showAdvanced:           false
    property alias  advanced:               advancedCheckBox.checked

    // RotorLab G97: read by SettingsGroupLayout, so section notes on setup
    // pages are at body size (same mechanism as the A156 settings pages).
    readonly property bool  bodySizeNotes:  true
    // RotorLab G97: true below about 110 character widths, for a page body
    // that wants to stack its fields under their labels.
    readonly property bool  narrow:         _rlPage.narrow
    readonly property real  _pagePadding:   _rlPage.pagePadding

    property bool   _vehicleIsRover:        globals.activeVehicle ? globals.activeVehicle.rover : false
    property bool   _vehicleArmed:          globals.activeVehicle ? globals.activeVehicle.armed : false
    property bool   _vehicleFlying:         globals.activeVehicle ? globals.activeVehicle.flying : false
    property bool   _disableDueToArmed:     vehicleComponent ? (!vehicleComponent.allowSetupWhileArmed && _vehicleArmed) : false
    // FIXME: The _vehicleIsRover checkl is a hack to work around https://github.com/PX4/Firmware/issues/10969
    property bool   _disableDueToFlying:    vehicleComponent ? (!_vehicleIsRover && !vehicleComponent.allowSetupWhileFlying && _vehicleFlying) : false
    property string _disableReason:         _disableDueToArmed ? qsTr("armed") : qsTr("flying")
    property real   _margins:               ScreenTools.defaultFontPixelHeight * 0.5
    property string _pageTitle:             qsTr("%1 Config").arg(pageName)

    Component.onCompleted: {
        if(pageLoader.item && pageLoader.item.setupPageCompleted) {
            pageLoader.item.setupPageCompleted()
        }
    }

    // RotorLab G97: not shown. The source of GC's page-frame sizes and colors
    // (pagePadding, narrow, noteColor, errorColor), so they are not copied
    // here. Not a parent of the page: bodies size themselves from
    // availableWidth and keep their horizontal scroll fallback.
    RLPageLayout {
        id:         _rlPage
        visible:    false
        width:      setupView.width
        height:     0
    }

    QGCFlickable {
        id:             _pageFlick // RotorLab G97
        anchors.fill:   parent
        contentWidth:   Math.max(availableWidth, pageLoader.x + pageLoader.item.width)
        contentHeight:  Math.max(availableHeight, pageLoader.y + pageLoader.item.height + ScreenTools.defaultFontPixelHeight) // RotorLab G97: one line below the page
        clip:           true
        // RotorLab G97: the fade-in indicators give way to the scroll bars below.
        indicatorColor: "transparent"

        // RotorLab G97: scroll bars styled like RLPageLayout, shown whenever the
        // page is taller (or wider) than the window.
        ScrollBar.vertical: ScrollBar {
            policy: _pageFlick.contentHeight > _pageFlick.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
            contentItem: Rectangle {
                implicitWidth:  ScreenTools.defaultFontPixelWidth * 0.75
                radius:         width / 2
                color:          QGroundControl.globalPalette.text
                opacity:        0.5
            }
        }
        ScrollBar.horizontal: ScrollBar {
            policy: _pageFlick.contentWidth > _pageFlick.width ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
            contentItem: Rectangle {
                implicitHeight: ScreenTools.defaultFontPixelWidth * 0.75
                radius:         height / 2
                color:          QGroundControl.globalPalette.text
                opacity:        0.5
            }
        }

        RowLayout {
            id:                 headingRow
            x:                  _pagePadding                        // RotorLab G97
            y:                  ScreenTools.defaultFontPixelHeight  // RotorLab G97: top inset one line
            width:              availableWidth
            spacing:            _margins
            layoutDirection:    Qt.RightToLeft
            visible:            showAdvanced || (pageDescription !== "" && !ScreenTools.isShortScreen)

            QGCCheckBox {
                id:         advancedCheckBox
                text:       qsTr("Advanced")
                visible:    showAdvanced
            }

            ColumnLayout {
                spacing:            _margins
                Layout.fillWidth:   true

                QGCLabel {
                    Layout.fillWidth:   true
                    // RotorLab G97: settings heading style; the disabled note in GC's readable red.
                    font.pointSize:     ScreenTools.defaultFontPointSize + 1
                    font.bold:          true
                    text:               !setupView.enabled ? _pageTitle + "<font color=\"" + _rlPage.errorColor + "\">" + qsTr(" (Disabled while the vehicle is %1)").arg(_disableReason) + "</font>" : _pageTitle
                    visible:            !ScreenTools.isShortScreen
                }

                QGCLabel {
                    Layout.fillWidth:   true
                    wrapMode:           Text.WordWrap
                    font.pointSize:     ScreenTools.defaultFontPointSize   // RotorLab G97: body size
                    color:              _rlPage.noteColor                  // RotorLab G97
                    text:               pageDescription
                    visible:            pageDescription !== "" && !ScreenTools.isShortScreen
                }
            }
        }

        Loader {
            id:                 pageLoader
            x:                  _pagePadding    // RotorLab G97
            anchors.topMargin:  _margins
            anchors.top:        headingRow.bottom
        }

        // Overlay to display when vehicle is armed and this setup page needs
        // to be disabled
        Rectangle {
            visible:            !setupView.enabled
            anchors.fill:       parent
            color:              "black"
            opacity:            0.5
        }
    }
}
