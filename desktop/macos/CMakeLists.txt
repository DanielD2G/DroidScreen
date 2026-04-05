cmake_minimum_required(VERSION 3.14)

# Enable Objective-C++ for .mm files.
enable_language(OBJCXX)

add_executable(droidscreen_desktop
    src/main.mm
    src/sck_capturer.mm
    src/vt_encoder.mm
)

target_include_directories(droidscreen_desktop
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(droidscreen_desktop
    PRIVATE
        droidscreen_common
        droidscreen_protocol
)

target_compile_features(droidscreen_desktop PRIVATE cxx_std_17)

# macOS frameworks.
target_link_libraries(droidscreen_desktop PRIVATE
    "-framework ScreenCaptureKit"
    "-framework VideoToolbox"
    "-framework CoreMedia"
    "-framework CoreVideo"
    "-framework CoreGraphics"
    "-framework Foundation"
)

# ARC for Objective-C++ files.
set_source_files_properties(
    src/main.mm
    src/sck_capturer.mm
    src/vt_encoder.mm
    PROPERTIES
        COMPILE_FLAGS "-fobjc-arc"
)
