cmake_minimum_required(VERSION 3.22)
project(vrstream_client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Shared protocol/FEC/packetizer core (repo root/common).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../../common vrstream_common)

find_package(OpenXR REQUIRED CONFIG)

add_library(vrstream_client SHARED
    jni_main.cpp
    client_net.cpp
    media_decoder.cpp
    xr_renderer.cpp
)

target_link_libraries(vrstream_client PRIVATE
    vrstream_common
    OpenXR::openxr_loader
    EGL
    GLESv3
    android
    mediandk
    nativewindow
    log
)
