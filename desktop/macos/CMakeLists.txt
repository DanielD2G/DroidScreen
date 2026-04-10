cmake_minimum_required(VERSION 3.14)

# Enable Objective-C++ for .mm files and Objective-C for helper dylibs.
enable_language(OBJCXX)
enable_language(OBJC)

add_executable(droidscreen_desktop MACOSX_BUNDLE
    src/main.mm
    src/sck_capturer.mm
    src/vt_encoder.mm
    src/virtual_display.mm
    src/touch_injector_mac.mm
    src/mouse_injector_mac.mm
)

add_library(droidscreen_mediaremote_mini SHARED
    src/mediaremote_helper.m
)

set_target_properties(droidscreen_mediaremote_mini PROPERTIES
    OUTPUT_NAME "MediaRemoteMini"
    PREFIX ""
)

# Bundle the app icon.
set(MACOSX_ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.icns")
set_source_files_properties(${MACOSX_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
target_sources(droidscreen_desktop PRIVATE ${MACOSX_ICON})

set(MEDIAREMOTE_MINI_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/scripts/mediaremote-mini.pl")
set_source_files_properties(${MEDIAREMOTE_MINI_SCRIPT} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
target_sources(droidscreen_desktop PRIVATE ${MEDIAREMOTE_MINI_SCRIPT})

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

target_include_directories(droidscreen_mediaremote_mini
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(droidscreen_desktop
    PRIVATE
        droidscreen_common
        droidscreen_protocol
)

target_link_libraries(droidscreen_mediaremote_mini PRIVATE
    "-framework Foundation"
    "-framework AppKit"
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
    "-framework CoreAudio"
    "-framework AudioToolbox"
    "-framework UniformTypeIdentifiers"
    "-framework ApplicationServices"
)

# ARC for Objective-C++ files.
set_source_files_properties(
    src/main.mm
    src/sck_capturer.mm
    src/vt_encoder.mm
    src/virtual_display.mm
    src/touch_injector_mac.mm
    src/mouse_injector_mac.mm
    PROPERTIES
        COMPILE_FLAGS "-fobjc-arc"
)

set_source_files_properties(
    src/mediaremote_helper.m
    PROPERTIES
        LANGUAGE OBJC
        COMPILE_FLAGS "-fobjc-arc -fvisibility=default"
)

add_dependencies(droidscreen_desktop droidscreen_mediaremote_mini)

add_custom_command(
    TARGET droidscreen_mediaremote_mini
    POST_BUILD
    COMMAND codesign --force --sign - "$<TARGET_FILE:droidscreen_mediaremote_mini>"
    COMMENT "Signing MediaRemoteMini.dylib"
)

add_custom_command(
    TARGET droidscreen_desktop
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:droidscreen_mediaremote_mini>"
            "$<TARGET_BUNDLE_CONTENT_DIR:droidscreen_desktop>/Resources/MediaRemoteMini.dylib"
    COMMAND chmod +x "$<TARGET_BUNDLE_CONTENT_DIR:droidscreen_desktop>/Resources/mediaremote-mini.pl"
    COMMENT "Bundling MediaRemote helper resources"
)
