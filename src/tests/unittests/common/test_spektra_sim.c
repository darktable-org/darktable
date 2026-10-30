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
/*
 * cmocka unit tests for the spektrafilm simulation engine, common/spektra_sim.c.
 *
 * These are ports of the unit tests the upstream python implementation runs
 * against the same algorithms (its tests/test_couplers.py,
 * test_morph_curves.py, test_parametric.py and test_gamut_compression.py).
 * Each test below names the upstream test it comes from, so that when the two
 * drift apart it is clear which side moved.
 *
 * What they cover is the pure math, plus the per-pixel entry points fed from a
 * hand-built sf_sim_t. Not covered is anything that needs a loaded profile --
 * the spectral upsampling tables, the enlarger's neutral filters, the grain
 * sublayer build. A test built on those reads real profile data off disk, which
 * makes it an integration test with a data dependency and not a unit test.
 * Those live next door, in test_spektra_pack.c, which reads a pack from the
 * directory it is configured with and skips itself when it is given none.
 *
 * Including the .c, not the .h is what the module tests already do (see
 * ../iop/test_filmicrgb.c): most of the algorithms are file-static helpers, and
 * they are the interesting part.
 *
 * Please see ../README.md for more detailed documentation.
 */
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>

#include <cmocka.h>

#include "../util/assert.h"
#include "../util/tracing.h"

#include "common/spektra_sim.c"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

/*
 * DEFINITIONS
 */

/* Comparison epsilon. The engine works in double, and every invariant tested
   here is exact in exact arithmetic, so this only has to absorb rounding.
   The output gamut compressors are the exception -- they run in float to
   match spektrafilm.cl -- but every value they are asserted against here
   (0.0, 0.5, 1.5, 2.0) is exactly representable and comes back through an
   untouched achromatic max, so E holds for those too. */
#define E 1e-9

/* Compare in double, whatever cmocka happens to be installed. cmocka's
   assert_float_equal() casts both operands and the epsilon to float, where an
   epsilon of 1e-9 sits below the resolution of values around 1, and its
   assert_double_equal() is missing from 1.1.0, the minimum the build accepts.
   cmocka defines no assert_double_close(), so this one is always in effect. */
#ifndef assert_double_close
#define assert_double_close(a, b, epsilon) \
  assert_true(fabs((double)(a) - (double)(b)) <= (double)(epsilon))
#endif

/* The knee parameters upstream's own gamut-compression tests use. They are not
   the ones spektrafilm ships (SF_TC_KNEE_* / SF_OUT_KNEE_* have threshold 0),
   which is precisely why the knee is tested through its parameters: the
   threshold behaviour has to hold for any threshold, not just the shipped one. */
#define KNEE_T 0.815
#define KNEE_L 1.0
#define KNEE_P 1.2

/* A square standing in for the spectral locus. The real locus needs the pack's
   colour matching functions; every property tested through it here is a
   property of the radial compressor, not of the polygon, so a closed convex
   polygon with white inside it is enough -- and it keeps the expected values
   hand-checkable. Closed: last vertex repeats the first. */
static const double LOCUS_SQUARE[5][2] = { { 0.0, 0.0 },
                                           { 1.0, 0.0 },
                                           { 1.0, 1.0 },
                                           { 0.0, 1.0 },
                                           { 0.0, 0.0 } };
static const double WHITE_E[2] = { 1.0 / 3.0, 1.0 / 3.0 };

/* One channel of a three-sublayer emulsion fit, taken from the first column of
   the model upstream's test_morph_curves.py builds. */
static const double CENTERS[3] = { -1.2, -1.1, -1.0 };
static const double AMPS[3] = { 0.25, 0.22, 0.20 };
static const double SIGMAS[3] = { 0.30, 0.28, 0.26 };

/* Log-exposure grid for the hand-built sims below: SF_NLE points over
   [-3, 1], the range the profiles themselves are fitted on. */
#define LE0 (-3.0)
#define LE_SPAN 4.0
#define LE_STEP (LE_SPAN / (double)(SF_NLE - 1))

/*
 * HELPERS
 */

/* Distance from white in the chromaticity plane. */
static double _dist_from_white(const double xy[2])
{
  return hypot(xy[0] - WHITE_E[0], xy[1] - WHITE_E[1]);
}

/* A sim carrying nothing but what the film-develop entry points read: a linear
   density ramp from 0 at log-exposure LE0 to dmax at LE0 + LE_SPAN, negative
   film, linear (non-Langmuir) couplers. Linear so that the expected
   density at any exposure is a closed form the test can state on its own, without borrowing the
   interpolator it is checking. Caller frees. */
static sf_sim_t *_sim_linear_ramp(const double dmax[3])
{
  sf_sim_t *sim = calloc(1, sizeof(sf_sim_t));
  assert_non_null(sim);
  sim->le0 = LE0;
  sim->le_step = LE_STEP;
  sim->inv_le_step = (float)(1.0 / LE_STEP);
  for(int c = 0; c < 3; c++)
  {
    sim->film_dmax[c] = dmax[c];
    for(int i = 0; i < SF_NLE; i++)
    {
      const double d = dmax[c] * (double)i / (double)(SF_NLE - 1);
      sim->curves_norm[i][c] = d;
      sim->curves_norm_f[i][c] = (float)d;
    }
  }
  sim->film_positive = 0;
  sim->couplers_active = 1;
  sim->couplers_donor_lm = 0;
  sim->couplers_recv_lm = 0;
  return sim;
}

/* The density the ramp above holds at log-exposure x, stated independently of
   the interpolator under test. */
static double _ramp_density(double x,
                            double dmax)
{
  const double t = (x - LE0) / LE_SPAN;
  return dmax * (t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t));
}

