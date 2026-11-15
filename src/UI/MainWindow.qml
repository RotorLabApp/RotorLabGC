/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Window

import QGroundControl
import QGroundControl.Palette
import QGroundControl.Controls
import QGroundControl.FactControls
import QGroundControl.ScreenTools
import QGroundControl.FlightDisplay
import QGroundControl.FlightMap

import QGroundControl.UTMSP

/// @brief Native QML top level window
/// All properties defined here are visible to all QML pages.
ApplicationWindow {
    id:             mainWindow
    visible:        true

    property bool   _utmspSendActTrigger
    property bool   _utmspStartTelemetry

    Component.onCompleted: {
        // RotorLab GC: hold first-run prompts behind the license gate. They
        // would otherwise render in the dialog overlay layer on top of the
        // gate. Start them only once the app is activated (immediately if it
        // already is; otherwise when activation completes, see Connections).
        if (LicenseManager.activated) {
            firstRunPromptManager.nextPrompt()
        }
    }

    // RotorLab GC: kick off the first-run prompt sequence the moment the app
    // becomes activated (fires once; the gate hides itself on the same signal).
    Connections {
        target: LicenseManager
        function onActivatedChanged() {
            if (LicenseManager.activated && firstRunPromptManager.nextPromptIdIndex === 0) {
                firstRunPromptManager.nextPrompt()
            }
        }
    }

    /// Saves main window position and size and re-opens it in the same position and size next time
    MainWindowSavedState {
        window: mainWindow
    }

    QtObject {
        id: firstRunPromptManager

        property var currentDialog:     null
        property var rgPromptIds:       QGroundControl.corePlugin.firstRunPromptsToShow()
        property int nextPromptIdIndex: 0

        function clearNextPromptSignal() {
            if (currentDialog) {
                currentDialog.closed.disconnect(nextPrompt)
            }
        }

        function nextPrompt() {
            if (nextPromptIdIndex < rgPromptIds.length) {
                var component = Qt.createComponent(QGroundControl.corePlugin.firstRunPromptResource(rgPromptIds[nextPromptIdIndex]));
                currentDialog = component.createObject(mainWindow)
                currentDialog.closed.connect(nextPrompt)
                currentDialog.open()
                nextPromptIdIndex++
            } else {
                currentDialog = null
                showPreFlightChecklistIfNeeded()
            }
        }
    }

    readonly property real      _topBottomMargins:          ScreenTools.defaultFontPixelHeight * 0.5

    //-------------------------------------------------------------------------
    //-- Global Scope Variables

    QtObject {
        id: globals

        readonly property var       activeVehicle:                  QGroundControl.multiVehicleManager.activeVehicle
        readonly property real      defaultTextHeight:              ScreenTools.defaultFontPixelHeight
        readonly property real      defaultTextWidth:               ScreenTools.defaultFontPixelWidth
        readonly property var       planMasterControllerFlyView:    flyView.planController
        readonly property var       guidedControllerFlyView:        flyView.guidedController

        // Number of QGCTextField's with validation errors. Used to prevent closing panels with validation errors.
        property int                validationErrorCount:           0 

        // Property to manage RemoteID quick access to settings page
        property bool               commingFromRIDIndicator:        false
    }

    /// Default color palette used throughout the UI
    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    //-------------------------------------------------------------------------
    //-- Actions

    signal armVehicleRequest
    signal forceArmVehicleRequest
    signal disarmVehicleRequest
    signal vtolTransitionToFwdFlightRequest
    signal vtolTransitionToMRFlightRequest
    signal showPreFlightChecklistIfNeeded

    //-------------------------------------------------------------------------
    //-- Global Scope Functions

    // This function is used to prevent view switching if there are validation errors
    function allowViewSwitch(previousValidationErrorCount = 0) {
        // Run validation on active focus control to ensure it is valid before switching views
        if (mainWindow.activeFocusControl instanceof FactTextField) {
            mainWindow.activeFocusControl._onEditingFinished()
        }
        return globals.validationErrorCount <= previousValidationErrorCount
    }

    function showPlanView() {
        flyView.visible = false
        planView.visible = true
    }

    function showFlyView() {
        flyView.visible = true
        planView.visible = false
    }

    function showTool(toolTitle, toolSource, toolIcon) {
        toolDrawer.backIcon     = flyView.visible ? "/qmlimages/PaperPlane.svg" : "/qmlimages/Plan.svg"
        toolDrawer.toolTitle    = toolTitle
        toolDrawer.toolSource   = toolSource
        toolDrawer.toolIcon     = toolIcon
        toolDrawer.visible      = true
    }

    function showAnalyzeTool() {
        showTool(qsTr("Analyze Tools"), "qrc:/qml/QGroundControl/AnalyzeView/AnalyzeView.qml", "/qmlimages/Analyze.svg")
    }

    function showVehicleConfig() {
        showTool(qsTr("Vehicle Configuration"), "qrc:/qml/QGroundControl/VehicleSetup/SetupView.qml", "/qmlimages/Gears.svg")
    }

    function showVehicleConfigParametersPage() {
        showVehicleConfig()
        toolDrawerLoader.item.showParametersPanel()
    }

    function showKnownVehicleComponentConfigPage(knownVehicleComponent) {
        showVehicleConfig()
        let vehicleComponent = globals.activeVehicle.autopilotPlugin.findKnownVehicleComponent(knownVehicleComponent)
        if (vehicleComponent) {
            toolDrawerLoader.item.showVehicleComponentPanel(vehicleComponent)
        }
    }

    function showSettingsTool(settingsPage = "") {
        showTool(qsTr("Application Settings"), "qrc:/qml/QGroundControl/Controls/AppSettings.qml", "/res/QGCLogoWhite")
        if (settingsPage !== "") {
            toolDrawerLoader.item.showSettingsPage(settingsPage)
        }
    }

    //-------------------------------------------------------------------------
    //-- Global simple message dialog

    function showMessageDialog(dialogTitle, dialogText, buttons = Dialog.Ok, acceptFunction = null, closeFunction = null) {
        simpleMessageDialogComponent.createObject(mainWindow, { title: dialogTitle, text: dialogText, buttons: buttons, acceptFunction: acceptFunction, closeFunction: closeFunction }).open()
    }

    // This variant is only meant to be called by QGCApplication
    function _showMessageDialog(dialogTitle, dialogText) {
        showMessageDialog(dialogTitle, dialogText)
    }

    Component {
        id: simpleMessageDialogComponent

        QGCSimpleMessageDialog {
        }
    }

    property bool _forceClose: false

    function finishCloseProcess() {
        _forceClose = true
        // For some reason on the Qml side Qt doesn't automatically disconnect a signal when an object is destroyed.
        // So we have to do it ourselves otherwise the signal flows through on app shutdown to an object which no longer exists.
        firstRunPromptManager.clearNextPromptSignal()
        QGroundControl.linkManager.shutdown()
        QGroundControl.videoManager.stopVideo();
        mainWindow.close()
    }

    // Check for things which should prevent the app from closing
    //  Returns true if it is OK to close
    readonly property int _skipUnsavedMissionCheckMask: 0x01
    readonly property int _skipPendingParameterWritesCheckMask: 0x02
    readonly property int _skipActiveConnectionsCheckMask: 0x04
    property int _closeChecksToSkip: 0
    function performCloseChecks() {
        if (!(_closeChecksToSkip & _skipUnsavedMissionCheckMask) && !checkForUnsavedMission()) {
            return false
        }
        if (!(_closeChecksToSkip & _skipPendingParameterWritesCheckMask) && !checkForPendingParameterWrites()) {
            return false
        }
        if (!(_closeChecksToSkip & _skipActiveConnectionsCheckMask) && !checkForActiveConnections()) {
            return false
        }
        finishCloseProcess()
        return true
    }

    property string closeDialogTitle: qsTr("Close %1").arg(QGroundControl.appName)

    function checkForUnsavedMission() {
        if (planView._planMasterController.dirty) {
            showMessageDialog(closeDialogTitle,
                              qsTr("You have a mission edit in progress which has not been saved/sent. If you close you will lose changes. Are you sure you want to close?"),
                              Dialog.Yes | Dialog.No,
                              function() { _closeChecksToSkip |= _skipUnsavedMissionCheckMask; performCloseChecks() })
            return false
        } else {
            return true
        }
    }

    function checkForPendingParameterWrites() {
        for (var index=0; index<QGroundControl.multiVehicleManager.vehicles.count; index++) {
            if (QGroundControl.multiVehicleManager.vehicles.get(index).parameterManager.pendingWrites) {
                mainWindow.showMessageDialog(closeDialogTitle,
                    qsTr("You have pending parameter updates to a vehicle. If you close you will lose changes. Are you sure you want to close?"),
                    Dialog.Yes | Dialog.No,
                    function() { _closeChecksToSkip |= _skipPendingParameterWritesCheckMask; performCloseChecks() })
                return false
            }
        }
        return true
    }

    function checkForActiveConnections() {
        if (QGroundControl.multiVehicleManager.activeVehicle) {
            mainWindow.showMessageDialog(closeDialogTitle,
                qsTr("There are still active connections to vehicles. Are you sure you want to exit?"),
                Dialog.Yes | Dialog.No,
                function() { _closeChecksToSkip |= _skipActiveConnectionsCheckMask; performCloseChecks() })
            return false
        } else {
            return true
        }
    }

    onClosing: (close) => {
        if (!_forceClose) {
            _closeChecksToSkip = 0
            close.accepted = performCloseChecks()
        }
    }

    background: Rectangle {
        anchors.fill:   parent
        color:          QGroundControl.globalPalette.window
    }

    FlyView { 
        id:                     flyView
        anchors.fill:           parent
        utmspSendActTrigger:    _utmspSendActTrigger
    }

    PlanView {
        id:             planView
        anchors.fill:   parent
        visible:        false
    }

    // RotorLab GC: startup license gate. A modal overlay that blocks the app
    // until this device is activated. Loaded from the custom plugin resources.
    Loader {
        anchors.fill:   parent
        z:              10000
        active:         true
        source:         "qrc:/custom/license/LicenseGate.qml"
    }

    // RotorLab GC: whichever connector is filing this station's flights.
    //
    // Exactly one is active at a time - the RotorLab Connector hides the Fleet
    // Connector when it is enabled - so the crew screen and the checklist
    // runner are shared rather than duplicated, and everything below binds
    // through this one property instead of naming a connector three times.
    readonly property var activeConnector:
        RotorLabConnector.enabled ? RotorLabConnector : FleetConnector

    // RotorLab GC: true while QGC's guided-action confirm (the arm, takeoff,
    // RTL... slider at the top of the fly view) is on screen. Every RotorLab
    // card and alert hides then, so nothing of ours can sit over the slider or
    // catch a slide meant for it. They come back when the confirm closes.
    readonly property bool _guidedConfirmShowing:
        flyView.guidedController !== undefined && flyView.guidedController !== null &&
        flyView.guidedController.guidedUIVisible === true

    // RotorLab GC: true while the fly view is what the operator sees: not the
    // plan view, and no tool (Application Settings, Vehicle Configuration,
    // Analyze) is open over it.
    readonly property bool _flyViewShowing: flyView.visible && !toolDrawer.visible

    // RotorLab GC: a mission pulled from RotorLab.app is on disk and ready to
    // open. Loading a plan REPLACES whatever is in the Plan view, so a crew
    // that has planning in progress is asked first - losing unsaved waypoints
    // to a background download would be unforgivable, and the download itself
    // is already safely on disk either way.
    Connections {
        target: RotorLabConnector
        function onMissionPlanReady(filePath, missionName) {
            var open = function() {
                // Close App Settings first. The mission is opened from a
                // settings page, and the Plan view is BEHIND that overlay -
                // without this the plan loads out of sight and the button
                // looks like it did nothing.
                toolDrawer.visible = false
                mainWindow.showPlanView()
                planView._planMasterController.loadFromFile(filePath)
            }
            if (planView._planMasterController.dirty) {
                mainWindow.showMessageDialog(
                    qsTr("Open \"%1\"?").arg(missionName),
                    qsTr("You have a mission edit in progress that has not been saved or sent. " +
                         "Opening this mission replaces it.

" +
                         "The downloaded mission is saved on this computer either way, so you " +
                         "can finish what you are doing and open it from the Plan view later."),
                    Dialog.Cancel | Dialog.Ok,
                    open)
            } else {
                open()
            }
        }
    }

    // RotorLab GC: the crew screen - who is flying. Opens after the licence
    // gate has cleared, never before it, and never as a step between the app
    // opening and the aircraft controls: it is a non-modal popup the crew can
    // dismiss in one action and fly.
    Loader {
        id:             crewScreenLoader
        z:              9000
        active:         LicenseManager.activated && activeConnector.crewScreenPending
        source:         "qrc:/custom/fleet/CrewScreen.qml"
        onLoaded: {
            item.backend = activeConnector
            item.open()
        }
    }

    // RotorLab GC: the station's own cards and alerts, stacked in one column.
    //
    // Not over a flight control: the column starts below the area where QGC's
    // guided-action confirm slider appears (top center of the fly view), and
    // the whole column hides while that confirm is showing. Only visible
    // children take space, so a hidden card leaves no gap.
    Column {
        id:                         rotorLabCards
        z:                          8500
        visible:                    !_guidedConfirmShowing
        anchors.horizontalCenter:   parent.horizontalCenter
        anchors.top:                parent.top
        anchors.topMargin:          ScreenTools.toolbarHeight + ScreenTools.defaultFontPixelHeight * 9
        spacing:                    ScreenTools.defaultFontPixelHeight / 2

        // RotorLab GC: station alert for an ARM, TKOFF or GOTO received from ATAK
        // (or any command above the station's altitude warning level). Informs
        // only (the command has already gone to the aircraft) and is dismissed by
        // the operator. First in the column. Shown over the fly view only, so
        // it never covers a settings page; while hidden it stays loaded and
        // comes back with the fly view. Every command is also on the TAK page
        // and in the command log.
        Loader {
            anchors.horizontalCenter:   parent.horizontalCenter
            active:                     LicenseManager.activated && TAKManager.bannerAlerts.length > 0
            visible:                    active && _flyViewShowing
            source:                     "qrc:/custom/tak/RemoteCommandBanner.qml"
        }

        // RotorLab GC: the checklist offer. A card, not a modal - the crew opens
        // it or ignores it, and either way the aircraft is unaffected. Never shown
        // while anything is armed, and never on the path to a flight control.
        Rectangle {
            id:      checklistOffer
            // Over the fly view only, like the TAK banner: never over a
            // settings page, and back with the fly view.
            visible: LicenseManager.activated && _flyViewShowing && !activeConnector.crewScreenPending &&
                     activeConnector.checklistOffer !== undefined &&
                     activeConnector.checklistOffer !== ""
            anchors.horizontalCenter: parent.horizontalCenter
            width:                    offerRow.implicitWidth + ScreenTools.defaultFontPixelWidth * 4
            height:                   offerRow.implicitHeight + ScreenTools.defaultFontPixelHeight
            radius:                   ScreenTools.defaultFontPixelHeight / 3
            color:                    qgcPal.windowShade
            border.color:             qgcPal.groupBorder
            border.width:             1

            RowLayout {
                id:               offerRow
                anchors.centerIn: parent
                spacing:          ScreenTools.defaultFontPixelWidth * 2
                QGCLabel {
                    text: activeConnector.checklistOffer === "preflight"
                          ? qsTr("Run the pre-flight checklist?")
                          : qsTr("Run the post-flight checklist?")
                }
                QGCButton {
                    text:    qsTr("Open")
                    primary: true
                    onClicked: {
                        checklistRunner.active = true
                        if (checklistRunner.item) {
                            checklistRunner.item.backend = activeConnector
                            checklistRunner.item.kind    = activeConnector.checklistOffer
                            checklistRunner.item.open()
                        }
                    }
                }
                QGCButton {
                    text: qsTr("Not now")
                    onClicked: activeConnector.dismissChecklistOffer()
                }
            }
        }

        // RotorLab GC: what the server said about a flight already filed (no pilot
        // named, a voided re-send). A card the crew reads and dismisses; it stays
        // until then so it is seen after landing, and it never blocks anything.
        // Hidden while anything is armed, like the checklist offer; the notices
        // themselves are kept and the card returns on disarm.
        Rectangle {
            id:      filingNoticeCard
            visible: LicenseManager.activated && _flyViewShowing && !activeConnector.crewScreenPending &&
                     activeConnector.anyArmed !== true &&
                     activeConnector.filingNotices !== undefined &&
                     activeConnector.filingNotices.length > 0

            // Never wider than the window allows, whatever the notice says.
            readonly property real _textMax: Math.min(ScreenTools.defaultFontPixelWidth * 60,
                                                      mainWindow.width * 0.8 - dismissButton.width)
            readonly property int  _shown:   activeConnector.filingNotices === undefined
                                             ? 0 : Math.min(3, activeConnector.filingNotices.length)
            anchors.horizontalCenter: parent.horizontalCenter
            width:                    noticeRow.implicitWidth + ScreenTools.defaultFontPixelWidth * 4
            height:                   noticeRow.implicitHeight + ScreenTools.defaultFontPixelHeight
            radius:                   ScreenTools.defaultFontPixelHeight / 3
            color:                    qgcPal.windowShade
            border.color:             qgcPal.groupBorder
            border.width:             1

            RowLayout {
                id:               noticeRow
                anchors.centerIn: parent
                spacing:          ScreenTools.defaultFontPixelWidth * 2
                ColumnLayout {
                    spacing: ScreenTools.defaultFontPixelHeight / 4
                    // At most three, pilot notices first. A no-pilot notice is
                    // never cut short; the others stop at four lines. The full
                    // list is on the RotorLab Connector settings page.
                    Repeater {
                        model: filingNoticeCard._shown
                        QGCLabel {
                            required property int index
                            Layout.maximumWidth: filingNoticeCard._textMax
                            wrapMode:            Text.WordWrap
                            maximumLineCount:    index < activeConnector.filingPilotNoticeCount ? 1000 : 4
                            elide:               Text.ElideRight
                            textFormat:          Text.PlainText
                            text:                activeConnector.filingNotices[index]
                        }
                    }
                    QGCLabel {
                        Layout.maximumWidth: filingNoticeCard._textMax
                        wrapMode:            Text.WordWrap
                        color:               qgcPal.colorGrey
                        visible:             activeConnector.filingNotices !== undefined &&
                                             activeConnector.filingNotices.length > filingNoticeCard._shown
                        text:                visible
                                             ? qsTr("and %1 more in RotorLab settings")
                                                   .arg(activeConnector.filingNotices.length - filingNoticeCard._shown)
                                             : ""
                    }
                }
                QGCButton {
                    id:        dismissButton
                    text:      qsTr("Dismiss")
                    // Only what this card showed; anything past the first three
                    // stays here and in RotorLab settings.
                    onClicked: activeConnector.dismissFilingNotices(
                                   activeConnector.filingNotices.slice(0, filingNoticeCard._shown))
                }
            }
        }

        // RotorLab GC: who the crew named as pilot, small and always there on
        // the fly view ("Remote pilot in command: Lee Smith (from 14:02)"). Text only,
        // nothing to press; it changes with Change pilot on the RotorLab
        // Connector page. It hides with the rest of this column while the
        // guided confirm is on screen.
        Rectangle {
            id:      picLine
            visible: LicenseManager.activated && _flyViewShowing && RotorLabConnector.enabled &&
                     RotorLabConnector.picText !== ""
            anchors.horizontalCenter: parent.horizontalCenter
            width:   picLabel.implicitWidth + ScreenTools.defaultFontPixelWidth * 2
            height:  picLabel.implicitHeight + ScreenTools.defaultFontPixelHeight / 3
            radius:  ScreenTools.defaultFontPixelHeight / 4
            color:   Qt.rgba(qgcPal.window.r, qgcPal.window.g, qgcPal.window.b, 0.75)
            QGCLabel {
                id:               picLabel
                anchors.centerIn: parent
                font.pointSize:   ScreenTools.smallFontPointSize
                textFormat:       Text.PlainText
                text:             RotorLabConnector.picText
            }
        }
    }

    Loader {
        id:     checklistRunner
        z:      9000
        active: false
        source: "qrc:/custom/fleet/ChecklistRunner.qml"
    }

    footer: LogReplayStatusBar {
        visible: QGroundControl.settingsManager.flyViewSettings.showLogReplayStatusBar.rawValue
    }

    MessageDialog {
        id:                 showTouchAreasNotification
        title:              qsTr("Debug Touch Areas")
        text:               qsTr("Touch Area display toggled")
        buttons:            MessageDialog.Ok
    }

    MessageDialog {
        id:                 advancedModeOnConfirmation
        title:              qsTr("Advanced Mode")
        text:               QGroundControl.corePlugin.showAdvancedUIMessage
        buttons:            MessageDialog.Yes | MessageDialog.No
        onButtonClicked: function (button, role) {
            if (button === MessageDialog.Yes) {
                QGroundControl.corePlugin.showAdvancedUI = true
            }
        }
    }

    MessageDialog {
        id:                 advancedModeOffConfirmation
        title:              qsTr("Advanced Mode")
        text:               qsTr("Turn off Advanced Mode?")
        buttons:            MessageDialog.Yes | MessageDialog.No
        onButtonClicked: function (button, role) {
            if (button === MessageDialog.Yes) {
                QGroundControl.corePlugin.showAdvancedUI = false
            }
        }
    }

    function showToolSelectDialog() {
        if (mainWindow.allowViewSwitch()) {
            mainWindow.showIndicatorDrawer(toolSelectComponent, null)
        }
    }

    Component {
        id: toolSelectComponent

        ToolIndicatorPage {
            id:         toolSelectDialog
            //title:      qsTr("Select Tool")

            property real _toolButtonHeight:    ScreenTools.defaultFontPixelHeight * 3
            property real _margins:             ScreenTools.defaultFontPixelWidth

            contentComponent: Component {
                ColumnLayout {
                    width:  innerLayout.width + (toolSelectDialog._margins * 2)
                    height: innerLayout.height + (toolSelectDialog._margins * 2)

                    ColumnLayout {
                        id:             innerLayout
                        Layout.margins: toolSelectDialog._margins
                        spacing:        ScreenTools.defaultFontPixelWidth

                        SubMenuButton {
                            height:             toolSelectDialog._toolButtonHeight
                            Layout.fillWidth:   true
                            text:               qsTr("Plan Flight")
                            imageResource:      "/qmlimages/Plan.svg"
                            onClicked: {
                                if (mainWindow.allowViewSwitch()) {
                                    mainWindow.closeIndicatorDrawer()
                                    mainWindow.showPlanView()
                                }
                            }
                        }

                        SubMenuButton {
                            id:                 analyzeButton
                            height:             toolSelectDialog._toolButtonHeight
                            Layout.fillWidth:   true
                            text:               qsTr("Analyze Tools")
                            imageResource:      "/qmlimages/Analyze.svg"
                            visible:            QGroundControl.corePlugin.showAdvancedUI
                            onClicked: {
                                if (mainWindow.allowViewSwitch()) {
                                    mainWindow.closeIndicatorDrawer()
                                    mainWindow.showAnalyzeTool()
                                }
                            }
                        }

                        SubMenuButton {
                            id:                 setupButton
                            height:             toolSelectDialog._toolButtonHeight
                            Layout.fillWidth:   true
                            text:               qsTr("Vehicle Configuration")
                            imageResource:      "/qmlimages/Gears.svg"
                            onClicked: {
                                if (mainWindow.allowViewSwitch()) {
                                    mainWindow.closeIndicatorDrawer()
                                    mainWindow.showVehicleConfig()
                                }
                            }
                        }

                        SubMenuButton {
                            id:                 settingsButton
                            height:             toolSelectDialog._toolButtonHeight
                            Layout.fillWidth:   true
                            text:               qsTr("Application Settings")
                            imageResource:      "/res/QGCLogoFull.svg"
                            imageColor:         "transparent"
                            visible:            !QGroundControl.corePlugin.options.combineSettingsAndSetup
                            onClicked: {
                                if (mainWindow.allowViewSwitch()) {
                                    drawer.close()
                                    mainWindow.showSettingsTool()
                                }
                            }
                        }

                        SubMenuButton {
                            id:                 closeButton
                            height:             toolSelectDialog._toolButtonHeight
                            Layout.fillWidth:   true
                            text:               qsTr("Close %1").arg(QGroundControl.appName)
                            imageResource:      "/res/cancel.svg"
                            visible:            mainWindow.visibility === Window.FullScreen
                            onClicked: {
                                if (mainWindow.allowViewSwitch()) {
                                    mainWindow.finishCloseProcess()
                                }
                            }
                        }

                        ColumnLayout {
                            width:                  innerLayout.width
                            spacing:                0
                            Layout.alignment:       Qt.AlignHCenter

                            QGCLabel {
                                id:                     versionLabel
                                text:                   qsTr("%1 Version").arg(QGroundControl.appName)
                                font.pointSize:         ScreenTools.smallFontPointSize
                                wrapMode:               QGCLabel.WordWrap
                                Layout.maximumWidth:    parent.width
                                Layout.alignment:       Qt.AlignHCenter
                            }

                            QGCLabel {
                                text:                   QGroundControl.qgcVersion
                                font.pointSize:         ScreenTools.smallFontPointSize
                                wrapMode:               QGCLabel.WrapAnywhere
                                Layout.maximumWidth:    parent.width
                                Layout.alignment:       Qt.AlignHCenter

                                QGCMouseArea {
                                    id:                 easterEggMouseArea
                                    anchors.topMargin:  -versionLabel.height
                                    anchors.fill:       parent

                                    onClicked: (mouse) => {
                                        if (mouse.modifiers & Qt.ControlModifier) {
                                            QGroundControl.corePlugin.showTouchAreas = !QGroundControl.corePlugin.showTouchAreas
                                            showTouchAreasNotification.open()
                                        } else if (ScreenTools.isMobile || mouse.modifiers & Qt.ShiftModifier) {
                                            mainWindow.closeIndicatorDrawer()
                                            if(!QGroundControl.corePlugin.showAdvancedUI) {
                                                advancedModeOnConfirmation.open()
                                            } else {
                                                advancedModeOffConfirmation.open()
                                            }
                                        }
                                    }

                                    // This allows you to change this on mobile
                                    onPressAndHold: {
                                        QGroundControl.corePlugin.showTouchAreas = !QGroundControl.corePlugin.showTouchAreas
                                        showTouchAreasNotification.open()
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Rectangle {
        id:             toolDrawer
        anchors.fill:   parent
        visible:        false
        color:          qgcPal.window

        property var backIcon
        property string toolTitle
        property alias toolSource:  toolDrawerLoader.source
        property var toolIcon

        onVisibleChanged: {
            if (!toolDrawer.visible) {
                toolDrawerLoader.source = ""
            }
        }

        // This need to block click event leakage to underlying map.
        DeadMouseArea {
            anchors.fill: parent
        }

        Rectangle {
            id:             toolDrawerToolbar
            anchors.left:   parent.left
            anchors.right:  parent.right
            anchors.top:    parent.top
            height:         ScreenTools.toolbarHeight
            color:          qgcPal.toolbarBackground

            RowLayout {
                id:                 toolDrawerToolbarLayout
                anchors.leftMargin: ScreenTools.defaultFontPixelWidth
                anchors.left:       parent.left
                anchors.top:        parent.top
                anchors.bottom:     parent.bottom
                spacing:            ScreenTools.defaultFontPixelWidth

                QGCLabel {
                    font.pointSize: ScreenTools.largeFontPointSize
                    text:           "<"
                }

                QGCLabel {
                    id:             toolbarDrawerText
                    text:           qsTr("Exit") + " " + toolDrawer.toolTitle
                    font.pointSize: ScreenTools.largeFontPointSize
                }
            }

            QGCMouseArea {
                anchors.fill: toolDrawerToolbarLayout
                onClicked: {
                    if (mainWindow.allowViewSwitch()) {
                        toolDrawer.visible = false
                    }
                }
            }
        }

        Loader {
            id:             toolDrawerLoader
            anchors.left:   parent.left
            anchors.right:  parent.right
            anchors.top:    toolDrawerToolbar.bottom
            anchors.bottom: parent.bottom

            Connections {
                target:                 toolDrawerLoader.item
                ignoreUnknownSignals:   true
                onPopout:               toolDrawer.visible = false
            }
        }
    }

    //-------------------------------------------------------------------------
    //-- Critical Vehicle Message Popup

    function showCriticalVehicleMessage(message) {
        closeIndicatorDrawer()
        if (criticalVehicleMessagePopup.visible || QGroundControl.videoManager.fullScreen) {
            // We received additional warning message while an older warning message was still displayed.
            // When the user close the older one drop the message indicator tool so they can see the rest of them.
            criticalVehicleMessagePopup.additionalCriticalMessagesReceived = true
        } else {
            criticalVehicleMessagePopup.criticalVehicleMessage      = message
            criticalVehicleMessagePopup.additionalCriticalMessagesReceived = false
            criticalVehicleMessagePopup.open()
        }
    }

    Popup {
        id:                 criticalVehicleMessagePopup
        y:                  ScreenTools.toolbarHeight + ScreenTools.defaultFontPixelHeight
        x:                  Math.round((mainWindow.width - width) * 0.5)
        width:              mainWindow.width  * 0.55
        height:             criticalVehicleMessageText.contentHeight + ScreenTools.defaultFontPixelHeight * 2
        modal:              false
        focus:              true

        property alias  criticalVehicleMessage:             criticalVehicleMessageText.text
        property bool   additionalCriticalMessagesReceived: false

        background: Rectangle {
            anchors.fill:   parent
            color:          qgcPal.alertBackground
            radius:         ScreenTools.defaultFontPixelHeight * 0.5
            border.color:   qgcPal.alertBorder
            border.width:   2

            Rectangle {
                anchors.horizontalCenter:   parent.horizontalCenter
                anchors.top:                parent.top
                anchors.topMargin:          -(height / 2)
                color:                      qgcPal.alertBackground
                radius:                     ScreenTools.defaultFontPixelHeight * 0.25
                border.color:               qgcPal.alertBorder
                border.width:               1
                width:                      vehicleWarningLabel.contentWidth + _margins
                height:                     vehicleWarningLabel.contentHeight + _margins

                property real _margins: ScreenTools.defaultFontPixelHeight * 0.25

                QGCLabel {
                    id:                 vehicleWarningLabel
                    anchors.centerIn:   parent
                    text:               qsTr("Vehicle Error")
                    font.pointSize:     ScreenTools.smallFontPointSize
                    color:              qgcPal.alertText
                }
            }

            Rectangle {
                id:                         additionalErrorsIndicator
                anchors.horizontalCenter:   parent.horizontalCenter
                anchors.bottom:             parent.bottom
                anchors.bottomMargin:       -(height / 2)
                color:                      qgcPal.alertBackground
                radius:                     ScreenTools.defaultFontPixelHeight * 0.25
                border.color:               qgcPal.alertBorder
                border.width:               1
                width:                      additionalErrorsLabel.contentWidth + _margins
                height:                     additionalErrorsLabel.contentHeight + _margins
                visible:                    criticalVehicleMessagePopup.additionalCriticalMessagesReceived

                property real _margins: ScreenTools.defaultFontPixelHeight * 0.25

                QGCLabel {
                    id:                 additionalErrorsLabel
                    anchors.centerIn:   parent
                    text:               qsTr("Additional errors received")
                    font.pointSize:     ScreenTools.smallFontPointSize
                    color:              qgcPal.alertText
                }
            }
        }

        QGCLabel {
            id:                 criticalVehicleMessageText
            width:              criticalVehicleMessagePopup.width - ScreenTools.defaultFontPixelHeight
            anchors.centerIn:   parent
            wrapMode:           Text.WordWrap
            color:              qgcPal.alertText
            textFormat:         TextEdit.RichText
        }

        MouseArea {
            anchors.fill: parent
            onClicked: {
                criticalVehicleMessagePopup.close()
                if (criticalVehicleMessagePopup.additionalCriticalMessagesReceived) {
                    criticalVehicleMessagePopup.additionalCriticalMessagesReceived = false;
                    flyView.dropMainStatusIndicatorTool();
                } else {
                    QGroundControl.multiVehicleManager.activeVehicle.resetErrorLevelMessages();
                }
            }
        }
    }

    //-------------------------------------------------------------------------
    //-- Indicator Drawer

    function showIndicatorDrawer(drawerComponent, indicatorItem) {
        indicatorDrawer.sourceComponent = drawerComponent
        indicatorDrawer.indicatorItem = indicatorItem
        indicatorDrawer.open()
    }

    function closeIndicatorDrawer() {
        indicatorDrawer.close()
    }

    Popup {
        id:             indicatorDrawer
        x:              calcXPosition()
        y:              ScreenTools.toolbarHeight + _margins
        leftInset:      0
        rightInset:     0
        topInset:       0
        bottomInset:    0
        padding:        _margins * 2
        visible:        false
        modal:          true
        focus:          true
        closePolicy:    Popup.CloseOnEscape | Popup.CloseOnPressOutside

        property var sourceComponent
        property var indicatorItem

        property bool _expanded:    false
        property real _margins:     ScreenTools.defaultFontPixelHeight / 4

        function calcXPosition() {
            if (indicatorItem) {
                var xCenter = indicatorItem.mapToItem(mainWindow.contentItem, indicatorItem.width / 2, 0).x
                return Math.max(_margins, Math.min(xCenter - (contentItem.implicitWidth / 2), mainWindow.contentItem.width - contentItem.implicitWidth - _margins - (indicatorDrawer.padding * 2) - (ScreenTools.defaultFontPixelHeight / 2)))
            } else {
                return _margins
            }
        }

        onOpened: {
            _expanded                               = false;
            indicatorDrawerLoader.sourceComponent   = indicatorDrawer.sourceComponent
        }
        onClosed: {
            _expanded                               = false
            indicatorItem                           = undefined
            indicatorDrawerLoader.sourceComponent   = undefined
        }

        background: Item {
            Rectangle {
                id:             backgroundRect
                anchors.fill:   parent
                color:          QGroundControl.globalPalette.window
                radius:         indicatorDrawer._margins
                opacity:        0.85
            }

            Rectangle {
                anchors.horizontalCenter:   backgroundRect.right
                anchors.verticalCenter:     backgroundRect.top
                width:                      ScreenTools.largeFontPixelHeight
                height:                     width
                radius:                     width / 2
                color:                      QGroundControl.globalPalette.button
                border.color:               QGroundControl.globalPalette.buttonText
                visible:                    indicatorDrawerLoader.item && indicatorDrawerLoader.item.showExpand && !indicatorDrawer._expanded

                QGCLabel {
                    anchors.centerIn:   parent
                    text:               ">"
                    color:              QGroundControl.globalPalette.buttonText
                }  

                QGCMouseArea {
                    fillItem: parent
                    onClicked: indicatorDrawer._expanded = true
                }
            }
        }

        contentItem: QGCFlickable {
            id:             indicatorDrawerLoaderFlickable
            implicitWidth:  Math.min(mainWindow.contentItem.width - (2 * indicatorDrawer._margins) - (indicatorDrawer.padding * 2), indicatorDrawerLoader.width)
            implicitHeight: Math.min(mainWindow.contentItem.height - ScreenTools.toolbarHeight - (2 * indicatorDrawer._margins) - (indicatorDrawer.padding * 2), indicatorDrawerLoader.height)
            contentWidth:   indicatorDrawerLoader.width
            contentHeight:  indicatorDrawerLoader.height

            Loader {
                id: indicatorDrawerLoader

                Binding {
                    target:     indicatorDrawerLoader.item
                    property:   "expanded"
                    value:      indicatorDrawer._expanded
                }

                Binding {
                    target:     indicatorDrawerLoader.item
                    property:   "drawer"
                    value:      indicatorDrawer
                }
            }
        }
    }

    // We have to create the popup windows for the Analyze pages here so that the creation context is rooted
    // to mainWindow. Otherwise if they are rooted to the AnalyzeView itself they will die when the analyze viewSwitch
    // closes.

    function createrWindowedAnalyzePage(title, source) {
        var windowedPage = windowedAnalyzePage.createObject(mainWindow)
        windowedPage.title = title
        windowedPage.source = source
    }

    Component {
        id: windowedAnalyzePage

        Window {
            width:      ScreenTools.defaultFontPixelWidth  * 100
            height:     ScreenTools.defaultFontPixelHeight * 40
            visible:    true

            property alias source: loader.source

            Rectangle {
                color:          QGroundControl.globalPalette.window
                anchors.fill:   parent

                Loader {
                    id:             loader
                    anchors.fill:   parent
                    onLoaded:       item.popped = true
                }
            }

            onClosing: {
                visible = false
                source = ""
            }
        }
    }

    Connections{
         target: activationbar
         function onActivationTriggered(value){
              _utmspSendActTrigger= value
         }
    }

    UTMSPActivationStatusBar{
         id:                         activationbar
         activationStartTimestamp:   UTMSPStateStorage.startTimeStamp
         activationApproval:         UTMSPStateStorage.showActivationTab && QGroundControl.utmspManager.utmspVehicle.vehicleActivation
         flightID:                   UTMSPStateStorage.flightID
         anchors.fill:               parent
    }
}
