message(STATUS "QGC: Adding RotorLab GC Custom Plugin")

# Enable custom build
set_property(DIRECTORY ${CMAKE_SOURCE_DIR}
    APPEND PROPERTY COMPILE_DEFINITIONS
    QGC_CUSTOM_BUILD
    CUSTOMHEADER="CustomPlugin.h"
    CUSTOMCLASS=CustomPlugin
)

if(ANDROID)
    set(CUSTOM_ANDROID_DIR "${CMAKE_SOURCE_DIR}/custom/android")
    if(EXISTS "${CUSTOM_ANDROID_DIR}")
        file(GLOB CUSTOM_ANDROID_FILES "${CUSTOM_ANDROID_DIR}/*")
        if(CUSTOM_ANDROID_FILES)
            message(STATUS "QGC: Custom Android package template found. Overlaying custom files...")
            set(DEFAULT_ANDROID_DIR "${CMAKE_SOURCE_DIR}/android")
            set(FINAL_ANDROID_DIR "${CMAKE_BINARY_DIR}/custom/android")
            file(COPY "${DEFAULT_ANDROID_DIR}/." DESTINATION "${FINAL_ANDROID_DIR}")
            # RotorLab: overwrite with every custom file, unconditionally. file(COPY)
            # skips a file whose timestamp matches the one already copied from
            # android/ (as happens in a fresh checkout), so the custom manifest
            # (package com.rotorlab.gc), icons and splash were silently dropped.
            # The copies get the current time so androiddeployqt, which copies only
            # newer files into android-build, takes them on an incremental build too.
            file(GLOB_RECURSE CUSTOM_ANDROID_OVERLAY RELATIVE "${CUSTOM_ANDROID_DIR}" "${CUSTOM_ANDROID_DIR}/*")
            foreach(_overlay_file IN LISTS CUSTOM_ANDROID_OVERLAY)
                get_filename_component(_overlay_dir "${FINAL_ANDROID_DIR}/${_overlay_file}" DIRECTORY)
                file(MAKE_DIRECTORY "${_overlay_dir}")
                file(COPY_FILE "${CUSTOM_ANDROID_DIR}/${_overlay_file}" "${FINAL_ANDROID_DIR}/${_overlay_file}")
                file(TOUCH_NOCREATE "${FINAL_ANDROID_DIR}/${_overlay_file}")
            endforeach()
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                            "${DEFAULT_ANDROID_DIR}/"
                            "${CUSTOM_ANDROID_DIR}/"
                        )
            set(QGC_ANDROID_PACKAGE_SOURCE_DIR "${FINAL_ANDROID_DIR}" CACHE PATH "Path to a custom Android package template" FORCE)
            message(STATUS "QGC: Android package template path will be set to: ${QGC_ANDROID_PACKAGE_SOURCE_DIR}")
        endif()
    endif()
endif()

# G89: the RotorLab license signing public keys GC trusts for the signed
# offline activation record, from custom/license-pubkeys.txt (64 hex characters
# per key, one per line, # starts a comment). Public keys only: RotorLab's
# private key is never in this repository. A stable (release) build refuses to
# configure without one, because such a GC could never be activated.
set(_rlgc_pubkey_file "${CMAKE_CURRENT_SOURCE_DIR}/license-pubkeys.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_rlgc_pubkey_file}")
set(_rlgc_pubkeys "")
if(EXISTS "${_rlgc_pubkey_file}")
    file(STRINGS "${_rlgc_pubkey_file}" _rlgc_pubkey_lines)
    foreach(_line IN LISTS _rlgc_pubkey_lines)
        string(REGEX REPLACE "#.*$" "" _line "${_line}")
        string(STRIP "${_line}" _line)
        if(_line STREQUAL "")
            continue()
        endif()
        string(TOLOWER "${_line}" _line)
        if(NOT _line MATCHES "^[0-9a-f]+$")
            message(FATAL_ERROR "custom/license-pubkeys.txt: not a hex public key: ${_line}")
        endif()
        string(LENGTH "${_line}" _len)
        if(NOT _len EQUAL 64)
            message(FATAL_ERROR "custom/license-pubkeys.txt: a public key is 64 hex characters: ${_line}")
        endif()
        list(APPEND _rlgc_pubkeys "${_line}")
    endforeach()
