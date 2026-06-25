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

/* Unit tests for the HDR exposure-bracket auto-alignment (common/hdr_alignment).
 *
 * The tests synthesise a Bayer mosaic, warp it by a known homography, and check
 * that the aligner recovers the motion and reduces the misalignment error; they
 * also check that the auto-reference probe ranks frames by feature richness.
 *
 * The assertions are robust to a build *without* OpenCV: in that case the public
 * API reports DT_HDR_ALIGN_DISABLED / a zero probe count and the tests verify the
 * documented graceful fall-through instead.
 */

#include <limits.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "common/hdr_alignment.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

#define TEST_W 512
#define TEST_H 512
// RGGB-style Bayer encoding compatible with the FC() formula; the scene is
// grayscale so the exact phase only exercises the same-color sampler.
#define TEST_FILTERS 0x94949494u

static unsigned int _rng = 12345u;
static float _frand(void)
{
  _rng = _rng * 1664525u + 1013904223u;
  return (float)((_rng >> 8) & 0xFFFFFF) / (float)0xFFFFFF;
}

// Deterministic textured scene: gentle gradients + scattered bright blobs and
// rectangles, which give SIFT plenty of distinctive keypoints.
static void _make_scene(float *s, int rich)
{
  _rng = 12345u;
  for(int y = 0; y < TEST_H; y++)
    for(int x = 0; x < TEST_W; x++)
      s[y * TEST_W + x] = 0.30f + 0.08f * sinf(x * 0.011f + y * 0.004f)
                          + 0.08f * cosf(y * 0.013f - x * 0.006f);
  if(!rich) return;
  for(int k = 0; k < 120; k++)
  {
    int rx = (int)(_frand() * TEST_W), ry = (int)(_frand() * TEST_H);
    int rw = 12 + (int)(_frand() * 26), rh = 12 + (int)(_frand() * 26);
    float ri = 0.5f + 0.4f * _frand();
    for(int y = ry; y < ry + rh && y < TEST_H; y++)
      for(int x = rx; x < rx + rw && x < TEST_W; x++) s[y * TEST_W + x] = ri;
  }
  for(int k = 0; k < 240; k++)
  {
    float cx = _frand() * TEST_W, cy = _frand() * TEST_H, r = 10.0f + _frand() * 18.0f;
    float inten = 0.4f + 0.5f * _frand();
    for(int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++)
      for(int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++)
      {
        if(x < 0 || x >= TEST_W || y < 0 || y >= TEST_H) continue;
        const float d = hypotf(x - cx, y - cy);
        if(d <= r)
        {
          const float v = inten * (1.0f - d / r);
          if(v > s[y * TEST_W + x]) s[y * TEST_W + x] = v;
        }
      }
  }
}

static float _bilin(const float *im, float x, float y)
{
  if(x < 0) x = 0;
  if(y < 0) y = 0;
  if(x > TEST_W - 1) x = TEST_W - 1;
  if(y > TEST_H - 1) y = TEST_H - 1;
  const int x0 = (int)x, y0 = (int)y;
  const int x1 = x0 < TEST_W - 1 ? x0 + 1 : x0, y1 = y0 < TEST_H - 1 ? y0 + 1 : y0;
  const float fx = x - x0, fy = y - y0;
  return (1 - fx) * (1 - fy) * im[y0 * TEST_W + x0] + fx * (1 - fy) * im[y0 * TEST_W + x1]
         + (1 - fx) * fy * im[y1 * TEST_W + x0] + fx * fy * im[y1 * TEST_W + x1];
}