/*
 * TEST FUNCTIONS: reinhard knee
 * (upstream tests/test_gamut_compression.py::TestReinhardKnee)
 */

static void test_knee_below_threshold_is_identity(void **state)
{
  TR_STEP("knee leaves distances below the threshold exactly alone");
  const double d[] = { 0.0, 0.2, 0.5, 0.8 };
  for(size_t i = 0; i < sizeof(d) / sizeof(d[0]); i++)
  {
    const double out = reinhard_knee(d[i], KNEE_T, KNEE_L, KNEE_P);
    TR_DEBUG("d=%e => %e", d[i], out);
    assert_double_close(out, d[i], E);
  }
}

static void test_knee_above_threshold_compresses(void **state)
{
  TR_STEP("knee compresses rather than stretches above the threshold");
  const double d[] = { 0.9, 1.5, 5.0, 100.0 };
  for(size_t i = 0; i < sizeof(d) / sizeof(d[0]); i++)
  {
    const double out = reinhard_knee(d[i], KNEE_T, KNEE_L, KNEE_P);
    TR_DEBUG("d=%e => %e", d[i], out);
    assert_true(out < d[i]);
  }
}

static void test_knee_asymptotes_at_limit(void **state)
{
  TR_STEP("knee approaches the limit as the distance grows without bound");
  const double out = reinhard_knee(1e9, KNEE_T, KNEE_L, KNEE_P);
  assert_double_close(out, KNEE_L, 1e-6);
}

static void test_knee_is_continuous_at_threshold(void **state)
{
  TR_STEP("knee has no step at the threshold");
  const double eps = 1e-9;
  const double below = reinhard_knee(KNEE_T - eps, KNEE_T, KNEE_L, KNEE_P);
  const double above = reinhard_knee(KNEE_T + eps, KNEE_T, KNEE_L, KNEE_P);
  assert_double_close(above, below, 1e-6);
}

static void test_knee_float_matches_double(void **state)
{
  /* reinhard_knee_f() is the knee the scan stage ships and spektrafilm.cl
     mirrors; the tests above exercise the double one. Both parameter sets:
     upstream's, and the shipped output knee with threshold 0. */
  TR_STEP("the float knee tracks the double one, for both parameter sets");
  const double params[2][3] = { { KNEE_T, KNEE_L, KNEE_P },
                                { SF_OUT_KNEE_T, SF_OUT_KNEE_L, SF_OUT_KNEE_P } };
  for(int k = 0; k < 2; k++)
  {
    float previous = -1.0f;
    for(int i = 0; i <= 400; i++)
    {
      const double d = 4.0 * (double)i / 400.0;
      const double ref = reinhard_knee(d, params[k][0], params[k][1], params[k][2]);
      const float out = reinhard_knee_f((float)d, (float)params[k][0], (float)params[k][1],
                                        (float)params[k][2]);
      assert_double_close(out, ref, 1e-6);
      assert_true(out >= previous); /* and stays monotonic in float */
      previous = out;
    }
  }
}

/*
 * TEST FUNCTIONS: output gamut compression, ACES RGC
 * (upstream tests/test_gamut_compression.py::TestCompressRgbAcesRgc)
 */

static void test_aces_neutral_is_unchanged(void **state)
{
  TR_STEP("achromatic pixels pass through the output compressor untouched");
  float rgb[3] = { 0.5f, 0.5f, 0.5f };
  compress_rgb_aces_f(rgb);
  for(int c = 0; c < 3; c++) assert_double_close(rgb[c], 0.5, E);
}

static void test_aces_black_is_identity(void **state)
{
  TR_STEP("pixels at or below black keep their values -- no chromaticity to compress");
  float rgb[3] = { 0.0f, 0.0f, 0.0f };
  compress_rgb_aces_f(rgb);
  for(int c = 0; c < 3; c++) assert_double_close(rgb[c], 0.0, E);
}

static void test_aces_leaves_the_max_channel_alone(void **state)
{
  TR_STEP("the achromatic max is the anchor and never moves");
  float rgb[3] = { 2.0f, -0.1f, 0.3f };
  compress_rgb_aces_f(rgb);
  assert_double_close(rgb[0], 2.0, E);
}

/* The shipped output knee, stated here on its own: threshold 0, limit 1,
   power 6. A channel at achromatic distance d = (max - v) / max comes back as
   max * (1 - d / (1 + d^6)^(1/6)). */
static double _aces_expected(double max,
                             double v)
{
  const double d = (max - v) / max;
  return max * (1.0 - d / pow(1.0 + pow(d, 6.0), 1.0 / 6.0));
}

static void test_aces_pulls_negatives_back_inside(void **state)
{
  TR_STEP("out-of-gamut negatives land where the knee puts them, inside the gamut");
  /* the closed form above is for these parameters; changing them is a change
     to the look, and this is where it has to be acknowledged */
  assert_double_close(SF_OUT_KNEE_T, 0.0, 0.0);
  assert_double_close(SF_OUT_KNEE_L, 1.0, 0.0);
  assert_double_close(SF_OUT_KNEE_P, 6.0, 0.0);

  float rgb[3] = { 1.5f, -0.1f, -0.05f };
  compress_rgb_aces_f(rgb);
  TR_DEBUG("out=(%e,%e,%e)", (double)rgb[0], (double)rgb[1], (double)rgb[2]);
  assert_double_close(rgb[0], 1.5, E);
  /* about 0.1241 and 0.1427: well inside, neither clipped to 0 nor left
     negative. Float arithmetic against the double closed form, hence 1e-6 */
  assert_double_close(rgb[1], _aces_expected(1.5, -0.1), 1e-6);
  assert_double_close(rgb[2], _aces_expected(1.5, -0.05), 1e-6);
  assert_true(rgb[1] > 0.1f && rgb[2] > 0.1f);
}

