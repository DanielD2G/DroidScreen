/*
 * DroidScreen Windows - NVIDIA NVENC hardware H.264 encoder
 *
 * Dynamically loads the NVENC API from nvEncodeAPI64.dll so that the
 * application can run (and gracefully fall back) on systems without
 * an NVIDIA GPU.
 *
 * The encoder accepts ID3D11Texture2D* input (BGRA), runs it through
 * the color converter to NV12, and outputs Annex B NAL units suitable
 * for Android MediaCodec.
 *
 * Configuration targets ultra-low-latency streaming:
 *   - H.264 Baseline / Main profile
 *   - P1 preset (fastest)
 *   - Ultra-low-latency tuning
 *   - CBR rate control
 *   - 0 B-frames, no lookahead
 */

#pragma once

#include "droidscreen/encoder.h"
#include "color_converter.h"

#include <atomic>
#include <mutex>
#include <vector>
#include <cstdint>

#include <d3d11.h>
#include <wrl/client.h>

// ---------------------------------------------------------------------------
// NVENC API types defined inline (from nvEncodeAPI.h in the NVIDIA Video
// Codec SDK). We define only the subset we need so that the SDK headers
// are not required at build time.
// ---------------------------------------------------------------------------

// NVENC status codes.
typedef enum {
    NV_ENC_SUCCESS                 = 0,
    NV_ENC_ERR_NO_ENCODE_DEVICE   = 1,
    NV_ENC_ERR_UNSUPPORTED_DEVICE = 2,
    NV_ENC_ERR_INVALID_ENCODERDEVICE = 3,
    NV_ENC_ERR_INVALID_DEVICE     = 4,
    NV_ENC_ERR_DEVICE_NOT_EXIST   = 5,
    NV_ENC_ERR_INVALID_PTR        = 6,
    NV_ENC_ERR_INVALID_EVENT      = 7,
    NV_ENC_ERR_INVALID_PARAM      = 8,
    NV_ENC_ERR_INVALID_CALL       = 9,
    NV_ENC_ERR_OUT_OF_MEMORY      = 10,
    NV_ENC_ERR_ENCODER_NOT_INITIALIZED = 11,
    NV_ENC_ERR_UNSUPPORTED_PARAM  = 12,
    NV_ENC_ERR_LOCK_BUSY          = 13,
    NV_ENC_ERR_NOT_ENOUGH_BUFFER  = 14,
    NV_ENC_ERR_INVALID_VERSION    = 15,
    NV_ENC_ERR_MAP_FAILED         = 16,
    NV_ENC_ERR_NEED_MORE_INPUT    = 17,
    NV_ENC_ERR_ENCODER_BUSY       = 18,
    NV_ENC_ERR_EVENT_NOT_REGISTERD = 19,
    NV_ENC_ERR_GENERIC             = 20,
    NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY = 21,
    NV_ENC_ERR_UNIMPLEMENTED      = 22,
    NV_ENC_ERR_RESOURCE_REGISTER_FAILED = 23,
    NV_ENC_ERR_RESOURCE_NOT_REGISTERED  = 24,
    NV_ENC_ERR_RESOURCE_NOT_MAPPED = 25,
} NVENCSTATUS;

// NVENC device types.
typedef enum {
    NV_ENC_DEVICE_TYPE_DIRECTX = 0,
    NV_ENC_DEVICE_TYPE_CUDA    = 1,
    NV_ENC_DEVICE_TYPE_OPENGL  = 2,
} NV_ENC_DEVICE_TYPE;

// Buffer format.
typedef enum {
    NV_ENC_BUFFER_FORMAT_UNDEFINED    = 0x00000000,
    NV_ENC_BUFFER_FORMAT_NV12         = 0x00000001,
    NV_ENC_BUFFER_FORMAT_YV12         = 0x00000010,
    NV_ENC_BUFFER_FORMAT_IYUV         = 0x00000100,
    NV_ENC_BUFFER_FORMAT_YUV444       = 0x00001000,
    NV_ENC_BUFFER_FORMAT_YUV420_10BIT = 0x00010000,
    NV_ENC_BUFFER_FORMAT_ARGB         = 0x01000000,
    NV_ENC_BUFFER_FORMAT_ABGR         = 0x02000000,
} NV_ENC_BUFFER_FORMAT;

