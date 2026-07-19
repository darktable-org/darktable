/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

/* spektrafilm.cl — OpenCL kernels for the native spektrafilm iop.
 *
 * Mirrors the CPU stage functions in spektra_sim.c (which are themselves a
 * validated port of spektrafilm 0.3.x). Per-pixel colour science runs here;
 * the Gaussian blurs (halation bounces, diffusion bank, DIR-coupler
 * correction diffusion, grain clumps) are done by darktable's
 * dt_gaussian_fast_blur_cl_buffer on the intermediate buffers, exactly as the
 * CPU path uses sf_blur_plane3.
 *
 * Pipeline: expose (CAT16'd RGB -> xy -> tri2quad -> Mitchell-cubic 2D
 * spectral LUT × brightness × 2^EV) -> [boost/diffusion/halation on linear]
 * -> lograw -> develop_corr -> [host blur] -> develop -> [grain] ->
 * print_expose (PCHIP 3D) -> print_develop -> scan (PCHIP 3D -> XYZ ->
 * work RGB -> OkLCh compression).
 *
 * Conventions:
 *   - working buffers are float4 (.w carries alpha where relevant);
 *   - all tables come from sf_sim_gpu_export(): the 2D spectral LUT
 *     (tc_n×tc_n×3), the density curves (256×3), and the 3D PCHIP tables
 *     (steps³×3 values + per-axis slopes + per-cell bounds) as __global
 *     float buffers (they exceed __constant limits at 33³+);
 *   - small matrices are packed into one __constant float block, see the
 *     SF_M_* offsets below;
 *   - the CPU engine computes in double; these kernels are float, so expect
 *     ~1e-3 vs the CPU path (validated with POCL against sf_sim_process).
 *   - exact-spectral quality has NO GPU path; process_cl falls back to CPU.
 */

constant sampler_t sampleri =
    CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;

#define SF_NLE 256
#define SF_LOG_EPS 1e-10f

/* offsets (in floats) into the packed matrix/constant buffer */
#define SF_M_IN 0        /* 9: work RGB -> XYZ(film ref), CAT16 included */
#define SF_M_OUT 9       /* 9: XYZ(view) -> work RGB, CAT02 included */
#define SF_M_COUPLERS 18 /* 9: DIR coupler matrix, amount-scaled */
#define SF_M_RGB2XYZ 27  /* 9: output RGB -> XYZ (plain, for OkLab) */
#define SF_M_XYZ2RGB 36  /* 9 */
#define SF_M_OK1 45      /* 9: OkLab M1 */
#define SF_M_OK2 54      /* 9: OkLab M2 */
#define SF_M_OK1I 63     /* 9: inv(M1) */
#define SF_M_OK2I 72     /* 9: inv(M2) */
#define SF_M_LM_DONOR 81 /* 6: langmuir donor K[3] + D_ref[3] (K=1e30 = linear) */
#define SF_M_LM_RECV 87  /* 6: langmuir receiver Kr[3] + c_ref[3] */
#define SF_M_TOTAL 93

static inline float sf_clampf(float x, float lo, float hi)
{
  return fmin(fmax(x, lo), hi);
}

static inline float3 sf_mat3(__constant const float *m, float3 v)
{
  return (float3)(m[0] * v.x + m[1] * v.y + m[2] * v.z,
                  m[3] * v.x + m[4] * v.y + m[5] * v.z,
                  m[6] * v.x + m[7] * v.y + m[8] * v.z);
}

/* ---- [su] Mitchell-Netravali cubic on the tc_n×tc_n×3 spectral LUT ------ */

static float sf_mitchell(float t)
{
  const float B = 1.0f / 3.0f, C = 1.0f / 3.0f;
  const float x = fabs(t);
  if(x < 1.0f)
    return (1.0f / 6.0f)
           * ((12.0f - 9.0f * B - 6.0f * C) * x * x * x
              + (-18.0f + 12.0f * B + 6.0f * C) * x * x + (6.0f - 2.0f * B));
  else if(x < 2.0f)
    return (1.0f / 6.0f)
           * ((-B - 6.0f * C) * x * x * x + (6.0f * B + 30.0f * C) * x * x
              + (-12.0f * B - 48.0f * C) * x + (8.0f * B + 24.0f * C));
  return 0.0f;
}

static inline int sf_reflect(int idx, int L)
{
  if(idx < 0) return -idx;
  if(idx >= L) return 2 * (L - 1) - idx;
  return idx;
}

static inline void sf_base_frac(float coord, int L, int *base, float *frac)
{
  coord = sf_clampf(coord, 0.0f, (float)(L - 1));
  if(coord >= (float)(L - 1))
  {
    *base = L - 2;
    *frac = 1.0f;
    return;
  }
  *base = (int)floor(coord);
  *frac = coord - *base;
}

static float3 sf_cubic2d(__global const float *lut, int L, float x, float y)
{
  int xb, yb;
  float xf, yf;
  sf_base_frac(x, L, &xb, &xf);
  sf_base_frac(y, L, &yb, &yf);
  float wx[4], wy[4];
  for(int i = 0; i < 4; i++)
  {
    wx[i] = sf_mitchell(xf + 1.0f - i);
    wy[i] = sf_mitchell(yf + 1.0f - i);
  }
  float3 acc = (float3)(0.0f);
  float wsum = 0.0f;
  for(int i = 0; i < 4; i++)
  {
    const int xi = sf_reflect(xb - 1 + i, L);
    for(int j = 0; j < 4; j++)
    {
      const int yj = sf_reflect(yb - 1 + j, L);
      const float w = wx[i] * wy[j];
      wsum += w;
      const size_t o = ((size_t)xi * L + yj) * 3;
      acc += w * (float3)(lut[o], lut[o + 1], lut[o + 2]);
    }
  }
  return (wsum != 0.0f) ? acc / wsum : acc;
}

