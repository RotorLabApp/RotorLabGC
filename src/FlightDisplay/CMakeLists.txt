qt_add_library(FlightDisplayModule STATIC)

# G88: the Fly view's video item. With GStreamer it is GStreamer's video item; otherwise a
# Qt VideoOutput, which GC's FFmpeg receiver (and Qt Multimedia) decode into. One file
# named QGCVideoBackground.qml is compiled, so the type name is the same either way.
if(QGC_ENABLE_GST_VIDEOSTREAMING)
    set(QGC_VIDEO_BACKGROUND_QML VideoBackground/GStreamer/QGCVideoBackground.qml)
else()
    set(QGC_VIDEO_BACKGROUND_QML VideoBackground/VideoOutput/QGCVideoBackground.qml)
endif()
# At the module's root, where QGCVideoBackground.qml always was
set_source_files_properties(${QGC_VIDEO_BACKGROUND_QML} PROPERTIES QT_RESOURCE_ALIAS QGCVideoBackground.qml)

qt_add_qml_module(FlightDisplayModule
    URI QGroundControl.FlightDisplay
    VERSION 1.0
    RESOURCE_PREFIX /qml
    QML_FILES
        CustomGuidedActionsController.qml
        DefaultChecklist.qml
        FixedWingChecklist.qml
        FlightDisplayViewDummy.qml
        FlightDisplayViewGStreamer.qml
        FlightDisplayViewQtMultimedia.qml
        FlightDisplayViewUVC.qml
        FlightDisplayViewVideo.qml
        FlyView.qml
        FlyViewAdditionalActionsButton.qml
        FlyViewAdditionalActionsList.qml
        FlyViewAdditionalActionsPanel.qml
        FlyViewAdditionalCustomActionsList.qml
        FlyViewBottomRightRowLayout.qml
        FlyViewCustomLayer.qml
        FlyViewInsetViewer.qml
        FlyViewInstrumentPanel.qml
        FlyViewMap.qml
        FlyViewMissionCompleteDialog.qml
        FlyViewPreFlightChecklistPopup.qml
        FlyViewToolStrip.qml
        FlyViewToolStripActionList.qml
        FlyViewTopRightColumnLayout.qml
        FlyViewTopRightPanel.qml
        FlyViewVideo.qml
        FlyViewWidgetLayer.qml
        GripperMenu.qml
        GuidedActionConfirm.qml
        GuidedActionGripper.qml
        GuidedActionLand.qml
        GuidedActionPause.qml
        GuidedActionRTL.qml
        GuidedActionsController.qml
        GuidedActionTakeoff.qml
        GuidedToolStripAction.qml
        GuidedValueSlider.qml
        MultiRotorChecklist.qml
        MultiVehicleList.qml
        ObstacleDistanceOverlay.qml
        ObstacleDistanceOverlayMap.qml
        ObstacleDistanceOverlayVideo.qml
        OnScreenGimbalController.qml
        PreFlightBatteryCheck.qml
        PreFlightCheckList.qml
        PreFlightCheckListShowAction.qml
        PreFlightGPSCheck.qml
        PreFlightRCCheck.qml
        PreFlightSensorsHealthCheck.qml
        PreFlightSoundCheck.qml
        ProximityRadarValues.qml
        ProximityRadarVideoView.qml
        ${QGC_VIDEO_BACKGROUND_QML}
        RoverChecklist.qml
        SubChecklist.qml
        TelemetryValuesBar.qml
        TerrainProgress.qml
        VehicleWarnings.qml
        VirtualJoystick.qml
    NO_PLUGIN # Required by Qt 6.6.3. TODO: Remove when no longer supported.
)
