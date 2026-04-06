package com.droidscreen.app

import android.view.MotionEvent
import kotlin.math.roundToInt

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
    private const val DS_TOUCH_HOVER = 4
    private const val DS_TOUCH_HOVER_LEAVE = 5
    private const val DS_TOUCH_BUTTON_ONLY = 6
    private const val ORIENTATION_UNKNOWN = 0xFFFF

    fun forwardTouch(event: MotionEvent, surfaceWidth: Int, surfaceHeight: Int, activity: MainActivity) {
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

    fun forwardGenericMotion(
        event: MotionEvent,
        surfaceWidth: Int,
        surfaceHeight: Int,
        activity: MainActivity
    ): Boolean {
        val pointerIndex = event.actionIndex.coerceIn(0, event.pointerCount - 1)

        return when (event.actionMasked) {
            MotionEvent.ACTION_HOVER_ENTER,
            MotionEvent.ACTION_HOVER_MOVE -> {
                sendPointer(event, pointerIndex, surfaceWidth, surfaceHeight, DS_TOUCH_HOVER, activity)
                true
            }

            MotionEvent.ACTION_HOVER_EXIT -> {
                sendPointer(event, pointerIndex, surfaceWidth, surfaceHeight, DS_TOUCH_HOVER_LEAVE, activity)
                true
            }

            MotionEvent.ACTION_BUTTON_PRESS,
            MotionEvent.ACTION_BUTTON_RELEASE -> {
                sendPointer(event, pointerIndex, surfaceWidth, surfaceHeight, DS_TOUCH_BUTTON_ONLY, activity)
                true
            }

            else -> false
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
        val touchMajor = event.getTouchMajor(pointerIndex)
        val touchMinor = event.getTouchMinor(pointerIndex)
        val orientationRad = event.getOrientation(pointerIndex)

        val xFrac = (x / surfaceWidth * FRAC_MAX).toInt().coerceIn(0, FRAC_MAX)
        val yFrac = (y / surfaceHeight * FRAC_MAX).toInt().coerceIn(0, FRAC_MAX)
        val pressureFrac = (pressure * FRAC_MAX).toInt().coerceIn(0, FRAC_MAX)
        val touchMajorFrac = (touchMajor / surfaceWidth * FRAC_MAX).toInt().coerceIn(0, FRAC_MAX)
        val touchMinorFrac = (touchMinor / surfaceHeight * FRAC_MAX).toInt().coerceIn(0, FRAC_MAX)
        val orientationDeg = if (touchMajor > 0f && touchMinor > 0f) {
            ((Math.toDegrees(orientationRad.toDouble()) + 360.0) % 360.0).roundToInt().coerceIn(0, 359)
        } else {
            ORIENTATION_UNKNOWN
        }

        activity.nativeSendTouch(
            action,
            pointerId,
            xFrac,
            yFrac,
            pressureFrac,
            touchMajorFrac,
            touchMinorFrac,
            orientationDeg
        )
    }
}