static void test_aces_compresses_stronger_excursions_harder(void **state)
{
  TR_STEP("the further out of gamut, the closer to the boundary the result lands");
  float mild[3] = { 1.0f, -0.05f, -0.05f };
  float hard[3] = { 1.0f, -1.0f, -1.0f };
  compress_rgb_aces_f(mild);
  compress_rgb_aces_f(hard);
  TR_DEBUG("mild=%e hard=%e", (double)mild[1], (double)hard[1]);
  assert_true(mild[1] >= 0.0f && mild[2] >= 0.0f);
  assert_true(hard[1] >= 0.0f && hard[2] >= 0.0f);
  assert_true(hard[1] < mild[1]);
  assert_true(hard[2] < mild[2]);
}

/*
 * TEST FUNCTIONS: input chromaticity compression
 * (upstream tests/test_gamut_compression.py::TestCompressXy)
 */

static void test_compress_xy_leaves_white_alone(void **state)
{
  TR_STEP("white has no direction to be pulled along and must not move");
  double out[2];
  compress_xy_radial(out, WHITE_E, WHITE_E, LOCUS_SQUARE, 5);
  assert_double_close(out[0], WHITE_E[0], E);
  assert_double_close(out[1], WHITE_E[1], E);
}

static void test_compress_xy_leaves_well_inside_alone(void **state)
{
  /* Upstream asserts exact identity here, which holds for its own threshold of
     0.815. spektrafilm ships SF_TC_KNEE_T = 0, so nothing is formally exempt
     and the assertion becomes the one that survives either threshold: deep
     inside the locus the knee is the identity to within rounding. */
  TR_STEP("chromaticities well inside the locus come back where they went in");
  const double xy[2] = { WHITE_E[0] + 0.02, WHITE_E[1] + 0.01 };
  double out[2];
  compress_xy_radial(out, xy, WHITE_E, LOCUS_SQUARE, 5);
  assert_double_close(out[0], xy[0], 1e-6);
  assert_double_close(out[1], xy[1], 1e-6);
}

static void test_compress_xy_pulls_oog_inside(void **state)
{
  TR_STEP("a chromaticity far outside the locus is pulled in, and stays in");
  const double xy[2] = { 1.4, -0.3 };
  double out[2];
  compress_xy_radial(out, xy, WHITE_E, LOCUS_SQUARE, 5);
  TR_DEBUG("in=(%e,%e) out=(%e,%e)", xy[0], xy[1], out[0], out[1]);
  assert_true(_dist_from_white(out) < _dist_from_white(xy));
  /* the limit is the boundary itself, so the result lands within the square */
  assert_true(out[0] >= 0.0 && out[0] <= 1.0);
  assert_true(out[1] >= 0.0 && out[1] <= 1.0);
}

static void test_compress_xy_preserves_direction(void **state)
{
  TR_STEP("compression is radial: only the distance from white changes");
  const double xy[2] = { 1.4, -0.3 };
  double out[2];
  compress_xy_radial(out, xy, WHITE_E, LOCUS_SQUARE, 5);
  const double in_dx = xy[0] - WHITE_E[0], in_dy = xy[1] - WHITE_E[1];
  const double out_dx = out[0] - WHITE_E[0], out_dy = out[1] - WHITE_E[1];
  /* cross product of the two offsets vanishes when they are collinear */
  assert_double_close(in_dx * out_dy - in_dy * out_dx, 0.0, E);
  /* and the pull is inward, never a reflection through white */
  assert_true(in_dx * out_dx + in_dy * out_dy > 0.0);
}

/*
 * TEST FUNCTIONS: density curve model
 * (upstream tests/test_parametric.py::TestParametricDensityCurvesModel)
 */

static void test_density_curve_is_monotonic(void **state)
{
  TR_STEP("a negative stock's density never falls as exposure rises");
  enum { N = 200 };
  double le[N], density[N];
  for(int i = 0; i < N; i++) le[i] = -3.0 + 5.0 * (double)i / (double)(N - 1);
  for(int sept = 0; sept < 2; sept++)
  {
    eval_cdfs_channel(density, le, N, CENTERS, AMPS, SIGMAS, NULL, sept, 3, 0);
    for(int i = 1; i < N; i++)
    {
      TR_DEBUG("le=%e d=%e", le[i], density[i]);
      assert_true(density[i] - density[i - 1] >= -E);
    }
  }
}

static void test_density_curve_is_near_zero_at_low_exposure(void **state)
{
  /* Measured from the curve, not from a fixed exposure far off the grid: four
     widths below the fastest layer's centre the Gaussian layers have all but
     vanished, and the septic ones, whose support ends SF_SEPT_K / 2 widths
     out, are exactly zero. */
  TR_STEP("density vanishes four layer widths below the toe");
  double le[1] = { CENTERS[0] - 4.0 * SIGMAS[0] };
  double density[1];
  eval_cdfs_channel(density, le, 1, CENTERS, AMPS, SIGMAS, NULL, 0, 3, 0);
  TR_DEBUG("gaussian: le=%e d=%e", le[0], density[0]);
  assert_true(density[0] >= 0.0 && density[0] < 1e-4);
  eval_cdfs_channel(density, le, 1, CENTERS, AMPS, SIGMAS, NULL, 1, 3, 0);
  TR_DEBUG("septic: le=%e d=%e", le[0], density[0]);
  assert_double_close(density[0], 0.0, 0.0);
}