/* ---- [dc] density curve interpolation over the uniform le grid ---------- */
/* x-axis = le/gamma  ->  index t = (x*gamma - le0)/le_step, endpoint-clamped */
static inline float sf_curve(__global const float *curves, float x, float gammac,
                             float le0, float le_step, int c)
{
  const float t = (x * gammac - le0) / le_step;
  if(t <= 0.0f) return curves[c];
  if(t >= (float)(SF_NLE - 1)) return curves[(SF_NLE - 1) * 3 + c];
  const int i = (int)t;
  const float f = t - i;
  return curves[i * 3 + c] + f * (curves[(i + 1) * 3 + c] - curves[i * 3 + c]);
}

/* ---- [fi] monotone-PCHIP 3D LUT (values + per-axis slopes + cell clamp) - */

static inline float sf_hermite(float y0, float y1, float m0, float m1, float t)
{
  const float t2 = t * t, t3 = t2 * t;
  return (2.0f * t3 - 3.0f * t2 + 1.0f) * y0 + (t3 - 2.0f * t2 + t) * m0
         + (-2.0f * t3 + 3.0f * t2) * y1 + (t3 - t2) * m1;
}

static float3 sf_pchip3d(__global const float *lut, __global const float *sx,
                         __global const float *sy, __global const float *sz,
                         __global const float *cmin, __global const float *cmax,
                         const int n, float r, float g, float b)
{
  const int m = n - 1;
  int i, j, k;
  float tr, tg, tb;
  sf_base_frac(r, n, &i, &tr);
  sf_base_frac(g, n, &j, &tg);
  sf_base_frac(b, n, &k, &tb);
  float out[3];
#define AT(arr, ii, jj, kk, c) arr[((((size_t)(ii)) * n + (jj)) * n + (kk)) * 3 + (c)]
  for(int c = 0; c < 3; c++)
  {
    const float v000 = sf_hermite(AT(lut, i, j, k, c), AT(lut, i + 1, j, k, c),
                                  AT(sx, i, j, k, c), AT(sx, i + 1, j, k, c), tr);
    const float v010 = sf_hermite(AT(lut, i, j + 1, k, c), AT(lut, i + 1, j + 1, k, c),
                                  AT(sx, i, j + 1, k, c), AT(sx, i + 1, j + 1, k, c), tr);
    const float v001 = sf_hermite(AT(lut, i, j, k + 1, c), AT(lut, i + 1, j, k + 1, c),
                                  AT(sx, i, j, k + 1, c), AT(sx, i + 1, j, k + 1, c), tr);
    const float v011
        = sf_hermite(AT(lut, i, j + 1, k + 1, c), AT(lut, i + 1, j + 1, k + 1, c),
                     AT(sx, i, j + 1, k + 1, c), AT(sx, i + 1, j + 1, k + 1, c), tr);
    const float sy00 = mix(AT(sy, i, j, k, c), AT(sy, i + 1, j, k, c), tr);
    const float sy10 = mix(AT(sy, i, j + 1, k, c), AT(sy, i + 1, j + 1, k, c), tr);
    const float sy01 = mix(AT(sy, i, j, k + 1, c), AT(sy, i + 1, j, k + 1, c), tr);
    const float sy11 = mix(AT(sy, i, j + 1, k + 1, c), AT(sy, i + 1, j + 1, k + 1, c), tr);
    const float vz0 = sf_hermite(v000, v010, sy00, sy10, tg);
    const float vz1 = sf_hermite(v001, v011, sy01, sy11, tg);
    const float sz0 = mix(mix(AT(sz, i, j, k, c), AT(sz, i + 1, j, k, c), tr),
                          mix(AT(sz, i, j + 1, k, c), AT(sz, i + 1, j + 1, k, c), tr), tg);
    const float sz1
        = mix(mix(AT(sz, i, j, k + 1, c), AT(sz, i + 1, j, k + 1, c), tr),
              mix(AT(sz, i, j + 1, k + 1, c), AT(sz, i + 1, j + 1, k + 1, c), tr), tg);
    float v = sf_hermite(vz0, vz1, sz0, sz1, tb);
    const size_t ci = ((((size_t)i) * m + j) * m + k) * 3 + c;
    v = sf_clampf(v, cmin[ci], cmax[ci]);
    out[c] = v;
  }
#undef AT
  return (float3)(out[0], out[1], out[2]);
}

/* ---- [gc] Reinhard knee + OkLCh output gamut compression ---------------- */

static inline float sf_knee(float d, float threshold, float limit, float power)
{
  if(d <= threshold) return d;
  const float scale = limit - threshold;
  const float x = (d - threshold) / scale;
  const float y = x / pow(1.0f + pow(x, power), 1.0f / power);
  return threshold + scale * y;
}

static inline float3 sf_xyz_to_oklab(__constant const float *mats, float3 xyz)
{
  float3 lms = sf_mat3(mats + SF_M_OK1, xyz);
  lms = (float3)(cbrt(lms.x), cbrt(lms.y), cbrt(lms.z));
  return sf_mat3(mats + SF_M_OK2, lms);
}

static inline float3 sf_oklab_to_xyz(__constant const float *mats, float3 lab)
{
  float3 lms = sf_mat3(mats + SF_M_OK2I, lab);
  lms = lms * lms * lms;
  return sf_mat3(mats + SF_M_OK1I, lms);
}

