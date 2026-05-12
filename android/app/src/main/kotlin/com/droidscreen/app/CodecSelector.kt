package com.droidscreen.app

import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.os.Build
import android.util.Log

object CodecSelector {
    private const val TAG = "DroidScreen"

    private const val DS_CODEC_H264 = 0
    private const val DS_CODEC_HEVC = 1
    private const val DS_CODEC_CAP_H264 = 1 shl DS_CODEC_H264
    private const val DS_CODEC_CAP_HEVC = 1 shl DS_CODEC_HEVC

    private const val MIME_H264 = "video/avc"
    private const val MIME_HEVC = "video/hevc"

    @JvmStatic
    fun chooseDecoderForNative(
        codecMask: Int,
        width: Int,
        height: Int,
        fps: Int
    ): NativeDecoderChoice? {
        val mask = if (codecMask == 0) DS_CODEC_CAP_H264 else codecMask
        val candidates = ArrayList<Candidate>()

        if ((mask and DS_CODEC_CAP_H264) != 0) {
            collectCandidates(DS_CODEC_H264, MIME_H264, width, height, fps, candidates)
        }
        if ((mask and DS_CODEC_CAP_HEVC) != 0) {
            collectCandidates(DS_CODEC_HEVC, MIME_HEVC, width, height, fps, candidates)
        }

        val selected = candidates.maxByOrNull { it.score } ?: run {
            Log.w(TAG, "CodecSelector: no hardware decoder candidate for mask=0x${mask.toString(16)}")
            return null
        }

        Log.i(
            TAG,
            "CodecSelector: selected ${selected.info.name} mime=${selected.mime} " +
                "codec=${selected.codecId} score=${selected.score} " +
                "lowLatency=${selected.lowLatency} direct=${selected.directSubmit}"
        )

        return NativeDecoderChoice(
            codecId = selected.codecId,
            mime = selected.mime,
            decoderName = selected.info.name,
            directSubmit = selected.directSubmit,
            hasAndroidLowLatency = selected.lowLatency,
            isQcomC2 = selected.isQcomC2,
            isQcomOmx = selected.isQcomOmx
        )
    }

    private fun collectCandidates(
        codecId: Int,
        mime: String,
        width: Int,
        height: Int,
        fps: Int,
        out: MutableList<Candidate>
    ) {
        if (codecId == DS_CODEC_HEVC && hevcIsBlockedForThisDevice()) {
            Log.w(TAG, "CodecSelector: skipping HEVC on ${Build.DEVICE}; output is unstable on this tablet")
            return
        }

        val codecs = try {
            MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos
        } catch (t: Throwable) {
            Log.w(TAG, "CodecSelector: MediaCodecList failed", t)
            return
        }

        for (info in codecs) {
            if (info.isEncoder) continue
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && info.isAlias) continue
            if (isSoftwareDecoder(info)) continue
            if (!info.supportedTypes.any { it.equals(mime, ignoreCase = true) }) continue

            val caps = try {
                info.getCapabilitiesForType(mime)
            } catch (t: Throwable) {
                Log.w(TAG, "CodecSelector: caps failed for ${info.name} $mime", t)
                continue
            }

            if (!sizeAndRateLooksSupported(caps, width, height, fps)) continue

            val name = info.name.lowercase()
            val lowLatency = Build.VERSION.SDK_INT >= Build.VERSION_CODES.R &&
                caps.isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency)
            val nameLowLatency = name.contains("low_latency") || name.contains("low-latency")
            val isQcomC2 = name.startsWith("c2.qti") || name.startsWith("c2.qcom")
            val isQcomOmx = name.startsWith("omx.qcom")
            val directSubmit = name.startsWith("c2.") ||
                name.startsWith("omx.qcom") ||
                name.startsWith("omx.sec") ||
                name.startsWith("omx.exynos") ||
                name.startsWith("omx.nvidia")

            var score = 0
            if (lowLatency) score += 1000
            if (nameLowLatency) score += 500
            if (codecId == DS_CODEC_HEVC) {
                score += if (lowLatency || nameLowLatency) 120 else -150
            } else {
                score += 50
            }
            if (isQcomC2) score += 80
            if (isQcomOmx) score += 60
            if (name.startsWith("c2.")) score += 40
            if (directSubmit) score += 20

            out += Candidate(
                codecId = codecId,
                mime = mime,
                info = info,
                lowLatency = lowLatency,
                directSubmit = directSubmit,
                isQcomC2 = isQcomC2,
                isQcomOmx = isQcomOmx,
                score = score
            )

            Log.i(
                TAG,
                "CodecSelector: candidate ${info.name} mime=$mime codec=$codecId " +
                    "score=$score lowLatency=$lowLatency direct=$directSubmit"
            )
        }
    }

    private fun hevcIsBlockedForThisDevice(): Boolean {
        // POCO Pad / Xiaomi dizi advertises c2.qti.hevc.decoder.low_latency,
        // but validation shows Qualcomm output-port errors followed by black
        // Surface content at 2560x1600@120. H.264 low_latency is the safe path.
        return Build.DEVICE.equals("dizi", ignoreCase = true)
    }

    private fun isSoftwareDecoder(info: MediaCodecInfo): Boolean {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && info.isSoftwareOnly) {
            return true
        }
        val name = info.name.lowercase()
        return name.startsWith("omx.google") ||
            name.startsWith("c2.android") ||
            name.contains("ffmpeg") ||
            name.contains("sw") ||
            name.contains("software")
    }

    private fun sizeAndRateLooksSupported(
        caps: MediaCodecInfo.CodecCapabilities,
        width: Int,
        height: Int,
        fps: Int
    ): Boolean {
        if (width <= 0 || height <= 0 || fps <= 0) return true
        return try {
            val videoCaps = caps.videoCapabilities ?: return true
            videoCaps.areSizeAndRateSupported(width, height, fps.toDouble())
        } catch (_: Throwable) {
            true
        }
    }

    private data class Candidate(
        val codecId: Int,
        val mime: String,
        val info: MediaCodecInfo,
        val lowLatency: Boolean,
        val directSubmit: Boolean,
        val isQcomC2: Boolean,
        val isQcomOmx: Boolean,
        val score: Int
    )
}
