/*
 * DroidScreen macOS - VideoToolbox H.264 encoder implementation
 *
 * CRITICAL: VideoToolbox outputs NAL units in AVCC format (4-byte
 * big-endian length prefix). Android MediaCodec expects Annex B
 * format (0x00 0x00 0x00 0x01 start codes). This encoder handles
 * the conversion in emit_frame().
 *
 * SPS/PPS parameter sets are extracted from the format description
 * and sent as a separate config packet with Annex B start codes.
 */

#import "vt_encoder.h"

#import <VideoToolbox/VideoToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace droidscreen {

// Annex B start code (4 bytes).
static const uint8_t kAnnexBStartCode[4] = {0x00, 0x00, 0x00, 0x01};

VTEncoder::VTEncoder() = default;

VTEncoder::~VTEncoder() {
    shutdown();
}

bool VTEncoder::init(uint32_t width, uint32_t height,
                     uint32_t fps, uint32_t bitrate_kbps) {
    width_  = width;
    height_ = height;
    fps_    = fps;

    // Create the compression session.
    OSStatus status = VTCompressionSessionCreate(
        kCFAllocatorDefault,
        (int32_t)width, (int32_t)height,
        kCMVideoCodecType_H264,
        nullptr,   // encoderSpecification
        nullptr,   // sourceImageBufferAttributes (use default)
        nullptr,   // compressedDataAllocator
        &VTEncoder::output_callback,
        this,      // outputCallbackRefCon
        &session_);

    if (status != noErr || !session_) {
        fprintf(stderr, "[vt] VTCompressionSessionCreate failed: %d\n",
                (int)status);
        return false;
    }

    // Configure for real-time encoding.
    VTSessionSetProperty(session_,
        kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);

    // H.264 Main profile.
    VTSessionSetProperty(session_,
        kVTCompressionPropertyKey_ProfileLevel,
        kVTProfileLevel_H264_Main_AutoLevel);

    // Disable B-frames for lowest latency.
    VTSessionSetProperty(session_,
        kVTCompressionPropertyKey_AllowFrameReordering,
        kCFBooleanFalse);

    // No frame delay -- emit compressed frames immediately.
    int zero_val = 0;
    CFNumberRef zero = CFNumberCreate(kCFAllocatorDefault,
                                       kCFNumberIntType, &zero_val);
    VTSessionSetProperty(session_,
        kVTCompressionPropertyKey_MaxFrameDelayCount, zero);
    CFRelease(zero);

    // Set target bitrate (in bits per second).
    int32_t bitrate_bps = (int32_t)(bitrate_kbps * 1000);
    CFNumberRef br = CFNumberCreate(kCFAllocatorDefault,
                                     kCFNumberSInt32Type, &bitrate_bps);
    VTSessionSetProperty(session_,
        kVTCompressionPropertyKey_AverageBitRate, br);
    CFRelease(br);

    // Set expected frame rate hint.
    int32_t fps_val = (int32_t)fps;
    CFNumberRef fr = CFNumberCreate(kCFAllocatorDefault,
                                     kCFNumberSInt32Type, &fps_val);
    VTSessionSetProperty(session_,
        kVTCompressionPropertyKey_ExpectedFrameRate, fr);
    CFRelease(fr);

    // Set keyframe interval (every 2 seconds).
    int32_t kf_interval = (int32_t)(fps * 2);
    CFNumberRef kfi = CFNumberCreate(kCFAllocatorDefault,
                                      kCFNumberSInt32Type, &kf_interval);
    VTSessionSetProperty(session_,
        kVTCompressionPropertyKey_MaxKeyFrameInterval, kfi);
    CFRelease(kfi);

    // Prepare the session.
    VTCompressionSessionPrepareToEncodeFrames(session_);

    config_sent_ = false;
    keyframe_pending_.store(false);

    fprintf(stderr, "[vt] encoder initialized: %ux%u@%u, %u kbps\n",
            width, height, fps, bitrate_kbps);
    return true;
}

