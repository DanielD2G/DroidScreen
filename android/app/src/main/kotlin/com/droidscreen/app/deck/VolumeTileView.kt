package com.droidscreen.app.deck

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.graphics.Typeface
import android.view.MotionEvent
import android.view.View
import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * Horizontal volume slider bar for the compact Stream Deck panel.
 * Shows a speaker icon on the left (tap to mute), a horizontal track
 * with a filled portion and circular thumb, and a small percentage label.
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
    private val tileCorner = TILE_CORNER_DP * density

    // -- State ------------------------------------------------------------

    private var volume = 0          // 0..65535
    private var muted = false
    private var dragStartX = 0f
    private var dragStartVolume = 0
    private var isDragging = false

    // -- Paints -----------------------------------------------------------

    private val tileBgPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(0x76, 0x08, 0x09, 0x12)
        style = Paint.Style.FILL
    }

    private val tileBorderPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(0x2C, 0xFF, 0xFF, 0xFF)
        style = Paint.Style.STROKE
        strokeWidth = 0.5f * density
    }

    private val trackBgPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(0x44, 0xFF, 0xFF, 0xFF)
        style = Paint.Style.FILL
    }

    private val trackFillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.parseColor("#8B5CF6")
        style = Paint.Style.FILL
    }

    private val trackMutedPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(0x40, 0xFF, 0x44, 0x44)
        style = Paint.Style.FILL
    }

    private val thumbPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        style = Paint.Style.FILL
    }

    private val iconPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(0xCC, 0xFF, 0xFF, 0xFF)
        style = Paint.Style.FILL
    }

    private val muteStrikePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(0xCC, 0xFF, 0x44, 0x44)
        style = Paint.Style.STROKE
        strokeWidth = 1.5f * density
        strokeCap = Paint.Cap.ROUND
    }

    private val pctPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(0x99, 0xFF, 0xFF, 0xFF)
        textSize = PCT_TEXT_SP * sp
        typeface = Typeface.create(Typeface.DEFAULT, Typeface.NORMAL)
        textAlign = Paint.Align.RIGHT
    }

    init {
        isClickable = true
        isFocusable = true
    }

    // ------------------------------------------------------------------
    // Layout helpers
    // ------------------------------------------------------------------

    /** Left edge of the track, after the speaker icon. */
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
        val bgRect = RectF(0f, 0f, width.toFloat(), height.toFloat())

        canvas.drawRoundRect(bgRect, tileCorner, tileCorner, tileBgPaint)
        canvas.drawRoundRect(bgRect, tileCorner, tileCorner, tileBorderPaint)

        // 1. Speaker icon (left side)
        drawSpeakerIcon(canvas, ICON_AREA_DP * density / 2f, cy)

        // 2. Track background
        val trackRect = RectF(tl, cy - th / 2f, tr, cy + th / 2f)
        canvas.drawRoundRect(trackRect, tRadius, tRadius, trackBgPaint)

        // 3. Filled portion
        val fraction = if (muted) 0f else volume / MAX_VOLUME.toFloat()
        val fillRight = tl + (tr - tl) * fraction.coerceIn(0f, 1f)
        if (fillRight > tl + 1f) {
            val fillRect = RectF(tl, cy - th / 2f, fillRight, cy + th / 2f)
            val fillPaint = if (muted) trackMutedPaint else trackFillPaint
            canvas.drawRoundRect(fillRect, tRadius, tRadius, fillPaint)
        }

        // 4. Thumb
        if (!muted && fraction > 0f) {
            val thumbX = fillRight.coerceIn(tl + thumbRadius(), tr - thumbRadius())
            canvas.drawCircle(thumbX, cy, thumbRadius(), thumbPaint)
        }

        // 5. Percentage text (right side)
        val pct = if (muted) "Mute" else "${((volume / MAX_VOLUME.toFloat()) * 100).roundToInt()}%"
        val pctX = width - 4f * density
        val pctY = cy + pctPaint.textSize * 0.35f
        canvas.drawText(pct, pctX, pctY, pctPaint)
    }

    private fun drawSpeakerIcon(canvas: Canvas, cx: Float, cy: Float) {
        val sz = SPEAKER_ICON_SIZE_DP * density
        // Speaker body
        val bodyPaint = Paint(iconPaint)
        if (muted) bodyPaint.color = Color.argb(0xAA, 0xFF, 0x44, 0x44)

        val bodyLeft = cx - sz * 0.5f
        val bodyRight = cx - sz * 0.05f
        val bodyTop = cy - sz * 0.25f
        val bodyBottom = cy + sz * 0.25f
        canvas.drawRect(bodyLeft, bodyTop, bodyRight, bodyBottom, bodyPaint)

        // Cone
        val path = Path()
        path.moveTo(bodyRight, bodyTop)
        path.lineTo(cx + sz * 0.4f, cy - sz * 0.55f)
        path.lineTo(cx + sz * 0.4f, cy + sz * 0.55f)
        path.lineTo(bodyRight, bodyBottom)
        path.close()
        canvas.drawPath(path, bodyPaint)

        if (muted) {
            // Strike-through
            canvas.drawLine(
                cx - sz * 0.6f, cy - sz * 0.5f,
                cx + sz * 0.6f, cy + sz * 0.5f,
                muteStrikePaint
            )
        } else {
            // Sound waves (small arcs)
            val wavePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
                color = Color.argb(0x60, 0xFF, 0xFF, 0xFF)
                style = Paint.Style.STROKE
                strokeWidth = 1f * density
                strokeCap = Paint.Cap.ROUND
            }
            val waveRect = RectF(
                cx + sz * 0.2f, cy - sz * 0.4f,
                cx + sz * 0.8f, cy + sz * 0.4f
            )
            canvas.drawArc(waveRect, -40f, 80f, false, wavePaint)
        }
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
                            onVolumeChange(volume, muted)
                            invalidate()
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
                }
                isDragging = false
                return true
            }
            MotionEvent.ACTION_CANCEL -> {
                isDragging = false
                return true
            }
        }
        return super.onTouchEvent(event)
    }

    // ------------------------------------------------------------------
    // State update
    // ------------------------------------------------------------------

    fun updateVolume(volume: Int, muted: Boolean) {
        this.volume = volume.coerceIn(0, MAX_VOLUME)
        this.muted = muted
        invalidate()
    }

    // ------------------------------------------------------------------
    // Constants
    // ------------------------------------------------------------------

    companion object {
        const val MAX_VOLUME = 65535

        private const val TRACK_HEIGHT_DP = 6f
        private const val THUMB_RADIUS_DP = 8f
        private const val ICON_AREA_DP = 42f
        private const val PCT_AREA_DP = 46f
        private const val PCT_TEXT_SP = 12f
        private const val SPEAKER_ICON_SIZE_DP = 10f
        private const val TILE_CORNER_DP = 16f
    }
}
