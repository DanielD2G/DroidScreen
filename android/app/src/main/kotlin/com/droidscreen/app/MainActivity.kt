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

class MainActivity : Activity(), SurfaceHolder.Callback {

    companion object {
        private const val TAG = "DroidScreen"

        init {
            System.loadLibrary("droidscreen_native")
        }
    }

    private lateinit var surfaceView: SurfaceView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        surfaceView = findViewById(R.id.surface_view)
        surfaceView.holder.addCallback(this)

        setImmersiveMode()
    }

    override fun onResume() {
        super.onResume()
        setImmersiveMode()
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
        nativeInit(holder.surface, USBConnectionManager.PORT)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        // No action needed; native layer handles surface directly
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        nativeStop()
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        val width = surfaceView.width
        val height = surfaceView.height
        if (width > 0 && height > 0) {
            TouchForwarder.forward(event, width, height, this)
        }
        return true
    }

    // Native methods
    private external fun nativeInit(surface: Surface, port: Int)
    private external fun nativeStop()
    external fun nativeSendTouch(action: Int, pointerId: Int, xFrac: Int, yFrac: Int, pressure: Int)
}
