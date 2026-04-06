package com.droidscreen.app

import android.app.Activity
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.InputDevice
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.Surface
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.LinearLayout
import android.widget.Spinner
import android.widget.TextView

class MainActivity : Activity(), SurfaceHolder.Callback {

    companion object {
        private const val TAG = "DroidScreen"
        private const val CONTROLS_AUTO_HIDE_DELAY_MS = 3_000L

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
    private lateinit var statusOverlay: LinearLayout
    private lateinit var statusText: TextView
    private lateinit var statusDetail: TextView
    private lateinit var inputSettingsScrim: View
    private lateinit var controlsContainer: LinearLayout
    private lateinit var inputToggleButton: Button
    private lateinit var inputPanel: LinearLayout
    private lateinit var fingerModeSpinner: Spinner
    private lateinit var stylusModeSpinner: Spinner
    private lateinit var unknownModeSpinner: Spinner
    private var nativeStarted = false
    private var controlsVisible = true
    private lateinit var inputSettings: InputSettings
    private lateinit var statsToggleButton: Button
    private lateinit var statsPanel: LinearLayout
    private lateinit var statsBitrate: TextView
    private lateinit var statsFps: TextView
    private lateinit var statsPacing: TextView
    private lateinit var statsStability: TextView
    private var statsVisible = false
    private var lastBytesReceived = 0L
    private var lastFramesDecoded = 0L
    private var lastStatsTime = 0L
    private val uiHandler = Handler(Looper.getMainLooper())
    private val statsUpdateRunnable = object : Runnable {
        override fun run() {
            if (statsVisible) {
                updateStats()
                uiHandler.postDelayed(this, 500)
            }
        }
    }
    private val hideControlsRunnable = Runnable {
        if (statusOverlay.visibility == View.GONE && controlsVisible) {
            setControlsVisible(false)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        surfaceView = findViewById(R.id.surface_view)
        statusOverlay = findViewById(R.id.status_overlay)
        statusText = findViewById(R.id.status_text)
        statusDetail = findViewById(R.id.status_detail)
        inputSettingsScrim = findViewById(R.id.input_settings_scrim)
        controlsContainer = findViewById(R.id.controls_container)
        inputToggleButton = findViewById(R.id.input_settings_toggle)
        inputPanel = findViewById(R.id.input_settings_panel)
        fingerModeSpinner = findViewById(R.id.finger_mode_spinner)
        stylusModeSpinner = findViewById(R.id.stylus_mode_spinner)
        unknownModeSpinner = findViewById(R.id.unknown_mode_spinner)
        statsToggleButton = findViewById(R.id.stats_toggle)
        statsPanel = findViewById(R.id.stats_panel)
        statsBitrate = findViewById(R.id.stats_bitrate)
        statsFps = findViewById(R.id.stats_fps)
        statsPacing = findViewById(R.id.stats_pacing)
        statsStability = findViewById(R.id.stats_stability)

        inputSettings = InputSettingsStore.load(this)

        surfaceView.isFocusable = true
        surfaceView.isFocusableInTouchMode = true
        surfaceView.requestFocus()
        surfaceView.setOnTouchListener { v, event ->
            handleTouchEvent(v, event)
        }
        surfaceView.setOnGenericMotionListener { v, event ->
            handleGenericMotionEvent(v, event)
        }
        surfaceView.holder.addCallback(this)

        // Show initial waiting state
        updateStatusUI(STATUS_WAITING)
        bindInputSettingsUi()
        bindStatsUi()

        setImmersiveMode()
    }

    override fun onResume() {
        super.onResume()
        setImmersiveMode()
    }

    override fun onDestroy() {
        uiHandler.removeCallbacks(hideControlsRunnable)
        uiHandler.removeCallbacks(statsUpdateRunnable)
        if (nativeStarted) {
            nativeStop()
            nativeStarted = false
        }
        super.onDestroy()
    }

    override fun onBackPressed() {
        if (statsVisible) {
            closeStatsPanel()
            return
        }

        if (inputPanel.visibility == View.VISIBLE) {
            closeInputPanel()
            return
        }

        if (statusOverlay.visibility == View.GONE && !controlsVisible) {
            setControlsVisible(true)
            return
        }

        super.onBackPressed()
    }

    private fun setImmersiveMode() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
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

    override fun surfaceCreated(holder: SurfaceHolder) {
        if (nativeStarted) {
            // Surface was recreated (e.g., app went to background and came back).
            // Must restart native layer with the new Surface — old one is dead.
            android.util.Log.i(TAG, "Surface recreated, restarting native layer")
            nativeStop()
            nativeStarted = false
        }
        nativeInit(holder.surface, USBConnectionManager.PORT)
        nativeStarted = true
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
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
        if (width <= 0 || height <= 0) {
            return false
        }

        if (!isPointerLikeSource(event)) {
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

    private fun bindInputSettingsUi() {
        val touchRescheduler = View.OnTouchListener { _, event ->
            if (event.actionMasked == MotionEvent.ACTION_DOWN) {
                scheduleControlsAutoHide()
            }
            false
        }

        controlsContainer.setOnTouchListener(touchRescheduler)
        inputSettingsScrim.setOnClickListener {
            closeInputPanel()
            scheduleControlsAutoHide()
        }
        inputSettingsScrim.setOnTouchListener(touchRescheduler)

        inputToggleButton.setOnClickListener {
            toggleInputPanel()
            scheduleControlsAutoHide()
        }

        bindSpinner(
            fingerModeSpinner,
            FingerInputMode.values().map { it.label },
            inputSettings.fingerInputMode.ordinal
        ) { index ->
            inputSettings = inputSettings.copy(
                fingerInputMode = FingerInputMode.values()[index]
            )
            persistInputSettings()
            scheduleControlsAutoHide()
        }

        bindSpinner(
            stylusModeSpinner,
            StylusInputMode.values().map { it.label },
            inputSettings.stylusInputMode.ordinal
        ) { index ->
            inputSettings = inputSettings.copy(
                stylusInputMode = StylusInputMode.values()[index]
            )
            persistInputSettings()
            scheduleControlsAutoHide()
        }

        bindSpinner(
            unknownModeSpinner,
            UnknownPointerFallback.values().map { it.label },
            inputSettings.unknownPointerFallback.ordinal
        ) { index ->
            inputSettings = inputSettings.copy(
                unknownPointerFallback = UnknownPointerFallback.values()[index]
            )
            persistInputSettings()
            scheduleControlsAutoHide()
        }
    }

    private fun bindSpinner(
        spinner: Spinner,
        items: List<String>,
        selectedIndex: Int,
        onSelected: (Int) -> Unit
    ) {
        val adapter = ArrayAdapter(this, android.R.layout.simple_spinner_item, items)
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        spinner.adapter = adapter
        spinner.setSelection(selectedIndex, false)
        spinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                onSelected(position)
            }

            override fun onNothingSelected(parent: AdapterView<*>?) = Unit
        }
    }

    private fun persistInputSettings() {
        InputSettingsStore.save(this, inputSettings)
    }

    private fun toggleInputPanel() {
        val showing = inputPanel.visibility == View.VISIBLE
        if (showing) {
            closeInputPanel()
        } else {
            inputPanel.visibility = View.VISIBLE
            inputSettingsScrim.visibility = View.VISIBLE
            inputToggleButton.text = "Close"
        }
    }

    private fun closeInputPanel() {
        inputPanel.visibility = View.GONE
        inputSettingsScrim.visibility = View.GONE
        inputToggleButton.text = "Input"
        scheduleControlsAutoHide()
    }

    private fun bindStatsUi() {
        statsToggleButton.setOnClickListener {
            toggleStatsPanel()
            scheduleControlsAutoHide()
        }
    }

    private fun toggleStatsPanel() {
        if (statsVisible) {
            closeStatsPanel()
        } else {
            statsPanel.visibility = View.VISIBLE
            statsToggleButton.text = "Close"
            statsVisible = true
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
        statsToggleButton.text = "Stats"
        statsVisible = false
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
            val expectedIntervalUs = stats[5]
            val jitterMs = jitterUs / 1000.0
            val pacingQuality = if (expectedIntervalUs > 0) {
                ((1.0 - (jitterUs.toDouble() / expectedIntervalUs)).coerceIn(0.0, 1.0) * 100).toInt()
            } else 0
            statsPacing.text = String.format("Pacing: %d%% (%.1fms)", pacingQuality, jitterMs)

            val totalFed = stats[2]
            val errors = stats[3]
            val stability = if (totalFed > 0) {
                ((1.0 - (errors.toDouble() / totalFed)).coerceIn(0.0, 1.0) * 100).toInt()
            } else 100
            statsStability.text = String.format("Stability: %d%%", stability)
        }

        lastBytesReceived = stats[0]
        lastFramesDecoded = stats[1]
        lastStatsTime = now
    }

    private fun setControlsVisible(visible: Boolean) {
        controlsVisible = visible
        controlsContainer.visibility = if (visible) View.VISIBLE else View.GONE
        if (!visible) {
            uiHandler.removeCallbacks(hideControlsRunnable)
            closeInputPanel()
        } else {
            scheduleControlsAutoHide()
        }
    }

    private fun scheduleControlsAutoHide() {
        uiHandler.removeCallbacks(hideControlsRunnable)
        if (statusOverlay.visibility == View.GONE && controlsVisible) {
            uiHandler.postDelayed(hideControlsRunnable, CONTROLS_AUTO_HIDE_DELAY_MS)
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
        when (status) {
            STATUS_WAITING -> {
                closeStatsPanel()
                setControlsVisible(true)
                uiHandler.removeCallbacks(hideControlsRunnable)
                statusOverlay.visibility = View.VISIBLE
                statusText.text = "Waiting for connection..."
                statusDetail.text = "Port: ${USBConnectionManager.PORT}"
            }
            STATUS_CONNECTED -> {
                setControlsVisible(false)
                statusOverlay.visibility = View.GONE
            }
            STATUS_DISCONNECTED -> {
                closeStatsPanel()
                setControlsVisible(true)
                uiHandler.removeCallbacks(hideControlsRunnable)
                statusOverlay.visibility = View.VISIBLE
                statusText.text = "Disconnected"
                statusDetail.text = "Reconnecting..."
            }
            STATUS_ERROR -> {
                closeStatsPanel()
                setControlsVisible(true)
                uiHandler.removeCallbacks(hideControlsRunnable)
                statusOverlay.visibility = View.VISIBLE
                statusText.text = "Error"
                statusDetail.text = "Please restart the app"
            }
        }
    }

    private external fun nativeInit(surface: Surface, port: Int)
    private external fun nativeStop()
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
}
