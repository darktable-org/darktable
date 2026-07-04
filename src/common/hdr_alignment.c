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

/* C port of the HDR use-case (`--preset hdr`) of the reference
 * align_and_blend.py.  This translation unit owns everything that is naturally
 * C and darktable-shaped:
 *
 *   - CFA mosaic  ->  reduced-resolution, CFA-free luma proxy
 *   - percentile normalization (np.percentile(1,99) + clip equivalent)
 *   - proxy-coordinate -> full-resolution homography rescale
 *   - CFA-aware (same-color) resampling of the full-resolution mosaic
 *   - warp sanity / corner-drift reliability gates
 *   - per-frame orchestration (set_reference / align_frame)
 *
 * The OpenCV-dependent primitives (SIFT, FLANN, findHomography) are reached
 * through the C-ABI seam declared in hdr_alignment.h and implemented in
 * hdr_alignment_cv.cc.
 *
 * See dev-doc/HDR_Alignment_Design.md for the full design and the mapping back
 * to align_and_blend.py.
 */

#include "common/hdr_alignment.h"

#include "common/darktable.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* CFA color lookup.  Reproduced from develop/imageop_math.h (FC / FCNxtrans) so
 * this lean common file does not have to pull in the OpenCL / imageop headers
 * that imageop_math.h depends on. */
static inline int _fc(const size_t row, const size_t col, const uint32_t filters)
{
  return filters >> (((row << 1 & 14) + (col & 1)) << 1) & 3;
}

static inline int _fcol(const int row, const int col, const uint32_t filters,
                        const uint8_t (*const xtrans)[6])
{
  if(filters == 9u)
    // +600 (a multiple of the 6x6 X-Trans period) keeps the index non-negative.
    return xtrans[(row + 600) % 6][(col + 600) % 6];
  return _fc((size_t)row, (size_t)col, filters);
}

/* ------------------------------------------------------------------------- *
 *  Constants ported from the HDR path of align_and_blend.py.
 *  (Only the subset reached by `--preset hdr` is reproduced here.)
 * ------------------------------------------------------------------------- */

// percentile_normalize() bounds
#define DT_HDR_PERCENTILE_LOW 1.0
#define DT_HDR_PERCENTILE_HIGH 99.0
#define DT_HDR_SMALL_EPS 1e-6

// Default per-frame SIFT keypoint budget after spatial balancing (the backend's
// kSiftSpatialBalanceTarget).  Kept here so it can seed the runtime parameter.
#define DT_HDR_SIFT_KEYPOINTS 5000

// Feature-proxy downscale relative to the full-resolution mosaic.  The Python
// prototype does its SIFT work at ~0.627x of the (already demosaiced) image;
// darktable historically used a 2x2-block proxy (0.5x).  A higher-resolution
// proxy carries more distinctive detail -> more SIFT keypoints survive and the
// matcher discriminates periodic structure better (fewer aliased lock-ons), at a
// roughly (scale/0.5)^2 cost in SIFT time.  Built CFA-free (see _build_proxy).
// This is the robust half of the distinctiveness work (the linear raw proxy
// yields far fewer SIFT keypoints than the prototype's tone-mapped JPEG -- e.g.
// 4242 vs 43626 raw on the same frame).  Override at build time
// (-DDT_HDR_PROXY_SCALE=0.5) to restore the legacy behaviour / speed.
#ifndef DT_HDR_PROXY_SCALE
#define DT_HDR_PROXY_SCALE 0.625
#endif

// Perceptual (display-gamma) encoding applied to the 8-bit SIFT proxy after the
// percentile stretch, before CLAHE, to make the linear raw proxy more
// display-like (closer to the prototype's tone-mapped JPEG input).  Note the
// preceding percentile stretch already performs the global [1,99] stretch, so
// this is mostly a *redistribution* on top of it: in testing it lifted keypoints
// on mid-key scenes but slightly reduced them on noisy deep-shadow frames (where
// the stretch already amplifies shadow noise).  Hence it is exposed as a
// build-time knob: 1.0 disables it (-DDT_HDR_PROXY_FEATURE_GAMMA=1.0); raise it
// for a stronger shadow lift.
#ifndef DT_HDR_PROXY_FEATURE_GAMMA
#define DT_HDR_PROXY_FEATURE_GAMMA 2.2
#endif

// CLAHE clip limit applied to the 8-bit SIFT proxy before detection.  0 disables
// it.  Off by default: the display-gamma proxy already mimics the prototype's
// tone-mapped JPEG, and -- as align_and_blend.py warns (SIFT_USE_CLAHE = False)
// -- CLAHE on repetitive textures changes descriptor signatures and manufactures
// false matches (the period-aliasing failure mode).  Raise it (e.g. 2.0) for
// genuinely feature-starved / extreme-DR brackets.
#ifndef DT_HDR_CLAHE_CLIP
#define DT_HDR_CLAHE_CLIP 0.0
#endif

// Longest-side resolution of the auto-reference SIFT probe (AUTO_REFERENCE_PROBE_DIM).
#define DT_HDR_AUTO_REFERENCE_PROBE_DIM 1500

// Feature-init reliability (FEATURE_HOMOGRAPHY_MIN_INLIERS).
#define DT_HDR_FEATURE_MIN_INLIERS 50

// Warp sanity bounds (WARP_SANITY_*).
#define DT_HDR_WARP_MAX_TRANSLATION_DIAG_FRAC 0.30
#define DT_HDR_WARP_SCALE_MIN 0.5
#define DT_HDR_WARP_SCALE_MAX 2.0

// Below this full-resolution corner motion the warp is treated as a no-op and
// the mosaic is copied through unresampled (avoids needlessly softening a
// frame that did not actually move).
#define DT_HDR_NOOP_MAX_CORNER_PX 0.5

