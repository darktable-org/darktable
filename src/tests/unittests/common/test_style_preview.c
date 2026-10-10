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
#include "common/darktable.h"
#include "imageio/imageio_common.h"

static cairo_surface_t *_test_preview(const dt_imgid_t imgid, const size_t width,
                                      const size_t height, const int history_end,
                                      const char *style_name);
static void _test_queue_draw(GtkWidget *widget);
#define dt_imageio_preview _test_preview
#define gtk_widget_queue_draw _test_queue_draw
#include "gui/styles_dialog.c"
#undef gtk_widget_queue_draw
#undef dt_imageio_preview

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static int _exports, _queued;
static gboolean _export_fails;

static cairo_surface_t *_test_preview(const dt_imgid_t imgid, const size_t width,
                                      const size_t height, const int history_end,
                                      const char *style_name)
{
  _exports++;
  if(_export_fails) return NULL;
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
  cairo_t *cr = cairo_create(surface);
  cairo_set_source_rgb(cr, 0.25, 0.5, 0.75);
  cairo_paint(cr);
  cairo_destroy(cr);
  return surface;
}

static void _test_queue_draw(GtkWidget *widget)
{
  _queued++;
}

static void _test_preview_draws(void **state)
{
  _exports = _queued = 0;
  cairo_surface_t *target = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 2, 2);
  cairo_t *cr = cairo_create(target);
  for(int attempt = 0; attempt < 2; attempt++)
  {
    _export_fails = attempt == 0;
    _preview_data_t data = { .imgid = 1, .first_draw = TRUE, .psize = 2 };
    assert_false(_preview_draw(NULL, cr, &data));
    assert_int_equal(_exports, attempt);
    assert_int_equal(_queued, attempt + 1);
    for(int draw = 0; draw < 4; draw++)
    {
      assert_false(_preview_draw(NULL, cr, &data));
      assert_int_equal(_exports, attempt + 1);
      assert_int_equal(_queued, attempt + 1);
    }
    if(_export_fails) assert_null(data.surface);
    else
    {
      assert_non_null(data.surface);
      cairo_surface_flush(target);
      const uint32_t pixel = *(uint32_t *)cairo_image_surface_get_data(target);
      assert_int_equal(pixel & 0x00ffffff, 0x004080bf);
      cairo_surface_destroy(data.surface);
    }
  }
  cairo_destroy(cr);
  cairo_surface_destroy(target);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = { cmocka_unit_test(_test_preview_draws) };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
