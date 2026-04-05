/*
 * DroidScreen macOS - VideoToolbox H.264 encoder
 *
 * Hardware-accelerated H.264 encoding using VTCompressionSession.
 * Outputs Annex B format NAL units suitable for Android MediaCodec.
 */

#pragma once

#include "droidscreen/encoder.h"

#include <atomic>
#include <functional>
#include <mutex>

#ifdef __OBJC__
#import <VideoToolbox/VideoToolbox.h>
#else
typedef struct OpaqueVTCompressionSession* VTCompressionSessionRef;
#endif

namespace droidscreen {

class VTEncoder : public Encoder {
public:
    VTEncoder();
    ~VTEncoder() override;

    bool init(uint32_t width, uint32_t height,
              uint32_t fps, uint32_t bitrate_kbps) override;
    bool encode(void* native_frame, int64_t timestamp_us,
                std::function<void(const EncodedPacket&)> on_packet) override;
    bool set_bitrate(uint32_t bitrate_kbps) override;
    bool force_keyframe() override;
    void shutdown() override;

private:
    VTCompressionSessionRef session_ = nullptr;
    uint32_t width_  = 0;
    uint32_t height_ = 0;
    uint32_t fps_    = 0;

    std::atomic<bool> keyframe_pending_{false};
    bool config_sent_ = false;

    // Mutex to protect the on_packet callback during encoding.
    std::mutex encode_mutex_;

    // The active on_packet callback (set per-encode call).
    std::function<void(const EncodedPacket&)> current_callback_;

    /// Extract SPS/PPS from the format description and emit as config packet.
    void emit_config(CMFormatDescriptionRef fmt, int64_t timestamp_us);

    /// Convert AVCC-formatted sample buffer to Annex B and emit.
    void emit_frame(CMSampleBufferRef sample_buf, bool is_keyframe);

    /// Static VTCompressionSession output callback.
    static void output_callback(void* refcon,
                                void* source_frame_refcon,
                                OSStatus status,
                                VTEncodeInfoFlags info_flags,
                                CMSampleBufferRef sample_buf);
};

} // namespace droidscreen
