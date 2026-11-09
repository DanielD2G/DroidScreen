package com.droidscreen.app

import android.content.Context
import android.net.ConnectivityManager
import android.net.LinkProperties
import android.net.NetworkCapabilities
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.net.wifi.WifiManager
import android.os.Build
import android.provider.Settings
import android.util.Log
import android.view.WindowManager
import java.net.Inet4Address
import java.security.MessageDigest
import java.security.SecureRandom

/**
 * Everything the tablet needs to be reachable without a cable:
 *
 *  - Advertises the DroidScreen TCP server on the local network via mDNS
 *    (`_droidscreen._tcp`), with a TXT record carrying a stable tablet id,
 *    the native panel resolution and the protocol version, so the desktop
 *    can configure the stream without adb.
 *  - Holds a low-latency Wi-Fi lock while streaming (disables Wi-Fi power
 *    save, which otherwise adds tens of ms of jitter).
 *  - Stores the desktops the user approved, with the token each one must
 *    present on later connections.
 */
class WirelessManager(private val context: Context) {

    companion object {
        private const val TAG = "DroidScreen"
        const val SERVICE_TYPE = "_droidscreen._tcp."
        private const val PREFS = "droidscreen_wireless"
        private const val KEY_TABLET_ID = "tablet_id"
        private const val TRUSTED_PREFIX = "trusted_token_"
        private const val TRUSTED_NAME_PREFIX = "trusted_name_"
        private const val TOKEN_BYTES = 16
    }

    private val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
    private val nsdManager = context.getSystemService(Context.NSD_SERVICE) as NsdManager
    private val wifiManager =
        context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
    private var registrationListener: NsdManager.RegistrationListener? = null
    private var wifiLock: WifiManager.WifiLock? = null

    /** Stable random id for this tablet, used by the desktop to remember pairing. */
    val tabletId: String by lazy {
        prefs.getString(KEY_TABLET_ID, null) ?: randomHex(TOKEN_BYTES).also {
            prefs.edit().putString(KEY_TABLET_ID, it).apply()
        }
    }

    /** Human-readable device name ("Xiaomi Pad 7"), falling back to the model. */
    val deviceName: String by lazy {
        Settings.Global.getString(context.contentResolver, Settings.Global.DEVICE_NAME)
            ?.takeIf { it.isNotBlank() }
            ?: "${Build.MANUFACTURER} ${Build.MODEL}"
    }

    fun startAdvertising(port: Int, protocolVersion: Int) {
        if (registrationListener != null) return
        val (w, h) = nativePanelSizeLandscape()
        val info = NsdServiceInfo().apply {
            serviceName = deviceName
            serviceType = SERVICE_TYPE
            setPort(port)
            setAttribute("id", tabletId)
            setAttribute("w", w.toString())
            setAttribute("h", h.toString())
            setAttribute("v", protocolVersion.toString())
        }
        val listener = object : NsdManager.RegistrationListener {
            override fun onServiceRegistered(info: NsdServiceInfo) {
                Log.i(TAG, "mDNS: advertising '${info.serviceName}' on port $port (${w}x$h)")
            }
            override fun onRegistrationFailed(info: NsdServiceInfo, errorCode: Int) {
                Log.e(TAG, "mDNS: registration failed: $errorCode")
            }
            override fun onServiceUnregistered(info: NsdServiceInfo) {
                Log.i(TAG, "mDNS: stopped advertising")
            }
            override fun onUnregistrationFailed(info: NsdServiceInfo, errorCode: Int) {
                Log.w(TAG, "mDNS: unregistration failed: $errorCode")
            }
        }
        registrationListener = listener
        nsdManager.registerService(info, NsdManager.PROTOCOL_DNS_SD, listener)
    }

    fun stopAdvertising() {
        val listener = registrationListener ?: return
        registrationListener = null
        try {
            nsdManager.unregisterService(listener)
        } catch (e: IllegalArgumentException) {
            Log.w(TAG, "mDNS: listener was not registered", e)
        }
    }

    fun acquireLowLatencyWifi() {
        if (wifiLock?.isHeld == true) return
        val mode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            WifiManager.WIFI_MODE_FULL_LOW_LATENCY
        } else {
            @Suppress("DEPRECATION")
            WifiManager.WIFI_MODE_FULL_HIGH_PERF
        }
        wifiLock = wifiManager.createWifiLock(mode, "DroidScreen:stream").apply {
            setReferenceCounted(false)
            acquire()
        }
        Log.i(TAG, "Wi-Fi low-latency lock acquired")
    }

    fun releaseLowLatencyWifi() {
        wifiLock?.let {
            if (it.isHeld) it.release()
            Log.i(TAG, "Wi-Fi low-latency lock released")
        }
        wifiLock = null
    }

    /** IPv4 address on the active Wi-Fi network, or null when not on Wi-Fi. */
    fun wifiAddress(): String? {
        val cm = context.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        val network = cm.activeNetwork ?: return null
        val caps = cm.getNetworkCapabilities(network) ?: return null
        if (!caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) return null
        val link: LinkProperties = cm.getLinkProperties(network) ?: return null
        return link.linkAddresses.map { it.address }
            .firstOrNull { it is Inet4Address && !it.isLoopbackAddress }
            ?.hostAddress
    }

    /** Returns the stored token if [desktopId] is trusted and presented it. */
    fun tokenIfTrusted(desktopId: String, presentedToken: String): String? {
        val stored = prefs.getString(TRUSTED_PREFIX + desktopId, null) ?: return null
        val matches = MessageDigest.isEqual(
            stored.toByteArray(Charsets.US_ASCII),
            presentedToken.lowercase().toByteArray(Charsets.US_ASCII)
        )
        return if (matches) stored else null
    }

    /** Trust [desktopId] and return the fresh token it must present from now on. */
    fun trust(desktopId: String, name: String): String {
        val token = randomHex(TOKEN_BYTES)
        prefs.edit()
            .putString(TRUSTED_PREFIX + desktopId, token)
            .putString(TRUSTED_NAME_PREFIX + desktopId, name)
            .apply()
        return token
    }

    private fun nativePanelSizeLandscape(): Pair<Int, Int> {
        @Suppress("DEPRECATION")
        val display = (context.getSystemService(Context.WINDOW_SERVICE) as WindowManager)
            .defaultDisplay
        val mode = display.mode
        val w = maxOf(mode.physicalWidth, mode.physicalHeight)
        val h = minOf(mode.physicalWidth, mode.physicalHeight)
        return w to h
    }

    private fun randomHex(bytes: Int): String {
        val buf = ByteArray(bytes)
        SecureRandom().nextBytes(buf)
        return buf.joinToString("") { "%02x".format(it) }
    }
}
