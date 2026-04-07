package com.droidscreen.app

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.view.MotionEvent
import kotlin.math.abs

/**
 * Detects a 3-finger long press to toggle the Stream Deck overlay.
 *
 * The gesture is recognized when exactly 3 fingers are held stationary
 * (within [MAX_DRIFT_DP] of the initial centroid) for [HOLD_DURATION_MS].
 * This avoids conflict with system 3-finger swipe gestures.
 *
 * Returns `true` from [onTouchEvent] when the gesture is being tracked,
 * signaling the caller to suppress forwarding to the protocol layer.
 */
class DeckGestureDetector(
    context: Context,
    private val onDeckToggle: () -> Unit
) {

    private companion object {
        const val REQUIRED_FINGERS = 3
        const val HOLD_DURATION_MS = 600L
        const val MAX_DRIFT_DP = 20f
    }

    private val density = context.resources.displayMetrics.density
    private val maxDriftPx = MAX_DRIFT_DP * density
    private val handler = Handler(Looper.getMainLooper())

    private var tracking = false
    private var gestureConsumed = false
    private var startX = 0f
    private var startY = 0f

    private val holdRunnable = Runnable {
        if (tracking) {
            gestureConsumed = true
            tracking = false
            onDeckToggle()
        }
    }

    /**
     * Feed every [MotionEvent] before it reaches the protocol router.
     *
     * @return `true` if this detector is consuming the event (caller should
     *         NOT forward it to the input protocol).
     */
    fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                reset()
            }

            MotionEvent.ACTION_POINTER_DOWN -> {
                if (event.pointerCount == REQUIRED_FINGERS && !gestureConsumed) {
                    tracking = true
                    val centroid = centroidOf(event)
                    startX = centroid[0]
                    startY = centroid[1]
                    handler.postDelayed(holdRunnable, HOLD_DURATION_MS)
                }
            }

            MotionEvent.ACTION_MOVE -> {
                if (!tracking) return gestureConsumed
                if (event.pointerCount != REQUIRED_FINGERS) {
                    reset()
                    return false
                }

                // Check drift — if fingers moved too much, cancel
                val centroid = centroidOf(event)
                val dx = abs(centroid[0] - startX)
                val dy = abs(centroid[1] - startY)
                if (dx > maxDriftPx || dy > maxDriftPx) {
                    reset()
                    return false
                }

                return true
            }

            MotionEvent.ACTION_POINTER_UP -> {
                if (tracking && event.pointerCount - 1 < REQUIRED_FINGERS) {
                    reset()
                }
            }

            MotionEvent.ACTION_UP,
            MotionEvent.ACTION_CANCEL -> {
                val wasConsumed = gestureConsumed || tracking
                reset()
                return wasConsumed
            }
        }

        return tracking || gestureConsumed
    }

    private fun centroidOf(event: MotionEvent): FloatArray {
        var sumX = 0f
        var sumY = 0f
        for (i in 0 until event.pointerCount) {
            sumX += event.getX(i)
            sumY += event.getY(i)
        }
        val count = event.pointerCount.toFloat()
        return floatArrayOf(sumX / count, sumY / count)
    }

    private fun reset() {
        handler.removeCallbacks(holdRunnable)
        tracking = false
        gestureConsumed = false
        startX = 0f
        startY = 0f
    }
}
