/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "UnitTestList.h"
#include "UnitTest.h"
#include "QGCLoggingCategory.h"
#ifdef QGC_CUSTOM_BUILD
#include "Credential.h"
#endif

#ifdef QGC_CUSTOM_BUILD
#include "ConnectorIntegrationTest.h"
#include "CredentialTest.h"
#include "FleetEnrollTest.h"
#include "LicenseLinkGateTest.h"
#include "LicenseRecheckTest.h"
#include "LicenseRecordTest.h"
#include "LicenseStubTest.h"
#include "LogCollectPolicyTest.h"
#ifdef RL_PLAN3D
#include "Plan3DTest.h"
#include "Plan3DEditTest.h"
#endif
#include "RotorLabCrewTest.h"
#include "RotorLabFilingTest.h"
#include "RotorLabRosterTest.h"
#include "RotorLabSpoolTest.h"
#include "RunGuardTest.h"
#include "TakEnrollWireTest.h"
#include "TakManagerTest.h"
#include "TakMarkerTest.h"
#include "TakPolicyTest.h"
#include "PlanCorpusOpenTest.h"
#include "VideoSinkTest.h"
// G95 local recording: the tests are registered only where they exist
#if __has_include("VideoUploaderTest.h")
#define RL_G95_RECORD_TESTS
#include "RecordingRecoveryTest.h"
#include "VideoUploaderTest.h"
#ifdef QGC_FFMPEG_STREAMING
#include "Fmp4MuxerTest.h"
#include "RecordingWriterTest.h"
#include "TimeMapWriterTest.h"
#endif
#endif
// G109 live relay: the tests are registered only where they exist
#if defined(QGC_FFMPEG_STREAMING) && __has_include("LiveSessionTest.h")
#define RL_G109_RELAY_TESTS
#include "EncoderProbeTest.h"
#include "LiveFragmenterTest.h"
#include "LiveSessionTest.h"
#include "RelayEncoderTest.h"
#endif
#ifdef RL_PLAN3D
#include "Plan3DGroundTest.h"
#include "Plan3DParityTest.h"
#endif
#endif

// ADSB
#include "ADSBTest.h"

// AnalyzeView
#include "ExifParserTest.h"
// #include "GeoTagControllerTest.h"
// #include "MavlinkLogTest.h"
#include "LogDownloadTest.h"
#include "PX4LogParserTest.h"
// #include "ULogParserTest.h"



// AutoPilotPlugins
// #include "RadioConfigTest.h"

// Camera
#include "QGCCameraManagerTest.h"

// Comms
#include "QGCSerialPortInfoTest.h"

// FactSystem
#include "FactSystemTestGeneric.h"
#include "FactSystemTestPX4.h"
#include "ParameterManagerTest.h"

// FollowMe
#include "FollowMeTest.h"

// GPS
#include "GpsTest.h"

// MAVLink
#include "StatusTextHandlerTest.h"
#include "SigningTest.h"

// MissionManager
#include "CameraCalcTest.h"
#include "CameraSectionTest.h"
#include "CorridorScanComplexItemTest.h"
// #include "FWLandingPatternTest.h"
// #include "LandingComplexItemTest.h"
// #include "MissionCommandTreeEditorTest.h"
#include "MissionCommandTreeTest.h"
#include "MissionControllerManagerTest.h"
#include "MissionControllerTest.h"
#include "MissionItemTest.h"
#include "MissionManagerTest.h"
#include "MissionSettingsTest.h"
#include "LandingSequenceTest.h"
#include "GeoFenceBreachReturnTest.h"
#include "PlanCorpusRoundTripTest.h"
#include "PlanManagerTransferTest.h"
#include "PlanMasterControllerTest.h"
#include "QGCMapPolygonTest.h"
#include "QGCMapPolylineTest.h"
// #include "SectionTest.h"
#include "SimpleMissionItemTest.h"
#include "SpeedSectionTest.h"
#include "StructureScanComplexItemTest.h"
#include "SurveyComplexItemTest.h"
#include "TransectStyleComplexItemTest.h"
// #include "VisualMissionItemTest.h"

// qgcunittest
#include "ComponentInformationCacheTest.h"
#include "ComponentInformationTranslationTest.h"

// QmlControls
#include "TerrainProfileLabelsTest.h"

// Terrain
#include "TerrainQueryTest.h"
#include "TerrainTileTest.h"

// UI

// Utilities
// Audio
#include "AudioOutputTest.h"
// Compression
#include "DecompressionTest.h"
// FileSystem
#include "QGCFileDownloadTest.h"
// Geo
#include "GeoTest.h"
// Shape
#include "ShapeTest.h"