static void test_skewed_density_curve_is_monotonic(void **state)
{
  TR_STEP("a skewed septic curve still never falls as exposure rises");
  enum { N = 400 };
  const double alphas[3] = { 0.6, -0.4, 1.0 };
  double le[N], density[N];
  for(int i = 0; i < N; i++) le[i] = -3.0 + 5.0 * (double)i / (double)(N - 1);
  eval_cdfs_channel(density, le, N, CENTERS, AMPS, SIGMAS, alphas, 1, 3, 0);
  for(int i = 1; i < N; i++) assert_true(density[i] - density[i - 1] >= -E);
}

static void test_skew_keeps_the_layer_median_on_its_centre(void **state)
{
  /* the warp vanishes at u = 0.5, so every layer still crosses half its
     amplitude exactly at its centre whatever the skew */
  TR_STEP("skew moves the tails, never the half-density point");
  const double alpha[] = { -1.0, -0.3, 0.0, 0.5, 1.0 };
  for(size_t i = 0; i < sizeof(alpha) / sizeof(alpha[0]); i++)
    assert_double_close(sept_cdf(0.0, alpha[i]), 0.5, E);
}

static void test_exhausted_density_curve_is_monotonic(void **state)
{
  /* the film-side curves are built from the Gumbel-blended layers, which
     eval_cdfs_channel() does not evaluate */
  TR_STEP("a curve blended toward the exhaustion shoulder stays monotonic");
  const double gmix[3] = { 0.35, 0.35, 0.35 };
  for(int positive = 0; positive < 2; positive++)
    for(int sept = 0; sept < 2; sept++)
    {
      double previous = _sf_channel_density_at(-3.0, CENTERS, AMPS, SIGMAS, NULL, sept, 3, gmix,
                                               positive);
      for(int i = 1; i < 400; i++)
      {
        const double x = -3.0 + 5.0 * (double)i / 399.0;
        const double d = _sf_channel_density_at(x, CENTERS, AMPS, SIGMAS, NULL, sept, 3, gmix,
                                                positive);
        if(positive)
          assert_true(d - previous <= E);
        else
          assert_true(d - previous >= -E);
        previous = d;
      }
    }
}

static void test_density_curve_is_inverted_for_positive_stock(void **state)
{
  TR_STEP("a positive stock's density falls where a negative's rises");
  enum { N = 64 };
  double le[N], neg[N], pos[N];
  for(int i = 0; i < N; i++) le[i] = -3.0 + 5.0 * (double)i / (double)(N - 1);
  eval_cdfs_channel(neg, le, N, CENTERS, AMPS, SIGMAS, NULL, 0, 3, 0);
  eval_cdfs_channel(pos, le, N, CENTERS, AMPS, SIGMAS, NULL, 0, 3, 1);
  const double total = AMPS[0] + AMPS[1] + AMPS[2];
  for(int i = 0; i < N; i++) assert_double_close(neg[i] + pos[i], total, E);
}

/*
 * TEST FUNCTIONS: developer exhaustion morph
 * (upstream tests/test_morph_curves.py)
 */

static void test_developer_exhaustion_preserves_midgray(void **state)
{
  /* Exhaustion blends the layer sigmoids toward a matched gumbel shoulder,
     which would drag the whole curve sideways; the solver's job is to find the
     centre offset that puts density at log-exposure 0 back where it was. Both
     polarities, because the solver negates z for a positive stock and a sign
     error there would only show on one of them. */
  TR_STEP("developer exhaustion reshapes the shoulder without moving midgray");
  for(int positive = 0; positive < 2; positive++)
  {
    double c_base[3], s_base[3], g_base[3];
    double c_exh[3], s_exh[3], g_exh[3];
    _sf_morph_channel(CENTERS, SIGMAS, NULL, 0, 3, positive, 1.0, 1.0, 1.0, 0.0, AMPS,
                      c_base, s_base, g_base);
    _sf_morph_channel(CENTERS, SIGMAS, NULL, 0, 3, positive, 1.0, 1.0, 1.0, 0.35, AMPS,
                      c_exh, s_exh, g_exh);

    const double d_base
        = _sf_channel_density_at(0.0, c_base, AMPS, s_base, NULL, 0, 3, g_base, positive);
    const double d_exh
        = _sf_channel_density_at(0.0, c_exh, AMPS, s_exh, NULL, 0, 3, g_exh, positive);
    TR_DEBUG("positive=%d D(0) %e -> %e", positive, d_base, d_exh);
    assert_double_close(d_exh, d_base, E);

    /* and it did something: the blend is on, the centres actually moved, and
       the curve away from midgray has a different shape */
    for(int l = 0; l < 3; l++) assert_double_close(g_exh[l], 0.35, E);
    assert_true(fabs(c_exh[0] - c_base[0]) > E);
    double max_diff = 0.0;
    for(int i = 0; i <= 100; i++)
    {
      const double x = -3.0 + 4.0 * (double)i / 100.0;
      const double db
          = _sf_channel_density_at(x, c_base, AMPS, s_base, NULL, 0, 3, g_base, positive);
      const double de
          = _sf_channel_density_at(x, c_exh, AMPS, s_exh, NULL, 0, 3, g_exh, positive);
      max_diff = fmax(max_diff, fabs(de - db));
    }
    TR_DEBUG("positive=%d largest density change %e", positive, max_diff);
    assert_true(max_diff > 1e-3);
  }
}

static void test_zero_exhaustion_leaves_the_curve_alone(void **state)
{
  TR_STEP("with no exhaustion and unit gammas the morph is the identity");
  double centers[3], sigmas[3], gmix[3];
  _sf_morph_channel(CENTERS, SIGMAS, NULL, 0, 3, 0, 1.0, 1.0, 1.0, 0.0, AMPS, centers, sigmas,
                    gmix);
  for(int l = 0; l < 3; l++)
  {
    assert_double_close(centers[l], CENTERS[l], E);
    assert_double_close(sigmas[l], SIGMAS[l], E);
    assert_double_close(gmix[l], 0.0, E);
  }
}