// Half-width (px) of the separable same-color resampling tent.  For a Bayer
// period-2 sublattice this reduces to exact bilinear; for X-Trans it is a
// smooth same-color weighted average.
#define DT_HDR_CFA_TENT_RADIUS 2

// Accepted ranges for the runtime-tunable parameters.  These MUST stay in sync
// with the <type min .. max> bounds advertised for the matching
// plugins/lighttable/hdr_merge_* keys in data/darktableconfig.xml.in; the
// clamps in dt_hdr_alignment_new() are the safety net behind that UI.
#define DT_HDR_PROXY_SCALE_MIN 0.25
#define DT_HDR_PROXY_SCALE_MAX 1.0
#define DT_HDR_FEATURE_GAMMA_MIN 1.0
#define DT_HDR_FEATURE_GAMMA_MAX 6.0
#define DT_HDR_CLAHE_CLIP_MIN 0.0
#define DT_HDR_CLAHE_CLIP_MAX 16.0
#define DT_HDR_SIFT_KEYPOINTS_MIN 500
#define DT_HDR_SIFT_KEYPOINTS_MAX 20000

/* ------------------------------------------------------------------------- *
 *  Alignment state.
 * ------------------------------------------------------------------------- */

struct dt_hdr_align_t
{
  // Runtime-tunable parameters (proxy scale, feature gamma, CLAHE clip, SIFT
  // budget), seeded from the compile-time defaults and overridable per run.
  dt_hdr_align_params_t params;

  // Per-merge debug-image policy, resolved once in dt_hdr_alignment_new() (env
  // override or the preference).  NULL => debug images off.  `debug_frame` is
  // bumped for each aligned moving frame so its visuals share a number.  Owned
  // here (not a backend global) so concurrent HDR merges cannot interfere.
  gchar *debug_dir;
  int debug_frame;

  // Cached reference SIFT features (opaque dt_hdr_cv_features handle), computed
  // once from the reference proxy in set_reference and matched against by every
  // moving frame -- so the reference is detected once per merge, not N-1 times.
  // NULL without OpenCV or if the precompute failed.
  void *ref_features;
  int pw, ph;             // proxy dimensions

  // Reference full-resolution geometry / CFA description.
  int width, height;
  uint32_t filters;
  uint8_t xtrans[6][6];

  gboolean have_reference;
};

/* ------------------------------------------------------------------------- *
 *  Small 3x3 (row-major) homography helpers.
 *
 *  These (and the gate / CFA-warp / logging helpers further down) are only used
 *  on the alignment path, which needs OpenCV; guard them so a build without
 *  OpenCV does not trip -Werror=unused-function on the plain static helpers.
 * ------------------------------------------------------------------------- */
#ifdef HAVE_OPENCV

static void _h_identity(double H[9])
{
  H[0] = 1.0; H[1] = 0.0; H[2] = 0.0;
  H[3] = 0.0; H[4] = 1.0; H[5] = 0.0;
  H[6] = 0.0; H[7] = 0.0; H[8] = 1.0;
}

// Map a point through a row-major homography.
static inline void _h_apply(const double H[9], double x, double y, double *ox, double *oy)
{
  const double w = H[6] * x + H[7] * y + H[8];
  const double iw = (fabs(w) < 1e-12) ? 1e12 : 1.0 / w;
  *ox = (H[0] * x + H[1] * y + H[2]) * iw;
  *oy = (H[3] * x + H[4] * y + H[5]) * iw;
}

// Rescale a homography estimated in proxy coordinates to full-resolution
// coordinates: H_full = S * H_proxy * S^-1 with S = diag(sx, sy, 1), where sx/sy
// are the full-res / proxy size ratios (≈ 1 / DT_HDR_PROXY_SCALE).  They are
// nearly equal (rounding of the proxy dims) but kept independent so an off-square
// rounding cannot skew the warp.  Expanding S*H*S^-1 gives the per-entry factors
// below (translation scales up, perspective scales down, the cross terms by the
// aspect ratio sx/sy).
static void _h_scale_proxy_to_full(double H[9], double sx, double sy)
{
  H[1] *= sx / sy;  // b
  H[2] *= sx;       // translation x
  H[3] *= sy / sx;  // d
  H[5] *= sy;       // translation y
  H[6] /= sx;       // perspective x
  H[7] /= sy;       // perspective y
}

#endif // HAVE_OPENCV

/* ------------------------------------------------------------------------- *
 *  Percentile bounds (np.percentile(img, (1, 99)) equivalent).
 *
 *  The bounds only gate an 8-bit normalization, so exact order statistics are
 *  unnecessary: a uniform histogram over [min,max] gives bucket-quantized
 *  percentiles that are more than accurate enough, in O(n) and fully parallel
 *  (two reductions, no full sort).  This replaces the previous qsort-of-a-copy,
 *  which was the dominant serial cost on the alignment path.
 * ------------------------------------------------------------------------- */

#define DT_HDR_PERCENTILE_BINS 4096

