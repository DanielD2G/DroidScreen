/*
 * DroidScreen Windows - FFmpeg encoder implementation
 *
 * Uses libavcodec to encode H.264 with automatic hardware/software fallback.
 *
 * Pipeline per encode() call:
 *   1. Copy the BGRA ID3D11Texture2D to a staging texture (GPU -> CPU).
 *   2. Map the staging texture to get CPU-accessible pixel data.
 *   3. Use sws_scale() to convert BGRA -> NV12.
 *   4. Send the NV12 frame to libavcodec.
 *   5. Receive encoded packets and emit via callback.
 *
 * The output is in Annex B format (FFmpeg produces Annex B by default
 * when AV_CODEC_FLAG_GLOBAL_HEADER is not set).
 */

#include "ffmpeg_encoder.h"

#include <cstdio>
#include <cstring>

namespace droidscreen {

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
// Staging texture creation
// ---------------------------------------------------------------------------

bool FFmpegEncoder::create_staging_texture(uint32_t width, uint32_t height) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width            = width;
    desc.Height           = height;
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
    desc.BindFlags        = 0;

    HRESULT hr = device_->CreateTexture2D(&desc, nullptr,
                                           staging_texture_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        fprintf(stderr, "[ffmpeg] CreateTexture2D (staging) failed: 0x%08lx\n", hr);
        return false;
    }

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
    ctx->pix_fmt   = AV_PIX_FMT_NV12;
    ctx->bit_rate  = static_cast<int64_t>(bitrate_kbps) * 1000;