static void test_morph_gamma_scales_centers_and_sigmas(void **state)
{
  /* The coupled-gamma morph divides each sublayer's centre and width by its
     own gamma, which is what makes a higher-contrast development steepen the curve, not merely
     shift it. */
  TR_STEP("the chemistry gamma divides centres and widths alike");
  const double gamma = 1.25;
  double centers[3], sigmas[3], gmix[3];
  _sf_morph_channel(CENTERS, SIGMAS, NULL, 0, 3, 0, gamma, 1.0, 1.0, 0.0, AMPS, centers, sigmas,
                    gmix);
  for(int l = 0; l < 3; l++)
  {
    assert_double_close(centers[l], CENTERS[l] / gamma, E);
    assert_double_close(sigmas[l], SIGMAS[l] / gamma, E);
  }
}

static void test_morph_gammas_follow_layer_speed(void **state)
{
  /* Layer speed is by centre, not by position in the arrays: the lowest
     centre is the fast layer and takes gamma * gamma_fast, the middle and the
     highest take gamma * gamma_slow (upstream gives the mid layer the slow
     factor). Centres deliberately out of order, and fast and slow factors
     deliberately different, so a swapped assignment or a skipped sort fails. */
  TR_STEP("the fast layer takes the fast factor, mid and slow the slow one");
  const double centers_in[3] = { -1.0, -1.2, -1.1 }; /* slow, fast, mid */
  const double sigmas_in[3] = { 0.26, 0.30, 0.28 };
  const double gamma = 1.1, gamma_fast = 1.3, gamma_slow = 0.9;
  double centers[3], sigmas[3], gmix[3];
  _sf_morph_channel(centers_in, sigmas_in, NULL, 0, 3, 0, gamma, gamma_fast, gamma_slow, 0.0,
                    AMPS, centers, sigmas, gmix);
  const double g[3] = { gamma * gamma_slow, gamma * gamma_fast, gamma * gamma_slow };
  for(int l = 0; l < 3; l++)
  {
    TR_DEBUG("layer %d center %e -> %e", l, centers_in[l], centers[l]);
    assert_double_close(centers[l], centers_in[l] / g[l], E);
    assert_double_close(sigmas[l], sigmas_in[l] / g[l], E);
  }
}

static void test_morph_sigma_has_a_floor(void **state)
{
  TR_STEP("a steepening gamma never narrows a layer below the sigma floor");
  const double sigmas_in[3] = { 0.06, 0.28, 0.26 };
  double centers[3], sigmas[3], gmix[3];
  _sf_morph_channel(CENTERS, sigmas_in, NULL, 0, 3, 0, 2.0, 1.0, 1.0, 0.0, AMPS, centers, sigmas,
                    gmix);
  assert_double_close(sigmas[0], SF_SIGMA_FLOOR, E); /* 0.03 unfloored */
  assert_double_close(sigmas[1], 0.14, E);
  assert_double_close(sigmas[2], 0.13, E);
  /* the floor holds the width only; the centre still moves */
  assert_double_close(centers[0], CENTERS[0] / 2.0, E);
}

/*
 * TEST FUNCTIONS: DIR couplers
 * (upstream tests/test_couplers.py::TestDirCouplers)
 */

static void test_couplers_inactive_gives_no_correction(void **state)
{
  TR_STEP("with the couplers off the exposure correction is identically zero");
  const double dmax[3] = { 2.4, 2.2, 2.0 };
  sf_sim_t *sim = _sim_linear_ramp(dmax);
  sim->couplers_active = 0;
  sim->couplers_M[0][0] = sim->couplers_M[1][1] = sim->couplers_M[2][2] = 0.5;

  const float lograw[3] = { -1.0f, -1.2f, -1.4f };
  float corr[3] = { 9.0f, 9.0f, 9.0f };
  sf_sim_develop_corr(sim, lograw, corr, 1, 3);
  for(int c = 0; c < 3; c++) assert_double_close(corr[c], 0.0, 1e-6);
  free(sim);
}

static void test_couplers_zero_density_gives_no_correction(void **state)
{
  /* No developed silver means no inhibitor released, whatever the matrix says. */
  TR_STEP("zero density releases no inhibitor, so the exposure is untouched");
  const double dmax[3] = { 0.0, 0.0, 0.0 };
  sf_sim_t *sim = _sim_linear_ramp(dmax);
  for(int i = 0; i < 3; i++)
    for(int j = 0; j < 3; j++) sim->couplers_M[i][j] = 0.3 + 0.1 * (i + j);

  const float lograw[3] = { -1.0f, -0.5f, 0.0f };
  float corr[3] = { 9.0f, 9.0f, 9.0f };
  sf_sim_develop_corr(sim, lograw, corr, 1, 3);
  for(int c = 0; c < 3; c++) assert_double_close(corr[c], 0.0, 1e-6);
  free(sim);
}

