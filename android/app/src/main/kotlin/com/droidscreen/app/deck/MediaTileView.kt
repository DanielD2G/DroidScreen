package com.droidscreen.app.deck

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.graphics.drawable.Drawable
import android.text.TextUtils
import android.view.MotionEvent
import android.view.View
import com.droidscreen.app.R
import com.droidscreen.app.ui.LinkLine

/**
 * "Now playing" card for the Deck rail (Link Line): ink card with a 1dp line
 * border, title and artist on one line, a thin progress bar with elapsed /
 * total time in mono, and prev / play-pause / next buttons (play-pause filled).
 * Album art, when the desktop sends it, sits to the left of the title.
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
    private val padH = PAD_H_DP * density
    private val padV = PAD_V_DP * density
    private val progressHeight = PROGRESS_HEIGHT_DP * density
    private val tileCorner = TILE_CORNER_DP * density

    // -- State ------------------------------------------------------------

    private var playing = false
    private var title = ""
    private var artist = ""
    private var albumArt: Bitmap? = null
    private var progress = 0f // 0..1
    private var durationSec = 0

    // -- Paints -----------------------------------------------------------

    private val tileBgPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = LinkLine.INK
        style = Paint.Style.FILL
    }

    private val tileBorderPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = LinkLine.LINE
        style = Paint.Style.STROKE
        strokeWidth = 1f * density
    }

    private val artPaint = Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG)

    private val titlePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = LinkLine.TEXT
        textSize = TITLE_SP * sp
        typeface = LinkLine.sans(context, 600)
    }

    private val artistPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = LinkLine.TEXT_2
        textSize = ARTIST_SP * sp
        typeface = LinkLine.sans(context)
    }

    private val timePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = LinkLine.TEXT_3
        textSize = TIME_SP * sp
        typeface = LinkLine.mono(context)
    }

    private val progressBgPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = LinkLine.LINE_2
        style = Paint.Style.FILL
    }

    private val progressFillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = LinkLine.TEXT
        style = Paint.Style.FILL
    }

    private val playBgPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = LinkLine.TEXT
        style = Paint.Style.FILL
    }

    private val prevIcon: Drawable? = context.getDrawable(R.drawable.ic_ph_skip_back_fill)?.mutate()
    private val nextIcon: Drawable? = context.getDrawable(R.drawable.ic_ph_skip_forward_fill)?.mutate()
    private val playIcon: Drawable? = context.getDrawable(R.drawable.ic_ph_play_fill)?.mutate()
    private val pauseIcon: Drawable? = context.getDrawable(R.drawable.ic_ph_pause_fill)?.mutate()

    // -- Hit test regions (computed in onDraw) ----------------------------

    private var prevBtnCx = 0f
    private var playBtnCx = 0f
    private var nextBtnCx = 0f
    private var btnCy = 0f

    init {
        isClickable = true
        isFocusable = true
    }

    private fun headerHeight(): Float =
        if (albumArt != null) artSize else titlePaint.fontSpacing

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val h = padV + headerHeight() + PROGRESS_TOP_DP * density + progressHeight +
            TIME_TOP_DP * density + timePaint.fontSpacing + BTN_TOP_DP * density + btnSize + padV
        setMeasuredDimension(
            MeasureSpec.getSize(widthMeasureSpec),
            resolveSize(h.toInt(), heightMeasureSpec)
        )
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
        val inset = tileBorderPaint.strokeWidth / 2f
        val bgRect = RectF(inset, inset, w - inset, h - inset)
        canvas.drawRoundRect(bgRect, tileCorner, tileCorner, tileBgPaint)
        canvas.drawRoundRect(bgRect, tileCorner, tileCorner, tileBorderPaint)

        // 1. Header: [art] title  artist
        var y = padV
        var textLeft = padH
        val art = albumArt
        val titleStr = title.ifBlank { "Nothing playing" }
        if (art != null) {
            val artRect = RectF(padH, y, padH + artSize, y + artSize)
            canvas.save()
            val clipPath = Path()
            clipPath.addRoundRect(artRect, artCorner, artCorner, Path.Direction.CW)
            canvas.clipPath(clipPath)
            canvas.drawBitmap(art, null, artRect, artPaint)
            canvas.restore()
            textLeft = artRect.right + 10f * density
            val maxW = w - padH - textLeft
            val titleBase = y + artSize / 2f - 2f * density
            canvas.drawText(ellipsize(titleStr, titlePaint, maxW), textLeft, titleBase, titlePaint)
            if (artist.isNotBlank()) {
                canvas.drawText(
                    ellipsize(artist, artistPaint, maxW), textLeft,
                    titleBase - artistPaint.ascent() + 2f * density, artistPaint
                )
            }
        } else {
            val baseline = y - titlePaint.ascent()
            val maxW = w - 2 * padH
            val titleText = ellipsize(titleStr, titlePaint, maxW)
            canvas.drawText(titleText, textLeft, baseline, titlePaint)
            val artistLeft = textLeft + titlePaint.measureText(titleText) + 8f * density
            val artistMax = w - padH - artistLeft
            if (artist.isNotBlank() && artistMax > 24f * density) {
                canvas.drawText(ellipsize(artist, artistPaint, artistMax), artistLeft, baseline, artistPaint)
            }
        }
        y += headerHeight() + PROGRESS_TOP_DP * density

        // 2. Progress bar
        val barRadius = progressHeight / 2f
        val barRect = RectF(padH, y, w - padH, y + progressHeight)
        canvas.drawRoundRect(barRect, barRadius, barRadius, progressBgPaint)
        val filledRight = barRect.left + barRect.width() * progress.coerceIn(0f, 1f)
        if (filledRight > barRect.left + 1f) {
            canvas.drawRoundRect(
                RectF(barRect.left, barRect.top, filledRight, barRect.bottom),
                barRadius, barRadius, progressFillPaint
            )
        }
        y += progressHeight + TIME_TOP_DP * density

        // 3. Elapsed / total
        val timeBase = y - timePaint.ascent()
        if (durationSec > 0) {
            val elapsed = (durationSec * progress.coerceIn(0f, 1f)).toInt()
            timePaint.textAlign = Paint.Align.LEFT
            canvas.drawText(formatTime(elapsed), padH, timeBase, timePaint)
            timePaint.textAlign = Paint.Align.RIGHT
            canvas.drawText(formatTime(durationSec), w - padH, timeBase, timePaint)
            timePaint.textAlign = Paint.Align.LEFT
        }
        y += timePaint.fontSpacing + BTN_TOP_DP * density

        // 4. Transport: prev, play/pause (filled), next
        btnCy = y + btnSize / 2f
        val spacing = btnSize + BTN_GAP_DP * density
        playBtnCx = w / 2f
        prevBtnCx = playBtnCx - spacing
        nextBtnCx = playBtnCx + spacing

        canvas.drawCircle(playBtnCx, btnCy, btnSize / 2f, playBgPaint)
        drawIcon(canvas, prevIcon, prevBtnCx, btnCy, LinkLine.TEXT)
        drawIcon(canvas, if (playing) pauseIcon else playIcon, playBtnCx, btnCy, LinkLine.INK)
        drawIcon(canvas, nextIcon, nextBtnCx, btnCy, LinkLine.TEXT)
    }

    private fun drawIcon(canvas: Canvas, icon: Drawable?, cx: Float, cy: Float, color: Int) {
        icon ?: return
        val half = ICON_DP * density / 2f
        icon.setTint(color)
        icon.setBounds((cx - half).toInt(), (cy - half).toInt(), (cx + half).toInt(), (cy + half).toInt())
        icon.draw(canvas)
    }

    private fun formatTime(totalSec: Int): String = "%d:%02d".format(totalSec / 60, totalSec % 60)

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
                // Don't block the rail's ScrollView: a vertical drag that starts
                // here should scroll (we then get CANCEL and do nothing).
                return true
            }
            MotionEvent.ACTION_UP -> {
                val x = event.x
                val y = event.y
                if (y < 0 || y > height) return true
                val hitRadius = btnSize * 0.6f

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
        val hadArt = albumArt != null
        albumArt = state.albumArtBitmap
        progress = state.progress
        durationSec = state.durationSec
        if (hadArt != (albumArt != null)) requestLayout()
        invalidate()
    }

    // ------------------------------------------------------------------
    // Constants
    // ------------------------------------------------------------------

    companion object {
        const val ACTION_PLAY_PAUSE = 1
        const val ACTION_PREV = 2
        const val ACTION_NEXT = 3

        private const val ART_SIZE_DP = 40f
        private const val ART_CORNER_DP = 6f
        private const val PAD_H_DP = 14f
        private const val PAD_V_DP = 12f
        private const val TITLE_SP = 15f
        private const val ARTIST_SP = 13f
        private const val TIME_SP = 11f
        private const val PROGRESS_TOP_DP = 10f
        private const val PROGRESS_HEIGHT_DP = 3f
        private const val TIME_TOP_DP = 5f
        private const val BTN_TOP_DP = 6f
        private const val BTN_SIZE_DP = 48f
        private const val BTN_GAP_DP = 14f
        private const val ICON_DP = 20f
        private const val TILE_CORNER_DP = 14f
    }
}
