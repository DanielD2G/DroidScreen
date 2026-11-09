package com.droidscreen.app.deck

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.drawable.Drawable
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View
import com.droidscreen.app.R
import com.droidscreen.app.ui.LinkLine

/**
 * The in-stream Link Line edge: a 4dp yellow line along the Deck side of the
 * screen (three small gaps when streaming over Wi-Fi, solid over USB). While
 * the Deck is hidden a small yellow handle tab sits on the line; tapping the
 * tab or the line opens the Deck. Touches anywhere else fall through to the
 * stream underneath.
 */
class DeckEdgeView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : View(context, attrs) {

    /** Called when the user taps the handle to bring the Deck back. */
    var onOpenDeck: (() -> Unit)? = null

    /** Wi-Fi draws a dashed line, USB a solid one. */
    var wifi: Boolean = false
        set(value) {
            if (field != value) {
                field = value
                invalidate()
            }
        }

    /** While the Deck is shown the handle tab is hidden (the rail covers it). */
    var deckShown: Boolean = false
        set(value) {
            if (field != value) {
                field = value
                invalidate()
            }
        }

    private val density = resources.displayMetrics.density
    private val lineWidth = LINE_DP * density
    private val tabRect = RectF()

    private val cablePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = LinkLine.CABLE }
    private val gripPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = LinkLine.INK
        alpha = 150
        strokeWidth = 1f * density
    }
    private val wifiIcon: Drawable? = context.getDrawable(R.drawable.ic_ph_wifi_high)?.mutate()
    private val usbIcon: Drawable? = context.getDrawable(R.drawable.ic_ph_usb)?.mutate()

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        val tabW = TAB_W_DP * density
        val tabH = TAB_H_DP * density
        tabRect.set(0f, (h - tabH) / 2f, tabW, (h + tabH) / 2f)
    }

    override fun onDraw(canvas: Canvas) {
        val h = height.toFloat()
        if (wifi) {
            // Four segments with three 1% gaps, as in the mockup.
            val gap = h * 0.01f
            val seg = (h - 3 * gap) / 4f
            for (i in 0 until 4) {
                val top = i * (seg + gap)
                canvas.drawRect(0f, top, lineWidth, top + seg, cablePaint)
            }
        } else {
            canvas.drawRect(0f, 0f, lineWidth, h, cablePaint)
        }

        if (deckShown) return
        val r = 10f * density
        // Square on the edge side, rounded toward the stream.
        canvas.drawRoundRect(tabRect, r, r, cablePaint)
        canvas.drawRect(tabRect.left, tabRect.top, tabRect.left + r, tabRect.bottom, cablePaint)

        val cx = tabRect.centerX()
        val gripH = 22f * density
        val gripTop = tabRect.centerY() - gripH / 2f - 9f * density
        canvas.drawLine(cx - 1.5f * density, gripTop, cx - 1.5f * density, gripTop + gripH, gripPaint)
        canvas.drawLine(cx + 1.5f * density, gripTop, cx + 1.5f * density, gripTop + gripH, gripPaint)

        val icon = if (wifi) wifiIcon else usbIcon
        icon?.let {
            val size = (15 * density).toInt()
            val left = (cx - size / 2f).toInt()
            val top = (gripTop + gripH + 6f * density).toInt()
            it.setTint(LinkLine.INK)
            it.setBounds(left, top, left + size, top + size)
            it.draw(canvas)
        }
    }

    @Suppress("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (deckShown) return false
        val hit = tabRect.contains(event.x, event.y) || event.x <= lineWidth * 2f
        if (event.actionMasked == MotionEvent.ACTION_DOWN) return hit
        if (event.actionMasked == MotionEvent.ACTION_UP && hit) {
            onOpenDeck?.invoke()
        }
        return true
    }

    private companion object {
        const val LINE_DP = 4f
        const val TAB_W_DP = 30f
        const val TAB_H_DP = 84f
    }
}
