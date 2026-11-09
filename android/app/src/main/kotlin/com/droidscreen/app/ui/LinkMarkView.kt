package com.droidscreen.app.ui

import android.content.Context
import android.graphics.Canvas
import android.graphics.DashPathEffect
import android.graphics.Paint
import android.graphics.RectF
import android.util.AttributeSet
import android.view.View

/**
 * The Link Line logo glyph: a laptop outline and a tablet outline joined by
 * a yellow cable. Wi-Fi draws the cable with three small gaps, USB solid.
 * Drawn in a 48x24 viewBox scaled to the view size.
 */
class LinkMarkView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : View(context, attrs) {

    /** True to draw the Wi-Fi (dashed) cable, false for USB (solid). */
    var wifi: Boolean = true
        set(value) {
            if (field != value) {
                field = value
                invalidate()
            }
        }

    /** Outline color of the two devices. */
    var deviceColor: Int = LinkLine.TEXT
        set(value) {
            field = value
            devicePaint.color = value
            invalidate()
        }

    private val devicePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 1.5f
        strokeJoin = Paint.Join.ROUND
        strokeCap = Paint.Cap.ROUND
        color = LinkLine.TEXT
    }

    private val cablePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2f
        color = LinkLine.CABLE
    }

    private val laptop = RectF(2.5f, 4.5f, 15.5f, 14f)
    private val tablet = RectF(32f, 5f, 47f, 16f)

    override fun onDraw(canvas: Canvas) {
        val scale = minOf(width / VIEW_W, height / VIEW_H)
        if (scale <= 0f) return
        canvas.save()
        canvas.translate((width - VIEW_W * scale) / 2f, (height - VIEW_H * scale) / 2f)
        canvas.scale(scale, scale)
        canvas.drawRoundRect(laptop, 1.5f, 1.5f, devicePaint)
        canvas.drawLine(0.8f, 17.5f, 17.2f, 17.5f, devicePaint)
        canvas.drawRoundRect(tablet, 2f, 2f, devicePaint)
        val length = CABLE_END - CABLE_START
        cablePaint.pathEffect = if (wifi) wifiDash(length) else null
        canvas.drawLine(CABLE_START, CABLE_Y, CABLE_END, CABLE_Y, cablePaint)
        canvas.restore()
    }

    companion object {
        private const val VIEW_W = 48f
        private const val VIEW_H = 24f
        private const val CABLE_START = 18.5f
        private const val CABLE_END = 31f
        private const val CABLE_Y = 10.8f

        /**
         * Four equal segments separated by three gaps, each gap [gapFraction]
         * of [length] (matches the mockup's pathLength-based dash arrays).
         */
        fun wifiDash(length: Float, gapFraction: Float = 0.04f): DashPathEffect {
            val gap = length * gapFraction
            val seg = (length - 3 * gap) / 4f
            return DashPathEffect(floatArrayOf(seg, gap, seg, gap, seg, gap, seg, length), 0f)
        }
    }
}
