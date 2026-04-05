/*
 * DroidScreen Windows - Color converter implementation
 *
 * Converts BGRA textures to NV12 using two GPU render passes.
 *
 * The HLSL shaders are compiled at runtime via D3DCompile. In a
 * shipping build you would pre-compile them, but runtime compilation
 * simplifies the build and is fast enough for a one-time init cost.
 */

#include "color_converter.h"

#include <cstdio>
#include <d3dcompiler.h>

namespace droidscreen {

// ---------------------------------------------------------------------------
// Inline HLSL source
// ---------------------------------------------------------------------------

static const char* kVertexShaderSource = R"hlsl(
/*
 * Fullscreen triangle vertex shader.
 *
 * Generates a full-screen triangle from SV_VertexID (0,1,2) without
 * any vertex buffer. The triangle covers the entire viewport with
 * UV coordinates in [0,1].
 */
struct VS_OUT {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VS_OUT main(uint id : SV_VertexID) {
    VS_OUT o;
    // Generate a triangle that covers [-1,1] x [-1,1]:
    //   id=0 -> (-1, -1)  uv=(0, 1)
    //   id=1 -> (-1,  3)  uv=(0,-1)
    //   id=2 -> ( 3, -1)  uv=(2, 1)
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
)hlsl";

static const char* kYPlaneShaderSource = R"hlsl(
/*
 * Y-plane pixel shader (BGRA -> Y).
 *
 * BT.709 limited range:
 *   Y = 16 + 219 * (0.2126*R + 0.7152*G + 0.0722*B)
 * Output as R8_UNORM, so we write Y/255.
 */
Texture2D<float4> InputTexture : register(t0);
SamplerState      LinearSampler : register(s0);

float main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float4 rgba = InputTexture.Sample(LinearSampler, uv);
    float y_linear = 0.2126 * rgba.r + 0.7152 * rgba.g + 0.0722 * rgba.b;
    float y = (16.0 + 219.0 * saturate(y_linear)) / 255.0;
    return y;
}
)hlsl";

static const char* kUVPlaneShaderSource = R"hlsl(
/*
 * UV-plane pixel shader (BGRA -> CbCr).
 *
 * BT.709 limited range:
 *   Y  = 0.2126*R + 0.7152*G + 0.0722*B
 *   Cb = 128 + 224 * 0.5389 * (B - Y)  (clamped to [16,240])
 *   Cr = 128 + 224 * 0.6350 * (R - Y)  (clamped to [16,240])
 *
 * This runs at half resolution. We sample the center of the 2x2 block.
 * Output as R8G8_UNORM: R=Cb/255, G=Cr/255.
 */
Texture2D<float4> InputTexture : register(t0);
SamplerState      LinearSampler : register(s0);

float2 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float4 rgba = InputTexture.Sample(LinearSampler, uv);
    float y_linear = 0.2126 * rgba.r + 0.7152 * rgba.g + 0.0722 * rgba.b;

    float cb = 128.0 + 224.0 * 0.5389 * (rgba.b - y_linear);
    float cr = 128.0 + 224.0 * 0.6350 * (rgba.r - y_linear);

    cb = clamp(cb, 16.0, 240.0);
    cr = clamp(cr, 16.0, 240.0);

    return float2(cb / 255.0, cr / 255.0);
}
)hlsl";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static Microsoft::WRL::ComPtr<ID3DBlob> compile_shader(
        const char* source, const char* entry, const char* target) {
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;

    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#ifndef NDEBUG
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    HRESULT hr = D3DCompile(
        source,
        strlen(source),
        nullptr,   // source name
        nullptr,   // defines
        nullptr,   // include
        entry,
        target,
        flags,
        0,
        blob.GetAddressOf(),
        errors.GetAddressOf());

    if (FAILED(hr)) {
        if (errors) {
            fprintf(stderr, "[color] shader compile error (%s): %s\n",
                    target,
                    static_cast<const char*>(errors->GetBufferPointer()));
        } else {
            fprintf(stderr, "[color] shader compile failed: 0x%08lx\n", hr);
        }
        return nullptr;
    }

    return blob;
}

// ---------------------------------------------------------------------------
// ColorConverter implementation
// ---------------------------------------------------------------------------

ColorConverter::ColorConverter() = default;

ColorConverter::~ColorConverter() {
    shutdown();
}

bool ColorConverter::init(ID3D11Device* device, uint32_t width, uint32_t height) {
    if (!device || width == 0 || height == 0) return false;

    device_ = device;
    device_->GetImmediateContext(context_.ReleaseAndGetAddressOf());
    width_  = width;
    height_ = height;

    if (!create_shaders())       return false;
    if (!create_textures())      return false;
    if (!create_pipeline_state()) return false;

    fprintf(stderr, "[color] converter initialized: %ux%u\n", width, height);
    return true;
}

