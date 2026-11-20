/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

/****************************************************************************
 *
 * RotorLab GC custom settings page list. Modified by RotorLab, Inc., 2026,
 * from src/UI/AppSettings/SettingsPagesModel.qml. Overrides the stock
 * SettingsPagesModel via the CustomOverrideInterceptor and adds the
 * TAK / ATAK page. All other entries mirror the stock model.
 *
 ****************************************************************************/

import QtQml.Models

import QGroundControl
import QGroundControl.ScreenTools

ListModel {
    ListElement {
        name: qsTr("General")
        url: "qrc:/qml/QGroundControl/AppSettings/GeneralSettings.qml"
        iconUrl: "qrc:/res/QGCLogoWhite.svg"
        pageVisible: function() { return true }
    }

    ListElement {
        name: qsTr("Fly View")
        url: "qrc:/qml/QGroundControl/AppSettings/FlyViewSettings.qml"
        iconUrl: "qrc:/qmlimages/PaperPlane.svg"
        pageVisible: function() { return true }
    }

    ListElement {
        name: qsTr("Plan View")
        url: "qrc:/qml/QGroundControl/AppSettings/PlanViewSettings.qml"
        iconUrl: "qrc:/qmlimages/Plan.svg"
        pageVisible: function() { return true }
    }

    ListElement {
        name: qsTr("Video")
        url: "qrc:/qml/QGroundControl/AppSettings/VideoSettings.qml"
        iconUrl: "qrc:/InstrumentValueIcons/camera.svg"
        pageVisible: function() { return QGroundControl.settingsManager.videoSettings.visible }
    }

    ListElement {
        // G95/G109: GC's own recording and Fleet live relay. Always listed; on a
        // build without GC's own video receiver the page says why it is empty.
        name: qsTr("Video recording and relay")
        url: "qrc:/Custom/qml/QGroundControl/AppSettings/VideoSettingsPage.qml"
        iconUrl: "qrc:/InstrumentValueIcons/camera.svg"
        pageVisible: function() { return true }
    }

    ListElement {
        name: qsTr("Telemetry")
        url: "qrc:/qml/QGroundControl/AppSettings/TelemetrySettings.qml"
        iconUrl: "qrc:/InstrumentValueIcons/drone.svg"
        pageVisible: function() { return true }
    }

    ListElement {
        name: qsTr("ADSB Server")
        url: "qrc:/qml/QGroundControl/AppSettings/ADSBServerSettings.qml"
        iconUrl: "qrc:/InstrumentValueIcons/airplane.svg"
        pageVisible: function() { return true }
    }

    ListElement {
        name: qsTr("TAK / ATAK")
        url: "qrc:/Custom/qml/QGroundControl/AppSettings/TAKSettings.qml"
        iconUrl: "qrc:/custom/img/rotorlab-original.svg"
        // Hidden while a Fleet Server is ACTIVELY this station's route: it is
        // then the way to ATAK, and two routes to the same TAK server put every
        // aircraft on the map twice and let one marker be carried out twice.
        //
        // Suspended does not count. When the RotorLab Connector takes over, the
        // direct link starts publishing again - RotorLab.app has no ATAK, so
        // suppressing it would leave the crew with no map - and a link that is
        // live must have a page the crew can reach to configure it.
        //
        // The settings are kept, not cleared, so this page and the crew's own
        // TAK server come straight back.
        pageVisible: function() { return !(FleetConnector.enrolled && !FleetConnector.suspended) }
    }

    ListElement {
        name: qsTr("Fleet")
        url: "qrc:/Custom/qml/QGroundControl/AppSettings/FleetSettings.qml"
        iconUrl: "qrc:/InstrumentValueIcons/drone.svg"
        pageVisible: function() { return true }
    }

    ListElement {
        name: qsTr("RotorLab Connector")
        url: "qrc:/Custom/qml/QGroundControl/AppSettings/RotorLabConnectorSettings.qml"
        iconUrl: "qrc:/InstrumentValueIcons/cloud-upload.svg"
        // The mirror of the row below: whichever connector is NOT this
        // station's route is hidden, so there is only ever one place to look.
        //
        // This row comes BEFORE "Fleet Connector" (G59). Turning the
        // RotorLab Connector on hides the Fleet Connector row; with that row
        // below this one, this row and every row above it keep their place,
        // so the row the operator just used does not jump under the pointer.
        //
        // The way in from the other side is the "File directly to RotorLab"
        // button on the Fleet Connector page - without it, an enrolled station
        // could never reach this page to switch, because the row it needs is
        // the one being hidden.
        //
        // The `|| enabled` is a backstop, not decoration. If the two conditions
        // ever disagree - as they did when the hand-over was applied on the
        // toggle but not at startup - this row stays reachable rather than both
        // connectors hiding each other and stranding the operator.
        pageVisible: function() {
            return RotorLabConnector.enabled ||
                   !(FleetConnector.enrolled && !FleetConnector.suspended)
        }
    }

    ListElement {
        name: qsTr("Fleet Connector")
        url: "qrc:/Custom/qml/QGroundControl/AppSettings/FleetConnectorSettings.qml"
        iconUrl: "qrc:/InstrumentValueIcons/cloud-upload.svg"
        // Exactly one connector is active at a time. Hidden - not disabled -
        // while the RotorLab Connector is on: a greyed-out row invites the
        // customer to click it and ask why it does not work, and a row that is
        // not there is not a question.
        pageVisible: function() { return !RotorLabConnector.enabled }
    }

    ListElement {
        name: qsTr("License")
        url: "qrc:/Custom/qml/QGroundControl/AppSettings/LicenseSettings.qml"
        iconUrl: "qrc:/InstrumentValueIcons/lock-closed.svg"
        pageVisible: function() { return true }
    }

    ListElement {
        name: qsTr("Comm Links")
        url: "qrc:/qml/QGroundControl/AppSettings/LinkSettings.qml"
        iconUrl: "qrc:/InstrumentValueIcons/usb.svg"
        pageVisible: function() { return true }
    }

    ListElement {
        name: qsTr("Maps")
        url: "qrc:/qml/QGroundControl/AppSettings/MapSettings.qml"
        iconUrl: "qrc:/InstrumentValueIcons/globe.svg"
        pageVisible: function() { return true }
    }

    ListElement {
        name: qsTr("PX4 Log Transfer")
        url: "qrc:/qml/QGroundControl/AppSettings/PX4LogTransferSettings.qml"
        iconUrl: "qrc:/InstrumentValueIcons/inbox-download.svg"
        pageVisible: function() {
            var activeVehicle = QGroundControl.multiVehicleManager.activeVehicle
            return QGroundControl.corePlugin.options.showPX4LogTransferOptions &&
                        QGroundControl.px4ProFirmwareSupported &&
                        (activeVehicle ? activeVehicle.px4Firmware : true)
        }
    }

    ListElement {
        name: qsTr("Remote ID")
        url: "qrc:/qml/QGroundControl/AppSettings/RemoteIDSettings.qml"
        iconUrl: "qrc:/qmlimages/RidIconManNoID.svg"
        pageVisible: function() { return true }
    }

    ListElement {
        name: qsTr("Console")
        url: "qrc:/qml/QGroundControl/Controls/AppMessages.qml"
        iconUrl: "qrc:/InstrumentValueIcons/conversation.svg"
        pageVisible: function() { return true }
    }

    ListElement {
        name: qsTr("Help")
        url: "qrc:/qml/QGroundControl/AppSettings/HelpSettings.qml"
        iconUrl: "qrc:/InstrumentValueIcons/question.svg"
        pageVisible: function() { return true }
    }

    ListElement {
        name: qsTr("Mock Link")
        url: "qrc:/qml/QGroundControl/AppSettings/MockLink.qml"
        iconUrl: "qrc:/InstrumentValueIcons/drone.svg"
        pageVisible: function() { return ScreenTools.isDebug }
    }

    ListElement {
        name: qsTr("Debug")
        url: "qrc:/qml/QGroundControl/AppSettings/DebugWindow.qml"
        iconUrl: "qrc:/InstrumentValueIcons/bug.svg"
        pageVisible: function() { return ScreenTools.isDebug }
    }

    ListElement {
        name: qsTr("Palette Test")
        url: "qrc:/qml/QGroundControl/AppSettings/QmlTest.qml"
        iconUrl: "qrc:/InstrumentValueIcons/photo.svg"
        pageVisible: function() { return ScreenTools.isDebug }
    }
}
