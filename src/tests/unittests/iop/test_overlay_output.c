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
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>

#define dt_dev_image _test_render
#define dt_image_exists _test_image_exists
#include "iop/overlay.c"
#undef dt_dev_image
#undef dt_image_exists

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static int _renders;
static int _test_tags(void) { return 1; }
static dt_dev_image_output_t _rendered_output;

gboolean _test_image_exists(const dt_imgid_t imgid) { return TRUE; }

void _test_render(const dt_imgid_t imgid, const size_t width, const size_t height,
                  const int history_end, uint8_t **buf, float *scale,
                  size_t *buf_width, size_t *buf_height, dt_dev_zoom_pos_t zoom_pos,
                  const int snapshot_id, GList *module_filter_out, const int devid,
                  const gboolean finalscale, const gboolean want_float,
                  const dt_dev_image_output_t *output)
{
  _renders++;
  _rendered_output = output ? *output : (dt_dev_image_output_t){ .type = DT_COLORSPACE_NONE };
  assert_int_equal(g_list_find_custom(module_filter_out, "colorout", (GCompareFunc)strcmp) != NULL,
                   output == NULL);
  g_list_free(module_filter_out);
  assert_true(finalscale);
  *buf_width = *buf_height = 4;
  *buf = dt_alloc_aligned(16 * (want_float ? 4 * sizeof(float) : sizeof(uint32_t)));
  const float red = output && output->type == DT_COLORSPACE_DISPLAY_TRANSPORT ? 0.25f : 0.75f;
  for(int i = 0; i < 16; i++)
  {
    if(want_float)
    {
      float *pixel = (float *)*buf + 4 * i;
      pixel[0] = red;
      pixel[1] = pixel[2] = 0.5f;
      pixel[3] = 1.0f;
    }
    else
      ((uint32_t *)*buf)[i] = 0xff008080u | ((uint32_t)(red * 255.0f) << 16);
  }
}

static void _render_overlay(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                            const gboolean legacy, const float red)
{
  const dt_iop_roi_t roi = { .width = 4, .height = 4, .scale = 1.0f };
  if(legacy)
  {
    int stride = 0;
    guint8 *result = _get_overlay_argb(module, piece, &roi, &roi, &stride);
    assert_non_null(result);
    assert_int_equal(result[stride + 4 + 2], (int)(red * 255.0f));
    g_free(result);
  }
  else
  {
    float *result = _get_overlay_rgba_f(module, piece, &roi, &roi);
    assert_non_null(result);
    assert_float_equal(result[4 * 5], red, 1e-6f);
    dt_free_align(result);
  }
}