// Input resource type.
typedef enum {
    NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX  = 0x0,
    NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR = 0x1,
    NV_ENC_INPUT_RESOURCE_TYPE_CUDAARRAY = 0x2,
    NV_ENC_INPUT_RESOURCE_TYPE_OPENGL_TEX = 0x3,
} NV_ENC_INPUT_RESOURCE_TYPE;

// Picture types.
typedef enum {
    NV_ENC_PIC_TYPE_P           = 0x0,
    NV_ENC_PIC_TYPE_B           = 0x01,
    NV_ENC_PIC_TYPE_I           = 0x02,
    NV_ENC_PIC_TYPE_IDR         = 0x03,
    NV_ENC_PIC_TYPE_BI          = 0x04,
    NV_ENC_PIC_TYPE_SKIPPED     = 0x05,
    NV_ENC_PIC_TYPE_INTRA_REFRESH = 0x06,
    NV_ENC_PIC_TYPE_UNKNOWN     = 0xFF,
} NV_ENC_PIC_TYPE;

// Picture struct for encoding.
typedef enum {
    NV_ENC_PIC_STRUCT_FRAME     = 0x01,
    NV_ENC_PIC_STRUCT_FIELD_TOP_BOTTOM = 0x02,
    NV_ENC_PIC_STRUCT_FIELD_BOTTOM_TOP = 0x03,
} NV_ENC_PIC_STRUCT;

// Rate control modes.
typedef enum {
    NV_ENC_PARAMS_RC_CONSTQP   = 0x0,
    NV_ENC_PARAMS_RC_VBR       = 0x1,
    NV_ENC_PARAMS_RC_CBR       = 0x2,
    NV_ENC_PARAMS_RC_CBR_LOWDELAY_HQ = 0x8,
    NV_ENC_PARAMS_RC_CBR_HQ    = 0x10,
    NV_ENC_PARAMS_RC_VBR_HQ    = 0x20,
} NV_ENC_PARAMS_RC_MODE;

// Tuning info.
typedef enum {
    NV_ENC_TUNING_INFO_UNDEFINED        = 0,
    NV_ENC_TUNING_INFO_HIGH_QUALITY     = 1,
    NV_ENC_TUNING_INFO_LOW_LATENCY      = 2,
    NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY = 3,
    NV_ENC_TUNING_INFO_LOSSLESS         = 4,
} NV_ENC_TUNING_INFO;

// Codec GUIDs (H.264 and HEVC).
// {6BC82762-4E63-4CA4-AA85-1E50F321F6BF} - NV_ENC_CODEC_H264_GUID
static const GUID NV_ENC_CODEC_H264_GUID =
    { 0x6BC82762, 0x4E63, 0x4CA4, { 0xAA, 0x85, 0x1E, 0x50, 0xF3, 0x21, 0xF6, 0xBF } };

// Preset GUIDs.
// {FC0A8D3E-45F8-4CF8-80C7-298871590EBF} - NV_ENC_PRESET_P1_GUID (fastest)
static const GUID NV_ENC_PRESET_P1_GUID =
    { 0xFC0A8D3E, 0x45F8, 0x4CF8, { 0x80, 0xC7, 0x29, 0x88, 0x71, 0x59, 0x0E, 0xBF } };

// H.264 profile GUIDs.
// {0727BCAA-78C4-4C83-8C2F-EF3DFF267C6A} - NV_ENC_H264_PROFILE_MAIN_GUID
static const GUID NV_ENC_H264_PROFILE_MAIN_GUID =
    { 0x0727BCAA, 0x78C4, 0x4C83, { 0x8C, 0x2F, 0xEF, 0x3D, 0xFF, 0x26, 0x7C, 0x6A } };

// Encode picture flags.
#define NV_ENC_PIC_FLAG_FORCEIDR       0x01
#define NV_ENC_PIC_FLAG_FORCEINTRA     0x02
#define NV_ENC_PIC_FLAG_EOS            0x04