static void test_couplers_diagonal_matrix_keeps_channels_independent(void **state)
{
  /* Upstream states this as "no interlayer inhibition is diagonal", checking
     the matrix the coupler parameters build. Here the matrix is built inside
     sf_sim_build() from a loaded profile, so the same property is asserted one
     step further down, where it is what actually matters: with no off-diagonal
     term, a channel's correction depends on its own density and nothing else. */
  TR_STEP("a diagonal coupler matrix leaves each channel to itself");
  const double dmax[3] = { 2.4, 2.2, 2.0 };
  const double self_inhibition[3] = { 0.5, 0.4, 0.3 };
  sf_sim_t *sim = _sim_linear_ramp(dmax);
  for(int c = 0; c < 3; c++) sim->couplers_M[c][c] = self_inhibition[c];

  const float lograw_a[3] = { -1.0f, -1.2f, -1.4f };
  float corr_a[3];
  sf_sim_develop_corr(sim, lograw_a, corr_a, 1, 3);
  for(int c = 0; c < 3; c++)
  {
    const double expected = _ramp_density(lograw_a[c], dmax[c]) * self_inhibition[c];
    TR_DEBUG("channel %d corr=%e expected=%e", c, corr_a[c], expected);
    assert_double_close(corr_a[c], expected, 1e-5);
  }

  /* move one channel's exposure: the other two corrections must not budge */
  const float lograw_b[3] = { 0.5f, -1.2f, -1.4f };
  float corr_b[3];
  sf_sim_develop_corr(sim, lograw_b, corr_b, 1, 3);
  assert_true(fabsf(corr_b[0] - corr_a[0]) > 1e-4f);
  assert_double_close(corr_b[1], corr_a[1], 1e-6);
  assert_double_close(corr_b[2], corr_a[2], 1e-6);
  free(sim);
}

static void test_couplers_interlayer_crosses_channels(void **state)
{
  /* The other side of the same coin, and the reason the test above is worth
     having: an off-diagonal term must reach across, or the matrix is being
     applied in the wrong orientation. Donor row -> receiver column, so
     M[0][1] carries red's density into green's correction. */
  TR_STEP("an off-diagonal coupler term carries one channel into another");
  const double dmax[3] = { 2.4, 2.2, 2.0 };
  sf_sim_t *sim = _sim_linear_ramp(dmax);
  sim->couplers_M[0][1] = 0.25;

  const float lograw[3] = { -1.0f, -1.2f, -1.4f };
  float corr[3];
  sf_sim_develop_corr(sim, lograw, corr, 1, 3);
  const double expected = _ramp_density(lograw[0], dmax[0]) * 0.25;
  assert_double_close(corr[1], expected, 1e-5);
  assert_double_close(corr[0], 0.0, 1e-6);
  assert_double_close(corr[2], 0.0, 1e-6);
  free(sim);
}

static void test_couplers_correction_scales_with_density(void **state)
{
  TR_STEP("more developed silver releases proportionally more inhibitor");
  const double dmax[3] = { 2.4, 2.2, 2.0 };
  sf_sim_t *sim = _sim_linear_ramp(dmax);
  for(int c = 0; c < 3; c++) sim->couplers_M[c][c] = 0.5;

  const float low[3] = { -2.0f, -2.0f, -2.0f };
  const float high[3] = { 0.0f, 0.0f, 0.0f };
  float corr_low[3], corr_high[3];
  sf_sim_develop_corr(sim, low, corr_low, 1, 3);
  sf_sim_develop_corr(sim, high, corr_high, 1, 3);
  for(int c = 0; c < 3; c++)
  {
    assert_true(corr_high[c] > corr_low[c]);
    /* the ramp is linear, so the ratio is the exposure ratio exactly */
    const double expected = _ramp_density(high[c], dmax[c]) / _ramp_density(low[c], dmax[c]);
    assert_double_close((double)corr_high[c] / corr_low[c], expected, 1e-4);
  }
  free(sim);
}

static void test_couplers_positive_stock_inverts_the_silver(void **state)
{
  /* A reversal stock's dye density falls where silver is developed, so the
     silver that releases inhibitor is dmax less the curve's density. */
  TR_STEP("a positive stock releases inhibitor from dmax less its density");
  const double dmax[3] = { 2.4, 2.2, 2.0 };
  sf_sim_t *sim = _sim_linear_ramp(dmax);
  sim->film_positive = 1;
  for(int c = 0; c < 3; c++) sim->couplers_M[c][c] = 0.5;

  const float lograw[3] = { -1.0f, -1.2f, -1.4f };
  float corr[3];
  sf_sim_develop_corr(sim, lograw, corr, 1, 3);
  for(int c = 0; c < 3; c++)
  {
    const double expected = (dmax[c] - _ramp_density(lograw[c], dmax[c])) * 0.5;
    TR_DEBUG("channel %d corr=%e expected=%e", c, corr[c], expected);
    assert_double_close(corr[c], expected, 1e-5);
  }
  free(sim);
}

static void test_couplers_donor_langmuir_saturates_the_silver(void **state)
{
  /* donor side: silver s releases s * (K + Dref) / (K + s), which is s itself
     at s = Dref, more below it and less above it */
  TR_STEP("the donor Langmuir term rescales silver around its reference density");
  const double dmax[3] = { 2.4, 2.2, 2.0 };
  sf_sim_t *sim = _sim_linear_ramp(dmax);
  sim->couplers_donor_lm = 1;
  for(int c = 0; c < 3; c++)
  {
    sim->couplers_M[c][c] = 0.5;
    sim->couplers_donor_K[c] = 0.5;
    sim->couplers_donor_Dref[c] = 0.99; /* the green channel's silver below */
  }

  const float lograw[3] = { -1.0f, -1.2f, -1.4f }; /* silver 1.2, 0.99, 0.8 */
  float corr[3];
  sf_sim_develop_corr(sim, lograw, corr, 1, 3);
  for(int c = 0; c < 3; c++)
  {
    const double silver = _ramp_density(lograw[c], dmax[c]);
    const double expected = silver * (0.5 + 0.99) / (0.5 + silver) * 0.5;
    TR_DEBUG("channel %d corr=%e expected=%e", c, corr[c], expected);
    assert_double_close(corr[c], expected, 1e-5);
  }
  assert_double_close(corr[1], 0.99 * 0.5, 1e-5);
  assert_true(corr[0] < 1.2 * 0.5);
  assert_true(corr[2] > 0.8 * 0.5);
  free(sim);
}