endif()
# The shared RotorLab test fixture key (tests/fixtures/license; its seed is
# SHA-256 of a public string) must never be trusted by any build.
if("bf3e3dd99f4e69b8f60f204da016d4e496b3fe312047871a154f061fcc112f54" IN_LIST _rlgc_pubkeys)
    message(FATAL_ERROR "custom/license-pubkeys.txt lists the public RotorLab test fixture key (kid 73b5467a18b55e25), whose seed is public; remove it.")
endif()
list(JOIN _rlgc_pubkeys "," _rlgc_pubkeys_joined)
if(QGC_STABLE_BUILD AND _rlgc_pubkeys_joined STREQUAL "")
    message(FATAL_ERROR "QGC_STABLE_BUILD needs at least one RotorLab license public key in custom/license-pubkeys.txt (G89).")
endif()
list(LENGTH _rlgc_pubkeys _rlgc_pubkey_count)
message(STATUS "QGC: RotorLab license public keys pinned: ${_rlgc_pubkey_count}")
set_property(DIRECTORY ${CMAKE_SOURCE_DIR}
    APPEND PROPERTY COMPILE_DEFINITIONS
    RLGC_LICENSE_PUBKEYS="${_rlgc_pubkeys_joined}"
)
# A stable build says so to the compiler too, so the test-key hooks
# (#ifdef QGC_DAILY_BUILD in LicenseRecord.cc and RotorLabStore.cc) can refuse
# to compile into it (#error there).
if(QGC_STABLE_BUILD)
    set_property(DIRECTORY ${CMAKE_SOURCE_DIR}
        APPEND PROPERTY COMPILE_DEFINITIONS
        RLGC_STABLE_BUILD
    )
endif()

# Our own, custom resources
list(APPEND CUSTOM_RESOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/custom.qrc
)

# G64 (MG2, decision 757): the Plan view 3D view is built for Windows and
# Linux only. Android (and iOS) keep the 2D Plan view: none of these files is
# compiled there, RL_PLAN3D is not defined, and the "3D" toggle never shows.
# It needs Qt Quick 3D only, not QGC's old Viewer3D: G87 forces QGC_VIEWER3D
# OFF in every GC build (custom/cmake/CustomOverrides.cmake).
if(NOT ANDROID AND NOT IOS)
    set(RL_PLAN3D ON CACHE INTERNAL "" FORCE)
    list(APPEND CUSTOM_RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/res/plan3d/plan3d.qrc)
    if(NOT QGC_STABLE_BUILD)
        # Test-only autorun (G64 timing, G65 drag runs): Daily builds only, never in a stable build.
        list(APPEND CUSTOM_RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/res/plan3d/plan3d-daily.qrc)
    endif()
else()
    set(RL_PLAN3D OFF CACHE INTERNAL "" FORCE)
endif()
# Guard (G64 M4): the 3D Plan view must never drop out of a Windows or Linux
# build without a word, as it would have when G87 turned QGC_VIEWER3D off.
if((WIN32 OR LINUX) AND NOT ANDROID)
    if(NOT RL_PLAN3D)
        message(FATAL_ERROR "G64: RL_PLAN3D is OFF on Windows or Linux; the 3D Plan view would be missing from this build.")
    endif()
    if(NOT TARGET Qt6::Quick3D)
        message(FATAL_ERROR "G64: the 3D Plan view needs Qt Quick 3D (Qt6::Quick3D), which this Qt does not provide.")
    endif()
endif()
message(STATUS "RotorLab 3D Plan view (RL_PLAN3D): ${RL_PLAN3D}")
# G109: the relay's QML, in its own qrc so a tree without
# src/video/relay still builds.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/video/relay/qml/VideoRelaySettings.qml)
    list(APPEND CUSTOM_RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/video-relay.qrc)
endif()
set(QGC_RESOURCES ${QGC_RESOURCES} ${CUSTOM_RESOURCES} CACHE STRING "Paths to .qrc Resources" FORCE)

