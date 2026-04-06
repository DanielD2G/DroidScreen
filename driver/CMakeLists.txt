# DroidScreen Virtual Display Driver - Build Configuration
#
# IMPORTANT: This IddCx UMDF v2 driver must be built with the Windows Driver
# Kit (WDK) toolchain. It cannot be built with a standard CMake/MSVC toolchain
# because it requires WDF headers, IddCx headers, and the UMDF framework.
#
# =============================================================================
# How to Build
# =============================================================================
#
# Option 1: Visual Studio (recommended)
#   1. Install Visual Studio 2022 with "Desktop development with C++"
#   2. Install the Windows Driver Kit (WDK) from:
#      https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk
#   3. Install the WDK Visual Studio Extension
#   4. Open DroidScreenDriver/DroidScreenDriver.vcxproj in Visual Studio
#   5. Select x64 platform, Debug or Release configuration
#   6. Build → Build Solution
#
# Option 2: MSBuild command line
#   From a "Developer Command Prompt for VS 2022":
#     cd DroidScreenDriver
#     msbuild DroidScreenDriver.vcxproj /p:Configuration=Release /p:Platform=x64
#
# =============================================================================
# Output
# =============================================================================
#
# The build produces:
#   - DroidScreenDriver.dll  (the UMDF v2 driver)
#   - DroidScreenDriver.inf  (installation information)
#   - DroidScreenDriver.cat  (catalog file for signing)
#
# These files are used together for driver installation. See ../install.bat.
#
# =============================================================================

cmake_minimum_required(VERSION 3.20)

message(WARNING
    "The DroidScreen IddCx driver cannot be built with CMake directly. "
    "Use the Visual Studio .vcxproj with WDK installed. "
    "See driver/README.md for instructions.")
