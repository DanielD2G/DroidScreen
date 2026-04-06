package com.droidscreen.app

import android.app.Activity
import android.os.Build
import android.os.Bundle
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.Surface
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.widget.LinearLayout
import android.widget.TextView

class MainActivity : Activity(), SurfaceHolder.Callback {

    companion object {
        private const val TAG = "DroidScreen"

        // Status constants — must match native_bridge.cpp
        const val STATUS_WAITING = 0
        const val STATUS_CONNECTED = 1
        const val STATUS_DISCONNECTED = 2
        const val STATUS_ERROR = 3

        init {
            System.loadLibrary("droidscreen_native")
        }
    }

    private lateinit var surfaceView: SurfaceView
    private lateinit var statusOverlay: LinearLayout
    private lateinit var statusText: TextView
    private lateinit var statusDetail: TextView
    private var nativeStarted = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        surfaceView = findViewById(R.id.surface_view)
        statusOverlay = findViewById(R.id.status_overlay)
        statusText = findViewById(R.id.status_text)
        statusDetail = findViewById(R.id.status_detail)

        surfaceView.holder.addCallback(this)

        // Show initial waiting state
        updateStatusUI(STATUS_WAITING)

        setImmersiveMode()
    }

    override fun onResume() {
        super.onResume()
        setImmersiveMode()
    }

    override fun onDestroy() {
        if (nativeStarted) {
            nativeStop()
            nativeStarted = false
        }
        super.onDestroy()
    }

    private fun setImmersiveMode() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.insetsController?.let { controller ->
                controller.hide(
                    WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars()
                )
                controller.systemBarsBehavior =
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = (
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    or View.SYSTEM_UI_FLAG_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
            )
        }
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        if (nativeStarted) {
            // Surface was recreated (e.g., app went to background and came back).
            // Must restart native layer with the new Surface — old one is dead.
            android.util.Log.i(TAG, "Surface recreated, restarting native layer")
            nativeStop()
            nativeStarted = false
        }
        nativeInit(holder.surface, USBConnectionManager.PORT)
        nativeStarted = true
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        // Don't stop native here — surfaceCreated will handle restart.
        // This avoids a race condition during rotation where surfaceDestroyed
        // is called immediately followed by surfaceCreated.
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        val width = surfaceView.width
        val height = surfaceView.height
        if (width > 0 && height > 0) {
            TouchForwarder.forward(event, width, height, this)
        }
        return true
    }

    /**
     * Called from native code (JNI) when connection status changes.
     * This is called from a native thread — must post to UI thread.
     */
    @Suppress("unused") // Called from JNI
    fun onNativeStatusChanged(status: Int) {
        runOnUiThread {
            updateStatusUI(status)
        }
    }

    private fun updateStatusUI(status: Int) {
        when (status) {
            STATUS_WAITING -> {
                statusOverlay.visibility = View.VISIBLE
                statusText.text = "Waiting for connection..."
                statusDetail.text = "Port: ${USBConnectionManager.PORT}"
            }
            STATUS_CONNECTED -> {
                statusOverlay.visibility = View.GONE
            }
            STATUS_DISCONNECTED -> {
                statusOverlay.visibility = View.VISIBLE
                statusText.text = "Disconnected"
                statusDetail.text = "Reconnecting..."
            }
            STATUS_ERROR -> {
                statusOverlay.visibility = View.VISIBLE
                statusText.text = "Error"
                statusDetail.text = "Please restart the app"
            }
        }
    }

    private external fun nativeInit(surface: Surface, port: Int)
    private external fun nativeStop()
    external fun nativeSendTouch(
        action: Int,
        pointerId: Int,
        xFrac: Int,
        yFrac: Int,
        pressure: Int,
        touchMajor: Int,
        touchMinor: Int,
        orientation: Int
    )
}
