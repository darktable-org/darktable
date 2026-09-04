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
 * cmocka tests for common/spektra_sim.c against the data pack darktable ships.
 *
 * The companion suite, test_spektra_sim.c, covers the algorithms that need no
 * data. These are the ones that do: profile contents, the enlarger's dichroic
 * filters and neutral filter database, and the assembled pipeline. They are
 * ports of the upstream python tests that use the same fixtures --
 * tests/test_profiles.py, tests/test_enlarger_filters.py and
 * tests/test_pipeline_smoke.py -- and each test names the one it came from.
 *
 * The pack is found at SPEKTRA_PACK_DIR, handed over by CMake (see
 * CMakeLists.txt in this directory), either as a directory holding pack.json
 * directly or as one holding a single versioned subdirectory that does. Only
 * edits pinned to an older pack fetch anything at runtime, so the current pack
 * being on disk is the normal case -- but when it is not there, every test
 * here skips. A missing pack means this build cannot answer
 * the question, which is not the same as the engine being wrong, and a build
 * that goes red for the wrong reason is a build people learn to ignore.
 *
 * The tolerances are deliberately loose. What is being pinned down is that the
 * pipeline stays finite, bounded, ordered and reproducible on real data, not
 * that particular numbers come out -- exact values belong in a regression
 * baseline against a pinned pack, where a deliberate change to the model can be
 * re-blessed in one place instead of scattering magic numbers through here.
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

#ifndef SPEKTRA_PACK_DIR
#define SPEKTRA_PACK_DIR ""
#endif

/* See test_spektra_sim.c: cmocka's assert_float_equal() collapses everything to
   float, and from 1.1.2 on it shadows the fallback in ../util/assert.h. */
#ifndef assert_double_close
#define assert_double_close(a, b, epsilon) \
  assert_true(fabs((double)(a) - (double)(b)) <= (double)(epsilon))
#endif

/* The stocks upstream's own fixtures use (conftest.py). Absent from a pack,
   the first film and the first paper stand in, so a pack that renames or drops
   them still gets exercised, not skipped. */
#define FILM_STOCK "kodak_portra_400"
#define PRINT_STOCK "kodak_portra_endura"

/* Output slack. The scanner's gamut compressor is what bounds the render, and
   it approaches its limit asymptotically, without clamping, so the bound is
   checked with room for the last ulps, not as a hard [0, 1]. */
#define OUT_SLACK 1e-3

typedef struct fixture_t
{
  sf_pack_t *pack;
  sf_profile_t *film, *print;
  char pack_dir[PATH_MAX];
} fixture_t;

/* No pack on disk, no verdict to give -- so every test starts with this. It
   tests the pack and nothing else: a test that asserts the pack CONTAINS
   something has to be able to fail when it does not, and folding that check in
   here would turn every such failure into a skip. */
#define REQUIRE_PACK(f)                                                                            \
  do                                                                                               \
  {                                                                                                \
    if(!(f) || !(f)->pack) skip();                                                                 \
  } while(0)

/* For tests that need a film to work with, not to check. A pack with no
   filming profile is a real fault, but test_pack_ships_both_a_film_and_a_paper
   is the one that reports it; everything else has nothing to say without one
   and skips. */
#define REQUIRE_FILM(f)                                                                            \
  do                                                                                               \
  {                                                                                                \
    REQUIRE_PACK(f);                                                                               \
    if(!(f)->film) skip();                                                                          \
  } while(0)

/*
 * FIXTURE
 */

/* SPEKTRA_PACK_DIR itself when it holds pack.json, else its one subdirectory
   that does. Written this way so the tests need not know which spektrafilm
   release is currently shipped, and so they keep working if the layout is
   flattened later. Returns false when there is no pack to be had. */
