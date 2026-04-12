/*
 * DroidScreen Windows - GPU color converter (BGRA/FP16 -> NV12)
 *
 * Converts captured D3D11 textures to NV12 entirely on the GPU using
 * HLSL pixel shaders, eliminating CPU readback and CPU color conversion.
 *
 * Pipeline per convert() call:
 *   Pass 1: Y plane pixel shader  (full resolution)  -> R8_UNORM  view of NV12
 *   Pass 2: UV plane pixel shader (half resolution)  -> R8G8_UNORM view of NV12
 *
 * SDR path (BGRA input):  direct RGB -> YUV color matrix
 * HDR path (FP16 input):  Reinhard tonemap + sRGB gamma + RGB -> YUV color matrix
 *
 * Uses BT.601 limited range coefficients (Y: 16-235, UV: 16-240).
 */

#pragma once

#include <cstdint>

#include <d3d11.h>
#include <wrl/client.h>

namespace droidscreen {

class GpuColorConverter {
public:
    GpuColorConverter() = default;
    ~GpuColorConverter();

    /// Initialize shaders and GPU resources.
    /// @param device   D3D11 device (shared with capturer and encoder).
    /// @param context  D3D11 immediate device context.
    /// @param width    Output NV12 width in pixels.
    /// @param height   Output NV12 height in pixels.
    /// @return true on success.
    bool init(ID3D11Device* device, ID3D11DeviceContext* context,
              uint32_t width, uint32_t height);

    /// Convert a BGRA or FP16 source texture to NV12 on the GPU.
    ///
    /// The returned texture is owned by this class and valid until the
    /// next convert() call.
    ///
    /// IMPORTANT: Caller must hold the D3D11 context lock if the context
    /// is shared between threads.
    ///
    /// @param source         Source D3D11 texture (BGRA or FP16 scRGB).
    /// @param source_format  DXGI format of the source texture.
    /// @return Pointer to the NV12 output texture, or nullptr on error.
    ID3D11Texture2D* convert(ID3D11Texture2D* source, DXGI_FORMAT source_format);

    /// Release all GPU resources.
    void shutdown();

    /// Returns true if the converter was initialized successfully.
    bool is_initialized() const { return initialized_; }

private:
    bool compile_shaders();
    bool create_nv12_resources();

    Microsoft::WRL::ComPtr<ID3D11Device>        device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>  context_;

    // Shaders.
    Microsoft::WRL::ComPtr<ID3D11VertexShader>  vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>   ps_y_sdr_;    // BGRA -> Y
    Microsoft::WRL::ComPtr<ID3D11PixelShader>   ps_uv_sdr_;   // BGRA -> UV (chroma subsample)
    Microsoft::WRL::ComPtr<ID3D11PixelShader>   ps_y_hdr_;    // FP16 scRGB -> Y (tonemap)
    Microsoft::WRL::ComPtr<ID3D11PixelShader>   ps_uv_hdr_;   // FP16 scRGB -> UV (tonemap)

    // Pipeline state objects.
    Microsoft::WRL::ComPtr<ID3D11SamplerState>  sampler_;
    Microsoft::WRL::ComPtr<ID3D11Buffer>        color_cb_;

    // NV12 output texture with per-plane render target views.
    Microsoft::WRL::ComPtr<ID3D11Texture2D>       nv12_texture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> y_rtv_;    // R8_UNORM  (Y plane)
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> uv_rtv_;   // R8G8_UNORM (UV plane)

    uint32_t width_  = 0;
    uint32_t height_ = 0;
    bool initialized_ = false;
};

} // namespace droidscreen
