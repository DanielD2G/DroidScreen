package com.droidscreen.app

import android.app.Activity
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.view.InputDevice
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.Surface
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.view.inputmethod.InputMethodManager
import android.widget.ImageButton
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import com.droidscreen.app.deck.DeckConfig
import com.droidscreen.app.deck.DeckEdgeView
import com.droidscreen.app.deck.DeckOverlayView
import com.droidscreen.app.deck.MediaState
import com.droidscreen.app.deck.SegmentedControlView
import com.droidscreen.app.input.KeyboardCaptureView
import com.droidscreen.app.ui.LinkHeroView
import com.droidscreen.app.ui.LinkLine
import com.droidscreen.app.ui.LinkMarkView
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

class MainActivity : Activity(), SurfaceHolder.Callback {

    companion object {
        private const val TAG = "DroidScreen"
        private const val DECK_AUTO_HIDE_MS = 6_000L
        // Must match DS_HELLO_APPROVAL_TIMEOUT_MS in protocol/include/droidscreen/hello.h
        private const val APPROVAL_TIMEOUT_MS = 60_000L
        // Distance from the approval card's top to where the hero cable crosses it.
        private const val CARD_CABLE_OFFSET_DP = 210f

        // Status constants — must match native_bridge.cpp
        const val STATUS_WAITING = 0
        const val STATUS_CONNECTED = 1
        const val STATUS_DISCONNECTED = 2
        const val STATUS_ERROR = 3

        init {
            System.loadLibrary("droidscreen_native")
        }
    }

    private lateinit var surfaceView: SurfaceView
    private lateinit var statusOverlay: FrameLayout
    private lateinit var statusText: TextView
    private lateinit var statusDetail: TextView
    private lateinit var statusDot: View
    private lateinit var statusMark: LinkMarkView
    private lateinit var statusDeviceName: TextView
    private lateinit var statusHintRow: View
    private lateinit var statusHint: TextView
    private lateinit var statusHero: LinkHeroView
    private lateinit var statusStep1: TextView
    private lateinit var statusStep2: TextView
    private lateinit var statusStep3: TextView
    private lateinit var statusVersion: TextView
    private lateinit var statusTransport: TextView
    private lateinit var approvalLayer: FrameLayout
    private lateinit var approvalCard: View
    private lateinit var approvalTitle: TextView
    private lateinit var approvalAllow: Button
    private lateinit var approvalDeny: Button
    private lateinit var approvalDotStart: View
    private lateinit var approvalDotEnd: View
    private lateinit var deckOverlayContainer: FrameLayout
    private lateinit var deckEdge: DeckEdgeView
    private lateinit var keyboardButton: ImageButton
    private lateinit var keyboardCapture: KeyboardCaptureView
    /** The soft keyboard is up (typing goes to the Mac). */
    private var keyboardOpen = false
    private var deckOverlay: DeckOverlayView? = null
    /** A finger is down on the Deck; auto-hide waits for it to lift. */
    private var deckGestureActive = false
    private var isDeckVisible = false
    private var isDeckLocked = false
    private var currentDeckConfig: DeckConfig = DeckConfig.createDefault()
    private var currentMediaState: MediaState? = null
    private var currentVolumeLevel = 0
    private var currentVolumeMuted = false
    private var nativeStarted = false
    private lateinit var inputSettings: InputSettings
    private lateinit var statsPanel: LinearLayout
    private lateinit var statsBitrate: TextView
    private lateinit var statsFps: TextView
    private lateinit var statsPacing: TextView
    private lateinit var statsStability: TextView
    private var statsVisible = false
    private var lastBytesReceived = 0L
    private var lastFramesDecoded = 0L
    private var lastStatsTime = 0L
    private var chipBytes = 0L
    private var chipFrames = 0L
    private var chipTime = 0L
    private var currentStatus = STATUS_WAITING
    /** Set by authorizeDesktop (Wi-Fi only) just before a connection starts. */
    @Volatile private var pendingWifiConnection = false
    private var streamOverWifi = false
    private val uiHandler = Handler(Looper.getMainLooper())
    private lateinit var wireless: WirelessManager