// API version.
// Use NVENC API 12.0 (compatible with NVIDIA driver R530+ / ~2023).
// Avoid 12.1+ which requires very recent drivers (R535+).
// If even 12.0 fails on older drivers, try 11.1 (R460+ / ~2021).
#define NVENCAPI_MAJOR_VERSION  12
#define NVENCAPI_MINOR_VERSION  0
#define NVENCAPI_VERSION        ((NVENCAPI_MAJOR_VERSION) | ((NVENCAPI_MINOR_VERSION) << 24))
#define NV_ENCODE_API_FUNCTION_LIST_VER \
    (sizeof(NV_ENCODE_API_FUNCTION_LIST) | (NVENCAPI_VERSION << 16))

// Struct version macro.
#define NVENC_STRUCT_VERSION(type, ver) \
    ((uint32_t)sizeof(type) | ((ver) << 16) | (NVENCAPI_VERSION << 24))

// ---------------------------------------------------------------------------
// Simplified NVENC structures (matching SDK layout for ABI compatibility)
// ---------------------------------------------------------------------------

#pragma pack(push, 8)

struct NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS {
    uint32_t version;
    NV_ENC_DEVICE_TYPE deviceType;
    void* device;
    uint32_t reserved;
    uint32_t apiVersion;
    uint32_t reserved1[253];
    void* reserved2[64];
};

struct NV_ENC_CONFIG_H264 {
    uint32_t enableTemporalSVC       : 1;
    uint32_t enableStereoMVC         : 1;
    uint32_t hierarchicalPFrames     : 1;
    uint32_t hierarchicalBFrames     : 1;
    uint32_t outputBufferingPeriodSEI: 1;
    uint32_t outputPictureTimingSEI  : 1;
    uint32_t outputAUD               : 1;
    uint32_t disableSPSPPS           : 1;
    uint32_t outputFramePackingSEI   : 1;
    uint32_t outputRecoveryPointSEI  : 1;
    uint32_t enableIntraRefresh      : 1;
    uint32_t enableConstrainedEncoding: 1;
    uint32_t repeatSPSPPS            : 1;
    uint32_t enableVFR               : 1;
    uint32_t enableLTR               : 1;
    uint32_t qpPrimeYZeroTransformBypassFlag : 1;
    uint32_t useConstrainedIntraPred : 1;
    uint32_t enableFillerDataInsertion : 1;
    uint32_t reservedBitFields       : 14;
    uint32_t level;
    uint32_t idrPeriod;
    uint32_t separateColourPlaneFlag;
    uint32_t disableDeblockingFilterIDC;
    uint32_t numTemporalLayers;
    uint32_t spsId;
    uint32_t ppsId;
    uint32_t adaptiveTransformMode;
    uint32_t fmoMode;
    uint32_t bdirectMode;
    uint32_t entropyCodingMode;  // 0=auto, 1=CABAC, 2=CAVLC
    uint32_t stereoMode;
    uint32_t sliceMode;
    uint32_t sliceModeData;
    uint32_t maxNumRefFrames;
    uint32_t useBFramesAsRef;
    uint32_t numRefL0;
    uint32_t numRefL1;
    uint8_t  reserved1[256];
};

struct NV_ENC_RC_PARAMS {
    uint32_t version;
    NV_ENC_PARAMS_RC_MODE rateControlMode;
    uint32_t constQP_interP;
    uint32_t constQP_interB;
    uint32_t constQP_intra;
    uint32_t averageBitRate;
    uint32_t maxBitRate;
    uint32_t vbvBufferSize;
    uint32_t vbvInitialDelay;
    uint32_t enableMinQP       : 1;
    uint32_t enableMaxQP       : 1;
    uint32_t enableInitialRCQP : 1;
    uint32_t enableAQ          : 1;
    uint32_t reservedBitField1 : 1;
    uint32_t enableLookahead   : 1;
    uint32_t disableIadapt     : 1;
    uint32_t disableBadapt     : 1;
    uint32_t enableTemporalAQ  : 1;
    uint32_t zeroReorderDelay  : 1;
    uint32_t enableNonRefP     : 1;
    uint32_t strictGOPTarget   : 1;
    uint32_t aqStrength        : 4;
    uint32_t reservedBitFields : 16;
    uint32_t minQP_interP;
    uint32_t minQP_interB;
    uint32_t minQP_intra;
    uint32_t maxQP_interP;
    uint32_t maxQP_interB;
    uint32_t maxQP_intra;
    uint32_t initialRCQP_interP;
    uint32_t initialRCQP_interB;
    uint32_t initialRCQP_intra;
    uint32_t temporallayerIdxMask;
    uint8_t  temporalLayerQP[8];
    uint16_t targetQuality;
    uint16_t targetQualityLSB;
    uint16_t lookaheadDepth;
    uint16_t lowDelayKeyFrameScale;
    uint32_t reserved1[14];
};

