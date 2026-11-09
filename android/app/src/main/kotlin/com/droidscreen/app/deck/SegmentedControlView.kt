package com.droidscreen.app.deck

import android.content.Context
import android.content.res.ColorStateList
import android.util.AttributeSet
import android.util.TypedValue
import android.view.Gravity
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import com.droidscreen.app.R
import com.droidscreen.app.ui.LinkLine

/**
 * Link Line segmented control: an ink track with a yellow pill behind the
 * selected option (ink text), unselected options in secondary text.
 */
class SegmentedControlView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : LinearLayout(context, attrs) {

    /** One option: its label and an optional leading icon drawable. */
    data class Option(val label: String, val iconRes: Int? = null)

    /** Called with the option index when the user picks a different option. */
    var onSelected: ((Int) -> Unit)? = null

    private val density = resources.displayMetrics.density
    private val segments = mutableListOf<LinearLayout>()
    private var selectedIndex = -1

    init {
        orientation = HORIZONTAL
        setBackgroundResource(R.drawable.ds_seg_bg)
        val pad = (2 * density).toInt()
        setPadding(pad, pad, pad, pad)
    }

    /** Replaces the options and selects [selected] without notifying. */
    fun setOptions(options: List<Option>, selected: Int) {
        removeAllViews()
        segments.clear()
        options.forEachIndexed { index, option ->
            val segment = buildSegment(option)
            segment.setOnClickListener {
                if (index != selectedIndex) {
                    select(index)
                    onSelected?.invoke(index)
                }
            }
            val lp = LayoutParams(0, LayoutParams.MATCH_PARENT, 1f)
            if (index > 0) lp.marginStart = (2 * density).toInt()
            addView(segment, lp)
            segments.add(segment)
        }
        selectedIndex = -1
        select(selected)
    }

    /** Moves the selection pill to [index] without notifying. */
    fun select(index: Int) {
        selectedIndex = index
        segments.forEachIndexed { i, segment ->
            val on = i == index
            segment.isSelected = on
            val color = if (on) LinkLine.INK else LinkLine.TEXT_2
            (segment.getChildAt(segment.childCount - 1) as? TextView)?.apply {
                setTextColor(color)
                typeface = LinkLine.sans(context, if (on) 500 else 400)
            }
            (segment.getChildAt(0) as? ImageView)?.imageTintList = ColorStateList.valueOf(color)
        }
    }

    private fun buildSegment(option: Option): LinearLayout {
        val segment = LinearLayout(context).apply {
            orientation = HORIZONTAL
            gravity = Gravity.CENTER
            setBackgroundResource(R.drawable.ds_seg_option)
            isClickable = true
            isFocusable = true
            contentDescription = option.label
            val padH = (6 * density).toInt()
            setPadding(padH, 0, padH, 0)
        }
        option.iconRes?.let { res ->
            val size = (14 * density).toInt()
            segment.addView(ImageView(context).apply {
                setImageResource(res)
                importantForAccessibility = IMPORTANT_FOR_ACCESSIBILITY_NO
            }, LayoutParams(size, size).apply { marginEnd = (5 * density).toInt() })
        }
        segment.addView(TextView(context).apply {
            text = option.label
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
            maxLines = 1
            includeFontPadding = false
        })
        return segment
    }
}
