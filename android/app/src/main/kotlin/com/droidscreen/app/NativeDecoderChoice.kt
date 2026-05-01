package com.droidscreen.app

class NativeDecoderChoice(
    @JvmField val codecId: Int,
    @JvmField val mime: String,
    @JvmField val decoderName: String,
    @JvmField val directSubmit: Boolean,
    @JvmField val hasAndroidLowLatency: Boolean,
    @JvmField val isQcomC2: Boolean,
    @JvmField val isQcomOmx: Boolean
)
