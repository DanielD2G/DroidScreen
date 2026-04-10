package com.droidscreen.app

import android.content.Context

enum class FingerInputMode(val storageValue: String, val label: String) {
    NATIVE_TOUCH("native_touch", "Native touch"),
    MOUSE_CURSOR("mouse_cursor", "Mouse cursor");

    companion object {
        fun fromStorage(value: String?): FingerInputMode =
            values().firstOrNull { it.storageValue == value } ?: NATIVE_TOUCH
    }
}

enum class StylusInputMode(val storageValue: String, val label: String) {
    NATIVE_PEN("native_pen", "Native pen"),
    NATIVE_TOUCH("native_touch", "Native touch"),
    MOUSE_CURSOR("mouse_cursor", "Mouse cursor");

    companion object {
        fun fromStorage(value: String?): StylusInputMode =
            values().firstOrNull { it.storageValue == value } ?: NATIVE_PEN
    }
}

enum class UnknownPointerFallback(val storageValue: String, val label: String) {
    TREAT_AS_FINGER("treat_as_finger", "Treat as finger"),
    IGNORE("ignore", "Ignore");

    companion object {
        fun fromStorage(value: String?): UnknownPointerFallback =
            values().firstOrNull { it.storageValue == value } ?: TREAT_AS_FINGER
    }
}

data class InputSettings(
    val fingerInputMode: FingerInputMode = FingerInputMode.NATIVE_TOUCH,
    val stylusInputMode: StylusInputMode = StylusInputMode.NATIVE_PEN,
    val unknownPointerFallback: UnknownPointerFallback = UnknownPointerFallback.TREAT_AS_FINGER
)

object InputSettingsStore {
    private const val PREFS_NAME = "droidscreen_input_settings"
    private const val KEY_FINGER_MODE = "finger_input_mode"
    private const val KEY_STYLUS_MODE = "stylus_input_mode"
    private const val KEY_UNKNOWN_FALLBACK = "unknown_pointer_fallback"

    fun load(context: Context): InputSettings {
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        return InputSettings(
            fingerInputMode = FingerInputMode.fromStorage(prefs.getString(KEY_FINGER_MODE, null)),
            stylusInputMode = StylusInputMode.fromStorage(prefs.getString(KEY_STYLUS_MODE, null)),
            unknownPointerFallback = UnknownPointerFallback.fromStorage(
                prefs.getString(KEY_UNKNOWN_FALLBACK, null)
            )
        )
    }

    fun save(context: Context, settings: InputSettings) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit()
            .putString(KEY_FINGER_MODE, settings.fingerInputMode.storageValue)
            .putString(KEY_STYLUS_MODE, settings.stylusInputMode.storageValue)
            .putString(KEY_UNKNOWN_FALLBACK, settings.unknownPointerFallback.storageValue)
            .apply()
    }
}
