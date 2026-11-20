/****************************************************************************
 *
 * RotorLab GC Plan view 3D (G64, MG2) - the legend: what the colors mean
 * (PLAN-CONTRACT.md section 4, CL-5 wording), the terrain source (CL-6), the
 * spline note (S-5), the terrain-frame note (A-6) and drawing warnings.
 *
 ****************************************************************************/

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls
import QGroundControl.Palette
import QGroundControl.ScreenTools

Rectangle {
    id:                 legend

    property var    sceneModel: null
    /// Imagery under the ground (block (b), Plan3DGroundLayer.imagerySource);
    /// set by the view. Empty: no imagery line.
    property string imagerySource:  ""
    property bool   imageryLoading: false
    property bool   expanded:   true
    /// RotorLab 5.3.0 review (accessibility M1): the tallest the legend may be,
    /// set by the view to the space above the edit panel and message box.
    /// Zero or less: no cap.
    property real   maxHeight:  0

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    readonly property real _pad:            ScreenTools.defaultFontPixelHeight / 2
    readonly property real _gap:            ScreenTools.defaultFontPixelHeight / 4
    readonly property real _barWidth:       ScreenTools.defaultFontPixelWidth * 0.75
    readonly property real _naturalHeight:  fixedColumn.implicitHeight + _gap + scrollColumn.implicitHeight + _pad * 2
    // The scroll area keeps at least three lines, so a very small cap never
    // hides it altogether.
    readonly property real _minHeight:      fixedColumn.implicitHeight + _gap + Math.min(scrollColumn.implicitHeight, ScreenTools.defaultFontPixelHeight * 3) + _pad * 2
    // Accessibility R1: when even that does not fit under the cap (large
    // fonts with a point selected), the legend keeps to the cap and the whole
    // of it scrolls as one, the counts first, instead of running down behind
    // the edit panel.
    readonly property bool _squeezed:       maxHeight > 0 && _minHeight > maxHeight
    /// The Flickable that scrolls at present (the autorun scrolls it).
    readonly property var  scrollArea:      _squeezed ? outerFlick : legendFlick
    on_SqueezedChanged: outerFlick.contentY = 0

    height:             maxHeight <= 0 ? _naturalHeight
                        : _squeezed ? Math.max(maxHeight, _pad * 2 + ScreenTools.defaultFontPixelHeight * 3)
                        : Math.min(_naturalHeight, maxHeight)
    radius:             ScreenTools.defaultFontPixelHeight / 3
    color:              Qt.rgba(qgcPal.window.r, qgcPal.window.g, qgcPal.window.b, 0.9)

    // Holds both parts. It scrolls only when the legend is squeezed; then the
    // inner scroll area shows all its lines and does not scroll itself.
    QGCFlickable {
        id:                 outerFlick
        anchors.fill:       parent
        anchors.margins:    legend._pad
        anchors.rightMargin: legend._pad / 2
        contentWidth:       width
        contentHeight:      legend._squeezed ? fixedColumn.implicitHeight + legend._gap + scrollColumn.implicitHeight : height
        interactive:        legend._squeezed
        clip:               true
        indicatorColor:     "transparent"

        ScrollBar.vertical: ScrollBar {
            policy:         legend._squeezed ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
            // Accessibility R2: a thumb long enough to see and grab.
            minimumSize:    0.2
            contentItem: Rectangle {
                implicitWidth:  legend._barWidth
                radius:         width / 2
                color:          qgcPal.text
                opacity:        0.5
            }
        }

        // Always shown, never scrolled away unless the legend is squeezed: the
        // title, the counts, the obstacles note and the strike places.
        ColumnLayout {
            id:                 fixedColumn
            x:                  0
            y:                  0
            // Room for the scroll bar is kept, as in the scroll area below, so
            // the text does not rewrap when the legend is squeezed.
            width:              outerFlick.width - legend._barWidth - legend._pad / 2
            spacing:            legend._gap

            RowLayout {
                Layout.fillWidth: true
                QGCLabel { text: qsTr("3D view"); font.bold: true; Layout.fillWidth: true }
                QGCButton { text: legend.expanded ? qsTr("Less") : qsTr("More"); onClicked: legend.expanded = !legend.expanded }
            }

            // G67 accessibility M1: the counts are always shown, at the default
            // size, also after "Less".
            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WordWrap
                text: legend.expanded
                      ? qsTr("Samples that meet the terrain or are within 2 m of the ground: %1. Ground contact samples at takeoff or landing (not counted as meeting the terrain): %2. Samples not checked (unknown ground or height): %3.")
                            .arg(sceneModel.samplesStrike).arg(sceneModel.samplesGroundContact).arg(sceneModel.samplesUnknown)
                      : qsTr("Samples that meet the terrain or are within 2 m of the ground: %1. Samples not checked: %2.")
                            .arg(sceneModel.samplesStrike).arg(sceneModel.samplesUnknown)
            }
            // RotorLab 5.3.0 review (M2): shown also after "Less", at
            // the default size, wherever the colors are shown.
            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WordWrap
                text:               qsTr("Obstacles, buildings and wires are not checked.")
            }
            // G67 visual M2, accessibility M2: where, in words, never by red alone.
            QGCLabel {
                Layout.fillWidth:   true
                visible:            sceneModel.strikePlaces.length > 0
                wrapMode:           Text.WordWrap
                font.bold:          true
                text: {
                    var p = sceneModel.strikePlaces
                    var shown = p.slice(0, 8).join(", ")
                    if (p.length > 8) {
                        shown = qsTr("%1, and %2 more").arg(shown).arg(p.length - 8)
                    }
                    return qsTr("Meets the terrain or within 2 m of the ground at: %1. Red is drawn over the ground, so a part below the ground stays visible.").arg(shown)
                }
            }
        }

        // The rest scrolls (accessibility M1), with a scroll bar shown whenever
        // there is more text than room, so no line is cut without a sign.
        QGCFlickable {
            id:                 legendFlick
            x:                  0
            y:                  fixedColumn.implicitHeight + legend._gap
            width:              outerFlick.width
            height:             legend._squeezed ? scrollColumn.implicitHeight
                                                 : Math.max(0, outerFlick.height - y)
            contentWidth:       width
            contentHeight:      scrollColumn.implicitHeight
            interactive:        !legend._squeezed
            clip:               true
            // The fade-in indicators give way to the scroll bar below (as G97).
            indicatorColor:     "transparent"

            ScrollBar.vertical: ScrollBar {
                policy: !legend._squeezed && legendFlick.contentHeight > legendFlick.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
                // Accessibility R2: a thumb long enough to see and grab.
                minimumSize: 0.2
                contentItem: Rectangle {
                    implicitWidth:  legend._barWidth
                    radius:         width / 2
                    color:          qgcPal.text
                    opacity:        0.5
                }
            }

            ColumnLayout {
                id:                 scrollColumn
                // Room for the scroll bar is kept whether or not it is shown, so
                // the text does not rewrap when it appears.
                width:              legendFlick.width - legend._barWidth - legend._pad / 2
                spacing:            legend._gap

                Repeater {
                    model: [
                        { state: "noStrike", text: qsTr("2 m or more above the loaded terrain") },
                        { state: "strike",   text: qsTr("Meets the terrain, or within 2 m of the ground") },
                        { state: "groundContact", text: qsTr("Ground contact (takeoff or landing)") },
                        { state: "unknown",  text: qsTr("Unknown ground or height: not checked") }
                    ]
                    delegate: RowLayout {
                        Layout.fillWidth: true
                        Rectangle { width: ScreenTools.defaultFontPixelWidth * 3; height: ScreenTools.defaultFontPixelHeight / 3; color: (sceneModel.darkTheme, sceneModel.stateColor(modelData.state)) }
                        QGCLabel { text: modelData.text; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                    }
                }

                // G67 visual M3: the height stems' own color and line.
                RowLayout {
                    Layout.fillWidth: true
                    Item {
                        width:  ScreenTools.defaultFontPixelWidth * 3
                        height: ScreenTools.defaultFontPixelHeight / 3
                        Rectangle { anchors.centerIn: parent; width: parent.width; height: Math.max(1, parent.height / 3); color: (sceneModel.darkTheme, sceneModel.stateColor("stem")) }
                    }
                    QGCLabel { text: qsTr("Thin line: drop line from a point to the loaded ground"); Layout.fillWidth: true; wrapMode: Text.WordWrap }
                }

                // G67 accessibility M3: fence colors, when the plan has fences.
                Repeater {
                    model: sceneModel.hasFences ? [
                        { state: "fenceInclusion", text: qsTr("Fence, inclusion (not checked against the terrain)") },
                        { state: "fenceExclusion", text: qsTr("Fence, exclusion (not checked against the terrain)") }
                    ] : []
                    delegate: RowLayout {
                        Layout.fillWidth: true
                        Rectangle { width: ScreenTools.defaultFontPixelWidth * 3; height: ScreenTools.defaultFontPixelHeight / 3; color: (sceneModel.darkTheme, sceneModel.stateColor(modelData.state)) }
                        QGCLabel { text: modelData.text; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                    }
                }

                QGCLabel {
                    Layout.fillWidth:   true
                    wrapMode:           Text.WordWrap
                    font.pointSize:     ScreenTools.smallFontPointSize
                    text: qsTr("Terrain: %1, through GC's terrain query (%2% of the area loaded%3).")
                              .arg(sceneModel.terrainSource)
                              .arg(sceneModel.groundKnownPercent)
                              .arg(sceneModel.groundLoading ? qsTr(", loading") : "")
                }
                QGCLabel {
                    Layout.fillWidth:   true
                    visible:            legend.imagerySource !== ""
                    wrapMode:           Text.WordWrap
                    font.pointSize:     ScreenTools.smallFontPointSize
                    text: qsTr("Imagery: %1%2.").arg(legend.imagerySource).arg(legend.imageryLoading ? qsTr(", loading") : "")
                }
                QGCLabel {
                    Layout.fillWidth:   true
                    visible:            legend.expanded
                    wrapMode:           Text.WordWrap
                    font.pointSize:     ScreenTools.smallFontPointSize
                    text: qsTr("Colors show what the loaded terrain data says about the drawn line, and nothing more. Heights are placed by each item's frame and altitude parameter.")
                }
                QGCLabel {
                    Layout.fillWidth:   true
                    visible:            legend.expanded && sceneModel.hasSpline
                    wrapMode:           Text.WordWrap
                    font.pointSize:     ScreenTools.smallFontPointSize
                    text: qsTr("Spline legs are drawn approximately; the autopilot computes its own curve. The terrain check on a spline leg uses the drawn approximation.")
                }
                QGCLabel {
                    Layout.fillWidth:   true
                    visible:            legend.expanded && sceneModel.hasTerrainFrame
                    wrapMode:           Text.WordWrap
                    font.pointSize:     ScreenTools.smallFontPointSize
                    text: qsTr("Terrain-frame heights are shown by this product's terrain; the autopilot flies them above its own terrain data.")
                }
                QGCLabel {
                    Layout.fillWidth:   true
                    visible:            legend.expanded && sceneModel.warnings.length > 0
                    wrapMode:           Text.WordWrap
                    font.pointSize:     ScreenTools.smallFontPointSize
                    text:               sceneModel.warnings.join("\n")
                }
            }
        }
    }
}
