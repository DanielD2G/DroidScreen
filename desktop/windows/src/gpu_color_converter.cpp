/*
 * DroidScreen Windows - GPU color converter implementation
 *
 * Uses HLSL pixel shaders compiled at runtime via D3DCompile to convert
 * captured BGRA/FP16 textures to NV12 on the GPU.
 *
 * The NV12 output texture is created with D3D11_BIND_RENDER_TARGET and
 * two render target views (Y plane as R8_UNORM, UV plane as R8G8_UNORM).
 * This requires D3D11.1 feature level 11_1 with NV12 render target
 * support, which is available on all modern GPUs (NVIDIA 2016+,
 * AMD 2017+, Intel 2018+).
 *
 * Color space: BT.601 limited range (Y: 16-235, UV: 16-240).
 * This matches the default sws_scale behavior for NV12 output.
 */

#include "gpu_color_converter.h"

#include <cstdio>
#include <string>
#include <d3dcompiler.h>

using Microsoft::WRL::ComPtr;

namespace droidscreen {

// ---------------------------------------------------------------------------
// Color matrix: BT.601 limited range, [0,1] RGB -> [0,1] YUV
// ---------------------------------------------------------------------------

struct alignas(16) ColorConvertCB {
    float cy[4];  // Y:  { R,  G,  B, offset }
    float cu[4];  // Cb: { R,  G,  B, offset }
    float cv[4];  // Cr: { R,  G,  B, offset }
};

static_assert(sizeof(ColorConvertCB) == 48,
              "Constant buffer must be 48 bytes (3 x float4)");

static constexpr ColorConvertCB kBT601 = {
    {  0.256788f,  0.504129f,  0.097906f, 0.062745f },  // Y
    { -0.148223f, -0.290993f,  0.439216f, 0.501961f },  // Cb
    {  0.439216f, -0.367788f, -0.071427f, 0.501961f },  // Cr
};

// ---------------------------------------------------------------------------
// Embedded HLSL shader sources
// ---------------------------------------------------------------------------

// Fullscreen triangle vertex shader (no vertex buffer needed).
// Vertex IDs 0, 1, 2 produce a triangle that covers the entire viewport.
static const char kVertexShader[] = R"(
struct VS_OUT {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VS_OUT main(uint id : SV_VertexID) {
    VS_OUT o;
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
)";

// Common HLSL preamble shared by all pixel shaders.
static const char kPSPreamble[] = R"(
Texture2D    source_tex : register(t0);
SamplerState samp       : register(s0);

cbuffer CB : register(b0) {
    float4 cy, cu, cv;
};
)";

// HDR tonemap function: Reinhard + linear-to-sRGB gamma.
static const char kTonemapFunc[] = R"(
float3 tonemap(float3 c) {
    c = max(0, c);
    c = c / (1.0 + c);
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return saturate(lerp(lo, hi, step(0.0031308, c)));
}
)";

// Y plane SDR: sample BGRA, output luma.
static const char kPSYSdrBody[] = R"(
float main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
    float3 rgb = source_tex.Sample(samp, uv).rgb;
    return dot(cy.xyz, rgb) + cy.w;
}
)";

// UV plane SDR: sample BGRA, output chroma pair.
// At half-resolution viewport, bilinear sampling naturally averages 2x2
// source pixels — correct chroma subsampling with no extra code.
static const char kPSUvSdrBody[] = R"(
float2 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
    float3 rgb = source_tex.Sample(samp, uv).rgb;
    return float2(dot(cu.xyz, rgb) + cu.w,
                  dot(cv.xyz, rgb) + cv.w);
}
)";

// Y plane HDR: tonemap FP16 scRGB -> SDR, then output luma.
static const char kPSYHdrBody[] = R"(
float main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
    float3 rgb = tonemap(source_tex.Sample(samp, uv).rgb);
    return dot(cy.xyz, rgb) + cy.w;
}
)";

// UV plane HDR: tonemap FP16 scRGB -> SDR, then output chroma pair.
static const char kPSUvHdrBody[] = R"(
float2 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
    float3 rgb = tonemap(source_tex.Sample(samp, uv).rgb);
    return float2(dot(cu.xyz, rgb) + cu.w,
                  dot(cv.xyz, rgb) + cv.w);
}
)";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static HRESULT compile_hlsl(const char* source, size_t length,
                            const char* entry, const char* target,
                            ID3DBlob** blob_out) {
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(source, length, nullptr, nullptr, nullptr,
                            entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            blob_out, errors.GetAddressOf());
    if (FAILED(hr) && errors) {
        fprintf(stderr, "[gpu_convert] shader compile error:\n%s\n",
                static_cast<const char*>(errors->GetBufferPointer()));
    }
    return hr;
}

