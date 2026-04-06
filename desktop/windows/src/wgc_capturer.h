/*
 * DroidScreen Windows - Windows.Graphics.Capture screen capturer
 *
 * Uses the WinRT Graphics Capture API (Windows 10 1903+) to capture
 * the desktop at up to 60 fps. Frames are delivered as ID3D11Texture2D*
 * via the FramePool.FrameArrived event.
 */

#pragma once

#include "droidscreen/capturer.h"

#include <atomic>
#include <memory>
#include <mutex>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace droidscreen {

class WGCCapturer : public Capturer {
public:
    WGCCapturer();
    ~WGCCapturer() override;

    bool init(uint32_t display_index = 0) override;

    /// Initialize capture targeting a specific HMONITOR (e.g. a virtual display).
    /// This bypasses the DXGI output enumeration and creates a capture item
    /// directly for the given monitor.
    bool init_with_monitor(HMONITOR monitor);

    bool start(std::function<void(const CapturedFrame&)> on_frame) override;
    void stop() override;

    uint32_t width() const override { return width_; }
    uint32_t height() const override { return height_; }

    /// Returns the D3D11 device used for capture (shared with encoder).
    ID3D11Device* device() const { return device_.Get(); }

    /// Returns the immediate device context.
    ID3D11DeviceContext* context() const { return context_.Get(); }

    /// Returns the D3D11 context mutex. The encoder should lock this
    /// around all D3D11 immediate context calls to prevent races with
    /// the WGC FrameArrived callback (which runs on a threadpool thread).
    std::mutex& d3d_mutex() { return d3d_mutex_; }

private:
    /// Called by the FramePool.FrameArrived event.
    void on_frame_arrived();
    bool ensure_staging_texture(uint32_t width, uint32_t height, DXGI_FORMAT format);

    uint32_t width_  = 0;
    uint32_t height_ = 0;
    std::atomic<bool> running_{false};

    std::function<void(const CapturedFrame&)> on_frame_;

    // D3D11 resources.
    Microsoft::WRL::ComPtr<ID3D11Device>        device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>  context_;
    Microsoft::WRL::ComPtr<IDXGIOutput1>         output_;

    // Staging texture for copying frames (the capture texture must be
    // released as quickly as possible to avoid frame pool starvation).
    Microsoft::WRL::ComPtr<ID3D11Texture2D>      staging_texture_;
    DXGI_FORMAT                                  staging_format_ = DXGI_FORMAT_UNKNOWN;

    // WinRT capture objects — stored via pimpl to keep WinRT headers
    // out of this header. Defined in the .cpp.
    struct WinRTState;
    std::unique_ptr<WinRTState> wrt_;

    std::mutex frame_mutex_;

    // Mutex protecting D3D11 immediate context access. Shared with
    // the encoder to prevent concurrent context calls from the WGC
    // callback thread and the encode thread.
    std::mutex d3d_mutex_;
};

} // namespace droidscreen
