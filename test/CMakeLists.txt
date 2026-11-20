if(NOT QGC_BUILD_TESTING)
    return()
endif()

# if(ANDROID)
    # include(AndroidTestUtilities)
# endif()

# set(UNIT_TESTING_RESOURCES)
# qt_add_resources(UNIT_TESTING_RESOURCES UnitTest.qrc)

target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        UnitTestList.cc
        UnitTestList.h
)

find_package(Qt6 REQUIRED COMPONENTS Test)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Test)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(QGC_CUSTOM_BUILD)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        Custom/ConnectorIntegrationTest.cc
        Custom/ConnectorIntegrationTest.h
        Custom/CredentialTest.cc
        Custom/CredentialTest.h
        Custom/FleetEnrollTest.cc
        Custom/FleetEnrollTest.h
        Custom/LogCollectPolicyTest.cc
        Custom/LogCollectPolicyTest.h
        Custom/LicenseLinkGateTest.cc
        Custom/LicenseLinkGateTest.h
        Custom/LicenseRecheckTest.cc
        Custom/LicenseRecheckTest.h
        Custom/LicenseRecordTest.cc
        Custom/LicenseRecordTest.h
        Custom/LicenseStubTest.cc
        Custom/LicenseStubTest.h
        Custom/LicenseTestRecords.h
        Custom/RotorLabCrewTest.cc
        Custom/RotorLabCrewTest.h
        Custom/RotorLabFilingTest.cc
        Custom/RotorLabFilingTest.h
        Custom/RotorLabRosterTest.cc
        Custom/RotorLabRosterTest.h
        Custom/RotorLabSpoolTest.cc
        Custom/RotorLabSpoolTest.h
        Custom/RunGuardTest.cc
        Custom/RunGuardTest.h
        Custom/TakEnrollWireTest.cc
        Custom/TakEnrollWireTest.h
        Custom/TakManagerTest.cc
        Custom/TakManagerTest.h
        Custom/TakMarkerTest.cc
        Custom/TakMarkerTest.h
        Custom/TakPolicyTest.cc
        Custom/TakPolicyTest.h
        Custom/PlanCorpusOpenTest.cc
        Custom/PlanCorpusOpenTest.h
        Custom/VideoSinkTest.cc
        Custom/VideoSinkTest.h
    )
    target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Custom)
    # G66: the RotorLab exports open in 2D unchanged, on every platform (Android has no 3D view).
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE RL_A204_EXPORTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Custom/plan-corpus/a204-rotorlab")
    if(RL_PLAN3D)
        target_sources(${CMAKE_PROJECT_NAME} PRIVATE Custom/Plan3DTest.cc Custom/Plan3DTest.h)
        # G65: 3D editing, contract section 4a.
        target_sources(${CMAKE_PROJECT_NAME} PRIVATE Custom/Plan3DEditTest.cc Custom/Plan3DEditTest.h)
        target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE RL_PLAN_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Custom/plan-corpus")
    endif()
    # G95 local recording: its tests are built only where they exist
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/Custom/Video/VideoUploaderTest.cc)
        target_sources(${CMAKE_PROJECT_NAME} PRIVATE
            Custom/Video/VideoTestStreams.h
            Custom/Video/RecordingRecoveryTest.cc Custom/Video/RecordingRecoveryTest.h
            Custom/Video/VideoUploaderTest.cc Custom/Video/VideoUploaderTest.h)
        target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Custom/Video)
        if(QGC_FFMPEG_VIDEOSTREAMING_BUILT)
            target_sources(${CMAKE_PROJECT_NAME} PRIVATE
                Custom/Video/Fmp4MuxerTest.cc Custom/Video/Fmp4MuxerTest.h
                Custom/Video/RecordingWriterTest.cc Custom/Video/RecordingWriterTest.h
                Custom/Video/TimeMapWriterTest.cc Custom/Video/TimeMapWriterTest.h)
        endif()
    endif()
    # G109 live relay: its tests are built only where they exist
    if(QGC_FFMPEG_VIDEOSTREAMING_BUILT AND EXISTS ${CMAKE_SOURCE_DIR}/custom/src/video/relay/test/LiveSessionTest.cc)
        set(_g109 ${CMAKE_SOURCE_DIR}/custom/src/video/relay/test)
        target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${_g109}/RelayTestSupport.h
            ${_g109}/EncoderProbeTest.cc ${_g109}/EncoderProbeTest.h
            ${_g109}/LiveFragmenterTest.cc ${_g109}/LiveFragmenterTest.h
            ${_g109}/LiveSessionTest.cc ${_g109}/LiveSessionTest.h
            ${_g109}/RelayEncoderTest.cc ${_g109}/RelayEncoderTest.h)
        target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${_g109})
    endif()
