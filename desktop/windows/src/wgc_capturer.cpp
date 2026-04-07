/*
 * DroidScreen Windows - WGC capturer implementation
 *
 * Uses Windows.Graphics.Capture (WinRT) to capture the desktop.
 * Requires Windows 10 version 1903 or later.
 *
 * The capture pipeline:
 *   1. Create a D3D11 device and enumerate DXGI outputs (monitors).
 *   2. Create a GraphicsCaptureItem for the chosen monitor via
 *      the interop factory (IGraphicsCaptureItemInterop).
 *   3. Create a Direct3D11CaptureFramePool with a free-threaded pool
 *      of 2 frames (double-buffered).
 *   4. Start a GraphicsCaptureSession.
 *   5. On FrameArrived, copy the captured texture to a staging
 *      texture and invoke the user callback.
 */

#include "wgc_capturer.h"

#include <cstdio>
#include <chrono>

// WinRT headers
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

// Interop for creating capture items from HMONITOR
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <dxgi.h>

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

namespace droidscreen {

// ---------------------------------------------------------------------------
// Pimpl: WinRT state stored here so the header stays free of WinRT includes.
// WinRT projection types delete operator new, but they're fine as struct
// members — the struct itself is heap-allocated via std::make_unique.
// ---------------------------------------------------------------------------

struct WGCCapturer::WinRTState {
    GraphicsCaptureItem           item{nullptr};
    Direct3D11CaptureFramePool    pool{nullptr};
    GraphicsCaptureSession        session{nullptr};
    winrt::event_token            frame_arrived_token{};
};

// ---------------------------------------------------------------------------
// Helpers: D3D11 <-> WinRT IDirect3DDevice interop
// ---------------------------------------------------------------------------

/// Wrap an ID3D11Device as a WinRT IDirect3DDevice via the DXGI interop path.
static IDirect3DDevice create_winrt_device(ID3D11Device* d3d_device) {
    // Get the DXGI device from the D3D11 device.
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    HRESULT hr = d3d_device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
    if (FAILED(hr)) {
        fprintf(stderr, "[wgc] QueryInterface(IDXGIDevice) failed: 0x%08lx\n", hr);
        return nullptr;
    }

    // Use CreateDirect3D11DeviceFromDXGIDevice to get a WinRT device.
    winrt::com_ptr<::IInspectable> inspectable;
    hr = CreateDirect3D11DeviceFromDXGIDevice(
        dxgi_device.Get(), inspectable.put());
    if (FAILED(hr)) {
        fprintf(stderr, "[wgc] CreateDirect3D11DeviceFromDXGIDevice failed: "
                "0x%08lx\n", hr);
        return nullptr;
    }

    return inspectable.as<IDirect3DDevice>();
}

/// Extract the ID3D11Texture2D from a WinRT IDirect3DSurface.
static Microsoft::WRL::ComPtr<ID3D11Texture2D>
get_texture_from_surface(const IDirect3DSurface& surface) {
    auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = access->GetInterface(IID_PPV_ARGS(&texture));
    if (FAILED(hr)) {
        fprintf(stderr, "[wgc] GetInterface(ID3D11Texture2D) failed: "
                "0x%08lx\n", hr);
        return nullptr;
    }
    return texture;
}

// ---------------------------------------------------------------------------
// WGCCapturer implementation
// ---------------------------------------------------------------------------

WGCCapturer::WGCCapturer() = default;

WGCCapturer::~WGCCapturer() {
    stop();
}

void WGCCapturer::set_target_fps(uint32_t fps) {
    if (fps == 0) {
        min_frame_interval_us_.store(0, std::memory_order_relaxed);
        return;
    }

    min_frame_interval_us_.store(
        static_cast<int64_t>(1000000ull / fps), std::memory_order_relaxed);
}

WGCCapturer::FrameSlot* WGCCapturer::acquire_frame_slot(
        uint32_t width, uint32_t height, DXGI_FORMAT format) {
    std::lock_guard<std::mutex> lock(frame_pool_mutex_);

    for (size_t i = 0; i < kFrameSlotCount; ++i) {
        const size_t slot_index = (next_frame_slot_ + i) % kFrameSlotCount;
        auto& slot = frame_slots_[slot_index];

        bool expected = false;
        if (!slot.in_use.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            continue;
        }

        if (!slot.texture ||
            slot.width != width ||
            slot.height != height ||
            slot.format != format) {
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width            = width;
            desc.Height           = height;
            desc.MipLevels        = 1;
            desc.ArraySize        = 1;
            desc.Format           = format;
            desc.SampleDesc.Count = 1;
            desc.Usage            = D3D11_USAGE_DEFAULT;
            desc.BindFlags        = 0;

            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            HRESULT hr = device_->CreateTexture2D(
                &desc, nullptr, texture.ReleaseAndGetAddressOf());
            if (FAILED(hr)) {
                fprintf(stderr,
                        "[wgc] CreateTexture2D (slot %zu, %ux%u fmt=%u) failed: 0x%08lx\n",
                        slot_index, width, height, format, hr);
                slot.in_use.store(false, std::memory_order_release);
                return nullptr;
            }

            slot.texture = std::move(texture);
            slot.width = width;
            slot.height = height;
            slot.format = format;
        }

        next_frame_slot_ = (slot_index + 1) % kFrameSlotCount;
        return &slot;
    }

    static bool warned = false;
    if (!warned) {
        fprintf(stderr, "[wgc] frame slot pool exhausted; dropping frames\n");
        warned = true;
    }
    return nullptr;
}

void WGCCapturer::release_frame_slot(void* release_ctx, void* /*native_handle*/) {
    if (!release_ctx) return;
    auto* slot = static_cast<FrameSlot*>(release_ctx);
    slot->in_use.store(false, std::memory_order_release);
}

void WGCCapturer::reset_frame_slots() {
    std::lock_guard<std::mutex> lock(frame_pool_mutex_);
    for (auto& slot : frame_slots_) {
        slot.texture.Reset();
        slot.format = DXGI_FORMAT_UNKNOWN;
        slot.width = 0;
        slot.height = 0;
        slot.in_use.store(false, std::memory_order_release);
    }
    next_frame_slot_ = 0;
}

bool WGCCapturer::init(uint32_t display_index) {
    reset_frame_slots();

    // Create D3D11 device with BGRA support (required for WGC).
    UINT creation_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifndef NDEBUG
    creation_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    HRESULT hr = D3D11CreateDevice(
        nullptr,                       // default adapter
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,                       // no software rasterizer
        creation_flags,
        feature_levels,
        _countof(feature_levels),
        D3D11_SDK_VERSION,
        device_.ReleaseAndGetAddressOf(),
        nullptr,                       // actual feature level out
        context_.ReleaseAndGetAddressOf());

    if (FAILED(hr)) {
        fprintf(stderr, "[wgc] D3D11CreateDevice failed: 0x%08lx\n", hr);
        return false;
    }

    // Enumerate DXGI outputs (monitors).
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    hr = device_->QueryInterface(IID_PPV_ARGS(&dxgi_device));
    if (FAILED(hr)) {
        fprintf(stderr, "[wgc] QueryInterface(IDXGIDevice) failed: 0x%08lx\n", hr);
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_device->GetAdapter(adapter.GetAddressOf());
    if (FAILED(hr)) {
        fprintf(stderr, "[wgc] GetAdapter failed: 0x%08lx\n", hr);
        return false;
    }

    // Walk outputs to find the requested display index.
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    HMONITOR target_monitor = nullptr;

    UINT output_idx = 0;
    UINT found_count = 0;
    while (adapter->EnumOutputs(output_idx, output.ReleaseAndGetAddressOf()) !=
           DXGI_ERROR_NOT_FOUND) {
        DXGI_OUTPUT_DESC desc;
        hr = output->GetDesc(&desc);
        if (SUCCEEDED(hr)) {
            fprintf(stderr, "[wgc] display %u: %ls (%dx%d)\n",
                    found_count,
                    desc.DeviceName,
                    desc.DesktopCoordinates.right - desc.DesktopCoordinates.left,
                    desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top);

            if (found_count == display_index) {
                target_monitor = desc.Monitor;
                width_  = static_cast<uint32_t>(
                    desc.DesktopCoordinates.right - desc.DesktopCoordinates.left);
                height_ = static_cast<uint32_t>(
                    desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top);
            }
            found_count++;
        }
        output_idx++;
    }

    if (!target_monitor) {
        fprintf(stderr, "[wgc] display index %u out of range "
                "(have %u displays)\n", display_index, found_count);
        return false;
    }

    // Create a GraphicsCaptureItem for the monitor via interop.
    auto interop_factory = winrt::get_activation_factory<
        GraphicsCaptureItem,
        IGraphicsCaptureItemInterop>();

    GraphicsCaptureItem item{nullptr};
    hr = interop_factory->CreateForMonitor(
        target_monitor,
        winrt::guid_of<GraphicsCaptureItem>(),
        winrt::put_abi(item));

    if (FAILED(hr) || !item) {
        fprintf(stderr, "[wgc] CreateForMonitor failed: 0x%08lx\n", hr);
        return false;
    }

    // Create the frame pool (2 frames, BGRA).
    IDirect3DDevice winrt_device = create_winrt_device(device_.Get());
    if (!winrt_device) {
        return false;
    }

    auto pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        winrt_device,
        DirectXPixelFormat::B8G8R8A8UIntNormalized,
        2,  // number of frames in pool
        {static_cast<int32_t>(width_), static_cast<int32_t>(height_)});

    if (!pool) {
        fprintf(stderr, "[wgc] CreateFreeThreaded frame pool failed\n");
        return false;
    }

    // Create the capture session.
    auto session = pool.CreateCaptureSession(item);
    if (!session) {
        fprintf(stderr, "[wgc] CreateCaptureSession failed\n");
        return false;
    }

    // Disable the yellow capture border (Windows 11 / 10 20H1+).
    // This property may not exist on older builds, so we ignore failure.
    try {
        session.IsBorderRequired(false);
    } catch (...) {
        fprintf(stderr, "[wgc] IsBorderRequired not supported on this build\n");
    }

    // Avoid cursor-only invalidations. Input is injected separately, so the
    // cursor does not need to be baked into the video stream.
    try {
        session.IsCursorCaptureEnabled(false);
    } catch (...) {
        // Not available on older builds.
    }

    // Store WinRT objects in the pimpl struct (no operator new issues).
    wrt_ = std::make_unique<WinRTState>();
    wrt_->item    = item;
    wrt_->pool    = pool;
    wrt_->session = session;

    fprintf(stderr, "[wgc] initialized for display %u (%ux%u)\n",
            display_index, width_, height_);
    return true;
}

bool WGCCapturer::init_with_monitor(HMONITOR monitor) {
    if (!monitor) {
        fprintf(stderr, "[wgc] init_with_monitor: null HMONITOR\n");
        return false;
    }

    reset_frame_slots();

    // Create D3D11 device with BGRA support (required for WGC).
    UINT creation_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifndef NDEBUG
    creation_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        creation_flags,
        feature_levels,
        _countof(feature_levels),
        D3D11_SDK_VERSION,
        device_.ReleaseAndGetAddressOf(),
        nullptr,
        context_.ReleaseAndGetAddressOf());

