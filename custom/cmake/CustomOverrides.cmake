set(QGC_APP_NAME "RotorLabGC" CACHE STRING "App Name" FORCE)
set(QGC_ORG_NAME "RotorLab" CACHE STRING "Org Name" FORCE)
set(QGC_APP_COPYRIGHT "Copyright (c) 2009-2026 QGroundControl contributors and RotorLab Inc. Licensed under the GNU GPL v3." CACHE STRING "Copyright" FORCE)
# A177: ProductName and FileDescription of the Windows exe version resource.
set(QGC_WINDOWS_PRODUCT_NAME "RotorLab GC")

set(QGC_APPIMAGE_ICON_PATH "${CMAKE_SOURCE_DIR}/custom/res/icons/custom_qgroundcontrol.png" CACHE FILEPATH "AppImage Icon Path" FORCE)

if(EXISTS ${CMAKE_SOURCE_DIR}/custom/deploy/windows/WindowsQGC.ico)
    set(QGC_WINDOWS_ICON_PATH "${CMAKE_SOURCE_DIR}/custom/deploy/windows/WindowsQGC.ico" CACHE FILEPATH "Windows Icon Path" FORCE)
endif()