static float sf_cmax_lookup(__global const float *table, const int nl, const int nh,
                            float L, float h)
{
  const float L_lo_v = 0.02f, L_hi_v = 1.0f;
  L = sf_clampf(L, L_lo_v, L_hi_v);
  const float h_step = 2.0f * M_PI_F / nh;
  const float h_idx = (h + M_PI_F) / h_step;
  const float h_floor = floor(h_idx);
  int h_lo = ((int)h_floor) % nh;
  if(h_lo < 0) h_lo += nh;
  const int h_hi = (h_lo + 1) % nh;
  const float h_frac = h_idx - h_floor;
  const float L_idx = (L - L_lo_v) / (L_hi_v - L_lo_v) * (float)(nl - 1);
  int L_lo = (int)floor(L_idx);
  L_lo = clamp(L_lo, 0, nl - 2);
  const float L_frac = L_idx - L_lo;
  const float v00 = table[(size_t)L_lo * nh + h_lo];
  const float v01 = table[(size_t)L_lo * nh + h_hi];
  const float v10 = table[(size_t)(L_lo + 1) * nh + h_lo];
  const float v11 = table[(size_t)(L_lo + 1) * nh + h_hi];
  return v00 * (1 - L_frac) * (1 - h_frac) + v01 * (1 - L_frac) * h_frac
         + v10 * L_frac * (1 - h_frac) + v11 * L_frac * h_frac;
}

/* ---- grain RNG, identical to spektra_core.h (see there for provenance) -- */
static inline uint sf_h(uint x)
{
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}
static inline float sf_u01(uint s)
{
  return (sf_h(s) & 0xffffff) / (float)0x1000000;
}
static inline float sf_nrm(uint s)
{
  float u1 = fmax(sf_u01(s), 1e-7f), u2 = sf_u01(s * 2654435761u + 1u);
  return native_sqrt(-2.f * native_log(u1)) * native_cos(6.2831853f * u2);
}
static inline uint sf_pixel_seed(uint xi, uint yi, uint chan)
{
  return xi * 73856093u ^ yi * 19349663u ^ chan * 83492791u;
}
static float sf_layer_particle(float density, float dmax, float npart, float unif, uint seed)
{
  float p = sf_clampf(density / dmax, 1e-6f, 1.f - 1e-6f), od = dmax / npart,
        sat = 1.f - p * unif * (1.f - 1e-6f), lam = npart / sat;
  float seeds = lam + native_sqrt(fmax(lam, 0.f)) * sf_nrm(seed * 0x9e3779b9u + 1u);
  seeds = fmax(seeds, 0.f);
  float mean = seeds * p, var = seeds * p * (1.f - p),
        g = mean + native_sqrt(fmax(var, 0.f)) * sf_nrm(seed * 0x85ebca6bU + 7u);
  g = fmax(g, 0.f);
  g = fmin(g, seeds);
  return g * od * sat;
}

/* ======================================================================== */
/* per-pixel stage kernels                                                  */
/* ======================================================================== */

/* stage 1: input image -> linear film raw exposure (spektra_sim: sf_sim_expose) */
__kernel void spektrafilm_expose(__read_only image2d_t in, __global float4 *plane,
                                 const int w, const int h, __constant float *mats,
                                 __global const float *tc_lut, const int tc_n,
                                 const float ev_scale)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= w || y >= h) return;
  float4 px = read_imagef(in, sampleri, (int2)(x, y));
  float3 xyz = sf_mat3(mats + SF_M_IN, (float3)(px.x, px.y, px.z));
  const float b = xyz.x + xyz.y + xyz.z;
  const float inv = 1.0f / fmax(b, 1e-10f);
  const float xx = xyz.x * inv, yy = xyz.y * inv;
  /* [su] tri2quad */
  const float tcx = sf_clampf((1.0f - xx) * (1.0f - xx), 0.0f, 1.0f);
  /* careful: tri2quad computes from CIE xy, matching spektra_sim tri2quad() */
  const float tcy = sf_clampf(yy / fmax(1.0f - xx, 1e-10f), 0.0f, 1.0f);
  const float scale = (float)(tc_n - 1);
  float3 raw = sf_cubic2d(tc_lut, tc_n, tcx * scale, tcy * scale);
  const float bb = isfinite(b) ? b : 0.0f;
  raw *= bb * ev_scale;
  plane[(size_t)y * w + x] = (float4)(raw.x, raw.y, raw.z, px.w);
}

/* stage 3a: linear raw -> log exposure (in place) */
__kernel void spektrafilm_lograw(__global float4 *plane, const int w, const int h)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= w || y >= h) return;
  const size_t k = (size_t)y * w + x;
  float4 p = plane[k];
  p.x = log10(fmax(p.x, 0.0f) + SF_LOG_EPS);
  p.y = log10(fmax(p.y, 0.0f) + SF_LOG_EPS);
  p.z = log10(fmax(p.z, 0.0f) + SF_LOG_EPS);
  plane[k] = p;
}

/* stage 3b: DIR coupler correction field (spektra_sim: sf_sim_develop_corr);
   blurred host-side with dt_gaussian, then consumed by _develop below */
