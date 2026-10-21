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

// Does the probe image actually probe anything?
//
// The verifier's whole claim -- "these harvested masks render identically
// before and after migration" -- is worth exactly nothing if the masks render
// to all-zero on the probe, because all-zero equals all-zero. The probe is the
// single point where that claim can quietly become vacuous, so its adequacy is
// measured here rather than assumed.
//
// What "adequate" means is not read off anyone's library. Profiling real edits
// to find which channels people select on would produce a bar shaped by one
// user's habits, and any user's habits can be arbitrarily unlike the next
// one's; ranges nobody in the sample happened to use would look like ranges
// nobody needs, and would then go unverified for everyone. So the bar comes
// from the colour space instead, computed here: every channel blendif offers,
// across every value that channel can physically take.
//
// Three properties are checked, and all three are measurements:
//
//   1. Global coverage -- every channel is occupied across everything the RGB
//      cube can reach, with no interior gap.
//   2. Local coverage -- the same, restricted to windows the size of a
//      plausible drawn mask, at many positions, and taken over the diffuse
//      range rather than the full scene range (see _diffuse_bin for why those
//      two must not share a standard). A drawn mask sees only what is under
//      it, so global coverage alone would let an ellipse land on a flat patch.
//   3. Structure -- hard edges and multi-scale texture are present, because
//      guided-filter feathering and detail masks read the guide image and
//      degenerate towards no-ops on a smooth one.
//
// The colour maths here is darktable's own (the same inline conversions
// blendif calls), not a reimplementation. The work profile is built locally
// from dt_Rec709_to_XYZ_D50 by transforming basis vectors, which avoids both
// hardcoding a matrix and requiring a full dt_init(); coverage is a question
// about spread, and does not turn on which of the plausible work profiles is
// used.

#include "common/colorspaces_inline_conversions.h"
#include "common/darktable.h"
#include "common/iop_profile.h"
#include "develop/blend.h"
#include "develop/masks/probe_image.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <cmocka.h>

#include <math.h>

// An ordinary photographic aspect and a size small enough that the suite stays
// fast. Coverage is a per-pixel statistic, so it does not improve with
// resolution; the small-size case, where the tiling is most degenerate, is
// checked separately.
#define PW 900
#define PH 600

#define NBINS 32

// ---------------------------------------------------------------------------
// channels
// ---------------------------------------------------------------------------

typedef enum
{
  CH_SCENE_GRAY = 0,
  CH_SCENE_R,
  CH_SCENE_G,
  CH_SCENE_B,
  CH_SCENE_JZ,
  CH_SCENE_CZ,
  CH_SCENE_HZ,
  CH_LAB_L,
  CH_LAB_H,
  CH_DISPLAY_H,
  CH_COUNT
} probe_channel_t;

static const char *const _ch_name[CH_COUNT] = {
  "RGB_SCENE gray_in", "RGB_SCENE R_in",  "RGB_SCENE G_in", "RGB_SCENE B_in",
  "RGB_SCENE Jz_in",   "RGB_SCENE Cz_in", "RGB_SCENE hz_in",
  "LAB L_in",          "LAB h_in",        "RGB_DISPLAY H_in"
};

// The domain each channel's histogram is taken over.
//
// These are not constants, and deliberately so. The obvious approach -- write
// [0,1] for each channel -- is wrong for Jz and Cz, which are physical
// quantities the slider addresses only through a per-channel boost factor
// (their descriptors in blend_gui.c carry boost_factor_offset =
// -6.64385619 = log2(1/100), so at the default boost a slider at 1.0 means
// Jz = 0.01). Asserting coverage of Jz over [0,1] would be asserting something
// false about the colour space rather than anything about the probe: no image
// reaches Jz = 1.
//
// The next-most-obvious approach -- take the bounds from the boost slider's
// range -- is no better. That slider spans 0..18 EV, which puts the largest
// addressable Jz around 2600. Nothing can cover that, and a mask selecting
// there is vacuous on every image in existence, not just on the probe.
//
// And bounds measured off some particular user's library would only encode
// that user's habits; the next user's may be nothing like them.
//
// So the domains are derived, at setup, from the colour space itself: sweep
// the linear-RGB cube over the scene-referred range and record what each
// channel can actually take. That yields, per channel, the set of values a
// real image *can* produce -- which is precisely the standard the probe should
// be held to, and it comes from darktable's own colour maths rather than from
// anyone's data. Where the achievable set is sparse or narrow, the reference
// is sparse or narrow in the same way, so the comparison stays fair.
static float _ch_lo[CH_COUNT];
static float _ch_hi[CH_COUNT];