static void _percentile_bounds(const float *src, size_t n, double low, double high,
                               float *p_low, float *p_high)
{
  *p_low = 0.0f;
  *p_high = 1.0f;
  if(n == 0) return;

  // Pass 1: data range (parallel min/max reduction).
  float lo = src[0], hi = src[0];
  DT_OMP_FOR(reduction(min : lo) reduction(max : hi))
  for(size_t i = 0; i < n; i++)
  {
    const float v = src[i];
    if(v < lo) lo = v;
    if(v > hi) hi = v;
  }
  if(!(hi > lo))  // flat (or single-valued) image: nothing to stretch
  {
    *p_low = lo;
    *p_high = hi;
    return;
  }

  // Pass 2: uniform histogram over [lo,hi] (parallel array reduction).
  uint32_t hist[DT_HDR_PERCENTILE_BINS] = { 0 };
  const double sbin = (double)DT_HDR_PERCENTILE_BINS / ((double)hi - (double)lo);
  DT_OMP_FOR(reduction(+ : hist[:DT_HDR_PERCENTILE_BINS]))
  for(size_t i = 0; i < n; i++)
  {
    int b = (int)(((double)src[i] - (double)lo) * sbin);
    if(b < 0) b = 0; else if(b >= DT_HDR_PERCENTILE_BINS) b = DT_HDR_PERCENTILE_BINS - 1;
    hist[b]++;
  }

  // Walk the cumulative counts to the requested order statistics (nearest-rank),
  // reporting each bin's center as its representative value.
  const double bw = ((double)hi - (double)lo) / (double)DT_HDR_PERCENTILE_BINS;
  const uint64_t rank_lo = (uint64_t)(low / 100.0 * (double)(n - 1) + 0.5);
  const uint64_t rank_hi = (uint64_t)(high / 100.0 * (double)(n - 1) + 0.5);
  uint64_t cum = 0;
  gboolean got_lo = FALSE;
  *p_low = lo;
  *p_high = hi;
  for(int b = 0; b < DT_HDR_PERCENTILE_BINS; b++)
  {
    const uint64_t next = cum + hist[b];
    const float center = (float)((double)lo + ((double)b + 0.5) * bw);
    if(!got_lo && next > rank_lo) { *p_low = center; got_lo = TRUE; }
    if(next > rank_hi) { *p_high = center; break; }
    cum = next;
  }
}

/* ------------------------------------------------------------------------- *
 *  CFA mosaic -> CFA-free luma proxy at a configurable scale.
 *
 *  Luma comes from a stride-1 2x2 window: for *any* 2x2 patch of a Bayer mosaic
 *  (whatever the phase) the four photosites are exactly one R, one B and two G,
 *  so 0.25*(sum) is the CFA-free luma (R + 2G + B)/4 at that location -- full
 *  resolution, no interpolation, no colour bias.  (For X-Trans a 2x2 patch is not
 *  tile-aligned, but averaging still strongly attenuates the CFA modulation, as
 *  before.)  That full-resolution luma is then area-averaged down to the target
 *  proxy size, so the proxy scale is decoupled from the 0.5x the old 2x2-block
 *  reduction was locked to.  A higher scale keeps more of the structural detail
 *  SIFT relies on; see DT_HDR_PROXY_SCALE.
 * ------------------------------------------------------------------------- */

// CFA-free luma at a stride-1 2x2 window with top-left (sx, sy), edge-clamped.
static inline float _luma2x2(const float *m, int width, int height, int sx, int sy)
{
  if(sx < 0) sx = 0; else if(sx > width - 2) sx = width - 2;
  if(sy < 0) sy = 0; else if(sy > height - 2) sy = height - 2;
  const float a = m[(size_t)sy * width + sx];
  const float b = m[(size_t)sy * width + sx + 1];
  const float c = m[(size_t)(sy + 1) * width + sx];
  const float d = m[(size_t)(sy + 1) * width + sx + 1];
  return 0.25f * (a + b + c + d);
}

static float *_build_proxy(const float *mosaic, int width, int height, double scale,
                           int *pw_out, int *ph_out)
{
  if(width < 4 || height < 4) return NULL;
  int pw = (int)lround((double)width * scale);
  int ph = (int)lround((double)height * scale);
  if(pw < 8 || ph < 8) return NULL;
  if(pw > width) pw = width;
  if(ph > height) ph = height;

  float *proxy = dt_alloc_align_float((size_t)pw * ph);
  if(!proxy) return NULL;

  // Source mosaic pixels per proxy pixel (>= ~1; equals 1/scale).
  const double fx = (double)width / (double)pw;
  const double fy = (double)height / (double)ph;

  DT_OMP_FOR()
  for(int ty = 0; ty < ph; ty++)
  {
    int y0 = (int)(ty * fy);
    int y1 = (int)((ty + 1) * fy);
    if(y1 <= y0) y1 = y0 + 1;
    if(y1 > height - 1) y1 = height - 1;
    if(y0 > height - 2) y0 = height - 2;
    for(int tx = 0; tx < pw; tx++)
    {
      int x0 = (int)(tx * fx);
      int x1 = (int)((tx + 1) * fx);
      if(x1 <= x0) x1 = x0 + 1;
      if(x1 > width - 1) x1 = width - 1;
      if(x0 > width - 2) x0 = width - 2;
      float sum = 0.0f;
      int cnt = 0;
      for(int sy = y0; sy < y1; sy++)
        for(int sx = x0; sx < x1; sx++)
        {
          sum += _luma2x2(mosaic, width, height, sx, sy);
          cnt++;
        }
      proxy[(size_t)ty * pw + tx] = (cnt > 0) ? sum / (float)cnt : 0.0f;
    }
  }

  *pw_out = pw;
  *ph_out = ph;
  return proxy;
}

