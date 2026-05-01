/*
 * DroidScreen Desktop - Abstract video encoder
 *
 * Platform-specific implementations provide the actual encoding logic.
 * macOS: VideoToolbox, Windows: Media Foundation
 */

#pragma once

#include <cstdint>
#include <functional>

extern "C" {
#include "droidscreen/handshake.h"
}

namespace droidscreen {

struct EncodedPacket {
    const uint8_t* data;
    size_t size;
    bool is_keyframe;
    bool is_config;        // true for SPS/PPS parameter set data
    int64_t timestamp_us;
};

class Encoder {
public:
    virtual ~Encoder() = default;

    /// Initialize the encoder with the given parameters.
    /// @param width Frame width in pixels.
    /// @param height Frame height in pixels.
    /// @param fps Target frames per second.
    /// @param bitrate_kbps Target bitrate in kilobits per second.
    /// @return true on success.
    virtual bool init(uint32_t width, uint32_t height,
                      uint32_t fps, uint32_t bitrate_kbps,
                      ds_codec_t codec = DS_CODEC_H264) = 0;

    /// Encode a single frame.
    /// @param native_frame Platform-specific frame handle (CVPixelBufferRef on macOS).
    /// @param timestamp_us Presentation timestamp in microseconds.
    /// @param on_packet Called for each output packet (may be called multiple times).
    /// @return true on success.
    virtual bool encode(void* native_frame, int64_t timestamp_us,
                        std::function<void(const EncodedPacket&)> on_packet) = 0;

    /// Change the target bitrate on the fly.
    virtual bool set_bitrate(uint32_t bitrate_kbps) = 0;

    /// Request that the next encoded frame be a keyframe.
    virtual bool force_keyframe() = 0;

    /// Shut down the encoder and release resources.
    virtual void shutdown() = 0;
};

} // namespace droidscreen
