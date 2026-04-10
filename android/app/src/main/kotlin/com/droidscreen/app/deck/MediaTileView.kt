package com.droidscreen.app.deck

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.graphics.Typeface
import android.text.TextUtils
import android.view.MotionEvent
import android.view.View

/**
 * Horizontal media bar for the compact Stream Deck panel.
 * Layout: [AlbumArt 40x40] [Title / Artist] [<<] [>||] [>>]
 * With a thin 2dp progress bar running along the bottom.
 * No glass background — sits inside the panel which provides that.
 */
class MediaTileView(
    context: Context,
    private val tileConfig: TileConfig.MediaTile,
    private val onAction: (actionType: Int, tileId: String) -> Unit
) : View(context) {

    // -- Metrics ----------------------------------------------------------

    private val density = resources.displayMetrics.density
    private val sp = resources.displayMetrics.scaledDensity
    private val artSize = ART_SIZE_DP * density
    private val artCorner = ART_CORNER_DP * density
    private val btnSize = BTN_SIZE_DP * density
    private val innerPad = INNER_PAD_DP * density
    private val progressHeight = PROGRESS_HEIGHT_DP * density
    private val tileCorner = TILE_CORNER_DP * density

    // -- State ------------------------------------------------------------

    private var playing = false
    private var title = ""
    private var artist = ""
    private var albumArt: Bitmap? = null
    private var progress = 0f // 0..1

    // -- Paints -----------------------------------------------------------

    private val tileBgPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(0x7A, 0x08, 0x09, 0x12)
        style = Paint.Style.FILL
    }

    private val tileBorderPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(0x30, 0xFF, 0xFF, 0xFF)
        style = Paint.Style.STROKE
        strokeWidth = 0.5f * density
    }

    private val separatorPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(0x28, 0xFF, 0xFF, 0xFF)
        style = Paint.Style.STROKE
        strokeWidth = 0.5f * density
    }

    private val artPaint = Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG)

    private val artPlaceholderPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(0x20, 0xFF, 0xFF, 0xFF)
        style = Paint.Style.FILL
    }

    private val titlePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(0xEE, 0xFF, 0xFF, 0xFF)
        textSize = TITLE_SP * sp
        typeface = Typeface.create(Typeface.DEFAULT, Typeface.BOLD)
        letterSpacing = 0.01f
    }

    private val artistPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.parseColor("#AAAAAA")
        textSize = ARTIST_SP * sp
        typeface = Typeface.create(Typeface.DEFAULT, Typeface.NORMAL)
    }

    private val progressBgPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(0x18, 0xFF, 0xFF, 0xFF)
        style = Paint.Style.FILL
    }

    private val progressFillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.parseColor("#8B5CF6")
        style = Paint.Style.FILL
    }

    private val btnPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(0xDD, 0xFF, 0xFF, 0xFF)
        style = Paint.Style.FILL
    }

    private val btnDimPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(0x88, 0xFF, 0xFF, 0xFF)
        style = Paint.Style.FILL
    }

    // -- Hit test regions (computed in onDraw) ----------------------------

    private var prevBtnCx = 0f
    private var playBtnCx = 0f
    private var nextBtnCx = 0f
    private var btnCy = 0f

    init {
        isClickable = true
        isFocusable = true
    }

    // ------------------------------------------------------------------
    // Draw
    // ------------------------------------------------------------------

    override fun onDraw(canvas: Canvas) {
        if (isInEditMode) {
            canvas.drawColor(Color.DKGRAY)
            return
        }

        val w = width.toFloat()
        val h = height.toFloat()
        val bgRect = RectF(0f, 0f, w, h)

        canvas.drawRoundRect(bgRect, tileCorner, tileCorner, tileBgPaint)
        canvas.drawRoundRect(bgRect, tileCorner, tileCorner, tileBorderPaint)

        // 0. Top separator line
        canvas.drawLine(innerPad, 0f, w - innerPad, 0f, separatorPaint)

        // Content area (vertically centered, leaving room for progress bar at bottom)
        val contentH = h - progressHeight
        val cy = contentH / 2f

        // 1. Album art (left side, centered vertically)
        val artLeft = innerPad
        val artTop = cy - artSize / 2f
        val artRect = RectF(artLeft, artTop, artLeft + artSize, artTop + artSize)

        val art = albumArt
        if (art != null) {
            canvas.save()
            val clipPath = Path()
            clipPath.addRoundRect(artRect, artCorner, artCorner, Path.Direction.CW)
            canvas.clipPath(clipPath)
            canvas.drawBitmap(art, null, artRect, artPaint)
            canvas.restore()
        } else {
            canvas.drawRoundRect(artRect, artCorner, artCorner, artPlaceholderPaint)
            drawMusicNote(canvas, artRect)
        }

        // 2. Playback buttons (right side): prev, play/pause, next
        val btnSpacing = btnSize * 1.2f
        val buttonsRight = w - innerPad
        nextBtnCx = buttonsRight - btnSize / 2f
        playBtnCx = nextBtnCx - btnSpacing
        prevBtnCx = playBtnCx - btnSpacing
        btnCy = cy

        drawPrevIcon(canvas, prevBtnCx, btnCy, btnSize * 0.35f)
        if (playing) {
            drawPauseIcon(canvas, playBtnCx, btnCy, btnSize * 0.4f)
        } else {
            drawPlayIcon(canvas, playBtnCx, btnCy, btnSize * 0.4f)
        }
        drawNextIcon(canvas, nextBtnCx, btnCy, btnSize * 0.35f)

        // 3. Text area (between art and buttons)
        val textLeft = artRect.right + innerPad
        val textRight = prevBtnCx - btnSize / 2f - innerPad * 0.5f
        val textMaxWidth = textRight - textLeft

        if (textMaxWidth > 0f) {
            val titleStr = title.ifBlank { "No media" }
            val artistStr = artist.ifBlank { "---" }

            val titleY = cy - 1f * density
            val ellipsizedTitle = ellipsize(titleStr, titlePaint, textMaxWidth)
            canvas.drawText(ellipsizedTitle, textLeft, titleY, titlePaint)

            val artistY = titleY + ARTIST_SP * sp * 1.5f
            val ellipsizedArtist = ellipsize(artistStr, artistPaint, textMaxWidth)
            canvas.drawText(ellipsizedArtist, textLeft, artistY, artistPaint)
        }

        // 4. Progress bar (thin strip at the very bottom)
        val barTop = h - progressHeight
        val barRect = RectF(0f, barTop, w, h)
        val barRadius = progressHeight / 2f
        canvas.drawRoundRect(barRect, barRadius, barRadius, progressBgPaint)
        val filledRight = w * progress.coerceIn(0f, 1f)
        if (filledRight > 1f) {
            canvas.drawRoundRect(
                RectF(0f, barTop, filledRight, h),
                barRadius, barRadius, progressFillPaint
            )
        }
    }

    // ------------------------------------------------------------------
    // Icon drawing helpers
    // ------------------------------------------------------------------

    private fun drawPlayIcon(canvas: Canvas, cx: Float, cy: Float, size: Float) {
        val path = Path()
        path.moveTo(cx - size * 0.35f, cy - size * 0.5f)
        path.lineTo(cx + size * 0.5f, cy)
        path.lineTo(cx - size * 0.35f, cy + size * 0.5f)
        path.close()
        canvas.drawPath(path, btnPaint)
    }

    private fun drawPauseIcon(canvas: Canvas, cx: Float, cy: Float, size: Float) {
        val barW = size * 0.26f
        val halfGap = size * 0.1f
        val halfH = size * 0.45f
        val r = barW * 0.25f
        canvas.drawRoundRect(
            cx - halfGap - barW, cy - halfH,
            cx - halfGap, cy + halfH,
            r, r, btnPaint
        )
        canvas.drawRoundRect(
            cx + halfGap, cy - halfH,
            cx + halfGap + barW, cy + halfH,
            r, r, btnPaint
        )
    }

    private fun drawPrevIcon(canvas: Canvas, cx: Float, cy: Float, size: Float) {
        // Two left-pointing triangles (<<)
        val path = Path()
        // First triangle
        path.moveTo(cx + size * 0.1f, cy - size * 0.4f)
        path.lineTo(cx - size * 0.35f, cy)
        path.lineTo(cx + size * 0.1f, cy + size * 0.4f)
        path.close()
        // Second triangle
        path.moveTo(cx + size * 0.5f, cy - size * 0.4f)
        path.lineTo(cx + size * 0.05f, cy)
        path.lineTo(cx + size * 0.5f, cy + size * 0.4f)
        path.close()
        canvas.drawPath(path, btnDimPaint)
    }

    private fun drawNextIcon(canvas: Canvas, cx: Float, cy: Float, size: Float) {
        // Two right-pointing triangles (>>)
        val path = Path()
        path.moveTo(cx - size * 0.5f, cy - size * 0.4f)
        path.lineTo(cx - size * 0.05f, cy)
        path.lineTo(cx - size * 0.5f, cy + size * 0.4f)
        path.close()
        path.moveTo(cx - size * 0.1f, cy - size * 0.4f)
        path.lineTo(cx + size * 0.35f, cy)
        path.lineTo(cx - size * 0.1f, cy + size * 0.4f)
        path.close()
        canvas.drawPath(path, btnDimPaint)
    }

    private fun drawMusicNote(canvas: Canvas, rect: RectF) {
        val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.argb(0x40, 0xFF, 0xFF, 0xFF)
            style = Paint.Style.FILL
        }
        val cx = rect.centerX()
        val cy = rect.centerY()
        val r = rect.width() * 0.1f
        canvas.drawCircle(cx - r * 0.5f, cy + r * 1.2f, r, paint)
        val stemPaint = Paint(paint).apply {
            style = Paint.Style.STROKE
            strokeWidth = 1f * density
        }
        canvas.drawLine(
            cx - r * 0.5f + r, cy + r * 1.2f,
            cx - r * 0.5f + r, cy - r * 1.5f,
            stemPaint
        )
        canvas.drawLine(
            cx - r * 0.5f + r, cy - r * 1.5f,
            cx + r * 1.2f, cy - r * 0.5f,
            stemPaint
        )
    }

    private fun ellipsize(text: String, paint: Paint, maxWidth: Float): String {
        if (maxWidth <= 0f) return ""
        return TextUtils.ellipsize(
            text, android.text.TextPaint(paint), maxWidth,
            TextUtils.TruncateAt.END
        ).toString()
    }

    // ------------------------------------------------------------------
    // Touch — hit-test for prev / play-pause / next buttons
    // ------------------------------------------------------------------

    @Suppress("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                parent?.requestDisallowInterceptTouchEvent(true)
                return true
            }
            MotionEvent.ACTION_UP -> {
                val x = event.x
                val y = event.y
                if (y < 0 || y > height) return true
                val hitRadius = btnSize * 0.7f

                when {
                    distSq(x, y, prevBtnCx, btnCy) < hitRadius * hitRadius -> {
                        onAction(ACTION_PREV, tileConfig.id)
                    }
                    distSq(x, y, playBtnCx, btnCy) < hitRadius * hitRadius -> {
                        playing = !playing
                        invalidate()
                        onAction(ACTION_PLAY_PAUSE, tileConfig.id)
                    }
                    distSq(x, y, nextBtnCx, btnCy) < hitRadius * hitRadius -> {
                        onAction(ACTION_NEXT, tileConfig.id)
                    }
                }
                return true
            }
        }
        return true
    }

    private fun distSq(x1: Float, y1: Float, x2: Float, y2: Float): Float {
        val dx = x1 - x2
        val dy = y1 - y2
        return dx * dx + dy * dy
    }

    // ------------------------------------------------------------------
    // State update
    // ------------------------------------------------------------------

    fun updateState(state: MediaState) {
        playing = state.playing
        title = state.title
        artist = state.artist
        albumArt = state.albumArtBitmap
        progress = state.progress
        invalidate()
    }

    // ------------------------------------------------------------------
    // Constants
    // ------------------------------------------------------------------

    companion object {
        const val ACTION_PLAY_PAUSE = 1
        const val ACTION_PREV = 2
        const val ACTION_NEXT = 3

        private const val ART_SIZE_DP = 52f
        private const val ART_CORNER_DP = 8f
        private const val INNER_PAD_DP = 14f
        private const val TITLE_SP = 15f
        private const val ARTIST_SP = 12f
        private const val PROGRESS_HEIGHT_DP = 3f
        private const val BTN_SIZE_DP = 34f
        private const val TILE_CORNER_DP = 18f
    }
}
