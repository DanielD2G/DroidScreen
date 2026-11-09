package com.droidscreen.app.deck

import android.content.Context
import android.content.res.ColorStateList
import android.view.LayoutInflater
import android.view.MotionEvent
import android.view.View
import android.view.animation.AccelerateInterpolator
import android.view.animation.DecelerateInterpolator
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import com.droidscreen.app.R
import com.droidscreen.app.ui.LinkLine
import com.droidscreen.app.ui.LinkMarkView

/**
 * Root overlay view for the Stream Deck, in the Link Line style: an opaque
 * rail docked to the Mac-facing (left) edge, at most 30% of the screen, with
 * no scrim and no blur. It holds the app-shortcut grid, the media card, the
 * volume row and the quick controls (input modes, Lock, Hide). Tapping the
 * stream outside the rail dismisses it.
 */
class DeckOverlayView(context: Context) : FrameLayout(context) {

    var onDeckAction: ((actionType: Int, slotIndex: Int) -> Unit)? = null
    var onVolumeChange: ((volume: Int, muted: Boolean) -> Unit)? = null
    var onDismiss: (() -> Unit)? = null

    /** Any touch inside the overlay (used to restart the auto-hide timer). */
    var onInteraction: (() -> Unit)? = null
    /** A finger went down on the Deck: hold auto-hide until it lifts. */
    var onGestureStart: (() -> Unit)? = null
    var onFingerModeSelected: ((Int) -> Unit)? = null
    var onStylusModeSelected: ((Int) -> Unit)? = null
    var onUnknownModeSelected: ((Int) -> Unit)? = null
    /** Keys row: 0 = show the keyboard button, 1 = hide it. */
    var onKeysModeSelected: ((Int) -> Unit)? = null
    var onLockToggle: (() -> Unit)? = null
    var onHide: (() -> Unit)? = null
    var onStatsChipClick: (() -> Unit)? = null

    private val density = resources.displayMetrics.density
    private val edgeOffsetPx = (EDGE_DP * density).toInt()
    private val tileHeightPx = (TILE_HEIGHT_DP * density).toInt()
    private val tileGapPx = (TILE_GAP_DP * density).toInt()

    private val rail: View = LayoutInflater.from(context).inflate(R.layout.deck_rail, this, false)
    private val appsLabel: View = rail.findViewById(R.id.deck_apps_label)
    private val appsGrid: LinearLayout = rail.findViewById(R.id.deck_apps_grid)
    private val mediaLabel: View = rail.findViewById(R.id.deck_media_label)
    private val mediaContainer: FrameLayout = rail.findViewById(R.id.deck_media_container)
    private val volumeLabel: View = rail.findViewById(R.id.deck_volume_label)
    private val volumeContainer: FrameLayout = rail.findViewById(R.id.deck_volume_container)
    private val mark: LinkMarkView = rail.findViewById(R.id.deck_mark)
    private val statsText: TextView = rail.findViewById(R.id.deck_stats_text)
    private val statsTransport: TextView = rail.findViewById(R.id.deck_stats_transport)
    private val statsTransportIcon: ImageView = rail.findViewById(R.id.deck_stats_transport_icon)
    private val statsChip: View = rail.findViewById(R.id.deck_stats_chip)
    private val fingerMode: SegmentedControlView = rail.findViewById(R.id.deck_finger_mode)
    private val stylusMode: SegmentedControlView = rail.findViewById(R.id.deck_stylus_mode)
    private val unknownMode: SegmentedControlView = rail.findViewById(R.id.deck_unknown_mode)
    private val keysMode: SegmentedControlView = rail.findViewById(R.id.deck_keys_mode)
    private val lockButton: View = rail.findViewById(R.id.deck_lock)
    private val lockIcon: ImageView = rail.findViewById(R.id.deck_lock_icon)
    private val lockText: TextView = rail.findViewById(R.id.deck_lock_text)

    private var config: DeckConfig? = null
    private var mediaTileView: MediaTileView? = null
    private var volumeTileView: VolumeTileView? = null

