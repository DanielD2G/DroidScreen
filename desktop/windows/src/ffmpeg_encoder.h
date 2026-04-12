/*
 * DroidScreen Windows - FFmpeg-based H.264 encoder (zero-copy GPU path)
 *
 * Uses libavcodec for H.264 encoding with automatic hardware detection.
 * Tries encoders in order:
 *   1. h264_nvenc  (NVIDIA)
 *   2. h264_qsv    (Intel Quick Sync)
 *   3. h264_amf    (AMD AMF)
 *
 * The encoding pipeline keeps all data on the GPU:
 *   1. GpuColorConverter converts BGRA/FP16 -> NV12 via HLSL shaders (GPU).
 *   2. NV12 texture is copied to the FFmpeg D3D11VA hardware frame pool (GPU).
 *   3. avcodec_send_frame passes the D3D11 texture directly to the
 *      hardware encoder (zero-copy for NVENC/QSV/AMF).
 *
 * This eliminates the CPU readback (Map/Unmap), CPU color conversion
 * (sws_scale), and CPU->GPU DMA that caused 10%+ GPU overhead.
 *
 * Output: H.264 Annex B NAL units via the on_packet callback.
 *
 * Configuration targets ultra-low-latency streaming:
 *   - 0 B-frames, no lookahead
 *   - CBR rate control
 *   - Keyframe every 2 seconds
 */

#pragma once

#include "droidscreen/encoder.h"
#include "gpu_color_converter.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <d3d11.h>
#include <wrl/client.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

namespace droidscreen {

class FFmpegEncoder : public Encoder {
public:
    FFmpegEncoder();
    ~FFmpegEncoder() override;

    /// Set the D3D11 device to use (must be called before init).
    /// If d3d_mutex is non-null, the encoder will lock it around all
    /// D3D11 context calls to prevent races with the WGC capturer's
    /// callback thread.
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

    /// Create FFmpeg D3D11VA hardware device and frames contexts.
    bool create_hw_contexts(uint32_t width, uint32_t height);

    /// Copy the converter's NV12 texture to the FFmpeg pool texture.
    bool copy_nv12_to_pool(ID3D11Texture2D* src_nv12);

    /// Emit SPS/PPS as a config packet via the user callback.
    void emit_config(const uint8_t* data, size_t size, int64_t timestamp_us,
                     const std::function<void(const EncodedPacket&)>& callback);

    // D3D11 device (shared from capturer).
    Microsoft::WRL::ComPtr<ID3D11Device>       device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;

    // Optional shared mutex for D3D11 context thread safety.
    std::mutex* d3d_mutex_ = nullptr;

    // GPU color converter (BGRA/FP16 -> NV12 via shaders).
    std::unique_ptr<GpuColorConverter> gpu_converter_;

    // FFmpeg hardware contexts for zero-copy D3D11 encoding.
    AVBufferRef* hw_device_ctx_ = nullptr;
    AVBufferRef* hw_frames_ctx_ = nullptr;

    // FFmpeg codec context.
    AVCodecContext* codec_ctx_ = nullptr;

    // Reusable AVFrame for D3D11 hardware frames.
    AVFrame* hw_frame_ = nullptr;

    // AVPacket for receiving encoded data.
    AVPacket* pkt_ = nullptr;

    uint32_t width_   = 0;
    uint32_t height_  = 0;
    uint32_t fps_     = 0;
    uint32_t bitrate_kbps_ = 0;

    std::string encoder_name_;

    std::atomic<bool> keyframe_pending_{false};
    bool config_sent_ = false;
    int64_t frame_index_ = 0;

    std::mutex encode_mutex_;
};

} // namespace droidscreen
