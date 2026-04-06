package com.droidscreen.app

import android.app.Activity
import android.os.Build
import android.os.Bundle
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
    private lateinit var inputToggleButton: Button
    private lateinit var inputPanel: LinearLayout
    private lateinit var fingerModeSpinner: Spinner
    private lateinit var stylusModeSpinner: Spinner
    private lateinit var unknownModeSpinner: Spinner
    private var nativeStarted = false
    private lateinit var inputSettings: InputSettings

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        surfaceView = findViewById(R.id.surface_view)
        statusOverlay = findViewById(R.id.status_overlay)
        statusText = findViewById(R.id.status_text)
        statusDetail = findViewById(R.id.status_detail)
        inputToggleButton = findViewById(R.id.input_settings_toggle)
        inputPanel = findViewById(R.id.input_settings_panel)
        fingerModeSpinner = findViewById(R.id.finger_mode_spinner)
        stylusModeSpinner = findViewById(R.id.stylus_mode_spinner)
        unknownModeSpinner = findViewById(R.id.unknown_mode_spinner)

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

        setImmersiveMode()
    }

    override fun onResume() {
        super.onResume()
        setImmersiveMode()
    }

    override fun onDestroy() {
        if (nativeStarted) {
            nativeStop()
            nativeStarted = false
        }
        super.onDestroy()
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
        return handleTouchEvent(surfaceView, event)
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        return handleGenericMotionEvent(surfaceView, event) || super.onGenericMotionEvent(event)
    }

    private fun handleTouchEvent(view: View, event: MotionEvent): Boolean {
        val width = view.width
        val height = view.height
        if (width <= 0 || height <= 0) {
            return false
        }

        val source = event.source
        val isPointerSource =
            (source and InputDevice.SOURCE_TOUCHSCREEN) == InputDevice.SOURCE_TOUCHSCREEN ||
            (source and InputDevice.SOURCE_STYLUS) == InputDevice.SOURCE_STYLUS
        if (!isPointerSource) {
            return false
        }

        view.requestUnbufferedDispatch(event)
        return InputRouter.forwardTouch(event, width, height, inputSettings, this)
    }

    private fun handleGenericMotionEvent(view: View, event: MotionEvent): Boolean {
        val width = view.width
        val height = view.height
        if (width <= 0 || height <= 0) {
            return false
        }

        val source = event.source
        val isPointerSource =
            (source and InputDevice.SOURCE_TOUCHSCREEN) == InputDevice.SOURCE_TOUCHSCREEN ||
            (source and InputDevice.SOURCE_STYLUS) == InputDevice.SOURCE_STYLUS
        if (!isPointerSource) {
            return false
        }

        view.requestUnbufferedDispatch(event)
        return InputRouter.forwardGenericMotion(event, width, height, inputSettings, this)
    }

    private fun bindInputSettingsUi() {
        inputToggleButton.setOnClickListener {
            val showing = inputPanel.visibility == View.VISIBLE
            inputPanel.visibility = if (showing) View.GONE else View.VISIBLE
            inputToggleButton.text = if (showing) "Input" else "Close"
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
                statusOverlay.visibility = View.VISIBLE
                statusText.text = "Waiting for connection..."
                statusDetail.text = "Port: ${USBConnectionManager.PORT}"
            }
            STATUS_CONNECTED -> {
                statusOverlay.visibility = View.GONE
            }
            STATUS_DISCONNECTED -> {
                statusOverlay.visibility = View.VISIBLE
                statusText.text = "Disconnected"
                statusDetail.text = "Reconnecting..."
            }
            STATUS_ERROR -> {
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
}
