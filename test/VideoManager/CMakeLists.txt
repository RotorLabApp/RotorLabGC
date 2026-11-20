# G88: FFmpegSource and RtpDepacketizer are plain Qt and always tested. The receiver's
# own tests and the loopback bench need the FFmpeg receiver; the bench runs only when asked
# for by name.
target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        FFmpegSourceTest.cc
        FFmpegSourceTest.h
        RtpDepacketizerTest.cc
        RtpDepacketizerTest.h
        VideoPacketTapTest.cc
        VideoPacketTapTest.h
)

if(QGC_FFMPEG_VIDEOSTREAMING_BUILT)
    target_sources(${CMAKE_PROJECT_NAME}
        PRIVATE
            FFmpegLoopbackBench.cc
            FFmpegLoopbackBench.h
            FFmpegReceiverTest.cc
            FFmpegReceiverTest.h
    )
endif()

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
