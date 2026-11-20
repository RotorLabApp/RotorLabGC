# G88: GC's own video receiver on the FFmpeg that Qt Multimedia ships: RtpDepacketizer for
# UDP H.264/H.265 (design C1), libavformat for RTSP, MPEG-TS and TCP (design C2).
#
# FFmpegSource and RtpDepacketizer are plain Qt and always build, so their unit tests run in
# every test build. The receiver and frame bridge need FFmpeg n7.1 headers and Qt's
# FFmpeg libraries (cmake/find-modules/FindQtFFmpeg.cmake); without them GC builds as
# before, without this receiver, and says why at configure time.

target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        FFmpegSource.cc
        FFmpegSource.h
        RtpDepacketizer.cc
        RtpDepacketizer.h
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(NOT QGC_ENABLE_FFMPEG_VIDEOSTREAMING)
    return()
endif()

find_package(QtFFmpeg MODULE)
if(NOT QtFFmpeg_FOUND)
    message(WARNING "G88: QGC_ENABLE_FFMPEG_VIDEOSTREAMING is ON but the FFmpeg receiver is not built: ${QtFFmpeg_NOT_FOUND_REASON}")
    set(QGC_FFMPEG_VIDEOSTREAMING_BUILT OFF CACHE INTERNAL "")
    return()
endif()

message(STATUS "G88: FFmpeg receiver on Qt's FFmpeg (avcodec ${QtFFmpeg_VERSION}), headers ${QtFFmpeg_INCLUDE_DIR}")
set(QGC_FFMPEG_VIDEOSTREAMING_BUILT ON CACHE INTERNAL "")

target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        FFmpegFrameBridge.cc
        FFmpegFrameBridge.h
        FFmpegReceiver.cc
        FFmpegReceiver.h
)

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE QtFFmpeg::avformat QtFFmpeg::avcodec QtFFmpeg::avutil)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE QGC_FFMPEG_STREAMING)
