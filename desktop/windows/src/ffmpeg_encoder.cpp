/*
 * DroidScreen Windows - FFmpeg encoder implementation (zero-copy GPU path)
 *
 * Uses D3D11VA hardware frames to keep all video data on the GPU:
 *   1. GpuColorConverter: BGRA/FP16 -> NV12 via HLSL pixel shaders (GPU)
 *   2. CopySubresourceRegion: converter NV12 -> FFmpeg pool NV12 (GPU)
 *   3. avcodec_send_frame: pool texture -> hardware encoder (zero-copy)
 *
 * Only the compressed H.264 bitstream ever touches CPU memory.
 * This reduces GPU overhead from 10%+ to ~2-3%, making DroidScreen
 * suitable as a second screen during gaming.
 */

#include "ffmpeg_encoder.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace droidscreen {

// ---------------------------------------------------------------------------
// D3D11VA lock/unlock callbacks for FFmpeg
// ---------------------------------------------------------------------------

namespace {

static void d3d11va_lock(void* lock_ctx) {
    static_cast<std::mutex*>(lock_ctx)->lock();
}

static void d3d11va_unlock(void* lock_ctx) {
    static_cast<std::mutex*>(lock_ctx)->unlock();
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

FFmpegEncoder::FFmpegEncoder() = default;

FFmpegEncoder::~FFmpegEncoder() {
    shutdown();
}

void FFmpegEncoder::set_d3d_device(ID3D11Device* device,
                                    ID3D11DeviceContext* context,
                                    std::mutex* d3d_mutex) {
    device_    = device;
    context_   = context;
    d3d_mutex_ = d3d_mutex;
}

// ---------------------------------------------------------------------------
// FFmpeg D3D11VA hardware contexts
// ---------------------------------------------------------------------------

bool FFmpegEncoder::create_hw_contexts(uint32_t width, uint32_t height) {
    // --- Hardware device context (wraps our existing ID3D11Device) ---
    hw_device_ctx_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!hw_device_ctx_) {
        fprintf(stderr, "[ffmpeg] av_hwdevice_ctx_alloc(D3D11VA) failed\n");
        return false;
    }

    auto* device_ctx = reinterpret_cast<AVHWDeviceContext*>(hw_device_ctx_->data);
    auto* d3d11_device_ctx =
        static_cast<AVD3D11VADeviceContext*>(device_ctx->hwctx);

    // Reuse the D3D11 device from the WGC capturer — no extra device.
    // AddRef because FFmpeg takes ownership and will Release on uninit.
    device_.Get()->AddRef();
    d3d11_device_ctx->device = device_.Get();

    // Provide lock/unlock so FFmpeg serialises its own D3D11 context calls
    // with ours (both share the same immediate context).
    if (d3d_mutex_) {
        d3d11_device_ctx->lock     = d3d11va_lock;
        d3d11_device_ctx->unlock   = d3d11va_unlock;
        d3d11_device_ctx->lock_ctx = d3d_mutex_;
    }

    int ret = av_hwdevice_ctx_init(hw_device_ctx_);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[ffmpeg] av_hwdevice_ctx_init failed: %s\n", errbuf);
        av_buffer_unref(&hw_device_ctx_);
        return false;
    }

    // --- Hardware frames context (NV12 texture pool) ---
    hw_frames_ctx_ = av_hwframe_ctx_alloc(hw_device_ctx_);
    if (!hw_frames_ctx_) {
        fprintf(stderr, "[ffmpeg] av_hwframe_ctx_alloc failed\n");
        av_buffer_unref(&hw_device_ctx_);
        return false;
    }

    auto* frames_ctx = reinterpret_cast<AVHWFramesContext*>(hw_frames_ctx_->data);
    frames_ctx->format    = AV_PIX_FMT_D3D11;
    frames_ctx->sw_format = AV_PIX_FMT_NV12;
    frames_ctx->width     = static_cast<int>(width);
    frames_ctx->height    = static_cast<int>(height);
    // Pool of 4 textures in a texture array.  With zero-latency encoding
    // and no B-frames only 1-2 are in flight, but 4 gives headroom.
    frames_ctx->initial_pool_size = 4;

