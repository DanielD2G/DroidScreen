cmake_minimum_required(VERSION 3.14)
project(droidscreen_android CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Include the protocol library
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../protocol
    ${CMAKE_CURRENT_BINARY_DIR}/protocol
)

# Native shared library
add_library(droidscreen_native SHARED
    native_bridge.cpp
    tcp_server.cpp
    decoder.cpp
    touch_sender.cpp
    pen_sender.cpp
    mouse_sender.cpp
)

target_include_directories(droidscreen_native PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(droidscreen_native
    droidscreen_protocol
    mediandk
    android
    log
)
