/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

import QtQuick
import QtCharts

import QGroundControl
import QGroundControl.ScreenTools
import QGroundControl.Controls
import QGroundControl.Palette

import "TerrainProfileLabels.js" as TerrainProfileLabels

Rectangle {
    id:         root
    radius:     ScreenTools.defaultFontPixelWidth * 0.5
    color:      qgcPal.window
    opacity:    0.80
    clip:       true

    property var missionController

    signal setCurrentSeqNum(int seqNum)

    property real _margins:                 ScreenTools.defaultFontPixelWidth / 2
    property var  _visualItems:             missionController.visualItems
    property real _altRange:                _maxAMSLAltitude - _minAMSLAltitude
    property real _indicatorSpacing:        5
    property real _minAMSLAltitude:         isNaN(terrainProfile.minAMSLAlt) ? 0 : terrainProfile.minAMSLAlt
    property real _maxAMSLAltitude:         isNaN(terrainProfile.maxAMSLAlt) ? 100 : terrainProfile.maxAMSLAlt
    // G76: the profile's own distance, which includes the RTL leg back to launch that the mission total leaves out
    property real _missionTotalDistance:    terrainProfile.profileDistance > 0 ? terrainProfile.profileDistance : (isNaN(missionController.missionTotalDistance) ? 100 : missionController.missionTotalDistance)
    property var  _unitsConversion:         QGroundControl.unitsConversion

    // RotorLab GC 5.1.2 (G78): one index label per item overlapped into a solid strip on long
    // plans. Labels are thinned so that shown labels keep a gap of 2.5 characters edge to edge,
    // each label counted at its real width (a 3-digit number is wider than its circle); the first
    // (launch, "L"), the last and the selected item always keep theirs. Slot 2 * index is an item's
    // label (or a complex item's entry label), slot 2 * index + 1 a complex item's exit label.
    // Tick lines of items without a shown label are faded so the terrain line stays visible.
    property real _labelMinSpacing:         ScreenTools.defaultFontPixelWidth * 2.5
    property real _labelCircleWidth:        Math.ceil(ScreenTools.defaultFontPixelHeight * ScreenTools.smallFontPointRatio)
    property real _unlabeledTickOpacity:    0.25
    property var  _labelShown:              []
    property real _labelPixelsPerMeter:     terrainProfile.pixelsPerMeter

    on_LabelPixelsPerMeterChanged:  _scheduleLabelUpdate()
    on_LabelMinSpacingChanged:      _scheduleLabelUpdate()

    function _scheduleLabelUpdate() {
        Qt.callLater(_updateLabelShown)     // once per event loop pass, however many items changed
    }

    // Width of an index label: its circle, or its text when the number is wider than the circle.
    function _labelWidth(text) {
        return Math.max(_labelCircleWidth, String(text).length * ScreenTools.defaultFontPixelWidth)
    }

    function _updateLabelShown() {
        var positions = []
        var forced = []
        var widths = []
        var ppm = terrainProfile.pixelsPerMeter
        for (var i = 0; i < _visualItems.count; i++) {
            var object = _visualItems.get(i)
            var entryX = NaN
            var exitX = NaN
            var entryText = ""
            var exitText = ""
            if (object.specifiesCoordinate && !object.standaloneCoordinate) {
                entryX = object.distanceFromStart * ppm
                if (object.isSimpleItem || object.isSingleItem) {
                    var letter = object.abbreviation.charAt(0)
                    entryText = letter > 'A' && letter < 'z' ? letter : object.sequenceNumber   // as the label below shows it
                } else {
                    entryText = object.sequenceNumber
                    exitX = (object.distanceFromStart + object.complexDistance) * ppm
                    exitText = object.lastSequenceNumber
                }
            }
            positions.push(entryX, exitX)
            forced.push(object.isCurrentItem, object.isCurrentItem)
            widths.push(_labelWidth(entryText), _labelWidth(exitText))
        }
        _labelShown = TerrainProfileLabels.thinLabels(positions, forced, _labelMinSpacing, widths)
    }

    QGCPalette { id: qgcPal }

    QGCLabel {
        id:                     titleLabel
        anchors.top:            parent.bottom
        width:                  parent.height
        font.pointSize:         ScreenTools.smallFontPointSize
        text:                   qsTr("Height AMSL (%1)").arg(_unitsConversion.appSettingsVerticalDistanceUnitsString)
        horizontalAlignment:    Text.AlignHCenter
        rotation:               -90
        transformOrigin:        Item.TopLeft
    }

    // RotorLab GC 5.2.0 (G80): unit of the horizontal (distance) axis, same unit setting as the axis values
    QGCLabel {
        id:                     distanceLabel
        anchors.bottom:         parent.bottom
        anchors.left:           terrainProfileFlickable.left
        anchors.right:          parent.right
        font.pointSize:         ScreenTools.smallFontPointSize
        text:                   qsTr("Distance (%1)").arg(_unitsConversion.appSettingsHorizontalDistanceUnitsString)
        elide:                  Text.ElideRight
        horizontalAlignment:    Text.AlignHCenter
    }

    QGCFlickable {
        id:                 terrainProfileFlickable
        anchors.top:        parent.top
        anchors.bottom:     distanceLabel.top
        anchors.leftMargin: titleLabel.contentHeight
        anchors.left:       parent.left
        anchors.right:      parent.right
        clip:               true

        Item {
            height: terrainProfileFlickable.height
            width:  terrainProfileFlickable.width

            ChartView {
                id:                 chart
                anchors.fill:       parent
                margins.top:        0
                margins.right:      0
                margins.bottom:     0
                margins.left:       0
                backgroundColor:    "transparent"
                legend.visible:     false
                antialiasing:       true

                ValueAxis {
                    id:                         axisX
                    min:                        0
                    max:                        _unitsConversion.metersToAppSettingsHorizontalDistanceUnits(_missionTotalDistance)
                    lineVisible:                true
                    labelsFont.family:          ScreenTools.fixedFontFamily
                    labelsFont.pointSize:       ScreenTools.smallFontPointSize
                    labelsColor:                qgcPal.text
                    tickCount:                  5
                    gridLineColor:              applyOpacity(qgcPal.text, 0.25)
                }

                ValueAxis {
                    id:                         axisY
                    min:                        _unitsConversion.metersToAppSettingsVerticalDistanceUnits(_minAMSLAltitude)
                    max:                        _unitsConversion.metersToAppSettingsVerticalDistanceUnits(_maxAMSLAltitude)
                    lineVisible:                true
                    labelsFont.family:          ScreenTools.fixedFontFamily
                    labelsFont.pointSize:       ScreenTools.smallFontPointSize
                    labelsColor:                qgcPal.text
                    tickCount:                  4
                    gridLineColor:              applyOpacity(qgcPal.text, 0.25)
                }

                LineSeries {
                    id:         lineSeries
                    axisX:      axisX
                    axisY:      axisY
                    visible:    true

                    XYPoint { x: 0; y: _unitsConversion.metersToAppSettingsVerticalDistanceUnits(_minAMSLAltitude) }
                    XYPoint { x: _unitsConversion.metersToAppSettingsHorizontalDistanceUnits(_missionTotalDistance); y: _unitsConversion.metersToAppSettingsVerticalDistanceUnits(_maxAMSLAltitude) }
                }
            }

            TerrainProfile {
                id:                 terrainProfile
                x:                  chart.plotArea.x
                y:                  chart.plotArea.y
                height:             chart.plotArea.height
                visibleWidth:       chart.plotArea.width
                missionController:  root.missionController

                Repeater {
                    model: missionController.visualItems

                    Item {
                        id:             topLevelItem
                        anchors.fill:   parent
                        visible:        object.specifiesCoordinate && !object.standaloneCoordinate

                        // G78: re-run label thinning when this item moves along the path or is selected
                        property real _labelDistance:   object.distanceFromStart + (complexItem.visible ? object.complexDistance : 0)
                        property int  _labelSlot:       index * 2   // the Repeater index; MissionItemIndexLabel has its own 'index'
                        property bool _labelSelected:   object.isCurrentItem
                        // An item with a shown label draws above the others, so their tick lines do not cover it
                        z:                              root._labelShown[_labelSlot] === true || root._labelShown[_labelSlot + 1] === true ? 1 : 0
                        on_LabelDistanceChanged:        root._scheduleLabelUpdate()
                        on_LabelSelectedChanged:        root._scheduleLabelUpdate()
                        onVisibleChanged:               root._scheduleLabelUpdate()
                        Component.onCompleted:          root._scheduleLabelUpdate()
                        Component.onDestruction:        root._scheduleLabelUpdate()

                        Rectangle {
                            id:         simpleItem
                            opacity:    root._labelShown[topLevelItem._labelSlot] === true ? 1 : root._unlabeledTickOpacity   // G78
                            height:     terrainProfile.height
                            width:      1
                            color:      qgcPal.text
                            x:          (object.distanceFromStart * terrainProfile.pixelsPerMeter)
                            visible:    object.isSimpleItem || object.isSingleItem

                            MissionItemIndexLabel {
                                anchors.horizontalCenter:   parent.horizontalCenter
                                anchors.bottom:             parent.bottom
                                small:                      true
                                checked:                    object.isCurrentItem
                                label:                      object.abbreviation.charAt(0)
                                visible:                    root._labelShown[topLevelItem._labelSlot] === true       // G78
                                index:                      object.abbreviation.charAt(0) > 'A' && object.abbreviation.charAt(0) < 'z' ? -1 : object.sequenceNumber
                                onClicked:                  root.setCurrentSeqNum(object.sequenceNumber)
                            }
                        }

                        Rectangle {
                            id:         complexItemEntry
                            opacity:    root._labelShown[topLevelItem._labelSlot] === true ? 1 : root._unlabeledTickOpacity   // G78
                            height:     terrainProfile.height
                            width:      1
                            color:      qgcPal.text
                            x:          (object.distanceFromStart * terrainProfile.pixelsPerMeter)
                            visible:    complexItem.visible

                            MissionItemIndexLabel {
                                anchors.horizontalCenter:   parent.horizontalCenter
                                anchors.bottom:             parent.bottom
                                small:                      true
                                checked:                    object.isCurrentItem
                                index:                      object.sequenceNumber
                                visible:                    root._labelShown[topLevelItem._labelSlot] === true       // G78
                                onClicked:                  root.setCurrentSeqNum(object.sequenceNumber)
                            }
                        }

                        Rectangle {
                            id:         complexItemExit
                            opacity:    root._labelShown[topLevelItem._labelSlot + 1] === true ? 1 : root._unlabeledTickOpacity   // G78
                            height:     terrainProfile.height
                            width:      1
                            color:      qgcPal.text
                            x:          ((object.distanceFromStart + object.complexDistance) * terrainProfile.pixelsPerMeter)
                            visible:    complexItem.visible

                            MissionItemIndexLabel {
                                anchors.horizontalCenter:   parent.horizontalCenter
                                anchors.bottom:             parent.bottom
                                small:                      true
                                checked:                    object.isCurrentItem
                                index:                      object.lastSequenceNumber
                                visible:                    root._labelShown[topLevelItem._labelSlot + 1] === true   // G78
                                onClicked:                  root.setCurrentSeqNum(object.sequenceNumber)
                            }
                        }

                        Rectangle {
                            id:             complexItem
                            anchors.bottom: parent.bottom
                            x:              (object.distanceFromStart * terrainProfile.pixelsPerMeter)
                            width:          complexItem.visible ? object.complexDistance * terrainProfile.pixelsPerMeter : 0
                            height:         patternNameLabel.height
                            color:          "green"
                            opacity:        0.5
                            visible:        !object.isSimpleItem && !object.isSingleItem

                            QGCMouseArea {
                                anchors.fill:   parent
                                onClicked:      root.setCurrentSeqNum(object.sequenceNumber)
                            }

                            QGCLabel {
                                id:                         patternNameLabel
                                anchors.horizontalCenter:   parent.horizontalCenter
                                text:                       complexItem.visible ? object.patternName : ""
                            }
                        }
                    }
                }
            }
        }
    }

    // RotorLab GC G76: the terrain state of the plan's legs, with the map's colors and cues. Guidance only: it
    // says what the loaded terrain data shows about the drawn lines, never that a flight may go.
    readonly property bool  _darkTheme:     qgcPal.globalTheme === QGCPalette.Dark
    readonly property color _casingColor:   Qt.rgba(0.08, 0.08, 0.10, 0.75)
    readonly property real  _swatchWidth:   ScreenTools.defaultFontPixelWidth * 3
    readonly property real  _swatchHeight:  Math.max(4, Math.round(ScreenTools.defaultFontPixelHeight * 0.3))

    // "1 leg", "2 legs". A state with no legs is hidden, so it has no entry and no accessible name.
    function _legCountText(count) {
        return count === 1 ? qsTr("1 leg") : qsTr("%1 legs").arg(count)
    }

    Rectangle {
        id:                 legStateBox
        anchors.top:        parent.top
        anchors.right:      parent.right
        anchors.margins:    _margins
        width:              legStateRow.width + (_margins * 2)
        height:             legStateRow.height + _margins
        radius:             _margins
        color:              qgcPal.window
        z:                  2
        visible:            terrainProfile.strikeLegCount + terrainProfile.unknownLegCount + terrainProfile.noStrikeLegCount > 0

        Row {
            id:                     legStateRow
            anchors.centerIn:       parent
            spacing:                ScreenTools.defaultFontPixelWidth

            // Strike: red line over a dark casing, with tick marks across it
            Row {
                spacing:                ScreenTools.defaultFontPixelWidth / 2
                visible:                terrainProfile.strikeLegCount > 0
                Accessible.role:        Accessible.StaticText
                Accessible.name:        qsTr("Meets the terrain or within 2 m of the ground: %1").arg(_legCountText(terrainProfile.strikeLegCount))

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width:                  _swatchWidth
                    height:                 _swatchHeight + 2
                    color:                  terrainProfile.terrainStateColorName(FlightPathSegment.TerrainStrike, _darkTheme)
                    border.color:           _casingColor
                    border.width:           1

                    Repeater {
                        model: 3
                        Rectangle {
                            x:      (index + 0.5) * parent.width / 3 - 1
                            y:      -2
                            width:  2
                            height: parent.height + 4
                            color:  parent.color
                        }
                    }
                }
                QGCLabel {
                    font.pointSize: ScreenTools.smallFontPointSize
                    text:           qsTr("Meets terrain or within 2 m: %1").arg(terrainProfile.strikeLegCount)
                }
            }

            // Unknown ground: violet dashes
            Row {
                spacing:                ScreenTools.defaultFontPixelWidth / 2
                visible:                terrainProfile.unknownLegCount > 0
                Accessible.role:        Accessible.StaticText
                Accessible.name:        qsTr("Unknown ground: terrain clearance not checked: %1").arg(_legCountText(terrainProfile.unknownLegCount))

                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing:                _swatchWidth / 9
                    Repeater {
                        model: 3
                        Rectangle {
                            width:          _swatchWidth / 4
                            height:         _swatchHeight
                            color:          terrainProfile.terrainStateColorName(FlightPathSegment.TerrainUnknown, _darkTheme)
                            border.color:   _casingColor
                            border.width:   1
                        }
                    }
                }
                QGCLabel {
                    font.pointSize: ScreenTools.smallFontPointSize
                    text:           qsTr("Unknown ground, not checked: %1").arg(terrainProfile.unknownLegCount)
                }
            }

            // No strike found: solid teal
            Row {
                spacing:                ScreenTools.defaultFontPixelWidth / 2
                visible:                terrainProfile.noStrikeLegCount > 0
                Accessible.role:        Accessible.StaticText
                Accessible.name:        qsTr("No strike found against the loaded terrain data: %1").arg(_legCountText(terrainProfile.noStrikeLegCount))

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width:                  _swatchWidth
                    height:                 _swatchHeight
                    color:                  terrainProfile.terrainStateColorName(FlightPathSegment.TerrainNoStrike, _darkTheme)
                    border.color:           _casingColor
                    border.width:           1
                }
                QGCLabel {
                    font.pointSize: ScreenTools.smallFontPointSize
                    text:           qsTr("No strike found: %1").arg(terrainProfile.noStrikeLegCount)
                }
            }
        }
    }

    function applyOpacity(colorIn, opacity){
        return Qt.rgba(colorIn.r, colorIn.g, colorIn.b, opacity)
    }
}