static gboolean _resolve_pack_dir(char *dst,
                                  size_t dstsz)
{
  const char *root = SPEKTRA_PACK_DIR;
  if(!root || !root[0]) return FALSE;

  char *direct = g_build_filename(root, "pack.json", NULL);
  const gboolean here = g_file_test(direct, G_FILE_TEST_IS_REGULAR);
  g_free(direct);
  if(here)
  {
    g_strlcpy(dst, root, dstsz);
    return TRUE;
  }

  GDir *gd = g_dir_open(root, 0, NULL);
  if(!gd) return FALSE;
  gboolean found = FALSE;
  const char *fn;
  while((fn = g_dir_read_name(gd)))
  {
    char *sub = g_build_filename(root, fn, "pack.json", NULL);
    if(g_file_test(sub, G_FILE_TEST_IS_REGULAR))
    {
      char *dir = g_build_filename(root, fn, NULL);
      g_strlcpy(dst, dir, dstsz);
      g_free(dir);
      found = TRUE;
    }
    g_free(sub);
    if(found) break;
  }
  g_dir_close(gd);
  return found;
}

/* Load the profile for `stock` from the pack, or -- when the pack has no such
   stock -- the first profile of the right kind, so the suite follows the pack
   it is given instead of a stock list baked in here. */
static sf_profile_t *_load_stock(const char *dir,
                                 const char *stock,
                                 const gboolean printing)
{
  char *profdir = g_build_filename(dir, "profiles", NULL);
  GDir *gd = g_dir_open(profdir, 0, NULL);
  if(!gd)
  {
    g_free(profdir);
    return NULL;
  }
  sf_profile_t *wanted = NULL, *fallback = NULL;
  const char *fn;
  while((fn = g_dir_read_name(gd)) && !wanted)
  {
    if(!g_str_has_suffix(fn, ".json")) continue;
    char *path = g_build_filename(profdir, fn, NULL);
    char *err = NULL;
    sf_profile_t *p = sf_profile_load(path, 0.0f, &err);
    g_free(path);
    free(err);
    if(!p) continue;

    const char *stage = sf_profile_stage(p);
    const gboolean is_print = stage && !strcmp(stage, "printing");
    if(is_print != printing)
    {
      sf_profile_free(p);
      continue;
    }
    const char *s = sf_profile_stock(p);
    if(s && !strcmp(s, stock))
      wanted = p;
    else if(!fallback)
      fallback = p;
    else
      sf_profile_free(p);
  }
  g_dir_close(gd);
  g_free(profdir);

  if(wanted)
  {
    if(fallback) sf_profile_free(fallback);
    return wanted;
  }
  if(fallback)
    TR_NOTE("pack has no %s; using %s instead", stock,
            sf_profile_stock(fallback) ? sf_profile_stock(fallback) : "(unnamed)");
  return fallback;
}

static int group_setup(void **state)
{
  fixture_t *f = calloc(1, sizeof(fixture_t));
  if(!f) return -1;
  *state = f;

  if(!_resolve_pack_dir(f->pack_dir, sizeof f->pack_dir))
  {
    TR_NOTE("no data pack under \"%s\" -- every test in this suite will skip", SPEKTRA_PACK_DIR);
    return 0;
  }
  TR_NOTE("data pack: %s", f->pack_dir);

  char *err = NULL;
  f->pack = sf_pack_load(f->pack_dir, &err, NULL);
  if(!f->pack)
  {
    TR_NOTE("pack failed to load: %s", err ? err : "(no message)");
    free(err);
    return 0; /* the first test reports it; the rest skip */
  }
  f->film = _load_stock(f->pack_dir, FILM_STOCK, FALSE);
  f->print = _load_stock(f->pack_dir, PRINT_STOCK, TRUE);
  return 0;
}

static int group_teardown(void **state)
{
  fixture_t *f = *state;
  if(f)
  {
    if(f->film) sf_profile_free(f->film);
    if(f->print) sf_profile_free(f->print);
    if(f->pack) sf_pack_free(f->pack);
    free(f);
  }
  return 0;
}

/*
 * HELPERS
 */

/* Build a sim over the fixture's stocks. `configure` may be NULL. */
static sf_sim_t *_build(const fixture_t *f,
                        void (*configure)(sf_sim_params_t *))
{
  sf_sim_params_t p;
  sf_sim_params_defaults(&p);
  if(configure) configure(&p);

  char *err = NULL;
  sf_sim_t *sim = sf_sim_build(f->pack, f->film, p.scan_film ? NULL : f->print, &p, &err);
  if(!sim) TR_NOTE("sim build failed: %s", err ? err : "(no message)");
  free(err);
  return sim;
}

