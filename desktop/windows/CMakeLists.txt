cmake_minimum_required(VERSION 3.14)

# Find FFmpeg libraries (vcpkg provides these automatically).
find_path(AVCODEC_INCLUDE_DIR libavcodec/avcodec.h)
find_library(AVCODEC_LIBRARY avcodec)
find_library(AVUTIL_LIBRARY avutil)
find_library(SWSCALE_LIBRARY swscale)

add_executable(droidscreen_desktop WIN32
    src/main.cpp
    src/wgc_capturer.cpp
    src/ffmpeg_encoder.cpp
    src/touch_injector_win.cpp
)

target_include_directories(droidscreen_desktop
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${AVCODEC_INCLUDE_DIR}
)

target_link_libraries(droidscreen_desktop
    PRIVATE
        droidscreen_common
        droidscreen_protocol
        ${AVCODEC_LIBRARY}
        ${AVUTIL_LIBRARY}
        ${SWSCALE_LIBRARY}
        d3d11
        dxgi
        ws2_32
        windowsapp    # WinRT / C++/WinRT support
        shell32       # Shell_NotifyIcon, SHGetFolderPath
        comctl32      # InitCommonControlsEx
        advapi32      # Registry API
)

target_compile_features(droidscreen_desktop PRIVATE cxx_std_17)

target_compile_definitions(droidscreen_desktop PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00    # Windows 10
    WINRT_LEAN_AND_MEAN
)

# Enable C++/WinRT language extensions.
set_target_properties(droidscreen_desktop PROPERTIES
    VS_GLOBAL_CppWinRTEnabled "true"
)

# Subsystem: Windows GUI application (no console window).
# WIN32 in add_executable sets /SUBSYSTEM:WINDOWS and uses WinMain entry point.
if(MSVC)
    target_link_options(droidscreen_desktop PRIVATE /SUBSYSTEM:WINDOWS)
    target_compile_options(droidscreen_desktop PRIVATE /await)
endif()
