package com.droidscreen.app.ui

import android.animation.ValueAnimator
import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.graphics.drawable.Drawable
import android.util.AttributeSet
import android.view.View
import android.view.animation.LinearInterpolator
import android.view.animation.PathInterpolator
import com.droidscreen.app.R

/**
 * Pairing hero: the Mac on the left, this tablet on the right, joined by the
 * yellow cable. While [waiting] is true three pulse dots travel along the
 * cable (a ValueAnimator that only runs while the view is attached and
 * waiting). Wi-Fi draws the cable with three gaps, USB draws it solid.
 *
 * Geometry follows the mockup's 1088x200 viewBox, scaled to the view width;
 * the device labels are drawn below it at their natural text size.
 */
class LinkHeroView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : View(context, attrs) {

    /** True when the tablet is reachable over Wi-Fi (dashed cable). */
    var wifi: Boolean = true
        set(value) {
            if (field != value) {
                field = value
                invalidate()
            }
        }

    /** IPv4 address shown under the tablet, or null to hide it. */
    var address: String? = null
        set(value) {
            if (field != value) {
                field = value
                invalidate()
            }
        }

    /** Label drawn on the middle of the cable, e.g. "Wi-Fi · waiting". */
    var cableLabel: String? = null
        set(value) {
            if (field != value) {
                field = value
                invalidate()
            }
        }

    /** Runs the pulse animation; only true on the waiting screen. */
    var waiting: Boolean = false
        set(value) {
            if (field != value) {
                field = value
                updateAnimator()
                invalidate()
            }
        }

    private val density = resources.displayMetrics.density
    private val sp = resources.displayMetrics.scaledDensity

