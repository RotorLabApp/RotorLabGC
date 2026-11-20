set(QGC_APP_NAME "RotorLabGC" CACHE STRING "App Name" FORCE)
set(QGC_ORG_NAME "RotorLab" CACHE STRING "Org Name" FORCE)
# Android application id only, so the APK installs next to stock QGroundControl.
# QGC_PACKAGE_NAME stays upstream's: it names the Linux desktop/metainfo id and the
# iOS bundle id. Desktop settings paths come from QGC_ORG_NAME/QGC_ORG_DOMAIN and
# the app name, and the installer package dirs are fixed paths; none of them change.
# Must match the namespace in custom/android/build.gradle and the manifest package.
set(QGC_ANDROID_PACKAGE_NAME "com.rotorlab.gc" CACHE STRING "Android Package Name" FORCE)
set(QGC_APP_COPYRIGHT "Copyright (c) 2009-2026 QGroundControl contributors and RotorLab, Inc. Licensed under the GNU GPL v3." CACHE STRING "Copyright" FORCE)
# G87 (5.2.0): QGroundControl's old 3D viewer (src/Viewer3D) is removed from GC.
# Forced off so an existing build folder configured with ON loses it too. GC has
# no QML entry point for it any more (Fly view, toolstrip, Fly View settings).
set(QGC_VIEWER3D OFF CACHE BOOL "Enable Viewer3D (removed from RotorLab GC, G87)" FORCE)
# A177: ProductName and FileDescription of the Windows exe version resource.
set(QGC_WINDOWS_PRODUCT_NAME "RotorLab GC")

set(QGC_APPIMAGE_ICON_PATH "${CMAKE_SOURCE_DIR}/custom/res/icons/custom_qgroundcontrol.png" CACHE FILEPATH "AppImage Icon Path" FORCE)

if(EXISTS ${CMAKE_SOURCE_DIR}/custom/deploy/windows/WindowsQGC.ico)
    set(QGC_WINDOWS_ICON_PATH "${CMAKE_SOURCE_DIR}/custom/deploy/windows/WindowsQGC.ico" CACHE FILEPATH "Windows Icon Path" FORCE)
endif()
