/// Metal shader source embedded as a string so no SPM resource bundle is needed.
/// Compiled at runtime via `device.makeLibrary(source:options:)`.
let metalShaderSource = """
#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 texcoord;
};

vertex VertexOut vertexPassthrough(uint vid [[vertex_id]])
{
    const float2 positions[3] = {
        float2(-1.0f, -1.0f),
        float2( 3.0f, -1.0f),
        float2(-1.0f,  3.0f)
    };
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

struct Uniforms {
    float edrHeadroom;
    float _pad0;
    float _pad1;
    float _pad2;
};

constant float3x3 BT2020_TO_DISPLAY_P3 = float3x3(
    float3( 1.3441f,  -0.1145f, -0.2298f),
    float3(-0.2817f,   1.2095f,  0.0723f),
    float3( 0.0053f,  -0.0358f,  1.0305f)
);

float3 toneMap(float3 c, float headroom)
{
    if (headroom <= 1.0f) {
        float lum = dot(c, float3(0.2126f, 0.7152f, 0.0722f));
        float lumOut = lum / (1.0f + lum);
        float scale = (lum > 0.0f) ? (lumOut / lum) : 0.0f;
        return c * scale;
    }
    float3 out;
    for (int i = 0; i < 3; ++i) {
        float v = c[i];
        if (v <= headroom) {
            out[i] = v;
        } else {
            out[i] = headroom * (v / (v + headroom - 1.0f));
        }
    }
    return out;
}

fragment half4 fragmentHDR(
    VertexOut        in       [[stage_in]],
    texture2d<float> srcTex   [[texture(0)]],
    sampler          smp      [[sampler(0)]],
    constant Uniforms& uni    [[buffer(0)]])
{
    float4 rgba = srcTex.sample(smp, in.texcoord);
    float3 p3   = BT2020_TO_DISPLAY_P3 * rgba.rgb;

    // Soft gamut compression: redistribute out-of-gamut negative energy toward
    // the achromatic axis instead of hard-clamping (which creates a visible line).
    float minVal = min(min(p3.r, p3.g), p3.b);
    if (minVal < 0.0f) {
        float lum = dot(p3, float3(0.2126f, 0.7152f, 0.0722f));
        float t = minVal / (minVal - lum + 1e-6f);
        p3 = mix(p3, float3(lum), saturate(t));
        p3 = max(p3, float3(0.0f));
    }

    float3 mapped = toneMap(p3, uni.edrHeadroom);
    return half4(half3(mapped), 1.0h);
}
"""
