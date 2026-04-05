/*
 * DroidScreen - BGRA to NV12 color conversion pixel shaders
 *
 * Converts BGRA (linear sRGB) input to NV12 using BT.709 coefficients
 * with limited (studio) range output:
 *
 *   Y  = 16  + 219 * (0.2126*R + 0.7152*G + 0.0722*B)
 *   Cb = 128 + 224 * 0.5389 * (B - Y')     [Y' = linear luma]
 *   Cr = 128 + 224 * 0.6350 * (R - Y')
 *
 * where Y is output in [16, 235] and Cb/Cr in [16, 240].
 *
 * Two shader entry points are provided:
 *
 *   main_y  -- Writes the Y (luminance) plane.
 *              Output: R8_UNORM render target at full resolution.
 *
 *   main_uv -- Writes the UV (chrominance) plane.
 *              Output: R8G8_UNORM render target at half resolution.
 *              R channel = Cb, G channel = Cr.
 *
 * BT.709 derivation:
 *   The chroma scale factors come from the standard:
 *     Cb = 0.5 * (B - Y) / (1 - Kb)  where Kb = 0.0722
 *        = 0.5 / (1 - 0.0722) = 0.5389
 *     Cr = 0.5 * (R - Y) / (1 - Kr)  where Kr = 0.2126
 *        = 0.5 / (1 - 0.2126) = 0.6350
 *
 *   Limited range applies a scale of 219 for luma and 224 for chroma,
 *   with offsets of 16 and 128 respectively.
 */

// ---------------------------------------------------------------------------
// Shared resources
// ---------------------------------------------------------------------------

Texture2D<float4> InputTexture  : register(t0);
SamplerState      LinearSampler : register(s0);

// BT.709 luma coefficients.
static const float3 kLumaCoeff = float3(0.2126, 0.7152, 0.0722);

// ---------------------------------------------------------------------------
// Y-plane shader (full resolution, R8_UNORM output)
// ---------------------------------------------------------------------------

float main_y(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float4 rgba = InputTexture.Sample(LinearSampler, uv);

    // Compute linear luminance.
    float y_linear = dot(rgba.rgb, kLumaCoeff);

    // Apply limited range: Y = (16 + 219 * Y') / 255
    float y = (16.0 + 219.0 * saturate(y_linear)) / 255.0;

    return y;
}

// ---------------------------------------------------------------------------
// UV-plane shader (half resolution, R8G8_UNORM output)
// ---------------------------------------------------------------------------

float2 main_uv(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    // Sample at half resolution -- the linear sampler averages the 2x2
    // block for us, providing correct chroma subsampling.
    float4 rgba = InputTexture.Sample(LinearSampler, uv);

    // Compute linear luminance.
    float y_linear = dot(rgba.rgb, kLumaCoeff);

    // BT.709 chroma with limited range.
    float cb = 128.0 + 224.0 * 0.5389 * (rgba.b - y_linear);
    float cr = 128.0 + 224.0 * 0.6350 * (rgba.r - y_linear);

    // Clamp to valid limited range.
    cb = clamp(cb, 16.0, 240.0);
    cr = clamp(cr, 16.0, 240.0);

    // Normalize to [0, 1] for UNORM output.
    return float2(cb / 255.0, cr / 255.0);
}
