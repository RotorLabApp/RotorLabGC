/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

import QtQuick
import QtLocation
import QtPositioning

import QGroundControl
import QGroundControl.Palette

/// The MissionLineView control is used to add lines between mission items.
///
/// RotorLab GC G76: each leg is drawn as the pieces FlightPathSegment.mapPieces gives, in the terrain state
/// colors of the 3D views (PLAN-CONTRACT.md section 4). The colors say what the loaded terrain data shows
/// about the line, nothing more:
///  - unknown ground (no terrain data, not yet checked, or no AMSL height): violet, dashed;
///  - a strike (less than 2 m above the ground, or below it): red, wider, with tick marks across the line;
///  - ground contact of a takeoff or landing (CL-1a): slate;
///  - no strike found: teal;
///  - no terrain query for this view (Fly view): the trajectory color, as before G76.
/// Every checked state is drawn over a thin dark casing so it keeps its contrast on light map areas (parks
/// on the road map) and on plain gray (no imagery). Dashes and ticks are the cues that do not rely on color.
MapItemView {
    property bool showSpecialVisual: false

    delegate: MapItemGroup {
        id: legGroup

        property var  _segment:     object
        property bool _darkTheme:   QGroundControl.globalPalette.globalTheme === QGCPalette.Dark
        // Note: Special visuals for ROI are hacked out for now since they are not working correctly
        property bool _showSpecialVisual: _segment && showSpecialVisual && _segment.specialVisual

        readonly property color _casingColor: Qt.rgba(0.08, 0.08, 0.10, 0.75)

        function _pieceColor(state) {
            var name = _segment ? _segment.terrainStateColorName(state, _darkTheme) : ""
            return name !== "" ? name : QGroundControl.globalPalette.mapMissionTrajectory
        }

        function _lineWidth(piece) {
            if (piece.tick) {
                return 3
            }
            return piece.state === FlightPathSegment.TerrainStrike ? 5 : 3
        }

        MapItemView {
            model: legGroup._segment ? legGroup._segment.mapPieces : []

            delegate: MapItemGroup {
                id: pieceGroup

                property var  _piece:   modelData
                property bool _cased:   _piece.state !== FlightPathSegment.TerrainNotChecked

                // Casing under the line: contrast on any background
                MapPolyline {
                    visible:    pieceGroup._cased
                    line.width: legGroup._lineWidth(pieceGroup._piece) + 2
                    line.color: legGroup._casingColor
                    z:          QGroundControl.zOrderWaypointLines
                    path:       pieceGroup._piece.path
                }

                MapPolyline {
                    line.width: legGroup._lineWidth(pieceGroup._piece)
                    line.color: legGroup._pieceColor(pieceGroup._piece.state)
                    z:          QGroundControl.zOrderWaypointLines + (pieceGroup._piece.tick ? 0.2 : 0.1)
                    path:       pieceGroup._piece.path
                }
            }
        }
    }
}
