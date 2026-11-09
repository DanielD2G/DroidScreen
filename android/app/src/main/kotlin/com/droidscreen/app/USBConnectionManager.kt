package com.droidscreen.app

/**
 * Manages USB connection parameters.
 * The TCP port is reachable over USB (`adb forward`) and over Wi-Fi.
 *
 * Stub for future USB accessory detection and automatic setup.
 */
class USBConnectionManager {

    companion object {
        /** Default TCP port used by the DroidScreen protocol. */
        const val PORT = 38271
    }
}