// Build the 8-bit SIFT proxy from a float luma proxy: percentile-stretch, apply a
// perceptual (display-gamma) encoding, then scale to [0,255].  The gamma step
// lifts shadow detail out of the linear raw signal into SIFT's operating range
// (see DT_HDR_PROXY_FEATURE_GAMMA); local-contrast enhancement (CLAHE) is applied
// afterwards, in the backend, just before SIFT detection.  Mirrors
// _to_feature_uint8() feeding display-referred data.
//
// The stretch and the gamma curve are fused into a single parallel pass (no
// intermediate [0,1] buffer), and the gamma is read from a small LUT so the hot
// loop is a clamp + table lookup instead of a per-pixel powf().
#define DT_HDR_GAMMA_LUT_SIZE 4096
static uint8_t *_proxy_to_u8(const float *proxy_f, int pw, int ph, double gamma)
{
  const size_t n = (size_t)pw * ph;
  uint8_t *out = dt_alloc_align_uint8(n);
  if(!out) return NULL;

  float p_low, p_high;
  _percentile_bounds(proxy_f, n, DT_HDR_PERCENTILE_LOW, DT_HDR_PERCENTILE_HIGH, &p_low, &p_high);
  const float scale = 1.0f / ((p_high - p_low) + (float)DT_HDR_SMALL_EPS);

  // Precompute the display-gamma response over the normalized [0,1] domain.
  const float inv_gamma = 1.0f / (float)(gamma > 0.0 ? gamma : 1.0);
  uint8_t lut[DT_HDR_GAMMA_LUT_SIZE];
  for(int k = 0; k < DT_HDR_GAMMA_LUT_SIZE; k++)
  {
    const float u = (float)k / (float)(DT_HDR_GAMMA_LUT_SIZE - 1);
    const float v = powf(u, inv_gamma) * 255.0f;
    lut[k] = (uint8_t)(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v));
  }

  DT_OMP_FOR()
  for(size_t i = 0; i < n; i++)
  {
    float nv = (proxy_f[i] - p_low) * scale;          // percentile stretch
    nv = nv < 0.0f ? 0.0f : (nv > 1.0f ? 1.0f : nv);  // clip to [0,1]
    const int idx = (int)(nv * (float)(DT_HDR_GAMMA_LUT_SIZE - 1) + 0.5f);
    out[i] = lut[idx];                                // gamma encode + quantize
  }
  return out;
}

/* ------------------------------------------------------------------------- *
 *  Reliability gates (ported from _warp_sanity_check).
 * ------------------------------------------------------------------------- */
#ifdef HAVE_OPENCV

static gboolean _warp_is_sane(const double H[9], int w, int h)
{
  const double diag = hypot((double)w, (double)h);
  // Translation as a fraction of the image diagonal.
  if(hypot(H[2], H[5]) > DT_HDR_WARP_MAX_TRANSLATION_DIAG_FRAC * diag) return FALSE;
  // Scale = column norms of the upper-left 2x2.
  const double s1 = hypot(H[0], H[3]);
  const double s2 = hypot(H[1], H[4]);
  if(s1 < DT_HDR_WARP_SCALE_MIN || s1 > DT_HDR_WARP_SCALE_MAX) return FALSE;
  if(s2 < DT_HDR_WARP_SCALE_MIN || s2 > DT_HDR_WARP_SCALE_MAX) return FALSE;
  return TRUE;
}

// Per-corner displacement between two warps; fills the 4 distances (px) and
// returns the maximum.  Median is derived by the caller.
static double _corner_drift(const double Ha[9], const double Hb[9], int w, int h, double dist[4])
{
  const double cx[4] = { 0.0, (double)w, 0.0, (double)w };
  const double cy[4] = { 0.0, 0.0, (double)h, (double)h };
  double maxd = 0.0;
  for(int i = 0; i < 4; i++)
  {
    double ax, ay, bx, by;
    _h_apply(Ha, cx[i], cy[i], &ax, &ay);
    _h_apply(Hb, cx[i], cy[i], &bx, &by);
    dist[i] = hypot(ax - bx, ay - by);
    if(dist[i] > maxd) maxd = dist[i];
  }
  return maxd;
}

/* ------------------------------------------------------------------------- *
 *  CFA-aware (same-color) resampling.
 *
 *  Fill output photosite (x, y) -- whose CFA color is _fcol(y, x) -- by mapping
 *  it through the full-resolution homography into the moving mosaic and
 *  interpolating ONLY from moving-mosaic photosites of the same color, using a
 *  separable width-(2*radius) tent.  Because we always read color c and write
 *  it to a cell declared color c, the reference frame's mosaic phase is
 *  preserved exactly (both frames share the camera's CFA layout).
 * ------------------------------------------------------------------------- */

static inline float _sample_cfa_same_color(const float *mosaic, int w, int h,
                                           uint32_t filters, const uint8_t (*xtrans)[6],
                                           int c, double sx, double sy)
{
  // Clamp the sampling center to the valid region (clamp-to-edge border).
  if(sx < 0.0) sx = 0.0;
  if(sy < 0.0) sy = 0.0;
  if(sx > (double)(w - 1)) sx = (double)(w - 1);
  if(sy > (double)(h - 1)) sy = (double)(h - 1);

  const int x0 = (int)floor(sx) - DT_HDR_CFA_TENT_RADIUS;
  const int x1 = (int)floor(sx) + DT_HDR_CFA_TENT_RADIUS;
  const int y0 = (int)floor(sy) - DT_HDR_CFA_TENT_RADIUS;
  const int y1 = (int)floor(sy) + DT_HDR_CFA_TENT_RADIUS;
  const double inv_r = 1.0 / (double)DT_HDR_CFA_TENT_RADIUS;

  double acc = 0.0;
  double wsum = 0.0;
  for(int j = y0; j <= y1; j++)
  {
    if(j < 0 || j >= h) continue;
    const double wy = 1.0 - fabs(sy - (double)j) * inv_r;
    if(wy <= 0.0) continue;
    for(int i = x0; i <= x1; i++)
    {
      if(i < 0 || i >= w) continue;
      if(_fcol(j, i, filters, xtrans) != c) continue;
      const double wx = 1.0 - fabs(sx - (double)i) * inv_r;
      if(wx <= 0.0) continue;
      const double wgt = wx * wy;
      acc += wgt * (double)mosaic[(size_t)j * w + i];
      wsum += wgt;
    }
  }
  if(wsum > 0.0) return (float)(acc / wsum);

  // Degenerate fallback: no same-color tap had positive tent weight.  This only
  // happens at extreme X-Trans borders, where the clamped tent window can shrink
  // to a patch that carries none of colour c.  Search outward in rings for the
  // nearest in-bounds photosite that *does* carry colour c, so we never write a
  // different-colour sample into a cell declared colour c (which would corrupt
  // the mosaic phase).  The X-Trans period is 6, so a same-colour site is always
  // found within a few rings; the loop is bounded for safety regardless.
  int bx = (int)(sx + 0.5);
  int by = (int)(sy + 0.5);
  if(bx < 0) bx = 0; else if(bx > w - 1) bx = w - 1;
  if(by < 0) by = 0; else if(by > h - 1) by = h - 1;
  for(int rad = 0; rad <= 8; rad++)
  {
    float best = 0.0f;
    double best_d2 = 0.0;
    gboolean found = FALSE;
    for(int j = by - rad; j <= by + rad; j++)
    {
      if(j < 0 || j >= h) continue;
      for(int i = bx - rad; i <= bx + rad; i++)
      {
        if(i < 0 || i >= w) continue;
        // ring only: skip interior cells already searched at a smaller radius
        if(rad > 0 && abs(i - bx) != rad && abs(j - by) != rad) continue;
        if(_fcol(j, i, filters, xtrans) != c) continue;
        const double d2 = (sx - i) * (sx - i) + (sy - j) * (sy - j);
        if(!found || d2 < best_d2) { best_d2 = d2; best = mosaic[(size_t)j * w + i]; found = TRUE; }
      }
    }
    if(found) return best;
  }
  // Should be unreachable for a valid CFA; keep a defined result just in case.
  return mosaic[(size_t)by * w + bx];
}

