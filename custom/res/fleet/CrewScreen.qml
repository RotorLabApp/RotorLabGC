/****************************************************************************
 *
 * RotorLab GC Fleet Connector - the crew screen.
 *
 * Who is flying. Shared by both connectors (MainWindow.qml sets backend to
 * whichever is active). Fleet Connector: opened after first sync, on every
 * launch while a Fleet Server is connected, and on demand. RotorLab Connector:
 * opened after each successful key check, or offline with a cached roster,
 * while "Ask who is flying at start-up" is on, at the disarm when that check
 * lands while armed, and with Change pilot on the RotorLab Connector page at
 * any time, also in flight. It records the pilot, aircraft,
 * site, flight type, and crew for the next sortie.
 *
 * Two rules this screen exists to honor:
 *
 *   The pilot is NEVER prefilled from this station's memory. Not the last
 *   pilot, not the most frequent one, not the only one on the roster. A
 *   dialog that opens with a name in it is a dialog people tap through, and
 *   tapping through a prefilled one puts somebody else's name on a flight
 *   they did not fly - a false record in the system an agency uses for
 *   currency, incident review and liability. Tapping through an empty one
 *   leaves a gap, which is honest and can be filled in later by someone who
 *   was there. The one exception is the planned remote pilot of today's
 *   sortie on the chosen aircraft, as RotorLab's planner named it (approved
 *   PIC design): it is preselected, marked as planned, and the crew picks
 *   anyone else or skips.
 *
 *   It is dismissable in one action, with no confirmation, and is never modal
 *   over a flight control. A crew that launches the GC into a live call must
 *   be able to clear it and fly in under a second.
 *
 * Currency is shown and is never a gate. A lapsed pilot is selectable, flies,
 * and is recorded - because a crew with one qualified pilot on a bad day flies
 * anyway, and blocking the record only makes the record worse.
 *
 ****************************************************************************/

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls
import QGroundControl.Palette
import QGroundControl.ScreenTools

