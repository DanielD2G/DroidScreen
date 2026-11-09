package com.droidscreen.app.ui

import android.content.Context
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Typeface
import android.text.TextPaint
import android.text.style.MetricAffectingSpan
import android.util.Log
import com.droidscreen.app.R

/**
 * "Link Line" design tokens shared by the custom-drawn views
 * (see mockups/droidscreen-ui/spec-3.md). XML layouts use the matching
 * `@color/ds_*` resources.
 */
object LinkLine {
    val INK = Color.parseColor("#0F1115")
    val SURFACE = Color.parseColor("#171A20")
    val RAISED = Color.parseColor("#1F232B")
    val LINE = Color.parseColor("#2A2F39")
    val LINE_2 = Color.parseColor("#363C48")
    val TEXT = Color.parseColor("#F3F1EA")
    val TEXT_2 = Color.parseColor("#A8ABB3")
    val TEXT_3 = Color.parseColor("#6C707A")
    val CABLE = Color.parseColor("#F2D13A")
    val LIVE = Color.parseColor("#5BD68A")
    val WARN = Color.parseColor("#F2994A")
    val ERROR = Color.parseColor("#EB5757")

    const val RADIUS_CARD_DP = 14f
    const val RADIUS_CONTROL_DP = 10f

    private val cache = HashMap<Int, Typeface>()

    /** Instrument Sans at [weight] (400/500/600), or the system sans-serif. */
    fun sans(context: Context, weight: Int = 400): Typeface = when {
        weight >= 600 -> font(context, R.font.instrument_sans_semibold, Typeface.SANS_SERIF, true)
        weight >= 500 -> font(context, R.font.instrument_sans_medium, Typeface.SANS_SERIF, true)
        else -> font(context, R.font.instrument_sans_regular, Typeface.SANS_SERIF, false)
    }

    /** IBM Plex Mono at [weight] (400/500), or the system monospace. */
    fun mono(context: Context, weight: Int = 400): Typeface = when {
        weight >= 500 -> font(context, R.font.ibm_plex_mono_medium, Typeface.MONOSPACE, true)
        else -> font(context, R.font.ibm_plex_mono_regular, Typeface.MONOSPACE, false)
    }

    private fun font(context: Context, id: Int, fallback: Typeface, bold: Boolean): Typeface =
        cache.getOrPut(id) {
            try {
                context.resources.getFont(id)
            } catch (e: Exception) {
                Log.w("DroidScreen", "Bundled font unavailable, using system fallback", e)
                Typeface.create(fallback, if (bold) Typeface.BOLD else Typeface.NORMAL)
            }
        }

    /** A text span that switches to [typeface] (API 26-safe TypefaceSpan). */
    class TypefaceSpanCompat(
        private val typeface: Typeface,
        private val color: Int? = null
    ) : MetricAffectingSpan() {
        override fun updateDrawState(tp: TextPaint) {
            apply(tp)
            color?.let { tp.color = it }
        }

        override fun updateMeasureState(tp: TextPaint) = apply(tp)

        private fun apply(paint: Paint) {
            paint.typeface = typeface
        }
    }
}
