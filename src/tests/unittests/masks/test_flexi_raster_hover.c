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

// What hovering a raster element's row paints on the canvas.
//
// A raster element has no shape, so with the masks shown on the canvas its
// only presence is a tint over the area its source's mask covers, drawn while
// its row is hovered in the panel. The group draws every member through
// post_expose(cr, zoom_scale, gui, index, num_points), and posing the hovered
// member as the selected one is all the hover does. That makes two things easy
// to get wrong without any build or run noticing:
//
//   - the parameters. index and num_points are both ints. Read the other way
//     round, the element compares the hovered member against its own point
//     count (always 1) and draws only when it happens to sit second, so the
//     hover does nothing and nothing complains. The raster here sits third in
//     its group for that reason.
//   - the mask's frame. The mask belongs to the source's output, so the tint
//     must show it where it lies on the preview and only there.
//
// The preview pipe is built by hand, as in test_flexi_raster_prune: one
// source module, its piece holding a known mask, and a group of three members
// the last of which is the raster element. The draw lands on a cairo image
// surface, whose pixels are what the cases look at.

#include "flexi_fixture.h"

#include "develop/pixelpipe_hb.h"

#include <cairo.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>

#define SOURCE_OP "exposure"
#define RASTER_ID 7
#define MASK_W 40
#define MASK_H 30
// the raster element's place in its group: not 1, the number of its points
#define RASTER_INDEX 2

typedef struct
{
  dt_develop_t dev;
  dt_iop_module_so_t source_so;
  dt_iop_module_t source;
  dt_dev_pixelpipe_t pipe;
  dt_dev_pixelpipe_iop_t piece;
  dt_develop_blend_params_t source_bp;
  dt_masks_form_gui_t gui;
  dt_masks_form_t *group;
  dt_masks_form_t *raster;
  cairo_surface_t *surface;
  cairo_t *cr;
} bench_t;

static bench_t B;

static int _source_flags(void)
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_WRITE_RASTER;
}

static dt_masks_point_group_t *_member(const dt_mask_id_t formid)
{
  dt_masks_point_group_t *pt = calloc(1, sizeof(dt_masks_point_group_t));
  pt->formid = formid;
  pt->parentid = B.group->formid;
  pt->state = DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW;
  pt->opacity = 1.0f;
  pt->group_opacity = 1.0f;
  return pt;
}

/* The source stored a mask that is 1 on the left half of its output and 0 on
   the right, at the preview's own size, so the preview maps onto it one to
   one. The group holds two circles and then the raster element, hovered. */
