/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

// The Fly view's video item when GStreamer is built (src/FlightDisplay/CMakeLists.txt picks
// this file or VideoOutput/QGCVideoBackground.qml).

import QtQuick
import org.freedesktop.gstreamer.Qt6GLVideoItem

GstGLQt6VideoItem {
    id: videoBackground
}
