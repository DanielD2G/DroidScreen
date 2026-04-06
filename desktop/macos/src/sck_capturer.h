/*
 * DroidScreen macOS - ScreenCaptureKit screen capturer
 *
 * Uses SCStream to capture the display at up to 60 fps.
 * Frames are delivered as CVPixelBufferRef via the SCStreamOutput
 * delegate protocol.
 */

#pragma once

#include "droidscreen/capturer.h"

#include <atomic>

#ifdef __OBJC__
@class SCKCapturerDelegate;
@class SCStream;
@class SCDisplay;
#else
typedef void SCKCapturerDelegate;
typedef void SCStream;
typedef void SCDisplay;
#endif

namespace droidscreen {

class SCKCapturer : public Capturer {
public:
    SCKCapturer();
    ~SCKCapturer() override;

    bool init(uint32_t display_index = 0) override;

    /// Initialize capture for a specific CGDirectDisplayID (e.g., virtual display).
    /// @param cg_display_id  The CGDirectDisplayID to capture.
    /// @param capture_width  Output pixel width (0 = use display's native).
    /// @param capture_height Output pixel height (0 = use display's native).
    /// @param target_fps     Target frame rate (default 60).
    bool init_with_display_id(uint32_t cg_display_id,
                              uint32_t capture_width = 0,
                              uint32_t capture_height = 0,
                              uint32_t target_fps = 60);
    bool start(std::function<void(const CapturedFrame&)> on_frame) override;
    void stop() override;

    uint32_t width() const override { return width_; }
    uint32_t height() const override { return height_; }

    // Called by the Obj-C delegate to deliver frames.
    void deliver_frame(const CapturedFrame& frame);

private:
    uint32_t width_  = 0;
    uint32_t height_ = 0;
    std::atomic<bool> running_{false};

    std::function<void(const CapturedFrame&)> on_frame_;

    // Objective-C objects (bridged via void* or __bridge).
    SCStream* stream_                  = nullptr;
    SCKCapturerDelegate* delegate_     = nullptr;
};

} // namespace droidscreen