set(CUSTOM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/CustomPlugin.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/CustomPlugin.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tak/CoTMessage.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tak/CoTMessage.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tak/TakCommand.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tak/TakCommand.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tak/TakManager.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tak/TakManager.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tak/TakDefaults.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tak/TakPolicy.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tak/TakPolicy.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/license/LicenseManager.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/license/LicenseManager.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/license/LicenseRecord.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/license/LicenseRecord.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/license/Ed25519.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/license/Ed25519.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/license/third_party/monocypher/monocypher.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/license/third_party/monocypher/monocypher-ed25519.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net/HttpClient.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net/HttpClient.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net/LogCollectPolicy.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net/LogCollectPolicy.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net/LogCollector.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net/LogCollector.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fleet/GuidedDispatch.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fleet/GuidedDispatch.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fleet/FleetStore.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fleet/FleetStore.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fleet/FleetClient.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fleet/FleetClient.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fleet/FleetTelemetry.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fleet/FleetTelemetry.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fleet/FleetChecklists.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fleet/FleetChecklists.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fleet/FleetLogs.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fleet/FleetLogs.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fleet/FleetConnector.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fleet/FleetConnector.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rotorlab/Credential.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rotorlab/Credential.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rotorlab/RotorLabStore.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rotorlab/RotorLabStore.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rotorlab/RotorLabClient.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rotorlab/RotorLabClient.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rotorlab/RotorLabRoster.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rotorlab/RotorLabRoster.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rotorlab/AirframeResolver.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rotorlab/AirframeResolver.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rotorlab/FlightWatcher.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rotorlab/FlightWatcher.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rotorlab/MissionLibrary.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rotorlab/MissionLibrary.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rotorlab/FilingRules.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rotorlab/FilingRules.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rotorlab/FlightSpool.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rotorlab/FlightSpool.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rotorlab/PicLog.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rotorlab/PicLog.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rotorlab/RotorLabConnector.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rotorlab/RotorLabConnector.h
    CACHE INTERNAL "" FORCE
)

