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
#include "control/signal.h"
#include "gui/presenter_layers.h"

#define WIDGET_KEY "dt-presenter"

typedef struct _widget_t
{
  GtkWidget *widget;
  // during a draw: the layers if the window has a native backend, and
  // GTK's context
  dt_presenter_layered_t *layered;
  cairo_t *widget_cr;
  cairo_matrix_t base;
  gboolean full;
  // the images of the draw in progress, or of the last one
  GArray *images;
  // those of the last draw GTK did not restrict to a damaged area,
  // for dt_presenter_keep()
  GArray *kept;
  // a reference on each frame either uses
  GPtrArray *frames;
} _widget_t;

static cairo_user_data_key_t _cr_key;

static void _frame_unref(gpointer data)
{
  dt_frame_unref(data);
}

static void _widget_reset(_widget_t *w)
{
  dt_presenter_layered_t *layered = dt_presenter_layered_get(w->widget, FALSE);
  if(layered) dt_presenter_layered_clear(layered);
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
  w->images = g_array_new(FALSE, FALSE, sizeof(dt_presenter_image_t));
  w->kept = g_array_new(FALSE, FALSE, sizeof(dt_presenter_image_t));
  w->frames = g_ptr_array_new_with_free_func(_frame_unref);
  g_signal_connect(widget, "unmap", G_CALLBACK(_widget_unmap), w);
  g_object_set_data_full(G_OBJECT(widget), WIDGET_KEY, w, _widget_free);
  return w;
}

static gboolean _uses(const GArray *images,
                      const dt_frame_t *frame)
{
  for(guint k = 0; k < images->len; k++)
    if(g_array_index(images, dt_presenter_image_t, k).frame == frame) return TRUE;
  return FALSE;
}

cairo_t *dt_presenter_begin(GtkWidget *widget,
                            cairo_t *cr)
{
  _widget_t *w = _widget_get(widget);
  g_array_set_size(w->images, 0);
  w->widget_cr = cr;
  w->layered = dt_presenter_layered_get(widget, TRUE);
  cairo_t *draw_cr = w->layered ? dt_presenter_layered_begin(w->layered) : cr;

  // a draw clipped to a damaged area records clipped images: they must
  // not replace what dt_presenter_keep() shows
  double x0, y0, x1, y1;
  cairo_clip_extents(draw_cr, &x0, &y0, &x1, &y1);
  w->full = x0 <= 0.0 && y0 <= 0.0
    && x1 >= gtk_widget_get_allocated_width(widget)
    && y1 >= gtk_widget_get_allocated_height(widget);

  cairo_get_matrix(draw_cr, &w->base);
  cairo_set_user_data(draw_cr, &_cr_key, w, NULL);
  return draw_cr;
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
    const dt_frame_t *f = g_ptr_array_index(w->frames, k - 1);
    if(!_uses(w->images, f) && !_uses(w->kept, f))
      g_ptr_array_remove_index_fast(w->frames, k - 1);
  }

  if(w->layered) dt_presenter_layered_end(w->layered, w->widget_cr, w->images);
  w->layered = NULL;
  w->widget_cr = NULL;
}

// a window whose output the platform owns shows frames in its profile
// only, in which the mask and channel views are authored for its device
// encoding. the headroom does not count: a frame rendered before the
// headroom changed keeps its tone mapping until it is rendered again
static gboolean _shown_in(const dt_display_output_t *output,
                          const dt_frame_t *frame)
{
  return !output->platform_owned
    || dt_color_encoding_same_content(&output->image, dt_frame_encoding(frame));
}

static void _paint(cairo_t *cr,
                   const dt_presenter_image_t *image)
{
  cairo_save(cr);
  cairo_transform(cr, &image->matrix);
  // a cairo window's frames are RGB24: their own pixels, as master painted
  // its backbuf
  cairo_set_source_surface(cr, dt_frame_surface(image->frame, NULL), 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), image->filter);
  cairo_paint(cr);
  cairo_restore(cr);
}

gboolean dt_presenter_image(cairo_t *cr,
                            const double x,
                            const double y,
                            dt_frame_t *frame,
                            const cairo_filter_t filter)
{
  _widget_t *w = cairo_get_user_data(cr, &_cr_key);
  if(!w || !frame) return FALSE;

  // an output the platform owns holds with a native backend and with cairo,
  // and taking it renders the window's images again: their producers find
  // a new serial on the redraw it queues. otherwise, cairo shows display
  // RGB in whatever display profile it was rendered for, as it always did
  dt_display_output_t output;
  dt_presenter_window_output(w->widget, &output);
  if(!_shown_in(&output, frame)) return FALSE;

  dt_presenter_image_t image = { .frame = frame, .filter = filter };
  // a native backend composes from float pixels, cairo paints 8 bits
  cairo_surface_t *surface = NULL;
  if(w->layered ? !(image.pixels = dt_frame_float(frame, NULL))
                : !(surface = dt_frame_surface(frame, NULL)))
    return FALSE;

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

  if(w->layered)
    dt_presenter_layered_image(w->layered, &image);
  else
  {
    cairo_save(cr);
    cairo_set_source_surface(cr, surface, x, y);
    cairo_pattern_set_filter(cairo_get_source(cr), filter);
    cairo_paint(cr);
    cairo_restore(cr);
  }

  if(!g_ptr_array_find(w->frames, frame, NULL))
    g_ptr_array_add(w->frames, dt_frame_ref(frame));
  g_array_append_val(w->images, image);
  return TRUE;
}

void dt_presenter_keep(cairo_t *cr)
{
  _widget_t *w = cairo_get_user_data(cr, &_cr_key);
  if(!w) return;

  dt_display_output_t output;
  dt_presenter_window_output(w->widget, &output);

  for(guint k = 0; k < w->kept->len; k++)
  {
    const dt_presenter_image_t image = g_array_index(w->kept, dt_presenter_image_t, k);

    // in an output no longer shown
    if(!_shown_in(&output, image.frame)) continue;

    // a window never changes its backend: the draw that kept a frame made
    // what this one needs
    if(w->layered)
      dt_presenter_layered_image(w->layered, &image);
    else
    {
      cairo_save(cr);
      cairo_set_matrix(cr, &w->base);
      cairo_rectangle(cr, image.clip[0], image.clip[1],
                      image.clip[2] - image.clip[0], image.clip[3] - image.clip[1]);
      cairo_clip(cr);
      _paint(cr, &image);
      cairo_restore(cr);
    }

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