// VideoManager (G88)
#include "FFmpegSourceTest.h"
#include "RtpDepacketizerTest.h"
#include "VideoPacketTapTest.h"
#ifdef QGC_FFMPEG_STREAMING
#include "FFmpegLoopbackBench.h"
#include "FFmpegReceiverTest.h"
#endif

// Vehicle
// Components
#include "ComponentInformationCacheTest.h"
#include "ComponentInformationTranslationTest.h"
#include "FTPManagerTest.h"
// #include "InitialConnectTest.h"
#include "MAVLinkLogManagerTest.h"
// #include "RequestMessageTest.h"
// #include "SendMavCommandWithHandlerTest.h"
// #include "SendMavCommandWithSignalingTest.h"
#include "VehicleLinkManagerTest.h"

// Missing
// #include "FlightGearUnitTest.h"
// #include "LinkManagerTest.h"
// #include "SendMavCommandTest.h"
// #include "TCPLinkTest.h"

QGC_LOGGING_CATEGORY(UnitTestsLog, "qgc.test.unittestlist")

int runTests(bool stress, QStringView unitTestOptions)
{
#ifdef QGC_CUSTOM_BUILD
    UT_REGISTER_TEST_STANDALONE(ConnectorIntegrationTest)
    UT_REGISTER_TEST(CredentialTest)
    UT_REGISTER_TEST(FleetEnrollTest)
    UT_REGISTER_TEST(LicenseLinkGateTest)
    UT_REGISTER_TEST(LicenseRecheckTest)
    UT_REGISTER_TEST(LicenseRecordTest)
    UT_REGISTER_TEST_STANDALONE(LicenseStubTest)
    UT_REGISTER_TEST(LogCollectPolicyTest)
#ifdef RL_PLAN3D
    UT_REGISTER_TEST(Plan3DTest)
    UT_REGISTER_TEST(Plan3DEditTest)
#endif
    UT_REGISTER_TEST(RotorLabCrewTest)
    UT_REGISTER_TEST(RotorLabFilingTest)
    UT_REGISTER_TEST(RotorLabRosterTest)
    UT_REGISTER_TEST(RotorLabSpoolTest)
    UT_REGISTER_TEST(RunGuardTest)
    UT_REGISTER_TEST(TakEnrollWireTest)
    UT_REGISTER_TEST(TakManagerTest)
    UT_REGISTER_TEST(TakMarkerTest)
    UT_REGISTER_TEST(TakPolicyTest)
    UT_REGISTER_TEST(PlanCorpusOpenTest)
    UT_REGISTER_TEST(VideoSinkTest)
#ifdef RL_G95_RECORD_TESTS
    UT_REGISTER_TEST(RecordingRecoveryTest)
    UT_REGISTER_TEST(VideoUploaderTest)
#ifdef QGC_FFMPEG_STREAMING
    UT_REGISTER_TEST(Fmp4MuxerTest)
    UT_REGISTER_TEST(RecordingWriterTest)
    UT_REGISTER_TEST(TimeMapWriterTest)
#endif
#endif
#ifdef RL_G109_RELAY_TESTS
    UT_REGISTER_TEST(EncoderProbeTest)
    UT_REGISTER_TEST(LiveFragmenterTest)
    UT_REGISTER_TEST(LiveSessionTest)
    UT_REGISTER_TEST(RelayEncoderTest)
#endif
#ifdef RL_PLAN3D
    UT_REGISTER_TEST(Plan3DGroundTest)
    UT_REGISTER_TEST(Plan3DParityTest)
#endif
#endif

    // ADSB
    UT_REGISTER_TEST(ADSBTest)

    // AnalyzeView
    UT_REGISTER_TEST(ExifParserTest)
    // UT_REGISTER_TEST(GeoTagControllerTest)
    // UT_REGISTER_TEST(MavlinkLogTest)
    UT_REGISTER_TEST(LogDownloadTest)
    UT_REGISTER_TEST(PX4LogParserTest)
    // UT_REGISTER_TEST(ULogParserTest)

    // AutoPilotPlugins
    // UT_REGISTER_TEST(RadioConfigTest)

    // Camera
    UT_REGISTER_TEST(QGCCameraManagerTest)

    // Comms
    UT_REGISTER_TEST(QGCSerialPortInfoTest)

    // FactSystem
    UT_REGISTER_TEST(FactSystemTestGeneric)
    UT_REGISTER_TEST(FactSystemTestPX4)
    UT_REGISTER_TEST(ParameterManagerTest)

    // FollowMe
    UT_REGISTER_TEST(FollowMeTest)

    // GPS
    // UT_REGISTER_TEST(GpsTest)

    // MAVLink
    UT_REGISTER_TEST(StatusTextHandlerTest)
    UT_REGISTER_TEST(SigningTest)

    // MissionManager
    UT_REGISTER_TEST(CameraCalcTest)
    UT_REGISTER_TEST(CameraSectionTest)
    UT_REGISTER_TEST(CorridorScanComplexItemTest)
    // UT_REGISTER_TEST(FWLandingPatternTest)
    // UT_REGISTER_TEST(LandingComplexItemTest)
    // UT_REGISTER_TEST_STANDALONE(MissionCommandTreeEditorTest)
    UT_REGISTER_TEST(MissionCommandTreeTest)
    UT_REGISTER_TEST(MissionControllerManagerTest)
    UT_REGISTER_TEST(MissionControllerTest)
    UT_REGISTER_TEST(MissionItemTest)
    UT_REGISTER_TEST(MissionManagerTest)
    UT_REGISTER_TEST(MissionSettingsTest)
    UT_REGISTER_TEST(LandingSequenceTest)       // RotorLab GC 5.1.2, G75
    UT_REGISTER_TEST(GeoFenceBreachReturnTest)  // RotorLab GC 5.2.0, G86
    UT_REGISTER_TEST(PlanCorpusRoundTripTest)   // RotorLab GC 5.1.2, G70 and G71
    UT_REGISTER_TEST(PlanManagerTransferTest)   // RotorLab GC 5.3.0, G92 and G93
    UT_REGISTER_TEST(PlanMasterControllerTest)
    UT_REGISTER_TEST(QGCMapPolygonTest)
    UT_REGISTER_TEST(QGCMapPolylineTest)
    // UT_REGISTER_TEST(SectionTest)
    UT_REGISTER_TEST(SimpleMissionItemTest)
    UT_REGISTER_TEST(SpeedSectionTest)
    UT_REGISTER_TEST(StructureScanComplexItemTest)
    UT_REGISTER_TEST(SurveyComplexItemTest)
    UT_REGISTER_TEST(TransectStyleComplexItemTest)
    // UT_REGISTER_TEST(VisualMissionItemTest)

    // qgcunittest

    // QmlControls
    UT_REGISTER_TEST(TerrainProfileLabelsTest)  // RotorLab GC 5.1.2, G78

    // Terrain
    UT_REGISTER_TEST(TerrainQueryTest)
    UT_REGISTER_TEST(TerrainTileTest)

    // UI

    // Utilities
    // Audio
    UT_REGISTER_TEST(AudioOutputTest)
    // Compression
    UT_REGISTER_TEST(DecompressionTest)
    // FileSystem
    UT_REGISTER_TEST(QGCFileDownloadTest)
    // Geo
    UT_REGISTER_TEST(GeoTest)
    // Shape
    UT_REGISTER_TEST(ShapeTest)

    // VideoManager (G88)
    UT_REGISTER_TEST(FFmpegSourceTest)
    UT_REGISTER_TEST(RtpDepacketizerTest)
    UT_REGISTER_TEST(VideoPacketTapTest)
#ifdef QGC_FFMPEG_STREAMING
    UT_REGISTER_TEST(FFmpegReceiverTest)
    UT_REGISTER_TEST_STANDALONE(FFmpegLoopbackBench)
#endif

    // Vehicle
    // Components
    UT_REGISTER_TEST(ComponentInformationCacheTest)
    UT_REGISTER_TEST(ComponentInformationTranslationTest)
    UT_REGISTER_TEST(FTPManagerTest)
    // UT_REGISTER_TEST(InitialConnectTest)
    UT_REGISTER_TEST(MAVLinkLogManagerTest)
    // UT_REGISTER_TEST(RequestMessageTest)
    // UT_REGISTER_TEST(SendMavCommandWithHandlerTest)
    // UT_REGISTER_TEST(SendMavCommandWithSignalingTest)
    UT_REGISTER_TEST(VehicleLinkManagerTest)

    // Missing
    // UT_REGISTER_TEST(FlightGearUnitTest)
    // UT_REGISTER_TEST(LinkManagerTest)
    // UT_REGISTER_TEST(SendMavCommandTest)
    // UT_REGISTER_TEST(TCPLinkTest)

    int result = 0;

    // G105: entries the tests saved under unit test names are erased when the
    // run ends, also after a failure (a test run never enters the event loop,
    // so nothing else would clean them up).
#ifdef QGC_CUSTOM_BUILD
    struct EraseUnitTestAccounts {
        ~EraseUnitTestAccounts() { Credential::eraseUnitTestAccounts(); }
    } eraseAtEnd;
#endif

    for (int i=0; i < (stress ? 20 : 1); i++) {
        // Run the test
        const int failures = UnitTest::run(unitTestOptions);
        if (failures == 0) {
            qDebug() << "ALL TESTS PASSED";
            result = 0;
        } else {
            qDebug() << failures << " TESTS FAILED!";
            result = -failures;
            break;
        }
    }

    return result;
}