bool ColorConverter::create_shaders() {
    // Compile vertex shader.
    auto vs_blob = compile_shader(kVertexShaderSource, "main", "vs_5_0");
    if (!vs_blob) return false;

    HRESULT hr = device_->CreateVertexShader(
        vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
        nullptr, vs_fullscreen_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        fprintf(stderr, "[color] CreateVertexShader failed: 0x%08lx\n", hr);
        return false;
    }

    // Compile Y-plane pixel shader.
    auto ps_y_blob = compile_shader(kYPlaneShaderSource, "main", "ps_5_0");
    if (!ps_y_blob) return false;

    hr = device_->CreatePixelShader(
        ps_y_blob->GetBufferPointer(), ps_y_blob->GetBufferSize(),
        nullptr, ps_y_plane_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        fprintf(stderr, "[color] CreatePixelShader (Y) failed: 0x%08lx\n", hr);
        return false;
    }

    // Compile UV-plane pixel shader.
    auto ps_uv_blob = compile_shader(kUVPlaneShaderSource, "main", "ps_5_0");
    if (!ps_uv_blob) return false;

    hr = device_->CreatePixelShader(
        ps_uv_blob->GetBufferPointer(), ps_uv_blob->GetBufferSize(),
        nullptr, ps_uv_plane_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        fprintf(stderr, "[color] CreatePixelShader (UV) failed: 0x%08lx\n", hr);
        return false;
    }

    return true;
}

bool ColorConverter::create_textures() {
    HRESULT hr;

    // Y plane: full-resolution R8_UNORM render target.
    {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width            = width_;
        desc.Height           = height_;
        desc.MipLevels        = 1;
        desc.ArraySize        = 1;
        desc.Format           = DXGI_FORMAT_R8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage            = D3D11_USAGE_DEFAULT;
        desc.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        hr = device_->CreateTexture2D(&desc, nullptr,
                                       y_texture_.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            fprintf(stderr, "[color] CreateTexture2D (Y) failed: 0x%08lx\n", hr);
            return false;
        }

        hr = device_->CreateRenderTargetView(
            y_texture_.Get(), nullptr,
            y_rtv_.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            fprintf(stderr, "[color] CreateRTV (Y) failed: 0x%08lx\n", hr);
            return false;
        }
    }

    // UV plane: half-resolution R8G8_UNORM render target.
    {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width            = width_ / 2;
        desc.Height           = height_ / 2;
        desc.MipLevels        = 1;
        desc.ArraySize        = 1;
        desc.Format           = DXGI_FORMAT_R8G8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage            = D3D11_USAGE_DEFAULT;
        desc.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        hr = device_->CreateTexture2D(&desc, nullptr,
                                       uv_texture_.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            fprintf(stderr, "[color] CreateTexture2D (UV) failed: 0x%08lx\n", hr);
            return false;
        }

        hr = device_->CreateRenderTargetView(
            uv_texture_.Get(), nullptr,
            uv_rtv_.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            fprintf(stderr, "[color] CreateRTV (UV) failed: 0x%08lx\n", hr);
            return false;
        }
    }

    // NV12 output texture for NVENC.
    // DXGI_FORMAT_NV12 is a planar format: Y plane (full res) followed by
    // interleaved UV plane (half res). NVENC can consume it directly.
    {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width            = width_;
        desc.Height           = height_;
        desc.MipLevels        = 1;
        desc.ArraySize        = 1;
        desc.Format           = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage            = D3D11_USAGE_DEFAULT;
        desc.BindFlags        = 0;  // NVENC registers it as an external resource

        hr = device_->CreateTexture2D(&desc, nullptr,
                                       nv12_texture_.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            fprintf(stderr, "[color] CreateTexture2D (NV12) failed: 0x%08lx\n", hr);
            return false;
        }
    }

    return true;
}

bool ColorConverter::create_pipeline_state() {
    HRESULT hr;

    // Linear sampler for the pixel shaders.
    {
        D3D11_SAMPLER_DESC desc = {};
        desc.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        desc.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        desc.MaxLOD         = D3D11_FLOAT32_MAX;

        hr = device_->CreateSamplerState(&desc,
                                          sampler_.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            fprintf(stderr, "[color] CreateSamplerState failed: 0x%08lx\n", hr);
            return false;
        }
    }

    // Rasterizer: no culling (we render a fullscreen triangle).
    {
        D3D11_RASTERIZER_DESC desc = {};
        desc.FillMode = D3D11_FILL_SOLID;
        desc.CullMode = D3D11_CULL_NONE;

        hr = device_->CreateRasterizerState(&desc,
                                             rasterizer_.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            fprintf(stderr, "[color] CreateRasterizerState failed: 0x%08lx\n", hr);
            return false;
        }
    }

    // Blend state: disabled (opaque write).
    {
        D3D11_BLEND_DESC desc = {};
        desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

        hr = device_->CreateBlendState(&desc,
                                        blend_state_.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            fprintf(stderr, "[color] CreateBlendState failed: 0x%08lx\n", hr);
            return false;
        }
    }

    return true;
}

