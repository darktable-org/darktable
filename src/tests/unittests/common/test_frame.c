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
 * cmocka unit tests for develop/frame.c: a frame lives as long as its
 * references, its rendition in its own format and profile is its pixels,
 * an RGB24 frame packs what dt_display_to_8bit() gives, a rendition is made
 * from the frame's pixels, whatever was asked for before, and profile
 * infos are made once per key, a failed one included. frame.c is included
 * for its static helpers
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <math.h>

#include <cmocka.h>

#include "develop/frame.c"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

#define W 7
#define H 3

typedef struct _fixture_t
{
  cmsHPROFILE srgb, rec2020;
  dt_color_encoding_t srgb_encoding, rec2020_encoding;
  float pixels[4 * W * H];
} _fixture_t;

// profiles made here with lcms, independently of colorspaces.c
static cmsHPROFILE _profile(const cmsCIExyY *white,
                            const cmsCIExyYTRIPLE *primaries,
                            cmsToneCurve *curve)
{
  cmsToneCurve *curves[3] = { curve, curve, curve };
  cmsHPROFILE profile = cmsCreateRGBProfile(white, primaries, curves);
  cmsFreeToneCurve(curve);
  return profile;
}

static dt_color_encoding_t _encoding(const dt_colorspaces_color_profile_type_t type,
                                     cmsHPROFILE profile)
{
  dt_color_encoding_t e;
  memset(&e, 0, sizeof(e));
  e.type = type;
  e.profile = profile;
  e.intent = DT_INTENT_PERCEPTUAL;
  e.headroom = 1.0f;
  return e;
}

static int _setup(void **state)
{
  _fixture_t *f = calloc(1, sizeof(_fixture_t));
  const cmsCIExyY d65 = { 0.3127, 0.3290, 1.0 };
  const cmsCIExyYTRIPLE rec709 = { { 0.64, 0.33, 1.0 }, { 0.30, 0.60, 1.0 },
                                   { 0.15, 0.06, 1.0 } };
  const cmsCIExyYTRIPLE rec2020 = { { 0.708, 0.292, 1.0 }, { 0.170, 0.797, 1.0 },
                                    { 0.131, 0.046, 1.0 } };
  const cmsFloat64Number srgb_trc[5] = { 2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92,
                                         0.04045 };
  f->srgb = _profile(&d65, &rec709, cmsBuildParametricToneCurve(NULL, 4, srgb_trc));
  f->rec2020 = _profile(&d65, &rec2020, cmsBuildGamma(NULL, 1.0));
  f->srgb_encoding = _encoding(DT_COLORSPACE_SRGB, f->srgb);
  f->rec2020_encoding = _encoding(DT_COLORSPACE_LIN_REC2020, f->rec2020);

  // values inside and outside [0, 1], halves of a code value, a NaN
  for(int k = 0; k < 4 * W * H; k++)
    f->pixels[k] = (k % 4 == 3) ? 1.0f : -0.25f + 1.5f * (k % 37) / 36.0f;
  f->pixels[0] = 127.5f / 255.0f;
  f->pixels[1] = NAN;
  f->pixels[2] = 2.0f;
  *state = f;
  return 0;
}

static int _teardown(void **state)
{
  _fixture_t *f = *state;
  dt_frame_cleanup();
  cmsCloseProfile(f->srgb);
  cmsCloseProfile(f->rec2020);
  free(f);
  return 0;
}

static dt_frame_t *_frame(const _fixture_t *f,
                          const dt_frame_format_t format)
{
  const dt_frame_geometry_t geometry = { .width = W, .height = H, .scale = 1.0f };
  return dt_frame_new(f->pixels, &geometry, format, &f->srgb_encoding, 5);
}

static void test_refcount(void **state)
{
  const _fixture_t *f = *state;
  dt_frame_t *frame = _frame(f, DT_FRAME_RGB24);
  assert_non_null(frame);
  assert_int_equal(dt_frame_made_for(frame), 5);

  // the frame holds its surface until its last reference goes
  cairo_surface_t *surface = cairo_surface_reference(dt_frame_surface(frame, NULL));
  assert_int_equal(cairo_surface_get_reference_count(surface), 2);
  assert_ptr_equal(dt_frame_ref(frame), frame);
  dt_frame_unref(frame);
  assert_int_equal(cairo_surface_get_reference_count(surface), 2);
  dt_frame_unref(frame);
  assert_int_equal(cairo_surface_get_reference_count(surface), 1);
  cairo_surface_destroy(surface);

  // ids are never reused
  dt_frame_t *a = _frame(f, DT_FRAME_FLOAT);
  dt_frame_t *b = _frame(f, DT_FRAME_FLOAT);
  assert_int_not_equal(dt_frame_id(a), dt_frame_id(b));
  dt_frame_unref(a);
  dt_frame_unref(b);

  dt_frame_unref(NULL);
  assert_null(dt_frame_ref(NULL));
}