// Alignment of a frame displaced by a known homography must either reduce the
// misalignment error (OpenCV build) or pass the frame through unchanged
// (no-OpenCV build).
static void test_hdr_align_reduces_misalignment(void **state)
{
  (void)state;
  float *ref = malloc(sizeof(float) * TEST_W * TEST_H);
  float *mov = malloc(sizeof(float) * TEST_W * TEST_H);
  float *out = malloc(sizeof(float) * TEST_W * TEST_H);
  assert_non_null(ref);
  assert_non_null(mov);
  assert_non_null(out);
  _make_scene(ref, 1);

  // Ground-truth ref->moving homography: rotation about the centre + translation.
  const double th = 1.2 * M_PI / 180.0, cx = TEST_W / 2.0, cy = TEST_H / 2.0;
  const double c = cos(th), s = sin(th), tx = 5.0, ty = -3.0;
  const double H0 = c, H1 = -s, H2 = cx - c * cx + s * cy + tx;
  const double H3 = s, H4 = c, H5 = cy - s * cx - c * cy + ty;
  const double det = H0 * H4 - H1 * H3;
  // inverse affine, to synthesise moving(u) = ref(Hinv * u)
  const double i0 = H4 / det, i1 = -H1 / det, i3 = -H3 / det, i4 = H0 / det;
  const double i2 = -(i0 * H2 + i1 * H5), i5 = -(i3 * H2 + i4 * H5);
  for(int y = 0; y < TEST_H; y++)
    for(int x = 0; x < TEST_W; x++)
      mov[y * TEST_W + x] = _bilin(ref, (float)(i0 * x + i1 * y + i2), (float)(i3 * x + i4 * y + i5));

  dt_hdr_align_t *a = dt_hdr_alignment_new(NULL);
  assert_non_null(a);
  assert_true(dt_hdr_alignment_set_reference(a, ref, TEST_W, TEST_H, TEST_FILTERS, NULL));

  dt_hdr_align_result_t info;
  const gboolean ok =
      dt_hdr_alignment_align_frame(a, mov, out, TEST_W, TEST_H, TEST_FILTERS, NULL, &info);

  if(info.status == DT_HDR_ALIGN_DISABLED)
  {
    // Built without OpenCV: the frame must be passed through untouched.
    assert_false(ok);
    assert_memory_equal(out, mov, sizeof(float) * TEST_W * TEST_H);
  }
  else
  {
    // Built with OpenCV: alignment must meaningfully reduce the misalignment.
    double e_before = 0.0, e_after = 0.0;
    for(int y = 48; y < TEST_H - 48; y++)
      for(int x = 48; x < TEST_W - 48; x++)
      {
        e_before += fabs(ref[y * TEST_W + x] - mov[y * TEST_W + x]);
        e_after += fabs(ref[y * TEST_W + x] - out[y * TEST_W + x]);
      }
    assert_true(ok);
    assert_true(e_after < 0.7 * e_before);
  }

  dt_hdr_alignment_free(a);
  free(ref);
  free(mov);
  free(out);
}

// CFA color of photosite (y, x) for TEST_FILTERS (RGGB), matching the FC()
// formula in hdr_alignment.c.  0 = R, 1 = G, 2 = B.
static int _fc_rggb(int y, int x)
{
  static const int rggb[4] = { 0, 1, 1, 2 };  // (0,0)R (0,1)G (1,0)G (1,1)B
  return rggb[((y & 1) << 1) | (x & 1)];
}

// Headline property: aligning a genuinely CFA-*modulated* mosaic must warp each
// colour channel without cross-talk.  We build a real Bayer mosaic (per-channel
// gains, so adjacent photosites differ by colour), displace it by a known
// homography, and require the aligner to recover the reference per photosite.
// A colour-blind (bilinear) warp would blend the strongly-different R/G/B taps
// and fail to reduce the error; the same-colour resampler must not.
static void test_hdr_align_cfa_modulated(void **state)
{
  (void)state;
  float *scene = malloc(sizeof(float) * TEST_W * TEST_H);
  float *ref = malloc(sizeof(float) * TEST_W * TEST_H);
  float *mov = malloc(sizeof(float) * TEST_W * TEST_H);
  float *out = malloc(sizeof(float) * TEST_W * TEST_H);
  assert_non_null(scene);
  assert_non_null(ref);
  assert_non_null(mov);
  assert_non_null(out);
  _make_scene(scene, 1);

  // Per-channel gains: make the mosaic strongly colour-modulated so any
  // channel cross-talk in the warp shows up as a large per-photosite error.
  const float gain[3] = { 1.6f, 1.0f, 0.55f };  // R, G, B

  // Ground-truth ref->moving homography: rotation about the centre + translation.
  const double th = 1.2 * M_PI / 180.0, cx = TEST_W / 2.0, cy = TEST_H / 2.0;
  const double c = cos(th), s = sin(th), tx = 5.0, ty = -3.0;
  const double H0 = c, H1 = -s, H2 = cx - c * cx + s * cy + tx;
  const double H3 = s, H4 = c, H5 = cy - s * cx - c * cy + ty;
  const double det = H0 * H4 - H1 * H3;
  const double i0 = H4 / det, i1 = -H1 / det, i3 = -H3 / det, i4 = H0 / det;
  const double i2 = -(i0 * H2 + i1 * H5), i5 = -(i3 * H2 + i4 * H5);

  for(int y = 0; y < TEST_H; y++)
    for(int x = 0; x < TEST_W; x++)
    {
      const float g = gain[_fc_rggb(y, x)];
      // reference mosaic: scene sampled at (x,y), modulated by CFA colour
      ref[y * TEST_W + x] = scene[y * TEST_W + x] * g;
      // moving mosaic: the shifted scene, sampled at the same photosite / colour
      const float sv = _bilin(scene, (float)(i0 * x + i1 * y + i2), (float)(i3 * x + i4 * y + i5));
      mov[y * TEST_W + x] = sv * g;
    }

  dt_hdr_align_t *a = dt_hdr_alignment_new(NULL);
  assert_non_null(a);
  assert_true(dt_hdr_alignment_set_reference(a, ref, TEST_W, TEST_H, TEST_FILTERS, NULL));

  dt_hdr_align_result_t info;
  const gboolean ok =
      dt_hdr_alignment_align_frame(a, mov, out, TEST_W, TEST_H, TEST_FILTERS, NULL, &info);

  if(info.status == DT_HDR_ALIGN_DISABLED)
  {
    assert_false(ok);
    assert_memory_equal(out, mov, sizeof(float) * TEST_W * TEST_H);
  }
  else
  {
    double e_before = 0.0, e_after = 0.0;
    for(int y = 48; y < TEST_H - 48; y++)
      for(int x = 48; x < TEST_W - 48; x++)
      {
        e_before += fabs(ref[y * TEST_W + x] - mov[y * TEST_W + x]);
        e_after += fabs(ref[y * TEST_W + x] - out[y * TEST_W + x]);
      }
    assert_true(ok);
    assert_true(e_after < 0.7 * e_before);
  }

  dt_hdr_alignment_free(a);
  free(scene);
  free(ref);
  free(mov);
  free(out);
}

