package com.droidscreen.app

import android.view.InputDevice
import android.view.MotionEvent
import kotlin.math.roundToInt

/**
 * Routes Android pointer input into explicit touch, pen, or mouse protocol events.
 */
object InputRouter {

    private const val FRAC_MAX = 65535

    private const val DS_TOUCH_DOWN = 0
    private const val DS_TOUCH_MOVE = 1
    private const val DS_TOUCH_UP = 2
    private const val DS_TOUCH_CANCEL = 3
    private const val DS_TOUCH_HOVER = 4
    private const val DS_TOUCH_HOVER_LEAVE = 5
    private const val DS_TOUCH_BUTTON_ONLY = 6

    private const val DS_TOUCH_TOOL_STYLUS = 2
    private const val DS_TOUCH_TOOL_ERASER = 3

    private const val DS_MOUSE_BUTTON_LEFT = 1 shl 0
    private const val DS_MOUSE_BUTTON_RIGHT = 1 shl 1
    private const val DS_MOUSE_BUTTON_MIDDLE = 1 shl 2

    private const val ORIENTATION_UNKNOWN = 0xFFFF
    private const val DISTANCE_UNKNOWN = 0xFFFF
    private const val TILT_UNKNOWN = 0xFFFF

    private enum class PointerSource {
        FINGER,
        STYLUS,
        ERASER,
        MOUSE,
        UNKNOWN
    }

    private enum class InputTarget {
        TOUCH,
        PEN,
        MOUSE,
        IGNORE
    }

    private var activeMousePointerId: Int? = null

    fun forwardTouch(
        event: MotionEvent,
        surfaceWidth: Int,
        surfaceHeight: Int,
        settings: InputSettings,
        activity: MainActivity
    ): Boolean {
        val actionMasked = event.actionMasked
        val pointerIndex = event.actionIndex
        var handled = false

        when (actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                handled = routePointer(event, 0, surfaceWidth, surfaceHeight, DS_TOUCH_DOWN, settings, activity)
            }

            MotionEvent.ACTION_POINTER_DOWN -> {
                handled = routePointer(event, pointerIndex, surfaceWidth, surfaceHeight, DS_TOUCH_DOWN, settings, activity)
            }

            MotionEvent.ACTION_MOVE -> {
                for (i in 0 until event.pointerCount) {
                    handled = routePointer(event, i, surfaceWidth, surfaceHeight, DS_TOUCH_MOVE, settings, activity) || handled
                }
            }

            MotionEvent.ACTION_UP -> {
                handled = routePointer(event, 0, surfaceWidth, surfaceHeight, DS_TOUCH_UP, settings, activity)
            }

            MotionEvent.ACTION_POINTER_UP -> {
                handled = routePointer(event, pointerIndex, surfaceWidth, surfaceHeight, DS_TOUCH_UP, settings, activity)
            }

            MotionEvent.ACTION_CANCEL -> {
                for (i in 0 until event.pointerCount) {
                    handled = routePointer(event, i, surfaceWidth, surfaceHeight, DS_TOUCH_CANCEL, settings, activity) || handled
                }
                activeMousePointerId = null
            }
        }

