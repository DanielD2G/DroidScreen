package com.droidscreen.app

import android.view.MotionEvent

/**
 * Converts MotionEvent touch data to fractional coordinates and forwards
 * them to the native layer for protocol serialization and transmission.
 */
object TouchForwarder {

    private const val FRAC_MAX = 65535

    // Mirror the protocol touch action constants
    private const val DS_TOUCH_DOWN = 0
    private const val DS_TOUCH_MOVE = 1
    private const val DS_TOUCH_UP = 2
    private const val DS_TOUCH_CANCEL = 3

    fun forward(event: MotionEvent, surfaceWidth: Int, surfaceHeight: Int, activity: MainActivity) {
        val actionMasked = event.actionMasked
        val pointerIndex = event.actionIndex

        when (actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                sendPointer(event, 0, surfaceWidth, surfaceHeight, DS_TOUCH_DOWN, activity)
            }

            MotionEvent.ACTION_POINTER_DOWN -> {
                sendPointer(event, pointerIndex, surfaceWidth, surfaceHeight, DS_TOUCH_DOWN, activity)
            }

            MotionEvent.ACTION_MOVE -> {
                for (i in 0 until event.pointerCount) {
                    sendPointer(event, i, surfaceWidth, surfaceHeight, DS_TOUCH_MOVE, activity)
                }
            }

            MotionEvent.ACTION_UP -> {
                sendPointer(event, 0, surfaceWidth, surfaceHeight, DS_TOUCH_UP, activity)
            }

            MotionEvent.ACTION_POINTER_UP -> {
                sendPointer(event, pointerIndex, surfaceWidth, surfaceHeight, DS_TOUCH_UP, activity)
            }

            MotionEvent.ACTION_CANCEL -> {
                for (i in 0 until event.pointerCount) {
                    sendPointer(event, i, surfaceWidth, surfaceHeight, DS_TOUCH_CANCEL, activity)
                }
            }
        }
    }

    private fun sendPointer(
        event: MotionEvent,
        pointerIndex: Int,
        surfaceWidth: Int,
        surfaceHeight: Int,
        action: Int,
        activity: MainActivity
    ) {
        val pointerId = event.getPointerId(pointerIndex)
        val x = event.getX(pointerIndex)
        val y = event.getY(pointerIndex)
        val pressure = event.getPressure(pointerIndex)

        val xFrac = (x / surfaceWidth * FRAC_MAX).toInt().coerceIn(0, FRAC_MAX)
        val yFrac = (y / surfaceHeight * FRAC_MAX).toInt().coerceIn(0, FRAC_MAX)
        val pressureFrac = (pressure * FRAC_MAX).toInt().coerceIn(0, FRAC_MAX)

        activity.nativeSendTouch(action, pointerId, xFrac, yFrac, pressureFrac)
    }
}