endif()

# G64 (b): Plan view 3D ground, Windows and Linux builds only (RL_PLAN3D).
if(QGC_CUSTOM_BUILD AND RL_PLAN3D)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        Custom/Plan3DGroundTest.cc
        Custom/Plan3DGroundTest.h
    )
    # G66: corpus parity with RotorLab's 3D on the shared terrain fixture G66-F1.
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        Custom/Plan3DParityTest.cc
        Custom/Plan3DParityTest.h
    )
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
        RL_G66_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Custom/plan-corpus/g66"
        RL_PLAN3D_RES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../custom/res/plan3d"
    )
endif()

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE QGC_UNITTEST_BUILD)

include(CTest)
add_custom_target(check
    COMMAND ctest --output-on-failure .
    USES_TERMINAL
)

function(add_qgc_test test_name)
    add_test(
        NAME ${test_name}
        COMMAND $<TARGET_FILE:${PROJECT_NAME}> --unittest:${test_name}
    )
    add_dependencies(check ${PROJECT_NAME})
endfunction()

if(QGC_CUSTOM_BUILD)
    add_qgc_test(CredentialTest)
    add_qgc_test(FleetEnrollTest)
    add_qgc_test(LicenseLinkGateTest)
    add_qgc_test(LicenseRecheckTest)
    add_qgc_test(LicenseRecordTest)
    if(RL_PLAN3D)
        add_qgc_test(Plan3DTest)
        add_qgc_test(Plan3DEditTest)
    endif()
    add_qgc_test(RotorLabCrewTest)
    add_qgc_test(RotorLabFilingTest)
    add_qgc_test(RotorLabRosterTest)
    add_qgc_test(RotorLabSpoolTest)
    add_qgc_test(RunGuardTest)
    add_qgc_test(TakEnrollWireTest)
    add_qgc_test(TakManagerTest)
    add_qgc_test(TakMarkerTest)
    add_qgc_test(TakPolicyTest)
    add_qgc_test(PlanCorpusOpenTest)
    add_qgc_test(VideoSinkTest)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/Custom/Video/VideoUploaderTest.cc)
        add_qgc_test(RecordingRecoveryTest)
        add_qgc_test(VideoUploaderTest)
        if(QGC_FFMPEG_VIDEOSTREAMING_BUILT)
            add_qgc_test(Fmp4MuxerTest)
            add_qgc_test(RecordingWriterTest)
            add_qgc_test(TimeMapWriterTest)
        endif()
    endif()
endif()
# G109 relay tests (sources added above); add_qgc_test exists only from here on
if(QGC_CUSTOM_BUILD AND QGC_FFMPEG_VIDEOSTREAMING_BUILT AND EXISTS ${CMAKE_SOURCE_DIR}/custom/src/video/relay/test/LiveSessionTest.cc)
    add_qgc_test(EncoderProbeTest)
    add_qgc_test(LiveFragmenterTest)
    add_qgc_test(LiveSessionTest)
    add_qgc_test(RelayEncoderTest)
endif()
if(QGC_CUSTOM_BUILD AND RL_PLAN3D)
    add_qgc_test(Plan3DGroundTest)
    add_qgc_test(Plan3DParityTest)
endif()

add_subdirectory(ADSB)
add_qgc_test(ADSBTest)