// Bayer fast path for the same-color sampler.  A Bayer CFA is period-2, so in a
// given row the photosites of color c sit on a single column parity (for R/B the
// color occupies only alternate rows; green occupies every row but flips parity).
// We therefore find that parity from the canonical 2x2 cell and step the inner
// loop by 2, visiting exactly the same-color taps with no per-tap _fcol() test.
// The weights and clamp-to-edge fallback are identical to the general sampler, so
// the result is bit-for-bit the same -- only ~1/3 the inner iterations.
static inline float _sample_bayer_same_color(const float *mosaic, int w, int h,
                                             uint32_t filters, int c, double sx, double sy)
{
  if(sx < 0.0) sx = 0.0;
  if(sy < 0.0) sy = 0.0;
  if(sx > (double)(w - 1)) sx = (double)(w - 1);
  if(sy > (double)(h - 1)) sy = (double)(h - 1);

  const int x0 = (int)floor(sx) - DT_HDR_CFA_TENT_RADIUS;
  const int x1 = (int)floor(sx) + DT_HDR_CFA_TENT_RADIUS;
  const int y0 = (int)floor(sy) - DT_HDR_CFA_TENT_RADIUS;
  const int y1 = (int)floor(sy) + DT_HDR_CFA_TENT_RADIUS;
  const double inv_r = 1.0 / (double)DT_HDR_CFA_TENT_RADIUS;

  double acc = 0.0;
  double wsum = 0.0;
  for(int j = y0; j <= y1; j++)
  {
    if(j < 0 || j >= h) continue;
    const double wy = 1.0 - fabs(sy - (double)j) * inv_r;
    if(wy <= 0.0) continue;

    // Column parity carrying color c in this row (& 1 is well-defined for the
    // negative window edge in two's complement: it yields the 0/1 parity).
    const int row_par = j & 1;
    int col_par;
    if(_fc((size_t)row_par, 0, filters) == c) col_par = 0;
    else if(_fc((size_t)row_par, 1, filters) == c) col_par = 1;
    else continue;  // this row carries no color c (e.g. an R/B row of the wrong parity)

    int istart = x0;
    if((istart & 1) != col_par) istart++;
    for(int i = istart; i <= x1; i += 2)
    {
      if(i < 0 || i >= w) continue;
      const double wx = 1.0 - fabs(sx - (double)i) * inv_r;
      if(wx <= 0.0) continue;
      const double wgt = wx * wy;
      acc += wgt * (double)mosaic[(size_t)j * w + i];
      wsum += wgt;
    }
  }
  if(wsum > 0.0) return (float)(acc / wsum);

  const int ix = (int)(sx + 0.5);
  const int iy = (int)(sy + 0.5);
  return mosaic[(size_t)iy * w + ix];
}

static void _warp_mosaic_cfa(const float *mosaic, float *out, int width, int height,
                             uint32_t filters, const uint8_t (*xtrans)[6], const double H[9])
{
  // Hoist the CFA dispatch out of the per-pixel loop: Bayer uses the period-2
  // fast path, X-Trans the general same-color sampler.
  if(filters != 9u)
  {
    DT_OMP_FOR(collapse(2))
    for(int y = 0; y < height; y++)
      for(int x = 0; x < width; x++)
      {
        const int c = _fcol(y, x, filters, xtrans);
        double sx, sy;
        _h_apply(H, (double)x, (double)y, &sx, &sy);
        out[(size_t)y * width + x] =
            _sample_bayer_same_color(mosaic, width, height, filters, c, sx, sy);
      }
  }
  else
  {
    DT_OMP_FOR(collapse(2))
    for(int y = 0; y < height; y++)
      for(int x = 0; x < width; x++)
      {
        const int c = _fcol(y, x, filters, xtrans);
        double sx, sy;
        _h_apply(H, (double)x, (double)y, &sx, &sy);
        out[(size_t)y * width + x] =
            _sample_cfa_same_color(mosaic, width, height, filters, xtrans, c, sx, sy);
      }
  }
}

#endif // HAVE_OPENCV

/* ------------------------------------------------------------------------- *
 *  Public API.
 * ------------------------------------------------------------------------- */

