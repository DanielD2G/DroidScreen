/*
 * DroidScreen Windows - FFmpeg-based H.264 encoder
 *
 * Uses libavcodec for H.264 encoding with automatic hardware detection
 * and software fallback. Tries encoders in order:
 *   1. h264_nvenc  (NVIDIA)
 *   2. h264_qsv    (Intel Quick Sync)
 *   3. h264_amf    (AMD AMF)
 *   4. libx264     (software fallback)
 *
 * Input: ID3D11Texture2D* (BGRA) from WGC capturer.
 * Conversion: BGRA -> NV12 via libswscale (replaces the HLSL color converter).
 * Output: H.264 Annex B NAL units via the on_packet callback.
 *
 * Configuration targets ultra-low-latency streaming:
 *   - 0 B-frames, no lookahead
 *   - CBR rate control
 *   - Keyframe every 2 seconds
 */

#pragma once

#include "droidscreen/encoder.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace droidscreen {

class FFmpegEncoder : public Encoder {
public:
    FFmpegEncoder();
    ~FFmpegEncoder() override;

    /// Set the D3D11 device to use (must be called before init).
    /// If d3d_mutex is non-null, the encoder will lock it around all
    /// D3D11 context calls (CopyResource, Map, Unmap) to prevent races
    /// with the WGC capturer's callback thread.
    void set_d3d_device(ID3D11Device* device, ID3D11DeviceContext* context,
                        std::mutex* d3d_mutex = nullptr);

    bool init(uint32_t width, uint32_t height,
              uint32_t fps, uint32_t bitrate_kbps) override;
    bool encode(void* native_frame, int64_t timestamp_us,
                std::function<void(const EncodedPacket&)> on_packet) override;
    bool set_bitrate(uint32_t bitrate_kbps) override;
    bool force_keyframe() override;
    void shutdown() override;

private:
    /// Try to open a specific encoder by name. Returns true on success.
    bool try_encoder(const char* encoder_name, uint32_t width, uint32_t height,
                     uint32_t fps, uint32_t bitrate_kbps);

    /// Create the D3D11 staging texture for GPU->CPU readback.
    bool create_staging_texture(uint32_t width, uint32_t height, DXGI_FORMAT format);

    /// Emit SPS/PPS as a config packet via the user callback.
    void emit_config(const uint8_t* data, size_t size, int64_t timestamp_us,
                     const std::function<void(const EncodedPacket&)>& callback);

    // D3D11 device (shared from capturer).
    Microsoft::WRL::ComPtr<ID3D11Device>       device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;

    // Optional shared mutex for D3D11 context thread safety.
    std::mutex* d3d_mutex_ = nullptr;

    // Staging texture for GPU -> CPU copy.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_texture_;

    // FFmpeg codec context.
    AVCodecContext* codec_ctx_ = nullptr;

    // SwsContext for BGRA -> NV12 conversion.
    SwsContext* sws_ctx_ = nullptr;

    // AVFrame for NV12 data to send to encoder.
    AVFrame* nv12_frame_ = nullptr;

    // AVPacket for receiving encoded data.
    AVPacket* pkt_ = nullptr;

    uint32_t width_   = 0;
    uint32_t height_  = 0;
    uint32_t fps_     = 0;
    uint32_t bitrate_kbps_ = 0;
    DXGI_FORMAT source_format_ = DXGI_FORMAT_UNKNOWN;

    std::string encoder_name_;
    std::vector<uint8_t> bgra_scratch_;

    std::atomic<bool> keyframe_pending_{false};
    bool config_sent_ = false;
    int64_t frame_index_ = 0;

    std::mutex encode_mutex_;
};

} // namespace droidscreen