/* The whole per-pixel chain, the way spektrafilm.c runs it minus the spatial
   effects (which are the caller's, not the engine's). In-place throughout,
   which the API allows and which keeps this readable. */
static void _render(const sf_sim_t *sim,
                    const float *rgb_in,
                    float *rgb_out,
                    const size_t npix)
{
  float *work = malloc(npix * 3 * sizeof(float));
  float *corr = malloc(npix * 3 * sizeof(float));
  assert_non_null(work);
  assert_non_null(corr);

  sf_sim_expose(sim, rgb_in, work, npix, 3, 3);
  sf_sim_lograw(work, npix, 3);
  sf_sim_develop_corr(sim, work, corr, npix, 3);
  sf_sim_develop(sim, work, corr, work, npix, 3, 3);
  if(sim->has_print)
  {
    sf_sim_print_expose(sim, work, work, npix, 3, 3);
    sf_sim_print_develop(sim, work, work, npix, 3, 3);
  }
  sf_sim_scan(sim, work, rgb_out, npix, 3, 3);

  free(work);
  free(corr);
}

static void _fill_grey(float *rgb,
                       const size_t npix,
                       const float level)
{
  for(size_t i = 0; i < npix; i++)
  {
    rgb[i * 3 + 0] = level;
    rgb[i * 3 + 1] = level;
    rgb[i * 3 + 2] = level;
  }
}

static double _mean(const float *rgb,
                    const size_t npix)
{
  double acc = 0.0;
  for(size_t i = 0; i < npix * 3; i++) acc += rgb[i];
  return acc / (double)(npix * 3);
}

/* upstream tests/test_pipeline_smoke.py::_assert_valid_output */
static void _assert_valid_output(const float *rgb,
                                 const size_t npix,
                                 const gboolean bounded)
{
  for(size_t i = 0; i < npix * 3; i++)
  {
    assert_true(isfinite(rgb[i]));
    if(bounded)
    {
      assert_true(rgb[i] >= -OUT_SLACK);
      assert_true(rgb[i] <= 1.0 + OUT_SLACK);
    }
  }
}

/*
 * TEST FUNCTIONS: the pack itself
 * (upstream tests/test_profiles.py, tests/test_lut.py)
 */

static void test_pack_loads_and_identifies_itself(void **state)
{
  fixture_t *f = *state;
  if(!f || !f->pack_dir[0]) skip();
  TR_STEP("the shipped pack loads and names its version and spectral table");
  /* deliberately not REQUIRE_PACK: a pack that is present but will not load is
     the one failure this suite must report, not skip past */
  assert_non_null(f->pack);
  assert_non_null(sf_pack_version(f->pack));
  assert_non_null(sf_pack_lut_id(f->pack));
  assert_true(sf_pack_lut_hash(f->pack) != 0);
  TR_DEBUG("version=%s lut=%s hash=%u", sf_pack_version(f->pack), sf_pack_lut_id(f->pack),
           sf_pack_lut_hash(f->pack));
}

static void test_pack_ships_both_a_film_and_a_paper(void **state)
{
  fixture_t *f = *state;
  REQUIRE_PACK(f);
  TR_STEP("the pack carries at least one filming and one printing profile");
  assert_non_null(f->film);
  assert_non_null(f->print);
  assert_non_null(sf_profile_stage(f->film));
  assert_non_null(sf_profile_stage(f->print));
  assert_string_equal(sf_profile_stage(f->film), "filming");
  assert_string_equal(sf_profile_stage(f->print), "printing");
}

/* upstream test_profiles.py::test_profile_data_shapes_are_consistent -- the
   array shapes it checks are fixed by the struct here, so what is left to check
   is that the arrays actually carry data and that the optional blocks are
   either absent or complete. */