if(RL_PLAN3D)
    set(CUSTOM_SOURCES ${CUSTOM_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/plan3d/Plan3DBuilder.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/plan3d/Plan3DBuilder.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/plan3d/Plan3DColors.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/plan3d/Plan3DEditor.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/plan3d/Plan3DEditor.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/plan3d/Plan3DGeometry.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/plan3d/Plan3DGeometry.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/plan3d/Plan3DGround.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/plan3d/Plan3DGround.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/plan3d/Plan3DGroundMesh.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/plan3d/Plan3DGroundMesh.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/plan3d/Plan3DMath.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/plan3d/Plan3DMath.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/plan3d/Plan3DScene.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/plan3d/Plan3DScene.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/plan3d/Plan3DSource.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/plan3d/Plan3DSource.h
        CACHE INTERNAL "" FORCE
    )
    if(NOT QGC_STABLE_BUILD)
        # Test-only frame-time probe for the G64 timing runs; never in a stable build.
        set(CUSTOM_SOURCES ${CUSTOM_SOURCES}
            ${CMAKE_CURRENT_SOURCE_DIR}/src/plan3d/Plan3DProbe.cc
            ${CMAKE_CURRENT_SOURCE_DIR}/src/plan3d/Plan3DProbe.h
            CACHE INTERNAL "" FORCE
        )
    endif()
endif()

# G95 and G109 (video recording and Fleet live relay). The plain Qt parts
# build everywhere; the parts on libavformat build only where GC's own FFmpeg
# receiver is built (QGC_FFMPEG_VIDEOSTREAMING_BUILT, C++ QGC_FFMPEG_STREAMING).
# That variable is set by src/VideoManager/VideoReceiver/FFmpeg, which is
# configured after this directory, so those sources are added by a call
# deferred to the end of the top-level directory.
set(CUSTOM_SOURCES ${CUSTOM_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/video/common/VideoCodecInfo.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/video/common/VideoCodecInfo.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/video/common/VideoTapRegistry.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/video/common/VideoTapRegistry.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/video/record/RecordingRecovery.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/video/record/RecordingRecovery.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/video/record/RecordingStore.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/video/record/RecordingStore.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/video/record/TimeMapWriter.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/video/record/TimeMapWriter.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/video/record/VehicleClock.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/video/record/VehicleClock.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/video/record/VideoRecorder.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/video/record/VideoRecorder.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/video/record/VideoUploader.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/video/record/VideoUploader.h
    CACHE INTERNAL "" FORCE
)
set(RLGC_VIDEO_FFMPEG_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/video/common/Fmp4Muxer.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/video/common/Fmp4Muxer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/video/record/RecordingWriter.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/video/record/RecordingWriter.h
    CACHE INTERNAL "" FORCE
)
# G109: live relay sources, only where the relay folder exists.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/video/relay/VideoRelay.cc)
    set(RLGC_VIDEO_FFMPEG_SOURCES ${RLGC_VIDEO_FFMPEG_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/video/relay/EncoderProbe.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/video/relay/EncoderProbe.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/video/relay/FleetLiveTransport.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/video/relay/FleetLiveTransport.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/video/relay/LiveFragmenter.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/video/relay/LiveFragmenter.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/video/relay/LiveSession.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/video/relay/LiveSession.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/video/relay/LiveTransport.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/video/relay/RelayEncoder.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/video/relay/RelayEncoder.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/video/relay/RelayPipeline.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/video/relay/RelayPipeline.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/video/relay/RelayTypes.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/video/relay/RelayTypes.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/video/relay/VideoRelay.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/video/relay/VideoRelay.h
        CACHE INTERNAL "" FORCE
    )
    set(RLGC_VIDEO_RELAY ON CACHE INTERNAL "" FORCE)
else()
    set(RLGC_VIDEO_RELAY OFF CACHE INTERNAL "" FORCE)
endif()
set(RLGC_CUSTOM_DIR ${CMAKE_CURRENT_SOURCE_DIR} CACHE INTERNAL "")
function(rlgc_add_video_ffmpeg_sources)
    # The time map header names the GC commit; only this one file sees it, so a
    # new commit does not rebuild the whole application.
    set_source_files_properties(${RLGC_CUSTOM_DIR}/src/video/record/VideoRecorder.cc
        PROPERTIES COMPILE_DEFINITIONS "RLGC_GIT_COMMIT=\"${QGC_GIT_HASH}\"")

    # G95 tests (test/Custom/Video): registering and running them is
    # test/CMakeLists.txt's and test/UnitTestList.cc's job. Until they are
    # listed there, a testing build compiles them here so they cannot rot;
    # once test/CMakeLists.txt lists Fmp4MuxerTest.cc this does nothing.
    set(_rlgc_vtests "")
    if(QGC_BUILD_TESTING AND EXISTS "${CMAKE_SOURCE_DIR}/test/Custom/Video/VideoUploaderTest.cc")
        get_target_property(_rlgc_srcs ${CMAKE_PROJECT_NAME} SOURCES)
        list(FILTER _rlgc_srcs INCLUDE REGEX "VideoUploaderTest\\.cc$")
        if(NOT _rlgc_srcs)
            set(_rlgc_vtests ${CMAKE_SOURCE_DIR}/test/Custom/Video)
            target_sources(${CMAKE_PROJECT_NAME} PRIVATE
                ${_rlgc_vtests}/VideoTestStreams.h
                ${_rlgc_vtests}/RecordingRecoveryTest.cc ${_rlgc_vtests}/RecordingRecoveryTest.h
                ${_rlgc_vtests}/VideoUploaderTest.cc ${_rlgc_vtests}/VideoUploaderTest.h)
        endif()
    endif()

    if(NOT QGC_FFMPEG_VIDEOSTREAMING_BUILT)
        message(STATUS "RotorLab video recording and relay (G95, G109): not built (no FFmpeg receiver in this build)")
        return()
    endif()
    message(STATUS "RotorLab video recording and relay (G95, G109): built on the FFmpeg receiver")
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${RLGC_VIDEO_FFMPEG_SOURCES})
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE QtFFmpeg::avformat QtFFmpeg::avcodec QtFFmpeg::avutil)
    if(RLGC_VIDEO_RELAY)
        target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${RLGC_CUSTOM_DIR}/src/video/relay)
        # swscale (Low and Medium) only where FindQtFFmpeg provides it.
        if(TARGET QtFFmpeg::swscale)
            target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE QtFFmpeg::swscale)
        endif()
    endif()
    if(_rlgc_vtests)
        target_sources(${CMAKE_PROJECT_NAME} PRIVATE
            ${_rlgc_vtests}/Fmp4MuxerTest.cc ${_rlgc_vtests}/Fmp4MuxerTest.h
            ${_rlgc_vtests}/RecordingWriterTest.cc ${_rlgc_vtests}/RecordingWriterTest.h
            ${_rlgc_vtests}/TimeMapWriterTest.cc ${_rlgc_vtests}/TimeMapWriterTest.h)
    endif()