    ctx->gop_size     = static_cast<int>(fps * 2);  // keyframe every 2 seconds
    ctx->max_b_frames = 0;

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
    } else if (strcmp(encoder_name, "libx264") == 0) {
        av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(ctx->priv_data, "tune",   "zerolatency", 0);
        ctx->thread_count = 1;
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
    fprintf(stderr, "[ffmpeg] opened encoder: %s\n", encoder_name);
    return true;
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

bool FFmpegEncoder::init(uint32_t width, uint32_t height,
                          uint32_t fps, uint32_t bitrate_kbps) {
    // Clean up any existing resources from a previous init() call.
    // This prevents resource leaks when Pipeline::start() re-inits
    // an already-initialized encoder.
    if (codec_ctx_) {
        fprintf(stderr, "[ffmpeg] re-init: closing previous encoder session\n");
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
    if (nv12_frame_) {
        av_frame_free(&nv12_frame_);
        nv12_frame_ = nullptr;
    }
    if (pkt_) {
        av_packet_free(&pkt_);
        pkt_ = nullptr;
    }
    // staging_texture_ is replaced below via ComPtr assignment.

    width_        = width;
    height_       = height;
    fps_          = fps;
    bitrate_kbps_ = bitrate_kbps;

    if (!device_ || !context_) {
        fprintf(stderr, "[ffmpeg] D3D11 device/context not set\n");
        return false;
    }

    // Create staging texture for GPU -> CPU readback.
    if (!create_staging_texture(width, height)) {
        return false;
    }

    // Try hardware encoders in order, then fall back to software.
    static const char* encoder_names[] = {
        "h264_nvenc",
        "h264_qsv",
        "h264_amf",
        "libx264",
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
        fprintf(stderr, "[ffmpeg] no suitable H.264 encoder found\n");
        staging_texture_.Reset();
        return false;
    }

    // Create SwsContext for BGRA -> NV12 conversion.
    sws_ctx_ = sws_getContext(
        static_cast<int>(width), static_cast<int>(height), AV_PIX_FMT_BGRA,
        static_cast<int>(width), static_cast<int>(height), AV_PIX_FMT_NV12,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!sws_ctx_) {
        fprintf(stderr, "[ffmpeg] sws_getContext failed\n");
        avcodec_free_context(&codec_ctx_);
        staging_texture_.Reset();
        return false;
    }

    // Allocate the NV12 output frame.
    nv12_frame_ = av_frame_alloc();
    if (!nv12_frame_) {
        fprintf(stderr, "[ffmpeg] av_frame_alloc failed\n");
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
        avcodec_free_context(&codec_ctx_);
        staging_texture_.Reset();
        return false;
    }

    nv12_frame_->format = AV_PIX_FMT_NV12;
    nv12_frame_->width  = static_cast<int>(width);
    nv12_frame_->height = static_cast<int>(height);

    int ret = av_frame_get_buffer(nv12_frame_, 32);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[ffmpeg] av_frame_get_buffer failed: %s\n", errbuf);
        av_frame_free(&nv12_frame_);
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
        avcodec_free_context(&codec_ctx_);
        staging_texture_.Reset();
        return false;
    }

    // Allocate a reusable packet.
    pkt_ = av_packet_alloc();
    if (!pkt_) {
        fprintf(stderr, "[ffmpeg] av_packet_alloc failed\n");
        av_frame_free(&nv12_frame_);
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
        avcodec_free_context(&codec_ctx_);
        staging_texture_.Reset();
        return false;
    }

    config_sent_ = false;
    keyframe_pending_.store(false);
    frame_index_ = 0;

    fprintf(stderr, "[ffmpeg] encoder initialized: %ux%u@%u, %u kbps (using %s)\n",
            width, height, fps, bitrate_kbps, encoder_name_.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

bool FFmpegEncoder::encode(void* native_frame, int64_t timestamp_us,
                            std::function<void(const EncodedPacket&)> on_packet) {
    if (!codec_ctx_) return false;

    std::lock_guard<std::mutex> lock(encode_mutex_);

    // The native_frame is an ID3D11Texture2D* in BGRA format.
    auto* bgra_texture = static_cast<ID3D11Texture2D*>(native_frame);
    if (!bgra_texture) return false;

    // Verify source texture dimensions and format.
    D3D11_TEXTURE2D_DESC src_desc = {};
    bgra_texture->GetDesc(&src_desc);

    // Log diagnostics on the first frame.
    if (frame_index_ == 0) {
        fprintf(stderr, "[ffmpeg] first frame: src texture %ux%u fmt=%u, "
                "encoder %ux%u\n",
                src_desc.Width, src_desc.Height, src_desc.Format,
                width_, height_);
    }

    if (src_desc.Width != width_ || src_desc.Height != height_) {
        // Log once then skip — resolution mismatch causes corruption.
        static bool warned = false;
        if (!warned) {
            fprintf(stderr, "[ffmpeg] WARNING: source texture %ux%u != "
                    "encoder %ux%u — skipping frame\n",
                    src_desc.Width, src_desc.Height, width_, height_);
            warned = true;
        }
        return false;
    }

    if (src_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        static bool format_warned = false;
        if (!format_warned) {
            fprintf(stderr, "[ffmpeg] WARNING: source texture format %u != "
                    "expected B8G8R8A8_UNORM (87) — colors may be wrong\n",
                    src_desc.Format);
            format_warned = true;
        }
    }

    // Lock the D3D11 context mutex to prevent races with the WGC
    // capturer's callback thread (both share the same immediate context).
    // Scope covers CopyResource + Map + sws_scale + Unmap since the
    // mapped pointer is only valid while the map is held.
    std::unique_lock<std::mutex> d3d_lock;
    if (d3d_mutex_) {
        d3d_lock = std::unique_lock<std::mutex>(*d3d_mutex_);
    }

    // Step 1: Copy the GPU texture to the staging texture.
    context_->CopyResource(staging_texture_.Get(), bgra_texture);

    // Step 2: Map the staging texture for CPU read.
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = context_->Map(staging_texture_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        fprintf(stderr, "[ffmpeg] Map staging texture failed: 0x%08lx\n", hr);
        return false;
    }

    // Log stride info on first frame.
    if (frame_index_ == 0) {
        fprintf(stderr, "[ffmpeg] first frame: mapped RowPitch=%u "
                "(expected min %u), NV12 linesize[0]=%d linesize[1]=%d\n",
                mapped.RowPitch, width_ * 4,
                nv12_frame_->linesize[0], nv12_frame_->linesize[1]);
    }

    // Step 3: Convert BGRA -> NV12 via sws_scale.
    const uint8_t* src_data[1] = { static_cast<const uint8_t*>(mapped.pData) };
    int src_linesize[1] = { static_cast<int>(mapped.RowPitch) };

    // Make the frame writable (in case it is referenced by the encoder).
    av_frame_make_writable(nv12_frame_);

    sws_scale(sws_ctx_,
              src_data, src_linesize,
              0, static_cast<int>(height_),
              nv12_frame_->data, nv12_frame_->linesize);

    // Step 4: Unmap the staging texture.
    context_->Unmap(staging_texture_.Get(), 0);

    // Release D3D context lock — the rest is CPU-only work.
    if (d3d_lock.owns_lock()) {
        d3d_lock.unlock();
    }

    // Step 5: Set frame properties.
    nv12_frame_->pts = frame_index_++;

    // Force keyframe if requested.
    if (keyframe_pending_.exchange(false)) {
        nv12_frame_->pict_type = AV_PICTURE_TYPE_I;
        nv12_frame_->key_frame = 1;
    } else {
        nv12_frame_->pict_type = AV_PICTURE_TYPE_NONE;
        nv12_frame_->key_frame = 0;
    }

    // Step 6: Send frame to encoder.
    int ret = avcodec_send_frame(codec_ctx_, nv12_frame_);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[ffmpeg] avcodec_send_frame failed: %s\n", errbuf);
        return false;
    }

    // Step 7: Receive encoded packets.
    while (true) {
        ret = avcodec_receive_packet(codec_ctx_, pkt_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
            av_strerror(ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "[ffmpeg] avcodec_receive_packet failed: %s\n", errbuf);
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

void FFmpegEncoder::emit_config(
        const uint8_t* data, size_t size, int64_t timestamp_us,
        const std::function<void(const EncodedPacket&)>& callback) {
    /*
     * Extract SPS and PPS NAL units from the first keyframe.
     *
     * Different encoders produce different NAL orderings:
     *   h264_nvenc:  [AUD(9)] [SPS(7)] [PPS(8)] [SEI(6)] [IDR(5)]
     *   libx264:     [SPS(7)] [PPS(8)] [IDR(5)]
     *   h264_qsv:   [AUD(9)] [SPS(7)] [PPS(8)] [IDR(5)]
     *   h264_amf:    [SPS(7)] [PPS(8)] [SEI(6)] [IDR(5)]
     *
     * We extract from the first SPS start code up to the first VCL NAL
     * (types 1-5 = coded slice data). This cleanly skips any AUD or SEI
     * before the SPS, and includes SPS + PPS (+ any non-VCL NALs between
     * them, which is rare but harmless).
     *
     * NAL unit types (nal_unit_type = byte after start code & 0x1F):
     *   1-5 = VCL (coded slice / IDR slice)
     *   6   = SEI
     *   7   = SPS
     *   8   = PPS
     *   9   = AUD (Access Unit Delimiter)
     */
    size_t first_sps_pos = SIZE_MAX;  // byte offset of SPS start code
    size_t first_vcl_pos = SIZE_MAX;  // byte offset of first VCL start code
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
                first_sps_pos = i;  // config starts here
                found_sps = true;
            }
            if (nal_type == 8) {
                found_pps = true;
            }

            // VCL NALs (coded slice types 1-5) mark the end of config.
            if (nal_type >= 1 && nal_type <= 5) {
                first_vcl_pos = i;
                break;
            }

            // Skip past the start code to continue scanning.
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

    // Note: For most FFmpeg encoders, changing bit_rate mid-stream has
    // limited effect without a full reinit. For NVENC/QSV/AMF the
    // underlying SDK may pick up the change. For libx264, it typically
    // requires a reinit. We do our best here.
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

    if (nv12_frame_) {
        av_frame_free(&nv12_frame_);
        nv12_frame_ = nullptr;
    }

    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }

    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }

    staging_texture_.Reset();
    context_.Reset();
    device_.Reset();

    encoder_name_.clear();
    config_sent_ = false;
    frame_index_ = 0;

    fprintf(stderr, "[ffmpeg] encoder shut down\n");
}

} // namespace droidscreen
