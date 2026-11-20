# G88: find the FFmpeg that Qt Multimedia ships, for GC's own video receiver.
#
# Qt 6.8.3 ships FFmpeg n7.1 as shared libraries (Windows bin/avcodec-61.dll and friends,
# Linux lib/libavcodec.so.61, Android lib/libavcodec.so) but no headers and no Windows
# import libraries. This module needs:
#   - FFmpeg n7.1 headers: QGC_FFMPEG_INCLUDE_DIR (cache or environment), the folder that
#     holds libavcodec/avcodec.h, or the fixed folder RotorLab's build images carry
#     (C:/ffmpeg-n7.1/include, /opt/ffmpeg-n7.1/include). Never downloaded here.
#   - the libraries from Qt's own install (QT6_INSTALL_PREFIX), or QGC_FFMPEG_LIB_DIR.
#   - on Windows, import libraries generated at configure time from Qt's DLLs with the
#     MSVC tools (link /dump /exports and lib /def), written to the build folder.
#
# The header majors must match the library majors (avcodec 61, avformat 61, avutil 59
# for n7.1): GC never builds against one FFmpeg and runs on another.
#
# Result: QtFFmpeg_FOUND, QtFFmpeg_INCLUDE_DIR, QtFFmpeg_VERSION (avcodec major),
# imported targets QtFFmpeg::avcodec, QtFFmpeg::avformat, QtFFmpeg::avutil, and
# QtFFmpeg_NOT_FOUND_REASON when not found.

include(FindPackageHandleStandardArgs)

set(QtFFmpeg_NOT_FOUND_REASON "")

set(_qtff_root "${QT6_INSTALL_PREFIX}")
if(NOT _qtff_root AND Qt6_DIR)
    get_filename_component(_qtff_root "${Qt6_DIR}/../../.." ABSOLUTE)
endif()
if(NOT _qtff_root AND DEFINED ENV{QT_ROOT_DIR})
    set(_qtff_root "$ENV{QT_ROOT_DIR}")
endif()

if(NOT QGC_FFMPEG_INCLUDE_DIR AND DEFINED ENV{QGC_FFMPEG_INCLUDE_DIR})
    set(QGC_FFMPEG_INCLUDE_DIR "$ENV{QGC_FFMPEG_INCLUDE_DIR}")
endif()
if(NOT QGC_FFMPEG_LIB_DIR AND DEFINED ENV{QGC_FFMPEG_LIB_DIR})
    set(QGC_FFMPEG_LIB_DIR "$ENV{QGC_FFMPEG_LIB_DIR}")
endif()

# Headers: only where we are told, or inside Qt's own folder. Not the system include
# folders: a distribution's FFmpeg (Ubuntu 24.04 has 6.1, avcodec 60) is another ABI.
find_path(QtFFmpeg_INCLUDE_DIR
    NAMES libavcodec/avcodec.h
    HINTS "${QGC_FFMPEG_INCLUDE_DIR}" "${_qtff_root}/include/ffmpeg" "${_qtff_root}/include"
          "C:/ffmpeg-n7.1/include" "/opt/ffmpeg-n7.1/include"
    NO_DEFAULT_PATH
)

function(_qtff_header_major lib out)
    set(_major "")
    foreach(_f version_major.h version.h)
        set(_h "${QtFFmpeg_INCLUDE_DIR}/lib${lib}/${_f}")
        if(EXISTS "${_h}")
            string(TOUPPER "${lib}" _LIB)
            file(STRINGS "${_h}" _line REGEX "#define[ \t]+LIB${_LIB}_VERSION_MAJOR[ \t]+[0-9]+")
            if(_line)
                string(REGEX REPLACE ".*MAJOR[ \t]+([0-9]+).*" "\\1" _major "${_line}")
                break()
            endif()
        endif()
    endforeach()
    set(${out} "${_major}" PARENT_SCOPE)
endfunction()

if(NOT QtFFmpeg_INCLUDE_DIR)
    set(QtFFmpeg_NOT_FOUND_REASON "no FFmpeg headers (libavcodec/avcodec.h): set QGC_FFMPEG_INCLUDE_DIR to FFmpeg n7.1's include folder (Qt ships no FFmpeg headers)")
