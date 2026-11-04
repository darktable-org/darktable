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

#include "gui/presenter.h"
#include "common/darktable.h"

#define WIDGET_KEY "dt-presenter"

// a frame converted for presentation, owned by the presenter
typedef struct _frame_t
{
  // identity of the source, never dereferenced
  const float *pixels;
  uint32_t id;
  int width, height;
  dt_display_output_t encoding;
  cairo_surface_t *surface;
} _frame_t;

typedef struct _image_t
{
  _frame_t *frame;
  // frame pixels to the user space the widget's draw started in
  cairo_matrix_t matrix;
  // the clip's extents in that space
  double clip[4];
  cairo_filter_t filter;
} _image_t;

typedef struct _widget_t
{
  GtkWidget *widget;
  cairo_matrix_t base;
  gboolean full;
  // the images of the draw in progress, or of the last one
  GArray *images;
  // those of the last draw GTK did not restrict to a damaged area,
  // for dt_presenter_keep()
  GArray *kept;
  // the frames either uses
  GPtrArray *frames;
} _widget_t;

static cairo_user_data_key_t _cr_key;

static void _frame_free(gpointer data)
{
  _frame_t *f = data;
  cairo_surface_destroy(f->surface);
  g_free(f);
}

static void _widget_reset(_widget_t *w)
{
  g_array_set_size(w->images, 0);
  g_array_set_size(w->kept, 0);
  g_ptr_array_set_size(w->frames, 0);
}

static void _widget_free(gpointer data)
{
  _widget_t *w = data;
  g_array_free(w->images, TRUE);
  g_array_free(w->kept, TRUE);
  g_ptr_array_free(w->frames, TRUE);
  g_free(w);
}

static void _widget_unmap(GtkWidget *widget,
                          _widget_t *w)
{
  _widget_reset(w);
}

static _widget_t *_widget_get(GtkWidget *widget)
{
  _widget_t *w = g_object_get_data(G_OBJECT(widget), WIDGET_KEY);
  if(w) return w;

  w = g_malloc0(sizeof(_widget_t));
  w->widget = widget;
  w->images = g_array_new(FALSE, FALSE, sizeof(_image_t));
  w->kept = g_array_new(FALSE, FALSE, sizeof(_image_t));
  w->frames = g_ptr_array_new_with_free_func(_frame_free);
  g_signal_connect(widget, "unmap", G_CALLBACK(_widget_unmap), w);
  g_object_set_data_full(G_OBJECT(widget), WIDGET_KEY, w, _widget_free);
  return w;
}

static gboolean _uses(const GArray *images, const _frame_t *frame)
{
  for(guint k = 0; k < images->len; k++)
    if(g_array_index(images, _image_t, k).frame == frame) return TRUE;
  return FALSE;
}

cairo_t *dt_presenter_begin(GtkWidget *widget,
                            cairo_t *cr)
{
  _widget_t *w = _widget_get(widget);
  g_array_set_size(w->images, 0);

  // a draw clipped to a damaged area records clipped images: they must
  // not replace what dt_presenter_keep() shows
  double x0, y0, x1, y1;
  cairo_clip_extents(cr, &x0, &y0, &x1, &y1);
  w->full = x0 <= 0.0 && y0 <= 0.0
    && x1 >= gtk_widget_get_allocated_width(widget)
    && y1 >= gtk_widget_get_allocated_height(widget);

  cairo_get_matrix(cr, &w->base);
  cairo_set_user_data(cr, &_cr_key, w, NULL);
  return cr;
}

void dt_presenter_end(GtkWidget *widget,
                      cairo_t *cr)
{
  _widget_t *w = cairo_get_user_data(cr, &_cr_key);
  if(!w) return;
  cairo_set_user_data(cr, &_cr_key, NULL, NULL);

  if(w->full)
  {
    g_array_set_size(w->kept, 0);
    g_array_append_vals(w->kept, w->images->data, w->images->len);
  }

  for(guint k = w->frames->len; k > 0; k--)
  {
    const _frame_t *f = g_ptr_array_index(w->frames, k - 1);
    if(!_uses(w->images, f) && !_uses(w->kept, f))
      g_ptr_array_remove_index_fast(w->frames, k - 1);
  }
}