static void _bench_init(void)
{
  memset(&B, 0, sizeof(B));
  dt_masks_pixel_cache_cleanup();

  darktable.develop = &B.dev;
  dt_pthread_recursive_mutex_init(&B.dev.history_mutex);
  dt_pthread_mutex_init(&B.pipe.busy_mutex);

  g_strlcpy(B.source_so.op, SOURCE_OP, sizeof(B.source_so.op));
  B.source.so = &B.source_so;
  B.source.dev = &B.dev;
  B.source.flags = _source_flags;
  B.source.iop_order = 100;
  g_strlcpy(B.source.op, SOURCE_OP, sizeof(B.source.op));
  B.dev.iop = g_list_append(NULL, &B.source);

  B.pipe.iop = g_list_append(NULL, &B.source);
  B.pipe.iscale = 1.0f;
  B.pipe.processed_width = MASK_W;
  B.pipe.processed_height = MASK_H;
  B.pipe.backbuf_hash = 1;
  B.dev.preview_pipe = &B.pipe;

  B.piece.module = &B.source;
  B.piece.pipe = &B.pipe;
  B.piece.enabled = TRUE;
  B.piece.blendop_data = &B.source_bp;
  B.piece.processed_roi_out = (dt_iop_roi_t){ 0, 0, MASK_W, MASK_H, 1.0f };
  B.piece.raster_masks = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                               dt_free_align_ptr);
  float *mask = dt_alloc_align_float((size_t)MASK_W * MASK_H);
  for(int y = 0; y < MASK_H; y++)
    for(int x = 0; x < MASK_W; x++) mask[y * MASK_W + x] = x < MASK_W / 2 ? 1.0f : 0.0f;
  g_hash_table_insert(B.piece.raster_masks, GINT_TO_POINTER(RASTER_ID), mask);
  B.pipe.nodes = g_list_append(NULL, &B.piece);

  B.raster = dt_masks_create(DT_MASKS_RASTER);
  B.raster->formid = 103;
  dt_masks_point_raster_t *rp = calloc(1, sizeof(dt_masks_point_raster_t));
  g_strlcpy(rp->source, SOURCE_OP, sizeof(rp->source));
  rp->id = RASTER_ID;
  B.raster->points = g_list_append(NULL, rp);

  B.group = dt_masks_create(DT_MASKS_GROUP);
  B.group->formid = 100;
  for(dt_mask_id_t id = 101; id <= 102; id++)
  {
    dt_masks_form_t *other = dt_masks_create(DT_MASKS_PARAMETRIC);
    other->formid = id;
    B.dev.forms = g_list_append(B.dev.forms, other);
    B.group->points = g_list_append(B.group->points, _member(id));
  }
  B.group->points = g_list_append(B.group->points, _member(B.raster->formid));
  B.dev.forms = g_list_append(B.dev.forms, B.raster);
  B.dev.forms = g_list_append(B.dev.forms, B.group);
  B.dev.form_visible = B.group;

  dt_masks_init_form_gui(&B.gui);
  B.gui.group_selected = RASTER_INDEX;
  B.gui.form_selected = TRUE;

  B.surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, MASK_W, MASK_H);
  B.cr = cairo_create(B.surface);
}

static void _bench_cleanup(void)
{
  cairo_destroy(B.cr);
  cairo_surface_destroy(B.surface);
  g_list_free_full(B.dev.forms, (GDestroyNotify)dt_masks_free_form);
  g_list_free(B.dev.iop);
  g_list_free(B.pipe.iop);
  g_list_free(B.pipe.nodes);
  g_hash_table_destroy(B.piece.raster_masks);
  dt_pthread_mutex_destroy(&B.pipe.busy_mutex);
  dt_masks_pixel_cache_cleanup();
  darktable.develop = NULL;
}

/** the group's call for the raster element, as dt_group_events_post_expose
    makes it: its index in the group, then its number of points */
static void _draw(void)
{
  dt_masks_functions_raster.post_expose(B.cr, 1.0f, &B.gui, RASTER_INDEX,
                                        g_list_length(B.raster->points));
}

static uint32_t _pixel(const int x, const int y)
{
  cairo_surface_flush(B.surface);
  const unsigned char *row =
    cairo_image_surface_get_data(B.surface) + (size_t)y * cairo_image_surface_get_stride(B.surface);
  return ((const uint32_t *)row)[x];
}

static unsigned _alpha(const int x, const int y) { return _pixel(x, y) >> 24; }

static void _assert_untouched(void)
{
  for(int y = 0; y < MASK_H; y += 5)
    for(int x = 0; x < MASK_W; x += 5) assert_int_equal(_pixel(x, y), 0);
}

// ---------------------------------------------------------------------------

/* The floor: hover the row and the area the mask covers is tinted red, the
   rest is not. With the parameters read the wrong way round this is the case
   that stays blank. */
static void test_a_hovered_raster_tints_the_area_its_mask_covers(void **state)
{
  _bench_init();
  _draw();

  assert_true(_alpha(5, 15) > 0);
  // premultiplied red: nothing in green or blue
  assert_int_equal(_pixel(5, 15) & 0x0000ffff, 0);
  assert_true(((_pixel(5, 15) >> 16) & 0xff) > 0);
  assert_int_equal(_alpha(MASK_W - 5, 15), 0);
  _bench_cleanup();
}

/* The group calls every member's draw, and each one decides from the index it
   is handed whether it is the hovered one. */
