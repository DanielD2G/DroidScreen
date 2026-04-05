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
#include <mutex>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

// Forward-declare WinRT types to avoid pulling in the full WinRT headers
// in every translation unit. The .cpp includes the real headers.
namespace winrt::Windows::Graphics::Capture {
    struct Direct3D11CaptureFramePool;
    struct GraphicsCaptureSession;
    struct GraphicsCaptureItem;
}

namespace droidscreen {

class WGCCapturer : public Capturer {
public:
    WGCCapturer();
    ~WGCCapturer() override;

    bool init(uint32_t display_index = 0) override;
    bool start(std::function<void(const CapturedFrame&)> on_frame) override;
    void stop() override;

    uint32_t width() const override { return width_; }
    uint32_t height() const override { return height_; }

    /// Returns the D3D11 device used for capture (shared with encoder).
    ID3D11Device* device() const { return device_.Get(); }

    /// Returns the immediate device context.
    ID3D11DeviceContext* context() const { return context_.Get(); }

private:
    /// Called by the FramePool.FrameArrived event.
    void on_frame_arrived();

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

    // WinRT capture objects — stored as void* here to keep the header
    // free of WinRT includes. The .cpp casts them properly.
    void* frame_pool_     = nullptr;  // Direct3D11CaptureFramePool
    void* capture_session_ = nullptr; // GraphicsCaptureSession
    void* capture_item_    = nullptr; // GraphicsCaptureItem
    void* frame_arrived_token_ = nullptr; // event token storage

    std::mutex frame_mutex_;
};

} // namespace droidscreen