static void test_profile_arrays_are_populated_and_finite(void **state)
{
  fixture_t *f = *state;
  REQUIRE_FILM(f);
  TR_STEP("profile spectra and curves are fully populated and finite");
  const sf_profile_t *profiles[2] = { f->film, f->print };
  for(int i = 0; i < 2; i++)
  {
    const sf_profile_t *p = profiles[i];
    if(!p) continue;
    /* A profile carries NaN (null in the JSON, see upstream's _json_safe)
       wherever a stock was never measured -- Portra 400's base density is
       undefined at both ends of the visible range, and its channel densities
       likewise. So the check is not that every sample is finite but that none
       is infinite, which would be a parse or arithmetic fault, not a
       gap, and that real data is present at all. */
    int sens_nonzero = 0, dens_finite = 0, base_finite = 0;
    for(int l = 0; l < SF_NWL; l++)
      for(int c = 0; c < 3; c++)
      {
        assert_false(isinf(p->channel_density[l][c]));
        assert_false(isinf(p->log_sensitivity[l][c]));
        if(isfinite(p->channel_density[l][c])) dens_finite++;
        if(isfinite(p->log_sensitivity[l][c]) && p->log_sensitivity[l][c] != 0.0) sens_nonzero++;
      }
    for(int l = 0; l < SF_NWL; l++)
    {
      assert_false(isinf(p->base_density[l]));
      if(isfinite(p->base_density[l])) base_finite++;
    }
    TR_DEBUG("%s: %d sensitivity, %d channel density, %d base density samples",
             sf_profile_stock(p) ? sf_profile_stock(p) : "(unnamed)", sens_nonzero, dens_finite,
             base_finite);
    assert_true(sens_nonzero > 0);
    assert_true(dens_finite > 0);
    assert_true(base_finite > 0);

    /* the log-exposure grid must be strictly increasing: everything downstream
       indexes it as uniform and would silently misread it otherwise */
    for(int k = 1; k < SF_NLE; k++) assert_true(p->log_exposure[k] > p->log_exposure[k - 1]);
    for(int k = 0; k < SF_NLE; k++)
      for(int c = 0; c < 3; c++) assert_true(isfinite(p->density_curves[k][c]));

    /* optional blocks are all-or-nothing */
    assert_true(p->window_n == 0 || p->window_n == 4);
    assert_true(p->surface_n == 0 || p->surface_n == SF_SURFACE_NCOEF);
  }
}

/* upstream test_profiles.py checks the adaptation surface is either empty or
   three channels wide. Here the interesting half is which stocks carry one:
   a paper never does, and the module's adaptation switch is a no-op for it. */
static void test_print_profile_carries_no_adaptation_surface(void **state)
{
  fixture_t *f = *state;
  REQUIRE_PACK(f);
  if(!f->print) skip();
  TR_STEP("print stocks carry no sensitivity adaptation surface");
  assert_int_equal(f->print->surface_n, 0);
}

static void test_film_target_print_resolves_to_a_paper(void **state)
{
  fixture_t *f = *state;
  REQUIRE_FILM(f);
  if(!f->film) skip();
  TR_STEP("a film's target print names a paper the pack actually ships");
  const char *target = sf_profile_target_print(f->film);
  if(!target || !target[0]) skip(); /* positive stocks name none */
  TR_DEBUG("%s targets %s", sf_profile_stock(f->film), target);

  sf_profile_t *paper = _load_stock(f->pack_dir, target, TRUE);
  assert_non_null(paper);
  assert_non_null(sf_profile_stock(paper));
  assert_string_equal(sf_profile_stock(paper), target);
  sf_profile_free(paper);
}

/*
 * TEST FUNCTIONS: enlarger filters
 * (upstream tests/test_enlarger_filters.py)
 */

/* upstream test_color_enlarger_cc_scale_matches_density_definition: a CC value
   is defined as density, so the deepest attenuation a filter reaches is
   10^(-cc/100) of the source. */
