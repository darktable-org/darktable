// lightleak.cl
#include "common.h"

kernel void lightleak(global const float4 *in,
                      global float4 *out,
                      constant dt_iop_lightleak_params_t *p,
                      int w, int h)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if (x >= w || y >= h) return;

  const int idx = y * w + x;

  float nx = (float)x / (float)(w - 1);
  float ny = (float)y / (float)(h - 1);

  // direction in radians
  float rad = p->direction * M_PI_F / 180.0f;
  float cos_dir = native_cos(rad);
  float sin_dir = native_sin(rad);

  // signed projection along direction axis (from center)
  float proj = (nx - 0.5f) * cos_dir + (ny - 0.5f) * sin_dir;

  // inset shift: 0.0 → at edge, 1.0 → through center
  float inset_norm = p->inset * 0.01f;           // /100.0f
  float offset = inset_norm * 0.5f;              // max shift 0.5 in normalized space

  // shift projection so zero (peak) moves inward
  proj += offset;

  // distance from the peak line (symmetric on both sides)
  float dist = fabs(proj);

  // basic tent falloff (linear fade to zero)
  // 0.707f ≈ √2/2 ≈ max distance to corner in normalized space
  float contrib = 1.0f - dist / 0.707f;
  contrib = max(contrib, 0.0f);

  // apply falloff exponent (higher falloff → sharper/narrower band)
  // mapping roughly similar to CPU side
  float exponent = 1.0f + (p->falloff / 25.0f);
  contrib = native_powr(contrib, exponent);

  // final opacity
  float opacity = contrib * (p->strength * 0.01f);  // /100.0f

  // HSV → RGB conversion (same as your original)
  float hh = p->hue * 6.0f;              // hue in [0–360] → internal [0–2160]
  float s   = p->sat * 0.01f;            // [0–100] → [0–1]
  float v   = 1.25f;

  int i = (int)hh;
  float f = hh - (float)i;

  float pp = v * (1.0f - s);
  float q  = v * (1.0f - s * f);
  float t  = v * (1.0f - s * (1.0f - f));

  float3 leak;
  switch (i % 6)   // safer than plain switch(i)
  {
    case 0: leak = (float3)(v, t, pp); break;
    case 1: leak = (float3)(q, v, pp); break;
    case 2: leak = (float3)(pp, v, t); break;
    case 3: leak = (float3)(pp, q, v); break;
    case 4: leak = (float3)(t, pp, v); break;
    default:
    case 5: leak = (float3)(v, pp, q); break;
  }

  float4 pin = in[idx];
  out[idx] = (float4)(pin.xyz + opacity * leak.xyz * 0.7f, pin.w);
}