/*
 * DroidScreen Desktop - Abstract screen capturer
 *
 * Platform-specific implementations provide the actual capture logic.
 * macOS: ScreenCaptureKit, Windows: DXGI Desktop Duplication
 */

#pragma once

#include <cstdint>
#include <functional>

namespace droidscreen {

struct CapturedFrame {
    void* native_handle;   // CVPixelBufferRef on macOS, ID3D11Texture2D* on Windows
    uint32_t width;
    uint32_t height;
    int64_t timestamp_us;
    bool is_idle;          // true if the frame content hasn't changed
};

class Capturer {
public:
    virtual ~Capturer() = default;

    /// Initialize capture for the given display.
    /// @param display_index Zero-based index of the display to capture.
    /// @return true on success.
    virtual bool init(uint32_t display_index = 0) = 0;

    /// Start capturing frames. Calls on_frame from the capture thread.
    virtual bool start(std::function<void(const CapturedFrame&)> on_frame) = 0;

    /// Stop capturing and release resources.
    virtual void stop() = 0;

    /// Width of the captured display in pixels.
    virtual uint32_t width() const = 0;

    /// Height of the captured display in pixels.
    virtual uint32_t height() const = 0;
};

} // namespace droidscreen