static void _test_cache_encoding(void **state)
{
  dt_develop_t dev = { .image_storage = { .id = 1, .width = 4, .height = 4 } };
  dt_develop_t export_dev = { .image_storage = { .id = 1, .width = 4, .height = 4 } };
  darktable.develop = &dev;
  darktable.num_openmp_threads = 1;
  dt_pthread_mutex_init(&darktable.plugin_threadsafe);
  dt_iop_overlay_global_data_t global = { 0 };
  dt_pthread_recursive_mutex_init(&global.overlay_threadsafe);
  dt_iop_overlay_params_t params = { .imgid = 2 };
  dt_iop_overlay_data_t data = { .imgid = 2, .scale = 100.0f, .alignment = 4 };
  dt_iop_module_so_t overlay_so = { .op = "overlay" }, colorout_so = { .op = "colorout" };
  dt_iop_module_t overlay = { .dev = &dev, .so = &overlay_so, .iop_order = 2,
                             .global_data = &global, .params = &params };
  dt_iop_module_t colorout = { .dev = &dev, .so = &colorout_so, .iop_order = 1,
                              .operation_tags = _test_tags };
  dt_iop_module_t focused = { .iop_order = 0, .operation_tags_filter = _test_tags };
  g_strlcpy(overlay.op, "overlay", sizeof(overlay.op));
  g_strlcpy(colorout.op, "colorout", sizeof(colorout.op));
  dt_iop_order_iccprofile_info_t encoding = { .type = DT_COLORSPACE_DISPLAY_TRANSPORT,
                                             .intent = DT_INTENT_PERCEPTUAL };
  dt_iop_order_iccprofile_info_t other_encoding = encoding;
  dt_dev_pixelpipe_t pipe = { .type = DT_DEV_PIXELPIPE_FULL, .output_encoding = &encoding,
                              .output_intent = DT_INTENT_RELATIVE_COLORIMETRIC,
                              .output_proof_type = DT_COLORSPACE_NONE };
  dt_dev_pixelpipe_iop_t node = { .module = &colorout, .pipe = &pipe, .enabled = TRUE };
  dt_dev_pixelpipe_iop_t piece = { .module = &overlay, .pipe = &pipe, .data = &data,
                                  .buf_in = { .width = 4, .height = 4 } };
  pipe.nodes = g_list_append(pipe.nodes, &node);
  pipe.nodes = g_list_append(pipe.nodes, &piece);
  for(int legacy = 0; legacy < 2; legacy++)
  {
    _clear_cache_entry(&overlay, 0);
    _renders = 0;
    pipe.output_encoding = &encoding;
    pipe.output_intent = DT_INTENT_RELATIVE_COLORIMETRIC;
    pipe.output_proof_mode = DT_PROFILE_NORMAL;
    pipe.output_proof_type = DT_COLORSPACE_NONE;
    pipe.output_proof_filename[0] = '\0';
    pipe.type = DT_DEV_PIXELPIPE_FULL;
    overlay.dev = &dev;
    _render_overlay(&overlay, &piece, legacy, 0.25f);
    assert_int_equal(_renders, 1);
    assert_int_equal(_rendered_output.intent, DT_INTENT_RELATIVE_COLORIMETRIC);
    pipe.output_encoding = &other_encoding;
    _render_overlay(&overlay, &piece, legacy, 0.25f);
    assert_int_equal(_renders, 1);

    // export uses the same image's shared cache slot but a different encoding
    overlay.dev = &export_dev;
    pipe.type = DT_DEV_PIXELPIPE_EXPORT;
    other_encoding.type = DT_COLORSPACE_SRGB;
    _render_overlay(&overlay, &piece, legacy, 0.75f);
    assert_int_equal(_renders, 2);
    pipe.type = DT_DEV_PIXELPIPE_FULL;
    overlay.dev = &dev;
    other_encoding.type = DT_COLORSPACE_DISPLAY_TRANSPORT;
    _render_overlay(&overlay, &piece, legacy, 0.25f);
    assert_int_equal(_renders, 3);
    pipe.output_intent = DT_INTENT_ABSOLUTE_COLORIMETRIC;
    _render_overlay(&overlay, &piece, legacy, 0.25f);
    assert_int_equal(_renders, 4);
    pipe.output_proof_mode = DT_PROFILE_SOFTPROOF;
    pipe.output_proof_type = DT_COLORSPACE_FILE;
    g_strlcpy(pipe.output_proof_filename, "proof.icc", sizeof(pipe.output_proof_filename));
    _render_overlay(&overlay, &piece, legacy, 0.25f);
    assert_int_equal(_renders, 5);
    assert_string_equal(_rendered_output.proof_filename, "proof.icc");
    g_strlcpy(pipe.output_proof_filename, "other-proof.icc", sizeof(pipe.output_proof_filename));
    _render_overlay(&overlay, &piece, legacy, 0.25f);
    assert_int_equal(_renders, 6);
    other_encoding.type = DT_COLORSPACE_FILE;
    g_strlcpy(other_encoding.filename, "output.icc", sizeof(other_encoding.filename));
    _render_overlay(&overlay, &piece, legacy, 0.75f);
    assert_int_equal(_renders, 7);
    g_strlcpy(other_encoding.filename, "other-output.icc", sizeof(other_encoding.filename));
    _render_overlay(&overlay, &piece, legacy, 0.75f);
    assert_int_equal(_renders, 8);
    other_encoding.type = DT_COLORSPACE_DISPLAY_TRANSPORT;
    other_encoding.filename[0] = '\0';
    _render_overlay(&overlay, &piece, legacy, 0.25f);
    assert_int_equal(_renders, 9);

    node.enabled = FALSE;
    _render_overlay(&overlay, &piece, legacy, 0.75f);
    assert_int_equal(_renders, 10);
    assert_int_equal(_rendered_output.type, DT_COLORSPACE_NONE);
    node.enabled = TRUE;
    pipe.nodes = g_list_reverse(pipe.nodes);
    _render_overlay(&overlay, &piece, legacy, 0.75f);
    assert_int_equal(_renders, 10);
    pipe.nodes = g_list_reverse(pipe.nodes);
    dev.gui_module = &focused;
    _render_overlay(&overlay, &piece, legacy, 0.75f);
    assert_int_equal(_renders, 10);
    dev.gui_module = NULL;
  }
  _clear_cache_entry(&overlay, 0);
  g_list_free(pipe.nodes);
  dt_pthread_mutex_destroy(&global.overlay_threadsafe);
  dt_pthread_mutex_destroy(&darktable.plugin_threadsafe);
  darktable.develop = NULL;
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = { cmocka_unit_test(_test_cache_encoding) };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