add_subdirectory(AnalyzeView)
add_qgc_test(ExifParserTest)
# add_qgc_test(GeoTagControllerTest)
add_qgc_test(LogDownloadTest)
# add_qgc_test(MavlinkLogTest)
add_qgc_test(PX4LogParserTest)
# add_qgc_test(ULogParserTest)

# add_subdirectory(AutoPilotPlugins)
# add_qgc_test(RadioConfigTest)

add_subdirectory(Camera)
add_qgc_test(QGCCameraManagerTest)

add_subdirectory(Comms)
add_qgc_test(QGCSerialPortInfoTest)

add_subdirectory(FactSystem)
add_qgc_test(FactSystemTestGeneric)
add_qgc_test(FactSystemTestPX4)
add_qgc_test(ParameterManagerTest)

add_subdirectory(FollowMe)
add_qgc_test(FollowMeTest)

add_subdirectory(GPS)
add_qgc_test(GpsTest)

add_subdirectory(MAVLink)
add_qgc_test(StatusTextHandlerTest)
add_qgc_test(SigningTest)

add_subdirectory(MissionManager)
add_qgc_test(CameraCalcTest)
add_qgc_test(CameraSectionTest)
add_qgc_test(CorridorScanComplexItemTest)
# add_qgc_test(FWLandingPatternTest)
# add_qgc_test(LandingComplexItemTest)
# add_qgc_test(MissionCommandTreeEditorTest)
add_qgc_test(MissionCommandTreeTest)
add_qgc_test(MissionControllerManagerTest)
add_qgc_test(MissionControllerTest)
add_qgc_test(MissionItemTest)
add_qgc_test(MissionManagerTest)
add_qgc_test(MissionSettingsTest)
add_qgc_test(PlanManagerTransferTest)
add_qgc_test(PlanMasterControllerTest)
add_qgc_test(QGCMapPolygonTest)
add_qgc_test(QGCMapPolylineTest)
# add_qgc_test(SectionTest)
add_qgc_test(SimpleMissionItemTest)
add_qgc_test(SpeedSectionTest)
add_qgc_test(StructureScanComplexItemTest)
add_qgc_test(SurveyComplexItemTest)
add_qgc_test(TransectStyleComplexItemTest)
# add_qgc_test(VisualMissionItemTest)

add_subdirectory(qgcunittest)
# add_qgc_test(FileDialogTest)
# add_qgc_test(MainWindowTest)
# add_qgc_test(MessageBoxTest)

add_subdirectory(QmlControls)

add_subdirectory(Terrain)
add_qgc_test(TerrainQueryTest)
add_qgc_test(TerrainTileTest)

add_subdirectory(UI)

add_subdirectory(Utilities)
# Audio
add_qgc_test(AudioOutputTest)
# Compression
add_qgc_test(DecompressionTest)
# FileSystem
add_qgc_test(QGCFileDownloadTest)
# Geo
add_qgc_test(GeoTest)
# Shape
add_qgc_test(ShapeTest)

# G88 video receiver
add_subdirectory(VideoManager)
add_qgc_test(FFmpegSourceTest)
add_qgc_test(RtpDepacketizerTest)
add_qgc_test(VideoPacketTapTest)
if(QGC_FFMPEG_VIDEOSTREAMING_BUILT)
    add_qgc_test(FFmpegReceiverTest)
endif()

add_subdirectory(Vehicle)
# Components
add_qgc_test(ComponentInformationCacheTest)
add_qgc_test(ComponentInformationTranslationTest)
add_qgc_test(FTPManagerTest)
# add_qgc_test(InitialConnectTest)
add_qgc_test(MAVLinkLogManagerTest)
# add_qgc_test(RequestMessageTest)
# add_qgc_test(SendMavCommandWithHandlerTest)
# add_qgc_test(SendMavCommandWithSignalingTest)
add_qgc_test(VehicleLinkManagerTest)

# add_qgc_test(FlightGearUnitTest)
# add_qgc_test(LinkManagerTest)
# add_qgc_test(SendMavCommandTest)
# add_qgc_test(TCPLinkTest)
