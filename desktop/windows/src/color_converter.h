/*
 * DroidScreen Windows - BGRA-to-NV12 color converter
 *
 * Uses D3D11 pixel shaders to convert BGRA input textures to NV12
 * format for NVENC. The conversion runs entirely on the GPU via two
 * render passes:
 *
 *   Pass 1 (Y plane):  Renders to a full-resolution R8_UNORM render
 *                       target. The pixel shader computes luminance
 *                       using BT.709 coefficients with limited range.
 *
 *   Pass 2 (UV plane):  Renders to a half-resolution R8G8_UNORM render
 *                        target. The shader computes chroma (Cb, Cr)
 *                        at half resolution.
 *
 * The Y and UV planes are then copied into a single DXGI_FORMAT_NV12
 * texture that NVENC can consume directly.
 */

#pragma once

#include <cstdint>
#include <d3d11.h>
#include <wrl/client.h>

namespace droidscreen {

class ColorConverter {
public:
    ColorConverter();
    ~ColorConverter();

    /// Initialize GPU resources for the converter.
    /// @param device D3D11 device to use.
    /// @param width  Frame width in pixels.
    /// @param height Frame height in pixels.
    /// @return true on success.
    bool init(ID3D11Device* device, uint32_t width, uint32_t height);

    /// Convert a BGRA texture to NV12.
    /// @param input_bgra  Source BGRA texture.
    /// @param output_nv12 Receives a pointer to the NV12 output texture.
    ///                    The texture is owned by the converter and valid
    ///                    until the next convert() or shutdown() call.
    /// @return true on success.
    bool convert(ID3D11Texture2D* input_bgra, ID3D11Texture2D** output_nv12);

    /// Release all GPU resources.
    void shutdown();

    /// Get the NV12 output texture (for NVENC resource registration).
    ID3D11Texture2D* nv12_texture() const { return nv12_texture_.Get(); }

private:
    /// Compile and create pixel/vertex shaders.
    bool create_shaders();

    /// Create render targets and textures.
    bool create_textures();

    /// Create the sampler, rasterizer, and blend states.
    bool create_pipeline_state();

    /// Run a full-screen pass with the given pixel shader and RTV.
    void draw_fullscreen(ID3D11PixelShader* ps,
                         ID3D11RenderTargetView* rtv,
                         uint32_t vp_width, uint32_t vp_height);

    Microsoft::WRL::ComPtr<ID3D11Device>        device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>  context_;

    uint32_t width_  = 0;
    uint32_t height_ = 0;

    // Shaders.
    Microsoft::WRL::ComPtr<ID3D11VertexShader>  vs_fullscreen_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>   ps_y_plane_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>   ps_uv_plane_;

    // Input SRV (created per-convert from the BGRA texture).
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> input_srv_;

    // Y plane: full-resolution R8_UNORM.
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          y_texture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>   y_rtv_;

    // UV plane: half-resolution R8G8_UNORM.
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          uv_texture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>   uv_rtv_;

    // NV12 output texture (DXGI_FORMAT_NV12) for NVENC.
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          nv12_texture_;

    // Pipeline state objects.
    Microsoft::WRL::ComPtr<ID3D11SamplerState>       sampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>    rasterizer_;
    Microsoft::WRL::ComPtr<ID3D11BlendState>         blend_state_;
};

} // namespace droidscreen