static void test_identity_is_own_pixels(void **state)
{
  const _fixture_t *f = *state;
  // the same profile rendered for another headroom and intent means the same
  dt_color_encoding_t other = f->srgb_encoding;
  other.headroom = 4.0f;
  other.intent = DT_INTENT_RELATIVE_COLORIMETRIC;

  dt_frame_t *rgb24 = _frame(f, DT_FRAME_RGB24);
  cairo_surface_t *surface = dt_frame_surface(rgb24, NULL);
  assert_ptr_equal(cairo_image_surface_get_data(surface), dt_frame_pixels(rgb24));
  assert_ptr_equal(dt_frame_surface(rgb24, &other), surface);
  dt_frame_unref(rgb24);

  dt_frame_t *flt = _frame(f, DT_FRAME_FLOAT);
  assert_ptr_equal(dt_frame_float(flt, NULL), dt_frame_pixels(flt));
  assert_ptr_equal(dt_frame_float(flt, &other), dt_frame_pixels(flt));
  assert_memory_equal(dt_frame_pixels(flt), f->pixels, sizeof(f->pixels));
  dt_frame_unref(flt);
}

static void test_rgb24_packs_as_display_to_8bit(void **state)
{
  const _fixture_t *f = *state;
  dt_frame_t *frames[2] = { _frame(f, DT_FRAME_RGB24), _frame(f, DT_FRAME_FLOAT) };
  // the RGB24 frame's pixels, and the RGB24 rendition of a float frame
  const uint8_t *packed[2] = {
    dt_frame_pixels(frames[0]),
    cairo_image_surface_get_data(dt_frame_surface(frames[1], NULL)) };
  for(int n = 0; n < 2; n++)
  {
    assert_non_null(packed[n]);
    for(int k = 0; k < W * H; k++)
    {
      const float *in = f->pixels + 4 * k;
      const uint8_t *out = packed[n] + 4 * k;
      assert_int_equal(out[0], dt_display_to_8bit(in[2]));
      assert_int_equal(out[1], dt_display_to_8bit(in[1]));
      assert_int_equal(out[2], dt_display_to_8bit(in[0]));
      assert_int_equal(out[3], 0xff);
    }
  }
  assert_int_equal(packed[0][2], 128);
  dt_frame_unref(frames[0]);
  dt_frame_unref(frames[1]);
}

// sRGB to linear Rec.2020 by lcms, unbounded
static void _reference(const _fixture_t *f,
                       const float *in,
                       float *out)
{
  cmsHTRANSFORM xform = cmsCreateTransform(f->srgb, TYPE_RGB_FLT, f->rec2020, TYPE_RGB_FLT,
                                           INTENT_PERCEPTUAL, 0);
  for(int k = 0; k < W * H; k++) cmsDoTransform(xform, in + 4 * k, out + 3 * k, 1);
  cmsDeleteTransform(xform);
}

static void test_rendition_from_frame_pixels(void **state)
{
  const _fixture_t *f = *state;
  // finite values inside the range lcms's curves cover
  float pixels[4 * W * H];
  for(int k = 0; k < 4 * W * H; k++) pixels[k] = (k % 4 == 3) ? 1.0f : (k % 23) / 22.0f;
  float expected[3 * W * H];
  _reference(f, pixels, expected);

  const dt_frame_geometry_t geometry = { .width = W, .height = H, .scale = 1.0f };
  dt_frame_t *frame = dt_frame_new(pixels, &geometry, DT_FRAME_FLOAT, &f->srgb_encoding, 1);

  // an 8-bit rendition first: the float one must not come from it
  cairo_surface_t *surface = dt_frame_surface(frame, &f->rec2020_encoding);
  assert_non_null(surface);
  const float *converted = dt_frame_float(frame, &f->rec2020_encoding);
  assert_non_null(converted);
  assert_ptr_not_equal(converted, dt_frame_pixels(frame));
  // the same rendition when asked again
  assert_ptr_equal(dt_frame_float(frame, &f->rec2020_encoding), converted);

  const uint8_t *data = cairo_image_surface_get_data(surface);
  for(int k = 0; k < W * H; k++)
  {
    for(int c = 0; c < 3; c++)
    {
      // a quarter of a linear 8-bit step: finer than what came from 8 bits
      assert_float_equal(converted[4 * k + c], expected[3 * k + c], 0.25f / 255.0f);
      const int code = dt_display_to_8bit(expected[3 * k + c]);
      assert_true(abs((int)data[4 * k + 2 - c] - code) <= 1);
    }
    assert_float_equal(converted[4 * k + 3], 1.0f, 0.0f);
  }

  // the frame's pixels are untouched
  assert_memory_equal(dt_frame_pixels(frame), pixels, sizeof(pixels));
  dt_frame_unref(frame);
}

