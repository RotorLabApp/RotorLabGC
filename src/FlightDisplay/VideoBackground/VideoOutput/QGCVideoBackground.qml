/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

// G88: the Fly view's video item when GStreamer is off (src/FlightDisplay/CMakeLists.txt picks
// this file or GStreamer/QGCVideoBackground.qml). It is the VideoOutput itself, not a wrapper:
// VideoManager finds it by objectName ("videoContent", "thermalVideo") and the receiver
// decodes into its video sink only if it is a real VideoOutput (G83).

import QtQuick
import QtMultimedia

VideoOutput {
    id: videoBackground

    // FlightDisplayViewVideo sizes this item to the video's aspect ratio for the chosen fit
    // mode (fit width, fit height, fill, no crop), so the picture fills it. Fit keeps the
    // picture undistorted while that size catches up with a new stream.
    fillMode: VideoOutput.PreserveAspectFit
}
