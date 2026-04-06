cmake_minimum_required(VERSION 3.14)
project(droidscreen_protocol C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(droidscreen_protocol STATIC
    src/protocol.c
    src/handshake.c
    src/frame.c
    src/touch.c
    src/pen.c
    src/mouse.c
)

target_include_directories(droidscreen_protocol
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