struct NV_ENC_CODEC_CONFIG {
    union {
        NV_ENC_CONFIG_H264 h264Config;
        uint8_t reserved[512];
    };
};

struct NV_ENC_CONFIG {
    uint32_t version;
    GUID profileGUID;
    uint32_t gopLength;
    int32_t  frameIntervalP;  // number of B-frames + 1
    uint32_t monoChromeEncoding;
    uint32_t frameFieldMode;
    uint32_t mvPrecision;
    NV_ENC_RC_PARAMS rcParams;
    NV_ENC_CODEC_CONFIG encodeCodecConfig;
    uint32_t reserved[278];
    void*    reserved2[64];
};

struct NV_ENC_PRESET_CONFIG {
    uint32_t version;
    NV_ENC_CONFIG presetCfg;
    uint32_t reserved1[255];
    void*    reserved2[64];
};

struct NV_ENC_INITIALIZE_PARAMS {
    uint32_t version;
    GUID encodeGUID;
    GUID presetGUID;
    uint32_t encodeWidth;
    uint32_t encodeHeight;
    uint32_t darWidth;
    uint32_t darHeight;
    uint32_t frameRateNum;
    uint32_t frameRateDen;
    uint32_t enableEncodeAsync    : 1;
    uint32_t enablePTD            : 1;
    uint32_t reportSliceOffsets   : 1;
    uint32_t enableSubFrameWrite  : 1;
    uint32_t enableExternalMEHints: 1;
    uint32_t enableMEOnlyMode     : 1;
    uint32_t enableWeightedPrediction : 1;
    uint32_t enableOutputInVidmem : 1;
    uint32_t reservedBitFields    : 24;
    uint32_t privDataSize;
    void*    privData;
    NV_ENC_CONFIG* encodeConfig;
    uint32_t maxEncodeWidth;
    uint32_t maxEncodeHeight;
    uint32_t reserved1[285];
    void*    reserved2[64];
    NV_ENC_TUNING_INFO tuningInfo;
};

struct NV_ENC_REGISTER_RESOURCE {
    uint32_t version;
    NV_ENC_INPUT_RESOURCE_TYPE resourceType;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t subResourceIndex;
    void*    resourceToRegister;
    void*    registeredResource;  // OUT
    NV_ENC_BUFFER_FORMAT bufferFormat;
    uint32_t bufferUsage;
    uint32_t reserved1[62];
    void*    reserved2[64];
};

struct NV_ENC_MAP_INPUT_RESOURCE {
    uint32_t version;
    uint32_t subResourceIndex;
    void*    inputResource;
    void*    mappedResource;    // OUT
    NV_ENC_BUFFER_FORMAT mappedBufferFmt;  // OUT
    uint32_t reserved1[61];
    void*    reserved2[63];
};

struct NV_ENC_INPUT_PTR_s;
typedef NV_ENC_INPUT_PTR_s* NV_ENC_INPUT_PTR;

struct NV_ENC_OUTPUT_PTR_s;
typedef NV_ENC_OUTPUT_PTR_s* NV_ENC_OUTPUT_PTR;

struct NV_ENC_CREATE_BITSTREAM_BUFFER {
    uint32_t version;
    uint32_t size;
    uint32_t memoryHeap;
    void*    bitstreamBuffer;     // OUT
    void*    bitstreamBufferPtr;  // OUT
    uint32_t reserved1[58];
    void*    reserved2[64];
};