    /** A Wi-Fi desktop waiting for the user to allow or deny it. */
    private class PendingApproval(val name: String) {
        val latch = CountDownLatch(1)
        @Volatile var approved = false
        fun decide(allow: Boolean) {
            approved = allow
            latch.countDown()
        }
    }
    @Volatile private var pendingApproval: PendingApproval? = null
    /** The request whose prompt is on screen (UI thread only). */
    private var shownApproval: PendingApproval? = null
    private val statsUpdateRunnable = object : Runnable {
        override fun run() {
            if (statsVisible) {
                updateStats()
                uiHandler.postDelayed(this, 500)
            }
        }
    }
    private val chipStatsRunnable = object : Runnable {
        override fun run() {
            if (isDeckVisible) {
                updateChipStats()
                uiHandler.postDelayed(this, 1_000)
            }
        }
    }
    private val hideDeckRunnable = Runnable {
        if (isDeckVisible && !isDeckLocked) {
            hideDeckAndControls()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(false)
        }
        setContentView(R.layout.activity_main)

        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        surfaceView = findViewById(R.id.surface_view)
        statusOverlay = findViewById(R.id.status_overlay)
        statusText = findViewById(R.id.status_text)
        statusDetail = findViewById(R.id.status_detail)
        statusDot = findViewById(R.id.status_dot)
        statusMark = findViewById(R.id.status_mark)
        statusDeviceName = findViewById(R.id.status_device_name)
        statusHintRow = findViewById(R.id.status_hint_row)
        statusHint = findViewById(R.id.status_hint)
        statusHero = findViewById(R.id.status_hero)
        statusStep1 = findViewById(R.id.status_step_1)
        statusStep2 = findViewById(R.id.status_step_2)
        statusStep3 = findViewById(R.id.status_step_3)
        statusVersion = findViewById(R.id.status_version)
        statusTransport = findViewById(R.id.status_transport)
        approvalLayer = findViewById(R.id.approval_layer)
        approvalCard = findViewById(R.id.approval_card)
        approvalTitle = findViewById(R.id.approval_title)
        approvalAllow = findViewById(R.id.approval_allow)
        approvalDeny = findViewById(R.id.approval_deny)
        approvalDotStart = findViewById(R.id.approval_dot_start)
        approvalDotEnd = findViewById(R.id.approval_dot_end)
        statsPanel = findViewById(R.id.stats_panel)
        statsBitrate = findViewById(R.id.stats_bitrate)
        statsFps = findViewById(R.id.stats_fps)
        statsPacing = findViewById(R.id.stats_pacing)
        statsStability = findViewById(R.id.stats_stability)

        inputSettings = InputSettingsStore.load(this)
        wireless = WirelessManager(this)
        wireless.startAdvertising(USBConnectionManager.PORT, nativeProtocolVersion())

        surfaceView.isFocusable = true
        surfaceView.isFocusableInTouchMode = true
        surfaceView.requestFocus()
        requestSurfaceFrameRate(surfaceView.holder.surface)
        surfaceView.setOnTouchListener { v, event ->
            handleTouchEvent(v, event)
        }
        surfaceView.setOnGenericMotionListener { v, event ->
            handleGenericMotionEvent(v, event)
        }
        surfaceView.holder.addCallback(this)

        deckOverlayContainer = findViewById(R.id.deck_overlay_container)
        deckEdge = findViewById(R.id.deck_edge)
        deckEdge.onOpenDeck = { showDeckAndControls() }
        bindKeyboard()
        bindWaitingScreen()

        // Show initial waiting state
        updateStatusUI(STATUS_WAITING)
        setImmersiveMode()
    }

    override fun onResume() {
        super.onResume()
        setImmersiveMode()
        // The Wi-Fi address may have changed while the app was in the background.
        if (currentStatus == STATUS_WAITING) renderWaitingDetail()
    }

    override fun onDestroy() {
        uiHandler.removeCallbacks(hideDeckRunnable)
        uiHandler.removeCallbacks(statsUpdateRunnable)
        uiHandler.removeCallbacks(chipStatsRunnable)
        wireless.stopAdvertising()
        wireless.releaseLowLatencyWifi()
        if (nativeStarted) {
            // The receive thread may be blocked waiting on an approval
            // dialog; release it first or nativeStop() could never join it.
            pendingApproval?.decide(false)
            nativeStop()
            nativeStarted = false
        }
        super.onDestroy()
    }