// Which bins of each channel the RGB cube can reach at all. The probe is
// required to cover these, and is not penalised for missing values no image
// could produce either.
static int _reference_bin[CH_COUNT][NBINS];

// The same, restricted to the diffuse cube -- linear RGB in [0,1]^3, i.e. up
// to diffuse white and no further.
//
// This exists because the two coverage properties are not the same property
// and must not be held to the same standard. Globally, the probe should span
// everything the colour space can express, highlights included. Locally it
// should not: requiring a patch a sixty-fourth of the image across to span
// four stops of dynamic range uniformly would be requiring something no
// photograph does either, and the probe could only satisfy it by becoming
// unphysical.
//
// So the local test is taken over the diffuse range instead. That bound is a
// colour-science constant -- diffuse white is 1.0 in a scene-referred space --
// rather than a number tuned until the test passed, and it is the range any
// real image spans locally.
static int _diffuse_bin[CH_COUNT][NBINS];

// Scene-referred headroom for that sweep, in linear RGB. 4.0 is +2 EV above
// diffuse white: a deliberate statement about how much highlight range the
// probe should be answerable for, not a measurement of anything. Raising it
// widens the domains and makes the coverage tests strictly harder.
#define MAX_SCENE 4.0f

static dt_iop_order_iccprofile_info_t _profile;

/** Build a work profile whose matrix_in is RGB->XYZ D50 and whose
    matrix_out(_transposed) is RGB->XYZ D65 -- the same split
    dt_develop_blendif_init_masking_profile() produces at runtime. The matrix
    is derived by pushing basis vectors through darktable's own
    dt_Rec709_to_XYZ_D50, so there is no second copy of it to drift. */
static void _profile_init(void)
{
  dt_ioppr_init_profile_info(&_profile, 0);
  _profile.nonlinearlut = 0;

  // Bradford D50 -> D65, as used by dt_develop_blendif_init_masking_profile
  const dt_colormatrix_t M = { {  0.9555766f, -0.0230393f,  0.0631636f, 0.0f },
                               { -0.0282895f,  1.0099416f,  0.0210077f, 0.0f },
                               {  0.0122982f, -0.0204830f,  1.3299098f, 0.0f } };

  for(int c = 0; c < 3; c++)
  {
    dt_aligned_pixel_t basis = { 0.0f, 0.0f, 0.0f, 0.0f };
    dt_aligned_pixel_t xyz_d50 = { 0.0f, 0.0f, 0.0f, 0.0f };
    basis[c] = 1.0f;
    dt_Rec709_to_XYZ_D50(basis, xyz_d50);
    for(int r = 0; r < 3; r++) _profile.matrix_in[r][c] = xyz_d50[r];
  }

  for(int r = 0; r < 3; r++)
    for(int c = 0; c < 3; c++)
    {
      float sum = 0.0f;
      for(int i = 0; i < 3; i++) sum += M[r][i] * _profile.matrix_in[i][c];
      _profile.matrix_out[r][c] = sum;
      _profile.matrix_out_transposed[c][r] = sum;
    }

  for(int r = 0; r < 3; r++)
    for(int c = 0; c < 3; c++) _profile.matrix_in_transposed[c][r] = _profile.matrix_in[r][c];
}