void ColorConverter::draw_fullscreen(ID3D11PixelShader* ps,
                                      ID3D11RenderTargetView* rtv,
                                      uint32_t vp_width, uint32_t vp_height) {
    // Set the render target.
    ID3D11RenderTargetView* rtvs[] = { rtv };
    context_->OMSetRenderTargets(1, rtvs, nullptr);

    // Set the viewport.
    D3D11_VIEWPORT vp = {};
    vp.Width    = static_cast<float>(vp_width);
    vp.Height   = static_cast<float>(vp_height);
    vp.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &vp);

    // Set pipeline state.
    context_->RSSetState(rasterizer_.Get());
    context_->OMSetBlendState(blend_state_.Get(), nullptr, 0xFFFFFFFF);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->IASetInputLayout(nullptr);  // no vertex buffer

    context_->VSSetShader(vs_fullscreen_.Get(), nullptr, 0);
    context_->PSSetShader(ps, nullptr, 0);
    context_->PSSetShaderResources(0, 1, input_srv_.GetAddressOf());
    context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());

    // Draw 3 vertices (fullscreen triangle, generated from SV_VertexID).
    context_->Draw(3, 0);

    // Unbind the SRV to avoid D3D warnings when the same texture is
    // later bound as a render target.
    ID3D11ShaderResourceView* null_srv = nullptr;
    context_->PSSetShaderResources(0, 1, &null_srv);
}

bool ColorConverter::convert(ID3D11Texture2D* input_bgra,
                              ID3D11Texture2D** output_nv12) {
    if (!device_ || !input_bgra || !output_nv12) return false;

    // Create (or re-create) an SRV for the input BGRA texture.
    // We cache it if the texture pointer hasn't changed, but for safety
    // we recreate every time since the caller may pass different textures.
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format                    = DXGI_FORMAT_B8G8R8A8_UNORM;
    srv_desc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels       = 1;
    srv_desc.Texture2D.MostDetailedMip = 0;

    HRESULT hr = device_->CreateShaderResourceView(
        input_bgra, &srv_desc, input_srv_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        fprintf(stderr, "[color] CreateSRV failed: 0x%08lx\n", hr);
        return false;
    }

    // Pass 1: Render Y plane (full resolution).
    draw_fullscreen(ps_y_plane_.Get(), y_rtv_.Get(), width_, height_);

    // Pass 2: Render UV plane (half resolution).
    draw_fullscreen(ps_uv_plane_.Get(), uv_rtv_.Get(),
                    width_ / 2, height_ / 2);

    // Copy the Y and UV planes into the NV12 texture.
    //
    // NV12 layout:
    //   - Subresource 0: Y plane (width x height, 1 byte per pixel)
    //   - Subresource 1: UV plane (width/2 x height/2, 2 bytes per pixel)
    //
    // We use CopySubresourceRegion to place each plane into the
    // correct subresource of the NV12 texture.

    // Copy Y plane -> NV12 subresource 0.
    D3D11_BOX y_box = {};
    y_box.right  = width_;
    y_box.bottom = height_;
    y_box.back   = 1;
    context_->CopySubresourceRegion(
        nv12_texture_.Get(), 0,   // dst: subresource 0
        0, 0, 0,
        y_texture_.Get(), 0,      // src: subresource 0
        &y_box);

    // Copy UV plane -> NV12 subresource 1.
    D3D11_BOX uv_box = {};
    uv_box.right  = width_ / 2;
    uv_box.bottom = height_ / 2;
    uv_box.back   = 1;
    context_->CopySubresourceRegion(
        nv12_texture_.Get(), 1,   // dst: subresource 1
        0, 0, 0,
        uv_texture_.Get(), 0,     // src: subresource 0
        &uv_box);

    *output_nv12 = nv12_texture_.Get();
    return true;
}

void ColorConverter::shutdown() {
    input_srv_.Reset();
    y_rtv_.Reset();
    y_texture_.Reset();
    uv_rtv_.Reset();
    uv_texture_.Reset();
    nv12_texture_.Reset();

    vs_fullscreen_.Reset();
    ps_y_plane_.Reset();
    ps_uv_plane_.Reset();

    sampler_.Reset();
    rasterizer_.Reset();
    blend_state_.Reset();

    context_.Reset();
    device_.Reset();

    width_  = 0;
    height_ = 0;

    fprintf(stderr, "[color] converter shut down\n");
}

} // namespace droidscreen
