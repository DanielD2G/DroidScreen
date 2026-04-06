package com.droidscreen.app

import android.view.InputDevice
import android.view.MotionEvent
import android.view.View
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.min
import kotlin.math.sin
import kotlin.math.sqrt
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
    private const val DS_TOUCH_CANCEL_ALL = 7

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
        inputView: View,
        surfaceView: View,
        event: MotionEvent,
        surfaceWidth: Int,
        surfaceHeight: Int,
        settings: InputSettings,
        activity: MainActivity
    ): Boolean {
        if (event.actionMasked == MotionEvent.ACTION_CANCEL) {
            activeMousePointerId = null
            return routeCancelAll(
                inputView,
                surfaceView,
                event,
                surfaceWidth,
                surfaceHeight,
                settings,
                activity
            )
        }

        val actionMasked = event.actionMasked
        val pointerIndex = event.actionIndex
        var handled = false
        val pointerAction = actionForTouchEvent(event)
        if (pointerAction < 0) {
            return false
        }

        when (actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                handled = routePointer(
                    inputView,
                    surfaceView,
                    event,
                    0,
                    surfaceWidth,
                    surfaceHeight,
                    pointerAction,
                    settings,
                    activity
                )
            }

            MotionEvent.ACTION_POINTER_DOWN -> {
                handled = routePointer(
                    inputView,
                    surfaceView,
                    event,
                    pointerIndex,
                    surfaceWidth,
                    surfaceHeight,
                    pointerAction,
                    settings,
                    activity
                )
            }

            MotionEvent.ACTION_MOVE -> {
                for (i in 0 until event.pointerCount) {
                    handled = routePointer(
                        inputView,
                        surfaceView,
                        event,
                        i,
                        surfaceWidth,
                        surfaceHeight,
                        pointerAction,
                        settings,
                        activity
                    ) || handled
                }
            }

            MotionEvent.ACTION_UP -> {
                handled = routePointer(
                    inputView,
                    surfaceView,
                    event,
                    0,
                    surfaceWidth,
                    surfaceHeight,
                    pointerAction,
                    settings,
                    activity
                )
            }

            MotionEvent.ACTION_POINTER_UP -> {
                handled = routePointer(
                    inputView,
                    surfaceView,
                    event,
                    pointerIndex,
                    surfaceWidth,
                    surfaceHeight,
                    pointerAction,
                    settings,
                    activity
                )
            }
        }

        return handled
    }

    fun forwardGenericMotion(
        inputView: View,
        surfaceView: View,
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

        return routePointer(
            inputView,
            surfaceView,
            event,
            pointerIndex,
            surfaceWidth,
            surfaceHeight,
            action,
            settings,
            activity
        )
    }

    private fun routePointer(
        inputView: View,
        surfaceView: View,
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
        if (source == PointerSource.FINGER &&
            (action == DS_TOUCH_HOVER || action == DS_TOUCH_HOVER_LEAVE || action == DS_TOUCH_BUTTON_ONLY)) {
            return false
        }
        return when (target) {
            InputTarget.TOUCH -> {
                sendTouch(inputView, surfaceView, event, pointerIndex, surfaceWidth, surfaceHeight, activity)
                true
            }

            InputTarget.PEN -> {
                sendPen(inputView, surfaceView, event, pointerIndex, surfaceWidth, surfaceHeight, action, source, activity)
                true
            }

            InputTarget.MOUSE -> sendMouse(inputView, surfaceView, event, pointerIndex, surfaceWidth, surfaceHeight, action, source, activity)
            InputTarget.IGNORE -> false
        }
    }

    private fun routeCancelAll(
        inputView: View,
        surfaceView: View,
        event: MotionEvent,
        surfaceWidth: Int,
        surfaceHeight: Int,
        settings: InputSettings,
        activity: MainActivity
    ): Boolean {
        var sentTouch = false
        var sentPen = false
        var sentMouse = false

        for (i in 0 until event.pointerCount) {
            val source = classifyPointerSource(event, i, settings.unknownPointerFallback)
            when (targetForSource(source, settings)) {
                InputTarget.TOUCH -> {
                    if (!sentTouch) {
                        activity.nativeSendTouch(
                            DS_TOUCH_CANCEL_ALL,
                            0,
                            0,
                            0,
                            0,
                            0,
                            0,
                            ORIENTATION_UNKNOWN
                        )
                        sentTouch = true
                    }
                }

                InputTarget.PEN -> {
                    if (!sentPen) {
                        activity.nativeSendPen(
                            DS_TOUCH_CANCEL_ALL,
                            0,
                            DS_TOUCH_TOOL_STYLUS,
                            0,
                            0,
                            0,
                            0,
                            DISTANCE_UNKNOWN,
                            TILT_UNKNOWN,
                            ORIENTATION_UNKNOWN
                        )
                        sentPen = true
                    }
                }

                InputTarget.MOUSE -> {
                    if (!sentMouse) {
                        activity.nativeSendMouse(DS_TOUCH_CANCEL, 0, 0, 0)
                        sentMouse = true
                    }
                }

                InputTarget.IGNORE -> Unit
            }
        }

        return sentTouch || sentPen || sentMouse
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
        inputView: View,
        surfaceView: View,
        event: MotionEvent,
        pointerIndex: Int,
        surfaceWidth: Int,
        surfaceHeight: Int,
        activity: MainActivity
    ) {
        val action = actionForTouchEvent(event)
        if (action < 0) {
            return
        }

        val pointerId = event.getPointerId(pointerIndex)
        val (xFrac, yFrac) = getSurfaceRelativeNormalizedXY(
            inputView,
            surfaceView,
            event,
            pointerIndex,
            surfaceWidth,
            surfaceHeight
        )
        val pressureOrDistance = getPressureOrDistance(event, pointerIndex)
        val normalizedContactArea = getNormalizedContactArea(event, pointerIndex, surfaceWidth, surfaceHeight)
        val orientation = getRotationDegrees(event, pointerIndex)

        activity.nativeSendTouch(
            action,
            pointerId,
            xFrac,
            yFrac,
            pressureOrDistance,
            normalizedContactArea.first,
            normalizedContactArea.second,
            orientation
        )
    }

    private fun sendPen(
        inputView: View,
        surfaceView: View,
        event: MotionEvent,
        pointerIndex: Int,
        surfaceWidth: Int,
        surfaceHeight: Int,
        action: Int,
        source: PointerSource,
        activity: MainActivity
    ) {
        val pointerId = event.getPointerId(pointerIndex)
        val (xFrac, yFrac) = getSurfaceRelativeNormalizedXY(
            inputView,
            surfaceView,
            event,
            pointerIndex,
            surfaceWidth,
            surfaceHeight
        )
        val pressureOrDistance = getPressureOrDistance(event, pointerIndex)
        val tilt = normalizeTilt(event, pointerIndex)
        val rotation = getRotationDegrees(event, pointerIndex)
        val toolType = if (source == PointerSource.ERASER) DS_TOUCH_TOOL_ERASER else DS_TOUCH_TOOL_STYLUS
        val buttons = mapStylusButtons(event.buttonState)

        activity.nativeSendPen(
            action,
            pointerId,
            toolType,
            buttons,
            xFrac,
            yFrac,
            pressureOrDistance,
            pressureOrDistance,
            tilt,
            rotation
        )
    }

    private fun sendMouse(
        inputView: View,
        surfaceView: View,
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

        val (xFrac, yFrac) = getSurfaceRelativeNormalizedXY(
            inputView,
            surfaceView,
            event,
            pointerIndex,
            surfaceWidth,
            surfaceHeight
        )
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

    private fun getSurfaceRelativeNormalizedXY(
        inputView: View,
        surfaceView: View,
        event: MotionEvent,
        pointerIndex: Int,
        surfaceWidth: Int,
        surfaceHeight: Int
    ): Pair<Int, Int> {
        var x = event.getX(pointerIndex)
        var y = event.getY(pointerIndex)

        if (inputView !== surfaceView) {
            val inputLocation = IntArray(2)
            val surfaceLocation = IntArray(2)
            inputView.getLocationInWindow(inputLocation)
            surfaceView.getLocationInWindow(surfaceLocation)

            x += (inputLocation[0] - surfaceLocation[0]).toFloat()
            y += (inputLocation[1] - surfaceLocation[1]).toFloat()
        }

        x = x.coerceIn(0f, surfaceWidth.toFloat())
        y = y.coerceIn(0f, surfaceHeight.toFloat())

        return normalizePosition(x, surfaceWidth) to normalizePosition(y, surfaceHeight)
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

    private fun actionForTouchEvent(event: MotionEvent): Int =
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN,
            MotionEvent.ACTION_POINTER_DOWN -> DS_TOUCH_DOWN

            MotionEvent.ACTION_UP,
            MotionEvent.ACTION_POINTER_UP -> {
                if ((event.flags and MotionEvent.FLAG_CANCELED) != 0) {
                    DS_TOUCH_CANCEL
                } else {
                    DS_TOUCH_UP
                }
            }

            MotionEvent.ACTION_MOVE -> DS_TOUCH_MOVE
            MotionEvent.ACTION_HOVER_ENTER,
            MotionEvent.ACTION_HOVER_MOVE -> DS_TOUCH_HOVER
            MotionEvent.ACTION_HOVER_EXIT -> DS_TOUCH_HOVER_LEAVE
            MotionEvent.ACTION_BUTTON_PRESS,
            MotionEvent.ACTION_BUTTON_RELEASE -> DS_TOUCH_BUTTON_ONLY
            MotionEvent.ACTION_CANCEL -> DS_TOUCH_CANCEL_ALL
            else -> -1
        }

    private fun getPressureOrDistance(event: MotionEvent, pointerIndex: Int): Int {
        val device = event.device
        return when (event.actionMasked) {
            MotionEvent.ACTION_HOVER_ENTER,
            MotionEvent.ACTION_HOVER_MOVE,
            MotionEvent.ACTION_HOVER_EXIT -> {
                val range = device?.getMotionRange(MotionEvent.AXIS_DISTANCE, event.source)
                if (range != null) {
                    normalizeValueInRange(event.getAxisValue(MotionEvent.AXIS_DISTANCE, pointerIndex), range)
                } else {
                    0
                }
            }

            else -> normalizeUnit(event.getPressure(pointerIndex))
        }
    }

    private fun getRotationDegrees(event: MotionEvent, pointerIndex: Int): Int {
        val device = event.device
        return if (device?.getMotionRange(MotionEvent.AXIS_ORIENTATION, event.source) != null) {
            normalizeRotation(event.getOrientation(pointerIndex))
        } else {
            ORIENTATION_UNKNOWN
        }
    }

    private fun getNormalizedContactArea(
        event: MotionEvent,
        pointerIndex: Int,
        surfaceWidth: Int,
        surfaceHeight: Int
    ): Pair<Int, Int> {
        if (surfaceWidth <= 0 || surfaceHeight <= 0) {
            return 0 to 0
        }

        val device = event.device
        val orientation = if (device?.getMotionRange(MotionEvent.AXIS_ORIENTATION, event.source) == null) {
            (Math.PI / 4.0).toFloat()
        } else {
            event.getOrientation(pointerIndex)
        }

        val (contactAreaMajor, contactAreaMinor) = when (event.actionMasked) {
            MotionEvent.ACTION_HOVER_ENTER,
            MotionEvent.ACTION_HOVER_MOVE,
            MotionEvent.ACTION_HOVER_EXIT -> event.getToolMajor(pointerIndex) to event.getToolMinor(pointerIndex)

            else -> event.getTouchMajor(pointerIndex) to event.getTouchMinor(pointerIndex)
        }

        val majorCartesian = polarToCartesian(contactAreaMajor, orientation)
        val minorCartesian = polarToCartesian(contactAreaMinor, orientation + (Math.PI / 2.0).toFloat())

        majorCartesian[0] = min(abs(majorCartesian[0]), surfaceWidth.toFloat()) / surfaceWidth.toFloat()
        minorCartesian[0] = min(abs(minorCartesian[0]), surfaceWidth.toFloat()) / surfaceWidth.toFloat()
        majorCartesian[1] = min(abs(majorCartesian[1]), surfaceHeight.toFloat()) / surfaceHeight.toFloat()
        minorCartesian[1] = min(abs(minorCartesian[1]), surfaceHeight.toFloat()) / surfaceHeight.toFloat()

        return normalizeUnit(cartesianToRadius(majorCartesian)) to
            normalizeUnit(cartesianToRadius(minorCartesian))
    }

    private fun normalizeValueInRange(value: Float, range: InputDevice.MotionRange): Int {
        if (!value.isFinite() || range.range == 0f) {
            return 0
        }
        return normalizeUnit((value - range.min) / range.range)
    }

    private fun polarToCartesian(r: Float, theta: Float): FloatArray =
        floatArrayOf((r * cos(theta)), (r * sin(theta)))

    private fun cartesianToRadius(point: FloatArray): Float =
        sqrt((point[0] * point[0]) + (point[1] * point[1]))

    private fun supportsAxis(event: MotionEvent, axis: Int): Boolean =
        event.device?.motionRanges?.any { range ->
            range.axis == axis
        } == true
}