static void test_dichroic_cc_scale_matches_the_density_definition(void **state)
{
  fixture_t *f = *state;
  REQUIRE_PACK(f);
  const double *filters = g_hash_table_lookup(f->pack->dichroics, "custom");
  assert_non_null(filters);
  TR_STEP("a CC value attenuates by its own density, 10^(-cc/100)");

  const double cc_values[] = { 30.0, 60.0, 100.0 };
  for(size_t i = 0; i < sizeof(cc_values) / sizeof(cc_values[0]); i++)
  {
    double flat[SF_NWL], out[SF_NWL];
    for(int l = 0; l < SF_NWL; l++) flat[l] = 1.0;
    const double cc[3] = { 0.0, 0.0, cc_values[i] };
    apply_dichroic_cc(out, flat, filters, cc);

    double lowest = INFINITY;
    for(int l = 0; l < SF_NWL; l++) lowest = fmin(lowest, out[l]);
    const double expected = pow(10.0, -cc_values[i] / 100.0);
    TR_DEBUG("cc=%.0f min=%e expected=%e", cc_values[i], lowest, expected);
    assert_double_close(lowest, expected, 1e-3);
  }
}

/* upstream test_color_enlarger_cc_filters_target_expected_spectral_bands:
   cyan takes out red, magenta green, yellow blue, and each leaves the other
   two bands substantially alone. */
static void test_dichroic_filters_attenuate_their_own_band(void **state)
{
  fixture_t *f = *state;
  REQUIRE_PACK(f);
  const double *filters = g_hash_table_lookup(f->pack->dichroics, "custom");
  assert_non_null(filters);
  TR_STEP("each dichroic takes out its own band and spares the others");

  /* SF_NWL samples from 380 nm in 5 nm steps; bands 0 = blue, 1 = green, 2 = red */
  for(int filter = 0; filter < 3; filter++)
  {
    double flat[SF_NWL], out[SF_NWL];
    for(int l = 0; l < SF_NWL; l++) flat[l] = 1.0;
    double cc[3] = { 0.0, 0.0, 0.0 };
    cc[filter] = 100.0;
    apply_dichroic_cc(out, flat, filters, cc);

    double sum[3] = { 0.0, 0.0, 0.0 };
    int n[3] = { 0, 0, 0 };
    for(int l = 0; l < SF_NWL; l++)
    {
      const double wl = 380.0 + 5.0 * l;
      int band = -1;
      if(wl < 480.0)
        band = 0;
      else if(wl >= 500.0 && wl < 600.0)
        band = 1;
      else if(wl >= 620.0)
        band = 2;
      if(band < 0) continue;
      sum[band] += out[l];
      n[band]++;
    }
    /* cyan (index 0) attenuates red, magenta green, yellow blue */
    const int hit = 2 - filter;
    for(int b = 0; b < 3; b++)
    {
      assert_true(n[b] > 0);
      const double mean = sum[b] / n[b];
      TR_DEBUG("filter %d band %d mean=%e", filter, b, mean);
      if(b == hit)
        assert_true(mean < 0.2);
      else
        assert_true(mean > 0.7);
    }
  }
}

static void test_neutral_filters_are_calibrated_for_the_shipped_pair(void **state)
{
  fixture_t *f = *state;
  REQUIRE_FILM(f);
  if(!f->film || !f->print) skip();
  TR_STEP("the pack knows the neutral filtration for its own film/paper pair");
  double cmy[3] = { -1.0, -1.0, -1.0 };
  const gboolean have = sf_pack_neutral_filters(f->pack, sf_profile_stock(f->print), "TH-KG3",
                                                sf_profile_stock(f->film), cmy);
  if(!have) skip(); /* not every pairing is calibrated */
  TR_DEBUG("cmy = %.1f %.1f %.1f", cmy[0], cmy[1], cmy[2]);
  for(int c = 0; c < 3; c++)
  {
    assert_true(isfinite(cmy[c]));
    /* Kodak CC units: a dichroic head cannot dial past 200 */
    assert_true(cmy[c] >= 0.0 && cmy[c] <= 200.0);
  }
}

/*
 * TEST FUNCTIONS: the assembled pipeline
 * (upstream tests/test_pipeline_smoke.py)
 */

