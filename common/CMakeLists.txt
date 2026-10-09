# Portable core: protocol, FEC, pacing, timesync, congestion, UDP socket.
add_library(vrstream_common STATIC
    src/fec.cpp
    src/packetizer.cpp
    src/udp_socket.cpp
)
target_include_directories(vrstream_common PUBLIC include)

if(WIN32)
    target_link_libraries(vrstream_common PUBLIC ws2_32)
endif()

add_executable(vrstream_tests tests/test_main.cpp)
target_link_libraries(vrstream_tests PRIVATE vrstream_common)

enable_testing()
add_test(NAME common_tests COMMAND vrstream_tests)