Popup {
    id:       root
    modal:    false          // never modal over a flight control
    focus:    true
    closePolicy: Popup.NoAutoClose
    // A fixed spot near the top of the window, centered across it, whatever
    // the content height. Centering on the window moved the dialog with every
    // note the aircraft carried, and its edge cut through the text behind it.
    parent:   Overlay.overlay
    x:        parent ? Math.round((parent.width - width) / 2) : 0
    y:        ScreenTools.defaultFontPixelHeight * 3
    margins:  ScreenTools.defaultFontPixelHeight
    // Explicit, and NOT derived from the content: a Popup whose width comes
    // from its child while the child's width comes from the Popup is a binding
    // loop, and Qt resolves it by collapsing the dialog to a sliver.
    //
    // While armed (Change pilot in flight) it also keeps clear of the fly
    // view's left tool strip (RTL, Land: 8 characters wide plus its margin),
    // on both sides since it is centered, so on a narrow tablet it never
    // covers a flight control.
    width:    Math.min(ScreenTools.defaultFontPixelWidth * 90,
                       Overlay.overlay ? Overlay.overlay.width * 0.85 : 900,
                       backend.anyArmed === true && Overlay.overlay
                           ? Overlay.overlay.width - _toolStripClearance * 2 : 100000)
    readonly property real _toolStripClearance: ScreenTools.defaultFontPixelWidth * 10
    padding:  0

    // Which connector this screen is collecting for. Exactly one is active at
    // a time, so the screen is shared rather than duplicated - the two present
    // the same members and the name resolves dynamically.
    property var    backend:     FleetConnector

    property int    _pilotId:    -1
    property string _pilotName:  ""
    property int    _assetId:    backend.assetId
    property string _site:       backend.site
    property string _flightType: backend.flightType
    property string _flightTypeOther: backend.flightTypeOther === undefined
                                      ? "" : backend.flightTypeOther
    property string _crew:       backend.crew
    property string _packs:      ""      // per sortie, like the pilot
    property string _handoffReason: ""
    // True once the crew picked a pilot by hand: the planned pilot of a
    // sortie that arrives later never overrides that pick.
    property bool   _pilotTouched: false

    // RotorLab 3.5.4 and later: the sortie's seats first, then every other
    // pilot. Older servers and the Fleet Connector: the roster as before.
    readonly property var _pilotModel: backend.crewPilots !== undefined
                                       ? backend.crewPilots : backend.pilots
    readonly property int _plannedPic: backend.plannedPicId === undefined
                                       ? -1 : backend.plannedPicId
    on_PlannedPicChanged: _applyPlanned()
    on_AssetIdChanged:    _loadSortie()

    function _loadSortie() {
        if (typeof backend.loadSortie === "function") {
            backend.loadSortie(_assetId)
        }
    }

    function _applyPlanned() {
        if (_pilotTouched || _plannedPic < 0) {
            return
        }
        for (var i = 0; i < _pilotModel.length; i++) {
            if (_pilotModel[i].id === _plannedPic) {
                _pilotId = _plannedPic
                _pilotName = _pilotModel[i].name
                return
            }
        }
    }

    // Never over a flight control. While QGC's guided-action confirm (the arm,
    // takeoff, RTL... slider at the top of the fly view) is on screen, this
    // screen steps out of the way, keeping what was picked, and comes back
    // when the confirm closes. Matters most for Change pilot, which can open
    // in flight.
    readonly property bool _confirmShowing: typeof mainWindow !== "undefined" && mainWindow !== null &&
                                            mainWindow._guidedConfirmShowing === true
    property bool _hiddenForConfirm: false
    property bool _resuming:         false

    on_ConfirmShowingChanged: {
        if (_confirmShowing && opened) {
            _hiddenForConfirm = true
            close()
        } else if (!_confirmShowing && _hiddenForConfirm) {
            _hiddenForConfirm = false
            _resuming = true
            open()
        }
    }

    // The pilot opens empty every single time - including after a dismissal.
    onOpened: {
        if (_resuming) {
            _resuming = false
            return
        }
        _pilotId = -1
        _pilotName = ""
        _packs = ""
        _handoffReason = ""
        _pilotTouched = false
        // The aircraft already set, so Change pilot reopens on it and its
        // sortie's seats show again. Never reset to "(not set)" on reopen.
        _assetId = backend.assetId
        _loadSortie()
        _applyPlanned()
        _flightTypeOther = backend.flightTypeOther === undefined
                         ? "" : backend.flightTypeOther
        if (_confirmShowing) {
            _hiddenForConfirm = true
            close()
        }
    }

    // Closed without an answer (Cancel, or "change" for the filing defaults):
    // tell the backend, so the screen can be opened again and the station's
    // cards come back. Nothing about the pilot changes.
    onClosed: {
        if (!_hiddenForConfirm && backend.crewScreenPending === true &&
                typeof backend.closeCrewScreen === "function") {
            backend.closeCrewScreen()
        }
    }

    function _wants(field) { return backend.promptFields.indexOf(field) >= 0 }

    /// The selected aircraft's row from the backend's asset list, or null.
    /// A Fleet Server backend supplies no readiness fields, and the callers
    /// above check for that rather than inventing a state it never reported.
    function _assetRow() {
        if (_assetId < 0 || backend.assets === undefined) return null
        for (var i = 0; i < backend.assets.length; i++) {
            if (backend.assets[i].id === _assetId) return backend.assets[i]
        }
        return null
    }

    /// RotorLab's note on the chosen aircraft, already in RotorLab's own words
    /// ("RotorLab lists this aircraft as Limits on record: ..."), or "". The
    /// RotorLab roster builds it for every state but go (RotorLabRoster::
    /// readinessFor). A Fleet Server row has no note, so nothing shows.
    function _readinessNote() {
        var row = _assetRow()
        if (row === null || row.note === undefined || row.note === null) return ""
        return row.note
    }

    function _readinessState() {
        var row = _assetRow()
        if (row === null || row.state === undefined || row.state === null) return ""
        return row.state
    }

    /// RotorLab's own word for the state, for the tag in front of the note.
    function _readinessWord() {
        var row = _assetRow()
        if (row !== null && row.readiness !== undefined && row.readiness !== null
                && row.readiness !== "") {
            return row.readiness
        }
        var st = _readinessState()
        return st === "no_go"   ? qsTr("No go")
             : st === "limits"  ? qsTr("Limits on record")
             : st === "unknown" ? qsTr("Condition unknown")
                                : ""
    }

    /// A go aircraft with nothing on record gets a neutral line, so it reads
    /// differently from an aircraft RotorLab said nothing about.
    function _showNoFlags() {
        return _readinessState() === "go" && _readinessNote() === ""
               && _openWork().length === 0
    }

    readonly property bool  _lightTheme:       qgcPal.globalTheme === QGCPalette.Light
    // Condition unknown is its own grey-blue, not the orange of Limits on record.
    readonly property color _unknownColor:     _lightTheme ? "#4a6584" : "#8aa4c8"
    // Aircraft No go is a different red from the pilot-currency red, and carries a tag.
    readonly property color _noGoColor:        _lightTheme ? "#a3122a" : "#ff5c6c"
    // Placeholders: readable on the text field in both themes (about 5:1).
    readonly property color _placeholderColor: _lightTheme ? "#5b6573" : "#8b97a6"
    readonly property color _tagTextColor:     _lightTheme ? "#ffffff" : "#0d1117"

    function _stateColor() {
        var st = _readinessState()
        return st === "no_go"    ? _noGoColor
             : st === "unknown"  ? _unknownColor
             : st === "advisory" ? qgcPal.text
                                 : _warnColor
    }

    /// Whether the advisory row is on screen at all. A GridLayout SKIPS
    /// invisible children, so the spacer and the advisory must appear and
    /// disappear together - otherwise the row consumes one cell instead of two
    /// and every label below it lands in the wrong column.
    function _showReadiness() {
        return _readinessNote() !== ""
               || _openWork().length > 0
               || _regExpiry() !== ""
               || _showNoFlags()
    }

    function _openWork() {
        var row = _assetRow()
        if (row === null || row.work === undefined) return []
        return row.work
    }

    function _regExpiry() {
        var row = _assetRow()
        if (row === null || row.registrationExpires === undefined) return ""
        return row.registrationExpires
    }

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    // Notes, captions and status lines stay readable in both themes, at
    // least 4.5:1 on the card. The stock gray, orange and green are too pale
    // on the light card, and the stock orange on the dark one.
    readonly property color _noteColor:  _lightTheme ? "#39414b" : "#d4d4d4"   // same as RLPageLayout.noteColor
    readonly property color _warnColor:  _lightTheme ? "#8a4a00" : "#e89020"

    background: Rectangle {
        color:        qgcPal.window
        border.color: qgcPal.groupBorder
        border.width: 1
        radius:       ScreenTools.defaultFontPixelHeight / 2
    }

    contentItem: ColumnLayout {
        spacing: ScreenTools.defaultFontPixelHeight / 2

        // ---- header
        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins:   ScreenTools.defaultFontPixelWidth * 2
            spacing:          ScreenTools.defaultFontPixelHeight / 4
            QGCLabel {
                text: qsTr("Who is flying?")
                font.bold: true
                font.pointSize: ScreenTools.defaultFontPointSize + 3
            }
            QGCLabel {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: _noteColor
                // RotorLab 3.5.2 and later file a flight with no pilot as
                // "Pilot not named" and list it for naming (A54). An older or
                // not yet verified server does not, so the copy then says only
                // what this station does. The Fleet text is kept.
                text: backend !== RotorLabConnector
                      ? qsTr("Recorded against this sortie's flights. You can skip this and fly: " +
                             "the flight is still logged, with no pilot named on it.")
                      : RotorLabConnector.serverNamesPilots
                      ? qsTr("Recorded against this sortie's flights. Skipping this never holds up " +
                             "the aircraft, but the flight then files marked \"Pilot not named\", and " +
                             "its record is incomplete until the pilot is named on it in RotorLab.")
                      : qsTr("Recorded against this sortie's flights. Skipping this never holds up " +
                             "the aircraft. The flight then files with no pilot named by this station.")
            }
        }

        // ---- pilot
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin:  ScreenTools.defaultFontPixelWidth * 2
            Layout.rightMargin: ScreenTools.defaultFontPixelWidth * 2
            spacing: ScreenTools.defaultFontPixelHeight / 4
            visible: _wants("pilot_id")

            RowLayout {
                Layout.fillWidth: true
                QGCLabel {
                    text: backend.pilotWord !== undefined ? backend.pilotWord : qsTr("Pilot")
                    font.bold: true
                }
                Item { Layout.fillWidth: true }
                QGCLabel { text: backend.rosterSourceText; textFormat: Text.PlainText; color: _noteColor }
            }
            // The sortie RotorLab plans on this aircraft today, whose crew is
            // listed first. RotorLab does not designate anyone; the crew does.
            QGCLabel {
                Layout.fillWidth: true
                wrapMode:   Text.WordWrap
                color:      _noteColor
                textFormat: Text.PlainText
                visible:    backend.sortieText !== undefined && backend.sortieText !== ""
                text:       visible ? qsTr("Sortie: %1. Its crew is listed first; anyone else stays selectable.")
                                          .arg(backend.sortieText) : ""
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: ScreenTools.defaultFontPixelHeight * 11
                color:        qgcPal.windowShade
                border.color: qgcPal.groupBorder
                border.width: 1
                radius:       4

                QGCLabel {
                    anchors.centerIn: parent
                    visible: _pilotModel.length === 0
                    color:   _noteColor
                    text:    backend.rosterLoaded
                             ? qsTr("No pilots on the roster")
                             : qsTr("Could not load the roster. Tap to retry")
                    MouseArea { anchors.fill: parent; onClicked: backend.refreshRoster() }
                }

                ListView {
                    id: pilotList
                    anchors.fill:    parent
                    anchors.margins: 2
                    clip:            true
                    model:           _pilotModel
                    delegate: Rectangle {
                        required property var modelData
                        width:  pilotList.width
                        height: ScreenTools.defaultFontPixelHeight * 2.4
                        color:  modelData.id === _pilotId ? qgcPal.buttonHighlight : "transparent"
                        RowLayout {
                            anchors.fill:        parent
                            anchors.leftMargin:  ScreenTools.defaultFontPixelWidth
                            anchors.rightMargin: ScreenTools.defaultFontPixelWidth
                            QGCLabel {
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                textFormat: Text.PlainText
                                text:  modelData.name
                                color: modelData.id === _pilotId ? qgcPal.buttonHighlightText : qgcPal.text
                            }
                            // The seat on today's sortie, in RotorLab's words,
                            // and whether the planner named this person as the
                            // remote pilot. Information only.
                            QGCLabel {
                                visible:    modelData.seat === true && modelData.dutyLabel !== undefined
                                            && modelData.dutyLabel !== ""
                                textFormat: Text.PlainText
                                color:      modelData.id === _pilotId ? qgcPal.buttonHighlightText : _noteColor
                                text:       visible
                                            ? (modelData.plannedPic === true
                                               ? qsTr("%1, planned").arg(modelData.dutyLabel)
                                               : modelData.dutyLabel)
                                            : ""
                            }
                            // Currency is information, never a gate. Every row
                            // stays selectable whatever this says.
                            QGCLabel {
                                visible: modelData.currency === "due_soon"
                                text:    qsTr("currency due soon")
                                color:   _warnColor
                            }
                            QGCLabel {
                                visible: modelData.currency === "lapsed"
                                textFormat: Text.PlainText
                                text:    modelData.lapsed.length > 0
                                         ? qsTr("currency lapsed: ") + modelData.lapsed.join(", ")
                                         : qsTr("currency lapsed")
                                color:   qgcPal.colorRed
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                _pilotTouched = true
                                _pilotId = modelData.id
                                _pilotName = modelData.name
                            }
                        }
                    }
                }
            }
        }

        // ---- aircraft, site, flight type, crew
        GridLayout {
            Layout.fillWidth:   true
            Layout.leftMargin:  ScreenTools.defaultFontPixelWidth * 2
            Layout.rightMargin: ScreenTools.defaultFontPixelWidth * 2
            columns:      2
            columnSpacing: ScreenTools.defaultFontPixelWidth * 2
            rowSpacing:    ScreenTools.defaultFontPixelHeight / 3

            QGCLabel { text: qsTr("Aircraft"); visible: _wants("asset_id") }
            QGCComboBox {
                visible: _wants("asset_id")
                Layout.fillWidth: true
                model: {
                    var out = [qsTr("(not set)")]
                    for (var i = 0; i < backend.assets.length; i++) {
                        out.push(backend.assets[i].name)
                    }
                    return out
                }
                // Bound, not set once: the backend is assigned after this is
                // created and the roster refreshes every 15 minutes, and a
                // one-time pick showed "(not set)" on reopen while an aircraft
                // was set.
                currentIndex: {
                    for (var i = 0; i < backend.assets.length; i++) {
                        if (backend.assets[i].id === _assetId) return i + 1
                    }
                    return 0
                }
                onActivated: _assetId = currentIndex === 0 ? -1 : backend.assets[currentIndex - 1].id
            }

            // What RotorLab says about the chosen airframe. SHOWN, NEVER A
            // GATE: the aircraft is still selectable, Start flying is still
            // enabled, and the flight still files. The crew read the reason and
            // decide - a ground station acting on a cached answer that may be
            // hours old has no business grounding anything.
            //
            // Empty for a Fleet Server backend, which does not report this, so
            // both connectors share the screen without either pretending.
            Item { visible: _showReadiness() }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                visible: _showReadiness()
                // One line per RotorLab state, in RotorLab's own word, never
                // relabeled. Shown only when RotorLab sent a note: a
                // registration date alone never brings up a state line.
                // The server spells the worst state no_go, with an underscore.
                // The tag names the state in RotorLab's word, so a state never
                // rests on color alone. Advisory carries no tag.
                Rectangle {
                    visible: _readinessNote() !== "" && _readinessState() !== "advisory"
                             && _readinessWord() !== ""
                    color:   _stateColor()
                    radius:  3
                    implicitWidth:  stateTag.implicitWidth + ScreenTools.defaultFontPixelWidth * 1.5
                    implicitHeight: stateTag.implicitHeight + ScreenTools.defaultFontPixelHeight * 0.2
                    Layout.bottomMargin: ScreenTools.defaultFontPixelHeight * 0.2
                    QGCLabel {
                        id: stateTag
                        anchors.centerIn: parent
                        font.bold:  true
                        color:      _tagTextColor
                        textFormat: Text.PlainText
                        text:       _readinessWord()
                    }
                }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible:  _readinessNote() !== ""
                    color:    _stateColor()
                    textFormat: Text.PlainText
                    text:     _readinessNote()
                }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible:  _showNoFlags()
                    color:    _noteColor
                    text:     qsTr("No flags on record in RotorLab")
                }

                // The work itself, worst first, each line built by the RotorLab
                // roster ("Open grounding work order: Replace port-aft motor
                // bearing"). "grounded by open work" tells a crew nothing they
                // can act on; the title does.
                // Grounding work stands out by more than color: a left bar
                // and bold text.
                Repeater {
                    model: _openWork()
                    RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: modelData.grounding ? ScreenTools.defaultFontPixelWidth : 0
                        Rectangle {
                            visible: modelData.grounding === true
                            Layout.fillHeight:     true
                            Layout.preferredWidth: 4
                            color: _noGoColor
                        }
                        QGCLabel {
                            Layout.fillWidth: true
                            wrapMode:  Text.WordWrap
                            font.bold: modelData.grounding === true
                            color:    modelData.grounding                 ? _noGoColor
                                    : modelData.overdue                   ? _warnColor
                                    : modelData.priority === "urgent"     ? _warnColor
                                    : modelData.dueSoon                   ? _warnColor
                                                                          : _noteColor
                            textFormat: Text.PlainText
                            text:     modelData.line !== undefined ? modelData.line : modelData.text
                        }
                    }
                }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible:  _regExpiry() !== ""
                    color:    _warnColor
                    textFormat: Text.PlainText
                    text:     qsTr("Registration expires %1").arg(_regExpiry())
                }
            }

            QGCLabel { text: qsTr("Site"); visible: _wants("site") }
            QGCTextField {
                visible: _wants("site")
                Layout.fillWidth: true
                text: _site
                placeholderText: qsTr("Northgate")
                placeholderTextColor: _placeholderColor
                onTextChanged: _site = text
            }

            QGCLabel { text: qsTr("Flight type"); visible: _wants("flight_type") }
            QGCComboBox {
                visible: _wants("flight_type")
                Layout.fillWidth: true
                // Always a supported list, never a free-text field.
                // Labels where the backend has them; the codes are what is set.
                model: [qsTr("(not set)")].concat(backend.flightTypeLabels !== undefined
                                                  ? backend.flightTypeLabels : backend.flightTypes)
                Component.onCompleted: {
                    var i = backend.flightTypes.indexOf(_flightType)
                    currentIndex = i >= 0 ? i + 1 : 0
                }
                onActivated: _flightType = currentIndex === 0 ? "" : backend.flightTypes[currentIndex - 1]
            }

            QGCLabel {
                text: qsTr("Other flight type")
                visible: _wants("flight_type_other") && _flightType === "other"
            }
            QGCTextField {
                visible: _wants("flight_type_other") && _flightType === "other"
                Layout.fillWidth: true
                text: _flightTypeOther
                maximumLength: 80
                placeholderText: qsTr("Describe the flight type")
                placeholderTextColor: _placeholderColor
                onTextChanged: _flightTypeOther = text
            }

            QGCLabel { text: qsTr("Crew"); visible: _wants("crew") }
            QGCTextField {
                visible: _wants("crew")
                Layout.fillWidth: true
                text: _crew
                placeholderText: qsTr("PIC + VO")
                placeholderTextColor: _placeholderColor
                onTextChanged: _crew = text
            }

            // One line a barcode scanner can type into. Up to 12 serials.
            QGCLabel { text: qsTr("Packs"); visible: _wants("battery_serials") }
            QGCTextField {
                visible: _wants("battery_serials")
                Layout.fillWidth: true
                text: _packs
                placeholderText: qsTr("Scan or type serials, commas or spaces")
                placeholderTextColor: _placeholderColor
                onTextChanged: _packs = text
            }
            Item { visible: _wants("battery_serials") }
            QGCLabel {
                visible: _wants("battery_serials")
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: _noteColor
                text: qsTr("Up to 12 packs. They stay set for this sortie's flights until " +
                           "this screen is opened again.")
            }
        }

        // ---- how this flight will be filed
        //
        // Read-only, one line, and deliberately not a picker. It exists so a
        // default chosen in January is not silently stamped onto a June sortie
        // that was flown differently - the same failure as pre-filling the
        // pilot, just slower to notice. Nothing here is required and nothing
        // blocks the crew.
        RowLayout {
            Layout.fillWidth:   true
            Layout.leftMargin:  ScreenTools.defaultFontPixelWidth * 2
            Layout.rightMargin: ScreenTools.defaultFontPixelWidth * 2
            visible: backend.filingAsText !== undefined && backend.filingAsText !== ""
            QGCLabel {
                color: _noteColor
                text:  qsTr("Filing as:")
            }
            QGCLabel {
                Layout.fillWidth: true
                textFormat: Text.PlainText
                text:  backend.filingAsText === undefined ? "" : backend.filingAsText
            }
            QGCLabel {
                text:  qsTr("change")
                color: qgcPal.colorBlue
                MouseArea {
                    anchors.fill: parent
                    cursorShape:  Qt.PointingHandCursor
                    onClicked: {
                        root.close()
                        mainWindow.showSettingsTool()
                    }
                }
            }
        }

        // ---- why the pilot is changing (optional, kept in the handoff log)
        ColumnLayout {
            Layout.fillWidth:   true
            Layout.leftMargin:  ScreenTools.defaultFontPixelWidth * 2
            Layout.rightMargin: ScreenTools.defaultFontPixelWidth * 2
            spacing: ScreenTools.defaultFontPixelHeight / 4
            visible: typeof backend.setHandoffReason === "function" && backend.pilotId >= 0
                     && _pilotId !== backend.pilotId
            QGCLabel {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                text: qsTr("Changing from %1. Reason for the change").arg(backend.pilotName)
            }
            QGCTextField {
                Layout.fillWidth: true
                text: _handoffReason
                maximumLength: 200
                placeholderText: qsTr("Optional, for example Battery swap. No health or personal details.")
                placeholderTextColor: _placeholderColor
                onTextChanged: _handoffReason = text
            }
        }

        // ---- actions
        RowLayout {
            Layout.fillWidth: true
            Layout.margins:   ScreenTools.defaultFontPixelWidth * 2
            spacing:          ScreenTools.defaultFontPixelWidth

            // One action, always available, no confirmation.
            // With a pilot already named, Skip would only close the screen,
            // so it is not offered; Cancel below does that. Fleet backends
            // keep Skip as before.
            QGCButton {
                text: qsTr("Skip")
                visible: !(backend.pilotId >= 0 && typeof backend.closeCrewScreen === "function"
                           && backend === RotorLabConnector)
                onClicked: {
                    if (typeof backend.setHandoffReason === "function") {
                        backend.setHandoffReason(_handoffReason)
                    }
                    backend.dismissCrewScreen()
                    root.close()
                }
            }
            // Opened with Change pilot: leave everything as it was.
            QGCButton {
                text:    qsTr("Cancel")
                visible: typeof backend.closeCrewScreen === "function" &&
                         (backend.crewScreenOnDemand === true ||
                          (backend === RotorLabConnector && backend.pilotId >= 0))
                onClicked: { backend.closeCrewScreen(); root.close() }
            }
            Item { Layout.fillWidth: true }
            QGCButton {
                // Mid-flight there is nothing to start: the pilot is set.
                text:    backend.anyArmed === true ? qsTr("Set pilot") : qsTr("Start flying")
                primary: true
                onClicked: {
                    if (typeof backend.setHandoffReason === "function") {
                        backend.setHandoffReason(_handoffReason)
                    }
                    backend.setCrew(_pilotId, _pilotName, _assetId,
                                           _site, _flightType, _crew)
                    if (typeof backend.setFlightTypeOther === "function") {
                        backend.setFlightTypeOther(_flightTypeOther)
                    }
                    if (typeof backend.setPackSerials === "function") {
                        backend.setPackSerials(_packs)
                    }
                    root.close()
                }
            }
        }
    }
}