        return handled
    }

    fun forwardGenericMotion(
        event: MotionEvent,
        surfaceWidth: Int,
        surfaceHeight: Int,
        settings: InputSettings,
        activity: MainActivity
    ): Boolean {
        val pointerIndex = event.actionIndex.coerceIn(0, event.pointerCount - 1)
        val action = when (event.actionMasked) {
            MotionEvent.ACTION_HOVER_ENTER,
            MotionEvent.ACTION_HOVER_MOVE -> DS_TOUCH_HOVER
            MotionEvent.ACTION_HOVER_EXIT -> DS_TOUCH_HOVER_LEAVE
            MotionEvent.ACTION_BUTTON_PRESS,
            MotionEvent.ACTION_BUTTON_RELEASE -> DS_TOUCH_BUTTON_ONLY
            else -> return false
        }

        return routePointer(event, pointerIndex, surfaceWidth, surfaceHeight, action, settings, activity)
    }

    private fun routePointer(
        event: MotionEvent,
        pointerIndex: Int,
        surfaceWidth: Int,
        surfaceHeight: Int,
        action: Int,
        settings: InputSettings,
        activity: MainActivity
    ): Boolean {
        val source = classifyPointerSource(event, pointerIndex, settings.unknownPointerFallback)
        val target = targetForSource(source, settings)
        return when (target) {
            InputTarget.TOUCH -> {
                sendTouch(event, pointerIndex, surfaceWidth, surfaceHeight, action, activity)
                true
            }

            InputTarget.PEN -> {
                sendPen(event, pointerIndex, surfaceWidth, surfaceHeight, action, source, activity)
                true
            }

            InputTarget.MOUSE -> sendMouse(event, pointerIndex, surfaceWidth, surfaceHeight, action, source, activity)
            InputTarget.IGNORE -> false
        }
    }

    private fun classifyPointerSource(
        event: MotionEvent,
        pointerIndex: Int,
        fallback: UnknownPointerFallback
    ): PointerSource {
        val source = event.source
        return when (event.getToolType(pointerIndex)) {
            MotionEvent.TOOL_TYPE_ERASER -> PointerSource.ERASER
            MotionEvent.TOOL_TYPE_STYLUS -> PointerSource.STYLUS
            MotionEvent.TOOL_TYPE_FINGER -> PointerSource.FINGER
            MotionEvent.TOOL_TYPE_MOUSE -> PointerSource.MOUSE
            else -> when {
                (source and InputDevice.SOURCE_MOUSE) == InputDevice.SOURCE_MOUSE -> PointerSource.MOUSE
                (source and InputDevice.SOURCE_STYLUS) == InputDevice.SOURCE_STYLUS -> PointerSource.STYLUS
                (source and InputDevice.SOURCE_TOUCHSCREEN) == InputDevice.SOURCE_TOUCHSCREEN -> PointerSource.FINGER
                fallback == UnknownPointerFallback.TREAT_AS_FINGER -> PointerSource.FINGER
                else -> PointerSource.UNKNOWN
            }
        }
    }

    private fun targetForSource(source: PointerSource, settings: InputSettings): InputTarget =
        when (source) {
            PointerSource.FINGER -> when (settings.fingerInputMode) {
                FingerInputMode.NATIVE_TOUCH -> InputTarget.TOUCH
                FingerInputMode.MOUSE_CURSOR -> InputTarget.MOUSE
            }

            PointerSource.STYLUS,
            PointerSource.ERASER -> when (settings.stylusInputMode) {
                StylusInputMode.NATIVE_PEN -> InputTarget.PEN
                StylusInputMode.NATIVE_TOUCH -> InputTarget.TOUCH
                StylusInputMode.MOUSE_CURSOR -> InputTarget.MOUSE
            }

            PointerSource.MOUSE,
            PointerSource.UNKNOWN -> InputTarget.IGNORE
        }

    private fun sendTouch(
        event: MotionEvent,
        pointerIndex: Int,
        surfaceWidth: Int,
        surfaceHeight: Int,
        action: Int,
        activity: MainActivity
    ) {
        val pointerId = event.getPointerId(pointerIndex)
        val xFrac = normalizePosition(event.getX(pointerIndex), surfaceWidth)
        val yFrac = normalizePosition(event.getY(pointerIndex), surfaceHeight)
        val pressureFrac = normalizeUnit(event.getPressure(pointerIndex))
        val touchMajorFrac = normalizeContact(event.getTouchMajor(pointerIndex), surfaceWidth)
        val touchMinorFrac = normalizeContact(event.getTouchMinor(pointerIndex), surfaceHeight)
        val orientation = normalizeRotation(event.getOrientation(pointerIndex))

        activity.nativeSendTouch(
            action,
            pointerId,
            xFrac,
            yFrac,
            pressureFrac,
            touchMajorFrac,
            touchMinorFrac,
            orientation
        )
    }

    private fun sendPen(
        event: MotionEvent,
        pointerIndex: Int,
        surfaceWidth: Int,
        surfaceHeight: Int,
        action: Int,
        source: PointerSource,
        activity: MainActivity
    ) {
        val pointerId = event.getPointerId(pointerIndex)
        val xFrac = normalizePosition(event.getX(pointerIndex), surfaceWidth)
        val yFrac = normalizePosition(event.getY(pointerIndex), surfaceHeight)
        val pressureFrac = normalizeUnit(event.getPressure(pointerIndex))
        val distance = normalizeAxis(event, pointerIndex, MotionEvent.AXIS_DISTANCE, DISTANCE_UNKNOWN)
        val tilt = normalizeTilt(event, pointerIndex)
        val rotation = normalizeRotation(event.getOrientation(pointerIndex))
        val toolType = if (source == PointerSource.ERASER) DS_TOUCH_TOOL_ERASER else DS_TOUCH_TOOL_STYLUS
        val buttons = mapStylusButtons(event.buttonState)

        activity.nativeSendPen(
            action,
            pointerId,
            toolType,
            buttons,
            xFrac,
            yFrac,
            pressureFrac,
            distance,
            tilt,
            rotation
        )
    }

    private fun sendMouse(
        event: MotionEvent,
        pointerIndex: Int,
        surfaceWidth: Int,
        surfaceHeight: Int,
        action: Int,
        source: PointerSource,
        activity: MainActivity
    ): Boolean {
        val pointerId = event.getPointerId(pointerIndex)
        val activePointerId = activeMousePointerId

        when (action) {
            DS_TOUCH_DOWN -> {
                if (activePointerId != null && activePointerId != pointerId) {
                    return false
                }
                activeMousePointerId = pointerId
            }

            DS_TOUCH_MOVE,
            DS_TOUCH_UP,
            DS_TOUCH_CANCEL -> {
                if (activePointerId != pointerId) {
                    return false
                }
            }

            DS_TOUCH_HOVER,
            DS_TOUCH_HOVER_LEAVE,
            DS_TOUCH_BUTTON_ONLY -> {
                if (activePointerId != null && activePointerId != pointerId) {
                    return false
                }
            }
        }

        val xFrac = normalizePosition(event.getX(pointerIndex), surfaceWidth)
        val yFrac = normalizePosition(event.getY(pointerIndex), surfaceHeight)
        val buttons = desiredMouseButtons(event, action, source)

        activity.nativeSendMouse(action, buttons, xFrac, yFrac)

        if (action == DS_TOUCH_UP || action == DS_TOUCH_CANCEL) {
            activeMousePointerId = null
        }
        return true
    }

    private fun desiredMouseButtons(event: MotionEvent, action: Int, source: PointerSource): Int {
        val stylusButtons = mapStylusButtons(event.buttonState)
        return when (source) {
            PointerSource.STYLUS,
            PointerSource.ERASER -> when (action) {
                DS_TOUCH_DOWN,
                DS_TOUCH_MOVE -> if (stylusButtons != 0) stylusButtons else DS_MOUSE_BUTTON_LEFT
                DS_TOUCH_BUTTON_ONLY -> stylusButtons
                else -> if (stylusButtons != 0 && action == DS_TOUCH_HOVER) stylusButtons else 0
            }

            PointerSource.FINGER -> when (action) {
                DS_TOUCH_DOWN,
                DS_TOUCH_MOVE -> DS_MOUSE_BUTTON_LEFT
                else -> 0
            }

            else -> 0
        }
    }

    private fun mapStylusButtons(buttonState: Int): Int {
        var result = 0
        if ((buttonState and MotionEvent.BUTTON_STYLUS_PRIMARY) != 0) {
            result = result or DS_MOUSE_BUTTON_RIGHT
        }
        if ((buttonState and MotionEvent.BUTTON_STYLUS_SECONDARY) != 0) {
            result = result or DS_MOUSE_BUTTON_MIDDLE
        }
        return result
    }

    private fun normalizePosition(value: Float, dimension: Int): Int {
        if (dimension <= 0 || !value.isFinite()) {
            return 0
        }
        return ((value / dimension) * FRAC_MAX).roundToInt().coerceIn(0, FRAC_MAX)
    }

    private fun normalizeContact(value: Float, dimension: Int): Int {
        if (dimension <= 0 || !value.isFinite()) {
            return 0
        }
        return ((value / dimension) * FRAC_MAX).roundToInt().coerceIn(0, FRAC_MAX)
    }

    private fun normalizeUnit(value: Float): Int {
        if (!value.isFinite()) {
            return 0
        }
        return (value.coerceIn(0f, 1f) * FRAC_MAX).roundToInt().coerceIn(0, FRAC_MAX)
    }

    private fun normalizeAxis(
        event: MotionEvent,
        pointerIndex: Int,
        axis: Int,
        unknown: Int
    ): Int {
        if (!supportsAxis(event, axis)) {
            return unknown
        }
        val value = event.getAxisValue(axis, pointerIndex)
        if (!value.isFinite()) {
            return unknown
        }
        return normalizeUnit(value)
    }

    private fun normalizeTilt(event: MotionEvent, pointerIndex: Int): Int {
        if (!supportsAxis(event, MotionEvent.AXIS_TILT)) {
            return TILT_UNKNOWN
        }
        val tiltRad = event.getAxisValue(MotionEvent.AXIS_TILT, pointerIndex)
        if (!tiltRad.isFinite()) {
            return TILT_UNKNOWN
        }
        return Math.toDegrees(tiltRad.toDouble()).roundToInt().coerceIn(0, 90)
    }

    private fun normalizeRotation(orientationRad: Float): Int {
        if (!orientationRad.isFinite()) {
            return ORIENTATION_UNKNOWN
        }
        return ((Math.toDegrees(orientationRad.toDouble()) + 360.0) % 360.0)
            .roundToInt()
            .coerceIn(0, 359)
    }

    private fun supportsAxis(event: MotionEvent, axis: Int): Boolean =
        event.device?.motionRanges?.any { range ->
            range.axis == axis
        } == true
}
