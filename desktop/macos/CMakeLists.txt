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
    src/wifi_discovery.mm
)

# -----------------------------------------------------------------------------
# SwiftUI interface (src/ui). The Makefile generator has no Swift support, so
# the module is compiled with swiftc into one object file and linked into the
# app; main.mm talks to it through src/ui/DSUIBridge.h.
# -----------------------------------------------------------------------------
file(GLOB DS_UI_SWIFT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.swift")
set(DS_UI_BRIDGE "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/DSUIBridge.h")
set(DS_UI_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/droidscreen_ui.o")
if(CMAKE_OSX_ARCHITECTURES)
    list(GET CMAKE_OSX_ARCHITECTURES 0 DS_UI_ARCH)
else()
    set(DS_UI_ARCH "${CMAKE_HOST_SYSTEM_PROCESSOR}")
endif()
if(CMAKE_OSX_DEPLOYMENT_TARGET)
    set(DS_UI_MIN_OS "${CMAKE_OSX_DEPLOYMENT_TARGET}")
else()
    set(DS_UI_MIN_OS "12.3")
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(DS_UI_OPT "-Onone" "-g")
else()
    set(DS_UI_OPT "-O")
endif()
add_custom_command(
    OUTPUT "${DS_UI_OBJECT}"
    COMMAND xcrun swiftc -parse-as-library -wmo ${DS_UI_OPT} -swift-version 5
            -module-name DroidScreenUI
            -target "${DS_UI_ARCH}-apple-macos${DS_UI_MIN_OS}"
            -import-objc-header "${DS_UI_BRIDGE}"
            -c ${DS_UI_SWIFT_SOURCES} -o "${DS_UI_OBJECT}"
    DEPENDS ${DS_UI_SWIFT_SOURCES} "${DS_UI_BRIDGE}"
    COMMENT "Compiling SwiftUI interface"
    VERBATIM
)
set_source_files_properties("${DS_UI_OBJECT}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
target_sources(droidscreen_desktop PRIVATE "${DS_UI_OBJECT}")

# Swift runtime: the OS one (/usr/lib/swift) plus the toolchain's
# back-deployment shims for older deployment targets.
execute_process(COMMAND xcrun --show-sdk-path OUTPUT_VARIABLE DS_SDK_PATH OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND xcrun --find swiftc OUTPUT_VARIABLE DS_SWIFTC OUTPUT_STRIP_TRAILING_WHITESPACE)
get_filename_component(DS_TOOLCHAIN_BIN "${DS_SWIFTC}" DIRECTORY)
target_link_directories(droidscreen_desktop PRIVATE
    "${DS_SDK_PATH}/usr/lib/swift"
    "${DS_TOOLCHAIN_BIN}/../lib/swift/macosx"
)
target_link_options(droidscreen_desktop PRIVATE "-Wl,-rpath,/usr/lib/swift")

# Link Line fonts (Instrument Sans, IBM Plex Mono; SIL OFL), registered at launch.
file(GLOB DS_UI_FONTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/*.ttf")
set_source_files_properties(${DS_UI_FONTS} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/Fonts")
target_sources(droidscreen_desktop PRIVATE ${DS_UI_FONTS})

add_executable(droidscreen_validation_scene_helper
    src/validation_scene_helper.mm
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

target_link_libraries(droidscreen_validation_scene_helper PRIVATE
    "-framework AppKit"
    "-framework QuartzCore"
    "-framework Foundation"
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
    "-framework QuartzCore"
    "-framework Foundation"
    "-framework CoreAudio"
    "-framework AudioToolbox"
    "-framework UniformTypeIdentifiers"
    "-framework ApplicationServices"
    "-framework SwiftUI"
)

# ARC for Objective-C++ files.
set_source_files_properties(
    src/main.mm
    src/sck_capturer.mm
    src/vt_encoder.mm
    src/virtual_display.mm
    src/touch_injector_mac.mm
    src/mouse_injector_mac.mm
    src/validation_scene_helper.mm
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