bool VTEncoder::encode(void* native_frame, int64_t timestamp_us,
                       std::function<void(const EncodedPacket&)> on_packet) {
    if (!session_) return false;

    CVPixelBufferRef pixel_buf = static_cast<CVPixelBufferRef>(native_frame);
    if (!pixel_buf) return false;

    // Set the callback for this encode call.
    {
        std::lock_guard<std::mutex> lock(encode_mutex_);
        current_callback_ = on_packet;
    }

    // Build the presentation timestamp.
    CMTime pts = CMTimeMake(timestamp_us, 1000000);

    // Frame properties -- request keyframe if pending.
    CFDictionaryRef frame_props = nullptr;
    if (keyframe_pending_.exchange(false)) {
        const void* keys[] = {
            kVTEncodeFrameOptionKey_ForceKeyFrame
        };
        const void* vals[] = { kCFBooleanTrue };
        frame_props = CFDictionaryCreate(
            kCFAllocatorDefault, keys, vals, 1,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
    }

    // DO NOT lock the CVPixelBuffer -- VideoToolbox handles it.
    OSStatus status = VTCompressionSessionEncodeFrame(
        session_,
        pixel_buf,
        pts,
        kCMTimeInvalid,   // duration
        frame_props,
        nullptr,          // sourceFrameRefcon
        nullptr);         // infoFlagsOut

    if (frame_props) CFRelease(frame_props);

    if (status != noErr) {
        fprintf(stderr, "[vt] VTCompressionSessionEncodeFrame failed: %d\n",
                (int)status);
        return false;
    }

    // Flush to ensure the callback fires synchronously.
    VTCompressionSessionCompleteFrames(session_, kCMTimeInvalid);

    // Clear the callback.
    {
        std::lock_guard<std::mutex> lock(encode_mutex_);
        current_callback_ = nullptr;
    }

    return true;
}

void VTEncoder::output_callback(void* refcon,
                                void* /*source_frame_refcon*/,
                                OSStatus status,
                                VTEncodeInfoFlags /*info_flags*/,
                                CMSampleBufferRef sample_buf) {
    if (status != noErr || !sample_buf) {
        fprintf(stderr, "[vt] output callback error: %d\n", (int)status);
        return;
    }

    VTEncoder* self = static_cast<VTEncoder*>(refcon);

    // Check if this is a keyframe.
    bool is_keyframe = false;
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(
        sample_buf, false);
    if (attachments && CFArrayGetCount(attachments) > 0) {
        CFDictionaryRef dict =
            (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
        CFBooleanRef not_sync = (CFBooleanRef)CFDictionaryGetValue(
            dict, kCMSampleAttachmentKey_NotSync);
        is_keyframe = (!not_sync || !CFBooleanGetValue(not_sync));
    }

    // Get the presentation timestamp.
    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample_buf);
    int64_t timestamp_us = (int64_t)(CMTimeGetSeconds(pts) * 1e6);

    // On keyframe (or first frame), extract and emit SPS/PPS config.
    if (is_keyframe || !self->config_sent_) {
        CMFormatDescriptionRef fmt =
            CMSampleBufferGetFormatDescription(sample_buf);
        if (fmt) {
            self->emit_config(fmt, timestamp_us);
        }
    }

    // Emit the frame data (converted from AVCC to Annex B).
    self->emit_frame(sample_buf, is_keyframe);
}

void VTEncoder::emit_config(CMFormatDescriptionRef fmt,
                            int64_t timestamp_us) {
    std::vector<uint8_t> config_data;

    // Extract SPS (index 0).
    {
        const uint8_t* sps_ptr = nullptr;
        size_t sps_len = 0;
        OSStatus status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
            fmt, 0, &sps_ptr, &sps_len, nullptr, nullptr);
        if (status == noErr && sps_ptr && sps_len > 0) {
            config_data.insert(config_data.end(),
                               kAnnexBStartCode, kAnnexBStartCode + 4);
            config_data.insert(config_data.end(),
                               sps_ptr, sps_ptr + sps_len);
        } else {
            fprintf(stderr, "[vt] failed to get SPS: %d\n", (int)status);
            return;
        }
    }

    // Extract PPS (index 1).
    {
        const uint8_t* pps_ptr = nullptr;
        size_t pps_len = 0;
        OSStatus status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
            fmt, 1, &pps_ptr, &pps_len, nullptr, nullptr);
        if (status == noErr && pps_ptr && pps_len > 0) {
            config_data.insert(config_data.end(),
                               kAnnexBStartCode, kAnnexBStartCode + 4);
            config_data.insert(config_data.end(),
                               pps_ptr, pps_ptr + pps_len);
        } else {
            fprintf(stderr, "[vt] failed to get PPS: %d\n", (int)status);
            return;
        }
    }

    // Emit as a config packet.
    std::lock_guard<std::mutex> lock(encode_mutex_);
    if (current_callback_) {
        EncodedPacket pkt;
        pkt.data         = config_data.data();
        pkt.size         = config_data.size();
        pkt.is_keyframe  = false;
        pkt.is_config    = true;
        pkt.timestamp_us = timestamp_us;
        current_callback_(pkt);
    }

    config_sent_ = true;
}