#ifdef HAVE_OPENCV
// Log a 3x3 homography (row-major) and its decomposition, matching the Python
// reference's log_warp() / decompose_warp() (MOTION_HOMOGRAPHY) so the C output
// can be compared line-by-line with align_and_blend.py.
static void _log_warp(const char *title, const double H[9])
{
  dt_print(DT_DEBUG_HDR_MERGE, "  %s:", title);
  dt_print(DT_DEBUG_HDR_MERGE, "    [ %.6f %.6f %.6f ]", H[0], H[1], H[2]);
  dt_print(DT_DEBUG_HDR_MERGE, "    [ %.6f %.6f %.6f ]", H[3], H[4], H[5]);
  dt_print(DT_DEBUG_HDR_MERGE, "    [ %.6f %.6f %.6f ]", H[6], H[7], H[8]);

  // Decompose the (homography-normalized) linear part via QR: L = Q*R, with Q a
  // pure rotation and R upper-triangular [[sx, shear], [0, sy]].
  double h22 = H[8];
  if(fabs(h22) < DT_HDR_SMALL_EPS) h22 = (h22 >= 0.0 ? DT_HDR_SMALL_EPS : -DT_HDR_SMALL_EPS);
  const double tx = H[2] / h22, ty = H[5] / h22;
  const double px = H[6] / h22, py = H[7] / h22;
  const double a = H[0] / h22, b = H[1] / h22, c = H[3] / h22, d = H[4] / h22;
  const double r11 = hypot(a, c);
  double rot_deg = 0.0, scale_x = r11, scale_y = 0.0, shear = 0.0;
  if(r11 > DT_HDR_SMALL_EPS)
  {
    const double r12 = (a * b + c * d) / r11;
    const double c2x = b - r12 * (a / r11);
    const double c2y = d - r12 * (c / r11);
    rot_deg = atan2(c, a) * (180.0 / M_PI);  // atan2(Q[1,0], Q[0,0])
    scale_x = r11;
    scale_y = hypot(c2x, c2y);
    shear = r12;
  }
  dt_print(DT_DEBUG_HDR_MERGE, "  Decomposition:");
  dt_print(DT_DEBUG_HDR_MERGE, "    translation_x: %.2f", tx);
  dt_print(DT_DEBUG_HDR_MERGE, "    translation_y: %.2f", ty);
  dt_print(DT_DEBUG_HDR_MERGE, "    perspective_x: %.2f", px);
  dt_print(DT_DEBUG_HDR_MERGE, "    perspective_y: %.2f", py);
  dt_print(DT_DEBUG_HDR_MERGE, "    rotation_deg: %.2f deg", rot_deg);
  dt_print(DT_DEBUG_HDR_MERGE, "    scale_x: %.2f", scale_x);
  dt_print(DT_DEBUG_HDR_MERGE, "    scale_y: %.2f", scale_y);
  dt_print(DT_DEBUG_HDR_MERGE, "    shear: %.2f", shear);
}

// Log the SIFT+RANSAC initialization metrics block (mirrors log_feature_init_stats).
static void _log_feature_stats(const dt_hdr_cv_feature_stats_t *s)
{
  dt_print(DT_DEBUG_HDR_MERGE, "  SIFT+RANSAC initialization metrics:");
  dt_print(DT_DEBUG_HDR_MERGE, "    keypoints (template/image): %d/%d", s->kp_template, s->kp_image);
  dt_print(DT_DEBUG_HDR_MERGE, "    good matches: %d", s->good_matches);
  if(s->good_matches > 0)
    dt_print(DT_DEBUG_HDR_MERGE, "    mutual-consistent matches: %d", s->good_matches);
  if(s->good_matches > 0)
    dt_print(DT_DEBUG_HDR_MERGE, "    RANSAC inliers: %d/%d (%.0f%%)",
             s->inliers, s->good_matches, 100.0 * s->inliers / s->good_matches);
  else
    dt_print(DT_DEBUG_HDR_MERGE, "    RANSAC inliers: %d", s->inliers);
  if(s->reproj_mean >= 0.0)
    dt_print(DT_DEBUG_HDR_MERGE, "    reproj mean/median/max: %.2f / %.2f / %.2f px",
             s->reproj_mean, s->reproj_median, s->reproj_max);
  else
    dt_print(DT_DEBUG_HDR_MERGE, "    reproj mean/median/max: n/a");
  dt_print(DT_DEBUG_HDR_MERGE, "    transform model: %s",
           s->used_translation ? "translation (cluster-degraded)"
                               : (s->used_affine ? "affine-fallback" : "homography"));
}
#endif // HAVE_OPENCV

void dt_hdr_alignment_default_params(dt_hdr_align_params_t *p)
{
  if(!p) return;
  p->proxy_scale = DT_HDR_PROXY_SCALE;
  p->feature_gamma = DT_HDR_PROXY_FEATURE_GAMMA;
  p->clahe_clip = DT_HDR_CLAHE_CLIP;
  p->sift_keypoints = DT_HDR_SIFT_KEYPOINTS;
  p->debug_images = 0;
}