__kernel void spektrafilm_develop_corr(__global const float4 *lograw, __global float4 *corr,
                                       const int w, const int h,
                                       __global const float *curves_norm,
                                       __constant float *mats, const float g0,
                                       const float g1, const float g2, const float le0,
                                       const float le_step, const float dmax0,
                                       const float dmax1, const float dmax2,
                                       const int positive)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= w || y >= h) return;
  const size_t k = (size_t)y * w + x;
  const float4 lg = lograw[k];
  const float gam[3] = { g0, g1, g2 };
  const float dmx[3] = { dmax0, dmax1, dmax2 };
  const float lgv[3] = { lg.x, lg.y, lg.z };
  float silver[3];
  for(int c = 0; c < 3; c++)
  {
    const float d = sf_curve(curves_norm, lgv[c], gam[c], le0, le_step, c);
    silver[c] = positive ? dmx[c] - d : d;
    /* Langmuir donor saturation (dev packs); K=1e30 degenerates to linear */
    const float K = mats[SF_M_LM_DONOR + c], Dref = mats[SF_M_LM_DONOR + 3 + c];
    silver[c] = silver[c] * (K + Dref) / (K + silver[c]);
  }
  __constant const float *M = mats + SF_M_COUPLERS; /* row donor -> col receiver */
  float out[3];
  for(int m = 0; m < 3; m++)
    out[m] = silver[0] * M[0 * 3 + m] + silver[1] * M[1 * 3 + m] + silver[2] * M[2 * 3 + m];
  corr[k] = (float4)(out[0], out[1], out[2], 0.0f);
}

/* stage 3c: develop to CMY film density (spektra_sim: sf_sim_develop).
   `curves` is curves_before when couplers are on, curves_norm otherwise. */
__kernel void spektrafilm_develop(__global const float4 *lograw, __global const float4 *corr,
                                  const int use_corr, __global float4 *cmy, const int w,
                                  const int h, __global const float *curves,
                                  __constant float *mats, const float g0,
                                  const float g1, const float g2, const float le0,
                                  const float le_step)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= w || y >= h) return;
  const size_t k = (size_t)y * w + x;
  const float4 lg = lograw[k];
  float4 cr = use_corr ? corr[k] : (float4)(0.0f);
  /* receiver-side Langmuir on the ARRIVED (post-diffusion) inhibitor;
     Kr=1e30 degenerates to linear */
  float crv[3] = { cr.x, cr.y, cr.z };
  for(int c = 0; c < 3; c++)
  {
    const float Kr = mats[SF_M_LM_RECV + c], cref = mats[SF_M_LM_RECV + 3 + c];
    crv[c] = crv[c] * (Kr + cref) / (Kr + crv[c]);
  }
  const float gam[3] = { g0, g1, g2 };
  const float lgv[3] = { lg.x - crv[0], lg.y - crv[1], lg.z - crv[2] };
  float out[3];
  for(int c = 0; c < 3; c++) out[c] = sf_curve(curves, lgv[c], gam[c], le0, le_step, c);
  cmy[k] = (float4)(out[0], out[1], out[2], lg.w);
}

/* stage 4: grain delta on the developed CMY density (spektra_core model);
   delta is blurred host-side, then added back by spektrafilm_grain_add */
__kernel void spektrafilm_grain_gen(__global const float4 *dens, __global float4 *grain_buf,
                                    const int w, const int h, const float grain_amount,
                                    const int roi_x, const int roi_y, const int mono,
                                    const float dmax0, const float dmax1, const float dmax2,
                                    const float dmin0, const float dmin1, const float dmin2,
                                    const float rms0, const float rms1, const float rms2,
                                    const float unf0, const float unf1, const float unf2)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= w || y >= h) return;
  const size_t k = (size_t)y * w + x;
  const float4 d4 = dens[k];
  const float dmn[3] = { dmin0, dmin1, dmin2 };
  /* per-film emulsion D-max: a hardcoded colour-negative 2.2 saturates the
     particle model in dense slide areas and tints them channel-dependently */
  const float dmc[3] = { fmax(dmax0, 1e-3f), fmax(dmax1, 1e-3f), fmax(dmax2, 1e-3f) };
  /* per-film catalogue grain (rms-granularity, uniformity) from
     film_render_defaults[stock].grain — replaces the earlier one-size-fits-all
     constants so e.g. Portra 400 and Tri-X render distinct grain */
  const float rms[3] = { rms0, rms1, rms2 }, unf[3] = { unf0, unf1, unf2 };
  const float A48 = 3.14159265f * 24.0f * 24.0f;
  const float ref_um = 10.0f, pix = ref_um * ref_um;
  const float dd[3] = { d4.x, d4.y, d4.z };
  float gd[3];
  if(mono) /* B&W stock: one achromatic grain realisation for all channels */
  {
    const float dm = (dd[0] + dd[1] + dd[2]) / 3.0f;
    const float dmax = dmc[1] + dmn[1], d_ref = 1.0f + dmn[1];
    const float sig = rms[1] / 1000.0f;
    const float denom = fmax(d_ref * (dmax - unf[1] * d_ref), 1e-6f);
    const float a_grain = sig * sig * A48 / denom;
    const float npart = pix / fmax(a_grain, 1e-4f);
    const float din = dm + dmn[1];
    uint seed = sf_pixel_seed((uint)(x + roi_x), (uint)(y + roi_y), 0u);
    const float gval = sf_layer_particle(din, dmax, npart, unf[1], seed) - dmn[1];
    const float dl = (gval - dm) * grain_amount;
    grain_buf[k] = (float4)(dl, dl, dl, 0.f);
    return;
  }
  for(int c = 0; c < 3; c++)
  {
    float dmax = dmc[c] + dmn[c], din = dd[c] + dmn[c];
    float d_ref = 1.0f + dmn[c], sig = rms[c] / 1000.0f;
    float denom = fmax(d_ref * (dmax - unf[c] * d_ref), 1e-6f);
    float a_grain = sig * sig * A48 / denom;
    float npart = pix / fmax(a_grain, 1e-4f);
    uint seed = sf_pixel_seed((uint)(x + roi_x), (uint)(y + roi_y), (uint)c);
    float g = sf_layer_particle(din, dmax, npart, unf[c], seed) - dmn[c];
    gd[c] = (g - dd[c]) * grain_amount;
  }
  grain_buf[k] = (float4)(gd[0], gd[1], gd[2], 0.f);
}

