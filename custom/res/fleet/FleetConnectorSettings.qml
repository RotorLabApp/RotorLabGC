/****************************************************************************
 *
 * RotorLab GC Fleet Connector settings page.
 *
 * Connects this ground station to the RotorLab unified server. Everything on
 * screen is a function of the connector's state; nothing here is ever allowed
 * to block, delay or complicate flying, in any state including the two red
 * ones.
 *
 ****************************************************************************/

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls
import QGroundControl.Palette
import QGroundControl.ScreenTools

Rectangle {
    id:             root
    color:          qgcPal.window
    anchors.fill:   parent

    property real _pad:           ScreenTools.defaultFontPixelWidth
    readonly property bool _narrow: page.narrow   // field under its label
    property real _keyWidth:      ScreenTools.defaultFontPixelWidth * 22   // left label column

    // Enrollment form state, kept so a refusal does not clear what was typed.
    property string _address:  FleetConnector.baseUrl.length > 0 ? FleetConnector.baseUrl : "https://"
    property string _code:     ""
    property string _name:     ""

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    // Note and status colors come from the shared page layout (A156).
    readonly property color _noteColor:  page.noteColor
    readonly property color _warnColor:  page.warnColor
    readonly property color _errorColor: page.errorColor
    readonly property color _okColor:    page.okColor

    /// Open a checklist on demand, from the rows below. Same runner and the
    /// same filing path as the offer card uses.
    function runChecklist(kind) {
        manualRunner.active = true
        if (manualRunner.item) {
            manualRunner.item.kind = kind
            manualRunner.item.open()
        }
    }

    Loader {
        id:     manualRunner
        active: false
        source: "qrc:/custom/fleet/ChecklistRunner.qml"
    }

    // FleetConnector.State
    readonly property int _stNotConnected: 0
    readonly property int _stEnrolling:    1
    readonly property int _stFirstSync:    2
    readonly property int _stConnected:    3
    readonly property int _stUnreachable:  4
    readonly property int _stRevoked:      5

    function _stateColor() {
        switch (FleetConnector.state) {
        case _stConnected:   return _okColor
        case _stUnreachable: return _warnColor
        case _stRevoked:     return _errorColor
        default:             return qgcPal.text
        }
    }

    Component.onCompleted: {
        if (_name.length === 0) {
            // A machine name is fine here; the admin uses it to tell two
            // identical laptops apart.
            _name = FleetConnector.stationName.length > 0
                  ? FleetConnector.stationName
                  : FleetConnector.machineName
        }
    }

    component Card: ColumnLayout {
        property alias title: _h.text
        default property alias content: _body.data
        Layout.fillWidth:   true
        spacing:            ScreenTools.defaultFontPixelHeight / 3
        QGCLabel {
            id:                 _h
            Layout.fillWidth:   true
            wrapMode:           Text.WordWrap
            font.bold:          true
            font.pointSize:     ScreenTools.defaultFontPointSize + 1
        }
        Rectangle {
            Layout.fillWidth:   true
            color:              qgcPal.windowShade
            border.color:       qgcPal.groupBorder
            border.width:       1
            radius:             ScreenTools.defaultFontPixelHeight / 2
            implicitHeight:     _body.implicitHeight + _pad * 2
            ColumnLayout {
                id:         _body
                x:          _pad
                y:          _pad
                width:      parent.width - _pad * 2
                spacing:    ScreenTools.defaultFontPixelHeight / 2
            }
        }
    }

    // Labelled rows are placed by the shared rule in RLPageLayout: the label
    // column is 40% of the row up to about 40 character widths and wraps, the
    // field follows right after it up to 48 character widths, and the rest of
    // the row stays empty. On a narrow page the field goes under its label.
    // The field's size is a maximum, so it always shrinks with the window.
    // Entries use the theme's input colors with a blue focus outline.
    component Field: GridLayout {
        id: _row
        property alias label: _l.text
        property alias text:  _tf.text
        property alias placeholder: _tf.placeholderText
        // A code field keeps exactly what is typed: no case change, no
        // characters removed. Only whitespace around a paste is dropped, and
        // the keyboard is told not to capitalize or correct it.
        property bool  exact: false
        signal edited(string value)
        Layout.fillWidth: true
        columns:          _narrow ? 1 : 3
        columnSpacing:    _pad * 2
        rowSpacing:       _pad / 2
        QGCLabel {
            id:                     _l
            Layout.alignment:       Qt.AlignVCenter
            Layout.fillWidth:       _narrow
            Layout.preferredWidth:  _narrow ? -1 : page.labelColumnWidth(_row.width)
            wrapMode:               Text.WordWrap
        }
        TextField {
            id:                     _tf
            Layout.fillWidth:       true
            Layout.minimumWidth:    ScreenTools.defaultFontPixelWidth * 8
            Layout.maximumWidth:    page.fieldMaxWidthFor(_row.width, _row.columnSpacing * 2)
            Layout.preferredWidth:  Layout.maximumWidth
            Layout.preferredHeight: ScreenTools.defaultFontPixelHeight * 2
            color:                  page.inputTextColor
            placeholderTextColor:   page.placeholderColor
            selectByMouse:          true
            selectionColor:         page.focusColor
            selectedTextColor:      "#0F1722"
            leftPadding:            ScreenTools.defaultFontPixelWidth
            inputMethodHints:       _row.exact
                                    ? (Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText | Qt.ImhPreferLatin)
                                    : Qt.ImhNone
            onTextChanged: {
                if (_row.exact) {
                    var t = text.trim()
                    if (t !== text) { text = t; return }
                }
                _row.edited(text)
            }
            background: Rectangle {
                color:        page.inputColor
                radius:       4
                border.width: _tf.activeFocus ? 2 : 1
                border.color: _tf.activeFocus ? page.focusColor : page.inputBorderColor
            }
            cursorDelegate: Rectangle { width: 2; color: page.focusColor; visible: _tf.cursorVisible }
        }
        Item { Layout.fillWidth: true; visible: !_narrow }
    }

    component Row2: RowLayout {
        property alias label: _k.text
        property alias value: _v.text
        property color valueColor: qgcPal.text
        Layout.fillWidth: true
        QGCLabel { id: _k; Layout.preferredWidth: _keyWidth; wrapMode: Text.WordWrap; color: _noteColor }
        QGCLabel { id: _v; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: parent.valueColor }
    }

    // The shared page layout (A156): full width minus the standard settings
    // padding, scrolls vertically, and says when the page is narrow.
    RLPageLayout {
        id:             page
        anchors.fill:   parent

        ColumnLayout {
            id:                 outer
            Layout.fillWidth:   true
            spacing:            ScreenTools.defaultFontPixelHeight

            // ---------------------------------------------- NOT CONNECTED

            Card {
                title:   qsTr("Connect to a RotorLab Server (Fleet Server role)")
                // Keyed on not being enrolled rather than on a pair of states.
                // Every other card here needs a token to mean anything, so if
                // this one is hidden while there is no token the page is blank
                // and the operator has no way back in. Belt and braces after
                // exactly that happened: a reply still in flight when the
                // operator disconnected pushed the state to UNREACHABLE, which
                // no card claimed.
                visible: !FleetConnector.enrolled || FleetConnector.state === _stEnrolling

                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Connects this ground station to a RotorLab Server, so aircraft telemetry, " +
                               "flight details, checklists, and logs can be reported. Your operator gives you the " +
                               "address and a code.")
                }

                Field {
                    label: qsTr("RotorLab Server address")
                    text:  _address
                    placeholder: "https://fleet-server:5450"
                    onEdited: function(v) { _address = v }
                }
                Field {
                    label: qsTr("Enrollment code")
                    text:  _code
                    placeholder: qsTr("16 characters, or code.pin")
                    exact: true
                    onEdited: function(v) { _code = v }
                }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color:   _noteColor
                    text:    qsTr("Type or paste the code exactly as the server console shows it. Upper and " +
                                  "lower case letters are different, and - and _ are part of the code. If the " +
                                  "console shows the code with a pin after a dot, or a rotorlab-server:// link, " +
                                  "paste all of it. Codes work once and expire after 15 minutes.")
                }
                Field {
                    label: qsTr("This ground station")
                    text:  _name
                    placeholder: qsTr("north-laptop")
                    onEdited: function(v) { _name = v }
                }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible: FleetConnector.lastError.length > 0
                    color:   _errorColor
                    text:    FleetConnector.lastError
                }

                RowLayout {
                    Layout.fillWidth: true
                    QGCButton {
                        text:    qsTr("Enroll")
                        primary: true
                        enabled: !FleetConnector.busy && FleetConnector.codeReady(_code) && _name.trim().length > 0
                        onClicked: FleetConnector.enroll(_address, _code, _name)
                    }
                    BusyIndicator {
                        running: FleetConnector.busy
                        visible: FleetConnector.busy
                        implicitWidth:  ScreenTools.defaultFontPixelHeight * 1.5
                        implicitHeight: ScreenTools.defaultFontPixelHeight * 1.5
                    }
                }
            }

            // ---------------------------------------------- FIRST SYNC

            Card {
                title:   qsTr("Setting up")
                visible: FleetConnector.state === _stFirstSync
                RowLayout {
                    Layout.fillWidth: true
                    BusyIndicator {
                        running: true
                        implicitWidth:  ScreenTools.defaultFontPixelHeight * 1.5
                        implicitHeight: ScreenTools.defaultFontPixelHeight * 1.5
                    }
                    QGCLabel {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("Fetching what this server needs…")
                    }
                }
            }

            // ---------------------------------------------- REVOKED

            Card {
                title:   qsTr("This ground station was removed")
                visible: FleetConnector.state === _stRevoked
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: _errorColor
                    text: qsTr("The RotorLab Server (Fleet Server role) no longer recognizes this station, so reporting has stopped. " +
                               "An administrator removed it. Your aircraft are unaffected and can be flown normally.")
                }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: _noteColor
                    text: qsTr("To reconnect, ask your operator for a new enrollment code, then disconnect and enroll again.")
                }
                QGCButton { text: qsTr("Disconnect"); onClicked: root.confirmDisconnect() }
            }

            // ---------------------------------------------- PANEL 1: connection

            Card {
                title:   qsTr("Connection")
                visible: FleetConnector.enrolled && FleetConnector.state !== _stEnrolling

                RowLayout {
                    Layout.fillWidth: true
                    QGCLabel { Layout.preferredWidth: _keyWidth; wrapMode: Text.WordWrap; text: qsTr("Status"); color: _noteColor }
                    QGCLabel {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: FleetConnector.stateText
                        color: _stateColor()
                        font.bold: true
                    }
                }
                Row2 { label: qsTr("RotorLab Server"); value: FleetConnector.baseUrl }
                Row2 { label: qsTr("Last report"); value: FleetConnector.lastReportText }
                Row2 { label: qsTr("Certificate"); value: FleetConnector.certInfo }

                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible: FleetConnector.statusText.length > 0
                    color: _warnColor
                    text: FleetConnector.statusText
                }

                RowLayout {
                    Layout.fillWidth: true
                    QGCButton { text: qsTr("Refresh now"); onClicked: FleetConnector.refreshNow() }
                    QGCButton { text: qsTr("Disconnect"); onClicked: root.confirmDisconnect() }
                }
            }

            // ---------------------------------------------- PANEL 2: this crew

            Card {
                title:   qsTr("This crew")
                visible: FleetConnector.enrolled && FleetConnector.state !== _stEnrolling

                Row2 { label: qsTr("Station name"); value: FleetConnector.stationName }
            }

            // ---------------------------------------- PANEL 3: who is flying

            Card {
                title:   qsTr("Who is flying")
                visible: FleetConnector.enrolled && FleetConnector.state !== _stEnrolling

                RowLayout {
                    Layout.fillWidth: true
                    QGCLabel { Layout.preferredWidth: _keyWidth; wrapMode: Text.WordWrap; text: qsTr("Pilot"); color: _noteColor }
                    QGCLabel {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text:  FleetConnector.pilotId >= 0 ? FleetConnector.pilotName : qsTr("not set")
                        color: FleetConnector.pilotId >= 0 ? qgcPal.text : _warnColor
                    }
                }
                Row2 { label: qsTr("Site");        value: FleetConnector.site.length        ? FleetConnector.site        : qsTr("(not set)") }
                Row2 { label: qsTr("Flight type"); value: FleetConnector.flightType.length  ? FleetConnector.flightType  : qsTr("(not set)") }
                Row2 { label: qsTr("Crew");        value: FleetConnector.crew.length        ? FleetConnector.crew        : qsTr("(not set)") }

                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: _noteColor
                    visible: FleetConnector.pilotId < 0
                    text: qsTr("Flights are still logged and still filed without a pilot. They are held " +
                               "for assignment rather than attributed to a guess. It can be set at any time.")
                }
                QGCButton {
                    text: qsTr("Change pilot")
                    onClicked: FleetConnector.openCrewScreen()
                }
            }

            // ---------------------------------------- PANEL 4: aircraft

            Card {
                title:   qsTr("Aircraft")
                visible: FleetConnector.enrolled && FleetConnector.state !== _stEnrolling

                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: _noteColor
                    text: qsTr("Vehicle details are managed in the Fleet screen.")
                }

                Repeater {
                    model: TAKManager.fleetModel
                    delegate: RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        QGCLabel {
                            Layout.preferredWidth: ScreenTools.defaultFontPixelWidth * 8
                            wrapMode: Text.WordWrap
                            text: qsTr("sysid ") + modelData.sysid
                            color: modelData.connected ? qgcPal.text : _noteColor
                        }
                        QGCLabel {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: modelData.callsign.length > 0 ? modelData.callsign : qsTr("(no callsign)")
                            color: modelData.callsign.length > 0 ? qgcPal.text : _noteColor
                        }
                    }
                }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible: TAKManager.fleetModel.length === 0
                    color:   _noteColor
                    text:    qsTr("No aircraft yet. They appear here as they connect.")
                }
            }

            // ---------------------------------------- checklists

            Card {
                title:   qsTr("Checklists")
                // Nothing at all when the server has no templates: no button,
                // no empty list, and certainly not one nobody wrote.
                visible: FleetConnector.enrolled &&
                         (FleetConnector.preflightTemplates.length > 0 ||
                          FleetConnector.postflightTemplates.length > 0 ||
                          FleetConnector.outboxCount > 0)

                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: _noteColor
                    text: qsTr("Offered before the first arm and after landing. If that moment has " +
                               "passed, or was dismissed, one can be run and filed from here at any time.")
                }

                // A dismissed offer must not be the only route to a checklist.
                // A crew that tapped past the card should still be able to
                // record what they checked, whenever there is a spare hand.
                RowLayout {
                    Layout.fillWidth: true
                    QGCLabel {
                        Layout.preferredWidth: _keyWidth
                        wrapMode: Text.WordWrap
                        text:  qsTr("Pre-flight")
                        color: _noteColor
                    }
                    QGCLabel {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: FleetConnector.preflightTemplates.length > 0
                              ? FleetConnector.preflightTemplates[0].name : qsTr("none published")
                        color: FleetConnector.preflightTemplates.length > 0 ? qgcPal.text : _noteColor
                    }
                    QGCButton {
                        text:    qsTr("Run now")
                        visible: FleetConnector.preflightTemplates.length > 0
                        onClicked: root.runChecklist("preflight")
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    QGCLabel {
                        Layout.preferredWidth: _keyWidth
                        wrapMode: Text.WordWrap
                        text:  qsTr("Post-flight")
                        color: _noteColor
                    }
                    QGCLabel {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: FleetConnector.postflightTemplates.length > 0
                              ? FleetConnector.postflightTemplates[0].name : qsTr("none published")
                        color: FleetConnector.postflightTemplates.length > 0 ? qgcPal.text : _noteColor
                    }
                    QGCButton {
                        text:    qsTr("Run now")
                        visible: FleetConnector.postflightTemplates.length > 0
                        onClicked: root.runChecklist("postflight")
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: FleetConnector.outboxCount > 0
                    QGCLabel { Layout.preferredWidth: _keyWidth; wrapMode: Text.WordWrap; text: qsTr("Waiting to file"); color: _noteColor }
                    QGCLabel {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: _warnColor
                        text:  qsTr("%1 completed checklist(s): they will be filed when the link is back")
                                    .arg(FleetConnector.outboxCount)
                    }
                }
            }

            // ---------------------------------------- flight logs

            Card {
                title:   qsTr("Flight logs")
                visible: FleetConnector.enrolled

                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: _noteColor
                    text: qsTr("After a flight, the aircraft's own recording is collected and uploaded " +
                               "to the RotorLab Server. Logs are retained locally until the server confirms them.")
                }

                QGCCheckBox {
                    text:    qsTr("Download the log from the aircraft after a flight")
                    checked: FleetConnector.collectLogsAutomatically
                    onClicked: FleetConnector.collectLogsAutomatically = checked
                }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color:    _noteColor
                    text:     qsTr("Never while the aircraft is armed, and stopped immediately if it " +
                                   "arms. Turn this off on a slow radio and attach the file by hand.")
                }

                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible: FleetConnector.flightLogs.length === 0
                    color:   _noteColor
                    text:    qsTr("Nothing waiting: every flight this station recorded has its log.")
                }

                Repeater {
                    model: FleetConnector.flightLogs

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: ScreenTools.defaultFontPixelWidth

                        QGCLabel {
                            Layout.preferredWidth: ScreenTools.defaultFontPixelWidth * 14
                            wrapMode: Text.WordWrap
                            text:  modelData.callsign
                        }
                        QGCLabel {
                            Layout.preferredWidth: ScreenTools.defaultFontPixelWidth * 16
                            wrapMode: Text.WordWrap
                            color: _noteColor
                            text:  modelData.when + "  ·  " + modelData.duration
                        }
                        // Wrapped rather than cut off; a long file name with no
                        // spaces breaks anywhere.
                        QGCLabel {
                            Layout.fillWidth: true
                            Layout.minimumWidth: ScreenTools.defaultFontPixelWidth * 12
                            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                            color: modelData.state === "parked"  ? _warnColor
                                 : modelData.state === "done"    ? _okColor
                                                                 : qgcPal.text
                            text: {
                                if (modelData.detail !== "")
                                    return modelData.detail
                                switch (modelData.state) {
                                case "waiting":    return qsTr("waiting for the file")
                                case "collecting": return qsTr("downloading from the aircraft")
                                case "ready":      return modelData.file
                                case "uploading":  return qsTr("uploading %1%")
                                                        .arg(Math.round(modelData.progress * 100))
                                case "done":       return qsTr("stored")
                                }
                                return ""
                            }
                        }
                        QGCButton {
                            text:      qsTr("Stop")
                            visible:   modelData.state === "collecting" || modelData.state === "uploading"
                            onClicked: FleetConnector.cancelFlightLogTransfer()
                        }
                        QGCButton {
                            text:      qsTr("Try again")
                            visible:   modelData.state === "parked" && modelData.retryable
                            onClicked: FleetConnector.retryFlightLog(modelData.ref)
                        }
                        // A crew that read the card on another machine has the
                        // file when we never will. Without this the flight keeps
                        // asking for a log nobody can give it.
                        QGCButton {
                            text:      qsTr("Attach…")
                            visible:   modelData.state === "waiting" || modelData.state === "parked"
                            onClicked: {
                                logPicker.targetRef = modelData.ref
                                logPicker.openForLoad()
                            }
                        }
                    }
                }
            }

            // ---------------------------------------- switch to RotorLab

            Card {
                title:   qsTr("Or file directly to RotorLab")
                visible: FleetConnector.enrolled

                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color:    _noteColor
                    text:     qsTr("This station can report through the RotorLab Server or file flights, " +
                                   "checklists and logs directly to RotorLab.")
                }
                QGCButton {
                    text:      qsTr("Switch to filing directly to RotorLab…")
                    onClicked: root.confirmSwitchToRotorLab()
                }
            }
        }
    }

    QGCFileDialog {
        id:             logPicker
        property string targetRef: ""

        title:          qsTr("Choose the flight log")
        nameFilters:    [ qsTr("Flight logs (*.bin *.tlog *.ulg)"), qsTr("All files (*)") ]
        onAcceptedForLoad: (file) => {
            FleetConnector.attachFlightLog(logPicker.targetRef, file)
            logPicker.close()
        }
    }


    // Both prompts go through the application's own dialog rather than a bare
    // QtQuick Dialog. A bare one keeps the default light style, which on this
    // dark page renders pale grey body text on white - technically present and
    // practically unreadable.
    function confirmSwitchToRotorLab() {
        mainWindow.showMessageDialog(
            qsTr("Switch to filing directly to RotorLab?"),
            qsTr("This station will report flights directly to RotorLab instead of through the " +
                 "RotorLab Server (Fleet Server role). Telemetry and flight reports to the server will pause.\n\n" +
                 "Your enrollment on the server is kept, so switching back restores it with nothing " +
                 "to re-enter."),
            Dialog.Cancel | Dialog.Ok,
            function() { RotorLabConnector.enabled = true })
    }

    // Disconnect is local only. An operator who expects it to remove the
    // station from the Fleet Server will otherwise think it failed.
    function confirmDisconnect() {
        mainWindow.showMessageDialog(
            qsTr("Disconnect from the RotorLab Server (Fleet Server role)?"),
            qsTr("This forgets the token and all Fleet data cached on this computer. It does not " +
                 "remove the station from the server; your operator " +
                 "revokes it there. You will need a new enrollment code to reconnect."),
            Dialog.Cancel | Dialog.Ok,
            function() { FleetConnector.disconnectFromServer() })
    }
}
