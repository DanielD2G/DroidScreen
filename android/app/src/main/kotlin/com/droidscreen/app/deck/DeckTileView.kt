package com.droidscreen.app.deck

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.view.MotionEvent
import android.view.View
import android.view.animation.OvershootInterpolator
import com.droidscreen.app.ui.LinkLine

/**
 * An app-shortcut tile for the Deck rail (Link Line): raised surface with a
 * 1dp line border, the app icon (or a letter placeholder) above a small
 * label. While pressed the tile fills with the cable yellow.
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
        color = LinkLine.RAISED
        style = Paint.Style.FILL
    }

    private val borderPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 1f * density
        color = LinkLine.LINE
    }

    private val iconPaint = Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG)

    private val placeholderBgPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 1.5f * density
        color = LinkLine.TEXT
    }

    private val letterPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = LinkLine.TEXT
        textAlign = Paint.Align.CENTER
        typeface = LinkLine.sans(context, 600)
    }

    private val labelPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = LinkLine.TEXT_2
        textSize = labelTextSize
        textAlign = Paint.Align.CENTER
        typeface = LinkLine.sans(context)
    }

    private var pressedState = false

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
        val inset = borderPaint.strokeWidth / 2f
        outerRect.set(inset, inset, w - inset, h - inset)
    }

    override fun onDraw(canvas: Canvas) {
        if (isInEditMode) {
            canvas.drawColor(Color.DKGRAY)
            return
        }

        val w = width.toFloat()
        val h = height.toFloat()
        val cx = w / 2f

        // 1. Tile background (yellow while pressed)
        bgPaint.color = if (pressedState) LinkLine.CABLE else LinkLine.RAISED
        borderPaint.color = if (pressedState) LinkLine.CABLE else LinkLine.LINE
        val inkOnPress = if (pressedState) LinkLine.INK else LinkLine.TEXT
        letterPaint.color = inkOnPress
        placeholderBgPaint.color = inkOnPress
        labelPaint.color = if (pressedState) LinkLine.INK else LinkLine.TEXT_2
        canvas.drawRoundRect(outerRect, cornerRadius, cornerRadius, bgPaint)
        canvas.drawRoundRect(outerRect, cornerRadius, cornerRadius, borderPaint)

        // 2. Icon + label block, centered vertically
        val blockH = iconSize + LABEL_TOP_GAP_DP * density + labelTextSize
        val iconTop = (h - blockH) / 2f
        val iconLeft = cx - iconSize / 2f
        val iconRect = RectF(iconLeft, iconTop, iconLeft + iconSize, iconTop + iconSize)

        val iconBitmap = tileConfig.iconBitmap
        if (iconBitmap != null) {
            canvas.drawBitmap(iconBitmap, null, iconRect, iconPaint)
        } else {
            // Placeholder: rounded rect with first letter
            canvas.drawRoundRect(iconRect, iconCorner, iconCorner, placeholderBgPaint)
            if (tileConfig.label.isNotEmpty()) {
                letterPaint.textSize = iconSize * 0.5f
                val letterY = iconRect.centerY() - (letterPaint.ascent() + letterPaint.descent()) / 2f
                canvas.drawText(
                    tileConfig.label.first().uppercase(),
                    cx, letterY, letterPaint
                )
            }
        }

        // 3. Label below icon (single line, ellipsized)
        if (tileConfig.label.isNotBlank()) {
            val labelY = iconRect.bottom + LABEL_TOP_GAP_DP * density - labelPaint.ascent()
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
                setPressedState(true)
                animate().scaleX(PRESS_SCALE).scaleY(PRESS_SCALE)
                    .setDuration(PRESS_ANIM_MS).setInterpolator(null).start()
                // No requestDisallowInterceptTouchEvent: a vertical drag from a
                // tile scrolls the rail (CANCEL resets the press state).
            }
            MotionEvent.ACTION_UP -> {
                setPressedState(false)
                animate().scaleX(1f).scaleY(1f)
                    .setDuration(RELEASE_ANIM_MS)
                    .setInterpolator(OvershootInterpolator(2f)).start()
                if (isPointInsideView(event.x, event.y)) {
                    onAction(ACTION_TAP, tileConfig.id)
                }
            }
            MotionEvent.ACTION_CANCEL -> {
                setPressedState(false)
                animate().scaleX(1f).scaleY(1f)
                    .setDuration(RELEASE_ANIM_MS)
                    .setInterpolator(OvershootInterpolator(2f)).start()
            }
        }
        return true
    }

    private fun setPressedState(pressed: Boolean) {
        if (pressedState != pressed) {
            pressedState = pressed
            invalidate()
        }
    }

    private fun isPointInsideView(x: Float, y: Float): Boolean {
        return x >= 0 && x <= width && y >= 0 && y <= height
    }

    // ------------------------------------------------------------------
    // Constants
    // ------------------------------------------------------------------

    companion object {
        const val ACTION_TAP = 0

        private const val CORNER_RADIUS_DP = 10f
        private const val ICON_SIZE_DP = 32f
        private const val ICON_CORNER_DP = 8f
        private const val LABEL_TOP_GAP_DP = 6f
        private const val LABEL_SP = 11f
        private const val PRESS_SCALE = 0.97f
        private const val PRESS_ANIM_MS = 80L
        private const val RELEASE_ANIM_MS = 150L
    }
}
