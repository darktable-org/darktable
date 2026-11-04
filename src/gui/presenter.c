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
  // the frames either uses
  GPtrArray *frames;
} _widget_t;

static cairo_user_data_key_t _cr_key;

static void _frame_free(gpointer data)
{
  dt_presenter_frame_t *f = data;
  if(f->surface) cairo_surface_destroy(f->surface);
  dt_free_align(f->pixels);
  g_free(f);
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
  w->frames = g_ptr_array_new_with_free_func(_frame_free);
  g_signal_connect(widget, "unmap", G_CALLBACK(_widget_unmap), w);
  g_object_set_data_full(G_OBJECT(widget), WIDGET_KEY, w, _widget_free);
  return w;
}

static gboolean _uses(const GArray *images,
                      const dt_presenter_frame_t *frame)
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
    const dt_presenter_frame_t *f = g_ptr_array_index(w->frames, k - 1);
    if(!_uses(w->images, f) && !_uses(w->kept, f))
      g_ptr_array_remove_index_fast(w->frames, k - 1);
  }

  if(w->layered) dt_presenter_layered_end(w->layered, w->widget_cr, w->images);
  w->layered = NULL;
  w->widget_cr = NULL;
}

// float RGBA in encoding to 8-bit RGBX, BGRX with bgr. with to_gtk, in the
// profile GTK's surface of the main window is shown in
static void _to_8bit(const float *pixels,
                     const size_t width,
                     const size_t height,
                     const dt_display_output_t *encoding,
                     const gboolean to_gtk,
                     uint8_t *out,
                     const size_t out_stride,
                     const gboolean bgr)
{
  cmsHTRANSFORM xform = NULL;
  dt_display_output_t gtk;
  if(to_gtk
     && dt_colorspaces_get_gtk_output(DT_COLORSPACE_DISPLAY, &gtk)
     && !dt_display_output_equal(&gtk, encoding))
  {
    const dt_colorspaces_color_profile_t *from =
      dt_colorspaces_get_profile(encoding->type, encoding->filename,
                                 DT_PROFILE_DIRECTION_ANY);
    const dt_colorspaces_color_profile_t *to =
      dt_colorspaces_get_profile(gtk.type, gtk.filename, DT_PROFILE_DIRECTION_ANY);
    if(from && to)
      xform = cmsCreateTransform(from->profile, TYPE_RGBA_FLT, to->profile, TYPE_RGBA_FLT,
                                 gtk.intent, 0);
  }

  const int r = bgr ? 2 : 0;
  size_t padded_size = 0;
  float *lines = xform ? dt_alloc_perthread_float(4 * width, &padded_size) : NULL;
  DT_OMP_FOR()
  for(size_t y = 0; y < height; y++)
  {
    const float *in = pixels + 4 * width * y;
    if(lines)
    {
      float *const line = dt_get_perthread(lines, padded_size);
      cmsDoTransform(xform, in, line, width);
      in = line;
    }
    uint8_t *o = out + out_stride * y;
    for(size_t x = 0; x < width; x++, in += 4, o += 4)
    {
      o[r] = dt_display_to_8bit(in[0]);
      o[1] = dt_display_to_8bit(in[1]);
      o[2 - r] = dt_display_to_8bit(in[2]);
      o[3] = 0xff;
    }
  }
  dt_free_align(lines);
  if(xform) cmsDeleteTransform(xform);
}

static cairo_surface_t *_surface(const float *pixels,
                                 const int width,
                                 const int height,
                                 const dt_display_output_t *encoding,
                                 const gboolean to_gtk)
{
  cairo_surface_t *surface =
    cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
  if(cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) return surface;

  // cairo's RGB24 is BGRX in memory
  _to_8bit(pixels, width, height, encoding, to_gtk, cairo_image_surface_get_data(surface),
           cairo_image_surface_get_stride(surface), TRUE);
  cairo_surface_mark_dirty(surface);
  return surface;
}

cairo_surface_t *dt_presenter_gtk_surface(const float *pixels,
                                          const int width,
                                          const int height,
                                          const dt_display_output_t *encoding)
{
  return _surface(pixels, width, height, encoding, TRUE);
}