/* Inverse-lookup: given the already-computed NET total density `target` for
 * one channel, find its fractional position on the [0, n) exposure-grid
 * axis by searching `arr` (assumed monotonic -- guaranteed for a sum of
 * same-signed CDF terms) via binary search plus linear interpolation. Walks
 * a strided (non-contiguous) column of a [n][...] table without copying it
 * out first -- the same "find where the total curve reads D" step
 * spektrafilm's own interp_density_cmy_layers_channel performs. */
static float sf_cl_grain_curve_inverse(__global const float *arr, int n, int stride, float target)
{
  const int increasing = arr[(n - 1) * stride] >= arr[0];
  int lo = 0, hi = n - 1;
  while(hi - lo > 1)
  {
    const int mid = (lo + hi) / 2;
    const float v = arr[mid * stride];
    if((increasing && v <= target) || (!increasing && v >= target)) lo = mid;
    else hi = mid;
  }
  const float v0 = arr[lo * stride], v1 = arr[hi * stride];
  const float denom = v1 - v0;
  float frac = (fabs(denom) > 1e-9f) ? (target - v0) / denom : 0.0f;
  frac = clamp(frac, 0.0f, 1.0f);
  return (float)lo + frac;
}

/* Linearly interpolate a strided per-index array at the continuous index
 * `pos` produced by sf_cl_grain_curve_inverse above. */
static float sf_cl_grain_curve_sample(__global const float *arr, int n, int stride, float pos)
{
  int i0 = (int)pos;
  if(i0 < 0) i0 = 0;
  if(i0 > n - 2) i0 = (n - 2 < 0) ? 0 : n - 2;
  const float frac = pos - (float)i0;
  return arr[i0 * stride] * (1.0f - frac) + arr[(i0 + 1) * stride] * frac;
}

/* Multi-sublayer grain delta (see sf_grain_delta_ml, spektra_sim.c): only
 * dispatched when n_sub > 1 -- a film whose own fitted density-curve model
 * has more than one emulsion sub-layer. For n_sub<=1 the host dispatches
 * spektrafilm_grain_gen above instead, unchanged. Sums each sub-layer's
 * independent particle draw, recovering that sub-layer's own density by
 * inverse-interpolating the self-consistent summed sub-layer curve at the
 * exposure position the already-computed total density corresponds to.
 * layer_curve is [nle][max_sub][3] row-major, layer_curve_total is
 * [nle][3]; max_sub (SF_GRAIN_MAX_SUBLAYERS host-side) is the table's
 * fixed stride regardless of how many sub-layers n_sub actually uses. */
__kernel void spektrafilm_grain_gen_ml(__global const float4 *dens, __global float4 *grain_buf,
                                       const int w, const int h, const float grain_amount,
                                       const int roi_x, const int roi_y, const int mono,
                                       const int n_sub, const int nle, const int max_sub,
                                       const float dmin0, const float dmin1, const float dmin2,
                                       const float unf0, const float unf1, const float unf2,
                                       __global const float *layer_dmax,
                                       __global const float *layer_npart,
                                       __global const float *layer_dmin,
                                       __global const float *layer_curve_total,
                                       __global const float *layer_curve)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= w || y >= h) return;
  const size_t k = (size_t)y * w + x;
  const float4 d4 = dens[k];
  const float dd[3] = { d4.x, d4.y, d4.z };
  const float dmn[3] = { dmin0, dmin1, dmin2 };
  const float unf[3] = { unf0, unf1, unf2 };
  const int lstride = max_sub * 3;

  if(mono)
  {
    const float dm = (dd[0] + dd[1] + dd[2]) / 3.0f;
    const float pos = sf_cl_grain_curve_inverse(layer_curve_total + 1, nle, 3, dm);
    float total_abs = 0.0f;
    for(int sl = 0; sl < n_sub; sl++)
    {
      const float raw = sf_cl_grain_curve_sample(layer_curve + sl * 3 + 1, nle, lstride, pos);
      const float d_abs = raw + layer_dmin[sl * 3 + 1];
      const uint seed = sf_pixel_seed((uint)(x + roi_x), (uint)(y + roi_y), (uint)(sl * 10));
      total_abs += sf_layer_particle(d_abs, layer_dmax[sl * 3 + 1], layer_npart[sl * 3 + 1], unf[1], seed);
    }
    const float gval = total_abs - dmn[1];
    const float dl = (gval - dm) * grain_amount;
    grain_buf[k] = (float4)(dl, dl, dl, 0.f);
    return;
  }

  float gd[3];
  for(int c = 0; c < 3; c++)
  {
    const float pos = sf_cl_grain_curve_inverse(layer_curve_total + c, nle, 3, dd[c]);
    float total_abs = 0.0f;
    for(int sl = 0; sl < n_sub; sl++)
    {
      const float raw = sf_cl_grain_curve_sample(layer_curve + sl * 3 + c, nle, lstride, pos);
      const float d_abs = raw + layer_dmin[sl * 3 + c];
      const uint seed = sf_pixel_seed((uint)(x + roi_x), (uint)(y + roi_y), (uint)(c + sl * 10));
      total_abs += sf_layer_particle(d_abs, layer_dmax[sl * 3 + c], layer_npart[sl * 3 + c], unf[c], seed);
    }
    const float g = total_abs - dmn[c];
    gd[c] = (g - dd[c]) * grain_amount;
  }
  grain_buf[k] = (float4)(gd[0], gd[1], gd[2], 0.f);
}

