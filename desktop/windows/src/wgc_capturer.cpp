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

bool WGCCapturer::init(uint32_t display_index) {
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

    // Store the capture item (type-erased to keep the header clean).
    capture_item_ = new GraphicsCaptureItem(item);

    // Create the staging texture for frame copies.
    D3D11_TEXTURE2D_DESC staging_desc = {};
    staging_desc.Width            = width_;
    staging_desc.Height           = height_;
    staging_desc.MipLevels        = 1;
    staging_desc.ArraySize        = 1;
    staging_desc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.Usage            = D3D11_USAGE_DEFAULT;
    staging_desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    hr = device_->CreateTexture2D(&staging_desc, nullptr,
                                   staging_texture_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        fprintf(stderr, "[wgc] CreateTexture2D (staging) failed: 0x%08lx\n", hr);
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

    frame_pool_ = new Direct3D11CaptureFramePool(pool);

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

    // Disable cursor rendering in the capture (optional).
    try {
        session.IsCursorCaptureEnabled(true);
    } catch (...) {
        // Not available on older builds.
    }

    capture_session_ = new GraphicsCaptureSession(session);

    fprintf(stderr, "[wgc] initialized for display %u (%ux%u)\n",
            display_index, width_, height_);
    return true;
}

bool WGCCapturer::start(std::function<void(const CapturedFrame&)> on_frame) {
    if (!frame_pool_ || !capture_session_) {
        fprintf(stderr, "[wgc] not initialized\n");
        return false;
    }

    on_frame_ = std::move(on_frame);
    running_.store(true);

    auto* pool = static_cast<Direct3D11CaptureFramePool*>(frame_pool_);

    // Subscribe to the FrameArrived event.
    auto token = pool->FrameArrived(
        [this](Direct3D11CaptureFramePool const& sender,
               winrt::Windows::Foundation::IInspectable const&) {
            on_frame_arrived();
        });

    // Store the token for later revocation.
    frame_arrived_token_ = new winrt::event_token(token);

    // Start the capture session.
    auto* session = static_cast<GraphicsCaptureSession*>(capture_session_);
    session->StartCapture();

    fprintf(stderr, "[wgc] capture started\n");
    return true;
}

void WGCCapturer::on_frame_arrived() {
    if (!running_.load()) return;

    std::lock_guard<std::mutex> lock(frame_mutex_);

    auto* pool = static_cast<Direct3D11CaptureFramePool*>(frame_pool_);
    auto frame = pool->TryGetNextFrame();
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

    // Compute a timestamp in microseconds from the system timestamp.
    auto sys_relative = frame.SystemRelativeTime();
    int64_t timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
        sys_relative).count();

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

    context_->CopySubresourceRegion(
        staging_texture_.Get(), 0,   // dst subresource, x, y, z
        0, 0, 0,
        source_texture.Get(), 0,     // src subresource
        &src_box);

    // Release the WGC frame immediately.
    frame.Close();

    // Determine if the frame is idle (WGC doesn't provide an idle flag
    // directly, so we always report not-idle; the pipeline handles
    // duplicate detection if needed).
    CapturedFrame captured;
    captured.native_handle = staging_texture_.Get();
    captured.width         = src_box.right;
    captured.height        = src_box.bottom;
    captured.timestamp_us  = timestamp_us;
    captured.is_idle       = false;

    if (on_frame_) {
        on_frame_(captured);
    }
}

void WGCCapturer::stop() {
    if (!running_.exchange(false)) return;

    // Revoke the FrameArrived event handler.
    if (frame_pool_ && frame_arrived_token_) {
        auto* pool = static_cast<Direct3D11CaptureFramePool*>(frame_pool_);
        auto* token = static_cast<winrt::event_token*>(frame_arrived_token_);
        pool->FrameArrived(*token);
        delete token;
        frame_arrived_token_ = nullptr;
    }

    // Close the capture session.
    if (capture_session_) {
        auto* session = static_cast<GraphicsCaptureSession*>(capture_session_);
        session->Close();
        delete session;
        capture_session_ = nullptr;
    }

    // Close the frame pool.
    if (frame_pool_) {
        auto* pool = static_cast<Direct3D11CaptureFramePool*>(frame_pool_);
        pool->Close();
        delete pool;
        frame_pool_ = nullptr;
    }

    // Clean up the capture item.
    if (capture_item_) {
        auto* item = static_cast<GraphicsCaptureItem*>(capture_item_);
        delete item;
        capture_item_ = nullptr;
    }

    // Release the staging texture.
    staging_texture_.Reset();

    fprintf(stderr, "[wgc] capture stopped\n");
}

} // namespace droidscreen
