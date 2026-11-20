/****************************************************************************
 *
 * RotorLab GC TAK / ATAK settings page.
 *
 * Laid out with the shared RLPageLayout (QGroundControl.Controls, A156):
 * every card fills the window width minus the standard settings padding,
 * fields stretch up to a readable maximum, rows put the field under its label
 * below about 1000 px, and long text wraps.
 * Binds to the native TAKManager (`TAKManager`).
 *
 ****************************************************************************/

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

import QGroundControl
import QGroundControl.Controls
import QGroundControl.Palette
import QGroundControl.ScreenTools

Rectangle {
    id:             root
    color:          qgcPal.window
    anchors.fill:   parent

    property string _log:       ""
    property string _testCmd:   "GOTO " + TAKManager.callsignPrefix + "-1"
    property string _testResult: ""
    property string _trustPath: ""
    property string _trustPass: ""   // empty = TAK default "atakatak" (TakManager)
    property string _clientPath: ""
    property string _clientPass: ""
    property real   _pad:       ScreenTools.defaultFontPixelWidth
    readonly property bool _narrow:        page.narrow          // field under its label

    // A SHA-256 fingerprint ("AB:CD:...", 32 bytes) as two lines of 16
    // bytes, or four lines of 8 on a narrow page, so it never breaks inside a
    // byte.
    function fingerprintLines(fp) {
        var b = fp.split(":")
        var n = _narrow ? 8 : 16
        if (b.length <= n) return fp
        var lines = []
        for (var i = 0; i < b.length; i += n) lines.push(b.slice(i, i + n).join(":"))
        return lines.join(":\n")
    }

    function urlToPath(u) {
        var s = u.toString()
        if (s.startsWith("file:///")) s = s.substring(8)
        else if (s.startsWith("file://")) s = s.substring(7)
        return decodeURIComponent(s)
    }

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    // Note and status colors come from the shared page layout.
    readonly property color _warnColor:  page.warnColor
    readonly property color _okColor:    page.okColor

    Connections {
        target: TAKManager
        function onLogMessage(line) {
            _log = line + "\n" + _log
            if (_log.length > 4000) _log = _log.substring(0, 4000)
        }
    }

    FileDialog {
        id: trustDialog
        title: qsTr("Select TAK truststore (.p12 or .pem)")
        onAccepted: _trustPath = urlToPath(selectedFile)
    }
    FileDialog {
        id: clientDialog
        title: qsTr("Select client key (.p12 / .pfx / .pem)")
        onAccepted: _clientPath = urlToPath(selectedFile)
    }

    // Labelled text-field row, placed by the shared rule in RLPageLayout: the
    // label column is 40% of the row up to about 40 character widths and
    // wraps, the field follows right after it up to 48 character widths, and
    // the rest of the row stays empty. On a narrow page the field goes under
    // its label. The field's size is a maximum, not a minimum, so it always
    // shrinks with the window instead of running past the edge.
    // The entry uses the theme's input colors, with a blue focus outline and a
    // blue caret, so it is obvious where focus is and where text will be typed.
    component Field: GridLayout {
        id: _row
        property alias label: _l.text
        property alias text: _tf.text
        property alias echoMode: _tf.echoMode
        property alias placeholderText: _tf.placeholderText
        signal committed(string value)
        Layout.fillWidth:   true
        columns:            _narrow ? 1 : 3
        columnSpacing:      _pad * 2
        rowSpacing:         _pad / 2
        QGCLabel {
            id:                     _l
            Layout.fillWidth:       _narrow
            Layout.preferredWidth:  _narrow ? -1 : page.labelColumnWidth(_row.width)
            verticalAlignment:      Text.AlignVCenter
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
            font.pointSize:         ScreenTools.defaultFontPointSize
            verticalAlignment:      TextInput.AlignVCenter
            leftPadding:            ScreenTools.defaultFontPixelWidth
            rightPadding:           ScreenTools.defaultFontPixelWidth
            selectByMouse:          true
            persistentSelection:    true
            selectionColor:         page.focusColor
            selectedTextColor:      "#0F1722"
            placeholderTextColor:   page.placeholderColor
            background: Rectangle {
                color:          page.inputColor
                radius:         4
                border.width:   _tf.activeFocus ? 2 : 1
                border.color:   _tf.activeFocus ? page.focusColor : page.inputBorderColor
            }
            cursorDelegate: Rectangle {
                width:      2
                color:      page.focusColor
                visible:    _tf.cursorVisible
            }
            onEditingFinished:      parent.committed(text)
        }
        Item { Layout.fillWidth: true; visible: !_narrow }
    }

    // Titled bordered card; children go inside the border.
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
            font.pointSize: ScreenTools.defaultFontPointSize + 1
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

    // The shared page layout (A156): full width minus the standard settings
    // padding, scrolls vertically, and says when the page is narrow.
    RLPageLayout {
        id:             page
        anchors.fill:   parent

        ColumnLayout {
            id:                 outer
            Layout.fillWidth:   true
            spacing:            ScreenTools.defaultFontPixelHeight

            Card {
                title: qsTr("TAK / OpenTAKServer")
                Field { label: qsTr("Server host"); text: TAKManager.host
                    onCommitted: TAKManager.host = value }
                Field { label: qsTr("CoT stream port"); text: TAKManager.port.toString()
                    onCommitted: TAKManager.port = parseInt(value) || 8089 }
                Field { label: qsTr("Enrollment port"); text: TAKManager.enrollPort.toString()
                    onCommitted: TAKManager.enrollPort = parseInt(value) || 8446 }
                Field { label: qsTr("Username"); text: TAKManager.username
                    onCommitted: TAKManager.username = value }
                Field { label: qsTr("Password"); echoMode: TextInput.Password; text: TAKManager.password
                    onCommitted: TAKManager.password = value }
                // Where the password is kept (G41): the platform key store,
                // or this session only. Never the settings file.
                QGCLabel {
                    Layout.fillWidth: true
                    visible:   TAKManager.passwordStorageText !== ""
                    wrapMode:  Text.WordWrap
                    text:      TAKManager.passwordStorageText
                }
                Field { label: qsTr("Callsign prefix"); text: TAKManager.callsignPrefix
                    onCommitted: TAKManager.callsignPrefix = value }
                // G47: a prefix that cannot name this station is not saved
                // (T12); a change turns "Commands to aircraft on" off (T14).
                QGCLabel {
                    Layout.fillWidth: true
                    visible:    TAKManager.prefixEntryError !== ""
                    wrapMode:   Text.WordWrap
                    textFormat: Text.PlainText
                    font.bold:  true
                    color:      _warnColor
                    text:       TAKManager.prefixEntryError
                }
                QGCLabel {
                    Layout.fillWidth: true
                    visible:    TAKManager.prefixChangedNotice !== ""
                    wrapMode:   Text.WordWrap
                    textFormat: Text.PlainText
                    text:       TAKManager.prefixChangedNotice
                }
                Field { label: qsTr("GCS UID"); text: TAKManager.gcsUid
                    onCommitted: TAKManager.gcsUid = value }
                QGCCheckBox { text: qsTr("Verify server certificate"); checked: TAKManager.verifyPeer
                    onClicked: TAKManager.verifyPeer = checked }
                // G40: the stronger warning. Verify stays the operator's
                // setting (default on); turning it off is also logged.
                Rectangle {
                    Layout.fillWidth: true
                    visible:        !TAKManager.verifyPeer
                    color:          "transparent"
                    border.color:   _warnColor
                    border.width:   2
                    radius:         4
                    implicitHeight: _verifyOffText.implicitHeight + _pad * 2
                    QGCLabel {
                        id:        _verifyOffText
                        x:         _pad
                        y:         _pad
                        width:     parent.width - _pad * 2
                        wrapMode:  Text.WordWrap
                        color:     _warnColor
                        textFormat: Text.StyledText
                        text:      "<b>" + qsTr("Verify server certificate is off.") + "</b> " +
                                   qsTr("The CoT stream accepts any server certificate, so any device on the " +
                                        "network path can pose as the TAK server. It can see the positions " +
                                        "this station sends, and it can send markers and commands to this " +
                                        "station as if it were the TAK server. With \"Commands to aircraft " +
                                        "on\", this station handles those commands like commands from the " +
                                        "real server and can send them to the aircraft. This setting is on by " +
                                        "default; turning it off is meant for testing on a network you " +
                                        "control.") + "<br><br>" +
                                   qsTr("Enrollment still sends the TAK username and password only after you " +
                                        "accept the server's certificate fingerprint below.")
                    }
                }
                QGCLabel {
                    Layout.fillWidth: true
                    visible:   TAKManager.verifyPeer && !TAKManager.haveTruststore
                    wrapMode:  Text.WordWrap
                    color:     _warnColor
                    text:      qsTr("No CA is imported. The station checks the server certificate against the " +
                                    "system trust store and any accepted fingerprint. If the system trust store " +
                                    "does not verify it (for example a self-signed TAK server), enrollment sends " +
                                    "the TAK username and password only after you accept the server's " +
                                    "certificate fingerprint below. Import the TAK server's truststore to verify " +
                                    "it by CA.")
                }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode:  Text.WordWrap
                    text:      qsTr("Certificate settings apply at the next enrollment or connect. A live " +
                                    "connection is not dropped.")
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: TAKManager.pendingFingerprint !== ""
                    QGCLabel {
                        Layout.fillWidth: true
                        wrapMode:  Text.WordWrap
                        font.bold: true
                        color:     _warnColor
                        text:      qsTr("Server certificate not verified. Nothing was sent to it. Compare this " +
                                        "SHA-256 fingerprint with the one your TAK server administrator gives " +
                                        "you before accepting it:")
                    }
                    QGCLabel {
                        Layout.fillWidth: true
                        textFormat:  Text.PlainText
                        font.family: ScreenTools.fixedFontFamily
                        text:        fingerprintLines(TAKManager.pendingFingerprint)
                    }
                    QGCButton {
                        text:      qsTr("Accept this certificate")
                        onClicked: TAKManager.acceptServerFingerprint()
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: TAKManager.pinnedFingerprint !== ""
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        QGCLabel {
                            Layout.fillWidth: true
                            wrapMode:   Text.WordWrap
                            text:       qsTr("Accepted server certificate (SHA-256):")
                        }
                        QGCLabel {
                            Layout.fillWidth: true
                            textFormat:  Text.PlainText
                            font.family: ScreenTools.fixedFontFamily
                            text:        fingerprintLines(TAKManager.pinnedFingerprint)
                        }
                    }
                    QGCButton {
                        text:      qsTr("Forget")
                        onClicked: TAKManager.forgetServerFingerprint()
                    }
                }
            }

            Card {
                title: qsTr("Enrollment & link")
                RowLayout {
                    Layout.fillWidth: true
                    QGCButton { text: qsTr("Enroll client cert"); onClicked: TAKManager.enroll() }
                    QGCButton { text: qsTr("Clear certs"); onClicked: TAKManager.clearCerts() }
                    QGCLabel {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: TAKManager.enrolled ? qsTr("enrolled")
                            : TAKManager.pendingFingerprint !== "" ? qsTr("not enrolled. Accept the certificate above.")
                            : qsTr("not enrolled")
                        color: TAKManager.enrolled ? _okColor : _warnColor
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    QGCButton {
                        text: TAKManager.connected ? qsTr("Disconnect") : qsTr("Connect")
                        onClicked: TAKManager.connected ? TAKManager.disconnectServer() : TAKManager.connectServer()
                    }
                    QGCLabel {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("Status: ") + TAKManager.statusText
                        color: TAKManager.connected ? _okColor : qgcPal.text
                    }
                }
            }

            Card {
                title: qsTr("Truststore & client key")
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Provide certificates directly instead of (or in addition to) enrolling. " +
                               "Truststore = the server CA, usually a TAK truststore-*.p12 (default password " +
                               "atakatak). Client key = your personal identity (.p12/.pfx), or a .pem holding " +
                               "both key and certificate.")
                }
                Field { label: qsTr("Truststore file"); text: _trustPath
                    onCommitted: _trustPath = value }
                Field { label: qsTr("Truststore password"); echoMode: TextInput.Password; text: _trustPass
                    placeholderText: qsTr("Default: atakatak")
                    onCommitted: _trustPass = value }
                RowLayout {
                    Layout.fillWidth: true
                    QGCButton { text: qsTr("Browse…"); onClicked: trustDialog.open() }
                    QGCButton { text: qsTr("Import truststore"); onClicked: TAKManager.importTruststore(_trustPath, _trustPass) }
                }
                Field { label: qsTr("Client key file"); text: _clientPath
                    onCommitted: _clientPath = value }
                Field { label: qsTr("Client key password"); echoMode: TextInput.Password; text: _clientPass
                    onCommitted: _clientPass = value }
                RowLayout {
                    Layout.fillWidth: true
                    QGCButton { text: qsTr("Browse…"); onClicked: clientDialog.open() }
                    QGCButton { text: qsTr("Import client key"); onClicked: TAKManager.importClientCert(_clientPath, _clientPass) }
                }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Loaded: ") + TAKManager.certInfo
                    color: TAKManager.enrolled ? _okColor : qgcPal.text
                }
            }

            Card {
                title: qsTr("GCS location")
                QGCCheckBox {
                    text: qsTr("Share this station's position with the TAK server")
                    checked: TAKManager.shareStationPosition
                    onClicked: TAKManager.shareStationPosition = checked
                }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("When on, the TAK server and everyone in this station's TAK groups can see " +
                               "where this computer is. When off, the GCS beacon still announces this " +
                               "station, with no location.")
                }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Position the GCS beacon sends when sharing is on. Automatic uses QGC's ground-station " +
                               "location: your system location, or an NMEA GPS device such as a USB GPS " +
                               "dongle configured in General settings. Manual broadcasts the fixed " +
                               "coordinates below (useful for a fixed ground station with no GPS).")
                }
                QGCCheckBox {
                    text: qsTr("Set GCS location manually")
                    checked: TAKManager.gcsManual
                    onClicked: TAKManager.gcsManual = checked
                }
                Field { label: qsTr("Latitude"); text: TAKManager.gcsManualLat.toFixed(7)
                    onCommitted: TAKManager.gcsManualLat = parseFloat(value) || 0 }
                Field { label: qsTr("Longitude"); text: TAKManager.gcsManualLon.toFixed(7)
                    onCommitted: TAKManager.gcsManualLon = parseFloat(value) || 0 }
                Field { label: qsTr("Altitude (m HAE)"); text: TAKManager.gcsManualAlt.toFixed(1)
                    onCommitted: TAKManager.gcsManualAlt = parseFloat(value) || 0 }
                RowLayout {
                    Layout.fillWidth: true
                    QGCButton {
                        text: qsTr("Use my current location")
                        onClicked: TAKManager.captureCurrentLocation()
                    }
                    QGCLabel { Layout.fillWidth: true }
                }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Broadcasting → ") + TAKManager.gcsPositionText
                    color: _okColor
                }
            }

            Card {
                title: qsTr("Publish to ATAK")
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Drones publish as air tracks (a-f-A-M-F-Q / a-f-A-M-H-Q) linked to the GCS EUD. " +
                               "The GCS publishes as a self EUD whose UID matches the enrolled certificate.")
                }
                QGCCheckBox { text: qsTr("Publish drones as CoT markers"); checked: TAKManager.publishEnabled
                    onClicked: TAKManager.publishEnabled = checked }
                QGCCheckBox { text: qsTr("Publish GCS beacon"); checked: TAKManager.publishGcsBeacon
                    onClicked: TAKManager.publishGcsBeacon = checked }
                QGCCheckBox { text: qsTr("Let ATAK address drones"); checked: TAKManager.dronesAddressable
                    onClicked: TAKManager.dronesAddressable = checked }
                QGCCheckBox { text: qsTr("Direct (addressed) instead of broadcast"); checked: TAKManager.publishDirect
                    onClicked: TAKManager.publishDirect = checked }
                Field { label: qsTr("Direct recipients (callsigns)"); text: TAKManager.directDest
                    onCommitted: TAKManager.directDest = value }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("ATAK EUD callsigns, separated by commas. Used when Direct is on.")
                }
            }

            Card {
                title: qsTr("Command By ATAK")
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("A verb from a map marker runs the identical RotorLab GC guided action a controller would, with no separate geofence or mode logic. Marker name example: GOTO 2 ALT 400 SPD 5")
                }
                QGCCheckBox { text: qsTr("Read commands from ATAK markers"); checked: TAKManager.commandEnabled
                    onClicked: TAKManager.commandEnabled = checked }
                // Command markers being dropped (5.1.2 review R1, R3).
                QGCLabel {
                    Layout.fillWidth: true
                    visible:    TAKManager.commandDropNotice !== ""
                    wrapMode:   Text.WordWrap
                    textFormat: Text.PlainText
                    font.bold:  true
                    color:      _warnColor
                    text:       TAKManager.commandDropNotice
                }
                // An empty callsign prefix stops command reading (G50).
                QGCLabel {
                    Layout.fillWidth: true
                    visible:   TAKManager.commandPrefixEmpty
                    wrapMode:  Text.WordWrap
                    font.bold: true
                    color:     _warnColor
                    text:      TAKManager.emptyPrefixNotice
                }
                // G47: a saved prefix that cannot name this station (T13), and
                // another station on the link using this prefix (T8).
                QGCLabel {
                    Layout.fillWidth: true
                    visible:    TAKManager.prefixProblemNotice !== ""
                    wrapMode:   Text.WordWrap
                    textFormat: Text.PlainText
                    font.bold:  true
                    color:      _warnColor
                    text:       TAKManager.prefixProblemNotice
                }
                // Security M1: commands are not read until the prefix is set or
                // confirmed on this install (T19).
                QGCLabel {
                    Layout.fillWidth: true
                    visible:    TAKManager.prefixNotSetNotice !== ""
                    wrapMode:   Text.WordWrap
                    textFormat: Text.PlainText
                    font.bold:  true
                    color:      _warnColor
                    text:       TAKManager.prefixNotSetNotice
                }
                QGCButton {
                    visible:   TAKManager.prefixNotSetNotice !== ""
                    text:      qsTr("Use %1 for this station").arg(TAKManager.callsignPrefix)
                    onClicked: TAKManager.confirmCallsignPrefix()
                }
                QGCLabel {
                    Layout.fillWidth: true
                    visible:    TAKManager.prefixConflictNotice !== ""
                    wrapMode:   Text.WordWrap
                    textFormat: Text.PlainText
                    font.bold:  true
                    color:      _warnColor
                    text:       TAKManager.prefixConflictNotice
                }
                // How a marker names this station's aircraft (G47, G48: T17).
                QGCLabel {
                    Layout.fillWidth: true
                    visible:    TAKManager.commandEnabled && !TAKManager.commandPrefixEmpty
                                && TAKManager.prefixProblemNotice === ""
                    wrapMode:   Text.WordWrap
                    textFormat: Text.PlainText
                    text:       TAKManager.addressingHelp
                }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible:  TAKManager.connected && TAKManager.commandEnabled && !TAKManager.cotReceivedSinceConnect
                    text:     qsTr("No CoT received from the TAK server since connecting")
                }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible:  TAKManager.commandEnabled && !TAKManager.commandPrefixEmpty
                              && TAKManager.prefixProblemNotice === ""
                    text:     qsTr("While Read commands from ATAK markers is on, this station appears in TAK " +
                                   "contact lists as %1. Its location is shared only while Publish drones as " +
                                   "CoT markers, Publish GCS beacon and Share this station's position " +
                                   "are on.").arg(TAKManager.callsignPrefix + "-GCS")
                }
                QGCCheckBox { text: qsTr("Command By ATAK on"); checked: TAKManager.c2Allowed
                    onClicked: TAKManager.c2Allowed = checked }
                QGCCheckBox { text: qsTr("Commands to aircraft on"); checked: TAKManager.c2Armed
                    onClicked: TAKManager.c2Armed = checked }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("When this is on, commands read from ATAK markers that pass this station's " +
                               "checks (enabled verbs, ATAK command senders, and the other settings on this " +
                               "page) are sent to the aircraft. When it is off, none are sent.")
                }
                QGCLabel {
                    Layout.fillWidth: true
                    visible:   TAKManager.openSenderWarning !== ""
                    wrapMode:  Text.WordWrap
                    font.bold: true
                    color:     _warnColor
                    text:      TAKManager.openSenderWarning
                }
                Field {
                    label: qsTr("Altitude warning (ft above launch)")
                    text:  TAKManager.altWarnAboveFt > 0 ? TAKManager.altWarnAboveFt.toString() : ""
                    onCommitted: TAKManager.altWarnAboveFt = Math.max(0, parseFloat(value) || 0)
                }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Empty means no warning. " +
                               "A command with an ALT above this level is still sent to the aircraft. " +
                               "The station alert adds a warning line. This level is set by this station. It is " +
                               "not a regulatory limit, and no warning does not mean a command is within any " +
                               "limit. Feet above launch is not feet above ground level.")
                }
            }

            Card {
                title: qsTr("Remote commands received")
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Commands received from ATAK since RotorLab GC started, newest first: verb, " +
                               "reported sender callsign and UID, target aircraft, time and result. Commands that " +
                               "pass this station's checks are sent to the aircraft when received; this list " +
                               "does not hold or cancel them. ARM, TKOFF " +
                               "and GOTO also show a banner over the fly view until dismissed. Sent means " +
                               "transmitted to the aircraft. The aircraft's response is not shown here.")
                }
                RowLayout {
                    Layout.fillWidth: true
                    QGCLabel {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: TAKManager.remoteCommands.length === 0
                              ? qsTr("None received since RotorLab GC started")
                              : qsTr("%1 received").arg(TAKManager.remoteCommands.length)
                    }
                    QGCButton {
                        text:    qsTr("Clear view")
                        enabled: TAKManager.remoteCommands.length > 0
                        onClicked: TAKManager.clearRemoteCommands()
                    }
                }
                // The entries sit under the "N received" line; the log path follows the list (G54).
                Repeater {
                    model: TAKManager.remoteCommands
                    delegate: ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        QGCLabel {
                            Layout.fillWidth: true
                            wrapMode:    Text.WordWrap
                            textFormat:  Text.PlainText
                            text:        modelData.date + " " + modelData.time + "  " + modelData.summary
                        }
                        // Marks a command whose banner is still showing on the fly view.
                        QGCLabel {
                            visible:        modelData.banner
                            font.pointSize: ScreenTools.defaultFontPointSize
                            color:          _warnColor
                            text:           qsTr("banner showing")
                        }
                        QGCLabel {
                            Layout.fillWidth: true
                            visible:     modelData.warning !== ""
                            wrapMode:    Text.WordWrap
                            textFormat:  Text.PlainText
                            color:       _warnColor
                            text:        modelData.warning
                        }
                    }
                }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Clears this screen only. All received commands stay in the command log.")
                }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode:   Text.WordWrap
                    text:       qsTr("Command log (the app only adds to this file. It is a station record " +
                                     "only; the file can be changed outside the app):")
                }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode:    Text.WrapAnywhere
                    textFormat:  Text.PlainText
                    font.family: ScreenTools.fixedFontFamily
                    text:        TAKManager.commandLogPath
                }
                QGCLabel {
                    Layout.fillWidth: true
                    visible: TAKManager.commandLogError
                    wrapMode: Text.WordWrap
                    color:   _warnColor
                    text:    qsTr("Command log could not be written")
                }
            }

            Card {
                id:    _pilotsCard
                title: qsTr("ATAK command senders")
                property string pendingPilot: ""
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Limit Command By ATAK to listed senders. The list matches the callsign " +
                               "and device UID each sender reports about itself in its marker; this station " +
                               "does not verify either. A device UID is less likely than a callsign to change " +
                               "or be reused by mistake, but anyone who can send to this TAK server can copy " +
                               "either. Add senders by device UID (such as ANDROID-...) where you can. A UID " +
                               "entry matches the reported UID only; a callsign entry matches the reported " +
                               "callsign only. Commands from anyone else are not sent to the aircraft and are " +
                               "logged. With the list on and empty, no command from ATAK is sent. This station " +
                               "cannot tell who really sent a marker. To limit who can send commands, put this " +
                               "station's certificate in a TAK server group shared only with the people who " +
                               "should command.")
                }
                QGCCheckBox {
                    text: qsTr("Send commands only from listed senders")
                    checked: TAKManager.approvedOnly
                    onClicked: TAKManager.approvedOnly = checked
                }
                QGCLabel {
                    Layout.fillWidth: true
                    visible:   TAKManager.openSenderWarning !== ""
                    wrapMode:  Text.WordWrap
                    font.bold: true
                    color:     _warnColor
                    text:      TAKManager.openSenderWarning
                }
                Field {
                    label: qsTr("Device UID or callsign")
                    text: _pilotsCard.pendingPilot
                    onCommitted: _pilotsCard.pendingPilot = value
                }
                RowLayout {
                    Layout.fillWidth: true
                    QGCButton {
                        text: qsTr("Add as device UID")
                        onClicked: {
                            TAKManager.addApprovedPilotAs(_pilotsCard.pendingPilot, "uid")
                            _pilotsCard.pendingPilot = ""
                        }
                    }
                    QGCButton {
                        text: qsTr("Add as callsign")
                        onClicked: {
                            TAKManager.addApprovedPilotAs(_pilotsCard.pendingPilot, "callsign")
                            _pilotsCard.pendingPilot = ""
                        }
                    }
                    QGCLabel {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: TAKManager.approvedPilots.length === 0
                              ? (TAKManager.approvedOnly ? qsTr("List empty. No command from ATAK is sent.")
                                                         : qsTr("List empty and not in use."))
                              : qsTr("%1 on the list").arg(TAKManager.approvedPilots.length)
                        color: TAKManager.approvedOnly && TAKManager.approvedPilots.length === 0
                               ? _warnColor : qgcPal.text
                    }
                }
                Repeater {
                    model: TAKManager.approvedPilotRows
                    delegate: RowLayout {
                        Layout.fillWidth: true
                        QGCLabel {
                            Layout.preferredWidth: ScreenTools.defaultFontPixelWidth * 12
                            wrapMode: Text.WordWrap
                            text: modelData.kind === "uid" ? qsTr("Device UID")
                                : modelData.kind === "callsign" ? qsTr("Callsign")
                                : qsTr("UID or callsign")
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            QGCLabel {
                                Layout.fillWidth: true
                                wrapMode:    Text.WrapAnywhere
                                textFormat:  Text.PlainText
                                text:        modelData.value
                                font.family: ScreenTools.fixedFontFamily
                            }
                            QGCLabel {
                                Layout.fillWidth: true
                                visible:  modelData.kind === "any"
                                wrapMode: Text.WordWrap
                                color:    _warnColor
                                text:     qsTr("Matches a callsign or a UID. Add this sender again as a device " +
                                               "UID or a callsign.")
                            }
                        }
                        QGCButton {
                            text: qsTr("Remove")
                            onClicked: TAKManager.removeApprovedPilot(modelData.entry)
                        }
                    }
                }
            }

            Card {
                title: qsTr("Enabled verbs")
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Only checked verbs are sent to the aircraft. Every verb is off until checked here.")
                }
                QGCCheckBox { text: qsTr("GOTO  (go / vec / vector / send)"); checked: TAKManager.verbGoto
                    onClicked: TAKManager.verbGoto = checked }
                QGCCheckBox { text: qsTr("RTL  (return to launch)"); checked: TAKManager.verbRtl
                    onClicked: TAKManager.verbRtl = checked }
                QGCCheckBox { text: qsTr("LAND"); checked: TAKManager.verbLand
                    onClicked: TAKManager.verbLand = checked }
                QGCCheckBox { text: qsTr("HOLD  (pause in place)"); checked: TAKManager.verbHold
                    onClicked: TAKManager.verbHold = checked }
                QGCCheckBox { text: qsTr("ALTHOLD  (change altitude)"); checked: TAKManager.verbAltHold
                    onClicked: TAKManager.verbAltHold = checked }
                QGCCheckBox { text: qsTr("GUIDED  (seize control)"); checked: TAKManager.verbGuided
                    onClicked: TAKManager.verbGuided = checked }
                QGCCheckBox { text: qsTr("ARM"); checked: TAKManager.verbArm
                    onClicked: TAKManager.verbArm = checked }
                QGCCheckBox { text: qsTr("DISARM"); checked: TAKManager.verbDisarm
                    onClicked: TAKManager.verbDisarm = checked }
                QGCCheckBox { text: qsTr("TKOFF  (take off; arms the aircraft only when ARM is on)"); checked: TAKManager.verbTakeoff
                    onClicked: TAKManager.verbTakeoff = checked }
            }

            Card {
                title: qsTr("Speed scale (SPD 1-10)")
                Field { label: qsTr("Speed step 1 (m/s)"); text: TAKManager.spdMin.toString()
                    onCommitted: TAKManager.spdMin = parseFloat(value) || 0 }
                Field { label: qsTr("Speed step 10 (m/s)"); text: TAKManager.spdMax.toString()
                    onCommitted: TAKManager.spdMax = parseFloat(value) || 0 }
            }

            Card {
                title: qsTr("Actions & validation")
                RowLayout {
                    Layout.fillWidth: true
                    QGCButton { text: qsTr("Save settings"); onClicked: TAKManager.save() }
                    QGCButton { text: qsTr("Run self-test"); onClicked: _log = TAKManager.runSelfTest() + "\n" + _log }
                }
                Field { label: qsTr("Test marker command"); text: _testCmd
                    onCommitted: _testCmd = value }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("The test command is subject to the same station checks as a marker from ATAK. Its " +
                               "sender is %1 (UID %2). With \"Send commands only from listed senders\" on, add that " +
                               "UID to the list as a device UID to test.").arg(TAKManager.testSenderCallsign).arg(TAKManager.testSenderUid)
                }
                RowLayout {
                    Layout.fillWidth: true
                    QGCButton {
                        text: qsTr("Send test command through real path")
                        onClicked: _testResult = TAKManager.injectTestCommand(_testCmd)
                    }
                    // The station's result for the last test command, including
                    // a command it could not read.
                    QGCLabel {
                        Layout.fillWidth: true
                        wrapMode:   Text.WordWrap
                        textFormat: Text.PlainText
                        text:       _testResult
                        color:      _testResult !== "" && !TAKManager.lastTestSent ? _warnColor : qgcPal.text
                    }
                }
            }

            Card {
                title: qsTr("Activity log")
                QGCLabel {
                    Layout.fillWidth:       true
                    Layout.minimumHeight:   ScreenTools.defaultFontPixelHeight * 10
                    verticalAlignment:      Text.AlignTop
                    wrapMode:               Text.WordWrap
                    font.family:            ScreenTools.fixedFontFamily
                    text:                   _log
                }
            }
        }
    }
}
