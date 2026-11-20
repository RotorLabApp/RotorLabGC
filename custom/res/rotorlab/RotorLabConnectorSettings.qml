/****************************************************************************
 *
 * RotorLab GC - RotorLab Connector settings page.
 *
 * Files this ground station's flights, checklists and logs straight to
 * RotorLab.app. In order: the toggle, the key, the behaviour switches, how this
 * operation files, checklists, missions, and the queue.
 *
 * Nothing here is ever allowed to block, delay or complicate flying, in any
 * state including the red one.
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
    property real _keyWidth:      ScreenTools.defaultFontPixelWidth * 22   // left label column
    readonly property bool _narrow: page.narrow   // field under its label

    // Labelled rows are placed by the shared rule in RLPageLayout: the label
    // column is 40% of the row up to about 40 character widths and wraps, the
    // field follows right after it up to 48 character widths, and the rest of
    // the row stays empty. On a narrow page the field goes under its label.
    // The field's size is a maximum, so it always shrinks with the window.
    // Entries use the theme's input colors with a blue focus outline.

    property string _key:      ""
    // Two separate things. `_editing` decides whether the key field is on the
    // page at all; `_reveal` only decides whether what is typed into it is
    // masked. Conflating them made re-keying impossible: the field was hidden
    // once a key was stored, and the button that would have brought it back
    // was inside the hidden field's own row.
    property bool   _editing:  false
    property bool   _reveal:   false

    /// Open a checklist on demand, from the rows below. Same runner and the
    /// same filing path the offer card uses - the run is spooled and retried
    /// exactly like one started from the offer.
    function runChecklist(kind, index) {
        manualRunner.active = true
        if (manualRunner.item) {
            manualRunner.item.backend    = RotorLabConnector
            manualRunner.item.kind       = kind
            manualRunner.item.startIndex = index === undefined ? 0 : index
            manualRunner.item.open()
        }
    }

    Loader {
        id:     manualRunner
        active: false
        source: "qrc:/custom/fleet/ChecklistRunner.qml"
    }

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    // Note and status colors come from the shared page layout (A156).
    readonly property color _noteColor:  page.noteColor
    readonly property color _warnColor:  page.warnColor
    readonly property color _errorColor: page.errorColor
    readonly property color _okColor:    page.okColor

    // RotorLabConnector.State
    readonly property int _stNotConfigured:  0
    readonly property int _stVerifying:      1
    readonly property int _stConnected:      2
    readonly property int _stOffline:        3
    readonly property int _stKeyRejected:    4
    readonly property int _stAccountBlocked: 5
    readonly property int _stUnavailable:    6

    function _stateColor() {
        switch (RotorLabConnector.state) {
        case _stConnected:      return _okColor
        case _stOffline:
        case _stUnavailable:
        case _stAccountBlocked: return _warnColor
        case _stKeyRejected:    return _errorColor
        default:                return qgcPal.text
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

    component Row2: RowLayout {
        property alias label: _k.text
        property alias value: _v.text
        property color valueColor: qgcPal.text
        Layout.fillWidth: true
        QGCLabel { id: _k; Layout.preferredWidth: _keyWidth; wrapMode: Text.WordWrap; color: _noteColor }
        QGCLabel { id: _v; Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText; color: parent.valueColor }
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

            // ------------------------------------------------ 1. enable

            Card {
                title: qsTr("RotorLab Connector")

                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Files this ground station's flights, checklists and logs straight to " +
                               "RotorLab. An organization admin creates a Device Key in RotorLab under " +
                               "Organization, Integrations, Devices.")
                }
                QGCCheckBox {
                    id:        enableBox
                    text:      qsTr("Enable RotorLab Connector")
                    checked:   RotorLabConnector.enabled
                    onClicked: {
                        // Switching connectors pauses Fleet Server reporting
                        // while the direct RotorLab connector is enabled.
                        if (checked && FleetConnector.state === 3 /* Connected */) {
                            checked = false          // not until they say so
                            root.confirmSwitchFromFleet()
                        } else {
                            RotorLabConnector.enabled = checked
                        }
                    }
                }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color:    _noteColor
                    text:     qsTr("Nothing here can hold up a launch. In every state (including a " +
                                   "rejected key) the aircraft flies, arms and shows video exactly " +
                                   "as it does with no connector configured.")
                }
            }

            // ------------------------------------------------ 2. device key

            Card {
                title:   qsTr("Device Key")
                visible: RotorLabConnector.enabled

                RowLayout {
                    Layout.fillWidth: true
                    spacing: _pad * 2
                    QGCLabel {
                        Layout.preferredWidth: _keyWidth
                        wrapMode: Text.WordWrap
                        text:  qsTr("Status")
                        color: _noteColor
                    }
                    QGCLabel {
                        Layout.fillWidth: true
                        text:      RotorLabConnector.stateText
                        color:     _stateColor()
                        font.bold: true
                        wrapMode:  Text.WordWrap
                    }
                }

                Row2 {
                    label: qsTr("Key")
                    value: RotorLabConnector.maskedKey
                    visible: RotorLabConnector.configured
                }
                Row2 {
                    label: qsTr("Name in RotorLab")
                    value: RotorLabConnector.keyLabel
                    visible: RotorLabConnector.keyLabel !== ""
                }
                Row2 {
                    label: qsTr("Stored in")
                    value: RotorLabConnector.keyStoreName
                    visible: RotorLabConnector.configured
                }
                // G82: kept for this session only, or moved from / removed
                // from the settings file. Never saved in the settings file.
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode:   Text.WordWrap
                    textFormat: Text.PlainText
                    visible:    RotorLabConnector.keyStorageText !== ""
                    text:       RotorLabConnector.keyStorageText
                }
                Row2 {
                    label: qsTr("Server")
                    value: RotorLabConnector.baseUrl
                    visible: RotorLabConnector.configured
                }

                // --- entry ---
                GridLayout {
                    id:               keyRow
                    Layout.fillWidth: true
                    visible: !RotorLabConnector.configured || _editing
                    columns:          _narrow ? 2 : 4
                    columnSpacing:    _pad * 2
                    rowSpacing:       _pad / 2
                    QGCLabel {
                        Layout.alignment: Qt.AlignVCenter
                        Layout.fillWidth: _narrow
                        Layout.preferredWidth: _narrow ? -1 : page.labelColumnWidth(keyRow.width)
                        Layout.columnSpan: _narrow ? 2 : 1
                        wrapMode: Text.WordWrap
                        text: RotorLabConnector.configured ? qsTr("New Device Key") : qsTr("Device Key")
                    }
                    TextField {
                        id:                     keyField
                        Layout.fillWidth:       true
                        Layout.minimumWidth:    ScreenTools.defaultFontPixelWidth * 8
                        Layout.maximumWidth:    page.fieldMaxWidthFor(keyRow.width,
                                                                      keyRow.columnSpacing * 3 + revealButton.width)
                        Layout.preferredWidth:  Layout.maximumWidth
                        Layout.preferredHeight: ScreenTools.defaultFontPixelHeight * 2
                        echoMode:               _reveal ? TextInput.Normal : TextInput.Password
                        placeholderText:        "rl_…"
                        color:                  page.inputTextColor
                        placeholderTextColor:   page.placeholderColor
                        selectByMouse:          true
                        leftPadding:            ScreenTools.defaultFontPixelWidth
                        onTextChanged:          root._key = text
                        background: Rectangle {
                            color:        page.inputColor
                            radius:       4
                            border.width: keyField.activeFocus ? 2 : 1
                            border.color: keyField.activeFocus ? page.focusColor : page.inputBorderColor
                        }
                    }
                    QGCButton {
                        id:        revealButton
                        text:      _reveal ? qsTr("Hide") : qsTr("Show")
                        onClicked: _reveal = !_reveal
                    }
                    Item { Layout.fillWidth: true; visible: !_narrow }
                }

                // Told immediately, rather than after a round trip.
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible:  _key.length > 0 && !RotorLabConnector.looksLikeDeviceKey(_key)
                    color:    _warnColor
                    text:     qsTr("That does not look like a Device Key. Device Keys begin with rl_ " +
                                   "and are created by an organization admin in RotorLab under " +
                                   "Organization, Integrations, Devices.")
                }

                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible:  RotorLabConnector.lastError !== ""
                    color:    RotorLabConnector.state === _stKeyRejected ? _errorColor : _warnColor
                    textFormat: Text.PlainText
                    text:     RotorLabConnector.lastError
                }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible:  RotorLabConnector.statusText !== ""
                    color:    _warnColor
                    textFormat: Text.PlainText
                    text:     RotorLabConnector.statusText
                }

                RowLayout {
                    Layout.fillWidth: true
                    QGCButton {
                        text:      qsTr("Verify")
                        enabled:   !RotorLabConnector.busy &&
                                   (RotorLabConnector.looksLikeDeviceKey(_key) || RotorLabConnector.configured)
                        onClicked: {
                            if (RotorLabConnector.looksLikeDeviceKey(_key)) {
                                RotorLabConnector.setKeyAndVerify(_key)
                                _key = ""
                                keyField.text = ""
                                _editing = false
                            } else {
                                RotorLabConnector.verifyNow()
                            }
                        }
                    }
                    // The way a station is re-keyed without an outage, and the
                    // only way back to the key field once one is stored.
                    QGCButton {
                        text:      _editing ? qsTr("Cancel") : qsTr("Replace key")
                        visible:   RotorLabConnector.configured
                        onClicked: {
                            _editing = !_editing
                            if (!_editing) {
                                _key = ""
                                keyField.text = ""
                            }
                        }
                    }
                    QGCButton {
                        text:      qsTr("Disconnect")
                        visible:   RotorLabConnector.configured
                        onClicked: root.confirmDisconnect()
                    }
                    Item { Layout.fillWidth: true }
                }

                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color:    _noteColor
                    text:     qsTr("Re-keying never needs an outage. Ask an organization admin to create a " +
                                   "second key in RotorLab under Organization, Integrations, Devices, " +
                                   "paste it here, then ask them to revoke the old one.")
                }
            }

            // ------------------------------------------------ 3. behaviour

            Card {
                title:   qsTr("Behavior")
                visible: RotorLabConnector.enabled && RotorLabConnector.configured

                QGCCheckBox {
                    text:      qsTr("File flights automatically when a flight ends")
                    checked:   RotorLabConnector.fileFlightsAutomatically
                    onClicked: RotorLabConnector.fileFlightsAutomatically = checked
                }
                QGCCheckBox {
                    text:      qsTr("Upload the flight log with the flight")
                    checked:   RotorLabConnector.uploadLogs
                    onClicked: RotorLabConnector.uploadLogs = checked
                }
                QGCCheckBox {
                    text:      qsTr("Ask who is flying at start-up")
                    checked:   RotorLabConnector.askWhoIsFlying
                    onClicked: RotorLabConnector.askWhoIsFlying = checked
                }

                // Change pilot (A79). Opens the crew screen at any time, also
                // in flight. It is non-modal and steps aside for the guided
                // confirm, so it never sits over a flight control.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: _pad * 2
                    QGCLabel {
                        Layout.preferredWidth: _keyWidth
                        wrapMode: Text.WordWrap
                        text:  RotorLabConnector.pilotWord
                        color: _noteColor
                    }
                    QGCLabel {
                        Layout.fillWidth: true
                        wrapMode:   Text.WordWrap
                        textFormat: Text.PlainText
                        text:       RotorLabConnector.pilotId >= 0
                                    ? qsTr("%1, on this station's flights until changed")
                                          .arg(RotorLabConnector.pilotName)
                                    : qsTr("No pilot named on this station's flights")
                        color:      RotorLabConnector.pilotId >= 0 ? qgcPal.text : _noteColor
                    }
                    QGCButton {
                        // "Change remote pilot in command", "Change remote
                        // pilot" or "Change pilot", in the regime's term.
                        text:      qsTr("Change %1").arg(RotorLabConnector.pilotWord.charAt(0).toLowerCase() +
                                                         RotorLabConnector.pilotWord.slice(1))
                        onClicked: RotorLabConnector.openCrewScreen()
                    }
                }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color:    _noteColor
                    // RotorLab 3.5.4 and later take the change as periods; an
                    // older RotorLab keeps one pilot per flight.
                    text:     RotorLabConnector.picSupported
                              ? qsTr("A change during a flight is filed with its time. The flight " +
                                     "names the pilot at takeoff.")
                              : qsTr("This RotorLab records one pilot per flight: a change during a " +
                                     "flight is filed under the pilot at takeoff.")
                }
            }

            // ------------------------------------------------ 4. compliance

            Card {
                title:   qsTr("How this operation files")
                visible: RotorLabConnector.enabled && RotorLabConnector.configured

                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color:    _noteColor
                    text:     qsTr("Sent with every flight so the record says which framework it was " +
                                   "flown under. Leave anything you are not sure of unset: nothing " +
                                   "checks these, so a wrong value is worse than none.")
                }

                GridLayout {
                    id:               jurisdictionRow
                    Layout.fillWidth: true
                    columns:          _narrow ? 1 : 3
                    columnSpacing:    _pad * 2
                    rowSpacing:       _pad / 2
                    QGCLabel {
                        Layout.fillWidth:      _narrow
                        Layout.preferredWidth: _narrow ? -1 : page.labelColumnWidth(jurisdictionRow.width)
                        wrapMode:              Text.WordWrap
                        text:                  qsTr("Jurisdiction")
                    }
                    QGCComboBox {
                        Layout.fillWidth:      true
                        Layout.minimumWidth:   ScreenTools.defaultFontPixelWidth * 8
                        Layout.maximumWidth:   page.fieldMaxWidthFor(jurisdictionRow.width, jurisdictionRow.columnSpacing * 2)
                        Layout.preferredWidth: Layout.maximumWidth
                        model: [qsTr("(not set)")].concat(RotorLabConnector.jurisdictions)
                        currentIndex: RotorLabConnector.jurisdiction === ""
                                      ? 0 : RotorLabConnector.jurisdictions.indexOf(RotorLabConnector.jurisdiction) + 1
                        onActivated: RotorLabConnector.jurisdiction =
                                     (index === 0 ? "" : RotorLabConnector.jurisdictions[index - 1])
                    }
                    Item { Layout.fillWidth: true; visible: !_narrow }
                }
                GridLayout {
                    id:               categoryRow
                    Layout.fillWidth: true
                    columns:          _narrow ? 1 : 3
                    columnSpacing:    _pad * 2
                    rowSpacing:       _pad / 2
                    QGCLabel {
                        Layout.fillWidth:      _narrow
                        Layout.preferredWidth: _narrow ? -1 : page.labelColumnWidth(categoryRow.width)
                        wrapMode:              Text.WordWrap
                        text:                  qsTr("Operating category")
                    }
                    QGCComboBox {
                        Layout.fillWidth:      true
                        Layout.minimumWidth:   ScreenTools.defaultFontPixelWidth * 8
                        Layout.maximumWidth:   page.fieldMaxWidthFor(categoryRow.width, categoryRow.columnSpacing * 2)
                        Layout.preferredWidth: Layout.maximumWidth
                        enabled: RotorLabConnector.operatingCategories.length > 0
                        model: [qsTr("(not set)")].concat(RotorLabConnector.operatingCategories)
                        currentIndex: RotorLabConnector.operatingCategory === ""
                                      ? 0 : RotorLabConnector.operatingCategories.indexOf(RotorLabConnector.operatingCategory) + 1
                        onActivated: RotorLabConnector.operatingCategory =
                                     (index === 0 ? "" : RotorLabConnector.operatingCategories[index - 1])
                    }
                    Item { Layout.fillWidth: true; visible: !_narrow }
                }
                GridLayout {
                    id:               visualModeRow
                    Layout.fillWidth: true
                    columns:          _narrow ? 1 : 3
                    columnSpacing:    _pad * 2
                    rowSpacing:       _pad / 2
                    QGCLabel {
                        Layout.fillWidth:      _narrow
                        Layout.preferredWidth: _narrow ? -1 : page.labelColumnWidth(visualModeRow.width)
                        wrapMode:              Text.WordWrap
                        text:                  qsTr("Visual mode")
                    }
                    QGCComboBox {
                        Layout.fillWidth:      true
                        Layout.minimumWidth:   ScreenTools.defaultFontPixelWidth * 8
                        Layout.maximumWidth:   page.fieldMaxWidthFor(visualModeRow.width, visualModeRow.columnSpacing * 2)
                        Layout.preferredWidth: Layout.maximumWidth
                        model: [qsTr("(not set)")].concat(RotorLabConnector.visualModes)
                        currentIndex: RotorLabConnector.visualMode === ""
                                      ? 0 : RotorLabConnector.visualModes.indexOf(RotorLabConnector.visualMode) + 1
                        onActivated: RotorLabConnector.visualMode =
                                     (index === 0 ? "" : RotorLabConnector.visualModes[index - 1])
                    }
                    Item { Layout.fillWidth: true; visible: !_narrow }
                }

                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color:    _noteColor
                    text:     qsTr("Whether anyone was overflown, and whether dangerous goods were " +
                                   "carried, are never sent from here. Nobody is asked them per " +
                                   "sortie, and leaving them unanswered is the honest record.")
                }
            }

            // ------------------------------------------------ 4a. checklists

            Card {
                title:   qsTr("Checklists")
                // Nothing at all when the org has published no templates: no
                // button, no empty list, and certainly not one nobody wrote.
                visible: RotorLabConnector.enabled && RotorLabConnector.configured &&
                         (RotorLabConnector.preflightTemplates.length > 0 ||
                          RotorLabConnector.postflightTemplates.length > 0)

                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: _noteColor
                    text: qsTr("The pre-flight is offered when an aircraft connects, before its first " +
                               "arm, and the post-flight after landing. Each offer can be ignored and " +
                               "never holds the aircraft. Any checklist can be run and filed from here " +
                               "at any time, also after an offer was dismissed.")
                }

                // A dismissed offer must not be the only route to a checklist.
                // A crew that tapped past the card should still be able to
                // record what they checked, whenever there is a spare hand.
                // Every published template of each kind, each with its own Run
                // now (A107), not only the first.
                Repeater {
                    model: [
                        { kind: "preflight",  label: qsTr("Pre-flight"),
                          list: RotorLabConnector.preflightTemplates },
                        { kind: "postflight", label: qsTr("Post-flight"),
                          list: RotorLabConnector.postflightTemplates }
                    ]
                    ColumnLayout {
                        id: kindGroup
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: ScreenTools.defaultFontPixelHeight / 4

                        RowLayout {
                            Layout.fillWidth: true
                            visible: modelData.list.length === 0
                            QGCLabel {
                                Layout.preferredWidth: _keyWidth
                                wrapMode: Text.WordWrap
                                text:  modelData.label
                                color: _noteColor
                            }
                            QGCLabel {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                text:  qsTr("none published")
                                color: _noteColor
                            }
                        }
                        Repeater {
                            model: modelData.list
                            RowLayout {
                                required property var modelData
                                required property int index
                                Layout.fillWidth: true
                                QGCLabel {
                                    Layout.preferredWidth: _keyWidth
                                    wrapMode: Text.WordWrap
                                    text:  index === 0 ? kindGroup.modelData.label : ""
                                    color: _noteColor
                                }
                                QGCLabel {
                                    Layout.fillWidth: true
                                    wrapMode:   Text.WordWrap
                                    textFormat: Text.PlainText
                                    text:       modelData.name
                                }
                                QGCButton {
                                    text:      qsTr("Run now")
                                    onClicked: root.runChecklist(kindGroup.modelData.kind, index)
                                }
                            }
                        }
                    }
                }
            }

            // ------------------------------------------------ 4b. missions

            Card {
                title:   qsTr("Missions from RotorLab")
                visible: RotorLabConnector.enabled && RotorLabConnector.configured

                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color:    _noteColor
                    text:     qsTr("Missions planned in RotorLab, opened here as an ordinary " +
                                   "plan. Editing and flying work exactly as they do for a plan " +
                                   "you built yourself. Nothing is sent back to RotorLab.")
                }

                RowLayout {
                    Layout.fillWidth: true
                    QGCLabel {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color:    _noteColor
                        visible:  RotorLabConnector.missions.length === 0
                        textFormat: Text.PlainText
                        // What the list is (A72): the missions published to the
                        // organization, narrowed to their author when a pilot is
                        // set on the crew screen. Nothing is assigned.
                        text:     RotorLabConnector.missionsEmptyText
                    }
                    QGCLabel {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color:    _noteColor
                        visible:  RotorLabConnector.missions.length > 0 &&
                                  RotorLabConnector.missionsPilotName !== ""
                        textFormat: Text.PlainText
                        text:     qsTr("Missions made by %1, the pilot on the crew screen.")
                                      .arg(RotorLabConnector.missionsPilotName)
                    }
                    Item { Layout.fillWidth: RotorLabConnector.missions.length > 0 }
                    QGCButton {
                        text:      qsTr("Refresh")
                        enabled:   !RotorLabConnector.missionsBusy
                        onClicked: RotorLabConnector.refreshMissions()
                    }
                }

                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible:  RotorLabConnector.missionsError !== ""
                    color:    _warnColor
                    textFormat: Text.PlainText
                    text:     RotorLabConnector.missionsError
                }

                Repeater {
                    model: RotorLabConnector.missions
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: ScreenTools.defaultFontPixelWidth
                        // Name and detail wrap rather than being cut off.
                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.minimumWidth: ScreenTools.defaultFontPixelWidth * 12
                            spacing: 0
                            QGCLabel {
                                Layout.fillWidth: true
                                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                                textFormat: Text.PlainText
                                text:  modelData.name
                            }
                            QGCLabel {
                                Layout.fillWidth: true
                                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                                color:   _noteColor
                                visible: modelData.detail !== ""
                                textFormat: Text.PlainText
                                text:    modelData.detail
                            }
                        }
                        QGCButton {
                            text:      qsTr("Open in Plan")
                            enabled:   !RotorLabConnector.missionsBusy
                            onClicked: RotorLabConnector.loadMission(modelData.id)
                        }
                    }
                }
            }

            // ------------------------------------------------ 5. the queue

            Card {
                title:   qsTr("Waiting to file")
                visible: RotorLabConnector.enabled && RotorLabConnector.configured

                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color:    _noteColor
                    visible:  RotorLabConnector.queuePending === 0 &&
                              RotorLabConnector.queueNeedsAttention === 0
                    text:     qsTr("Nothing waiting: everything this station recorded has been filed.")
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: RotorLabConnector.queuePending > 0
                    QGCLabel {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: RotorLabConnector.queueText + "  " + RotorLabConnector.queueLastAttempt
                    }
                    QGCButton {
                        text:      qsTr("Retry now")
                        onClicked: RotorLabConnector.retryQueueNow()
                    }
                }

                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color:    _noteColor
                    visible:  RotorLabConnector.queuePending > 0
                    text:     qsTr("A record is written to disk before the first attempt, so nothing " +
                                   "is lost if this computer is closed or loses power. Flying is " +
                                   "never held up by it.")
                }

                // About flights already filed: no pilot named, a voided
                // re-send. Kept until dismissed; never a gate.
                RowLayout {
                    Layout.fillWidth: true
                    visible: RotorLabConnector.filingNotice !== ""
                    QGCLabel {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color:    _warnColor
                        textFormat: Text.PlainText
                        text:     RotorLabConnector.filingNotice
                    }
                    QGCButton {
                        text:      qsTr("Dismiss")
                        onClicked: RotorLabConnector.dismissFilingNotice()
                    }
                }

                // The only category a human has to do something about.
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible:  RotorLabConnector.queueNeedsAttention > 0
                    color:    _warnColor
                    text:     qsTr("%n item(s) need attention. Each one says why. Flying is never " +
                                   "held up by them.", "", RotorLabConnector.queueNeedsAttention)
                }

                Repeater {
                    model: RotorLabConnector.queueModel
                    RowLayout {
                        Layout.fillWidth: true
                        visible: modelData.parked
                        spacing: ScreenTools.defaultFontPixelWidth
                        QGCLabel {
                            Layout.preferredWidth: ScreenTools.defaultFontPixelWidth * 12
                            wrapMode: Text.WordWrap
                            text:  modelData.kind
                        }
                        QGCLabel {
                            Layout.preferredWidth: ScreenTools.defaultFontPixelWidth * 16
                            wrapMode: Text.WordWrap
                            color: _noteColor
                            text:  modelData.when
                        }
                        // Why RotorLab refused it, in full (A77): wrapped,
                        // never cut off, so the crew can read what to fix. A long
                        // token with no spaces breaks anywhere rather than run out.
                        QGCLabel {
                            Layout.fillWidth: true
                            Layout.minimumWidth: ScreenTools.defaultFontPixelWidth * 12
                            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                            color: _warnColor
                            textFormat: Text.PlainText
                            text:  modelData.detail
                        }
                        QGCButton {
                            text:      qsTr("Try again")
                            onClicked: RotorLabConnector.retryQueueItem(modelData.id)
                        }
                        QGCButton {
                            text:      qsTr("Discard")
                            onClicked: root.confirmDiscard(modelData)
                        }
                    }
                }

                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color:    _noteColor
                    visible:  RotorLabConnector.queueNeedsAttention > 0
                    text:     qsTr("Try again sends a flight again under the same reference, adding any " +
                                   "pilot, aircraft or site it was missing. Values it already has are " +
                                   "kept, and it is never filed twice. Try again on a log that was not " +
                                   "collected collects it again.")
                }
            }
        }
    }

    // Both prompts go through the application's own dialog rather than a bare
    // QtQuick Dialog, which keeps the default light style and renders pale
    // body text on white over this dark page - present, but unreadable.
    function confirmSwitchFromFleet() {
        mainWindow.showMessageDialog(
            qsTr("Switch to filing directly to RotorLab?"),
            qsTr("This station currently reports through a RotorLab Server (Fleet Server role). Switching to the direct " +
                 "RotorLab Connector pauses telemetry and flight reporting to that server.\n\n" +
                 "Your enrollment on that server is kept, so switching back restores it with nothing " +
                 "to re-enter."),
            Dialog.Cancel | Dialog.Ok,
            function() { RotorLabConnector.enabled = true; enableBox.checked = true })
    }

    function confirmDiscard(item) {
        var withLog  = item.kind === "flight" && item.logs > 0
        var withRuns = item.kind === "flight" && item.checklists > 0
        var withAny  = withLog || withRuns
        var what = withLog && withRuns    ? qsTr("Discard this flight and its log and checklist?")
                 : withLog                ? qsTr("Discard this flight and its log?")
                 : withRuns               ? qsTr("Discard this flight and its checklist?")
                 : item.kind === "flight" ? qsTr("Discard this flight?")
                 : item.kind === "log"    ? qsTr("Discard this log?")
                                          : qsTr("Discard this checklist run?")
        var body = withAny
                 ? qsTr("They are removed from this computer and are not filed to RotorLab. This cannot be undone.")
                 : item.kind === "flight"
                 ? qsTr("It is removed from this computer and is not filed to RotorLab. This cannot be undone.")
                 : item.kind === "log"
                 ? qsTr("It is removed from the queue on this computer and is not sent to RotorLab. The " +
                        "flight itself is not affected. Attach the file in RotorLab by hand if you still " +
                        "want it there.")
                 : qsTr("It is removed from this computer and is not filed to RotorLab. This cannot be undone.")
        var id = item.id
        mainWindow.showMessageDialog(what, body, Dialog.Cancel | Dialog.Ok,
                                     function() { RotorLabConnector.forgetQueueItem(id) })
    }

    function confirmDisconnect() {
        mainWindow.showMessageDialog(
            qsTr("Disconnect from RotorLab?"),
            qsTr("Your Device Key and cached lists will be removed from this computer. Anything " +
                 "still waiting to file is kept. Reconnect and it will file.\n\n" +
                 "This does not revoke the key in RotorLab; an organization admin does that under " +
                 "Organization, Integrations, Devices."),
            Dialog.Cancel | Dialog.Ok,
            function() { RotorLabConnector.disconnectFromRotorLab() })
    }
}
