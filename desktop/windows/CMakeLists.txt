cmake_minimum_required(VERSION 3.14)

# Find FFmpeg libraries.
# Supports: vcpkg, prebuilt BtbN builds (via -DFFMPEG_ROOT=...), or system install.
if(DEFINED FFMPEG_ROOT)
    # Prebuilt FFmpeg from BtbN or custom location.
    set(AVCODEC_INCLUDE_DIR "${FFMPEG_ROOT}/include")
    find_library(AVCODEC_LIBRARY avcodec PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
    find_library(AVUTIL_LIBRARY  avutil  PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
    find_library(SWSCALE_LIBRARY swscale PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
    message(STATUS "FFmpeg from FFMPEG_ROOT: ${FFMPEG_ROOT}")
    message(STATUS "  avcodec:  ${AVCODEC_LIBRARY}")
    message(STATUS "  avutil:   ${AVUTIL_LIBRARY}")
    message(STATUS "  swscale:  ${SWSCALE_LIBRARY}")
else()
    find_path(AVCODEC_INCLUDE_DIR libavcodec/avcodec.h)
    find_library(AVCODEC_LIBRARY avcodec)
    find_library(AVUTIL_LIBRARY avutil)
    find_library(SWSCALE_LIBRARY swscale)
endif()

add_executable(droidscreen_desktop WIN32
    src/main.cpp
    src/wgc_capturer.cpp
    src/ffmpeg_encoder.cpp
    src/mouse_injector_win.cpp
    src/touch_injector_win.cpp
    src/virtual_display_win.cpp
    src/win_app_launcher.cpp
    src/win_media_controller.cpp
    src/win_volume_controller.cpp
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
        setupapi      # SetupDi* functions (parsec-vdd)
        cfgmgr32      # CM_Get_DevNode_Status (parsec-vdd)
        gdiplus
        shlwapi
        crypt32
        ole32
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