cairo_surface_t *dt_presenter_gtk_surface(const float *pixels,
                                          const int width,
                                          const int height,
                                          const dt_display_output_t *encoding)
{
  cairo_surface_t *surface =
    cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
  if(cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) return surface;

  uint8_t *const data = cairo_image_surface_get_data(surface);
  const size_t stride = cairo_image_surface_get_stride(surface);
  DT_OMP_FOR()
  for(int y = 0; y < height; y++)
  {
    const float *in = pixels + (size_t)4 * width * y;
    uint8_t *out = data + stride * y;
    for(int x = 0; x < width; x++, in += 4, out += 4)
    {
      // cairo's RGB24 is BGRX in memory
      out[0] = dt_display_to_8bit(in[2]);
      out[1] = dt_display_to_8bit(in[1]);
      out[2] = dt_display_to_8bit(in[0]);
      out[3] = 0xff;
    }
  }
  cairo_surface_mark_dirty(surface);
  return surface;
}

// a frame already converted from the same source, or a new one
static _frame_t *_frame_get(_widget_t *w,
                            const float *pixels,
                            const int width,
                            const int height,
                            const dt_display_output_t *encoding,
                            const uint32_t id)
{
  for(guint k = 0; k < w->frames->len; k++)
  {
    _frame_t *f = g_ptr_array_index(w->frames, k);
    if(f->pixels == pixels && f->id == id && f->width == width && f->height == height
       && dt_display_output_equal(&f->encoding, encoding))
    {
      // a failed conversion leaves cairo's error surface: try again
      if(cairo_surface_status(f->surface) != CAIRO_STATUS_SUCCESS)
      {
        cairo_surface_destroy(f->surface);
        f->surface = dt_presenter_gtk_surface(pixels, width, height, encoding);
      }
      return f;
    }
  }

  _frame_t *f = g_malloc0(sizeof(_frame_t));
  f->pixels = pixels;
  f->id = id;
  f->width = width;
  f->height = height;
  f->encoding = *encoding;
  f->surface = dt_presenter_gtk_surface(pixels, width, height, encoding);
  g_ptr_array_add(w->frames, f);
  return f;
}

static void _paint(cairo_t *cr, const _image_t *image)
{
  cairo_save(cr);
  cairo_transform(cr, &image->matrix);
  cairo_set_source_surface(cr, image->frame->surface, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), image->filter);
  cairo_paint(cr);
  cairo_restore(cr);
}

gboolean dt_presenter_image(cairo_t *cr,
                            const double x,
                            const double y,
                            const float *pixels,
                            const int width,
                            const int height,
                            const dt_display_output_t *encoding,
                            const uint32_t id,
                            const cairo_filter_t filter)
{
  _widget_t *w = cairo_get_user_data(cr, &_cr_key);
  if(!w || !pixels || width <= 0 || height <= 0) return FALSE;

  _image_t image = { .frame = _frame_get(w, pixels, width, height, encoding, id),
                     .filter = filter };

  // frame pixels -> user space now -> device -> user space of the base
  cairo_matrix_t ctm, inverse_base = w->base;
  cairo_get_matrix(cr, &ctm);
  cairo_matrix_invert(&inverse_base);
  cairo_matrix_init_translate(&image.matrix, x, y);
  cairo_matrix_multiply(&image.matrix, &image.matrix, &ctm);
  cairo_matrix_multiply(&image.matrix, &image.matrix, &inverse_base);

  cairo_save(cr);
  cairo_set_matrix(cr, &w->base);
  cairo_clip_extents(cr, &image.clip[0], &image.clip[1], &image.clip[2], &image.clip[3]);
  cairo_restore(cr);

  cairo_save(cr);
  cairo_set_source_surface(cr, image.frame->surface, x, y);
  cairo_pattern_set_filter(cairo_get_source(cr), filter);
  cairo_paint(cr);
  cairo_restore(cr);

  g_array_append_val(w->images, image);
  return TRUE;
}

void dt_presenter_keep(cairo_t *cr)
{
  _widget_t *w = cairo_get_user_data(cr, &_cr_key);
  if(!w) return;

  for(guint k = 0; k < w->kept->len; k++)
  {
    const _image_t image = g_array_index(w->kept, _image_t, k);

    cairo_save(cr);
    cairo_set_matrix(cr, &w->base);
    cairo_rectangle(cr, image.clip[0], image.clip[1],
                    image.clip[2] - image.clip[0], image.clip[3] - image.clip[1]);
    cairo_clip(cr);
    _paint(cr, &image);
    cairo_restore(cr);

    g_array_append_val(w->images, image);
  }
}

void dt_presenter_clear(GtkWidget *widget)
{
  _widget_t *w = g_object_get_data(G_OBJECT(widget), WIDGET_KEY);
  if(w) _widget_reset(w);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