struct NV_ENC_PIC_PARAMS {
    uint32_t version;
    uint32_t inputWidth;
    uint32_t inputHeight;
    uint32_t inputPitch;
    uint32_t encodePicFlags;
    uint32_t frameIdx;
    uint64_t inputTimeStamp;
    uint64_t inputDuration;
    void*    inputBuffer;
    void*    outputBitstream;
    void*    completionEvent;
    NV_ENC_BUFFER_FORMAT bufferFmt;
    NV_ENC_PIC_STRUCT pictureStruct;
    NV_ENC_PIC_TYPE pictureType;
    uint32_t codecPicParams[256];
    uint32_t reserved1[233];
    void*    reserved2[64];
};

struct NV_ENC_LOCK_BITSTREAM {
    uint32_t version;
    uint32_t doNotWait : 1;
    uint32_t ltrFrame  : 1;
    uint32_t reservedBitFields : 30;
    void*    outputBitstream;
    uint32_t* sliceOffsets;
    uint32_t frameIdx;
    uint32_t hwEncodeStatus;
    uint32_t numSlices;
    uint32_t bitstreamSizeInBytes;
    uint64_t outputTimeStamp;
    uint64_t outputDuration;
    void*    bitstreamBufferPtr;     // OUT
    NV_ENC_PIC_TYPE pictureType;     // OUT
    NV_ENC_PIC_STRUCT pictureStruct; // OUT
    uint32_t frameAvgQP;
    uint32_t frameSatd;
    uint32_t ltrFrameIdx;
    uint32_t ltrFrameBitmap;
    uint32_t reserved[13];
    uint32_t intraMBCount;
    uint32_t interMBCount;
    int32_t  averageMVX;
    int32_t  averageMVY;
    uint32_t reserved1[219];
    void*    reserved2[64];
};

struct NV_ENC_RECONFIGURE_PARAMS {
    uint32_t version;
    NV_ENC_INITIALIZE_PARAMS reInitEncodeParams;
    uint32_t resetEncoder    : 1;
    uint32_t forceIDR        : 1;
    uint32_t reserved        : 30;
    uint32_t reserved1[255];
    void*    reserved2[64];
};

// NVENC function list.
struct NV_ENCODE_API_FUNCTION_LIST {
    uint32_t version;
    uint32_t reserved;
    NVENCSTATUS (*nvEncOpenEncodeSessionEx)(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS*, void**);
    NVENCSTATUS (*nvEncGetEncodeGUIDs)(void*, GUID*, uint32_t, uint32_t*);
    NVENCSTATUS (*nvEncGetEncodeProfileGUIDCount)(void*, GUID, uint32_t*);
    NVENCSTATUS (*nvEncGetEncodeProfileGUIDs)(void*, GUID, GUID*, uint32_t, uint32_t*);
    NVENCSTATUS (*nvEncGetInputFormatCount)(void*, GUID, uint32_t*);
    NVENCSTATUS (*nvEncGetInputFormats)(void*, GUID, NV_ENC_BUFFER_FORMAT*, uint32_t, uint32_t*);
    NVENCSTATUS (*nvEncGetEncodeCaps)(void*, GUID, void*, void*);
    NVENCSTATUS (*nvEncGetEncodePresetCount)(void*, GUID, uint32_t*);
    NVENCSTATUS (*nvEncGetEncodePresetGUIDs)(void*, GUID, GUID*, uint32_t, uint32_t*);
    NVENCSTATUS (*nvEncGetEncodePresetConfig)(void*, GUID, GUID, NV_ENC_PRESET_CONFIG*);
    NVENCSTATUS (*nvEncGetEncodePresetConfigEx)(void*, GUID, GUID, NV_ENC_TUNING_INFO, NV_ENC_PRESET_CONFIG*);
    NVENCSTATUS (*nvEncInitializeEncoder)(void*, NV_ENC_INITIALIZE_PARAMS*);
    NVENCSTATUS (*nvEncCreateInputBuffer)(void*, void*);
    NVENCSTATUS (*nvEncDestroyInputBuffer)(void*, void*);
    NVENCSTATUS (*nvEncCreateBitstreamBuffer)(void*, NV_ENC_CREATE_BITSTREAM_BUFFER*);
    NVENCSTATUS (*nvEncDestroyBitstreamBuffer)(void*, void*);
    NVENCSTATUS (*nvEncEncodePicture)(void*, NV_ENC_PIC_PARAMS*);
    NVENCSTATUS (*nvEncLockBitstream)(void*, NV_ENC_LOCK_BITSTREAM*);
    NVENCSTATUS (*nvEncUnlockBitstream)(void*, void*);
    NVENCSTATUS (*nvEncLockInputBuffer)(void*, void*);
    NVENCSTATUS (*nvEncUnlockInputBuffer)(void*, void*);
    NVENCSTATUS (*nvEncGetEncodeStats)(void*, void*);
    NVENCSTATUS (*nvEncGetSequenceParams)(void*, void*);
    NVENCSTATUS (*nvEncRegisterAsyncEvent)(void*, void*);
    NVENCSTATUS (*nvEncUnregisterAsyncEvent)(void*, void*);
    NVENCSTATUS (*nvEncMapInputResource)(void*, NV_ENC_MAP_INPUT_RESOURCE*);
    NVENCSTATUS (*nvEncUnmapInputResource)(void*, void*);
    NVENCSTATUS (*nvEncDestroyEncoder)(void*);
    NVENCSTATUS (*nvEncInvalidateRefFrames)(void*, uint64_t);
    NVENCSTATUS (*nvEncOpenEncodeSession)(void*, uint32_t, void**);
    NVENCSTATUS (*nvEncRegisterResource)(void*, NV_ENC_REGISTER_RESOURCE*);
    NVENCSTATUS (*nvEncUnregisterResource)(void*, void*);
    NVENCSTATUS (*nvEncReconfigureEncoder)(void*, NV_ENC_RECONFIGURE_PARAMS*);
    void*       reserved2[285];
};