    override fun onBackPressed() {
        // Back on the approval prompt denies it (like cancelling a dialog).
        shownApproval?.let { request ->
            decideApproval(request, false)
            return
        }

        // If deck is showing, hide everything
        if (isDeckVisible) {
            hideDeckAndControls()
            return
        }

        if (statsVisible) {
            closeStatsPanel()
            return
        }

        // If streaming with no UI → show deck + controls
        if (statusOverlay.visibility == View.GONE) {
            showDeckAndControls()
            return
        }

        super.onBackPressed()
    }

    private fun setImmersiveMode() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(false)
            window.insetsController?.let { controller ->
                controller.hide(
                    WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars()
                )
                controller.systemBarsBehavior =
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = (
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    or View.SYSTEM_UI_FLAG_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
            )
        }
    }

    private fun requestSurfaceFrameRate(surface: Surface) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && surface.isValid) {
            surface.setFrameRate(120.0f, Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)
            android.util.Log.i(TAG, "Requested Surface frame rate: 120Hz fixed source")
        }
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        if (nativeStarted) {
            // Surface was recreated (e.g., app went to background and came back).
            // Must restart native layer with the new Surface — old one is dead.
            android.util.Log.i(TAG, "Surface recreated, restarting native layer")
            pendingApproval?.decide(false)
            nativeStop()
            nativeStarted = false
        }
        nativeInit(holder.surface, USBConnectionManager.PORT)
        nativeStarted = true
        requestSurfaceFrameRate(holder.surface)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        requestSurfaceFrameRate(holder.surface)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        // Don't stop native here — surfaceCreated will handle restart.
        // This avoids a race condition during rotation where surfaceDestroyed
        // is called immediately followed by surfaceCreated.
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        return handleTouchEvent(window.decorView, event)
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        return handleGenericMotionEvent(window.decorView, event) || super.onGenericMotionEvent(event)
    }

    private fun handleTouchEvent(view: View, event: MotionEvent): Boolean {
        val width = surfaceView.width
        val height = surfaceView.height
        if (width <= 0 || height <= 0) return false
        if (!isPointerLikeSource(event)) return false

        // When deck is visible, reset auto-hide timer and let overlay handle touches
        if (isDeckVisible) {
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    deckGestureActive = true
                    uiHandler.removeCallbacks(hideDeckRunnable)
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    deckGestureActive = false
                    scheduleDeckAutoHide()
                }
            }
            return false
        }

        view.requestUnbufferedDispatch(event)
        return InputRouter.forwardTouch(view, surfaceView, event, width, height, inputSettings, this)
    }

    private fun handleGenericMotionEvent(view: View, event: MotionEvent): Boolean {
        val width = surfaceView.width
        val height = surfaceView.height
        if (width <= 0 || height <= 0) {
            return false
        }

        if (!isPointerLikeSource(event)) {
            return false
        }

        view.requestUnbufferedDispatch(event)
        return InputRouter.forwardGenericMotion(view, surfaceView, event, width, height, inputSettings, this)
    }

    private fun isPointerLikeSource(event: MotionEvent): Boolean {
        val source = event.source
        return (source and InputDevice.SOURCE_CLASS_POINTER) != 0 ||
            (source and InputDevice.SOURCE_CLASS_POSITION) != 0 ||
            source == InputDevice.SOURCE_MOUSE_RELATIVE
    }

    // ------------------------------------------------------------------
    // Soft keyboard -> Mac typing
    // ------------------------------------------------------------------

    private fun bindKeyboard() {
        keyboardButton = findViewById(R.id.keyboard_button)
        keyboardCapture = findViewById(R.id.keyboard_capture)
        keyboardCapture.onText = { text -> nativeSendKeyText(text.toByteArray(Charsets.UTF_8)) }
        keyboardCapture.onKey = { key -> nativeSendKey(key, 0) }
        // Back closes the keyboard on every Android version; on 11+ the IME insets
        // below also catch its own hide key.
        keyboardCapture.onImeDismissed = {
            setKeyboardOpen(false)
            // The Mac's cursor may move before the keyboard is used again:
            // restart the input session so the IME and our model start fresh
            // together (the old connection's late calls are ignored).
            val imm = getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager
            imm.restartInput(keyboardCapture)
        }
        keyboardButton.setOnClickListener {
            // Below Android 11 the IME's own hide key can't be observed, so a tap
            // there always opens the keyboard (re-opening is harmless).
            if (keyboardOpen && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) hideKeyboard()
            else showKeyboard()
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            // Follow the real IME state (it can also be closed with Back or its own key).
            keyboardCapture.setOnApplyWindowInsetsListener { _, insets ->
                setKeyboardOpen(insets.isVisible(WindowInsets.Type.ime()))
                insets
            }
        }
    }

    private fun showKeyboard() {
        // Only a fresh input session starts a fresh model (onCreateInputConnection
        // resets it); re-showing a live session must keep the composing baseline.
        keyboardCapture.requestFocus()
        val imm = getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager
        imm.showSoftInput(keyboardCapture, 0)
        setKeyboardOpen(true)
    }

    private fun hideKeyboard() {
        val imm = getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager
        imm.hideSoftInputFromWindow(keyboardCapture.windowToken, 0)
        keyboardCapture.clearFocus()
        keyboardCapture.resetComposition()
        setKeyboardOpen(false)
    }

    private fun setKeyboardOpen(open: Boolean) {
        keyboardOpen = open
        keyboardButton.isActivated = open
    }

    /** The button shows only while streaming and when enabled in the Deck. */
    private fun updateKeyboardButton() {
        val show = currentStatus == STATUS_CONNECTED && inputSettings.showKeyboardButton
        keyboardButton.visibility = if (show) View.VISIBLE else View.GONE
        if (!show && keyboardOpen) hideKeyboard()
    }

    private fun persistInputSettings() {
        InputSettingsStore.save(this, inputSettings)
    }

    /** Fills the Deck's Finger / Stylus / Other segmented controls. */
    private fun bindDeckInputModes(overlay: DeckOverlayView) {
        val fingerIcons = mapOf(
            FingerInputMode.NATIVE_TOUCH to R.drawable.ic_ph_hand_pointing,
            FingerInputMode.MOUSE_CURSOR to R.drawable.ic_ph_cursor
        )
        val stylusIcons = mapOf(
            StylusInputMode.NATIVE_PEN to R.drawable.ic_ph_pen,
            StylusInputMode.NATIVE_TOUCH to R.drawable.ic_ph_hand_pointing,
            StylusInputMode.MOUSE_CURSOR to R.drawable.ic_ph_cursor
        )
        overlay.bindInputModes(
            finger = FingerInputMode.values().map {
                SegmentedControlView.Option(it.shortLabel, fingerIcons[it])
            },
            fingerSelected = inputSettings.fingerInputMode.ordinal,
            stylus = StylusInputMode.values().map {
                SegmentedControlView.Option(it.shortLabel, stylusIcons[it])
            },
            stylusSelected = inputSettings.stylusInputMode.ordinal,
            unknown = UnknownPointerFallback.values().map { SegmentedControlView.Option(it.shortLabel) },
            unknownSelected = inputSettings.unknownPointerFallback.ordinal
        )
        overlay.onFingerModeSelected = { index ->
            inputSettings = inputSettings.copy(fingerInputMode = FingerInputMode.values()[index])
            persistInputSettings()
            scheduleDeckAutoHide()
        }
        overlay.onStylusModeSelected = { index ->
            inputSettings = inputSettings.copy(stylusInputMode = StylusInputMode.values()[index])
            persistInputSettings()
            scheduleDeckAutoHide()
        }
        overlay.bindKeyboardButton(inputSettings.showKeyboardButton)
        overlay.onKeysModeSelected = { index ->
            inputSettings = inputSettings.copy(showKeyboardButton = index == 0)
            persistInputSettings()
            updateKeyboardButton()
            scheduleDeckAutoHide()
        }
        overlay.onUnknownModeSelected = { index ->
            inputSettings = inputSettings.copy(
                unknownPointerFallback = UnknownPointerFallback.values()[index]
            )
            persistInputSettings()
            scheduleDeckAutoHide()
        }
    }

    private fun toggleStatsPanel() {
        if (statsVisible) {
            closeStatsPanel()
        } else {
            statsPanel.visibility = View.VISIBLE
            statsVisible = true
            deckOverlay?.setStatsExpanded(true)
            val stats = nativeGetStats()
            lastBytesReceived = stats[0]
            lastFramesDecoded = stats[1]
            lastStatsTime = System.nanoTime()
            uiHandler.post(statsUpdateRunnable)
        }
    }

    private fun closeStatsPanel() {
        if (!statsVisible) return
        statsPanel.visibility = View.GONE
        statsVisible = false
        deckOverlay?.setStatsExpanded(false)
        uiHandler.removeCallbacks(statsUpdateRunnable)
    }

    private fun updateStats() {
        val stats = nativeGetStats()
        val now = System.nanoTime()
        val elapsed = (now - lastStatsTime) / 1_000_000_000.0

        if (elapsed > 0.1) {
            val deltaBytes = stats[0] - lastBytesReceived
            val bitrateMbps = (deltaBytes * 8.0) / elapsed / 1_000_000.0
            statsBitrate.text = String.format("Bitrate: %.1f Mbps", bitrateMbps)

            val deltaFrames = stats[1] - lastFramesDecoded
            val fps = deltaFrames / elapsed
            statsFps.text = String.format("FPS: %.1f", fps)

            val jitterUs = stats[4]
            val jitterMs = jitterUs / 1000.0
            val latencyReleaseMs = if (stats.size > 8) stats[8] / 1000.0 else 0.0
            val desktopMs = if (stats.size > 9) stats[9] / 1000.0 else 0.0
            val androidMs = if (stats.size > 11) stats[11] / 1000.0 else 0.0
            val rttMs = if (stats.size > 12) stats[12] / 1000.0 else 0.0
            statsPacing.text = String.format(
                "Latency: %.1fms active (desk %.1f / and %.1f / rtt %.1f)",
                latencyReleaseMs,
                desktopMs,
                androidMs,
                rttMs
            )

            val totalFed = stats[2]
            val errors = stats[3]
            val skipped = if (stats.size > 6) stats[6] else 0L
            val stability = if (totalFed > 0) {
                ((1.0 - (errors.toDouble() / totalFed)).coerceIn(0.0, 1.0) * 100).toInt()
            } else 100
            statsStability.text = String.format(
                "Stability: %d%% (skip: %d, jitter %.1fms)",
                stability,
                skipped,
                jitterMs
            )
        }

        lastBytesReceived = stats[0]
        lastFramesDecoded = stats[1]
        lastStatsTime = now
    }

    /** Live "30 ms · 57 fps ·" summary for the Deck's stats chip. */
    private fun updateChipStats() {
        val stats = nativeGetStats()
        val now = System.nanoTime()
        val elapsed = (now - chipTime) / 1_000_000_000.0
        if (chipTime != 0L && elapsed > 0.1) {
            val fps = (stats[1] - chipFrames) / elapsed
            val latencyMs = if (stats.size > 8) stats[8] / 1000.0 else 0.0
            val latency = if (latencyMs > 0.0) String.format("%.0f", latencyMs) else "--"
            deckOverlay?.setStats(String.format("%s ms · %.0f fps ·", latency, fps), streamOverWifi)
        } else {
            deckOverlay?.setStats("-- ms · -- fps ·", streamOverWifi)
        }
        chipBytes = stats[0]
        chipFrames = stats[1]
        chipTime = now
    }

    private fun showDeckAndControls() {
        if (isDeckVisible) return
        isDeckVisible = true
        isDeckLocked = false

        // Re-force immersive mode so system bars don't steal space
        setImmersiveMode()

        val overlay = DeckOverlayView(this).apply {
            onDeckAction = { actionType, slotIndex ->
                nativeSendDeckAction(actionType, slotIndex)
                scheduleDeckAutoHide()
            }
            onVolumeChange = { volume, muted ->
                nativeSendVolumeChange(volume, if (muted) 1 else 0)
                scheduleDeckAutoHide()
            }
            onDismiss = { hideDeckAndControls() }
            onInteraction = {
                deckGestureActive = false
                scheduleDeckAutoHide()
            }
            onGestureStart = {
                deckGestureActive = true
                uiHandler.removeCallbacks(hideDeckRunnable)
            }
            onHide = { hideDeckAndControls() }
            onLockToggle = {
                isDeckLocked = !isDeckLocked
                setLocked(isDeckLocked)
                if (isDeckLocked) uiHandler.removeCallbacks(hideDeckRunnable)
                else scheduleDeckAutoHide()
            }
            onStatsChipClick = {
                toggleStatsPanel()
                scheduleDeckAutoHide()
            }
        }
        bindDeckInputModes(overlay)
        overlay.setLocked(false)
        overlay.setStatsExpanded(statsVisible)

        overlay.buildFromConfig(currentDeckConfig)
        currentMediaState?.let(overlay::updateMediaState)
        overlay.updateVolume(currentVolumeLevel, currentVolumeMuted)

        deckOverlayContainer.removeAllViews()
        deckOverlayContainer.addView(overlay, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.MATCH_PARENT
        ))
        deckOverlayContainer.visibility = View.VISIBLE
        overlay.animateIn()
        deckOverlay = overlay
        deckEdge.deckShown = true

        chipTime = 0L
        uiHandler.removeCallbacks(chipStatsRunnable)
        uiHandler.post(chipStatsRunnable)
        scheduleDeckAutoHide()
    }

    private fun hideDeckAndControls() {
        uiHandler.removeCallbacks(hideDeckRunnable)
        deckGestureActive = false
        uiHandler.removeCallbacks(chipStatsRunnable)

        if (!isDeckVisible) return

        deckOverlay?.animateOut {
            deckOverlayContainer.removeAllViews()
            deckOverlayContainer.visibility = View.GONE
            deckOverlay = null
            isDeckVisible = false
            isDeckLocked = false
            deckEdge.deckShown = false
        }
    }

    private fun scheduleDeckAutoHide() {
        uiHandler.removeCallbacks(hideDeckRunnable)
        // Callbacks fired mid-gesture (e.g. volume drag) must not re-arm the timer;
        // it restarts when the finger lifts.
        if (isDeckVisible && !isDeckLocked && !deckGestureActive) {
            uiHandler.postDelayed(hideDeckRunnable, DECK_AUTO_HIDE_MS)
        }
    }

    /**
     * Called from native code (JNI) when connection status changes.
     * This is called from a native thread — must post to UI thread.
     */
    @Suppress("unused") // Called from JNI
    fun onNativeStatusChanged(status: Int) {
        runOnUiThread {
            updateStatusUI(status)
        }
    }

    private fun updateStatusUI(status: Int) {
        currentStatus = status
        when (status) {
            STATUS_WAITING -> {
                closeStatsPanel()
                hideDeckAndControls()
                showStatusScreen()
                setStatusPill("Waiting for a Mac", LinkLine.CABLE)
                renderWaitingDetail()
                wireless.releaseLowLatencyWifi()
            }
            STATUS_CONNECTED -> {
                wireless.acquireLowLatencyWifi()
                hideDeckAndControls()
                streamOverWifi = pendingWifiConnection
                pendingWifiConnection = false
                statusOverlay.visibility = View.GONE
                statusHero.waiting = false
                deckEdge.wifi = streamOverWifi
                deckEdge.deckShown = false
                deckEdge.visibility = View.VISIBLE
            }
            STATUS_DISCONNECTED -> {
                closeStatsPanel()
                hideDeckAndControls()
                showStatusScreen()
                setStatusPill("Reconnecting…", LinkLine.WARN)
                wireless.releaseLowLatencyWifi()
                renderWaitingDetail(headline = "Disconnected from your Mac")
            }
            STATUS_ERROR -> {
                closeStatsPanel()
                hideDeckAndControls()
                showStatusScreen()
                setStatusPill("Error", LinkLine.ERROR)
                statusHero.waiting = false
                statusDetail.text = "Something went wrong. Please restart the app."
                statusHintRow.visibility = View.GONE
            }
        }
        updateKeyboardButton()
    }

    /** One-time setup of the static parts of the waiting screen. */
    private fun bindWaitingScreen() {
        val name = wireless.deviceName
        statusDeviceName.text = name
        statusStep1.text = stepText("Open ", "DroidScreen", " on your Mac")
        statusStep2.text = stepText("Choose ", "\u201C$name\u201D", " in the menu bar")
        statusStep3.text = stepText("", "Allow", " the connection here")
        statusVersion.text = "DroidScreen ${appVersionName()}"
        approvalAllow.setOnClickListener { shownApproval?.let { decideApproval(it, true) } }
        approvalDeny.setOnClickListener { shownApproval?.let { decideApproval(it, false) } }
        statusHero.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ ->
            if (approvalLayer.visibility == View.VISIBLE) positionApprovalCard()
        }
        approvalCard.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ ->
            if (approvalLayer.visibility == View.VISIBLE) positionApprovalCard()
        }
    }

    private fun showStatusScreen() {
        statusOverlay.visibility = View.VISIBLE
        deckEdge.visibility = View.GONE
        streamOverWifi = false
    }

    private fun setStatusPill(text: String, dotColor: Int) {
        statusText.text = text
        statusDot.background.mutate().setTint(dotColor)
    }

    /** "Ready on Wi-Fi as <ip>" (IP in mono yellow), or the USB-only variant. */
    private fun renderWaitingDetail(headline: String? = null) {
        val ip = wireless.wifiAddress()
        val wifi = ip != null
        statusMark.wifi = wifi
        statusHero.wifi = wifi
        statusHero.address = ip
        statusHero.waiting = currentStatus != STATUS_ERROR
        statusHero.cableLabel = if (approvalLayer.visibility == View.VISIBLE) null
        else if (wifi) "Wi-Fi · waiting" else "USB · waiting"
        statusTransport.text = if (wifi) "Wi-Fi · $ip" else "USB"
        statusHintRow.visibility = View.VISIBLE
        if (headline != null) {
            statusDetail.text = headline
            statusHint.text = if (wifi) "Reconnecting over Wi-Fi or USB…" else "Reconnecting over USB…"
        } else if (wifi) {
            val text = SpannableStringBuilder("Ready on Wi-Fi as ")
            val start = text.length
            text.append(ip)
            text.setSpan(
                LinkLine.TypefaceSpanCompat(LinkLine.mono(this), LinkLine.CABLE),
                start, text.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE
            )
            statusDetail.text = text
            statusHint.text = "or connect a USB cable"
        } else {
            statusDetail.text = "Not on Wi-Fi"
            statusHint.text = "Connect a USB cable, or join the same Wi-Fi as your Mac"
        }
    }

    private fun stepText(before: String, strong: String, after: String): CharSequence {
        val text = SpannableStringBuilder(before)
        val start = text.length
        text.append(strong)
        text.setSpan(
            LinkLine.TypefaceSpanCompat(LinkLine.sans(this, 500), LinkLine.TEXT),
            start, text.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE
        )
        text.append(after)
        return text
    }

    private fun appVersionName(): String = try {
        packageManager.getPackageInfo(packageName, 0).versionName ?: ""
    } catch (e: Exception) {
        ""
    }

    /** Shows the in-layout approval card for [request] (UI thread). */
    private fun showApprovalPrompt(request: PendingApproval) {
        shownApproval = request
        approvalTitle.text = "\u201C${request.name}\u201D?"
        approvalLayer.visibility = View.VISIBLE
        statusHero.cableLabel = null
        // Allow is never pre-focused.
        approvalLayer.requestFocus()
        positionApprovalCard()
    }

    /** Hides the prompt if it still belongs to [request] (UI thread). */
    private fun hideApprovalPrompt(request: PendingApproval) {
        if (shownApproval !== request) return
        shownApproval = null
        approvalLayer.visibility = View.GONE
        if (currentStatus == STATUS_WAITING || currentStatus == STATUS_DISCONNECTED) {
            renderWaitingDetail(
                headline = if (currentStatus == STATUS_DISCONNECTED) "Disconnected from your Mac" else null
            )
        }
    }

    private fun decideApproval(request: PendingApproval, allow: Boolean) {
        // Only the first answer counts; the receive thread may already be reading it.
        if (request.latch.count == 0L) return
        request.decide(allow)
        hideApprovalPrompt(request)
    }

    /**
     * Anchors the approval card on the middle of the hero cable, with the two
     * yellow dots where the cable meets the card's edges. Falls back to the
     * vertical center when the waiting screen is not showing.
     */
    private fun positionApprovalCard() {
        val layerH = approvalLayer.height
        val cardH = approvalCard.height
        if (layerH == 0 || cardH == 0) {
            approvalLayer.post { if (approvalLayer.visibility == View.VISIBLE) positionApprovalCard() }
            return
        }
        val density = resources.displayMetrics.density
        val margin = 16f * density
        val heroVisible = statusOverlay.visibility == View.VISIBLE && statusHero.isShown
        val cableY: Float
        val cardTop: Float
        if (heroVisible) {
            val heroLoc = IntArray(2)
            val layerLoc = IntArray(2)
            statusHero.getLocationInWindow(heroLoc)
            approvalLayer.getLocationInWindow(layerLoc)
            cableY = heroLoc[1] - layerLoc[1] + statusHero.cableCenterY()
            cardTop = (cableY - CARD_CABLE_OFFSET_DP * density)
                .coerceIn(margin, (layerH - cardH - margin).coerceAtLeast(margin))
        } else {
            cardTop = (layerH - cardH) / 2f
            cableY = -1f
        }
        approvalCard.translationY = cardTop
        val dotsVisible = heroVisible && cableY > cardTop && cableY < cardTop + cardH
        approvalDotStart.visibility = if (dotsVisible) View.VISIBLE else View.INVISIBLE
        approvalDotEnd.visibility = if (dotsVisible) View.VISIBLE else View.INVISIBLE
        if (dotsVisible) {
            val half = approvalDotStart.width / 2f
            approvalDotStart.translationX = approvalCard.left - half
            approvalDotStart.translationY = cableY - half
            approvalDotEnd.translationX = approvalCard.right - half
            approvalDotEnd.translationY = cableY - half
        }
    }

    /**
     * Called from the native receive thread when a computer connects over
     * Wi-Fi. Returns the token it must keep, or null to reject it. Blocks
     * until the user decides (or the request times out).
     */
    @Suppress("unused") // Called from JNI
    fun authorizeDesktop(desktopId: String, name: String, presentedToken: String): String? {
        wireless.tokenIfTrusted(desktopId, presentedToken)?.let {
            pendingWifiConnection = true
            return it
        }

        val request = PendingApproval(name)
        pendingApproval = request
        runOnUiThread {
            if (isFinishing || isDestroyed) {
                request.decide(false)
                return@runOnUiThread
            }
            showApprovalPrompt(request)
        }
        request.latch.await(APPROVAL_TIMEOUT_MS, TimeUnit.MILLISECONDS)
        runOnUiThread { hideApprovalPrompt(request) }
        pendingApproval = null
        if (request.approved) pendingWifiConnection = true
        return if (request.approved) wireless.trust(desktopId, name) else null
    }

    @Suppress("unused") // Called from JNI
    fun onDeckConfigReceived(json: String) {
        runOnUiThread {
            try {
                currentDeckConfig = DeckConfig.fromJson(json)
                deckOverlay?.buildFromConfig(currentDeckConfig)
                currentMediaState?.let { deckOverlay?.updateMediaState(it) }
                deckOverlay?.updateVolume(currentVolumeLevel, currentVolumeMuted)
            } catch (e: Exception) {
                android.util.Log.w(TAG, "Failed to parse deck config", e)
            }
        }
    }

    @Suppress("unused") // Called from JNI
    fun onMediaStateReceived(json: String) {
        runOnUiThread {
            try {
                val state = MediaState.fromJson(json, currentMediaState)
                currentMediaState = state
                deckOverlay?.updateMediaState(state)
            } catch (e: Exception) {
                android.util.Log.w(TAG, "Failed to parse media state", e)
            }
        }
    }

    @Suppress("unused") // Called from JNI
    fun onVolumeStateReceived(volume: Int, muted: Boolean) {
        runOnUiThread {
            currentVolumeLevel = volume
            currentVolumeMuted = muted
            deckOverlay?.updateVolume(volume, muted)
        }
    }

    private external fun nativeInit(surface: Surface, port: Int)
    private external fun nativeStop()
    private external fun nativeProtocolVersion(): Int
    external fun nativeSendTouch(
        action: Int,
        pointerId: Int,
        xFrac: Int,
        yFrac: Int,
        pressure: Int,
        touchMajor: Int,
        touchMinor: Int,
        orientation: Int
    )
    external fun nativeSendPen(
        action: Int,
        pointerId: Int,
        toolType: Int,
        buttons: Int,
        xFrac: Int,
        yFrac: Int,
        pressure: Int,
        distance: Int,
        tilt: Int,
        rotation: Int
    )
    external fun nativeSendMouse(
        action: Int,
        buttons: Int,
        xFrac: Int,
        yFrac: Int
    )
    private external fun nativeGetStats(): LongArray
    external fun nativeSendDeckAction(actionType: Int, slotIndex: Int)
    external fun nativeSendVolumeChange(volume: Int, muted: Int)
    external fun nativeSendKeyText(utf8: ByteArray)
    external fun nativeSendKey(key: Int, modifiers: Int)
}