/* The correction is consumed by sf_sim_develop(): it is subtracted from the
   log exposure, and the result is read off the pre-coupler curves. With the
   couplers off, the correction is ignored and the normalized curves are read.
   The two tables are made to differ here so the test sees which one is read. */
static void test_couplers_correction_shifts_the_developed_density(void **state)
{
  TR_STEP("develop subtracts the correction from exposure on the pre-coupler curves");
  const double dmax[3] = { 2.4, 2.2, 2.0 };
  sf_sim_t *sim = _sim_linear_ramp(dmax);
  for(int i = 0; i < SF_NLE; i++)
    for(int c = 0; c < 3; c++) sim->curves_before_f[i][c] = 0.5f * sim->curves_norm_f[i][c];

  const float lograw[3] = { -1.0f, -1.2f, -1.4f };
  const float corr[3] = { 0.2f, 0.1f, 0.0f };
  float cmy[3];
  sf_sim_develop(sim, lograw, corr, cmy, 1, 3, 3);
  for(int c = 0; c < 3; c++)
  {
    const double expected = 0.5 * _ramp_density(lograw[c] - corr[c], dmax[c]);
    TR_DEBUG("channel %d cmy=%e expected=%e", c, cmy[c], expected);
    assert_double_close(cmy[c], expected, 1e-5);
  }

  sim->couplers_active = 0;
  sf_sim_develop(sim, lograw, corr, cmy, 1, 3, 3);
  for(int c = 0; c < 3; c++)
    assert_double_close(cmy[c], _ramp_density(lograw[c], dmax[c]), 1e-5);
  free(sim);
}

static void test_couplers_receiver_langmuir_saturates_the_inhibitor(void **state)
{
  /* receiver side: an arriving inhibitor c acts as c * (Kr + cref) / (Kr + c) */
  TR_STEP("the receiver Langmuir term rescales the inhibitor that arrives");
  const double dmax[3] = { 2.4, 2.2, 2.0 };
  sf_sim_t *sim = _sim_linear_ramp(dmax);
  for(int i = 0; i < SF_NLE; i++)
    for(int c = 0; c < 3; c++) sim->curves_before_f[i][c] = sim->curves_norm_f[i][c];
  sim->couplers_recv_lm = 1;
  for(int c = 0; c < 3; c++)
  {
    sim->couplers_recv_Kr[c] = 0.5;
    sim->couplers_recv_cref[c] = 0.3;
  }

  const float lograw[3] = { -1.0f, -1.2f, -1.4f };
  const float corr[3] = { 0.2f, 0.3f, 0.4f };
  float cmy[3];
  sf_sim_develop(sim, lograw, corr, cmy, 1, 3, 3);
  for(int c = 0; c < 3; c++)
  {
    const double eff = corr[c] * (0.5 + 0.3) / (0.5 + corr[c]);
    const double expected = _ramp_density(lograw[c] - eff, dmax[c]);
    TR_DEBUG("channel %d cmy=%e expected=%e", c, cmy[c], expected);
    assert_double_close(cmy[c], expected, 1e-5);
  }
  /* at c = cref the term is the identity */
  assert_double_close(cmy[1], _ramp_density(lograw[1] - 0.3, dmax[1]), 1e-5);
  free(sim);
}

/*
 * TEST FUNCTIONS: spectral upsampling table declarations
 */

/* a minimal SFS2 table header: what sf_pack_peek_tables() reads and nothing
   more, so the test needs no real table data */
static void _write_table_header(const char *path,
                                const uint32_t lut_hash,
                                const char *lut_id)
{
  FILE *fh = g_fopen(path, "wb");
  assert_non_null(fh);
  const int32_t version = 2, dims[3] = { 2, 2, SF_NWL }, dtype = 0;
  const int32_t id_len = (int32_t)strlen(lut_id);
  assert_int_equal(fwrite("SFS2", 1, 4, fh), 4);
  assert_int_equal(fwrite(&version, 4, 1, fh), 1);
  assert_int_equal(fwrite(dims, 4, 3, fh), 3);
  assert_int_equal(fwrite(&dtype, 4, 1, fh), 1);
  assert_int_equal(fwrite(&lut_hash, 4, 1, fh), 1);
  assert_int_equal(fwrite(&id_len, 4, 1, fh), 1);
  assert_int_equal(fwrite(lut_id, 1, id_len, fh), (size_t)id_len);
  fclose(fh);
}

static void _write_file(const char *dir,
                        const char *name,
                        const char *text)
{
  char *path = g_build_filename(dir, name, NULL);
  assert_true(g_file_set_contents(path, text, -1, NULL));
  g_free(path);
}

static void _remove_in(const char *dir,
                       const char *name)
{
  char *path = g_build_filename(dir, name, NULL);
  g_remove(path);
  g_free(path);
}

/* pack_format 3: the declaration decides which files are tables and which one
   is the default, the headers decide their identity. The default comes first
   whatever its position in the declaration, and an entry whose file names a
   path rather than a bare file name is not a table. The table it names exists
   and is valid, so only the path check can be what rejects it */
