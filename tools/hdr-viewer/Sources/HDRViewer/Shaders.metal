#include <metal_stdlib>
using namespace metal;

// ---------------------------------------------------------------------------
// Vertex shader
// ---------------------------------------------------------------------------
// Generates a single full-screen triangle from 3 vertices with no vertex
// buffer. The triangle is large enough to cover the entire clip-space quad
// [-1,+1] x [-1,+1].
//
//  Vertex 0: (-1, -1)  →  UV (0, 1)   bottom-left
//  Vertex 1: ( 3, -1)  →  UV (2, 1)   far right
//  Vertex 2: (-1,  3)  →  UV (0,-1)   far top
//
// The texture coordinate convention in Metal is (0,0) = top-left, which
// matches the row-major top-to-bottom pixel data sent by darktable.

struct VertexOut {
    float4 position [[position]];
    float2 texcoord;
};

vertex VertexOut vertexPassthrough(uint vid [[vertex_id]])
{
    // Full-screen triangle trick – no vertex buffer needed
    const float2 positions[3] = {
        float2(-1.0f, -1.0f),
        float2( 3.0f, -1.0f),
        float2(-1.0f,  3.0f)
    };
    // Map clip-space [-1,1] → UV [0,1].  Note: Metal UV origin is top-left,
    // clip-space Y is bottom=−1, top=+1, so we flip Y.
    const float2 texcoords[3] = {
        float2(0.0f, 1.0f),
        float2(2.0f, 1.0f),
        float2(0.0f, -1.0f)
    };

    VertexOut out;
    out.position = float4(positions[vid], 0.0f, 1.0f);
    out.texcoord = texcoords[vid];
    return out;
}

// ---------------------------------------------------------------------------
// Uniforms
// ---------------------------------------------------------------------------
struct Uniforms {
    float edrHeadroom;   // maximumExtendedDynamicRangeColorComponentValue
    float _pad0;
    float _pad1;
    float _pad2;
};

// ---------------------------------------------------------------------------
// Color matrix: BT.2020 linear D65 → Linear Display-P3 D65
// ---------------------------------------------------------------------------
// Derived from: BT.2020 → XYZ(D65) → Display-P3
// Column vectors are BT.2020 primaries expressed in Display-P3.
//
// Each column of a float3x3 in MSL is a column vector.
constant float3x3 BT2020_TO_DISPLAY_P3 = float3x3(
    //  col 0 (R)         col 1 (G)          col 2 (B)
    float3( 1.3441f,  -0.1145f, -0.2298f),   // row 0 → P3 R
    float3(-0.2817f,   1.2095f,  0.0723f),   // row 1 → P3 G
    float3( 0.0053f,  -0.0358f,  1.0305f)    // row 2 → P3 B
);

// ---------------------------------------------------------------------------
// Tone mapping
// ---------------------------------------------------------------------------
// We use a simple "knee" function:
//   - Values ≤ SDR_WHITE pass through unchanged (pure linear)
//   - Values above SDR_WHITE are compressed toward edrHeadroom using a
//     smooth Reinhard-style knee so that specular highlights are visible
//     as HDR signal rather than clipping.
//
// If the display does not support EDR (headroom == 1.0), a standard
// Reinhard curve is applied to keep everything in [0, 1].
//
// This is intentionally minimal. Replace with your preferred operator.

constant float SDR_WHITE = 1.0f;   // in linear Display-P3

float3 toneMap(float3 c, float headroom)
{
    if (headroom <= 1.0f) {
        // SDR display: simple Reinhard on luminance to avoid hue shifts
        float lum = dot(c, float3(0.2126f, 0.7152f, 0.0722f));
        float lumOut = lum / (1.0f + lum);
        float scale = (lum > 0.0f) ? (lumOut / lum) : 0.0f;
        return c * scale;
    }

    // HDR display path: pass through values that are already in [0, headroom].
    // Apply a soft knee only above headroom to handle extreme values.
    float3 out;
    for (int i = 0; i < 3; ++i) {
        float v = c[i];
        if (v <= headroom) {
            out[i] = v;              // preserve HDR signal intact
        } else {
            // Soft compress above headroom using Reinhard scaled to headroom
            out[i] = headroom * (v / (v + headroom - SDR_WHITE));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Fragment shader
// ---------------------------------------------------------------------------
fragment half4 fragmentHDR(
    VertexOut        in       [[stage_in]],
    texture2d<float> srcTex   [[texture(0)]],
    sampler          smp      [[sampler(0)]],
    constant Uniforms& uni    [[buffer(0)]])
{
    // Sample the source texture (linear BT.2020, RGBA32Float)
    float4 rgba = srcTex.sample(smp, in.texcoord);
    float3 rgb  = rgba.rgb;

    // 1. Convert BT.2020 linear → Linear Display-P3
    //    The CAMetalLayer colorspace is extendedLinearDisplayP3, so values we
    //    write here are interpreted as linear Display-P3 by the compositor.
    float3 p3 = BT2020_TO_DISPLAY_P3 * rgb;

    // 2. Clamp negative values (out-of-gamut colors below 0 – rare in practice)
    p3 = max(p3, float3(0.0f));

    // 3. Tone map to [0, edrHeadroom]
    float3 mapped = toneMap(p3, uni.edrHeadroom);

    // Output as half4; Metal will store this as RGBA16Float in the drawable.
    return half4(half3(mapped), 1.0h);
}