static void test_pipeline_renders_valid_output(void **state)
{
  fixture_t *f = *state;
  REQUIRE_FILM(f);
  if(!f->print) skip();
  TR_STEP("the full chain returns finite, bounded output for ordinary input");
  sf_sim_t *sim = _build(f, NULL);
  assert_non_null(sim);

  enum { N = 16 };
  float in[N * 3], out[N * 3];
  for(int i = 0; i < N; i++)
  {
    const float level = 0.01f + 0.99f * (float)i / (float)(N - 1);
    in[i * 3 + 0] = level;
    in[i * 3 + 1] = level * 0.6f;
    in[i * 3 + 2] = level * 0.3f;
  }
  _render(sim, in, out, N);
  _assert_valid_output(out, N, TRUE);
  sf_sim_free(sim);
}

/* upstream: pure black, and a wildly over-range input, must not produce NaN */
static void test_pipeline_survives_extreme_input(void **state)
{
  fixture_t *f = *state;
  REQUIRE_FILM(f);
  if(!f->print) skip();
  TR_STEP("black and blown-out input stay finite and in range");
  sf_sim_t *sim = _build(f, NULL);
  assert_non_null(sim);

  enum { N = 4 };
  float in[N * 3], out[N * 3];
  _fill_grey(in, N, 0.0f);
  _render(sim, in, out, N);
  _assert_valid_output(out, N, TRUE);

  _fill_grey(in, N, 10000.0f);
  _render(sim, in, out, N);
  _assert_valid_output(out, N, TRUE);
  sf_sim_free(sim);
}

/* upstream test_uniform_gray_output_is_stable_and_artifact_free */
static void test_uniform_patch_renders_uniform_and_repeatably(void **state)
{
  fixture_t *f = *state;
  REQUIRE_FILM(f);
  if(!f->print) skip();
  TR_STEP("a flat patch renders flat, and twice over renders identically");
  sf_sim_t *sim = _build(f, NULL);
  assert_non_null(sim);

  enum { N = 9 };
  float in[N * 3], first[N * 3], second[N * 3];
  _fill_grey(in, N, 0.184f);
  _render(sim, in, first, N);
  _render(sim, in, second, N);
  _assert_valid_output(first, N, TRUE);

  for(int i = 0; i < N * 3; i++)
  {
    assert_true(first[i] == second[i]);        /* bit-identical: no hidden state */
    assert_double_close(first[i], first[i % 3], 1e-6);
  }
  sf_sim_free(sim);
}

/* upstream test_exposure_controls_behave_consistently, first half */
static void test_brighter_input_renders_brighter(void **state)
{
  fixture_t *f = *state;
  REQUIRE_FILM(f);
  if(!f->print) skip();
  TR_STEP("output brightness follows input brightness, monotonically");
  sf_sim_t *sim = _build(f, NULL);
  assert_non_null(sim);

  const float levels[] = { 0.02f, 0.05f, 0.18f, 0.5f, 0.9f };
  enum { N = 4 };
  double previous = -INFINITY;
  for(size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++)
  {
    float in[N * 3], out[N * 3];
    _fill_grey(in, N, levels[i]);
    _render(sim, in, out, N);
    const double mean = _mean(out, N);
    TR_DEBUG("level=%.2f mean=%e", levels[i], mean);
    assert_true(mean > previous);
    previous = mean;
  }
  sf_sim_free(sim);
}

static void _cfg_plus_two_ev(sf_sim_params_t *p)
{
  p->exposure_comp_ev = 2.0;
  p->print_exposure_compensation = false;
}
static void _cfg_minus_two_ev(sf_sim_params_t *p)
{
  p->exposure_comp_ev = -2.0;
  p->print_exposure_compensation = false;
}
static void _cfg_no_print_comp(sf_sim_params_t *p)
{
  p->print_exposure_compensation = false;
}

/* upstream test_exposure_controls_behave_consistently, second half */
static void test_exposure_compensation_moves_the_render(void **state)
{
  fixture_t *f = *state;
  REQUIRE_FILM(f);
  if(!f->print) skip();
  TR_STEP("exposure compensation brightens and darkens as its sign says");
  enum { N = 4 };
  float in[N * 3], out[N * 3];
  _fill_grey(in, N, 0.18f);

  double means[3];
  void (*configs[3])(sf_sim_params_t *) = { _cfg_minus_two_ev, _cfg_no_print_comp, _cfg_plus_two_ev };
  for(int i = 0; i < 3; i++)
  {
    sf_sim_t *sim = _build(f, configs[i]);
    assert_non_null(sim);
    _render(sim, in, out, N);
    _assert_valid_output(out, N, TRUE);
    means[i] = _mean(out, N);
    sf_sim_free(sim);
  }
  TR_DEBUG("-2EV=%e 0EV=%e +2EV=%e", means[0], means[1], means[2]);
  assert_true(means[0] < means[1]);
  assert_true(means[1] < means[2]);
}

