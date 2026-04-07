package com.droidscreen.app.deck

import android.animation.AnimatorSet
import android.animation.ObjectAnimator
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Shader
import android.view.MotionEvent
import android.view.View
import android.view.animation.AccelerateInterpolator
import android.view.animation.DecelerateInterpolator
import android.widget.FrameLayout

/**
 * Root overlay view for the Stream Deck. Draws a semi-transparent scrim over
 * the entire screen and lays out a **compact floating panel** in the center
 * containing an app-icon grid, media bar, and volume slider — like a real
 * Stream Deck control panel.
 */
class DeckOverlayView(context: Context) : FrameLayout(context) {

    var onDeckAction: ((actionType: Int, slotIndex: Int) -> Unit)? = null
    var onVolumeChange: ((volume: Int, muted: Boolean) -> Unit)? = null
    var onDismiss: (() -> Unit)? = null

    private val density = resources.displayMetrics.density

    // -- Panel metrics (dp -> px) -----------------------------------------

    private val tileSizePx = (TILE_SIZE_DP * density).toInt()
    private val tileHeightPx = (TILE_HEIGHT_DP * density).toInt()
    private val tileGapPx = (TILE_GAP_DP * density).toInt()
    private val panelPadPx = (PANEL_PAD_DP * density).toInt()
    private val panelCorner = PANEL_CORNER_DP * density
    private val mediaBarHeightPx = (MEDIA_BAR_HEIGHT_DP * density).toInt()
    private val volumeBarHeightPx = (VOLUME_BAR_HEIGHT_DP * density).toInt()
    private val sectionGapPx = (SECTION_GAP_DP * density).toInt()
    private val maxPanelWidthPx = (MAX_PANEL_WIDTH_DP * density).toInt()

    // -- Paints -----------------------------------------------------------

    private val scrimPaint = Paint().apply {
        color = Color.argb(0x70, 0x00, 0x00, 0x10)
        style = Paint.Style.FILL
    }