/** All measured channel values for one linear-RGB pixel. */
static void _channels(const float *const restrict px, float out[CH_COUNT])
{
  dt_aligned_pixel_t rgb = { px[0], px[1], px[2], 0.0f };

  out[CH_SCENE_R] = rgb[0];
  out[CH_SCENE_G] = rgb[1];
  out[CH_SCENE_B] = rgb[2];

  out[CH_SCENE_GRAY] = dt_ioppr_get_rgb_matrix_luminance(rgb, _profile.matrix_in, _profile.lut_in,
                                                         _profile.unbounded_coeffs_in,
                                                         _profile.lutsize, _profile.nonlinearlut);

  dt_aligned_pixel_t xyz_d65 = { 0.0f, 0.0f, 0.0f, 0.0f };
  for(int r = 0; r < 3; r++)
    xyz_d65[r] = _profile.matrix_out[r][0] * rgb[0]
               + _profile.matrix_out[r][1] * rgb[1]
               + _profile.matrix_out[r][2] * rgb[2];

  dt_aligned_pixel_t jzazbz = { 0.0f, 0.0f, 0.0f, 0.0f };
  dt_aligned_pixel_t jzczhz = { 0.0f, 0.0f, 0.0f, 0.0f };
  dt_XYZ_2_JzAzBz(xyz_d65, jzazbz);
  dt_JzAzBz_2_JzCzhz(jzazbz, jzczhz);
  out[CH_SCENE_JZ] = jzczhz[0];
  out[CH_SCENE_CZ] = jzczhz[1];
  out[CH_SCENE_HZ] = jzczhz[2];

  dt_aligned_pixel_t xyz_d50 = { 0.0f, 0.0f, 0.0f, 0.0f };
  for(int r = 0; r < 3; r++)
    xyz_d50[r] = _profile.matrix_in[r][0] * rgb[0]
               + _profile.matrix_in[r][1] * rgb[1]
               + _profile.matrix_in[r][2] * rgb[2];

  dt_aligned_pixel_t lab = { 0.0f, 0.0f, 0.0f, 0.0f };
  dt_XYZ_to_Lab(xyz_d50, lab);
  out[CH_LAB_L] = lab[0] / 100.0f;
  // LAB hue, normalised to [0,1) the same way JzCzhz does it
  {
    float h = atan2f(lab[2], lab[1]) / DT_2PI_F;
    out[CH_LAB_H] = h >= 0.0f ? h : 1.0f + h;
  }

  dt_aligned_pixel_t hsl = { 0.0f, 0.0f, 0.0f, 0.0f };
  dt_RGB_2_HSL(rgb, hsl);
  out[CH_DISPLAY_H] = hsl[0];
}

// ---------------------------------------------------------------------------
// histogram helpers
// ---------------------------------------------------------------------------

/** Accumulate `buf` over the rectangle (x0,y0)-(x1,y1) into per-channel
    histograms. Values outside the channel's domain are counted into the end
    bins: a probe that reaches past the domain is fine (and intended, for the
    scene-referred highlights), it is interior gaps that matter. */
static void _histogram(const float *const buf,
                       const int width,
                       const int x0, const int y0,
                       const int x1, const int y1,
                       int hist[CH_COUNT][NBINS])
{
  memset(hist, 0, sizeof(int) * CH_COUNT * NBINS);

  for(int y = y0; y < y1; y++)
    for(int x = x0; x < x1; x++)
    {
      float v[CH_COUNT];
      _channels(buf + ((size_t)y * width + x) * 4, v);

      for(int c = 0; c < CH_COUNT; c++)
      {
        const float t = (v[c] - _ch_lo[c]) / (_ch_hi[c] - _ch_lo[c]);
        int b = (int)(t * NBINS);
        if(b < 0) b = 0;
        if(b >= NBINS) b = NBINS - 1;
        hist[c][b]++;
      }
    }
}