    auto* d3d11_frames =
        static_cast<AVD3D11VAFramesContext*>(frames_ctx->hwctx);
    // BIND_RENDER_TARGET is required by NVENC for input texture registration
    // and is harmless for QSV/AMF.
    d3d11_frames->BindFlags = D3D11_BIND_RENDER_TARGET;
    d3d11_frames->MiscFlags = 0;

    ret = av_hwframe_ctx_init(hw_frames_ctx_);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[ffmpeg] av_hwframe_ctx_init failed: %s\n", errbuf);
        av_buffer_unref(&hw_frames_ctx_);
        av_buffer_unref(&hw_device_ctx_);
        return false;
    }

    fprintf(stderr, "[ffmpeg] D3D11VA hardware contexts created "
            "(%ux%u NV12, pool=%d)\n", width, height,
            frames_ctx->initial_pool_size);
    return true;
}

// ---------------------------------------------------------------------------
// Encoder probing
// ---------------------------------------------------------------------------

bool FFmpegEncoder::try_encoder(const char* encoder_name,
                                 uint32_t width, uint32_t height,
                                 uint32_t fps, uint32_t bitrate_kbps) {
    const AVCodec* codec = avcodec_find_encoder_by_name(encoder_name);
    if (!codec) {
        fprintf(stderr, "[ffmpeg] encoder '%s' not found\n", encoder_name);
        return false;
    }

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        fprintf(stderr, "[ffmpeg] failed to alloc context for '%s'\n", encoder_name);
        return false;
    }

    ctx->width     = static_cast<int>(width);
    ctx->height    = static_cast<int>(height);
    ctx->time_base = { 1, static_cast<int>(fps) };
    ctx->framerate = { static_cast<int>(fps), 1 };
    ctx->bit_rate  = static_cast<int64_t>(bitrate_kbps) * 1000;

    ctx->gop_size     = static_cast<int>(fps * 2);  // keyframe every 2 seconds
    ctx->max_b_frames = 0;

    // --- D3D11VA hardware frames for zero-copy encoding ---
    ctx->pix_fmt        = AV_PIX_FMT_D3D11;
    ctx->hw_frames_ctx  = av_buffer_ref(hw_frames_ctx_);

    // Encoder-specific options for ultra-low-latency.
    if (strcmp(encoder_name, "h264_nvenc") == 0) {
        av_opt_set(ctx->priv_data, "preset", "p1", 0);
        av_opt_set(ctx->priv_data, "tune",   "ull", 0);
        av_opt_set(ctx->priv_data, "rc",     "cbr", 0);
        av_opt_set(ctx->priv_data, "zerolatency", "1", 0);
    } else if (strcmp(encoder_name, "h264_qsv") == 0) {
        av_opt_set(ctx->priv_data, "preset",   "veryfast", 0);
        av_opt_set(ctx->priv_data, "scenario", "livestreaming", 0);
        ctx->thread_count = 1;
    } else if (strcmp(encoder_name, "h264_amf") == 0) {
        av_opt_set(ctx->priv_data, "usage",   "ultralowlatency", 0);
        av_opt_set(ctx->priv_data, "quality", "speed", 0);
        av_opt_set(ctx->priv_data, "rc",      "cbr", 0);
    }

    int ret = avcodec_open2(ctx, codec, nullptr);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[ffmpeg] failed to open '%s': %s\n", encoder_name, errbuf);
        avcodec_free_context(&ctx);
        return false;
    }

    codec_ctx_ = ctx;
    encoder_name_ = encoder_name;
    fprintf(stderr, "[ffmpeg] opened encoder: %s (D3D11VA zero-copy)\n",
            encoder_name);
    return true;
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

