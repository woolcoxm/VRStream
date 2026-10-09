# SteamVR driver DLL: virtual HMD + DirectMode capture + NVENC + loopback feed.
add_library(driver_vrstream SHARED
    src/vrstream_driver.cpp
    ../src/nvenc_encoder.cpp
    ../src/d3d_helpers.cpp
)
target_include_directories(driver_vrstream PRIVATE src vendor ../vendor ../src)
target_link_libraries(driver_vrstream PRIVATE vrstream_common)
target_compile_definitions(driver_vrstream PRIVATE NOMINMAX)

set_target_properties(driver_vrstream PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/driver-root/vrstream"
)
add_custom_command(TARGET driver_vrstream POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/driver.vrdrivermanifest"
        "$<TARGET_FILE_DIR:driver_vrstream>/driver.vrdrivermanifest"
)