static int _cmp_double(const void *a, const void *b)
{
  const double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

/** number of bins holding at least `min_count` samples */
static int _occupied(const int *const h, const int min_count)
{
  int n = 0;
  for(int b = 0; b < NBINS; b++) if(h[b] >= min_count) n++;
  return n;
}

// ---------------------------------------------------------------------------
// tests
// ---------------------------------------------------------------------------

static float *_probe = NULL;

/** Sweep the linear-RGB cube over [0, MAX_SCENE]^3 and record, per channel,
    both the range and the individual histogram bins it can reach. This is the
    reference the probe is measured against: it is what the colour space
    permits, computed here rather than assumed or taken from a user's edits. */
static void _derive_domains(void)
{
  const int N = 48; // 48^3 = 110k samples; plenty to fill 32 bins

  for(int c = 0; c < CH_COUNT; c++)
  {
    _ch_lo[c] = INFINITY;
    _ch_hi[c] = -INFINITY;
  }

  // first pass: the range
  for(int i = 0; i < N; i++)
    for(int j = 0; j < N; j++)
      for(int k = 0; k < N; k++)
      {
        const float px[4] = { MAX_SCENE * i / (N - 1.0f),
                              MAX_SCENE * j / (N - 1.0f),
                              MAX_SCENE * k / (N - 1.0f), 0.0f };
        float v[CH_COUNT];
        _channels(px, v);
        for(int c = 0; c < CH_COUNT; c++)
        {
          if(v[c] < _ch_lo[c]) _ch_lo[c] = v[c];
          if(v[c] > _ch_hi[c]) _ch_hi[c] = v[c];
        }
      }

  for(int c = 0; c < CH_COUNT; c++)
    if(!(_ch_hi[c] > _ch_lo[c])) _ch_hi[c] = _ch_lo[c] + 1.0f; // degenerate guard

  // second pass: which bins of that range are actually reachable, both over
  // the full scene range and over the diffuse cube alone
  memset(_reference_bin, 0, sizeof(_reference_bin));
  memset(_diffuse_bin, 0, sizeof(_diffuse_bin));
  for(int i = 0; i < N; i++)
    for(int j = 0; j < N; j++)
      for(int k = 0; k < N; k++)
        for(int diffuse = 0; diffuse < 2; diffuse++)
        {
          const float top = diffuse ? 1.0f : MAX_SCENE;
          const float px[4] = { top * i / (N - 1.0f),
                                top * j / (N - 1.0f),
                                top * k / (N - 1.0f), 0.0f };
          float v[CH_COUNT];
          _channels(px, v);
          for(int c = 0; c < CH_COUNT; c++)
          {
            int b = (int)((v[c] - _ch_lo[c]) / (_ch_hi[c] - _ch_lo[c]) * NBINS);
            if(b < 0) b = 0;
            if(b >= NBINS) b = NBINS - 1;
            if(diffuse) _diffuse_bin[c][b]++;
            else _reference_bin[c][b]++;
          }
        }
}

/** number of bins the RGB cube can reach for this channel */
static int _reference_reachable(const int c)
{
  int n = 0;
  for(int b = 0; b < NBINS; b++) if(_reference_bin[c][b] > 0) n++;
  return n;
}

/** the same, over the diffuse cube only */
static int _diffuse_reachable(const int c)
{
  int n = 0;
  for(int b = 0; b < NBINS; b++) if(_diffuse_bin[c][b] > 0) n++;
  return n;
}

static int _setup(void **state)
{
  (void)state;
  _profile_init();
  _derive_domains();

  for(int c = 0; c < CH_COUNT; c++)
    print_message("domain %-20s [%.5f .. %.5f], %d/%d bins reachable\n",
                  _ch_name[c], _ch_lo[c], _ch_hi[c], _reference_reachable(c), NBINS);

  _probe = dt_masks_probe_new(PW, PH);
  assert_non_null(_probe);
  return 0;
}

static int _teardown(void **state)
{
  (void)state;
  dt_free_align(_probe);
  _probe = NULL;
  dt_ioppr_cleanup_profile_info(&_profile);
  return 0;
}

/** Every channel is occupied across its whole domain, with no interior gap.
    This is the headline property: a gap is a band of values no harvested mask
    could ever select on, i.e. a blind spot in the verification. */
static void test_every_channel_is_fully_covered(void **state)
{
  (void)state;
  static int hist[CH_COUNT][NBINS];
  _histogram(_probe, PW, 0, 0, PW, PH, hist);

  // 0.02% of pixels -- enough that a bin is genuinely populated rather than
  // holding a handful of outliers, but far below a uniform share (1/32 = 3%),
  // so a channel is not required to be *flat*, only gap-free.
  const int min_count = (PW * PH) / 5000;

  for(int c = 0; c < CH_COUNT; c++)
  {
    // Only bins the RGB cube can actually reach are required. A bin the
    // colour space cannot produce is not a hole in the probe, and demanding
    // it would make the test unsatisfiable rather than strict.
    int missing = 0;
    char gaps[512] = { 0 };
    size_t used = 0;
    for(int b = 0; b < NBINS; b++)
    {
      if(_reference_bin[c][b] == 0) continue;
      if(hist[c][b] >= min_count) continue;
      missing++;
      if(used < sizeof(gaps) - 32)
        used += (size_t)snprintf(gaps + used, sizeof(gaps) - used, "[%.5f..%.5f) ",
                                 _ch_lo[c] + (_ch_hi[c] - _ch_lo[c]) * b / NBINS,
                                 _ch_lo[c] + (_ch_hi[c] - _ch_lo[c]) * (b + 1) / NBINS);
    }
    if(missing)
      print_error("channel %s: %d of %d reachable bins uncovered: %s\n",
                  _ch_name[c], missing, _reference_reachable(c), gaps);
    assert_int_equal(missing, 0);
  }
}

/** The same, under windows the size of a plausible drawn mask, at many
    positions. Global coverage says nothing about what sits under any one
    ellipse; this is the property that stops a harvested drawn mask landing on
    a flat patch and passing vacuously. */
static void test_local_windows_are_well_covered(void **state)
{
  (void)state;
  static int hist[CH_COUNT][NBINS];

  // ~1/8 of each axis: a mask smaller than this covers little enough of the
  // image that a real user would be editing a detail, not a region.
  const int ww = PW / 8, wh = PH / 8;
  const int min_count = 1; // a window is small; presence is the question

  // Deliberately not aligned to the tile grid, and stepped by a stride
  // coprime-ish with it, so windows do not all sample the tiling in phase.
  // measured as a fraction of what the colour space allows, per channel
  double worst_frac = 2.0;
  int worst_occ = 0, worst_ref = 0, worst_ch = -1, worst_x = 0, worst_y = 0;

  for(int y0 = 0; y0 + wh <= PH; y0 += wh / 2 + 7)
    for(int x0 = 0; x0 + ww <= PW; x0 += ww / 2 + 11)
    {
      _histogram(_probe, PW, x0, y0, x0 + ww, y0 + wh, hist);
      for(int c = 0; c < CH_COUNT; c++)
      {
        const int ref = _diffuse_reachable(c);
        if(ref == 0) continue;
        // count only bins the diffuse cube can reach: highlight bins are the
        // global test's business, not this one's
        int occ = 0;
        for(int b = 0; b < NBINS; b++)
          if(_diffuse_bin[c][b] > 0 && hist[c][b] >= min_count) occ++;
        const double frac = (double)occ / (double)ref;
        if(frac < worst_frac)
        {
          worst_frac = frac;
          worst_occ = occ;
          worst_ref = ref;
          worst_ch = c;
          worst_x = x0;
          worst_y = y0;
        }
      }
    }

  // 70%: a window covering 1/64 of the image cannot be expected to span
  // everything the whole colour space allows, but it must be far from flat.
  if(worst_frac < 0.70)
    print_error("worst local window at (%d,%d): channel %s covers %d of %d reachable bins\n",
                worst_x, worst_y, _ch_name[worst_ch], worst_occ, worst_ref);
  assert_true(worst_frac >= 0.70);
}

/** Hard edges exist, in quantity, and at every orientation.

    Guided-filter feathering is an edge-aware operator: on a smooth image it
    degenerates towards a plain blur and stops discriminating, so a mask using
    it would be compared before and after migration and prove nothing either
    way.

    Counting strong gradients alone is not enough to establish that, and this
    test originally did only that -- until a mutation that stripped every last
    piece of texture and every irregular edge out of the generator failed to
    make it fail. The reason is the tile lattice: it alone puts a sharp step
    every 16 pixels, so the count stays high on a probe whose tile interiors
    are perfectly flat. A test that a deliberately gutted probe still passes is
    not measuring the property it claims to.

    What distinguishes the lattice from photographic structure is that it is
    strictly axis-aligned. So the assertion is on the *distribution* of edge
    orientations: a pure grid occupies two orientation bins out of sixteen,
    while real structure occupies all of them. That is a property the lattice
    cannot fake. */
static void test_probe_carries_hard_edges(void **state)
{
  (void)state;
#define NORIENT 16
  size_t strong = 0, total = 0;
  size_t orient[NORIENT] = { 0 };

  for(int y = 1; y < PH - 1; y++)
    for(int x = 1; x < PW - 1; x++)
    {
      float c[CH_COUNT], r[CH_COUNT], d[CH_COUNT];
      _channels(_probe + ((size_t)y * PW + x) * 4, c);
      _channels(_probe + ((size_t)y * PW + x + 1) * 4, r);
      _channels(_probe + ((size_t)(y + 1) * PW + x) * 4, d);

      const float gx = r[CH_SCENE_GRAY] - c[CH_SCENE_GRAY];
      const float gy = d[CH_SCENE_GRAY] - c[CH_SCENE_GRAY];
      const float grad = fmaxf(fabsf(gx), fabsf(gy));
      total++;

      // a step of 0.1 in luminance between adjacent pixels is an edge no
      // smooth gradient produces at this resolution
      if(grad > 0.1f)
      {
        strong++;
        // orientation modulo pi: an edge and its reverse are the same edge
        float a = atan2f(gy, gx);
        if(a < 0.0f) a += M_PI_F;
        int b = (int)(a / M_PI_F * NORIENT);
        if(b < 0) b = 0;
        if(b >= NORIENT) b = NORIENT - 1;
        orient[b]++;
      }
    }

  const double frac = (double)strong / (double)total;
  print_message("hard-edge pixels: %.3f%%\n", 100.0 * frac);
  assert_true(frac > 0.005);

  // Every orientation must carry edges, measured as a fraction of all pixels
  // rather than as a share of the edge population.
  //
  // Sharing it out was the first attempt and it is the wrong metric: the
  // denominator is dominated by the axis-aligned lattice, so every off-axis
  // edge added to the probe also raises the bar those edges are judged
  // against. Improving the probe could not move it, and it would in fact
  // penalise a probe for having *more* structure.
  //
  // The property actually needed is weaker and better posed: guided filtering
  // has to encounter edges at every orientation, not equally many of each. So
  // the bar is absolute. 0.05% of pixels per orientation is an order of
  // magnitude above what a purely axis-aligned probe produces -- which is
  // zero -- while staying well clear of the natural anisotropy of a pixel
  // grid, where a 30-degree edge is inherently rarer than a vertical one.
  const double floor_frac = 0.0005;
  for(int b = 0; b < NORIENT; b++)
  {
    const double of_pixels = (double)orient[b] / (double)total;
    if(of_pixels < floor_frac)
      print_error("edge orientation bin %d/%d (%.0f-%.0f deg): only %.4f%% of pixels"
                  " -- structure is too close to axis-aligned\n",
                  b, NORIENT, 180.0 * b / NORIENT, 180.0 * (b + 1) / NORIENT,
                  100.0 * of_pixels);
    assert_true(of_pixels >= floor_frac);
  }
#undef NORIENT
}

/** Texture exists at every scale the detail mask looks at.

    The detail mask is a wavelet decomposition, so it responds per band. Noise
    at a single frequency would light up one band and leave the rest flat,
    making any mask that refines on the other bands vacuous in exactly the
    bands it uses.

    Getting this measurement right took three attempts, and the two failures
    are worth recording because both *passed* on a probe with every last bit of
    noise removed:

      - Total energy per octave. Defeated by the tile lattice: a periodic
        square lattice has harmonics in every band, so the numbers stayed high
        with the tile interiors perfectly flat.

      - Energy restricted to edge-free quads. Defeated by circularity. The
        noise is itself what makes a quad non-flat, so selecting the flat
        quads selected precisely against the signal being looked for; the
        statistic came out identical to five decimal places with the noise
        switched off.

    What works is a robust statistic over *every* quad, with no selection at
    all. The measure is the diagonal (HH) wavelet coefficient, which is
    identically zero for any function linear in x and y -- so the tile's own
    red/green sweep, a pure ramp, contributes nothing however steep it is. Hard
    steps do produce large coefficients, but they are sparse, so they move the
    upper percentiles and not the median. What is left in the median is
    texture, and nothing else. */
static void test_probe_has_multiscale_texture(void **state)
{
  (void)state;

  int w = PW, h = PH;
  float *cur = malloc(sizeof(float) * (size_t)w * h);
  assert_non_null(cur);
  for(int y = 0; y < h; y++)
    for(int x = 0; x < w; x++)
    {
      float v[CH_COUNT];
      _channels(_probe + ((size_t)y * PW + x) * 4, v);
      cur[(size_t)y * w + x] = v[CH_SCENE_GRAY];
    }

  // Four octaves, not more, and the bound is structural rather than chosen:
  // the tile lattice sits at _tile_size() pixels (16 at this resolution), so
  // by the fifth halving the analysis window is the lattice period itself and
  // there is no sub-lattice scale left in which texture means anything.
  for(int octave = 0; octave < 4; octave++)
  {
    const int nw = w / 2, nh = h / 2;
    assert_true(nw > 0 && nh > 0);
    float *next = malloc(sizeof(float) * (size_t)nw * nh);
    double *hh2 = malloc(sizeof(double) * (size_t)nw * nh);
    assert_non_null(next);
    assert_non_null(hh2);

    for(int y = 0; y < nh; y++)
      for(int x = 0; x < nw; x++)
      {
        const float a = cur[(size_t)(2 * y) * w + 2 * x];
        const float b = cur[(size_t)(2 * y) * w + 2 * x + 1];
        const float c = cur[(size_t)(2 * y + 1) * w + 2 * x];
        const float d = cur[(size_t)(2 * y + 1) * w + 2 * x + 1];
        next[(size_t)y * nw + x] = 0.25f * (a + b + c + d);

        const double hh = (double)(a - b - c + d);
        hh2[(size_t)y * nw + x] = hh * hh;
      }

    const size_t nq = (size_t)nw * nh;
    qsort(hh2, nq, sizeof(double), _cmp_double);
    const double median = hh2[nq / 2];

    print_message("octave %d (%dx%d): median HH^2 %.3e (p90 %.3e)\n",
                  octave, w, h, median, hh2[(nq * 9) / 10]);

    // A ramp gives exactly zero and sparse steps cannot lift a median, so
    // anything appreciably above zero here is texture. The bar is set well
    // below what the generator produces and well above what a noiseless probe
    // does -- with the noise switched off this statistic collapses by orders
    // of magnitude, which is what makes it a real assertion rather than a
    // number that happens to be true.
    assert_true(median > 1e-8);

    free(cur);
    free(hh2);
    cur = next;
    w = nw;
    h = nh;
  }
  free(cur);
}

/** Same size in, same bytes out -- on any machine, any run.

    A harvested file is collected on a user's machine and replayed on ours, so
    the probe must not depend on anything local. If this ever fails, a
    verification result stops being reproducible and a reported difference
    could not be investigated. */
static void test_generation_is_deterministic(void **state)
{
  (void)state;
  float *a = dt_masks_probe_new(320, 240);
  float *b = dt_masks_probe_new(320, 240);
  assert_non_null(a);
  assert_non_null(b);
  assert_memory_equal(a, b, sizeof(float) * 320 * 240 * 4);
  dt_free_align(a);
  dt_free_align(b);
}

/** The probe reaches above 1.0.

    The working space is scene-referred: blendif boost factors let a slider
    address values well past 1.0, and a probe clipped at 1.0 would make every
    such selection vacuous. Also checks it never goes negative, which would be
    outside anything the pipe produces. */
static void test_probe_is_scene_referred(void **state)
{
  (void)state;
  size_t above = 0;
  float maxv = 0.0f, minv = 1e9f;

  for(size_t i = 0; i < (size_t)PW * PH; i++)
    for(int c = 0; c < 3; c++)
    {
      const float v = _probe[i * 4 + c];
      if(v > 1.0f) above++;
      maxv = fmaxf(maxv, v);
      minv = fminf(minv, v);
    }

  print_message("probe range [%.3f .. %.3f], %.2f%% of samples above 1.0\n",
                minv, maxv, 100.0 * (double)above / ((double)PW * PH * 3));
  assert_true(minv >= 0.0f);
  assert_true(maxv > 1.5f);
  assert_true(above > ((size_t)PW * PH * 3) / 100); // at least 1%
}

/** Coverage does not depend on the probe being large.

    A mask is replayed at whatever size it was harvested against, so small
    sizes have to work as well as large ones. This re-runs the headline
    coverage check where the tiling is at its most degenerate. */
static void test_small_probe_is_still_covered(void **state)
{
  (void)state;
  const int sw = 240, sh = 160;
  float *small = dt_masks_probe_new(sw, sh);
  assert_non_null(small);

  static int hist[CH_COUNT][NBINS];
  _histogram(small, sw, 0, 0, sw, sh, hist);

  for(int c = 0; c < CH_COUNT; c++)
  {
    const int ref = _reference_reachable(c);
    if(ref == 0) continue;
    const int occ = _occupied(hist[c], 1);
    if(occ * 10 < ref * 9)
      print_error("small probe, channel %s: %d of %d reachable bins\n",
                  _ch_name[c], occ, ref);
    assert_true(occ * 10 >= ref * 9);
  }
  dt_free_align(small);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_every_channel_is_fully_covered),
    cmocka_unit_test(test_local_windows_are_well_covered),
    cmocka_unit_test(test_probe_carries_hard_edges),
    cmocka_unit_test(test_probe_has_multiscale_texture),
    cmocka_unit_test(test_generation_is_deterministic),
    cmocka_unit_test(test_probe_is_scene_referred),
    cmocka_unit_test(test_small_probe_is_still_covered),
  };
  return cmocka_run_group_tests(tests, _setup, _teardown);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
