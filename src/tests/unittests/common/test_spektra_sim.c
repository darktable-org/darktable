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
 * sublayer build. That is a matter of scope rather than availability: the
 * current pack ships with darktable under data/spektrafilm/, and only edits
 * pinned to an older one fetch anything at runtime, so a fixture is there for
 * the taking. But a test built on it reads real profile data off disk, which
 * makes it an integration test with a data dependency and not a unit test.
 * Those live next door, in test_spektra_pack.c, which CMake points at the
 * shipped pack and which skips itself when there is none on disk.
 *
 * Including the .c rather than the .h is what the module tests already do (see
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
   here is exact in exact arithmetic, so this only has to absorb rounding. */
#define E 1e-9

/* Compare in double, whatever cmocka happens to be installed.
   cmocka's own assert_double_close() casts both operands AND the epsilon to
   float, so from 1.1.2 on -- where its macro shadows the fallback in
   ../util/assert.h -- an epsilon of 1e-9 sits below the float resolution of
   values around 1 and the comparison stops meaning anything. Its
   assert_double_equal() would be the right tool, but 1.1.0 is the minimum the
   build accepts and has neither macro, so neither can be relied on. */
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
   density at any exposure is a closed form the test can state on its own rather
   than borrowing the interpolator it is checking. Caller frees. */
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

/*
 * TEST FUNCTIONS: output gamut compression, ACES RGC
 * (upstream tests/test_gamut_compression.py::TestCompressRgbAcesRgc)
 */

static void test_aces_neutral_is_unchanged(void **state)
{
  TR_STEP("achromatic pixels pass through the output compressor untouched");
  double rgb[3] = { 0.5, 0.5, 0.5 };
  compress_rgb_aces(rgb);
  for(int c = 0; c < 3; c++) assert_double_close(rgb[c], 0.5, E);
}

static void test_aces_black_is_identity(void **state)
{
  TR_STEP("pixels at or below black keep their values -- no chromaticity to compress");
  double rgb[3] = { 0.0, 0.0, 0.0 };
  compress_rgb_aces(rgb);
  for(int c = 0; c < 3; c++) assert_double_close(rgb[c], 0.0, E);
}

static void test_aces_leaves_the_max_channel_alone(void **state)
{
  TR_STEP("the achromatic max is the anchor and never moves");
  double rgb[3] = { 2.0, -0.1, 0.3 };
  compress_rgb_aces(rgb);
  assert_double_close(rgb[0], 2.0, E);
}

static void test_aces_pulls_negatives_back_inside(void **state)
{
  TR_STEP("out-of-gamut negatives come back non-negative, and by a real margin");
  double rgb[3] = { 1.5, -0.1, -0.05 };
  compress_rgb_aces(rgb);
  assert_double_close(rgb[0], 1.5, E);
  assert_true(rgb[1] >= 0.0);
  assert_true(rgb[2] >= 0.0);
  /* not merely clipped to a hair above zero: the knee lands them well inside */
  assert_true(rgb[1] < 0.2 * 1.5);
  assert_true(rgb[2] < 0.2 * 1.5);
}

static void test_aces_compresses_stronger_excursions_harder(void **state)
{
  TR_STEP("the further out of gamut, the closer to the boundary the result lands");
  double mild[3] = { 1.0, -0.05, -0.05 };
  double hard[3] = { 1.0, -1.0, -1.0 };
  compress_rgb_aces(mild);
  compress_rgb_aces(hard);
  TR_DEBUG("mild=%e hard=%e", mild[1], hard[1]);
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
  TR_STEP("density vanishes far below the toe");
  enum { N = 200 };
  double le[N], density[N];
  for(int i = 0; i < N; i++) le[i] = -6.0 + 8.0 * (double)i / (double)(N - 1);
  for(int sept = 0; sept < 2; sept++)
  {
    eval_cdfs_channel(density, le, N, CENTERS, AMPS, SIGMAS, NULL, sept, 3, 0);
    for(int i = 0; i < 5; i++) assert_true(density[i] < 0.01);
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

    /* and it did something: the blend is on, and the centres actually moved */
    for(int l = 0; l < 3; l++) assert_double_close(g_exh[l], 0.35, E);
    assert_true(fabs(c_exh[0] - c_base[0]) > E);
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
     own gamma, which is what makes a higher-contrast development steepen the
     curve rather than merely shift it. */
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
    cmocka_unit_test(test_developer_exhaustion_preserves_midgray),
    cmocka_unit_test(test_zero_exhaustion_leaves_the_curve_alone),
    cmocka_unit_test(test_morph_gamma_scales_centers_and_sigmas),
    cmocka_unit_test(test_couplers_inactive_gives_no_correction),
    cmocka_unit_test(test_couplers_zero_density_gives_no_correction),
    cmocka_unit_test(test_couplers_diagonal_matrix_keeps_channels_independent),
    cmocka_unit_test(test_couplers_interlayer_crosses_channels),
    cmocka_unit_test(test_couplers_correction_scales_with_density),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