__kernel void spektrafilm_grain_add(__global float4 *dens_buf, __global const float4 *grain_buf,
                                    const int w, const int h, const float renorm)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= w || y >= h) return;
  const size_t k = (size_t)y * w + x;
  float4 d = dens_buf[k];
  float4 g = grain_buf[k];
  dens_buf[k] = (float4)(d.x + g.x * renorm, d.y + g.y * renorm, d.z + g.z * renorm, d.w);
}

/* stage 5a: CMY film density -> print log exposure (sf_sim_print_expose) */
__kernel void spektrafilm_print_expose(__global const float4 *cmy, __global float4 *loge,
                                       const int w, const int h,
                                       __global const float *lut, __global const float *sx,
                                       __global const float *sy, __global const float *sz,
                                       __global const float *cmn, __global const float *cmx,
                                       const int steps, const float lo0, const float lo1,
                                       const float lo2, const float hi0, const float hi1,
                                       const float hi2, const float print_exposure)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= w || y >= h) return;
  const size_t k = (size_t)y * w + x;
  const float4 in = cmy[k];
  const float scale = (float)(steps - 1);
  const float r = (in.x - lo0) / (hi0 - lo0) * scale;
  const float g = (in.y - lo1) / (hi1 - lo1) * scale;
  const float b = (in.z - lo2) / (hi2 - lo2) * scale;
  float3 l1 = sf_pchip3d(lut, sx, sy, sz, cmn, cmx, steps, r, g, b);
  /* [st] raw = 10^l1 * print_exposure; back to log10 */
  float3 out;
  out.x = log10(fmax(exp10(l1.x) * print_exposure, 0.0f) + SF_LOG_EPS);
  out.y = log10(fmax(exp10(l1.y) * print_exposure, 0.0f) + SF_LOG_EPS);
  out.z = log10(fmax(exp10(l1.z) * print_exposure, 0.0f) + SF_LOG_EPS);
  loge[k] = (float4)(out.x, out.y, out.z, in.w);
}

/* stage 5b: print log exposure -> print CMY density (sf_sim_print_develop) */
__kernel void spektrafilm_print_develop(__global const float4 *loge, __global float4 *cmy,
                                        const int w, const int h,
                                        __global const float *print_curves, const float le0,
                                        const float le_step)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= w || y >= h) return;
  const size_t k = (size_t)y * w + x;
  const float4 in = loge[k];
  const float lgv[3] = { in.x, in.y, in.z };
  float out[3];
  for(int c = 0; c < 3; c++)
    out[c] = sf_curve(print_curves, lgv[c], 1.0f, le0, le_step, c);
  cmy[k] = (float4)(out[0], out[1], out[2], in.w);
}

/* stage 6: scan — CMY density -> log XYZ (PCHIP) -> XYZ -> work RGB with
   OkLCh (mode 1) / ACES RGC (mode 2) gamut compression. Runs on the OUTPUT
   grid, cropping (ox, oy) from the full-ROI plane and taking alpha from the
   input image (spektra_sim: sf_sim_scan). */
__kernel void spektrafilm_scan(__global const float4 *cmy, __read_only image2d_t in,
                               __write_only image2d_t out, const int w, const int ow,
                               const int oh, const int ox, const int oy,
                               __global const float *lut, __global const float *sx,
                               __global const float *sy, __global const float *sz,
                               __global const float *cmn, __global const float *cmx,
                               const int steps, const float lo0, const float lo1,
                               const float lo2, const float hi0, const float hi1,
                               const float hi2, __constant float *mats,
                               __global const float *cmax_table, const int cmax_nl,
                               const int cmax_nh, const int compress_mode,
                               const float out_luminance_boost,
                               const int bw_on, const float bw_m, const float bw_q)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= ow || y >= oh) return;
  const size_t k = (size_t)(y + oy) * w + (x + ox);
  const float4 c4 = cmy[k];
  const float scale = (float)(steps - 1);
  const float r = (c4.x - lo0) / (hi0 - lo0) * scale;
  const float g = (c4.y - lo1) / (hi1 - lo1) * scale;
  const float b = (c4.z - lo2) / (hi2 - lo2) * scale;
  float3 lx = sf_pchip3d(lut, sx, sy, sz, cmn, cmx, steps, r, g, b);
  float3 xyz = (float3)(exp10(lx.x), exp10(lx.y), exp10(lx.z));
  if(out_luminance_boost != 1.0f) xyz *= out_luminance_boost;
  if(bw_on) /* scanner black/white point (positive film scans) */
  {
    const float yc = sf_clampf(bw_m * xyz.y + bw_q, 0.0f, 1.0f);
    xyz *= yc / (xyz.y + 1e-10f);
  }
  float3 rgb = sf_mat3(mats + SF_M_OUT, xyz);

  if(compress_mode == 1) /* OkLCh chroma + lightness compression */
  {
    float3 lab = sf_xyz_to_oklab(mats, sf_mat3(mats + SF_M_RGB2XYZ, rgb));
    float L = sf_knee(lab.x, 0.7f, 1.0f, 2.2f); /* lightness first */
    const float C = hypot(lab.y, lab.z);
    const float hh = atan2(lab.z, lab.y);
    const float C_max = fmax(sf_cmax_lookup(cmax_table, cmax_nl, cmax_nh, L, hh), 1e-9f);
    const float d = sf_knee(C / C_max, 0.0f, 1.0f, 6.0f);
    const float C_new = d * C_max;
    float3 lab_new = (float3)(L, C_new * cos(hh), C_new * sin(hh));
    rgb = sf_mat3(mats + SF_M_XYZ2RGB, sf_oklab_to_xyz(mats, lab_new));
  }
  else if(compress_mode == 2) /* ACES reference gamut compression style */
  {
    const float ach = fmax(rgb.x, fmax(rgb.y, rgb.z));
    if(ach > 1e-12f)
    {
      float v[3] = { rgb.x, rgb.y, rgb.z };
      for(int c = 0; c < 3; c++)
      {
        const float d = (ach - v[c]) / ach;
        const float dc = sf_knee(d, 0.0f, 1.0f, 6.0f);
        v[c] = ach * (1.0f - dc);
      }
      rgb = (float3)(v[0], v[1], v[2]);
    }
  }

  const float4 px = read_imagef(in, sampleri, (int2)(x + ox, y + oy));
  write_imagef(out, (int2)(x, y), (float4)(rgb.x, rgb.y, rgb.z, px.w));
}

