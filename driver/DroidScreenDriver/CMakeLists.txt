# DroidScreenDriver - IddCx UMDF v2 Virtual Display Driver
#
# This CMakeLists.txt is provided for IDE integration (e.g., CLion) but the
# actual build MUST use the .vcxproj with the Windows Driver Kit (WDK).
#
# =============================================================================
# Why not CMake?
# =============================================================================
#
# IddCx/UMDF v2 drivers require:
#   - WDF (Windows Driver Frameworks) build infrastructure
#   - IddCxStub.lib from the WDK
#   - Proper UMDF metadata embedding
#   - INF file processing and catalog generation
#   - Inf2Cat and SignTool integration
#
# The WDK MSBuild targets handle all of this automatically. CMake does not
# have native support for WDK driver projects.
#
# =============================================================================
# Build Instructions
# =============================================================================
#
# Prerequisites:
#   - Windows 10/11 x64
#   - Visual Studio 2022 (v143 toolset)
#   - Windows 11 SDK (10.0.22621.0+)
#   - Windows Driver Kit (WDK) 10.0.22621.0+
#
# Build steps:
#   1. Open "Developer Command Prompt for VS 2022"
#   2. cd to this directory
#   3. Run: msbuild DroidScreenDriver.vcxproj /p:Configuration=Release /p:Platform=x64
#
# Or open DroidScreenDriver.vcxproj in Visual Studio and build from the IDE.
#
# =============================================================================

cmake_minimum_required(VERSION 3.20)
project(DroidScreenDriver LANGUAGES CXX)

# This is for IDE support only — it will NOT produce a working driver
message(WARNING
    "CMake build is for IDE navigation only. "
    "Build the actual driver with: msbuild DroidScreenDriver.vcxproj /p:Configuration=Release /p:Platform=x64")

# List sources for IDE indexing
set(DRIVER_SOURCES
    Driver.cpp
    IndirectMonitor.cpp
    SwapChainProcessor.cpp
)

set(DRIVER_HEADERS
    Driver.h
    IndirectMonitor.h
    SwapChainProcessor.h
)

# Dummy target for IDE file listing (will not compile without WDK)
add_library(DroidScreenDriver SHARED ${DRIVER_SOURCES} ${DRIVER_HEADERS})

target_compile_features(DroidScreenDriver PRIVATE cxx_std_17)
