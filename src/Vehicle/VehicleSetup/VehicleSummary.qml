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

import QGroundControl
import QGroundControl.FactSystem
import QGroundControl.Controls
import QGroundControl.ScreenTools
import QGroundControl.MultiVehicleManager
import QGroundControl.Palette
import QGroundControl.AutoPilotPlugins.PX4
import QGroundControl.AutoPilotPlugins.APM

// RotorLab G97: GC's page frame (A156, RLPageLayout) on the Summary page: two
// character widths of side padding, a one-line top inset and a scroll bar
// that stays on while the page is taller than the window. Cards in the
// settings section style, all as tall as the tallest summary.
Rectangle {
    id:             _summaryRoot
    anchors.fill:   parent
    color:          qgcPal.window

    property real _minSummaryW:     ScreenTools.isTinyScreen ? ScreenTools.defaultFontPixelWidth * 28 : ScreenTools.defaultFontPixelWidth * 36
    property real _summaryBoxWidth: _minSummaryW
    property real _summaryBoxSpace: ScreenTools.defaultFontPixelWidth * 2

    // RotorLab G97: the column inside the page padding, card margins, and the
    // tallest card body so far (every card in the grid gets that height).
    readonly property real summaryContentWidth: Math.max(0, width - _rlPage.pagePadding * 2)
    readonly property real _cardMargins:        ScreenTools.defaultFontPixelHeight / 2
    property real _summaryBodyHeight:           ScreenTools.defaultFontPixelHeight * 3

    function computeSummaryBoxSize() {
        var sw  = 0
        var rw  = 0
        var idx = Math.floor(summaryContentWidth / (_minSummaryW + ScreenTools.defaultFontPixelWidth))   // RotorLab G97: padded width
        if(idx < 1) {
            _summaryBoxWidth = summaryContentWidth
            _summaryBoxSpace = 0
        } else {
            _summaryBoxSpace = 0
            if(idx > 1) {
                _summaryBoxSpace = ScreenTools.defaultFontPixelWidth * 2
                sw = _summaryBoxSpace * (idx - 1)
            }
            rw = summaryContentWidth - sw
            _summaryBoxWidth = rw / idx
        }
    }

    function capitalizeWords(sentence) {
        return sentence.replace(/(?:^|\s)\S/g, function(a) { return a.toUpperCase(); });
    }

    // RotorLab G97: the height a summary body asks for. Every summary is an
    // Item that fills its card with a Column, so the Column's implicit height
    // is the content height.
    function _bodyContentHeight(item) {
        var h = 0
        if (item) {
            for (var i = 0; i < item.children.length; i++) {
                h = Math.max(h, item.children[i].implicitHeight)
            }
        }
        return h
    }

    QGCPalette {
        id:                 qgcPal
        colorGroupEnabled:  enabled
    }

    // RotorLab G97: not shown; the source of GC's page-frame padding and colors.
    RLPageLayout {
        id:         _rlPage
        visible:    false
        width:      _summaryRoot.width
        height:     0
    }

    Component.onCompleted: {
        computeSummaryBoxSize()
    }

    onSummaryContentWidthChanged: {
        computeSummaryBoxSize()
    }

    QGCFlickable {
        id:                 _summaryFlick   // RotorLab G97
        clip:               true
        anchors.fill:       parent
        contentHeight:      summaryColumn.height + ScreenTools.defaultFontPixelHeight * 2  // RotorLab G97: top and bottom inset
        contentWidth:       _summaryRoot.width
        flickableDirection: Flickable.VerticalFlick
        indicatorColor:     "transparent"   // RotorLab G97: the scroll bar below replaces the fade-in indicator

        // RotorLab G97: styled like RLPageLayout, on whenever the page is taller than the window.
        ScrollBar.vertical: ScrollBar {
            policy: _summaryFlick.contentHeight > _summaryFlick.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
            contentItem: Rectangle {
                implicitWidth:  ScreenTools.defaultFontPixelWidth * 0.75
                radius:         width / 2
                color:          qgcPal.text
                opacity:        0.5
            }
        }

        Column {
            id:             summaryColumn
            x:              _rlPage.pagePadding                 // RotorLab G97
            y:              ScreenTools.defaultFontPixelHeight  // RotorLab G97
            width:          summaryContentWidth
            spacing:        ScreenTools.defaultFontPixelHeight

            QGCLabel {
                width:			parent.width
                wrapMode:		Text.WordWrap
                color:			setupComplete ? _rlPage.noteColor : _rlPage.errorColor   // RotorLab G97: readable colors
                font.bold:      true
                text:           setupComplete ?
                    qsTr("Below you will find a summary of the settings for your vehicle. To the left are the setup menus for each component.") :
                    qsTr("WARNING: Your vehicle requires setup prior to flight. Please resolve the items marked in red using the menu on the left.")

                property bool setupComplete: QGroundControl.multiVehicleManager.activeVehicle ? QGroundControl.multiVehicleManager.activeVehicle.autopilotPlugin.setupComplete : false
            }

            Flow {
                id:         _flowCtl
                width:      summaryContentWidth
                spacing:    _summaryBoxSpace

                Repeater {
                    model: QGroundControl.multiVehicleManager.activeVehicle ? QGroundControl.multiVehicleManager.activeVehicle.autopilotPlugin.vehicleComponents : undefined

                    // RotorLab G97: a summary card in the SettingsGroupLayout look,
                    // a bold heading over a bordered, rounded box. The heading
                    // opens the component page, as the title button did.
                    Item {
                        width:      _summaryBoxWidth
                        height:     titleBar.height + _summaryBodyHeight + _cardMargins * 2.5
                        visible:    modelData.summaryQmlSource.toString() !== ""

                        // Title row
                        Item {
                            id:     titleBar
                            width:  parent.width
                            height: ScreenTools.defaultFontPixelHeight * 1.75

                            readonly property bool _canOpen: modelData.setupSource.toString() !== ""

                            QGCLabel {
                                anchors.left:           parent.left
                                anchors.leftMargin:     _cardMargins
                                anchors.right:          setupDot.visible ? setupDot.left : parent.right
                                anchors.rightMargin:    _cardMargins
                                anchors.verticalCenter: parent.verticalCenter
                                text:                   capitalizeWords(modelData.name)
                                font.pointSize:         ScreenTools.defaultFontPointSize + 1
                                font.bold:              true
                                font.underline:         titleMouse.containsMouse && titleBar._canOpen
                                color:                  titleMouse.containsMouse && titleBar._canOpen ? qgcPal.buttonHighlight : qgcPal.text
                                elide:                  Text.ElideRight
                            }

                            // Setup indicator
                            Rectangle {
                                id:                     setupDot
                                anchors.rightMargin:    _cardMargins
                                anchors.right:          parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                width:                  ScreenTools.defaultFontPixelWidth * 1.75
                                height:                 width
                                radius:                 width / 2
                                color:                  modelData.setupComplete ? _rlPage.okColor : _rlPage.errorColor   // RotorLab G97
                                visible:                modelData.requiresSetup && modelData.setupSource !== ""
                            }

                            MouseArea {
                                id:             titleMouse
                                anchors.fill:   parent
                                hoverEnabled:   true
                                cursorShape:    titleBar._canOpen ? Qt.PointingHandCursor : Qt.ArrowCursor

                                onClicked : {
                                    //console.log(modelData.setupSource)
                                    if (modelData.setupSource !== "") {
                                        setupView.showVehicleComponentPanel(modelData)
                                    }
                                }
                            }
                        }
                        // Summary Qml
                        Rectangle {
                            anchors.top:        titleBar.bottom
                            anchors.topMargin:  _cardMargins / 2
                            width:              parent.width
                            height:             _summaryBodyHeight + _cardMargins * 2
                            color:              "transparent"
                            border.width:       1
                            border.color:       qgcPal.groupBorder
                            radius:             ScreenTools.defaultFontPixelHeight / 2

                            Loader {
                                anchors.fill:       parent
                                anchors.margins:    _cardMargins
                                source:             modelData.summaryQmlSource

                                property var vehicleComponent: modelData

                                // RotorLab G97: grow every card to the tallest summary.
                                readonly property real bodyContentHeight: _bodyContentHeight(item)
                                function _growCards() {
                                    if (bodyContentHeight > _summaryRoot._summaryBodyHeight) {
                                        _summaryRoot._summaryBodyHeight = bodyContentHeight
                                    }
                                }
                                onBodyContentHeightChanged: _growCards()
                                Component.onCompleted:      _growCards()
                            }
                        }
                    }
                }
            }
        }
    }
}