// The auto-reference probe must rank a feature-rich frame above a flat one.
static void test_hdr_probe_ranks_richness(void **state)
{
  (void)state;
  float *flat = malloc(sizeof(float) * TEST_W * TEST_H);
  float *rich = malloc(sizeof(float) * TEST_W * TEST_H);
  assert_non_null(flat);
  assert_non_null(rich);
  for(int i = 0; i < TEST_W * TEST_H; i++) flat[i] = 0.3f;
  _make_scene(rich, 1);

  const int n_flat = dt_hdr_alignment_probe_features(flat, TEST_W, TEST_H);
  const int n_rich = dt_hdr_alignment_probe_features(rich, TEST_W, TEST_H);

  if(n_flat == 0 && n_rich == 0)
  {
    // Built without OpenCV: probing is unavailable; nothing to rank.
  }
  else
  {
    assert_true(n_rich > n_flat);
  }

  free(flat);
  free(rich);
}

// One reference, several differently-shifted moving frames.  The reference
// features are detected once (in set_reference) and reused by every align_frame,
// so this exercises the per-merge feature cache and confirms it is not corrupted
// across frames: every frame must still be corrected (OpenCV build) or passed
// through (no-OpenCV build).
static void test_hdr_align_reference_reused_across_frames(void **state)
{
  (void)state;
  float *ref = malloc(sizeof(float) * TEST_W * TEST_H);
  float *mov = malloc(sizeof(float) * TEST_W * TEST_H);
  float *out = malloc(sizeof(float) * TEST_W * TEST_H);
  assert_non_null(ref);
  assert_non_null(mov);
  assert_non_null(out);
  _make_scene(ref, 1);

  dt_hdr_align_t *a = dt_hdr_alignment_new(NULL);
  assert_non_null(a);
  assert_true(dt_hdr_alignment_set_reference(a, ref, TEST_W, TEST_H, TEST_FILTERS, NULL));

  const double shifts[3][2] = { { 4.0, -2.0 }, { -6.0, 3.0 }, { 2.0, 5.0 } };
  for(int t = 0; t < 3; t++)
  {
    const double tx = shifts[t][0], ty = shifts[t][1];
    for(int y = 0; y < TEST_H; y++)
      for(int x = 0; x < TEST_W; x++)
        mov[y * TEST_W + x] = _bilin(ref, (float)(x - tx), (float)(y - ty));

    dt_hdr_align_result_t info;
    const gboolean ok =
        dt_hdr_alignment_align_frame(a, mov, out, TEST_W, TEST_H, TEST_FILTERS, NULL, &info);

    if(info.status == DT_HDR_ALIGN_DISABLED)
    {
      assert_false(ok);
      assert_memory_equal(out, mov, sizeof(float) * TEST_W * TEST_H);
    }
    else
    {
      double e_before = 0.0, e_after = 0.0;
      for(int y = 48; y < TEST_H - 48; y++)
        for(int x = 48; x < TEST_W - 48; x++)
        {
          e_before += fabs(ref[y * TEST_W + x] - mov[y * TEST_W + x]);
          e_after += fabs(ref[y * TEST_W + x] - out[y * TEST_W + x]);
        }
      assert_true(ok);
      assert_true(e_after < 0.7 * e_before);
    }
  }

  dt_hdr_alignment_free(a);
  free(ref);
  free(mov);
  free(out);
}

int main(int argc, char *argv[])
{
  (void)argc;
  (void)argv;
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_hdr_align_reduces_misalignment),
    cmocka_unit_test(test_hdr_align_cfa_modulated),
    cmocka_unit_test(test_hdr_align_reference_reused_across_frames),
    cmocka_unit_test(test_hdr_probe_ranks_richness),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