bool FFmpegEncoder::init(uint32_t width, uint32_t height,
                          uint32_t fps, uint32_t bitrate_kbps) {
    // Clean up any existing resources from a previous init() call.
    if (codec_ctx_) {
        fprintf(stderr, "[ffmpeg] re-init: closing previous encoder session\n");
        shutdown();
    }

    width_        = width;
    height_       = height;
    fps_          = fps;
    bitrate_kbps_ = bitrate_kbps;

    if (!device_ || !context_) {
        fprintf(stderr, "[ffmpeg] D3D11 device/context not set\n");
        return false;
    }

    // Step 1: Create FFmpeg D3D11VA hardware contexts.
    if (!create_hw_contexts(width, height)) {
        fprintf(stderr, "[ffmpeg] D3D11VA hardware context creation failed\n");
        return false;
    }

    // Step 2: Create GPU color converter (HLSL shaders).
    gpu_converter_ = std::make_unique<GpuColorConverter>();
    if (!gpu_converter_->init(device_.Get(), context_.Get(), width, height)) {
        fprintf(stderr, "[ffmpeg] GPU color converter init failed\n");
        av_buffer_unref(&hw_frames_ctx_);
        av_buffer_unref(&hw_device_ctx_);
        gpu_converter_.reset();
        return false;
    }

    // Step 3: Try hardware encoders in order.
    static const char* encoder_names[] = {
        "h264_nvenc",
        "h264_qsv",
        "h264_amf",
    };

    bool opened = false;
    for (const char* name : encoder_names) {
        fprintf(stderr, "[ffmpeg] trying encoder: %s\n", name);
        if (try_encoder(name, width, height, fps, bitrate_kbps)) {
            opened = true;
            break;
        }
    }

    if (!opened) {
        fprintf(stderr, "[ffmpeg] no suitable hardware H.264 encoder found\n");
        gpu_converter_.reset();
        av_buffer_unref(&hw_frames_ctx_);
        av_buffer_unref(&hw_device_ctx_);
        return false;
    }

    // Step 4: Allocate reusable AVFrame and AVPacket.
    hw_frame_ = av_frame_alloc();
    if (!hw_frame_) {
        fprintf(stderr, "[ffmpeg] av_frame_alloc failed\n");
        avcodec_free_context(&codec_ctx_);
        gpu_converter_.reset();
        av_buffer_unref(&hw_frames_ctx_);
        av_buffer_unref(&hw_device_ctx_);
        return false;
    }

    pkt_ = av_packet_alloc();
    if (!pkt_) {
        fprintf(stderr, "[ffmpeg] av_packet_alloc failed\n");
        av_frame_free(&hw_frame_);
        avcodec_free_context(&codec_ctx_);
        gpu_converter_.reset();
        av_buffer_unref(&hw_frames_ctx_);
        av_buffer_unref(&hw_device_ctx_);
        return false;
    }

    config_sent_ = false;
    keyframe_pending_.store(false);
    frame_index_ = 0;

    fprintf(stderr, "[ffmpeg] encoder initialized: %ux%u@%u, %u kbps "
            "(using %s, zero-copy GPU path)\n",
            width, height, fps, bitrate_kbps, encoder_name_.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// NV12 copy to FFmpeg pool texture
// ---------------------------------------------------------------------------

bool FFmpegEncoder::copy_nv12_to_pool(ID3D11Texture2D* src_nv12) {
    // Get the pool texture and array index from the AVFrame.
    auto* pool_tex = reinterpret_cast<ID3D11Texture2D*>(hw_frame_->data[0]);
    auto  pool_idx = static_cast<UINT>(reinterpret_cast<intptr_t>(hw_frame_->data[1]));

    D3D11_TEXTURE2D_DESC pool_desc = {};
    pool_tex->GetDesc(&pool_desc);

    // NV12 subresource layout for texture arrays:
    //   Y  plane of slice i:  i
    //   UV plane of slice i:  i + ArraySize
    // (formula: mipSlice + arraySlice*mipLevels + planeSlice*mipLevels*arraySize)
    UINT dst_y_sub  = pool_idx;
    UINT dst_uv_sub = pool_idx + pool_desc.ArraySize;

    // Our converter's NV12 is a single texture (ArraySize=1):
    //   Y  plane: subresource 0
    //   UV plane: subresource 1
    context_->CopySubresourceRegion(pool_tex, dst_y_sub,  0, 0, 0,
                                     src_nv12, 0, nullptr);
    context_->CopySubresourceRegion(pool_tex, dst_uv_sub, 0, 0, 0,
                                     src_nv12, 1, nullptr);
    return true;
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

bool FFmpegEncoder::encode(void* native_frame, int64_t timestamp_us,
                            std::function<void(const EncodedPacket&)> on_packet) {
    if (!codec_ctx_) return false;

    std::lock_guard<std::mutex> lock(encode_mutex_);

    auto* source_texture = static_cast<ID3D11Texture2D*>(native_frame);
    if (!source_texture) return false;

    // Verify source texture dimensions and format.
    D3D11_TEXTURE2D_DESC src_desc = {};
    source_texture->GetDesc(&src_desc);

    if (frame_index_ == 0) {
        fprintf(stderr, "[ffmpeg] first frame: src texture %ux%u fmt=%u, "
                "encoder %ux%u\n",
                src_desc.Width, src_desc.Height, src_desc.Format,
                width_, height_);
    }

    if (src_desc.Width != width_ || src_desc.Height != height_) {
        static bool warned = false;
        if (!warned) {
            fprintf(stderr, "[ffmpeg] WARNING: source texture %ux%u != "
                    "encoder %ux%u — skipping frame\n",
                    src_desc.Width, src_desc.Height, width_, height_);
            warned = true;
        }
        return false;
    }

    if (src_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        src_desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
        fprintf(stderr, "[ffmpeg] unsupported source texture format %u\n",
                src_desc.Format);
        return false;
    }

    // Step 1: Get a pool texture from FFmpeg's D3D11VA frame pool.
    av_frame_unref(hw_frame_);
    int ret = av_hwframe_get_buffer(hw_frames_ctx_, hw_frame_, 0);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[ffmpeg] av_hwframe_get_buffer failed: %s\n", errbuf);
        return false;
    }

    // Step 2: Convert BGRA/FP16 -> NV12 on GPU, then copy to pool.
    // Lock the D3D11 context for our shader draws + copy.
    {
        std::unique_lock<std::mutex> d3d_lock;
        if (d3d_mutex_) {
            d3d_lock = std::unique_lock<std::mutex>(*d3d_mutex_);
        }

        ID3D11Texture2D* nv12 = gpu_converter_->convert(
            source_texture, src_desc.Format);
        if (!nv12) {
            fprintf(stderr, "[ffmpeg] GPU color conversion failed\n");
            return false;
        }

        if (!copy_nv12_to_pool(nv12)) {
            fprintf(stderr, "[ffmpeg] NV12 copy to pool failed\n");
            return false;
        }
    }
    // D3D11 context lock released — FFmpeg may acquire it internally.

    // Step 3: Set frame properties.
    hw_frame_->pts = frame_index_++;

    if (keyframe_pending_.exchange(false)) {
        hw_frame_->pict_type = AV_PICTURE_TYPE_I;
        hw_frame_->key_frame = 1;
    } else {
        hw_frame_->pict_type = AV_PICTURE_TYPE_NONE;
        hw_frame_->key_frame = 0;
    }

    // Step 4: Send frame to encoder (zero-copy — encoder reads the D3D11
    // texture directly from the pool).
    ret = avcodec_send_frame(codec_ctx_, hw_frame_);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[ffmpeg] avcodec_send_frame failed: %s\n", errbuf);
        return false;
    }

    // Step 5: Receive encoded packets.
    while (true) {
        ret = avcodec_receive_packet(codec_ctx_, pkt_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
            av_strerror(ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "[ffmpeg] avcodec_receive_packet failed: %s\n",
                    errbuf);
            return false;
        }

        bool is_keyframe = (pkt_->flags & AV_PKT_FLAG_KEY) != 0;

        // On the first keyframe, emit SPS/PPS as a config packet.
        if (is_keyframe && !config_sent_) {
            emit_config(pkt_->data, pkt_->size, timestamp_us, on_packet);
        }

        // Emit the encoded frame data.
        if (on_packet) {
            EncodedPacket epkt;
            epkt.data         = pkt_->data;
            epkt.size         = static_cast<size_t>(pkt_->size);
            epkt.is_keyframe  = is_keyframe;
            epkt.is_config    = false;
            epkt.timestamp_us = timestamp_us;
            on_packet(epkt);
        }

        av_packet_unref(pkt_);
    }

    return true;
}

// ---------------------------------------------------------------------------
// SPS/PPS extraction
// ---------------------------------------------------------------------------

void FFmpegEncoder::emit_config(
        const uint8_t* data, size_t size, int64_t timestamp_us,
        const std::function<void(const EncodedPacket&)>& callback) {
    /*
     * Extract SPS and PPS NAL units from the first keyframe.
     *
     * Different encoders produce different NAL orderings:
     *   h264_nvenc:  [AUD(9)] [SPS(7)] [PPS(8)] [SEI(6)] [IDR(5)]
     *   h264_qsv:   [AUD(9)] [SPS(7)] [PPS(8)] [IDR(5)]
     *   h264_amf:    [SPS(7)] [PPS(8)] [SEI(6)] [IDR(5)]
     *
     * We extract from the first SPS start code up to the first VCL NAL
     * (types 1-5 = coded slice data).
     */
    size_t first_sps_pos = SIZE_MAX;
    size_t first_vcl_pos = SIZE_MAX;
    bool   found_sps = false;
    bool   found_pps = false;

    size_t i = 0;
    while (i + 3 < size) {
        bool is_4byte = (i + 4 <= size &&
                         data[i] == 0 && data[i+1] == 0 &&
                         data[i+2] == 0 && data[i+3] == 1);
        bool is_3byte = (!is_4byte &&
                         data[i] == 0 && data[i+1] == 0 &&
                         data[i+2] == 1);

        if (is_4byte || is_3byte) {
            size_t sc_len   = is_4byte ? 4 : 3;
            size_t nal_hdr  = i + sc_len;
            if (nal_hdr >= size) break;

            uint8_t nal_type = data[nal_hdr] & 0x1F;

            if (nal_type == 7 && first_sps_pos == SIZE_MAX) {
                first_sps_pos = i;
                found_sps = true;
            }
            if (nal_type == 8) {
                found_pps = true;
            }

            if (nal_type >= 1 && nal_type <= 5) {
                first_vcl_pos = i;
                break;
            }

            i = nal_hdr + 1;
            continue;
        }
        i++;
    }

    if (first_sps_pos != SIZE_MAX && first_vcl_pos > first_sps_pos &&
        found_sps && found_pps && callback) {
        size_t config_size = first_vcl_pos - first_sps_pos;

        fprintf(stderr, "[ffmpeg] config packet: %zu bytes "
                "(SPS@%zu, VCL@%zu)\n",
                config_size, first_sps_pos, first_vcl_pos);

        EncodedPacket pkt;
        pkt.data         = data + first_sps_pos;
        pkt.size         = config_size;
        pkt.is_keyframe  = false;
        pkt.is_config    = true;
        pkt.timestamp_us = timestamp_us;
        callback(pkt);
        config_sent_ = true;
    } else {
        fprintf(stderr, "[ffmpeg] WARNING: could not extract config from "
                "keyframe (sps=%d pps=%d sps_pos=%zu vcl_pos=%zu "
                "size=%zu)\n",
                found_sps, found_pps,
                first_sps_pos, first_vcl_pos, size);
    }
}

// ---------------------------------------------------------------------------
// Dynamic reconfiguration
// ---------------------------------------------------------------------------

bool FFmpegEncoder::set_bitrate(uint32_t bitrate_kbps) {
    if (!codec_ctx_) return false;

    std::lock_guard<std::mutex> lock(encode_mutex_);

    bitrate_kbps_ = bitrate_kbps;
    codec_ctx_->bit_rate = static_cast<int64_t>(bitrate_kbps) * 1000;

    fprintf(stderr, "[ffmpeg] bitrate set to %u kbps\n", bitrate_kbps);
    return true;
}

bool FFmpegEncoder::force_keyframe() {
    keyframe_pending_.store(true);
    return true;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void FFmpegEncoder::shutdown() {
    if (pkt_) {
        av_packet_free(&pkt_);
        pkt_ = nullptr;
    }

    if (hw_frame_) {
        av_frame_free(&hw_frame_);
        hw_frame_ = nullptr;
    }

    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }

    if (gpu_converter_) {
        gpu_converter_->shutdown();
        gpu_converter_.reset();
    }

    if (hw_frames_ctx_) {
        av_buffer_unref(&hw_frames_ctx_);
        hw_frames_ctx_ = nullptr;
    }

    if (hw_device_ctx_) {
        av_buffer_unref(&hw_device_ctx_);
        hw_device_ctx_ = nullptr;
    }

    // NOTE: do NOT reset device_ / context_ here — they are borrowed
    // from the capturer via set_d3d_device() and must survive re-init.

    encoder_name_.clear();
    config_sent_ = false;
    frame_index_ = 0;

    fprintf(stderr, "[ffmpeg] encoder shut down\n");
}

} // namespace droidscreen