    private val panelBgPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(0x9A, 0x08, 0x09, 0x12)
        style = Paint.Style.FILL
    }

    private val panelBorderPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 1f * density
        color = Color.argb(0x52, 0xFF, 0xFF, 0xFF)
    }

    private val panelInnerGlowPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 0.5f * density
        color = Color.argb(0x18, 0xFF, 0xFF, 0xFF)
    }

    // -- State ------------------------------------------------------------

    private var config: DeckConfig? = null
    private val tileViews = mutableListOf<View>()
    private val panelRect = RectF()
    private val panelInnerRect = RectF()

    // Two-box layout: separate apps panel and media panel
    private val appsPanelRect = RectF()
    private val appsPanelInnerRect = RectF()
    private val mediaPanelRect = RectF()
    private val mediaPanelInnerRect = RectF()
    private val panelBoxGapPx = (PANEL_BOX_GAP_DP * density).toInt()

    // Categorized views (filled in buildFromConfig)
    private val appTileViews = mutableListOf<View>()
    private var mediaTileView: MediaTileView? = null
    private var volumeTileView: VolumeTileView? = null

    init {
        setWillNotDraw(false)
        isClickable = true
        isFocusable = true
        // Allow drawing beyond view bounds (for scrim covering system bar gaps)
        clipToPadding = false
        clipChildren = false
    }

    // ------------------------------------------------------------------
    // Drawing — scrim + panel background
    // ------------------------------------------------------------------

    override fun onDraw(canvas: Canvas) {
        // Full-screen scrim — simple overdraw to cover any system bar gaps
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), scrimPaint)

        val innerCorner = panelCorner - 1f * density

        // Apps panel background (Liquid Glass)
        if (!appsPanelRect.isEmpty) {
            canvas.drawRoundRect(appsPanelRect, panelCorner, panelCorner, panelBgPaint)
            canvas.drawRoundRect(appsPanelRect, panelCorner, panelCorner, panelBorderPaint)
            canvas.drawRoundRect(appsPanelInnerRect, innerCorner, innerCorner, panelInnerGlowPaint)
        }

        // Media + Volume panel background (Liquid Glass)
        if (!mediaPanelRect.isEmpty) {
            canvas.drawRoundRect(mediaPanelRect, panelCorner, panelCorner, panelBgPaint)
            canvas.drawRoundRect(mediaPanelRect, panelCorner, panelCorner, panelBorderPaint)
            canvas.drawRoundRect(mediaPanelInnerRect, innerCorner, innerCorner, panelInnerGlowPaint)
        }
    }

    // ------------------------------------------------------------------
    // Configuration
    // ------------------------------------------------------------------

    fun buildFromConfig(config: DeckConfig) {
        this.config = config
        tileViews.forEach { removeView(it) }
        tileViews.clear()
        appTileViews.clear()
        mediaTileView = null
        volumeTileView = null

        for (tile in config.tiles) {
            val view: View = when (tile) {
                is TileConfig.AppTile -> {
                    val v = DeckTileView(context, tile) { actionType, _ ->
                        onDeckAction?.invoke(actionType, tile.slotIndex)
                    }
                    appTileViews.add(v)
                    v
                }
                is TileConfig.MediaTile -> {
                    val v = MediaTileView(context, tile) { actionType, _ ->
                        onDeckAction?.invoke(actionType, tile.slotIndex)
                    }
                    mediaTileView = v
                    v
                }
                is TileConfig.VolumeTile -> {
                    val v = VolumeTileView(context, tile) { vol, muted ->
                        onVolumeChange?.invoke(vol, muted)
                    }
                    volumeTileView = v
                    v
                }
            }
            tileViews.add(view)
            addView(view)
        }
        requestLayout()
    }

    // ------------------------------------------------------------------
    // Layout — compact floating panel
    // ------------------------------------------------------------------

    override fun onLayout(changed: Boolean, l: Int, t: Int, r: Int, b: Int) {
        val cfg = config ?: return
        if (tileViews.isEmpty()) return

        val totalW = r - l
        val totalH = b - t

        // --- Determine panel width ---
        val cols = cfg.gridCols.coerceAtLeast(1)
        // Panel width: fit the icon grid + padding, capped against the screen so the deck
        // reads larger without clipping on smaller devices.
        val gridContentW = cols * tileSizePx + (cols - 1) * tileGapPx
        val desiredPanelW = gridContentW + 2 * panelPadPx
        val panelW = desiredPanelW.coerceAtMost(
            (totalW * PANEL_WIDTH_FRACTION).toInt().coerceAtMost(maxPanelWidthPx)
        )

        // Inner content width
        val contentW = panelW - 2 * panelPadPx

        // --- Calculate icon grid rows ---
        val appCount = appTileViews.size
        val iconGridRows = if (appCount == 0) 0 else ((appCount + cols - 1) / cols)
        val iconGridH = if (iconGridRows > 0) {
            iconGridRows * tileHeightPx + (iconGridRows - 1) * tileGapPx
        } else 0

        // --- Calculate apps panel height ---
        val hasApps = iconGridH > 0
        val appsPanelH = if (hasApps) iconGridH + 2 * panelPadPx else 0

        // --- Calculate media panel height ---
        val hasMedia = mediaTileView != null || volumeTileView != null
        var mediaContentH = 0
        if (mediaTileView != null) {
            mediaContentH += mediaBarHeightPx
        }
        if (volumeTileView != null) {
            if (mediaContentH > 0) mediaContentH += sectionGapPx
            mediaContentH += volumeBarHeightPx
        }
        val mediaPanelH = if (hasMedia) mediaContentH + 2 * panelPadPx else 0

        // --- Total height of both boxes + gap ---
        val totalPanelH = appsPanelH +
            (if (hasApps && hasMedia) panelBoxGapPx else 0) +
            mediaPanelH

        // --- Center both boxes vertically as a group ---
        // Order: media/volume panel on top, apps panel below
        val groupTop = (totalH - totalPanelH) / 2
        val panelLeft = (totalW - panelW) / 2

        // --- Set media panel rect (TOP) ---
        mediaPanelRect.setEmpty()
        mediaPanelInnerRect.setEmpty()
        if (hasMedia) {
            mediaPanelRect.set(
                panelLeft.toFloat(), groupTop.toFloat(),
                (panelLeft + panelW).toFloat(), (groupTop + mediaPanelH).toFloat()
            )
            val inset = 1.5f * density
            mediaPanelInnerRect.set(
                mediaPanelRect.left + inset, mediaPanelRect.top + inset,
                mediaPanelRect.right - inset, mediaPanelRect.bottom - inset
            )
        }

        // --- Set apps panel rect (BOTTOM) ---
        appsPanelRect.setEmpty()
        appsPanelInnerRect.setEmpty()
        if (hasApps) {
            val appsTop = groupTop + mediaPanelH + (if (hasMedia) panelBoxGapPx else 0)
            appsPanelRect.set(
                panelLeft.toFloat(), appsTop.toFloat(),
                (panelLeft + panelW).toFloat(), (appsTop + appsPanelH).toFloat()
            )
            val inset = 1.5f * density
            appsPanelInnerRect.set(
                appsPanelRect.left + inset, appsPanelRect.top + inset,
                appsPanelRect.right - inset, appsPanelRect.bottom - inset
            )
        }

        // Update legacy panelRect to union of both for hit-testing
        panelRect.setEmpty()
        if (hasApps) panelRect.union(appsPanelRect)
        if (hasMedia) panelRect.union(mediaPanelRect)

        // Rebuild border gradient for current panel size
        panelBorderPaint.shader = LinearGradient(
            panelRect.left, panelRect.top,
            panelRect.right, panelRect.bottom,
            Color.argb(0x35, 0xFF, 0xFF, 0xFF),
            Color.argb(0x14, 0xFF, 0xFF, 0xFF),
            Shader.TileMode.CLAMP
        )

        // --- Layout children within the panels ---

        // 1. Media bar + Volume bar (inside mediaPanelRect, TOP)
        if (hasMedia) {
            val cLeft = panelLeft + panelPadPx
            val cRight = panelLeft + panelPadPx + contentW
            var cursorY = mediaPanelRect.top.toInt() + panelPadPx

            mediaTileView?.let {
                it.measure(
                    MeasureSpec.makeMeasureSpec(cRight - cLeft, MeasureSpec.EXACTLY),
                    MeasureSpec.makeMeasureSpec(mediaBarHeightPx, MeasureSpec.EXACTLY)
                )
                it.layout(cLeft, cursorY, cRight, cursorY + mediaBarHeightPx)
                cursorY += mediaBarHeightPx
            }
            volumeTileView?.let {
                if (mediaTileView != null) cursorY += sectionGapPx
                it.measure(
                    MeasureSpec.makeMeasureSpec(cRight - cLeft, MeasureSpec.EXACTLY),
                    MeasureSpec.makeMeasureSpec(volumeBarHeightPx, MeasureSpec.EXACTLY)
                )
                it.layout(cLeft, cursorY, cRight, cursorY + volumeBarHeightPx)
            }
        }

        // 2. App icon grid (inside appsPanelRect, BOTTOM)
        if (appCount > 0) {
            val appsContentLeft = panelLeft + panelPadPx
            val appsCursorY = appsPanelRect.top.toInt() + panelPadPx

            // Center the grid horizontally within content area
            val actualGridW = cols.coerceAtMost(appCount) * tileSizePx +
                    (cols.coerceAtMost(appCount) - 1).coerceAtLeast(0) * tileGapPx
            val gridOffsetX = appsContentLeft + (contentW - actualGridW) / 2

            for (i in appTileViews.indices) {
                val view = appTileViews[i]
                val col = i % cols
                val row = i / cols
                val tileLeft = gridOffsetX + col * (tileSizePx + tileGapPx)
                val tileTop = appsCursorY + row * (tileHeightPx + tileGapPx)
                val tileRight = tileLeft + tileSizePx
                val tileBottom = tileTop + tileHeightPx

                view.measure(
                    MeasureSpec.makeMeasureSpec(tileRight - tileLeft, MeasureSpec.EXACTLY),
                    MeasureSpec.makeMeasureSpec(tileBottom - tileTop, MeasureSpec.EXACTLY)
                )
                view.layout(tileLeft, tileTop, tileRight, tileBottom)
            }
        }

        // (media + volume already laid out above in section 1)

        invalidate() // repaint panel backgrounds
    }

    // ------------------------------------------------------------------
    // Touch — dismiss on scrim tap, let tiles handle their own events
    // ------------------------------------------------------------------

    override fun onInterceptTouchEvent(ev: MotionEvent): Boolean {
        return false
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (event.actionMasked == MotionEvent.ACTION_UP) {
            // If tap is outside the panel, dismiss
            val x = event.x
            val y = event.y
            if (!appsPanelRect.contains(x, y) && !mediaPanelRect.contains(x, y)) {
                onDismiss?.invoke()
                return true
            }
        }
        return super.onTouchEvent(event)
    }

    // ------------------------------------------------------------------
    // Animations
    // ------------------------------------------------------------------

    fun animateIn() {
        alpha = 0f
        scaleX = 0.92f
        scaleY = 0.92f
        visibility = View.VISIBLE

        val fadeIn = ObjectAnimator.ofFloat(this, "alpha", 0f, 1f)
        val scaleXIn = ObjectAnimator.ofFloat(this, "scaleX", 0.92f, 1f)
        val scaleYIn = ObjectAnimator.ofFloat(this, "scaleY", 0.92f, 1f)

        AnimatorSet().apply {
            playTogether(fadeIn, scaleXIn, scaleYIn)
            duration = ANIM_IN_MS
            interpolator = DecelerateInterpolator()
            start()
        }
    }

    fun animateOut(onEnd: () -> Unit) {
        val fadeOut = ObjectAnimator.ofFloat(this, "alpha", 1f, 0f)
        val scaleXOut = ObjectAnimator.ofFloat(this, "scaleX", 1f, 0.92f)
        val scaleYOut = ObjectAnimator.ofFloat(this, "scaleY", 1f, 0.92f)

        AnimatorSet().apply {
            playTogether(fadeOut, scaleXOut, scaleYOut)
            duration = ANIM_OUT_MS
            interpolator = AccelerateInterpolator()
            addListener(object : android.animation.AnimatorListenerAdapter() {
                override fun onAnimationEnd(animation: android.animation.Animator) {
                    visibility = View.GONE
                    onEnd()
                }
            })
            start()
        }
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

    // ------------------------------------------------------------------
    // Constants
    // ------------------------------------------------------------------

    private companion object {
        const val TILE_SIZE_DP = 96f
        const val TILE_HEIGHT_DP = 112f
        const val TILE_GAP_DP = 14f
        const val PANEL_PAD_DP = 24f
        const val PANEL_CORNER_DP = 28f
        const val PANEL_BOX_GAP_DP = 16f
        const val MEDIA_BAR_HEIGHT_DP = 72f
        const val VOLUME_BAR_HEIGHT_DP = 48f
        const val SECTION_GAP_DP = 10f
        const val MAX_PANEL_WIDTH_DP = 1200f
        const val PANEL_WIDTH_FRACTION = 0.96f
        const val ANIM_IN_MS = 200L
        const val ANIM_OUT_MS = 150L
    }
}