static void test_rendition_keeps_what_8_bits_cannot(void **state)
{
  const _fixture_t *f = *state;
  // linear Rec.2020 above reference white, shown in sRGB
  float pixels[4 * W * H];
  for(int k = 0; k < 4 * W * H; k++) pixels[k] = (k % 4 == 3) ? 1.0f : 2.0f;
  const dt_frame_geometry_t geometry = { .width = W, .height = H, .scale = 1.0f };
  dt_frame_t *frame =
    dt_frame_new(pixels, &geometry, DT_FRAME_FLOAT, &f->rec2020_encoding, 1);

  const float *flt = dt_frame_float(frame, &f->srgb_encoding);
  const uint8_t *data =
    cairo_image_surface_get_data(dt_frame_surface(frame, &f->srgb_encoding));
  for(int k = 0; k < W * H; k++)
    for(int c = 0; c < 3; c++)
    {
      // the float rendition is not clipped, the 8-bit one clips
      assert_true(flt[4 * k + c] > 1.0f);
      assert_int_equal(data[4 * k + c], 255);
    }
  dt_frame_unref(frame);

  // an RGB24 frame as floats: its bytes
  dt_frame_t *rgb24 = _frame(f, DT_FRAME_RGB24);
  const uint8_t *bytes = dt_frame_pixels(rgb24);
  const float *unpacked = dt_frame_float(rgb24, NULL);
  for(int k = 0; k < W * H; k++)
  {
    assert_float_equal(unpacked[4 * k + 0], bytes[4 * k + 2] / 255.0f, 0.0f);
    assert_float_equal(unpacked[4 * k + 1], bytes[4 * k + 1] / 255.0f, 0.0f);
    assert_float_equal(unpacked[4 * k + 2], bytes[4 * k + 0] / 255.0f, 0.0f);
    assert_float_equal(unpacked[4 * k + 3], 1.0f, 0.0f);
  }
  dt_frame_unref(rgb24);
}

static void test_pq_rendition_above_white(void **state)
{
  const _fixture_t *f = *state;
  // linear 2, twice reference white, as 8-bit PQ: PQ encodes it within 8
  // bits, so the byte is PQ(2), not the PQ(1) of a value clipped before
  float pixels[4 * W * H];
  for(int k = 0; k < 4 * W * H; k++) pixels[k] = (k % 4 == 3) ? 1.0f : 2.0f;
  const dt_frame_geometry_t geometry = { .width = W, .height = H, .scale = 1.0f };
  dt_frame_t *frame =
    dt_frame_new(pixels, &geometry, DT_FRAME_FLOAT, &f->rec2020_encoding, 1);
  const dt_color_encoding_t pq = _encoding(DT_COLORSPACE_PQ_REC2020, f->rec2020);
  const int transfer = dt_colorspaces_hdr_transfer(DT_COLORSPACE_PQ_REC2020);
  dt_aligned_pixel_t two = { 2.0f, 2.0f, 2.0f, 1.0f }, one = { 1.0f, 1.0f, 1.0f, 1.0f };
  dt_hdr_encode(two, transfer);
  dt_hdr_encode(one, transfer);
  assert_true(dt_display_to_8bit(two[0]) > dt_display_to_8bit(one[0]));

  const uint8_t *data = cairo_image_surface_get_data(dt_frame_surface(frame, &pq));
  for(int k = 0; k < W * H; k++)
    for(int c = 0; c < 3; c++)
      assert_true(abs((int)data[4 * k + c] - dt_display_to_8bit(two[0])) <= 1);
  dt_frame_unref(frame);
}

static void test_infos_made_once(void **state)
{
  const _fixture_t *f = *state;
  // a key that gives an info: made once, found after
  const dt_iop_order_iccprofile_info_t *srgb = _info(&f->srgb_encoding, DT_INTENT_SATURATION);
  assert_non_null(srgb);
  const guint n = g_list_length(_infos);
  assert_ptr_equal(_info(&f->srgb_encoding, DT_INTENT_SATURATION), srgb);
  assert_int_equal(g_list_length(_infos), n);

  // a key whose info could not be made: found, NULL, not made again
  dt_color_encoding_t missing = f->srgb_encoding;
  missing.type = DT_COLORSPACE_FILE;
  g_strlcpy(missing.filename, "/nonexistent/display.icc", sizeof(missing.filename));
  _info_t *failed = g_malloc0(sizeof(_info_t));
  failed->type = missing.type;
  g_strlcpy(failed->filename, missing.filename, sizeof(failed->filename));
  failed->profile = missing.profile;
  failed->intent = DT_INTENT_ABSOLUTE_COLORIMETRIC;
  _infos = g_list_prepend(_infos, failed);
  assert_null(_info(&missing, DT_INTENT_ABSOLUTE_COLORIMETRIC));
  assert_null(_info(&missing, DT_INTENT_ABSOLUTE_COLORIMETRIC));
  assert_int_equal(g_list_length(_infos), n + 1);

  // the profile of a FILE encoding lives without its file: an info is made
  assert_non_null(_info(&missing, DT_INTENT_RELATIVE_COLORIMETRIC));
  assert_int_equal(g_list_length(_infos), n + 2);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_refcount),
    cmocka_unit_test(test_identity_is_own_pixels),
    cmocka_unit_test(test_rgb24_packs_as_display_to_8bit),
    cmocka_unit_test(test_rendition_from_frame_pixels),
    cmocka_unit_test(test_rendition_keeps_what_8_bits_cannot),
    cmocka_unit_test(test_pq_rendition_above_white),
    cmocka_unit_test(test_infos_made_once),
  };
  return cmocka_run_group_tests(tests, _setup, _teardown);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
