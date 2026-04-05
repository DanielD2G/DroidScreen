package com.droidscreen.app

import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView

/**
 * Wrapper managing SurfaceView lifecycle.
 * Provides the Surface to the native layer for video rendering.
 */
class DisplayRenderer(private val surfaceView: SurfaceView) {

    val surface: Surface?
        get() = if (surfaceView.holder.surface.isValid) surfaceView.holder.surface else null

    val width: Int
        get() = surfaceView.width

    val height: Int
        get() = surfaceView.height

    val isReady: Boolean
        get() = surface != null && width > 0 && height > 0
}