#pragma pack(pop)

// NvEncodeAPICreateInstance function signature.
typedef NVENCSTATUS (*NvEncodeAPICreateInstance_t)(NV_ENCODE_API_FUNCTION_LIST*);

namespace droidscreen {

class NvencEncoder : public Encoder {
public:
    NvencEncoder();
    ~NvencEncoder() override;

    /// Set the D3D11 device to use (must be called before init).
    /// If not set, the encoder creates its own device.
    void set_d3d_device(ID3D11Device* device, ID3D11DeviceContext* context);

    bool init(uint32_t width, uint32_t height,
              uint32_t fps, uint32_t bitrate_kbps) override;
    bool encode(void* native_frame, int64_t timestamp_us,
                std::function<void(const EncodedPacket&)> on_packet) override;
    bool set_bitrate(uint32_t bitrate_kbps) override;
    bool force_keyframe() override;
    void shutdown() override;

private:
    /// Load nvEncodeAPI64.dll and resolve NvEncodeAPICreateInstance.
    bool load_nvenc_library();

    /// Emit SPS/PPS as a config packet via the user callback.
    void emit_config(const uint8_t* data, size_t size, int64_t timestamp_us,
                     const std::function<void(const EncodedPacket&)>& callback);

    // D3D11 device (shared or owned).
    Microsoft::WRL::ComPtr<ID3D11Device>       device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    bool owns_device_ = false;

    // Color converter (BGRA -> NV12).
    ColorConverter color_converter_;

    // NVENC library handle and function list.
    HMODULE nvenc_lib_ = nullptr;
    NV_ENCODE_API_FUNCTION_LIST nvenc_ = {};

    // NVENC encoder session.
    void* encoder_ = nullptr;

    // Registered input resource (the NV12 texture from color_converter_).
    void* registered_resource_ = nullptr;

    // Output bitstream buffer.
    void* bitstream_buffer_ = nullptr;

    // Encoder configuration snapshot (for reconfigure).
    NV_ENC_INITIALIZE_PARAMS init_params_ = {};
    NV_ENC_CONFIG            encode_config_ = {};

    uint32_t width_   = 0;
    uint32_t height_  = 0;
    uint32_t fps_     = 0;
    uint32_t bitrate_kbps_ = 0;

    std::atomic<bool> keyframe_pending_{false};
    bool config_sent_ = false;

    std::mutex encode_mutex_;
};

} // namespace droidscreen
