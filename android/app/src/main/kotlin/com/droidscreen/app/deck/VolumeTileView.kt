package com.droidscreen.app.deck

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.drawable.Drawable
import android.view.MotionEvent
import android.view.View
import com.droidscreen.app.R
import com.droidscreen.app.ui.LinkLine
import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * Volume row for the Deck rail (Link Line): a 44dp mute button on the left
 * (yellow while muted), a 4dp track with a yellow fill and a warm-white
 * thumb, and the percentage in mono on the right.
 *
 * Volume range: 0..65535 (matching system volume APIs).
 */
class VolumeTileView(
    context: Context,
    private val tileConfig: TileConfig.VolumeTile,
    private val onVolumeChange: (volume: Int, muted: Boolean) -> Unit
) : View(context) {

    // -- Metrics ----------------------------------------------------------

    private val density = resources.displayMetrics.density
    private val sp = resources.displayMetrics.scaledDensity

    // -- State ------------------------------------------------------------

    private var volume = 0          // 0..65535
    private var muted = false
    private var dragStartX = 0f
    private var dragStartVolume = 0
    private var isDragging = false
    private var lastDragEndTime = 0L   // SystemClock.uptimeMillis – cooldown after drag
    private var lastSendTime = 0L      // throttle outgoing volume messages during drag

    // -- Paints -----------------------------------------------------------

    private val muteBorderPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = LinkLine.LINE
        style = Paint.Style.STROKE
        strokeWidth = 1f * density
    }

    private val muteFillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = LinkLine.CABLE
        style = Paint.Style.FILL
    }

    private val trackBgPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = LinkLine.LINE_2
        style = Paint.Style.FILL
    }

    private val trackFillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = LinkLine.CABLE
        style = Paint.Style.FILL
    }

    private val thumbPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = LinkLine.TEXT
        style = Paint.Style.FILL
    }

    private val thumbRingPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = LinkLine.INK
        style = Paint.Style.STROKE
        strokeWidth = 2f * density
    }

    private val pctPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = LinkLine.TEXT
        textSize = PCT_TEXT_SP * sp
        typeface = LinkLine.mono(context)
        textAlign = Paint.Align.RIGHT
    }

    private val speakerIcon: Drawable? = context.getDrawable(R.drawable.ic_ph_speaker_high)?.mutate()
    private val mutedIcon: Drawable? = context.getDrawable(R.drawable.ic_ph_speaker_slash)?.mutate()

    init {
        isClickable = true
        isFocusable = true
    }

    // ------------------------------------------------------------------
    // Layout helpers
    // ------------------------------------------------------------------

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        setMeasuredDimension(
            MeasureSpec.getSize(widthMeasureSpec),
            resolveSize((ROW_HEIGHT_DP * density).toInt(), heightMeasureSpec)
        )
    }

    /** Left edge of the track, after the mute button. */
    private fun trackLeft(): Float = ICON_AREA_DP * density

    /** Right edge of the track, before the percentage text. */
    private fun trackRight(): Float = width - PCT_AREA_DP * density

    private fun trackCenterY(): Float = height / 2f

    private fun trackHeight(): Float = TRACK_HEIGHT_DP * density

    private fun thumbRadius(): Float = THUMB_RADIUS_DP * density

    // ------------------------------------------------------------------
    // Draw
    // ------------------------------------------------------------------

    override fun onDraw(canvas: Canvas) {
        if (isInEditMode) {
            canvas.drawColor(Color.DKGRAY)
            return
        }

        val cy = trackCenterY()
        val tl = trackLeft()
        val tr = trackRight()
        val th = trackHeight()
        val tRadius = th / 2f

        // 1. Mute button (yellow while muted)
        val btn = MUTE_BUTTON_DP * density
        val inset = muteBorderPaint.strokeWidth / 2f
        val btnRect = RectF(inset, cy - btn / 2f + inset, btn - inset, cy + btn / 2f - inset)
        val corner = LinkLine.RADIUS_CONTROL_DP * density
        if (muted) {
            canvas.drawRoundRect(btnRect, corner, corner, muteFillPaint)
        } else {
            canvas.drawRoundRect(btnRect, corner, corner, muteBorderPaint)
        }
        (if (muted) mutedIcon else speakerIcon)?.let {
            val half = SPEAKER_ICON_DP * density / 2f
            it.setTint(if (muted) LinkLine.INK else LinkLine.TEXT)
            it.setBounds(
                (btnRect.centerX() - half).toInt(), (cy - half).toInt(),
                (btnRect.centerX() + half).toInt(), (cy + half).toInt()
            )
            it.draw(canvas)
        }

        // 2. Track background
        val trackRect = RectF(tl, cy - th / 2f, tr, cy + th / 2f)
        canvas.drawRoundRect(trackRect, tRadius, tRadius, trackBgPaint)

        // 3. Filled portion (dimmed while muted)
        val fraction = (volume / MAX_VOLUME.toFloat()).coerceIn(0f, 1f)
        val fillRight = tl + (tr - tl) * fraction
        trackFillPaint.alpha = if (muted) 90 else 255
        if (fillRight > tl + 1f) {
            val fillRect = RectF(tl, cy - th / 2f, fillRight, cy + th / 2f)
            canvas.drawRoundRect(fillRect, tRadius, tRadius, trackFillPaint)
        }

        // 4. Thumb
        thumbPaint.alpha = if (muted) 90 else 255
        val thumbX = fillRight.coerceIn(tl + thumbRadius(), tr - thumbRadius())
        canvas.drawCircle(thumbX, cy, thumbRadius(), thumbPaint)
        canvas.drawCircle(thumbX, cy, thumbRadius(), thumbRingPaint)

        // 5. Percentage text (right side)
        val pct = if (muted) "Mute" else "${(fraction * 100).roundToInt()}%"
        pctPaint.color = if (muted) LinkLine.TEXT_3 else LinkLine.TEXT
        val pctY = cy - (pctPaint.ascent() + pctPaint.descent()) / 2f
        canvas.drawText(pct, width.toFloat(), pctY, pctPaint)
    }

    // ------------------------------------------------------------------
    // Touch — horizontal drag to adjust volume, tap speaker to mute
    // ------------------------------------------------------------------

    @Suppress("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                dragStartX = event.x
                dragStartVolume = volume
                isDragging = false
                parent?.requestDisallowInterceptTouchEvent(true)
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                val dx = event.x - dragStartX
                val dragThreshold = 6f * density
                if (!isDragging && abs(dx) > dragThreshold) {
                    isDragging = true
                }
                if (isDragging) {
                    val trackW = trackRight() - trackLeft()
                    if (trackW > 0f) {
                        val sensitivity = MAX_VOLUME.toFloat() / trackW
                        val newVol = (dragStartVolume + dx * sensitivity)
                            .roundToInt()
                            .coerceIn(0, MAX_VOLUME)
                        if (newVol != volume) {
                            volume = newVol
                            if (muted && volume > 0) muted = false
                            invalidate()
                            // Throttle outgoing messages to ~20/sec during drag
                            val now = android.os.SystemClock.uptimeMillis()
                            if (now - lastSendTime >= SEND_THROTTLE_MS) {
                                lastSendTime = now
                                onVolumeChange(volume, muted)
                            }
                        }
                    }
                }
                return true
            }
            MotionEvent.ACTION_UP -> {
                if (!isDragging) {
                    // Tap in speaker icon area -> toggle mute
                    if (event.x < trackLeft()) {
                        muted = !muted
                        onVolumeChange(volume, muted)
                        invalidate()
                    } else {
                        // Tap on track -> jump to position
                        val tl = trackLeft()
                        val tr = trackRight()
                        val fraction = ((event.x - tl) / (tr - tl)).coerceIn(0f, 1f)
                        volume = (fraction * MAX_VOLUME).roundToInt()
                        if (muted) muted = false
                        onVolumeChange(volume, muted)
                        invalidate()
                    }
                } else {
                    // Drag ended: always send final value (may have been throttled)
                    onVolumeChange(volume, muted)
                }
                isDragging = false
                lastDragEndTime = android.os.SystemClock.uptimeMillis()
                return true
            }
            MotionEvent.ACTION_CANCEL -> {
                isDragging = false
                lastDragEndTime = android.os.SystemClock.uptimeMillis()
                return true
            }
        }
        return super.onTouchEvent(event)
    }

    // ------------------------------------------------------------------
    // State update
    // ------------------------------------------------------------------

    fun updateVolume(volume: Int, muted: Boolean) {
        // Ignore incoming state while user is actively dragging or within
        // the cooldown window after release — prevents the desktop's polling
        // echo from fighting with the user's adjustment.
        if (isDragging) return
        if (android.os.SystemClock.uptimeMillis() - lastDragEndTime < DRAG_COOLDOWN_MS) return

        this.volume = volume.coerceIn(0, MAX_VOLUME)
        this.muted = muted
        invalidate()
    }

    // ------------------------------------------------------------------
    // Constants
    // ------------------------------------------------------------------

    companion object {
        const val MAX_VOLUME = 65535

        private const val DRAG_COOLDOWN_MS = 500L  // ignore incoming state after drag
        private const val SEND_THROTTLE_MS = 50L   // max ~20 sends/sec during drag

        private const val ROW_HEIGHT_DP = 44f
        private const val MUTE_BUTTON_DP = 44f
        private const val TRACK_HEIGHT_DP = 4f
        private const val THUMB_RADIUS_DP = 8f
        private const val ICON_AREA_DP = 54f
        private const val PCT_AREA_DP = 50f
        private const val PCT_TEXT_SP = 13f
        private const val SPEAKER_ICON_DP = 19f
    }
}