    init {
        // The rail moves with translationX, which doesn't refresh the window's
        // transparent-region hint over the video SurfaceView. Without this the
        // video was composited over most of the rail until the next layout pass.
        setWillNotDraw(false)
        isClickable = true
        isFocusable = true
        addView(rail, LayoutParams(railWidthFor(resources.displayMetrics.widthPixels), LayoutParams.MATCH_PARENT).apply {
            marginStart = edgeOffsetPx
        })
        statsChip.setOnClickListener { onStatsChipClick?.invoke() }
        lockButton.setOnClickListener { onLockToggle?.invoke() }
        rail.findViewById<View>(R.id.deck_hide).setOnClickListener { onHide?.invoke() }
        fingerMode.onSelected = { onFingerModeSelected?.invoke(it) }
        stylusMode.onSelected = { onStylusModeSelected?.invoke(it) }
        unknownMode.onSelected = { onUnknownModeSelected?.invoke(it) }
        keysMode.onSelected = { onKeysModeSelected?.invoke(it) }
    }

    private fun railWidthFor(screenWidth: Int): Int =
        minOf((MAX_RAIL_WIDTH_DP * density).toInt(), (screenWidth * MAX_RAIL_FRACTION).toInt())

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val lp = rail.layoutParams
        val target = railWidthFor(MeasureSpec.getSize(widthMeasureSpec))
        if (target > 0 && lp.width != target) lp.width = target
        super.onMeasure(widthMeasureSpec, heightMeasureSpec)
    }

    // ------------------------------------------------------------------
    // Configuration
    // ------------------------------------------------------------------

    fun buildFromConfig(config: DeckConfig) {
        this.config = config
        appsGrid.removeAllViews()
        mediaContainer.removeAllViews()
        volumeContainer.removeAllViews()
        mediaTileView = null
        volumeTileView = null

        val appTiles = mutableListOf<View>()
        for (tile in config.tiles) {
            when (tile) {
                is TileConfig.AppTile -> appTiles.add(
                    DeckTileView(context, tile) { actionType, _ ->
                        onDeckAction?.invoke(actionType, tile.slotIndex)
                    }
                )
                is TileConfig.MediaTile -> {
                    val v = MediaTileView(context, tile) { actionType, _ ->
                        onDeckAction?.invoke(actionType, tile.slotIndex)
                    }
                    mediaTileView = v
                    mediaContainer.removeAllViews()
                    mediaContainer.addView(v)
                }
                is TileConfig.VolumeTile -> {
                    val v = VolumeTileView(context, tile) { vol, muted ->
                        onVolumeChange?.invoke(vol, muted)
                    }
                    volumeTileView = v
                    volumeContainer.removeAllViews()
                    volumeContainer.addView(v)
                }
            }
        }

        // App shortcuts: fixed 3-column grid; the last row keeps empty cells.
        appTiles.chunked(RAIL_COLUMNS).forEachIndexed { rowIndex, rowTiles ->
            val row = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
            for (col in 0 until RAIL_COLUMNS) {
                val cell = rowTiles.getOrNull(col) ?: View(context)
                val lp = LinearLayout.LayoutParams(0, tileHeightPx, 1f)
                if (col > 0) lp.marginStart = tileGapPx
                row.addView(cell, lp)
            }
            val rowLp = LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT)
            if (rowIndex > 0) rowLp.topMargin = tileGapPx
            appsGrid.addView(row, rowLp)
        }

        appsLabel.visibility = if (appTiles.isEmpty()) View.GONE else View.VISIBLE
        mediaLabel.visibility = if (mediaTileView == null) View.GONE else View.VISIBLE
        volumeLabel.visibility = if (volumeTileView == null) View.GONE else View.VISIBLE
        requestLayout()
    }

    /** Fills the input-mode segmented controls. */
    fun bindInputModes(
        finger: List<SegmentedControlView.Option>, fingerSelected: Int,
        stylus: List<SegmentedControlView.Option>, stylusSelected: Int,
        unknown: List<SegmentedControlView.Option>, unknownSelected: Int
    ) {
        fingerMode.setOptions(finger, fingerSelected)
        stylusMode.setOptions(stylus, stylusSelected)
        unknownMode.setOptions(unknown, unknownSelected)
    }

    /** Keys row state: whether the floating keyboard button is shown. */
    fun bindKeyboardButton(shown: Boolean) {
        keysMode.setOptions(
            listOf(
                SegmentedControlView.Option("Button", R.drawable.ic_ph_keyboard),
                SegmentedControlView.Option("Off")
            ),
            if (shown) 0 else 1
        )
    }

    /** Shows the Lock button as pressed (yellow) while the Deck is pinned open. */
    fun setLocked(locked: Boolean) {
        lockButton.isActivated = locked
        val color = if (locked) LinkLine.INK else LinkLine.TEXT
        lockText.text = if (locked) "Locked" else "Lock"
        lockText.setTextColor(color)
        lockIcon.imageTintList = ColorStateList.valueOf(color)
        lockButton.contentDescription = if (locked) "Unlock Deck" else "Lock Deck open"
    }

    /** Updates the header stats chip, e.g. "30 ms · 57 fps ·" + transport. */
    fun setStats(summary: String, wifi: Boolean) {
        statsText.text = summary
        statsTransport.text = if (wifi) "Wi-Fi" else "USB"
        statsTransportIcon.setImageResource(if (wifi) R.drawable.ic_ph_wifi_high else R.drawable.ic_ph_usb)
        mark.wifi = wifi
    }

    /** Highlights the stats chip while the detailed stats panel is open. */
    fun setStatsExpanded(expanded: Boolean) {
        statsChip.isActivated = expanded
    }

    // ------------------------------------------------------------------
    // Touch — dismiss on taps outside the rail
    // ------------------------------------------------------------------

    override fun dispatchTouchEvent(ev: MotionEvent): Boolean {
        when (ev.actionMasked) {
            MotionEvent.ACTION_DOWN -> onGestureStart?.invoke()
            // Restart the auto-hide countdown only once the gesture ends, so a
            // long scroll or press can't be dismissed under the finger.
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> onInteraction?.invoke()
        }
        return super.dispatchTouchEvent(ev)
    }

    override fun onInterceptTouchEvent(ev: MotionEvent): Boolean {
        return false
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (event.actionMasked == MotionEvent.ACTION_UP) {
            if (event.x > rail.right || event.x < rail.left) {
                onDismiss?.invoke()
                return true
            }
        }
        return super.onTouchEvent(event)
    }

    // ------------------------------------------------------------------
    // Animations — the rail slides out of the edge line
    // ------------------------------------------------------------------

    fun animateIn() {
        visibility = View.VISIBLE
        val offset = -(railWidthFor(resources.displayMetrics.widthPixels) + edgeOffsetPx).toFloat()
        rail.translationX = offset
        rail.animate()
            .translationX(0f)
            .setDuration(ANIM_IN_MS)
            .setInterpolator(DecelerateInterpolator(2f))
            .start()
    }

    fun animateOut(onEnd: () -> Unit) {
        val offset = -(rail.width + edgeOffsetPx + (8 * density).toInt()).toFloat()
        rail.animate()
            .translationX(offset)
            .setDuration(ANIM_OUT_MS)
            .setInterpolator(AccelerateInterpolator())
            .withEndAction {
                visibility = View.GONE
                onEnd()
            }
            .start()
    }

    // ------------------------------------------------------------------
    // State updates
    // ------------------------------------------------------------------

    fun updateMediaState(state: MediaState) {
        mediaTileView?.updateState(state)
    }

    fun updateVolume(volume: Int, muted: Boolean) {
        volumeTileView?.updateVolume(volume, muted)
    }

    private companion object {
        const val EDGE_DP = 4f
        const val MAX_RAIL_WIDTH_DP = 340f
        const val MAX_RAIL_FRACTION = 0.30f
        const val RAIL_COLUMNS = 3
        const val TILE_HEIGHT_DP = 84f
        const val TILE_GAP_DP = 8f
        const val ANIM_IN_MS = 300L
        const val ANIM_OUT_MS = 200L
    }
}