/* passthrough crop when no sim is available */
__kernel void spektrafilm_passthrough(__read_only image2d_t in, __write_only image2d_t out,
                                      const int ow, const int oh, const int ox, const int oy)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= ow || y >= oh) return;
  write_imagef(out, (int2)(x, y), read_imagef(in, sampleri, (int2)(x + ox, y + oy)));
}

/* ======================================================================== */
/* spatial-effect kernels (identical to the LUT module's; blurs host-side)  */
/* ======================================================================== */

/* Direct (exact) separable Gaussian convolution, one pass along rows or
 * columns. `weights` holds 2*radius+1 normalized taps built host-side by
 * sf_gauss_kernel_1d() (spektra_core.c/.h) -- the same kernel the CPU path
 * convolves with, so GPU and CPU renders match. Clamp-to-edge boundary.
 * Separate _row/_col entry points (rather than a stride parameter) keep the
 * inner loop's memory access pattern explicit at the call site. Separate
 * _1c/_4c variants avoid packing a lone scatter-stage channel into an
 * otherwise-wasted float4. */
__kernel void spektrafilm_gauss_row_4c(__global const float4 *src, __global float4 *dst,
                                       const int w, const int h,
                                       __global const float *weights, const int radius)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= w || y >= h) return;
  float4 acc = (float4)(0.0f);
  for(int k = -radius; k <= radius; k++)
  {
    int xx = x + k;
    xx = xx < 0 ? 0 : (xx >= w ? w - 1 : xx);
    acc += weights[k + radius] * src[(size_t)y * w + xx];
  }
  dst[(size_t)y * w + x] = acc;
}

__kernel void spektrafilm_gauss_col_4c(__global const float4 *src, __global float4 *dst,
                                       const int w, const int h,
                                       __global const float *weights, const int radius)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= w || y >= h) return;
  float4 acc = (float4)(0.0f);
  for(int k = -radius; k <= radius; k++)
  {
    int yy = y + k;
    yy = yy < 0 ? 0 : (yy >= h ? h - 1 : yy);
    acc += weights[k + radius] * src[(size_t)yy * w + x];
  }
  dst[(size_t)y * w + x] = acc;
}

__kernel void spektrafilm_gauss_row_1c(__global const float *src, __global float *dst,
                                       const int w, const int h,
                                       __global const float *weights, const int radius)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= w || y >= h) return;
  float acc = 0.0f;
  for(int k = -radius; k <= radius; k++)
  {
    int xx = x + k;
    xx = xx < 0 ? 0 : (xx >= w ? w - 1 : xx);
    acc += weights[k + radius] * src[(size_t)y * w + xx];
  }
  dst[(size_t)y * w + x] = acc;
}

__kernel void spektrafilm_gauss_col_1c(__global const float *src, __global float *dst,
                                       const int w, const int h,
                                       __global const float *weights, const int radius)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= w || y >= h) return;
  float acc = 0.0f;
  for(int k = -radius; k <= radius; k++)
  {
    int yy = y + k;
    yy = yy < 0 ? 0 : (yy >= h ? h - 1 : yy);
    acc += weights[k + radius] * src[(size_t)yy * w + x];
  }
  dst[(size_t)y * w + x] = acc;
}

__kernel void spektrafilm_scatter_combine(__global const float4 *raw, __global const float4 *core,
                                          __global const float4 *tail, __global float4 *out,
                                          const int w, const int h, const float s_amount,
                                          const float ws_r, const float ws_g, const float ws_b)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= w || y >= h) return;
  size_t k = (size_t)y * w + x;
  float4 r = raw[k], c = core[k], t = tail[k], o;
  o.x = r.x + s_amount * (((1.f - ws_r) * c.x + ws_r * t.x) - r.x);
  o.y = r.y + s_amount * (((1.f - ws_g) * c.y + ws_g * t.y) - r.y);
  o.z = r.z + s_amount * (((1.f - ws_b) * c.z + ws_b * t.z) - r.z);
  o.w = r.w;
  out[k] = o;
}

__kernel void spektrafilm_accum(__global const float4 *blurred, __global float4 *acc, const int w,
                                const int h, const float wk, const int reset)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= w || y >= h) return;
  size_t k = (size_t)y * w + x;
  float4 b = blurred[k];
  float4 a = reset ? (float4)(0.f) : acc[k];
  a.x += wk * b.x;
  a.y += wk * b.y;
  a.z += wk * b.z;
  acc[k] = a;
}