    private val devicePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2.5f
        strokeJoin = Paint.Join.ROUND
        strokeCap = Paint.Cap.ROUND
        color = LinkLine.TEXT
    }
    private val deviceFillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = LinkLine.SURFACE }
    private val screenPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = LinkLine.INK }
    private val cablePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 4f
        color = LinkLine.CABLE
    }
    private val pulsePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = LinkLine.CABLE }
    private val labelPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = LinkLine.TEXT_2
        textSize = 13f * sp
        textAlign = Paint.Align.CENTER
        typeface = LinkLine.sans(context)
    }
    private val addressPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = LinkLine.TEXT
        textSize = 15f * sp
        textAlign = Paint.Align.CENTER
        typeface = LinkLine.mono(context)
    }
    private val midPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = LinkLine.CABLE
        textSize = 13f * sp
        typeface = LinkLine.mono(context)
    }
    private val midBgPaint = Paint().apply { color = LinkLine.INK }

    private val laptop = RectF(20f, 30f, 210f, 152f)
    private val laptopScreen = RectF(34f, 44f, 196f, 138f)
    private val laptopBase = Path().apply {
        moveTo(0f, 166f)
        lineTo(230f, 166f)
        lineTo(216f, 180f)
        lineTo(14f, 180f)
        close()
    }
    private val tablet = RectF(870f, 36f, 1080f, 168f)
    private val tabletScreen = RectF(884f, 50f, 1066f, 154f)

    private val pulseEase = PathInterpolator(0.45f, 0f, 0.55f, 1f)
    private var phase = 0f
    private var animator: ValueAnimator? = null

    private val wifiIcon: Drawable? = context.getDrawable(R.drawable.ic_ph_wifi_high)?.mutate()
    private val usbIcon: Drawable? = context.getDrawable(R.drawable.ic_ph_usb)?.mutate()

    private val heroScale: Float get() = width / VIEW_W

    /** Y of the cable inside this view, used to anchor the approval card. */
    fun cableCenterY(): Float = CABLE_Y * heroScale

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = MeasureSpec.getSize(widthMeasureSpec)
        val h = (w * VIEW_H / VIEW_W + LABEL_AREA_DP * density).toInt()
        setMeasuredDimension(w, resolveSize(h, heightMeasureSpec))
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        updateAnimator()
    }

    override fun onDetachedFromWindow() {
        stopAnimator()
        super.onDetachedFromWindow()
    }

    override fun onVisibilityAggregated(isVisible: Boolean) {
        super.onVisibilityAggregated(isVisible)
        if (isVisible) updateAnimator() else stopAnimator()
    }

    private fun updateAnimator() {
        val shouldRun = waiting && isAttachedToWindow && isShown && ValueAnimator.areAnimatorsEnabled()
        if (!shouldRun) {
            stopAnimator()
            return
        }
        if (animator != null) return
        animator = ValueAnimator.ofFloat(0f, 1f).apply {
            duration = PULSE_PERIOD_MS
            repeatCount = ValueAnimator.INFINITE
            interpolator = LinearInterpolator()
            addUpdateListener {
                phase = it.animatedValue as Float
                invalidate()
            }
            start()
        }
    }

    private fun stopAnimator() {
        animator?.cancel()
        animator = null
    }

    override fun onDraw(canvas: Canvas) {
        val s = heroScale
        if (s <= 0f) return

        canvas.save()
        canvas.scale(s, s)
        canvas.drawRoundRect(laptop, 10f, 10f, deviceFillPaint)
        canvas.drawRoundRect(laptop, 10f, 10f, devicePaint)
        canvas.drawRoundRect(laptopScreen, 3f, 3f, screenPaint)
        canvas.drawPath(laptopBase, devicePaint)
        canvas.drawRoundRect(tablet, 14f, 14f, deviceFillPaint)
        canvas.drawRoundRect(tablet, 14f, 14f, devicePaint)
        canvas.drawRoundRect(tabletScreen, 4f, 4f, screenPaint)

        val cableLength = CABLE_END - CABLE_START
        cablePaint.pathEffect = if (wifi) LinkMarkView.wifiDash(cableLength, 0.03f) else null
        canvas.drawLine(CABLE_START, CABLE_Y, CABLE_END, CABLE_Y, cablePaint)

        drawPulses(canvas)
        canvas.restore()

        drawCableLabel(canvas, s)
        drawDeviceLabels(canvas, s)
    }

    private fun drawPulses(canvas: Canvas) {
        val animating = animator != null
        if (!waiting) return
        for (i in 0 until PULSE_COUNT) {
            val x: Float
            val alpha: Float
            if (animating) {
                var t = phase - i * PULSE_STAGGER_MS / PULSE_PERIOD_MS.toFloat()
                if (t < 0f) t += 1f
                x = PULSE_START + PULSE_TRAVEL * pulseEase.getInterpolation(t)
                alpha = when {
                    t < 0.12f -> t / 0.12f
                    t > 0.88f -> (1f - t) / 0.12f
                    else -> 1f
                }
            } else {
                // Reduced motion: three static dots at quarter points.
                x = PULSE_START + PULSE_TRAVEL * (i + 1) / 4f
                alpha = 1f
            }
            pulsePaint.alpha = (alpha.coerceIn(0f, 1f) * 255).toInt()
            canvas.drawCircle(x, CABLE_Y, PULSE_RADIUS, pulsePaint)
        }
    }

    private fun drawCableLabel(canvas: Canvas, s: Float) {
        val label = cableLabel ?: return
        val icon = if (wifi) wifiIcon else usbIcon
        val iconSize = 14f * density
        val gap = 6f * density
        val padH = 12f * density
        val padV = 4f * density
        val textW = midPaint.measureText(label)
        val contentW = iconSize + gap + textW
        val cx = VIEW_W / 2f * s
        val cy = CABLE_Y * s
        val fm = midPaint.fontMetrics
        val textH = fm.descent - fm.ascent
        val boxH = maxOf(textH, iconSize) + 2 * padV
        val left = cx - contentW / 2f - padH
        canvas.drawRect(left, cy - boxH / 2f, cx + contentW / 2f + padH, cy + boxH / 2f, midBgPaint)
        val iconLeft = (cx - contentW / 2f).toInt()
        icon?.let {
            it.setTint(LinkLine.CABLE)
            it.setBounds(
                iconLeft, (cy - iconSize / 2f).toInt(),
                (iconLeft + iconSize).toInt(), (cy + iconSize / 2f).toInt()
            )
            it.draw(canvas)
        }
        val baseline = cy - (fm.ascent + fm.descent) / 2f
        canvas.drawText(label, iconLeft + iconSize + gap, baseline, midPaint)
    }

    private fun drawDeviceLabels(canvas: Canvas, s: Float) {
        val top = LABEL_TOP * s + 4f * density
        val labelBaseline = top - labelPaint.fontMetrics.ascent
        canvas.drawText("Your Mac", LAPTOP_CENTER_X * s, labelBaseline, labelPaint)
        canvas.drawText("This tablet", TABLET_CENTER_X * s, labelBaseline, labelPaint)
        address?.let {
            val addrBaseline = labelBaseline + labelPaint.fontMetrics.descent +
                2f * density - addressPaint.fontMetrics.ascent
            canvas.drawText(it, TABLET_CENTER_X * s, addrBaseline, addressPaint)
        }
    }

    private companion object {
        const val VIEW_W = 1088f
        const val VIEW_H = 200f
        const val LABEL_AREA_DP = 48f
        const val LABEL_TOP = 192f
        const val CABLE_Y = 100f
        const val CABLE_START = 236f
        const val CABLE_END = 852f
        const val LAPTOP_CENTER_X = 115f
        const val TABLET_CENTER_X = 975f
        const val PULSE_COUNT = 3
        const val PULSE_RADIUS = 6f
        const val PULSE_START = 244f
        const val PULSE_TRAVEL = 600f
        const val PULSE_PERIOD_MS = 2800L
        const val PULSE_STAGGER_MS = 450f
    }
}
