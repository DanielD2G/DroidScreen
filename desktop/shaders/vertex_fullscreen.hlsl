/*
 * DroidScreen - Fullscreen triangle vertex shader
 *
 * Generates a screen-covering triangle from SV_VertexID (0, 1, 2)
 * without any vertex buffer. The oversized triangle is clipped by
 * the hardware rasterizer to produce a full-screen quad.
 *
 * Vertex positions and UV coordinates:
 *
 *   id=0: pos=(-1, -1)  uv=(0, 1)   (bottom-left)
 *   id=1: pos=(-1,  3)  uv=(0,-1)   (top-left, oversized)
 *   id=2: pos=( 3, -1)  uv=(2, 1)   (bottom-right, oversized)
 *
 * After clipping, the visible region covers [-1,1] x [-1,1] with
 * UV in [0,1] x [0,1].
 */

struct VS_OUTPUT {
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

VS_OUTPUT main(uint vertex_id : SV_VertexID) {
    VS_OUTPUT output;

    // Generate UV from vertex ID bits.
    float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);

    // Map UV to clip space: [0,2] -> [-1,3] horizontally,
    // [0,2] -> [1,-3] vertically (flip Y for DirectX convention).
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0),
                             0.0, 1.0);
    output.texcoord = uv;

    return output;
}