static void test_peek_tables_reads_a_format_3_declaration(void **state)
{
  (void)state;
  TR_STEP("a format 3 pack lists its tables, default first");
  gchar *dir = g_dir_make_tmp("spektra-pack-XXXXXX", NULL);
  assert_non_null(dir);

  _write_file(dir, "pack.json",
              "{ \"pack_format\": 3, \"spectral_upsampling\": ["
              " { \"identifier\": \"first\", \"kind\": \"irradiance\","
              "   \"file\": \"first.f32\" },"
              " { \"identifier\": \"second\", \"kind\": \"reflectance\","
              "   \"scene_illuminant\": \"D65\", \"file\": \"second.f32\","
              "   \"default\": true },"
              " { \"identifier\": \"nested\", \"kind\": \"irradiance\","
              "   \"file\": \"sub/third.f32\" } ] }");
  char *first = g_build_filename(dir, "first.f32", NULL);
  char *second = g_build_filename(dir, "second.f32", NULL);
  char *subdir = g_build_filename(dir, "sub", NULL);
  char *third = g_build_filename(subdir, "third.f32", NULL);
  _write_table_header(first, 0x11111111u, "first_id");
  _write_table_header(second, 0x22222222u, "second_id");
  assert_int_equal(g_mkdir(subdir, 0700), 0);
  _write_table_header(third, 0x33333333u, "third_id");

  sf_table_info_t info[SF_MAX_TABLES];
  const int n = sf_pack_peek_tables(dir, info, SF_MAX_TABLES);
  assert_int_equal(n, 2);
  assert_string_equal(info[0].identifier, "second");
  assert_int_equal(info[0].lut_hash, 0x22222222u);
  assert_string_equal(info[0].lut_id, "second_id");
  assert_int_equal(info[0].kind, SF_LUT_REFLECTANCE);
  assert_string_equal(info[1].identifier, "first");
  assert_int_equal(info[1].lut_hash, 0x11111111u);
  assert_int_equal(info[1].kind, SF_LUT_IRRADIANCE);

  g_remove(third);
  g_rmdir(subdir);
  g_remove(first);
  g_remove(second);
  _remove_in(dir, "pack.json");
  g_rmdir(dir);
  g_free(third);
  g_free(subdir);
  g_free(first);
  g_free(second);
  g_free(dir);
}

/* below format 3 nothing is declared: spectra_lut.f32 is the one table, and
   it is the irradiance one */
static void test_peek_tables_reads_a_format_2_pack(void **state)
{
  (void)state;
  TR_STEP("a format 2 pack has one undeclared irradiance table");
  gchar *dir = g_dir_make_tmp("spektra-pack-XXXXXX", NULL);
  assert_non_null(dir);
  _write_file(dir, "pack.json", "{ \"pack_format\": 2 }");
  char *lut = g_build_filename(dir, "spectra_lut.f32", NULL);
  _write_table_header(lut, 0x33333333u, "irradiance_xy_tc@0.3.3");

  sf_table_info_t info[SF_MAX_TABLES];
  assert_int_equal(sf_pack_peek_tables(dir, info, SF_MAX_TABLES), 1);
  assert_string_equal(info[0].identifier, "");
  assert_int_equal(info[0].lut_hash, 0x33333333u);
  assert_int_equal(info[0].kind, SF_LUT_IRRADIANCE);

  g_remove(lut);
  _remove_in(dir, "pack.json");
  g_rmdir(dir);
  g_free(lut);
  g_free(dir);
}

/*
 * MAIN
 */

int main(int argc,
         char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_knee_below_threshold_is_identity),
    cmocka_unit_test(test_knee_above_threshold_compresses),
    cmocka_unit_test(test_knee_asymptotes_at_limit),
    cmocka_unit_test(test_knee_is_continuous_at_threshold),
    cmocka_unit_test(test_knee_float_matches_double),
    cmocka_unit_test(test_aces_neutral_is_unchanged),
    cmocka_unit_test(test_aces_black_is_identity),
    cmocka_unit_test(test_aces_leaves_the_max_channel_alone),
    cmocka_unit_test(test_aces_pulls_negatives_back_inside),
    cmocka_unit_test(test_aces_compresses_stronger_excursions_harder),
    cmocka_unit_test(test_compress_xy_leaves_white_alone),
    cmocka_unit_test(test_compress_xy_leaves_well_inside_alone),
    cmocka_unit_test(test_compress_xy_pulls_oog_inside),
    cmocka_unit_test(test_compress_xy_preserves_direction),
    cmocka_unit_test(test_density_curve_is_monotonic),
    cmocka_unit_test(test_density_curve_is_near_zero_at_low_exposure),
    cmocka_unit_test(test_density_curve_is_inverted_for_positive_stock),
    cmocka_unit_test(test_skewed_density_curve_is_monotonic),
    cmocka_unit_test(test_skew_keeps_the_layer_median_on_its_centre),
    cmocka_unit_test(test_exhausted_density_curve_is_monotonic),
    cmocka_unit_test(test_developer_exhaustion_preserves_midgray),
    cmocka_unit_test(test_zero_exhaustion_leaves_the_curve_alone),
    cmocka_unit_test(test_morph_gamma_scales_centers_and_sigmas),
    cmocka_unit_test(test_morph_gammas_follow_layer_speed),
    cmocka_unit_test(test_morph_sigma_has_a_floor),
    cmocka_unit_test(test_couplers_inactive_gives_no_correction),
    cmocka_unit_test(test_couplers_zero_density_gives_no_correction),
    cmocka_unit_test(test_couplers_diagonal_matrix_keeps_channels_independent),
    cmocka_unit_test(test_couplers_interlayer_crosses_channels),
    cmocka_unit_test(test_couplers_correction_scales_with_density),
    cmocka_unit_test(test_couplers_positive_stock_inverts_the_silver),
    cmocka_unit_test(test_couplers_donor_langmuir_saturates_the_silver),
    cmocka_unit_test(test_couplers_correction_shifts_the_developed_density),
    cmocka_unit_test(test_couplers_receiver_langmuir_saturates_the_inhibitor),
    cmocka_unit_test(test_peek_tables_reads_a_format_3_declaration),
    cmocka_unit_test(test_peek_tables_reads_a_format_2_pack),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