/* Pull one channel out of a float4 buffer into a packed single-channel
 * buffer, so it can be blurred on its own (1 channel of work) instead of
 * blurring all 4 channels of a float4 buffer just to keep 1 of them. */
__kernel void spektrafilm_channel_extract(__global const float4 *src, __global float *dst,
                                          const int w, const int h, const int channel)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= w || y >= h) return;
  size_t k = (size_t)y * w + x;
  float4 s = src[k];
  dst[k] = (channel == 0) ? s.x : (channel == 1) ? s.y : s.z;
}

/* Accumulate weight*blurred[k] (a single-channel buffer, already blurred with
 * that channel's own sigma via spektrafilm_channel_extract + a 1-channel
 * Gaussian blur) into acc[.channel] only, leaving the other two channels of
 * acc untouched (unless reset, which zeroes all of acc.xyz once up front).
 * Used to assemble a genuinely per-channel-sigma blur: each channel gets its
 * own extract + blur + accum, at 1x the per-channel blur cost instead of
 * blurring a full float4 (4x the work) just to keep one channel of it.
 * Channel is 0=R, 1=G, 2=B; alpha (.w) is left as acc's own, unset here
 * since none of the scatter/tail stages carry alpha. */
__kernel void spektrafilm_channel_accum(__global const float *blurred, __global float4 *acc,
                                        const int w, const int h, const float weight,
                                        const int channel, const int reset)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= w || y >= h) return;
  size_t k = (size_t)y * w + x;
  const float bv = blurred[k];
  float4 a = reset ? (float4)(0.f) : acc[k];
  const float av = (channel == 0) ? a.x : (channel == 1) ? a.y : a.z;
  const float nv = av + weight * bv;
  if(channel == 0) a.x = nv; else if(channel == 1) a.y = nv; else a.z = nv;
  acc[k] = a;
}

__kernel void spektrafilm_halation_apply(__global float4 *raw, __global const float4 *blur,
                                         const int w, const int h, const float a_r, const float a_g,
                                         const float a_b)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= w || y >= h) return;
  size_t k = (size_t)y * w + x;
  float4 r = raw[k], b = blur[k];
  r.x = (r.x + a_r * b.x) / (1.f + a_r);
  r.y = (r.y + a_g * b.y) / (1.f + a_g);
  r.z = (r.z + a_b * b.z) / (1.f + a_b);
  raw[k] = r;
}

__kernel void spektrafilm_max_partials(__global const float4 *plane, const int npix,
                                       __global float *partials, const int npartials)
{
  const int gid = get_global_id(0);
  if(gid >= npartials) return;
  float m = 0.0f;
  for(int i = gid; i < npix; i += npartials)
  {
    float4 p = plane[i];
    m = fmax(m, fmax(p.x, fmax(p.y, p.z)));
  }
  partials[gid] = m;
}

__kernel void spektrafilm_max_reduce(__global const float *partials, __global float *maxv_buf,
                                     const int npartials)
{
  float m = 0.0f;
  for(int i = 0; i < npartials; i++) m = fmax(m, partials[i]);
  maxv_buf[0] = m;
}

__kernel void spektrafilm_boost(__global float4 *plane, const int w, const int h,
                                const float boost_ev, const float boost_range,
                                const float protect_ev, __global const float *maxv_buf)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= w || y >= h) return;
  const int k = y * w + x;
  const float maxv = maxv_buf[0];
  if(boost_ev <= 0.0f || maxv <= 0.0f) return;

  const float midgray = 0.184f;
  const float rng = fmin(fmax(boost_range, 0.0f), 1.0f);
  const float raw_x0 = midgray * exp2(fmax(protect_ev, 0.0f));
  if(raw_x0 >= maxv) return;
  const float a = pow(28.0f, 1.0f - rng);
  const float x0 = raw_x0 / maxv;
  const float denom = exp(a * (1.0f - x0)) - a * (1.0f - x0) - 1.0f;
  if(denom <= 0.0f) return;
  const float kk = (exp2(boost_ev) - 1.0f) / denom;
  const float inv_max = 1.0f / maxv, boost_scale = kk * maxv;

  float4 p = plane[k];
  float v[3] = { p.x, p.y, p.z };
  for(int c = 0; c < 3; c++)
  {
    if(v[c] > raw_x0)
    {
      const float dx = (v[c] - raw_x0) * inv_max;
      v[c] = v[c] + boost_scale * (exp(a * dx) - a * dx - 1.0f);
    }
  }
  plane[k] = (float4)(v[0], v[1], v[2], p.w);
}

__kernel void spektrafilm_diffusion_accum(__global const float4 *blurred, __global float4 *acc,
                                          const int w, const int h, const float wr, const float wg,
                                          const float wb, const int reset)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= w || y >= h) return;
  const int k = y * w + x;
  float4 b = blurred[k];
  float4 a = reset ? (float4)(0.f, 0.f, 0.f, 0.f) : acc[k];
  acc[k] = (float4)(a.x + wr * b.x, a.y + wg * b.y, a.z + wb * b.z, b.w);
}

__kernel void spektrafilm_diffusion_mix(__global float4 *plane, __global const float4 *acc,
                                        const int w, const int h, const float p_s)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= w || y >= h) return;
  const int k = y * w + x;
  float4 e = plane[k], s = acc[k];
  plane[k] = (float4)((1.f - p_s) * e.x + p_s * s.x, (1.f - p_s) * e.y + p_s * s.y,
                      (1.f - p_s) * e.z + p_s * s.z, e.w);
}