static void _cfg_scan_film(sf_sim_params_t *p)
{
  p->scan_film = true;
}

static void test_scan_film_builds_without_a_paper(void **state)
{
  fixture_t *f = *state;
  REQUIRE_FILM(f);
  TR_STEP("scanning the negative directly needs no print stock at all");
  sf_sim_t *sim = _build(f, _cfg_scan_film);
  assert_non_null(sim);
  assert_int_equal(sim->has_print, 0);

  enum { N = 4 };
  float in[N * 3], out[N * 3];
  _fill_grey(in, N, 0.18f);
  _render(sim, in, out, N);
  _assert_valid_output(out, N, TRUE);
  sf_sim_free(sim);
}

static void _cfg_lut_quality(sf_sim_params_t *p)
{
  p->lut_steps = 33;
}

/* upstream tests/test_lut_mode.py: the tabulated path is an approximation of
   the exact spectral one and has to stay close to it. A 33-step table on the
   0.3.3 pack lands within 6e-4 of exact across a colour ramp; the bound below
   leaves an order of magnitude of headroom for other stocks and other
   platforms' rounding, because what would be a regression is the two paths
   diverging visibly, not the interpolation error moving in its last digits. */
static void test_lut_path_tracks_exact_spectral(void **state)
{
  fixture_t *f = *state;
  REQUIRE_FILM(f);
  if(!f->print) skip();
  TR_STEP("the quality LUT stays close to the exact spectral path");
  enum { N = 8 };
  float in[N * 3], exact[N * 3], lut[N * 3];
  for(int i = 0; i < N; i++)
  {
    const float level = 0.02f + 0.9f * (float)i / (float)(N - 1);
    in[i * 3 + 0] = level;
    in[i * 3 + 1] = level * 0.75f;
    in[i * 3 + 2] = level * 0.45f;
  }

  sf_sim_t *sim = _build(f, NULL);
  assert_non_null(sim);
  _render(sim, in, exact, N);
  sf_sim_free(sim);

  sim = _build(f, _cfg_lut_quality);
  assert_non_null(sim);
  _render(sim, in, lut, N);
  sf_sim_free(sim);

  for(int i = 0; i < N * 3; i++)
  {
    TR_DEBUG("exact=%e lut=%e", exact[i], lut[i]);
    assert_double_close(lut[i], exact[i], 0.005);
  }
}

/*
 * MAIN
 */

int main(int argc,
         char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_pack_loads_and_identifies_itself),
    cmocka_unit_test(test_pack_ships_both_a_film_and_a_paper),
    cmocka_unit_test(test_profile_arrays_are_populated_and_finite),
    cmocka_unit_test(test_print_profile_carries_no_adaptation_surface),
    cmocka_unit_test(test_film_target_print_resolves_to_a_paper),
    cmocka_unit_test(test_dichroic_cc_scale_matches_the_density_definition),
    cmocka_unit_test(test_dichroic_filters_attenuate_their_own_band),
    cmocka_unit_test(test_neutral_filters_are_calibrated_for_the_shipped_pair),
    cmocka_unit_test(test_pipeline_renders_valid_output),
    cmocka_unit_test(test_pipeline_survives_extreme_input),
    cmocka_unit_test(test_uniform_patch_renders_uniform_and_repeatably),
    cmocka_unit_test(test_brighter_input_renders_brighter),
    cmocka_unit_test(test_exposure_compensation_moves_the_render),
    cmocka_unit_test(test_scan_film_builds_without_a_paper),
    cmocka_unit_test(test_lut_path_tracks_exact_spectral),
  };

  return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
