cmake_minimum_required(VERSION 3.14)

# Enable Objective-C++ for .mm files.
enable_language(OBJCXX)

add_executable(droidscreen_desktop MACOSX_BUNDLE
    src/main.mm
    src/sck_capturer.mm
    src/vt_encoder.mm
    src/virtual_display.mm
)

# Bundle the app icon.
set(MACOSX_ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.icns")
set_source_files_properties(${MACOSX_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
target_sources(droidscreen_desktop PRIVATE ${MACOSX_ICON})

# Info.plist for menu-bar-only app (LSUIElement = true).
set_target_properties(droidscreen_desktop PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
    MACOSX_BUNDLE_BUNDLE_NAME "DroidScreen"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.droidscreen.desktop"
    MACOSX_BUNDLE_ICON_FILE "icon"
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
    "-framework AppKit"
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
    src/virtual_display.mm
    PROPERTIES
        COMPILE_FLAGS "-fobjc-arc"
)