static std::string concat(std::initializer_list<const char*> parts) {
    std::string result;
    for (auto* p : parts) result += p;
    return result;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

GpuColorConverter::~GpuColorConverter() {
    shutdown();
}

bool GpuColorConverter::init(ID3D11Device* device, ID3D11DeviceContext* context,
                              uint32_t width, uint32_t height) {
    device_  = device;
    context_ = context;
    width_   = width;
    height_  = height;

    // Check NV12 render target support.
    UINT nv12_support = 0;
    device_->CheckFormatSupport(DXGI_FORMAT_NV12, &nv12_support);
    if (!(nv12_support & D3D11_FORMAT_SUPPORT_RENDER_TARGET)) {
        fprintf(stderr, "[gpu_convert] ERROR: GPU does not support NV12 "
                "render targets (required for zero-copy encoding)\n");
        return false;
    }
    fprintf(stderr, "[gpu_convert] NV12 render target support: OK\n");

    if (!compile_shaders()) return false;
    if (!create_nv12_resources()) return false;

    // Create bilinear sampler (clamp to edge).
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter   = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    HRESULT hr = device_->CreateSamplerState(&sd, sampler_.GetAddressOf());
    if (FAILED(hr)) {
        fprintf(stderr, "[gpu_convert] CreateSamplerState failed: 0x%08lx\n", hr);
        return false;
    }

    // Create constant buffer with BT.601 color matrix.
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth      = sizeof(ColorConvertCB);
    cbd.Usage           = D3D11_USAGE_IMMUTABLE;
    cbd.BindFlags       = D3D11_BIND_CONSTANT_BUFFER;

    D3D11_SUBRESOURCE_DATA init_data = {};
    init_data.pSysMem = &kBT601;

    hr = device_->CreateBuffer(&cbd, &init_data, color_cb_.GetAddressOf());
    if (FAILED(hr)) {
        fprintf(stderr, "[gpu_convert] CreateBuffer (CB) failed: 0x%08lx\n", hr);
        return false;
    }

    initialized_ = true;
    fprintf(stderr, "[gpu_convert] initialized %ux%u (NV12 GPU path)\n",
            width, height);
    return true;
}

bool GpuColorConverter::compile_shaders() {
    HRESULT hr;

    // Vertex shader.
    {
        ComPtr<ID3DBlob> blob;
        hr = compile_hlsl(kVertexShader, sizeof(kVertexShader) - 1,
                          "main", "vs_5_0", blob.GetAddressOf());
        if (FAILED(hr)) return false;

        hr = device_->CreateVertexShader(blob->GetBufferPointer(),
                                          blob->GetBufferSize(),
                                          nullptr, vs_.GetAddressOf());
        if (FAILED(hr)) {
            fprintf(stderr, "[gpu_convert] CreateVertexShader failed: 0x%08lx\n", hr);
            return false;
        }
    }

    // Helper lambda for pixel shaders.
    auto compile_ps = [&](const std::string& src,
                          ComPtr<ID3D11PixelShader>& ps,
                          const char* label) -> bool {
        ComPtr<ID3DBlob> blob;
        hr = compile_hlsl(src.c_str(), src.size(),
                          "main", "ps_5_0", blob.GetAddressOf());
        if (FAILED(hr)) {
            fprintf(stderr, "[gpu_convert] %s compile failed\n", label);
            return false;
        }
        hr = device_->CreatePixelShader(blob->GetBufferPointer(),
                                         blob->GetBufferSize(),
                                         nullptr, ps.GetAddressOf());
        if (FAILED(hr)) {
            fprintf(stderr, "[gpu_convert] CreatePixelShader(%s) failed: "
                    "0x%08lx\n", label, hr);
            return false;
        }
        return true;
    };

    // SDR pixel shaders.
    if (!compile_ps(concat({kPSPreamble, kPSYSdrBody}),
                    ps_y_sdr_, "Y_SDR"))
        return false;
    if (!compile_ps(concat({kPSPreamble, kPSUvSdrBody}),
                    ps_uv_sdr_, "UV_SDR"))
        return false;

    // HDR pixel shaders (include tonemap function).
    if (!compile_ps(concat({kPSPreamble, kTonemapFunc, kPSYHdrBody}),
                    ps_y_hdr_, "Y_HDR"))
        return false;
    if (!compile_ps(concat({kPSPreamble, kTonemapFunc, kPSUvHdrBody}),
                    ps_uv_hdr_, "UV_HDR"))
        return false;

    fprintf(stderr, "[gpu_convert] compiled 4 pixel shaders + 1 vertex shader\n");
    return true;
}

bool GpuColorConverter::create_nv12_resources() {
    // NV12 output texture.
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width            = width_;
    desc.Height           = height_;
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_DEFAULT;
    desc.BindFlags        = D3D11_BIND_RENDER_TARGET;

    HRESULT hr = device_->CreateTexture2D(&desc, nullptr,
                                           nv12_texture_.GetAddressOf());
    if (FAILED(hr)) {
        fprintf(stderr, "[gpu_convert] CreateTexture2D (NV12) failed: 0x%08lx\n", hr);
        return false;
    }

    // Y plane RTV — views the luma plane as R8_UNORM at full resolution.
    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {};
    rtv_desc.ViewDimension         = D3D11_RTV_DIMENSION_TEXTURE2D;
    rtv_desc.Texture2D.MipSlice    = 0;

    rtv_desc.Format = DXGI_FORMAT_R8_UNORM;
    hr = device_->CreateRenderTargetView(nv12_texture_.Get(), &rtv_desc,
                                          y_rtv_.GetAddressOf());
    if (FAILED(hr)) {
        fprintf(stderr, "[gpu_convert] CreateRTV (Y plane) failed: 0x%08lx\n", hr);
        return false;
    }

    // UV plane RTV — views the chroma plane as R8G8_UNORM at half resolution.
    rtv_desc.Format = DXGI_FORMAT_R8G8_UNORM;
    hr = device_->CreateRenderTargetView(nv12_texture_.Get(), &rtv_desc,
                                          uv_rtv_.GetAddressOf());
    if (FAILED(hr)) {
        fprintf(stderr, "[gpu_convert] CreateRTV (UV plane) failed: 0x%08lx\n", hr);
        return false;
    }

    fprintf(stderr, "[gpu_convert] created NV12 %ux%u texture + RTVs\n",
            width_, height_);
    return true;
}

// ---------------------------------------------------------------------------
// Conversion
// ---------------------------------------------------------------------------

ID3D11Texture2D* GpuColorConverter::convert(ID3D11Texture2D* source,
                                             DXGI_FORMAT source_format) {
    if (!initialized_ || !source) return nullptr;

    // Create a temporary SRV on the source texture.
    ComPtr<ID3D11ShaderResourceView> srv;
    HRESULT hr = device_->CreateShaderResourceView(source, nullptr,
                                                    srv.GetAddressOf());
    if (FAILED(hr)) {
        fprintf(stderr, "[gpu_convert] CreateSRV on source failed: 0x%08lx\n", hr);
        return nullptr;
    }

    const bool hdr = (source_format == DXGI_FORMAT_R16G16B16A16_FLOAT);

    // Save the current rasterizer state so we can set our own viewport
    // without affecting the WGC callback's D3D11 state.
    // (WGC doesn't rely on rasterizer state, but this is defensive.)

    // --- Common pipeline state ---
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vs_.Get(), nullptr, 0);

    ID3D11SamplerState* samplers[] = { sampler_.Get() };
    context_->PSSetSamplers(0, 1, samplers);

    ID3D11Buffer* cbs[] = { color_cb_.Get() };
    context_->PSSetConstantBuffers(0, 1, cbs);

    ID3D11ShaderResourceView* srvs[] = { srv.Get() };
    context_->PSSetShaderResources(0, 1, srvs);

    // --- Pass 1: Y plane (full resolution) ---
    {
        ID3D11RenderTargetView* rtvs[] = { y_rtv_.Get() };
        context_->OMSetRenderTargets(1, rtvs, nullptr);

        D3D11_VIEWPORT vp = {};
        vp.Width    = static_cast<float>(width_);
        vp.Height   = static_cast<float>(height_);
        vp.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &vp);

        context_->PSSetShader(hdr ? ps_y_hdr_.Get() : ps_y_sdr_.Get(),
                              nullptr, 0);
        context_->Draw(3, 0);
    }

    // --- Pass 2: UV plane (half resolution) ---
    {
        ID3D11RenderTargetView* rtvs[] = { uv_rtv_.Get() };
        context_->OMSetRenderTargets(1, rtvs, nullptr);

        D3D11_VIEWPORT vp = {};
        vp.Width    = static_cast<float>(width_  / 2);
        vp.Height   = static_cast<float>(height_ / 2);
        vp.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &vp);

        context_->PSSetShader(hdr ? ps_uv_hdr_.Get() : ps_uv_sdr_.Get(),
                              nullptr, 0);
        context_->Draw(3, 0);
    }

    // --- Unbind resources to avoid hazards on next use ---
    ID3D11RenderTargetView* null_rtv[] = { nullptr };
    context_->OMSetRenderTargets(1, null_rtv, nullptr);

    ID3D11ShaderResourceView* null_srv[] = { nullptr };
    context_->PSSetShaderResources(0, 1, null_srv);

    return nv12_texture_.Get();
}

void GpuColorConverter::shutdown() {
    y_rtv_.Reset();
    uv_rtv_.Reset();
    nv12_texture_.Reset();
    color_cb_.Reset();
    sampler_.Reset();
    ps_uv_hdr_.Reset();
    ps_y_hdr_.Reset();
    ps_uv_sdr_.Reset();
    ps_y_sdr_.Reset();
    vs_.Reset();
    context_.Reset();
    device_.Reset();
    initialized_ = false;
}

} // namespace droidscreen
