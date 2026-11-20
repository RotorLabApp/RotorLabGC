target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        VideoPacketTap.cc
        VideoPacketTap.h
        VideoReceiver.h
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(GStreamer)
add_subdirectory(QtMultimedia)
add_subdirectory(FFmpeg)