    if (FAILED(hr)) {
        fprintf(stderr, "[wgc] D3D11CreateDevice failed: 0x%08lx\n", hr);
        return false;
    }

    // Get monitor dimensions.
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(monitor, &mi)) {
        fprintf(stderr, "[wgc] GetMonitorInfo failed\n");
        return false;
    }

    width_  = static_cast<uint32_t>(mi.rcMonitor.right - mi.rcMonitor.left);
    height_ = static_cast<uint32_t>(mi.rcMonitor.bottom - mi.rcMonitor.top);

    fprintf(stderr, "[wgc] init_with_monitor: %ls (%ux%u)\n",
            mi.szDevice, width_, height_);

    // Create a GraphicsCaptureItem for the monitor via interop.
    auto interop_factory = winrt::get_activation_factory<
        GraphicsCaptureItem,
        IGraphicsCaptureItemInterop>();

    GraphicsCaptureItem item{nullptr};
    hr = interop_factory->CreateForMonitor(
        monitor,
        winrt::guid_of<GraphicsCaptureItem>(),
        winrt::put_abi(item));

    if (FAILED(hr) || !item) {
        fprintf(stderr, "[wgc] CreateForMonitor failed: 0x%08lx\n", hr);
        return false;
    }

    // Create the frame pool (2 frames, BGRA).
    IDirect3DDevice winrt_device = create_winrt_device(device_.Get());
    if (!winrt_device) {
        return false;
    }

    auto pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        winrt_device,
        DirectXPixelFormat::B8G8R8A8UIntNormalized,
        2,
        {static_cast<int32_t>(width_), static_cast<int32_t>(height_)});

    if (!pool) {
        fprintf(stderr, "[wgc] CreateFreeThreaded frame pool failed\n");
        return false;
    }

    // Create the capture session.
    auto session = pool.CreateCaptureSession(item);
    if (!session) {
        fprintf(stderr, "[wgc] CreateCaptureSession failed\n");
        return false;
    }

    // Disable the yellow capture border (Windows 11 / 10 20H1+).
    try {
        session.IsBorderRequired(false);
    } catch (...) {
        fprintf(stderr, "[wgc] IsBorderRequired not supported on this build\n");
    }

    // Avoid cursor-only invalidations. Input is injected separately, so the
    // cursor does not need to be baked into the video stream.
    try {
        session.IsCursorCaptureEnabled(false);
    } catch (...) {
    }

    // Store WinRT objects.
    wrt_ = std::make_unique<WinRTState>();
    wrt_->item    = item;
    wrt_->pool    = pool;
    wrt_->session = session;

    fprintf(stderr, "[wgc] initialized for HMONITOR %p (%ux%u)\n",
            monitor, width_, height_);
    return true;
}

