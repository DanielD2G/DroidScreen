cmake_minimum_required(VERSION 3.14)

add_library(droidscreen_common STATIC
    src/server.cpp
    src/pipeline.cpp
    src/rate_controller.cpp
)

target_include_directories(droidscreen_common
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(droidscreen_common
    PUBLIC
        droidscreen_protocol
)

target_compile_features(droidscreen_common PUBLIC cxx_std_17)

# Link Winsock on Windows.
if(WIN32)
    target_link_libraries(droidscreen_common PUBLIC ws2_32)
endif()
