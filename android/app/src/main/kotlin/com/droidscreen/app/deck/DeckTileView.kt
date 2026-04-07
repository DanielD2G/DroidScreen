package com.droidscreen.app.deck

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Typeface
import android.view.MotionEvent
import android.view.View
import android.view.animation.OvershootInterpolator

/**
 * A compact app-icon tile (64x76dp) for the floating Stream Deck panel.
 * Shows a small rounded icon (or letter placeholder) with a tiny label below.
 */
class DeckTileView(
    context: Context,
    private val tileConfig: TileConfig.AppTile,
    private val onAction: (actionType: Int, tileId: String) -> Unit
) : View(context) {

    // -- Metrics ----------------------------------------------------------

    private val density = resources.displayMetrics.density
    private val cornerRadius = CORNER_RADIUS_DP * density
    private val iconSize = ICON_SIZE_DP * density
    private val iconCorner = ICON_CORNER_DP * density
    private val labelTextSize = LABEL_SP * resources.displayMetrics.scaledDensity

    // -- Paints -----------------------------------------------------------

    private val bgPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(0x66, 0x08, 0x0A, 0x12)
        style = Paint.Style.FILL
    }

    private val borderPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 0.5f * density
        color = Color.argb(0x38, 0xFF, 0xFF, 0xFF)
    }

    private val iconPaint = Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG)

    private val placeholderBgPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(0x44, 0x18, 0x1C, 0x28)
        style = Paint.Style.FILL
    }

    private val letterPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(0xDD, 0xFF, 0xFF, 0xFF)
        textAlign = Paint.Align.CENTER
        typeface = Typeface.create(Typeface.DEFAULT, Typeface.BOLD)
    }

    private val labelPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(0xBB, 0xFF, 0xFF, 0xFF)
        textSize = labelTextSize
        textAlign = Paint.Align.CENTER
        typeface = Typeface.create(Typeface.DEFAULT, Typeface.NORMAL)
        letterSpacing = 0.01f
    }

    // -- Rects (lazily sized) ---------------------------------------------

    private val outerRect = RectF()

    init {
        isClickable = true
        isFocusable = true
    }

    // ------------------------------------------------------------------
    // Measure / Draw
    // ------------------------------------------------------------------

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        outerRect.set(0f, 0f, w.toFloat(), h.toFloat())
    }

    override fun onDraw(canvas: Canvas) {
        if (isInEditMode) {
            canvas.drawColor(Color.DKGRAY)
            return
        }

        val w = width.toFloat()
        val h = height.toFloat()
        val cx = w / 2f

        // 1. Background pill
        canvas.drawRoundRect(outerRect, cornerRadius, cornerRadius, bgPaint)
        canvas.drawRoundRect(outerRect, cornerRadius, cornerRadius, borderPaint)

        // 2. Icon area — centered horizontally, near the top
        val iconTop = ICON_TOP_PAD_DP * density
        val iconLeft = cx - iconSize / 2f
        val iconRect = RectF(iconLeft, iconTop, iconLeft + iconSize, iconTop + iconSize)

        val iconBitmap = tileConfig.iconBitmap
        if (iconBitmap != null) {
            canvas.drawBitmap(iconBitmap, null, iconRect, iconPaint)
        } else {
            // Placeholder: rounded rect with first letter
            canvas.drawRoundRect(iconRect, iconCorner, iconCorner, placeholderBgPaint)
            if (tileConfig.label.isNotEmpty()) {
                letterPaint.textSize = iconSize * 0.45f
                val letterY = iconRect.centerY() + iconSize * 0.16f
                canvas.drawText(
                    tileConfig.label.first().uppercase(),
                    cx, letterY, letterPaint
                )
            }
        }

        // 3. Label below icon (single line, ellipsized)
        if (tileConfig.label.isNotBlank()) {
            val labelY = iconRect.bottom + LABEL_TOP_GAP_DP * density + labelTextSize
            val maxLabelW = w - 4f * density
            val ellipsized = ellipsize(tileConfig.label, labelPaint, maxLabelW)
            canvas.drawText(ellipsized, cx, labelY, labelPaint)
        }
    }

    private fun ellipsize(text: String, paint: Paint, maxWidth: Float): String {
        if (maxWidth <= 0f) return ""
        return android.text.TextUtils.ellipsize(
            text, android.text.TextPaint(paint), maxWidth,
            android.text.TextUtils.TruncateAt.END
        ).toString()
    }

    // ------------------------------------------------------------------
    // Touch — press / release animation + click
    // ------------------------------------------------------------------

    @Suppress("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                animate().scaleX(PRESS_SCALE).scaleY(PRESS_SCALE)
                    .setDuration(PRESS_ANIM_MS).setInterpolator(null).start()
                parent?.requestDisallowInterceptTouchEvent(true)
            }
            MotionEvent.ACTION_UP -> {
                animate().scaleX(1f).scaleY(1f)
                    .setDuration(RELEASE_ANIM_MS)
                    .setInterpolator(OvershootInterpolator(2f)).start()
                if (isPointInsideView(event.x, event.y)) {
                    onAction(ACTION_TAP, tileConfig.id)
                }
            }
            MotionEvent.ACTION_CANCEL -> {
                animate().scaleX(1f).scaleY(1f)
                    .setDuration(RELEASE_ANIM_MS)
                    .setInterpolator(OvershootInterpolator(2f)).start()
            }
        }
        return true
    }

    private fun isPointInsideView(x: Float, y: Float): Boolean {
        return x >= 0 && x <= width && y >= 0 && y <= height
    }

    // ------------------------------------------------------------------
    // Constants
    // ------------------------------------------------------------------

    companion object {
        const val ACTION_TAP = 0

        private const val CORNER_RADIUS_DP = 14f
        private const val ICON_SIZE_DP = 52f
        private const val ICON_CORNER_DP = 12f
        private const val ICON_TOP_PAD_DP = 12f
        private const val LABEL_TOP_GAP_DP = 6f
        private const val LABEL_SP = 11f
        private const val PRESS_SCALE = 0.95f
        private const val PRESS_ANIM_MS = 80L
        private const val RELEASE_ANIM_MS = 150L
    }
}
