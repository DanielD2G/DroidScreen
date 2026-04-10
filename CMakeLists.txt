cmake_minimum_required(VERSION 3.20)
project(DroidScreen VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(protocol)
add_subdirectory(desktop/common)

if(WIN32)
    add_subdirectory(desktop/windows)
elseif(APPLE)
    add_subdirectory(desktop/macos)
endif()