void dt_presenter_gtk_colors(const float *pixels,
                             const size_t count,
                             const dt_display_output_t *encoding,
                             float *rgb)
{
  uint8_t *out = g_malloc(4 * count);
  _to_8bit(pixels, count, 1, encoding, TRUE, out, 4 * count, FALSE);
  for(size_t k = 0; k < count; k++)
    for(int c = 0; c < 3; c++)
      rgb[3 * k + c] = out[4 * k + c] / 255.0f;
  g_free(out);
}

// a frame already converted from the same source, or a new one
static dt_presenter_frame_t *_frame_get(_widget_t *w,
                                        const float *pixels,
                                        const int width,
                                        const int height,
                                        const dt_display_output_t *encoding,
                                        const uint32_t id)
{
  dt_presenter_frame_t *f = NULL;
  for(guint k = 0; k < w->frames->len && !f; k++)
  {
    dt_presenter_frame_t *c = g_ptr_array_index(w->frames, k);
    if(c->source == pixels && c->id == id && c->width == width && c->height == height
       && dt_display_output_equal(&c->encoding, encoding))
      f = c;
  }

  if(!f)
  {
    f = g_malloc0(sizeof(dt_presenter_frame_t));
    f->source = pixels;
    f->id = id;
    f->width = width;
    f->height = height;
    f->encoding = *encoding;
    g_ptr_array_add(w->frames, f);
  }

  // a native backend composes from the float pixels, cairo paints 8 bits
  const size_t size = sizeof(float) * 4 * width * height;
  if(w->layered && !f->pixels && (f->pixels = dt_alloc_aligned(size)))
    memcpy(f->pixels, pixels, size);
  // a failed conversion leaves cairo's error surface: try again
  if(!w->layered && f->surface && cairo_surface_status(f->surface) != CAIRO_STATUS_SUCCESS)
  {
    cairo_surface_destroy(f->surface);
    f->surface = NULL;
  }
  if(!w->layered && !f->surface)
    f->surface = _surface(pixels, width, height, encoding, FALSE);
  return f;
}

static void _paint(cairo_t *cr,
                   const dt_presenter_image_t *image)
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

  // a window that declared its output shows that one only, with a native
  // backend or with cairo. otherwise cairo shows display RGB in whatever
  // display profile it was rendered for, as it always did
  dt_display_output_t output;
  if(dt_presenter_declared_output(w->widget, &output)
     && !dt_display_output_equal(encoding, &output))
    return FALSE;

  dt_presenter_image_t image = { .frame = _frame_get(w, pixels, width, height,
                                                     encoding, id),
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

  if(w->layered)
  {
    if(!image.frame->pixels) return FALSE;
    dt_presenter_layered_image(w->layered, &image);
  }
  else
  {
    cairo_save(cr);
    cairo_set_source_surface(cr, image.frame->surface, x, y);
    cairo_pattern_set_filter(cairo_get_source(cr), filter);
    cairo_paint(cr);
    cairo_restore(cr);
  }

  g_array_append_val(w->images, image);
  return TRUE;
}

void dt_presenter_keep(cairo_t *cr)
{
  _widget_t *w = cairo_get_user_data(cr, &_cr_key);
  if(!w) return;

  dt_display_output_t output;
  const gboolean declared = dt_presenter_declared_output(w->widget, &output);

  for(guint k = 0; k < w->kept->len; k++)
  {
    const dt_presenter_image_t image = g_array_index(w->kept, dt_presenter_image_t, k);

    // in an output no longer shown
    if(declared && !dt_display_output_equal(&image.frame->encoding, &output)) continue;

    if(w->layered)
    {
      // kept from a draw before the backend
      if(!image.frame->pixels) continue;
      dt_presenter_layered_image(w->layered, &image);
    }
    else
    {
      if(!image.frame->surface) continue;
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

static void _display_control_update(gpointer instance,
                                    GtkWidget *control)
{
  dt_display_output_t gtk;
  const dt_colorspaces_color_profile_type_t window =
    GPOINTER_TO_INT(g_object_get_data(G_OBJECT(control), "dt-presenter-window"));
  gtk_widget_set_sensitive(control, !dt_colorspaces_get_gtk_output(window, &gtk));
}

void dt_presenter_display_control(GtkWidget *control,
                                  const dt_colorspaces_color_profile_type_t window)
{
  g_object_set_data(G_OBJECT(control), "dt-presenter-window", GINT_TO_POINTER(window));
  _display_control_update(NULL, control);
  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_CONTROL_PROFILE_CHANGED,
                            _display_control_update, control);
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
