# Windows PC host: capture + NVENC encode + paced UDP streaming.
add_executable(vrstream_host
    src/main.cpp
    src/session.cpp
    src/nvenc_encoder.cpp
    src/d3d_helpers.cpp
)
target_include_directories(vrstream_host PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR}/vendor)
target_link_libraries(vrstream_host PRIVATE vrstream_common)
target_compile_definitions(vrstream_host PRIVATE NOMINMAX)

if(MSVC)
    target_compile_options(vrstream_host PRIVATE /W4)
endif()
