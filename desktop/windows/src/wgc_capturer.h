/*
 * DroidScreen Windows - Windows.Graphics.Capture screen capturer
 *
 * Uses the WinRT Graphics Capture API (Windows 10 1903+) to capture
 * the desktop at up to 60 fps. Frames are delivered as ID3D11Texture2D*
 * via the FramePool.FrameArrived event.
 */

#pragma once

#include "droidscreen/capturer.h"

#include <array>
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

    /// Caps how often frames are delivered into the pipeline. This is applied
    /// before the costly D3D copy into our own frame slots.
    void set_target_fps(uint32_t fps);

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
    uint64_t frames_rate_limited() const { return frames_rate_limited_.load(); }

private:
    /// Called by the FramePool.FrameArrived event.
    void on_frame_arrived();
    struct FrameSlot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        uint32_t width = 0;
        uint32_t height = 0;
        std::atomic<bool> in_use{false};
    };

    FrameSlot* acquire_frame_slot(uint32_t width, uint32_t height, DXGI_FORMAT format);
    static void release_frame_slot(void* release_ctx, void* native_handle);
    void reset_frame_slots();

    uint32_t width_  = 0;
    uint32_t height_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<int64_t> min_frame_interval_us_{0};
    std::atomic<int64_t> last_delivered_ts_us_{0};
    std::atomic<uint64_t> frames_rate_limited_{0};

    std::function<void(const CapturedFrame&)> on_frame_;

    // D3D11 resources.
    Microsoft::WRL::ComPtr<ID3D11Device>        device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>  context_;
    Microsoft::WRL::ComPtr<IDXGIOutput1>         output_;

    // WinRT capture objects — stored via pimpl to keep WinRT headers
    // out of this header. Defined in the .cpp.
    struct WinRTState;
    std::unique_ptr<WinRTState> wrt_;

    std::mutex frame_mutex_;
    std::mutex frame_pool_mutex_;
    static constexpr size_t kFrameSlotCount = 4;
    std::array<FrameSlot, kFrameSlotCount> frame_slots_{};
    size_t next_frame_slot_ = 0;

    // Mutex protecting D3D11 immediate context access. Shared with
    // the encoder to prevent concurrent context calls from the WGC
    // callback thread and the encode thread.
    std::mutex d3d_mutex_;
};

} // namespace droidscreen
