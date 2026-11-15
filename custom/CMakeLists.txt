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

# Our own, custom resources
list(APPEND CUSTOM_RESOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/custom.qrc
)
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
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net/HttpClient.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net/HttpClient.h
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

set(CUSTOM_QT_COMPONENTS
    Core
    Network
    Qml
    Positioning
    CACHE INTERNAL "" FORCE
)

set(CUSTOM_LIBRARIES
    Qt6::Network
    Qt6::Qml
    Qt6::Positioning
    CACHE INTERNAL "" FORCE
)

set(CUSTOM_INCLUDE_DIRECTORIES
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tak
    ${CMAKE_CURRENT_SOURCE_DIR}/src/license
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fleet
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rotorlab
    CACHE INTERNAL "" FORCE
)

# The Fleet Connector seals its station token with DPAPI on Windows so it is
# never plaintext on disk. Crypt32 is where CryptProtectData lives.
#
# The RotorLab Connector goes further and keeps its device key out of the
# application's settings entirely, in the Windows Credential Manager.
# CredWrite/CredRead/CredFree live in Advapi32.
if(WIN32)
    set(CUSTOM_LIBRARIES ${CUSTOM_LIBRARIES} Crypt32 Advapi32 CACHE INTERNAL "" FORCE)
endif()