void VTEncoder::emit_frame(CMSampleBufferRef sample_buf, bool is_keyframe) {
    // Get the block buffer containing the encoded data.
    CMBlockBufferRef block_buf = CMSampleBufferGetDataBuffer(sample_buf);
    if (!block_buf) return;

    size_t total_len = 0;
    char* data_ptr = nullptr;
    OSStatus status = CMBlockBufferGetDataPointer(
        block_buf, 0, nullptr, &total_len, &data_ptr);
    if (status != noErr || !data_ptr || total_len == 0) {
        fprintf(stderr, "[vt] CMBlockBufferGetDataPointer failed: %d\n",
                (int)status);
        return;
    }

    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample_buf);
    int64_t timestamp_us = (int64_t)(CMTimeGetSeconds(pts) * 1e6);

    /*
     * AVCC -> Annex B conversion.
     *
     * VideoToolbox outputs NAL units with 4-byte big-endian length prefixes:
     *   [len_3][len_2][len_1][len_0][NAL data...]
     *
     * Android MediaCodec expects Annex B start codes:
     *   [0x00][0x00][0x00][0x01][NAL data...]
     *
     * We iterate through the buffer, replacing each length prefix with
     * the 4-byte Annex B start code.
     */
    std::vector<uint8_t> annex_b;
    annex_b.reserve(total_len + 64);  // slight over-allocation

    size_t offset = 0;
    while (offset + 4 <= total_len) {
        // Read 4-byte big-endian NAL unit length.
        uint32_t nal_len =
            (static_cast<uint32_t>((uint8_t)data_ptr[offset])     << 24) |
            (static_cast<uint32_t>((uint8_t)data_ptr[offset + 1]) << 16) |
            (static_cast<uint32_t>((uint8_t)data_ptr[offset + 2]) <<  8) |
            (static_cast<uint32_t>((uint8_t)data_ptr[offset + 3]));
        offset += 4;

        if (nal_len == 0 || offset + nal_len > total_len) {
            fprintf(stderr, "[vt] invalid NAL length %u at offset %zu "
                    "(total %zu)\n", nal_len, offset - 4, total_len);
            break;
        }

        // Write Annex B start code.
        annex_b.insert(annex_b.end(),
                       kAnnexBStartCode, kAnnexBStartCode + 4);

        // Copy NAL unit data.
        annex_b.insert(annex_b.end(),
                       (uint8_t*)data_ptr + offset,
                       (uint8_t*)data_ptr + offset + nal_len);

        offset += nal_len;
    }

    if (annex_b.empty()) return;

    // Emit the converted frame.
    std::lock_guard<std::mutex> lock(encode_mutex_);
    if (current_callback_) {
        EncodedPacket pkt;
        pkt.data         = annex_b.data();
        pkt.size         = annex_b.size();
        pkt.is_keyframe  = is_keyframe;
        pkt.is_config    = false;
        pkt.timestamp_us = timestamp_us;
        current_callback_(pkt);
    }
}

bool VTEncoder::set_bitrate(uint32_t bitrate_kbps) {
    if (!session_) return false;

    int32_t bitrate_bps = (int32_t)(bitrate_kbps * 1000);
    CFNumberRef br = CFNumberCreate(kCFAllocatorDefault,
                                     kCFNumberSInt32Type, &bitrate_bps);
    OSStatus status = VTSessionSetProperty(
        session_, kVTCompressionPropertyKey_AverageBitRate, br);
    CFRelease(br);

    if (status != noErr) {
        fprintf(stderr, "[vt] set_bitrate failed: %d\n", (int)status);
        return false;
    }

    fprintf(stderr, "[vt] bitrate set to %u kbps\n", bitrate_kbps);
    return true;
}

bool VTEncoder::force_keyframe() {
    keyframe_pending_.store(true);
    return true;
}

void VTEncoder::shutdown() {
    if (session_) {
        VTCompressionSessionCompleteFrames(session_, kCMTimeInvalid);
        VTCompressionSessionInvalidate(session_);
        CFRelease(session_);
        session_ = nullptr;
        fprintf(stderr, "[vt] encoder shut down\n");
    }
}

} // namespace droidscreen
