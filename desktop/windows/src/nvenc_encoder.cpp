/*
 * DroidScreen Windows - NVENC encoder implementation
 *
 * Dynamically loads nvEncodeAPI64.dll and uses the NV_ENCODE_API_FUNCTION_LIST
 * to drive hardware H.264 encoding on NVIDIA GPUs.
 *
 * Pipeline per encode() call:
 *   1. Color-convert the BGRA ID3D11Texture2D to NV12 via ColorConverter.
 *   2. Map the registered NV12 texture as NVENC input.
 *   3. Call nvEncEncodePicture.
 *   4. Lock the bitstream output, extract NAL units, emit via callback.
 *   5. Unlock and unmap.
 *
 * The output is already in Annex B format (NVENC produces it natively
 * when repeatSPSPPS is enabled and inline headers are requested).
 */

#include "nvenc_encoder.h"

#include <cstdio>
#include <cstring>

namespace droidscreen {

NvencEncoder::NvencEncoder() = default;

NvencEncoder::~NvencEncoder() {
    shutdown();
}

void NvencEncoder::set_d3d_device(ID3D11Device* device,
                                   ID3D11DeviceContext* context) {
    device_  = device;
    context_ = context;
    owns_device_ = false;
}

// ---------------------------------------------------------------------------
// Library loading
// ---------------------------------------------------------------------------

bool NvencEncoder::load_nvenc_library() {
    // Try the 64-bit DLL first.
    nvenc_lib_ = LoadLibraryA("nvEncodeAPI64.dll");
    if (!nvenc_lib_) {
        // Try the 32-bit DLL as a fallback (unlikely on 64-bit builds).
        nvenc_lib_ = LoadLibraryA("nvEncodeAPI.dll");
    }
    if (!nvenc_lib_) {
        fprintf(stderr, "[nvenc] nvEncodeAPI64.dll not found -- "
                "NVENC not available\n");
        return false;
    }

    auto create_instance = reinterpret_cast<NvEncodeAPICreateInstance_t>(
        GetProcAddress(nvenc_lib_, "NvEncodeAPICreateInstance"));
    if (!create_instance) {
        fprintf(stderr, "[nvenc] NvEncodeAPICreateInstance not found\n");
        FreeLibrary(nvenc_lib_);
        nvenc_lib_ = nullptr;
        return false;
    }

    memset(&nvenc_, 0, sizeof(nvenc_));
    nvenc_.version = NV_ENCODE_API_FUNCTION_LIST_VER;

    fprintf(stderr, "[nvenc] requesting API version %d.%d (ver=0x%x)\n",
            NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION,
            (unsigned)nvenc_.version);

    NVENCSTATUS status = create_instance(&nvenc_);
    if (status != NV_ENC_SUCCESS) {
        fprintf(stderr, "[nvenc] NvEncodeAPICreateInstance failed: %d "
                "(15=INVALID_VERSION: driver too old for API %d.%d, "
                "update NVIDIA drivers)\n",
                (int)status, NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
        FreeLibrary(nvenc_lib_);
        nvenc_lib_ = nullptr;
        return false;
    }

    fprintf(stderr, "[nvenc] API %d.%d loaded successfully\n",
            NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
    return true;
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

bool NvencEncoder::init(uint32_t width, uint32_t height,
                         uint32_t fps, uint32_t bitrate_kbps) {
    width_        = width;
    height_       = height;
    fps_          = fps;
    bitrate_kbps_ = bitrate_kbps;

    // Load the NVENC library.
    if (!load_nvenc_library()) {
        return false;
    }

    // Create a D3D11 device if one was not provided externally.
    if (!device_) {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL feature_levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };

        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            flags, feature_levels, _countof(feature_levels),
            D3D11_SDK_VERSION,
            device_.ReleaseAndGetAddressOf(),
            nullptr,
            context_.ReleaseAndGetAddressOf());

        if (FAILED(hr)) {
            fprintf(stderr, "[nvenc] D3D11CreateDevice failed: 0x%08lx\n", hr);
            return false;
        }
        owns_device_ = true;
    }

    // Open an NVENC encode session.
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS session_params = {};
    session_params.version    = NVENC_STRUCT_VERSION(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS, 1);
    session_params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    session_params.device     = device_.Get();
    session_params.apiVersion = NVENCAPI_VERSION;

    NVENCSTATUS status = nvenc_.nvEncOpenEncodeSessionEx(&session_params, &encoder_);
    if (status != NV_ENC_SUCCESS) {
        fprintf(stderr, "[nvenc] nvEncOpenEncodeSessionEx failed: %d\n",
                (int)status);
        return false;
    }

    // Get the preset configuration for P1 + ultra-low-latency.
    NV_ENC_PRESET_CONFIG preset_config = {};
    preset_config.version = NVENC_STRUCT_VERSION(NV_ENC_PRESET_CONFIG, 1);
    preset_config.presetCfg.version = NVENC_STRUCT_VERSION(NV_ENC_CONFIG, 1);

    status = nvenc_.nvEncGetEncodePresetConfigEx(
        encoder_,
        NV_ENC_CODEC_H264_GUID,
        NV_ENC_PRESET_P1_GUID,
        NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY,
        &preset_config);

    if (status != NV_ENC_SUCCESS) {
        fprintf(stderr, "[nvenc] nvEncGetEncodePresetConfigEx failed: %d\n",
                (int)status);
        nvenc_.nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
        return false;
    }

    // Copy the preset config and customize.
    encode_config_ = preset_config.presetCfg;
    encode_config_.version = NVENC_STRUCT_VERSION(NV_ENC_CONFIG, 1);

    // GOP and frame structure.
    encode_config_.gopLength     = fps * 2;  // keyframe every 2 seconds
    encode_config_.frameIntervalP = 1;       // no B-frames (P=1 means 0 B-frames)

    // Profile: Main (good balance of compression and speed).
    encode_config_.profileGUID = NV_ENC_H264_PROFILE_MAIN_GUID;

    // Rate control: CBR at the requested bitrate.
    encode_config_.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    encode_config_.rcParams.averageBitRate  = bitrate_kbps * 1000;
    encode_config_.rcParams.maxBitRate      = bitrate_kbps * 1000;
    encode_config_.rcParams.vbvBufferSize   = (bitrate_kbps * 1000) / fps;
    encode_config_.rcParams.vbvInitialDelay = (bitrate_kbps * 1000) / fps;
    encode_config_.rcParams.enableLookahead = 0;  // no lookahead
    encode_config_.rcParams.zeroReorderDelay = 1;
    encode_config_.rcParams.lowDelayKeyFrameScale = 1;

    // H.264-specific settings.
    auto& h264 = encode_config_.encodeCodecConfig.h264Config;
    h264.idrPeriod       = encode_config_.gopLength;
    h264.repeatSPSPPS    = 1;  // repeat SPS/PPS with every IDR
    h264.disableSPSPPS   = 0;
    h264.sliceMode       = 0;
    h264.sliceModeData   = 0;
    h264.maxNumRefFrames = 1;  // minimum reference frames for low latency
    h264.entropyCodingMode = 1;  // CABAC for better compression

    // Initialize the encoder.
    memset(&init_params_, 0, sizeof(init_params_));
    init_params_.version       = NVENC_STRUCT_VERSION(NV_ENC_INITIALIZE_PARAMS, 1);
    init_params_.encodeGUID    = NV_ENC_CODEC_H264_GUID;
    init_params_.presetGUID    = NV_ENC_PRESET_P1_GUID;
    init_params_.encodeWidth   = width;
    init_params_.encodeHeight  = height;
    init_params_.darWidth      = width;
    init_params_.darHeight     = height;
    init_params_.frameRateNum  = fps;
    init_params_.frameRateDen  = 1;
    init_params_.enablePTD     = 1;  // picture type decision by encoder
    init_params_.encodeConfig  = &encode_config_;
    init_params_.maxEncodeWidth  = width;
    init_params_.maxEncodeHeight = height;
    init_params_.tuningInfo    = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;

    status = nvenc_.nvEncInitializeEncoder(encoder_, &init_params_);
    if (status != NV_ENC_SUCCESS) {
        fprintf(stderr, "[nvenc] nvEncInitializeEncoder failed: %d\n",
                (int)status);
        nvenc_.nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
        return false;
    }

    // Initialize the color converter (BGRA -> NV12).
    if (!color_converter_.init(device_.Get(), width, height)) {
        fprintf(stderr, "[nvenc] color converter init failed\n");
        nvenc_.nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
        return false;
    }

    // Register the NV12 output texture from the color converter with NVENC.
    ID3D11Texture2D* nv12_texture = color_converter_.nv12_texture();
    if (!nv12_texture) {
        fprintf(stderr, "[nvenc] color converter NV12 texture is null\n");
        nvenc_.nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
        return false;
    }

    NV_ENC_REGISTER_RESOURCE reg = {};
    reg.version           = NVENC_STRUCT_VERSION(NV_ENC_REGISTER_RESOURCE, 1);
    reg.resourceType      = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    reg.width             = width;
    reg.height            = height;
    reg.pitch             = 0;  // determined by D3D11
    reg.resourceToRegister = nv12_texture;
    reg.bufferFormat      = NV_ENC_BUFFER_FORMAT_NV12;
    reg.bufferUsage       = 0;  // encoder input

    status = nvenc_.nvEncRegisterResource(encoder_, &reg);
    if (status != NV_ENC_SUCCESS) {
        fprintf(stderr, "[nvenc] nvEncRegisterResource failed: %d\n",
                (int)status);
        nvenc_.nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
        return false;
    }
    registered_resource_ = reg.registeredResource;

    // Create a bitstream output buffer.
    NV_ENC_CREATE_BITSTREAM_BUFFER bsbuf = {};
    bsbuf.version = NVENC_STRUCT_VERSION(NV_ENC_CREATE_BITSTREAM_BUFFER, 1);

    status = nvenc_.nvEncCreateBitstreamBuffer(encoder_, &bsbuf);
    if (status != NV_ENC_SUCCESS) {
        fprintf(stderr, "[nvenc] nvEncCreateBitstreamBuffer failed: %d\n",
                (int)status);
        nvenc_.nvEncUnregisterResource(encoder_, registered_resource_);
        registered_resource_ = nullptr;
        nvenc_.nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
        return false;
    }
    bitstream_buffer_ = bsbuf.bitstreamBuffer;

    config_sent_ = false;
    keyframe_pending_.store(false);

    fprintf(stderr, "[nvenc] encoder initialized: %ux%u@%u, %u kbps\n",
            width, height, fps, bitrate_kbps);
    return true;
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

bool NvencEncoder::encode(void* native_frame, int64_t timestamp_us,
                           std::function<void(const EncodedPacket&)> on_packet) {
    if (!encoder_) return false;

    std::lock_guard<std::mutex> lock(encode_mutex_);

    // The native_frame is an ID3D11Texture2D* in BGRA format.
    auto* bgra_texture = static_cast<ID3D11Texture2D*>(native_frame);
    if (!bgra_texture) return false;

    // Step 1: Convert BGRA to NV12.
    ID3D11Texture2D* nv12_out = nullptr;
    if (!color_converter_.convert(bgra_texture, &nv12_out)) {
        fprintf(stderr, "[nvenc] color conversion failed\n");
        return false;
    }

    // Step 2: Map the registered resource.
    NV_ENC_MAP_INPUT_RESOURCE map_res = {};
    map_res.version       = NVENC_STRUCT_VERSION(NV_ENC_MAP_INPUT_RESOURCE, 1);
    map_res.inputResource = registered_resource_;

    NVENCSTATUS status = nvenc_.nvEncMapInputResource(encoder_, &map_res);
    if (status != NV_ENC_SUCCESS) {
        fprintf(stderr, "[nvenc] nvEncMapInputResource failed: %d\n",
                (int)status);
        return false;
    }

    // Step 3: Encode.
    NV_ENC_PIC_PARAMS pic = {};
    pic.version        = NVENC_STRUCT_VERSION(NV_ENC_PIC_PARAMS, 1);
    pic.inputWidth     = width_;
    pic.inputHeight    = height_;
    pic.inputPitch     = 0;
    pic.inputBuffer    = map_res.mappedResource;
    pic.outputBitstream = bitstream_buffer_;
    pic.bufferFmt      = NV_ENC_BUFFER_FORMAT_NV12;
    pic.pictureStruct  = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputTimeStamp = static_cast<uint64_t>(timestamp_us);

    // Force IDR if requested.
    if (keyframe_pending_.exchange(false)) {
        pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;
    }

    status = nvenc_.nvEncEncodePicture(encoder_, &pic);
    if (status != NV_ENC_SUCCESS && status != NV_ENC_ERR_NEED_MORE_INPUT) {
        fprintf(stderr, "[nvenc] nvEncEncodePicture failed: %d\n",
                (int)status);
        nvenc_.nvEncUnmapInputResource(encoder_, map_res.mappedResource);
        return false;
    }

    // Step 4: Lock and read the bitstream output.
    if (status == NV_ENC_SUCCESS) {
        NV_ENC_LOCK_BITSTREAM lock_bs = {};
        lock_bs.version         = NVENC_STRUCT_VERSION(NV_ENC_LOCK_BITSTREAM, 1);
        lock_bs.outputBitstream = bitstream_buffer_;

        status = nvenc_.nvEncLockBitstream(encoder_, &lock_bs);
        if (status == NV_ENC_SUCCESS) {
            const uint8_t* bs_data =
                static_cast<const uint8_t*>(lock_bs.bitstreamBufferPtr);
            uint32_t bs_size = lock_bs.bitstreamSizeInBytes;

            bool is_keyframe =
                (lock_bs.pictureType == NV_ENC_PIC_TYPE_IDR ||
                 lock_bs.pictureType == NV_ENC_PIC_TYPE_I);

            // If this is an IDR and we haven't sent config yet, extract
            // SPS/PPS from the beginning of the bitstream. NVENC with
            // repeatSPSPPS includes them inline before the IDR slice.
            if (is_keyframe && !config_sent_) {
                emit_config(bs_data, bs_size, timestamp_us, on_packet);
            }

            // Emit the encoded frame.
            if (on_packet) {
                EncodedPacket pkt;
                pkt.data         = bs_data;
                pkt.size         = bs_size;
                pkt.is_keyframe  = is_keyframe;
                pkt.is_config    = false;
                pkt.timestamp_us = timestamp_us;
                on_packet(pkt);
            }

            nvenc_.nvEncUnlockBitstream(encoder_, bitstream_buffer_);
        } else {
            fprintf(stderr, "[nvenc] nvEncLockBitstream failed: %d\n",
                    (int)status);
        }
    }

    // Step 5: Unmap the input resource.
    nvenc_.nvEncUnmapInputResource(encoder_, map_res.mappedResource);

    return true;
}

void NvencEncoder::emit_config(
        const uint8_t* data, size_t size, int64_t timestamp_us,
        const std::function<void(const EncodedPacket&)>& callback) {
    /*
     * NVENC with repeatSPSPPS prepends SPS and PPS NAL units to every
     * IDR frame in Annex B format. We scan for the first non-SPS/PPS
     * NAL unit to determine where the config data ends.
     *
     * NAL unit types (nal_unit_type is the lower 5 bits of the first byte
     * after the start code):
     *   7 = SPS
     *   8 = PPS
     *   5 = IDR slice
     */
    static const uint8_t start_code_3[3] = {0x00, 0x00, 0x01};
    static const uint8_t start_code_4[4] = {0x00, 0x00, 0x00, 0x01};

    size_t config_end = 0;
    size_t i = 0;
    while (i + 4 < size) {
        bool found_4 = (data[i] == 0 && data[i+1] == 0 &&
                        data[i+2] == 0 && data[i+3] == 1);
        bool found_3 = (!found_4 && data[i] == 0 &&
                        data[i+1] == 0 && data[i+2] == 1);

        if (found_4 || found_3) {
            size_t nal_start = found_4 ? i + 4 : i + 3;
            if (nal_start < size) {
                uint8_t nal_type = data[nal_start] & 0x1F;
                if (nal_type == 7 || nal_type == 8) {
                    // SPS or PPS -- config data continues past this NAL.
                    // Move past this start code.
                    i = nal_start;
                    continue;
                } else {
                    // Non-config NAL found; config data ends here.
                    config_end = i;
                    break;
                }
            }
        }
        i++;
    }

    if (config_end > 0 && callback) {
        EncodedPacket pkt;
        pkt.data         = data;
        pkt.size         = config_end;
        pkt.is_keyframe  = false;
        pkt.is_config    = true;
        pkt.timestamp_us = timestamp_us;
        callback(pkt);
        config_sent_ = true;
    }
}

// ---------------------------------------------------------------------------
// Dynamic reconfiguration
// ---------------------------------------------------------------------------

bool NvencEncoder::set_bitrate(uint32_t bitrate_kbps) {
    if (!encoder_) return false;

    std::lock_guard<std::mutex> lock(encode_mutex_);

    bitrate_kbps_ = bitrate_kbps;

    // Update the rate control parameters.
    encode_config_.rcParams.averageBitRate = bitrate_kbps * 1000;
    encode_config_.rcParams.maxBitRate     = bitrate_kbps * 1000;
    encode_config_.rcParams.vbvBufferSize  = (bitrate_kbps * 1000) / fps_;
    encode_config_.rcParams.vbvInitialDelay = (bitrate_kbps * 1000) / fps_;

    NV_ENC_RECONFIGURE_PARAMS reconfig = {};
    reconfig.version = NVENC_STRUCT_VERSION(NV_ENC_RECONFIGURE_PARAMS, 1);
    reconfig.reInitEncodeParams = init_params_;
    reconfig.reInitEncodeParams.encodeConfig = &encode_config_;
    reconfig.resetEncoder = 0;
    reconfig.forceIDR     = 0;

    NVENCSTATUS status = nvenc_.nvEncReconfigureEncoder(encoder_, &reconfig);
    if (status != NV_ENC_SUCCESS) {
        fprintf(stderr, "[nvenc] nvEncReconfigureEncoder failed: %d\n",
                (int)status);
        return false;
    }

    fprintf(stderr, "[nvenc] bitrate set to %u kbps\n", bitrate_kbps);
    return true;
}

bool NvencEncoder::force_keyframe() {
    keyframe_pending_.store(true);
    return true;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void NvencEncoder::shutdown() {
    if (!encoder_) return;

    // Send an EOS frame to flush the encoder.
    NV_ENC_PIC_PARAMS eos = {};
    eos.version        = NVENC_STRUCT_VERSION(NV_ENC_PIC_PARAMS, 1);
    eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    nvenc_.nvEncEncodePicture(encoder_, &eos);

    // Destroy the bitstream buffer.
    if (bitstream_buffer_) {
        nvenc_.nvEncDestroyBitstreamBuffer(encoder_, bitstream_buffer_);
        bitstream_buffer_ = nullptr;
    }

    // Unregister the input resource.
    if (registered_resource_) {
        nvenc_.nvEncUnregisterResource(encoder_, registered_resource_);
        registered_resource_ = nullptr;
    }

    // Destroy the encoder session.
    nvenc_.nvEncDestroyEncoder(encoder_);
    encoder_ = nullptr;

    // Shut down the color converter.
    color_converter_.shutdown();

    // Release D3D11 device if we own it.
    if (owns_device_) {
        context_.Reset();
        device_.Reset();
        owns_device_ = false;
    }

    // Unload the NVENC library.
    if (nvenc_lib_) {
        FreeLibrary(nvenc_lib_);
        nvenc_lib_ = nullptr;
    }

    memset(&nvenc_, 0, sizeof(nvenc_));

    fprintf(stderr, "[nvenc] encoder shut down\n");
}

} // namespace droidscreen
