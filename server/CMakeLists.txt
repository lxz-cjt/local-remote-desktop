cmake_minimum_required(VERSION 3.21)
project(LocalRemoteServer VERSION 0.2.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "LocalRemoteServer requires Windows")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(FFMPEG_ROOT "" CACHE PATH "Optional FFmpeg prefix containing include, lib and bin")
set(OPENSSL_ROOT_DIR "" CACHE PATH "Optional OpenSSL installation prefix")

find_package(OpenSSL REQUIRED)
find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h HINTS "${FFMPEG_ROOT}/include" REQUIRED)
find_library(AVCODEC_LIBRARY NAMES avcodec HINTS "${FFMPEG_ROOT}/lib" REQUIRED)
find_library(AVUTIL_LIBRARY NAMES avutil HINTS "${FFMPEG_ROOT}/lib" REQUIRED)
find_library(SWSCALE_LIBRARY NAMES swscale HINTS "${FFMPEG_ROOT}/lib" REQUIRED)

add_executable(LocalRemoteServer
    src/main.cpp
    src/certificate.cpp
    src/desktop_capture.cpp
    src/video_encoder.cpp
    src/audio_capture.cpp
    src/audio_manager.cpp
    src/network_server.cpp
    src/input_injector.cpp
)

target_compile_definitions(LocalRemoteServer PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WINSOCKAPI_
    LRDC_VERSION="${PROJECT_VERSION}"
)
if(MSVC)
    target_compile_options(LocalRemoteServer PRIVATE /utf-8 /W4)
endif()

target_include_directories(LocalRemoteServer PRIVATE src "${FFMPEG_INCLUDE_DIR}")
target_link_libraries(LocalRemoteServer PRIVATE
    "${AVCODEC_LIBRARY}"
    "${AVUTIL_LIBRARY}"
    "${SWSCALE_LIBRARY}"
    OpenSSL::SSL
    OpenSSL::Crypto
    d3d11
    dxgi
    ole32
    oleaut32
    mmdevapi
    avrt
    ws2_32
    bcrypt
)

if(FFMPEG_ROOT AND EXISTS "${FFMPEG_ROOT}/bin")
    file(GLOB FFMPEG_DLLS "${FFMPEG_ROOT}/bin/*.dll")
    foreach(dll IN LISTS FFMPEG_DLLS)
        add_custom_command(TARGET LocalRemoteServer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" $<TARGET_FILE_DIR:LocalRemoteServer>
        )
    endforeach()
endif()

if(OPENSSL_ROOT_DIR AND EXISTS "${OPENSSL_ROOT_DIR}/bin")
    file(GLOB OPENSSL_DLLS
        "${OPENSSL_ROOT_DIR}/bin/libssl*.dll"
        "${OPENSSL_ROOT_DIR}/bin/libcrypto*.dll"
        "${OPENSSL_ROOT_DIR}/bin/ssleay32*.dll"
        "${OPENSSL_ROOT_DIR}/bin/libeay32*.dll"
    )
    foreach(dll IN LISTS OPENSSL_DLLS)
        add_custom_command(TARGET LocalRemoteServer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" $<TARGET_FILE_DIR:LocalRemoteServer>
        )
    endforeach()
endif()
