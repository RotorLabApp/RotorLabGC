target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        KMLDomDocument.cc
        KMLDomDocument.h
        KMLHelper.cc
        KMLHelper.h
        ShapeFileHelper.cc
        ShapeFileHelper.h
        SHPFileHelper.cc
        SHPFileHelper.h
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

#===========================================================================#

CPMAddPackage(
    NAME Shapelib
    VERSION 1.6.1
    GITHUB_REPOSITORY OSGeo/shapelib
    OPTIONS
        "BUILD_SHAPELIB_CONTRIB OFF"
        "BUILD_APPS OFF"
        "BUILD_TESTING OFF"
    # RotorLab: G108. Built and linked into GC only: EXCLUDE_FROM_ALL keeps its
    # install rules (static library, header, CMake and pkg-config files) out of
    # the release files.
    EXCLUDE_FROM_ALL YES
)

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE shp)