static void test_another_member_hovered_draws_nothing(void **state)
{
  _bench_init();
  B.gui.group_selected = 0;
  _draw();
  _assert_untouched();

  B.gui.group_selected = -1;
  _draw();
  _assert_untouched();
  _bench_cleanup();
}

/* A member merely selected, as the panel's selection does, is posed without
   form_selected: only a hover draws. */
static void test_a_selected_but_not_hovered_raster_draws_nothing(void **state)
{
  _bench_init();
  B.gui.form_selected = FALSE;
  _draw();
  _assert_untouched();
  _bench_cleanup();
}

/* A pipe that is mid-run owns its buffers: the draw skips the frame instead of
   waiting for it or reading them, and draws once the pipe is idle again. A
   frame skipped must not be remembered as the answer. */
static void test_a_busy_preview_pipe_draws_nothing_until_idle(void **state)
{
  _bench_init();
  dt_pthread_mutex_lock(&B.pipe.busy_mutex);
  _draw();
  _assert_untouched();
  dt_pthread_mutex_unlock(&B.pipe.busy_mutex);

  _draw();
  assert_true(_alpha(5, 15) > 0);
  _bench_cleanup();
}

/* A source that is off, or in this pipe not at all, can still hold the mask it
   stored when it was on; the module applies nowhere then, and so does the
   tint. */
static void test_a_disabled_source_draws_nothing(void **state)
{
  _bench_init();
  B.piece.enabled = FALSE;
  _draw();
  _assert_untouched();
  _bench_cleanup();

  _bench_init();
  g_list_free(B.pipe.nodes);
  B.pipe.nodes = NULL;
  _draw();
  _assert_untouched();
  _bench_cleanup();
}

/* The element names a source instance that is not there. */
static void test_an_unresolved_source_draws_nothing(void **state)
{
  _bench_init();
  ((dt_masks_point_raster_t *)B.raster->points->data)->instance = 1;
  _draw();
  _assert_untouched();
  _bench_cleanup();
}

/* The source has not stored the mask the element names, as before its first
   run in this pipe. */
static void test_a_mask_not_stored_draws_nothing(void **state)
{
  _bench_init();
  g_hash_table_remove(B.piece.raster_masks, GINT_TO_POINTER(RASTER_ID));
  _draw();
  _assert_untouched();
  _bench_cleanup();
}

/* The tint is kept for the preview it was made for, so a row hovered through
   many redraws is built once; a new preview, which is a new mask, builds it
   again. */
static void test_the_tint_follows_the_preview(void **state)
{
  _bench_init();
  _draw();
  assert_true(_alpha(5, 15) > 0);

  // the source's mask moves to the right half, and the preview with it
  float *mask = g_hash_table_lookup(B.piece.raster_masks, GINT_TO_POINTER(RASTER_ID));
  for(int y = 0; y < MASK_H; y++)
    for(int x = 0; x < MASK_W; x++) mask[y * MASK_W + x] = x < MASK_W / 2 ? 0.0f : 1.0f;
  B.pipe.backbuf_hash = 2;

  cairo_set_operator(B.cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint(B.cr);
  cairo_set_operator(B.cr, CAIRO_OPERATOR_OVER);
  _draw();
  assert_int_equal(_alpha(5, 15), 0);
  assert_true(_alpha(MASK_W - 5, 15) > 0);
  _bench_cleanup();
}

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_a_hovered_raster_tints_the_area_its_mask_covers),
    cmocka_unit_test(test_another_member_hovered_draws_nothing),
    cmocka_unit_test(test_a_selected_but_not_hovered_raster_draws_nothing),
    cmocka_unit_test(test_a_busy_preview_pipe_draws_nothing_until_idle),
    cmocka_unit_test(test_a_disabled_source_draws_nothing),
    cmocka_unit_test(test_an_unresolved_source_draws_nothing),
    cmocka_unit_test(test_a_mask_not_stored_draws_nothing),
    cmocka_unit_test(test_the_tint_follows_the_preview),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