else()
    set(_qtff_libdir "${QGC_FFMPEG_LIB_DIR}")
    if(NOT _qtff_libdir)
        if(WIN32)
            set(_qtff_libdir "${_qtff_root}/bin")
        else()
            set(_qtff_libdir "${_qtff_root}/lib")
        endif()
    endif()

    set(_qtff_gen "${CMAKE_BINARY_DIR}/qtffmpeg")
    foreach(_lib avcodec avformat avutil)
        _qtff_header_major(${_lib} _major)
        if(NOT _major)
            set(QtFFmpeg_NOT_FOUND_REASON "cannot read the ${_lib} version from ${QtFFmpeg_INCLUDE_DIR}")
            break()
        endif()
        if(_lib STREQUAL "avcodec")
            set(QtFFmpeg_VERSION "${_major}")
        endif()

        if(WIN32)
            set(_dll "${_qtff_libdir}/${_lib}-${_major}.dll")
            if(NOT EXISTS "${_dll}")
                set(QtFFmpeg_NOT_FOUND_REASON "headers say ${_lib} ${_major} but ${_dll} does not exist")
                break()
            endif()
            set(_implib "${_qtff_gen}/${_lib}.lib")
            if(NOT EXISTS "${_implib}" OR "${_dll}" IS_NEWER_THAN "${_implib}")
                # Import library from the DLL's export table (MSVC: link /dump /exports, lib /def)
                get_filename_component(_tooldir "${CMAKE_LINKER}" DIRECTORY)
                file(MAKE_DIRECTORY "${_qtff_gen}")
                execute_process(COMMAND "${CMAKE_LINKER}" /dump /exports "${_dll}"
                    OUTPUT_VARIABLE _exports RESULT_VARIABLE _rc)
                if(NOT _rc EQUAL 0)
                    set(QtFFmpeg_NOT_FOUND_REASON "link /dump /exports failed on ${_dll}")
                    break()
                endif()
                string(REPLACE "\n" ";" _lines "${_exports}")
                set(_def "LIBRARY ${_lib}-${_major}\nEXPORTS\n")
                foreach(_l IN LISTS _lines)
                    if(_l MATCHES "^[ \t]+[0-9]+[ \t]+[0-9A-Fa-f]+[ \t]+[0-9A-Fa-f]+[ \t]+([A-Za-z_][A-Za-z0-9_@]*)")
                        string(APPEND _def "    ${CMAKE_MATCH_1}\n")
                    endif()
                endforeach()
                file(WRITE "${_qtff_gen}/${_lib}.def" "${_def}")
                set(_libexe "${CMAKE_AR}")
                if(NOT _libexe)
                    set(_libexe "${_tooldir}/lib.exe")
                endif()
                execute_process(COMMAND "${_libexe}" /nologo /machine:x64 "/def:${_qtff_gen}/${_lib}.def" "/out:${_implib}"
                    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
                if(NOT _rc EQUAL 0)
                    set(QtFFmpeg_NOT_FOUND_REASON "lib /def failed for ${_lib}: ${_out} ${_err}")
                    break()
                endif()
            endif()
            if(NOT TARGET QtFFmpeg::${_lib})
                add_library(QtFFmpeg::${_lib} SHARED IMPORTED GLOBAL)
                set_target_properties(QtFFmpeg::${_lib} PROPERTIES
                    IMPORTED_LOCATION "${_dll}"
                    IMPORTED_IMPLIB "${_implib}"
                    INTERFACE_INCLUDE_DIRECTORIES "${QtFFmpeg_INCLUDE_DIR}")
            endif()
        else()
            if(ANDROID)
                set(_so "${_qtff_libdir}/lib${_lib}.so")
            else()
                set(_so "${_qtff_libdir}/lib${_lib}.so.${_major}")
            endif()
            if(NOT EXISTS "${_so}")
                set(QtFFmpeg_NOT_FOUND_REASON "headers say ${_lib} ${_major} but ${_so} does not exist")
                break()
            endif()
            if(NOT TARGET QtFFmpeg::${_lib})
                add_library(QtFFmpeg::${_lib} SHARED IMPORTED GLOBAL)
                set_target_properties(QtFFmpeg::${_lib} PROPERTIES
                    IMPORTED_LOCATION "${_so}"
                    IMPORTED_NO_SONAME FALSE
                    INTERFACE_INCLUDE_DIRECTORIES "${QtFFmpeg_INCLUDE_DIR}")
            endif()
        endif()
        list(APPEND QtFFmpeg_LIBRARIES QtFFmpeg::${_lib})
    endforeach()
endif()

# G109: swscale (scaling for the relay's re-encode). Optional and separate: if it is missing
# or cannot be imported, QtFFmpeg::swscale simply does not exist and the receiver is unaffected.
if(NOT QtFFmpeg_NOT_FOUND_REASON AND QtFFmpeg_INCLUDE_DIR)
    foreach(_lib swscale)
        set(_qtff_opt_reason "")
        _qtff_header_major(${_lib} _major)
        if(NOT _major)
            set(_qtff_opt_reason "no ${_lib} headers in ${QtFFmpeg_INCLUDE_DIR}")
        elseif(WIN32)
            set(_dll "${_qtff_libdir}/${_lib}-${_major}.dll")
            set(_implib "${_qtff_gen}/${_lib}.lib")
            if(NOT EXISTS "${_dll}")
                set(_qtff_opt_reason "${_dll} does not exist")
            elseif(NOT EXISTS "${_implib}" OR "${_dll}" IS_NEWER_THAN "${_implib}")
                get_filename_component(_tooldir "${CMAKE_LINKER}" DIRECTORY)
                file(MAKE_DIRECTORY "${_qtff_gen}")
                execute_process(COMMAND "${CMAKE_LINKER}" /dump /exports "${_dll}"
                    OUTPUT_VARIABLE _exports RESULT_VARIABLE _rc)
                if(NOT _rc EQUAL 0)
                    set(_qtff_opt_reason "link /dump /exports failed on ${_dll}")
                else()
                    string(REPLACE "\n" ";" _lines "${_exports}")
                    set(_def "LIBRARY ${_lib}-${_major}\nEXPORTS\n")
                    foreach(_l IN LISTS _lines)
                        if(_l MATCHES "^[ \t]+[0-9]+[ \t]+[0-9A-Fa-f]+[ \t]+[0-9A-Fa-f]+[ \t]+([A-Za-z_][A-Za-z0-9_@]*)")
                            string(APPEND _def "    ${CMAKE_MATCH_1}\n")
                        endif()
                    endforeach()
                    file(WRITE "${_qtff_gen}/${_lib}.def" "${_def}")
                    set(_libexe "${CMAKE_AR}")
                    if(NOT _libexe)
                        set(_libexe "${_tooldir}/lib.exe")
                    endif()
                    execute_process(COMMAND "${_libexe}" /nologo /machine:x64 "/def:${_qtff_gen}/${_lib}.def" "/out:${_implib}"
                        RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_QUIET)
                    if(NOT _rc EQUAL 0)
                        set(_qtff_opt_reason "lib /def failed for ${_lib}")
                    endif()
                endif()
            endif()
            if(NOT _qtff_opt_reason AND NOT TARGET QtFFmpeg::${_lib})
                add_library(QtFFmpeg::${_lib} SHARED IMPORTED GLOBAL)
                set_target_properties(QtFFmpeg::${_lib} PROPERTIES
                    IMPORTED_LOCATION "${_dll}"
                    IMPORTED_IMPLIB "${_implib}"
                    INTERFACE_INCLUDE_DIRECTORIES "${QtFFmpeg_INCLUDE_DIR}")
            endif()
        else()
            if(ANDROID)
                set(_so "${_qtff_libdir}/lib${_lib}.so")
            else()
                set(_so "${_qtff_libdir}/lib${_lib}.so.${_major}")
            endif()
            if(NOT EXISTS "${_so}")
                set(_qtff_opt_reason "${_so} does not exist")
            elseif(NOT TARGET QtFFmpeg::${_lib})
                add_library(QtFFmpeg::${_lib} SHARED IMPORTED GLOBAL)
                set_target_properties(QtFFmpeg::${_lib} PROPERTIES
                    IMPORTED_LOCATION "${_so}"
                    IMPORTED_NO_SONAME FALSE
                    INTERFACE_INCLUDE_DIRECTORIES "${QtFFmpeg_INCLUDE_DIR}")
            endif()
        endif()
        if(_qtff_opt_reason)
            message(STATUS "QtFFmpeg: optional ${_lib} not available (${_qtff_opt_reason})")
        else()
            message(STATUS "QtFFmpeg: optional ${_lib} ${_major} available as QtFFmpeg::${_lib}")
        endif()
    endforeach()
endif()

if(QtFFmpeg_NOT_FOUND_REASON)
    # a ';' in the text would split it into arguments of find_package_handle_standard_args
    string(REPLACE ";" "," QtFFmpeg_NOT_FOUND_REASON "${QtFFmpeg_NOT_FOUND_REASON}")
    unset(QtFFmpeg_LIBRARIES)
    set(_qtff_ok "")
else()
    set(_qtff_ok TRUE)
endif()

find_package_handle_standard_args(QtFFmpeg
    REQUIRED_VARS QtFFmpeg_INCLUDE_DIR _qtff_ok
    VERSION_VAR QtFFmpeg_VERSION
    REASON_FAILURE_MESSAGE "${QtFFmpeg_NOT_FOUND_REASON}"
)
