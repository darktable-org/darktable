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
#include "common/iop_profile.h"
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

// cairo's RGB24 is BGRX in memory
static inline void _store(uint8_t *const out,
                          const float *const rgb)
{
  out[0] = dt_display_to_8bit(rgb[2]);
  out[1] = dt_display_to_8bit(rgb[1]);
  out[2] = dt_display_to_8bit(rgb[0]);
  out[3] = 0xff;
}

// a window whose output the platform owns shows frames in its profile
// only, made for its device encoding, in which the mask and channel views
// are authored. the headroom does not count: a frame rendered before the
// headroom changed keeps its tone mapping until it is rendered again
static gboolean _shown_in(const dt_display_output_t *output,
                          const dt_display_output_t *label)
{
  return !output->platform_owned
    || (dt_color_encoding_same_content(&output->image, &label->image)
        && dt_color_encoding_same_content(&output->device, &label->device));
}

// float RGBA in encoding as cairo's RGB24, converted to the profile gtk
// unless that is NULL
static cairo_surface_t *_surface(const float *pixels,
                                 const int width,
                                 const int height,
                                 const dt_color_encoding_t *encoding,
                                 const dt_color_encoding_t *gtk)
{
  cairo_surface_t *surface =
    cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
  if(cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) return surface;

  uint8_t *const data = cairo_image_surface_get_data(surface);
  const size_t stride = cairo_image_surface_get_stride(surface);

  const dt_iop_order_iccprofile_info_t *from = NULL, *to = NULL;
  if(gtk && !dt_color_encoding_same_content(gtk, encoding))
  {
    from = dt_ioppr_add_profile_info_for_content(darktable.develop, encoding->type,
                                                 encoding->filename, encoding->profile,
                                                 gtk->intent);
    to = dt_ioppr_add_profile_info_for_content(darktable.develop, gtk->type, gtk->filename,
                                               gtk->profile, gtk->intent);
  }

  if(from && to
     && dt_is_valid_colormatrix(from->matrix_in[0][0])
     && dt_is_valid_colormatrix(to->matrix_out[0][0]))
  {
    dt_colormatrix_t product, matrix;
    dt_colormatrix_mul(product, to->matrix_out, from->matrix_in);
    transpose_3xSSE(product, matrix);
    DT_OMP_FOR(shared(matrix))
    for(int y = 0; y < height; y++)
    {
      const float *in = pixels + (size_t)4 * width * y;
      uint8_t *out = data + stride * y;
      for(int x = 0; x < width; x++, in += 4, out += 4)
      {
        dt_aligned_pixel_t linear, converted, rgb;
        dt_ioppr_decode_rgb(in, linear, from);
        dt_apply_transposed_color_matrix(linear, matrix, converted);
        // 8 bits hold [0, 1]: what lies outside needs no transfer function
        for_three_channels(c) converted[c] = CLAMPF(converted[c], 0.0f, 1.0f);
        dt_ioppr_encode_rgb(converted, rgb, to);
        _store(out, rgb);
      }
    }
    cairo_surface_mark_dirty(surface);
    return surface;
  }

  // no matrix profile: lcms converts a float copy
  float *converted = NULL;
  if(from && to)
  {
    converted = dt_alloc_align_float((size_t)4 * width * height);
    // unconverted pixels would be kept as converted: fail as cairo does
    if(!converted)
    {
      cairo_surface_destroy(surface);
      return cairo_image_surface_create(CAIRO_FORMAT_INVALID, 0, 0);
    }
    dt_ioppr_transform_image_colorspace_rgb(pixels, converted, width, height, from, to,
                                            "gtk surface");
    pixels = converted;
  }
  DT_OMP_FOR()
  for(int y = 0; y < height; y++)
    dt_display_to_8bit_pixels(pixels + (size_t)4 * width * y, data + stride * y, width,
                              TRUE);
  dt_free_align(converted);
  cairo_surface_mark_dirty(surface);
  return surface;
}

// GTK-side drawing draws the same frames again and again (navigation,
// the filmstrip thumbnail and the correction cursor the preview, focus
// peaking the main image on every draw), so each is converted once.
// enough for what is drawn at once: the preview and, with focus peaking,
// the main image, a snapshot and the second window's image
#define GTK_SURFACES 4

typedef struct _gtk_surface_t
{
  uint32_t id;
  dt_display_output_t encoding;
  // the profile of GTK's surface if the platform owns the output, type
  // DT_COLORSPACE_NONE if not
  dt_color_encoding_t gtk;
  cairo_surface_t *surface;
} _gtk_surface_t;

// most recently used first
static _gtk_surface_t _gtk_surfaces[GTK_SURFACES];

cairo_surface_t *dt_presenter_gtk_surface(const float *pixels,
                                          const int width,
                                          const int height,
                                          const dt_display_output_t *encoding,
                                          const uint32_t id)
{
  // on a cairo window, GTK-side drawing shows a frame as the center does:
  // display RGB as it was rendered
  dt_display_output_t output;
  dt_colorspaces_get_display_output(DT_COLORSPACE_DISPLAY, &output);
  const gboolean owned = output.platform_owned;
  dt_color_encoding_t gtk = output.device;
  if(!owned)
  {
    memset(&gtk, 0, sizeof(gtk));
    gtk.type = DT_COLORSPACE_NONE;
  }

  int k = 0;
  while(k < GTK_SURFACES
        && !(_gtk_surfaces[k].surface && _gtk_surfaces[k].id == id
             && dt_display_output_equal(&_gtk_surfaces[k].encoding, encoding)
             && dt_color_encoding_equal(&_gtk_surfaces[k].gtk, &gtk)))
    k++;

  _gtk_surface_t entry;
  if(k < GTK_SURFACES)
    entry = _gtk_surfaces[k];
  else
  {
    cairo_surface_t *surface =
      _surface(pixels, width, height, &encoding->image, owned ? &gtk : NULL);
    // a failed conversion is not kept: the next draw tries again
    if(cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) return surface;

    k = GTK_SURFACES - 1;
    if(_gtk_surfaces[k].surface) cairo_surface_destroy(_gtk_surfaces[k].surface);
    entry = (_gtk_surface_t){ .id = id, .encoding = *encoding, .gtk = gtk,
                              .surface = surface };
  }
  memmove(_gtk_surfaces + 1, _gtk_surfaces, sizeof(_gtk_surface_t) * k);
  _gtk_surfaces[0] = entry;
  return cairo_surface_reference(entry.surface);
}

// the frame id in encoding, converted already or new
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
    if(c->id == id && dt_display_output_equal(&c->encoding, encoding)) f = c;
  }

  if(!f)
  {
    f = g_malloc0(sizeof(dt_presenter_frame_t));
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
    f->surface = _surface(pixels, width, height, &encoding->image, NULL);
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

  // an output the platform owns holds with a native backend and with cairo,
  // and taking it renders the window's pipes again (colorout's
  // DT_SIGNAL_CONTROL_PROFILE_CHANGED handler). otherwise, cairo shows
  // display RGB in whatever display profile it was rendered for, as it
  // always did
  dt_display_output_t output;
  dt_presenter_window_output(w->widget, &output);
  if(!_shown_in(&output, encoding)) return FALSE;

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
  dt_presenter_window_output(w->widget, &output);

  for(guint k = 0; k < w->kept->len; k++)
  {
    const dt_presenter_image_t image = g_array_index(w->kept, dt_presenter_image_t, k);

    // in an output no longer shown
    if(!_shown_in(&output, &image.frame->encoding)) continue;

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