bool WGCCapturer::start(std::function<void(const CapturedFrame&)> on_frame) {
    if (!wrt_) {
        fprintf(stderr, "[wgc] not initialized\n");
        return false;
    }

    on_frame_ = std::move(on_frame);
    running_.store(true);
    last_delivered_ts_us_.store(0, std::memory_order_relaxed);
    frames_rate_limited_.store(0, std::memory_order_relaxed);

    // Subscribe to the FrameArrived event.
    wrt_->frame_arrived_token = wrt_->pool.FrameArrived(
        [this](Direct3D11CaptureFramePool const& /*sender*/,
               winrt::Windows::Foundation::IInspectable const&) {
            on_frame_arrived();
        });

    // Start the capture session.
    wrt_->session.StartCapture();

    fprintf(stderr, "[wgc] capture started\n");
    return true;
}

void WGCCapturer::on_frame_arrived() {
    if (!running_.load()) return;

    std::lock_guard<std::mutex> lock(frame_mutex_);

    auto frame = wrt_->pool.TryGetNextFrame();
    if (!frame) return;

    // Get the surface and extract the D3D11 texture.
    auto surface = frame.Surface();
    auto source_texture = get_texture_from_surface(surface);
    if (!source_texture) {
        frame.Close();
        return;
    }

    // Get the content size (may differ from pool size if display resolution
    // changed during capture).
    auto content_size = frame.ContentSize();
    uint32_t frame_w = static_cast<uint32_t>(content_size.Width);
    uint32_t frame_h = static_cast<uint32_t>(content_size.Height);

    // Log diagnostics on the very first frame.
    static bool first_frame_logged = false;
    if (!first_frame_logged) {
        D3D11_TEXTURE2D_DESC src_desc = {};
        source_texture->GetDesc(&src_desc);
        fprintf(stderr, "[wgc] first frame: texture %ux%u fmt=%u, "
                "content %ux%u, expected %ux%u\n",
                src_desc.Width, src_desc.Height, src_desc.Format,
                frame_w, frame_h, width_, height_);
        first_frame_logged = true;
    }

    // Compute a timestamp in microseconds from the system timestamp.
    auto sys_relative = frame.SystemRelativeTime();
    int64_t timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
        sys_relative).count();

    const int64_t min_interval_us =
        min_frame_interval_us_.load(std::memory_order_relaxed);
    const int64_t last_delivered_us =
        last_delivered_ts_us_.load(std::memory_order_relaxed);
    if (min_interval_us > 0 && last_delivered_us > 0 &&
        (timestamp_us - last_delivered_us) < min_interval_us) {
        frames_rate_limited_.fetch_add(1, std::memory_order_relaxed);
        frame.Close();
        return;
    }

    // Copy the captured texture to the staging texture.
    // We must release the frame as quickly as possible to avoid stalling
    // the frame pool.
    D3D11_BOX src_box = {};
    src_box.left   = 0;
    src_box.top    = 0;
    src_box.front  = 0;
    src_box.right  = (frame_w < width_)  ? frame_w : width_;
    src_box.bottom = (frame_h < height_) ? frame_h : height_;
    src_box.back   = 1;

    D3D11_TEXTURE2D_DESC src_desc = {};
    source_texture->GetDesc(&src_desc);
    auto* frame_slot = acquire_frame_slot(
        src_box.right, src_box.bottom, src_desc.Format);
    if (!frame_slot) {
        frame.Close();
        return;
    }

    // Lock the D3D11 context mutex — the immediate context is NOT
    // thread-safe and the encode thread also uses it for CopyResource/Map.
    {
        std::lock_guard<std::mutex> d3d_lock(d3d_mutex_);
        context_->CopySubresourceRegion(
            frame_slot->texture.Get(), 0, // dst subresource, x, y, z
            0, 0, 0,
            source_texture.Get(), 0,     // src subresource
            &src_box);
    }

    // Release the WGC frame immediately.
    frame.Close();
    last_delivered_ts_us_.store(timestamp_us, std::memory_order_relaxed);

    // Determine if the frame is idle (WGC doesn't provide an idle flag
    // directly, so we always report not-idle; the pipeline handles
    // duplicate detection if needed).
    CapturedFrame captured;
    captured.native_handle = frame_slot->texture.Get();
    captured.release_ctx = frame_slot;
    captured.release_fn = &WGCCapturer::release_frame_slot;
    captured.width         = src_box.right;
    captured.height        = src_box.bottom;
    captured.timestamp_us  = timestamp_us;
    captured.is_idle       = false;

    if (on_frame_) {
        on_frame_(captured);
    } else {
        release_frame_slot(frame_slot, frame_slot->texture.Get());
    }
}

void WGCCapturer::stop() {
    if (!running_.exchange(false)) return;

    if (wrt_) {
        // Revoke the FrameArrived event handler.
        wrt_->pool.FrameArrived(wrt_->frame_arrived_token);

        // Close the session and pool.
        wrt_->session.Close();
        wrt_->pool.Close();

        // Release all WinRT objects.
        wrt_.reset();
    }
    fprintf(stderr, "[wgc] capture stopped (rate-limited=%llu)\n",
            static_cast<unsigned long long>(
                frames_rate_limited_.load(std::memory_order_relaxed)));
}

} // namespace droidscreen