endfunction()
cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL rlgc_add_video_ffmpeg_sources)

set(CUSTOM_QT_COMPONENTS
    Core
    Network
    Qml
    Positioning
    CACHE INTERNAL "" FORCE
)

# G89: Monocypher 4.0.3 (vendored unmodified; BSD-2-Clause or CC0-1.0) for
# the Ed25519 check of the signed offline activation record. Its own C99
# static library, so the main target's Qt C++ precompiled headers never reach
# these C files. Warnings are silenced here only; no optimization or math
# flags change for it.
add_library(rlgc_monocypher STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/license/third_party/monocypher/monocypher.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/license/third_party/monocypher/monocypher.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/license/third_party/monocypher/monocypher-ed25519.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/license/third_party/monocypher/monocypher-ed25519.h
)
set_target_properties(rlgc_monocypher PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(rlgc_monocypher PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/license/third_party/monocypher
)
target_compile_options(rlgc_monocypher PRIVATE
    $<$<C_COMPILER_ID:MSVC>:/W3 /wd4146 /wd4244 /wd4267>
    $<$<NOT:$<C_COMPILER_ID:MSVC>>:-w>
)

set(CUSTOM_LIBRARIES
    Qt6::Network
    Qt6::Qml
    Qt6::Positioning
    rlgc_monocypher
    CACHE INTERNAL "" FORCE
)

if(RL_PLAN3D)
    set(CUSTOM_QT_COMPONENTS ${CUSTOM_QT_COMPONENTS} Quick3D CACHE INTERNAL "" FORCE)
    set(CUSTOM_LIBRARIES ${CUSTOM_LIBRARIES} Qt6::Quick3D CACHE INTERNAL "" FORCE)
    set_property(DIRECTORY ${CMAKE_SOURCE_DIR} APPEND PROPERTY COMPILE_DEFINITIONS RL_PLAN3D)
endif()

set(CUSTOM_INCLUDE_DIRECTORIES
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tak
    ${CMAKE_CURRENT_SOURCE_DIR}/src/license
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fleet
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rotorlab
    ${CMAKE_CURRENT_SOURCE_DIR}/src/video/common
    ${CMAKE_CURRENT_SOURCE_DIR}/src/video/record
    CACHE INTERNAL "" FORCE
)
if(RL_PLAN3D)
    set(CUSTOM_INCLUDE_DIRECTORIES ${CUSTOM_INCLUDE_DIRECTORIES} ${CMAKE_CURRENT_SOURCE_DIR}/src/plan3d CACHE INTERNAL "" FORCE)
endif()

# The Fleet Connector seals its station token with DPAPI on Windows so it is
# never plaintext on disk. Crypt32 is where CryptProtectData lives.
#
# The RotorLab Connector goes further and keeps its device key out of the
# application's settings entirely, in the Windows Credential Manager.
# CredWrite/CredRead/CredFree live in Advapi32.
if(WIN32)
    set(CUSTOM_LIBRARIES ${CUSTOM_LIBRARIES} Crypt32 Advapi32 CACHE INTERNAL "" FORCE)
endif()