// Clamp user-supplied parameters to the ranges advertised by the matching
// preferences (see the DT_HDR_*_MIN/MAX defines, kept in sync with
// data/darktableconfig.xml.in).
static double _clampd(double v, double lo, double hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}
static int _clampi(int v, int lo, int hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

// Resolve the per-merge debug-image directory from the env override or the
// preference.  Returns a newly-allocated path (free with g_free) or NULL when
// debug images are off / the directory cannot be created.
static gchar *_resolve_debug_dir(int debug_images)
{
  const char *env = getenv("DT_HDR_DEBUG_IMAGE_DIR");
  gchar *dir = (env && env[0])
                   ? g_strdup(env)
                   : (debug_images
                          ? g_build_filename(g_get_tmp_dir(), "darktable_hdr_align_debug", NULL)
                          : NULL);
  if(!dir) return NULL;

  // Ensure the directory exists (the env override may name a path that does not
  // yet exist); otherwise the backend would silently fail to write every dump.
  if(g_mkdir_with_parents(dir, 0755) != 0)
  {
    dt_print(DT_DEBUG_HDR_MERGE,
             "  [hdr merge] could not create debug image directory '%s'; disabling debug images",
             dir);
    g_free(dir);
    return NULL;
  }
  dt_print(DT_DEBUG_HDR_MERGE, "  debug images -> %s", dir);
  return dir;
}

dt_hdr_align_t *dt_hdr_alignment_new(const dt_hdr_align_params_t *params)
{
  dt_hdr_align_t *a = calloc(1, sizeof(dt_hdr_align_t));
  if(!a) return NULL;
  dt_hdr_alignment_default_params(&a->params);
  if(params)
  {
    a->params.proxy_scale = _clampd(params->proxy_scale,
                                    DT_HDR_PROXY_SCALE_MIN, DT_HDR_PROXY_SCALE_MAX);
    a->params.feature_gamma = _clampd(params->feature_gamma,
                                      DT_HDR_FEATURE_GAMMA_MIN, DT_HDR_FEATURE_GAMMA_MAX);
    a->params.clahe_clip = _clampd(params->clahe_clip,
                                   DT_HDR_CLAHE_CLIP_MIN, DT_HDR_CLAHE_CLIP_MAX);
    a->params.sift_keypoints = _clampi(params->sift_keypoints,
                                       DT_HDR_SIFT_KEYPOINTS_MIN, DT_HDR_SIFT_KEYPOINTS_MAX);
    a->params.debug_images = params->debug_images ? 1 : 0;
  }
  a->debug_dir = _resolve_debug_dir(a->params.debug_images);
  return a;
}

void dt_hdr_alignment_free(dt_hdr_align_t *a)
{
  if(!a) return;
#ifdef HAVE_OPENCV
  if(a->ref_features) dt_hdr_cv_features_destroy(a->ref_features);
#endif
  g_free(a->debug_dir);
  free(a);
}

gboolean dt_hdr_alignment_set_reference(dt_hdr_align_t *a,
                                        const float *mosaic,
                                        int width,
                                        int height,
                                        uint32_t filters,
                                        const uint8_t (*xtrans)[6])
{
  if(!a || !mosaic) return FALSE;

  int pw = 0, ph = 0;
  float *proxy_f = _build_proxy(mosaic, width, height, a->params.proxy_scale, &pw, &ph);
  if(!proxy_f) return FALSE;

  // Build the 8-bit SIFT proxy; the float luma is no longer needed afterwards.
  uint8_t *proxy_u8 = _proxy_to_u8(proxy_f, pw, ph, a->params.feature_gamma);
  dt_free_align(proxy_f);
  if(!proxy_u8) return FALSE;

  a->pw = pw;
  a->ph = ph;
  a->width = width;
  a->height = height;
  a->filters = filters;
  if(xtrans)
    memcpy(a->xtrans, xtrans, sizeof(a->xtrans));
  else
    memset(a->xtrans, 0, sizeof(a->xtrans));

#ifdef HAVE_OPENCV
  // Precompute (and cache) the reference's SIFT features once; every moving frame
  // then matches against this instead of re-detecting the reference.  On failure
  // ref_features stays NULL and frames fall through unaligned (graceful).
  if(a->ref_features) dt_hdr_cv_features_destroy(a->ref_features);
  a->ref_features = dt_hdr_cv_features_create(proxy_u8, pw, ph,
                                              a->params.sift_keypoints, a->params.clahe_clip);
#endif
  dt_free_align(proxy_u8);

  a->have_reference = TRUE;
  dt_print(DT_DEBUG_HDR_MERGE,
           "  reference proxy: %dx%d mosaic -> %dx%d luma (%s)",
           width, height, pw, ph, filters == 9u ? "X-Trans" : "Bayer");
  return TRUE;
}

int dt_hdr_alignment_probe_features(const float *mosaic, int width, int height)
{
#ifndef HAVE_OPENCV
  (void)mosaic;
  (void)width;
  (void)height;
  return 0;
#else
  if(!mosaic) return 0;
  // The probe ranks frames before any state exists, so it uses the default gamma
  // (the ranking only needs to be self-consistent).  SIFT runs at the probe
  // resolution (longest side <= AUTO_REFERENCE_PROBE_DIM), so build the proxy
  // directly at that scale instead of building the full DT_HDR_PROXY_SCALE proxy
  // and letting the backend downscale it -- for a large raw that avoids building
  // (and normalizing) a proxy several times bigger than the probe ever uses.
  const int max_side = (width > height) ? width : height;
  double probe_scale = (double)DT_HDR_AUTO_REFERENCE_PROBE_DIM / (double)(max_side > 0 ? max_side : 1);
  if(probe_scale > DT_HDR_PROXY_SCALE) probe_scale = DT_HDR_PROXY_SCALE;
  int pw = 0, ph = 0;
  float *proxy = _build_proxy(mosaic, width, height, probe_scale, &pw, &ph);
  if(!proxy) return 0;
  uint8_t *u8 = _proxy_to_u8(proxy, pw, ph, DT_HDR_PROXY_FEATURE_GAMMA);
  dt_free_align(proxy);
  if(!u8) return 0;
  // Pass the probe dim through as a safety cap; the proxy is already at or below
  // it, so this normally does not resize again.
  const int n = dt_hdr_cv_count_features(u8, pw, ph, DT_HDR_AUTO_REFERENCE_PROBE_DIM);
  dt_free_align(u8);
  return n;
#endif
}

gboolean dt_hdr_alignment_align_frame(dt_hdr_align_t *a,
                                      const float *mosaic,
                                      float *out,
                                      int width,
                                      int height,
                                      uint32_t filters,
                                      const uint8_t (*xtrans)[6],
                                      dt_hdr_align_result_t *info)
{
  if(info)
    *info = (dt_hdr_align_result_t){ .status = DT_HDR_ALIGN_IDENTITY,
                                     .feature_inliers = 0,
                                     .corner_drift = 0.0 };

  // Default behavior on any early-out: pass the frame through unchanged so the
  // caller accumulates the unaligned mosaic (current darktable behavior).
  if(!a || !a->have_reference || !mosaic || !out || out == mosaic)
  {
    if(out && mosaic && out != mosaic)
      memcpy(out, mosaic, (size_t)width * height * sizeof(float));
    return FALSE;
  }

  // Frames must share geometry with the reference (the merge already enforces
  // identical size / orientation upstream).
  if(width != a->width || height != a->height)
  {
    memcpy(out, mosaic, (size_t)width * height * sizeof(float));
    return FALSE;
  }

#ifndef HAVE_OPENCV
  // Built without OpenCV: registration is unavailable; accumulate unaligned.
  // (filters / xtrans are only consumed by the CFA-aware warp below.)
  (void)filters;
  (void)xtrans;
  if(info) info->status = DT_HDR_ALIGN_DISABLED;
  memcpy(out, mosaic, (size_t)width * height * sizeof(float));
  return FALSE;
#else
  const int pw = a->pw;
  const int ph = a->ph;

  // No reference feature cache (precompute failed / too few features): there is
  // nothing to align against, so pass the frame through unaligned.
  if(!a->ref_features)
  {
    memcpy(out, mosaic, (size_t)width * height * sizeof(float));
    return FALSE;
  }

  // Build the moving frame's proxies.
  int mpw = 0, mph = 0;
  float *mov_f = _build_proxy(mosaic, width, height, a->params.proxy_scale, &mpw, &mph);
  if(!mov_f || mpw != pw || mph != ph)
  {
    if(mov_f) dt_free_align(mov_f);
    memcpy(out, mosaic, (size_t)width * height * sizeof(float));
    return FALSE;
  }
  uint8_t *mov_u8 = _proxy_to_u8(mov_f, pw, ph, a->params.feature_gamma);
  dt_free_align(mov_f);  // float proxy is only an intermediate for the u8 proxy
  if(!mov_u8)
  {
    memcpy(out, mosaic, (size_t)width * height * sizeof(float));
    return FALSE;
  }

  // The per-merge debug-image directory (env override or preference) was
  // resolved once in dt_hdr_alignment_new(); number each aligned moving frame so
  // its visuals share an index.  Both are passed to the backend -- no global
  // debug state -- so concurrent merges stay independent.
  const int frame_index = ++a->debug_frame;

  // --- Stage 1: feature initialization (SIFT + RANSAC homography) ----------
  double H_feature[9];
  _h_identity(H_feature);
  dt_hdr_cv_feature_stats_t fstats = { 0 };
  const int feature_inliers = dt_hdr_cv_feature_homography(
      a->ref_features, mov_u8, pw, ph, a->params.sift_keypoints, a->params.clahe_clip,
      a->debug_dir, frame_index, H_feature, &fstats);
  const gboolean feature_ok = feature_inliers >= DT_HDR_FEATURE_MIN_INLIERS;
  // (the per-stage SIFT / match / inlier lines come from the backend; the
  //  structured metrics block is emitted below.)

  dt_free_align(mov_u8);

  // --- Stage 2: choose and apply the warp ----------------------------------
  // The SIFT feature-init homography is the final warp when it is reliable and
  // geometrically sane; otherwise the frame is accumulated unaligned (never
  // worse than the legacy, alignment-free merge).
  double H_final[9];
  dt_hdr_align_status_t status;
  if(feature_ok && _warp_is_sane(H_feature, pw, ph))
  {
    memcpy(H_final, H_feature, sizeof(H_final));
    status = DT_HDR_ALIGN_OK;
  }
  else
  {
    _h_identity(H_final);
    status = DT_HDR_ALIGN_IDENTITY;
  }

  // Rescale proxy -> full-resolution coordinates.
  _h_scale_proxy_to_full(H_final, (double)width / pw, (double)height / ph);

  // Report the full-resolution corner motion vs. identity.
  double ident[9];
  _h_identity(ident);
  double dist_id[4];
  const double corner_motion = _corner_drift(ident, H_final, width, height, dist_id);

  if(info)
  {
    info->status = status;
    info->feature_inliers = feature_inliers;
    info->corner_drift = corner_motion;
  }

  // --- Structured log ------------------------------------------------------
  // Warps are reported in full-resolution coordinates so they line up with the
  // Python reference, which works at full image resolution.
  _log_feature_stats(&fstats);
  if(status != DT_HDR_ALIGN_IDENTITY)
    _log_warp("Final warp matrix (SIFT feature-init)", H_final);
  else
    dt_print(DT_DEBUG_HDR_MERGE,
             "  feature init unreliable (%d inliers): frame left unaligned",
             feature_inliers);

  if(status == DT_HDR_ALIGN_IDENTITY || corner_motion < DT_HDR_NOOP_MAX_CORNER_PX)
  {
    // No reliable warp, or motion below the resample threshold: the caller's
    // original `mosaic` is already the correct data to accumulate (an identity /
    // sub-pixel warp we deliberately do not resample -- see DT_HDR_NOOP_MAX_...).
    // Return FALSE so the caller uses its own source buffer and skip the
    // full-frame copy into `out` (a static frame is otherwise a pure ~w*h*4-byte
    // memcpy per frame).  `info->status` still reports the OK/IDENTITY decision.
    return FALSE;
  }

  _warp_mosaic_cfa(mosaic, out, width, height, filters, xtrans, H_final);
  return TRUE;
#endif // HAVE_OPENCV
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
