# qt_add_qml_module(QmlControlsTest
#     URI qmlcontrolstest
#     VERSION 1.0
#     QML_FILES
#         QGCControlDebug.qml
#         QmlTest.qml
#     IMPORT_PATH ${QT_QML_OUTPUT_DIRECTORY}
# )

# RotorLab GC 5.1.2 (G78)
target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        TerrainProfileLabelsTest.cc TerrainProfileLabelsTest.h
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
