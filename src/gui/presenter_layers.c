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
 * the GTK3 side of native presentation: GTK3 cannot draw into a surface
 * with a color state of its own, so a native backend puts the image on a
 * layer above GTK's surface, and everything that has to appear above the
 * image moves to a second layer above that. a GTK4 port, where widgets
 * draw textures with color states, deletes this file.
 */

#include "gui/presenter_layers.h"
#include "common/darktable.h"
#include "gui/gtk.h"

#define WINDOW_KEY "dt-presenter-window"

struct dt_presenter_window_t
{
  GtkWidget *toplevel;
  // DT_COLORSPACE_DISPLAY for the main window, DISPLAY2 for the second
  dt_colorspaces_color_profile_type_t kind;
  dt_presenter_backend_t *backend;
  GList *widgets;
};

// what an image layer was composed from, to know when to compose again
typedef struct _shown_t
{
  uint32_t id;
  cairo_matrix_t matrix;
  double clip[4];
  cairo_filter_t filter;
} _shown_t;

struct dt_presenter_layered_t
{
  GtkWidget *widget;
  dt_presenter_window_t *window;
  gulong destroy_handler;
  // what darktable draws, in device pixels
  cairo_surface_t *foreground;
  cairo_t *cr;
  int width, height, scale;
  // the images of the last draw
  GArray *images;
  // what the image layer holds
  GArray *shown;
  int shown_width, shown_height, shown_scale;
  float *image;
  // drawn and not presented yet
  gboolean pending;
  void *backend_data;
};

static void _layered_free(dt_presenter_layered_t *l)
{
  if(l->backend_data) l->window->backend->release(l->window->backend, &l->backend_data);
  g_signal_handler_disconnect(l->widget, l->destroy_handler);
  if(l->cr) cairo_destroy(l->cr);
  if(l->foreground) cairo_surface_destroy(l->foreground);
  g_array_free(l->images, TRUE);
  g_array_free(l->shown, TRUE);
  dt_free_align(l->image);
  g_free(l);
}

static void _widget_destroy(GtkWidget *widget,
                            dt_presenter_layered_t *l)
{
  l->window->widgets = g_list_remove(l->window->widgets, l);
  _layered_free(l);
}

static void _window_free(gpointer data)
{
  dt_presenter_window_t *w = data;
  while(w->widgets)
  {
    _layered_free(w->widgets->data);
    w->widgets = g_list_delete_link(w->widgets, w->widgets);
  }
  if(w->backend) w->backend->free(w->backend);
  // whatever the platform owned goes back to the system display profile
  dt_colorspaces_set_window_output(w->kind, NULL);
  g_free(w);
}

static void _toplevel_unrealize(GtkWidget *toplevel,
                                gpointer data)
{
  g_object_set_data(G_OBJECT(toplevel), WINDOW_KEY, NULL);
}

static void _present(dt_presenter_layered_t *l);

void dt_presenter_window_present(dt_presenter_window_t *w)
{
  for(GList *l = w->widgets; l; l = g_list_next(l))
  {
    dt_presenter_layered_t *layered = l->data;
    if(layered->pending) _present(layered);
  }
}

static dt_colorspaces_color_profile_type_t _kind(GtkWidget *toplevel)
{
  return toplevel == dt_ui_main_window(darktable.gui->ui)
    ? DT_COLORSPACE_DISPLAY
    : DT_COLORSPACE_DISPLAY2;
}

static dt_presenter_window_t *_window_get(GtkWidget *widget)
{
  GtkWidget *toplevel = gtk_widget_get_toplevel(widget);
  if(!gtk_widget_is_toplevel(toplevel) || !gtk_widget_get_realized(toplevel)) return NULL;

  dt_presenter_window_t *w = g_object_get_data(G_OBJECT(toplevel), WINDOW_KEY);
  if(w) return w;

  w = g_malloc0(sizeof(dt_presenter_window_t));
  w->toplevel = toplevel;
  w->kind = _kind(toplevel);
  g_object_set_data_full(G_OBJECT(toplevel), WINDOW_KEY, w, _window_free);
  g_signal_connect(toplevel, "unrealize", G_CALLBACK(_toplevel_unrealize), NULL);

#ifdef HAVE_WAYLAND
  gboolean manages_color = FALSE;
  w->backend = dt_presenter_wayland_new(toplevel, w, &manages_color);
  if(!w->backend)
  {
    // a compositor that manages color takes GTK's untagged surface as sRGB
    // and converts it to the screen itself: rendering for the display
    // profile would convert twice. NULL also takes back what a failed
    // setup reported
    const dt_display_output_t srgb =
      { .image = { .type = DT_COLORSPACE_SRGB, .intent = DT_INTENT_PERCEPTUAL,
                   .headroom = 1.0f },
        .device = { .type = DT_COLORSPACE_SRGB, .intent = DT_INTENT_PERCEPTUAL,
                    .headroom = 1.0f },
        .format = DT_FRAME_RGB24 };
    dt_presenter_window_set_output(w, manages_color ? &srgb : NULL);
  }
#endif
  return w;
}

void dt_presenter_window_set_output(dt_presenter_window_t *window,
                                    const dt_display_output_t *output)
{
  dt_colorspaces_set_window_output(window->kind, output);
}

void dt_presenter_window_redraw(dt_presenter_window_t *window)
{
  gtk_widget_queue_draw(window->toplevel);
}

dt_presenter_layered_t *dt_presenter_layered_get(GtkWidget *widget,
                                                 const gboolean create)
{
  GtkWidget *toplevel = gtk_widget_get_toplevel(widget);
  dt_presenter_window_t *w = create
    ? _window_get(widget)
    : g_object_get_data(G_OBJECT(toplevel), WINDOW_KEY);
  if(!w || !w->backend) return NULL;

  for(GList *l = w->widgets; l; l = g_list_next(l))
    if(((dt_presenter_layered_t *)l->data)->widget == widget) return l->data;
  if(!create) return NULL;

  dt_presenter_layered_t *l = g_malloc0(sizeof(dt_presenter_layered_t));
  l->widget = widget;
  l->window = w;
  l->images = g_array_new(FALSE, FALSE, sizeof(dt_presenter_image_t));
  l->shown = g_array_new(FALSE, FALSE, sizeof(_shown_t));
  l->destroy_handler =
    g_signal_connect_data(widget, "destroy", G_CALLBACK(_widget_destroy), l, NULL, 0);
  w->widgets = g_list_prepend(w->widgets, l);
  return l;
}

void dt_presenter_window_output(GtkWidget *widget,
                                dt_display_output_t *output)
{
  dt_colorspaces_get_display_output(_kind(gtk_widget_get_toplevel(widget)), output);
}

cairo_t *dt_presenter_layered_begin(dt_presenter_layered_t *l)
{
  const int width = gtk_widget_get_allocated_width(l->widget);
  const int height = gtk_widget_get_allocated_height(l->widget);
  const int scale = gtk_widget_get_scale_factor(l->widget);

  if(l->cr) cairo_destroy(l->cr);
  // a failed allocation leaves cairo's error surface: try again. a context
  // on it is in error too, so presenter.c's user data does not stick and
  // the draw presents nothing
  if(!l->foreground || cairo_surface_status(l->foreground) != CAIRO_STATUS_SUCCESS
     || width != l->width || height != l->height || scale != l->scale)
  {
    if(l->foreground) cairo_surface_destroy(l->foreground);
    l->foreground = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                               width * scale, height * scale);
    cairo_surface_set_device_scale(l->foreground, scale, scale);
    l->width = width;
    l->height = height;
    l->scale = scale;
  }
  l->cr = cairo_create(l->foreground);
  cairo_save(l->cr);
  cairo_set_operator(l->cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint(l->cr);
  cairo_restore(l->cr);
  return l->cr;
}

// the device pixels an image covers lie in box (x0, y0, x1, y1), and are
// those whose center inverse maps into the frame
static gboolean _coverage(const dt_presenter_layered_t *l,
                          const dt_presenter_image_t *image,
                          int box[4],
                          cairo_matrix_t *inverse)
{
  const int s = l->scale;
  cairo_matrix_t device, forward;
  cairo_matrix_init_scale(&device, s, s);
  cairo_matrix_multiply(&forward, &image->matrix, &device);

  // the frame's bounding box
  const dt_frame_geometry_t *g = dt_frame_geometry(image->frame);
  const double fw = g->width, fh = g->height;
  const double corners[4][2] = { { 0, 0 }, { fw, 0 }, { 0, fh }, { fw, fh } };
  double x0 = DBL_MAX, y0 = DBL_MAX, x1 = -DBL_MAX, y1 = -DBL_MAX;
  for(int k = 0; k < 4; k++)
  {
    double x = corners[k][0], y = corners[k][1];
    cairo_matrix_transform_point(&forward, &x, &y);
    x0 = fmin(x0, x);
    y0 = fmin(y0, y);
    x1 = fmax(x1, x);
    y1 = fmax(y1, y);
  }

  // pixels whose center is inside the clip; _inside() tests the frame
  box[0] = MAX(0, MAX((int)floor(x0), (int)ceil(image->clip[0] * s - 0.5)));
  box[1] = MAX(0, MAX((int)floor(y0), (int)ceil(image->clip[1] * s - 0.5)));
  box[2] = MIN(l->width * s, MIN((int)ceil(x1), (int)ceil(image->clip[2] * s - 0.5)));
  box[3] = MIN(l->height * s, MIN((int)ceil(y1), (int)ceil(image->clip[3] * s - 0.5)));

  *inverse = forward;
  return box[0] < box[2] && box[1] < box[3]
    && cairo_matrix_invert(inverse) == CAIRO_STATUS_SUCCESS;
}

static inline gboolean _inside(const cairo_matrix_t *inverse,
                               const dt_frame_geometry_t *frame,
                               const int x,
                               const int y,
                               double *u,
                               double *v)
{
  *u = x + 0.5;
  *v = y + 0.5;
  cairo_matrix_transform_point(inverse, u, v);
  return *u >= 0.0 && *v >= 0.0 && *u < frame->width && *v < frame->height;
}

void dt_presenter_layered_image(dt_presenter_layered_t *l,
                                const dt_presenter_image_t *image)
{
  int box[4];
  cairo_matrix_t inverse;
  if(!_coverage(l, image, box, &inverse)) return;

  cairo_surface_flush(l->foreground);
  uint8_t *const data = cairo_image_surface_get_data(l->foreground);
  const int stride = cairo_image_surface_get_stride(l->foreground);
  const dt_frame_geometry_t *frame = dt_frame_geometry(image->frame);
  DT_OMP_FOR()
  for(int y = box[1]; y < box[3]; y++)
  {
    uint32_t *row = (uint32_t *)(data + (size_t)stride * y);
    for(int x = box[0]; x < box[2]; x++)
    {
      double u, v;
      if(_inside(&inverse, frame, x, y, &u, &v)) row[x] = 0;
    }
  }
  cairo_surface_mark_dirty(l->foreground);
}

static void _compose(dt_presenter_layered_t *l,
                     const int width,
                     const int height)
{
  const size_t size = (size_t)4 * width * height;
  if(width != l->shown_width || height != l->shown_height)
  {
    dt_free_align(l->image);
    l->image = dt_alloc_align_float(size);
  }
  // without a buffer nothing is shown: the next draw tries again
  if(!l->image)
  {
    l->shown_width = l->shown_height = 0;
    return;
  }
  l->shown_width = width;
  l->shown_height = height;
  memset(l->image, 0, sizeof(float) * size);

  for(guint k = 0; k < l->images->len; k++)
  {
    const dt_presenter_image_t *image =
      &g_array_index(l->images, dt_presenter_image_t, k);
    const dt_frame_geometry_t *frame = dt_frame_geometry(image->frame);
    const float *const pixels = image->pixels;
    int box[4];
    cairo_matrix_t inverse;
    if(!pixels || !_coverage(l, image, box, &inverse)) continue;

    const gboolean nearest = image->filter == CAIRO_FILTER_FAST
                          || image->filter == CAIRO_FILTER_NEAREST;
    float *const out = l->image;
    DT_OMP_FOR()
    for(int y = box[1]; y < box[3]; y++)
      for(int x = box[0]; x < box[2]; x++)
      {
        double u, v;
        if(!_inside(&inverse, frame, x, y, &u, &v)) continue;

        float *const px = out + (size_t)4 * (y * width + x);
        if(nearest)
        {
          const float *in = pixels + (size_t)4 * ((int)v * frame->width + (int)u);
          for(int c = 0; c < 3; c++) px[c] = in[c];
        }
        else
        {
          const double fu = CLAMP(u - 0.5, 0.0, frame->width - 1.0);
          const double fv = CLAMP(v - 0.5, 0.0, frame->height - 1.0);
          const int u0 = (int)fu, v0 = (int)fv;
          const int u1 = MIN(u0 + 1, frame->width - 1),
                    v1 = MIN(v0 + 1, frame->height - 1);
          const float a = fu - u0, b = fv - v0;
          const float *p00 = pixels + (size_t)4 * (v0 * frame->width + u0);
          const float *p01 = pixels + (size_t)4 * (v0 * frame->width + u1);
          const float *p10 = pixels + (size_t)4 * (v1 * frame->width + u0);
          const float *p11 = pixels + (size_t)4 * (v1 * frame->width + u1);
          for(int c = 0; c < 3; c++)
            px[c] = (1.0f - b) * ((1.0f - a) * p00[c] + a * p01[c])
                    + b * ((1.0f - a) * p10[c] + a * p11[c]);
        }
        px[3] = 1.0f;
      }
  }
}

static gboolean _image_changed(const dt_presenter_layered_t *l,
                               const int width,
                               const int height)
{
  if(width != l->shown_width || height != l->shown_height || l->scale != l->shown_scale
     || l->images->len != l->shown->len)
    return TRUE;
  for(guint k = 0; k < l->images->len; k++)
  {
    const dt_presenter_image_t *i = &g_array_index(l->images, dt_presenter_image_t, k);
    const _shown_t *s = &g_array_index(l->shown, _shown_t, k);
    if(dt_frame_id(i->frame) != s->id || i->filter != s->filter
       || memcmp(&i->matrix, &s->matrix, sizeof(cairo_matrix_t))
       || memcmp(i->clip, s->clip, sizeof(s->clip)))
      return TRUE;
  }
  return FALSE;
}

// GTK3 draws the widgets that GtkOverlay stacks above a presenting
// widget into its own surface, which the layers cover: draw them into the
// foreground too. the real ones stay below and keep getting the input,
// since the layers take none. subtrees that present are left out, they
// are the layers' own content
static void _replay_overlays(dt_presenter_layered_t *l)
{
  GtkWidget *child = l->widget;
  for(GtkWidget *parent = gtk_widget_get_parent(child);
      parent;
      child = parent, parent = gtk_widget_get_parent(parent))
  {
    if(!GTK_IS_OVERLAY(parent)) continue;

    GList *children = gtk_container_get_children(GTK_CONTAINER(parent));
    gboolean above = FALSE;
    for(GList *c = children; c; c = g_list_next(c))
    {
      GtkWidget *overlay = c->data;
      if(overlay == child)
      {
        above = TRUE;
        continue;
      }
      int x, y;
      if(!above || !gtk_widget_is_drawable(overlay)
         || !gtk_widget_translate_coordinates(overlay, l->widget, 0, 0, &x, &y))
        continue;

      cairo_save(l->cr);
      cairo_translate(l->cr, x, y);
      gtk_widget_draw(overlay, l->cr);
      cairo_restore(l->cr);
    }
    g_list_free(children);
  }
}

static void _present(dt_presenter_layered_t *l)
{
  l->pending = FALSE;
  const int width = l->width * l->scale;
  const int height = l->height * l->scale;

  const gboolean changed = _image_changed(l, width, height);
  if(changed)
  {
    _compose(l, width, height);
    if(!l->image) return;
    l->shown_scale = l->scale;
    g_array_set_size(l->shown, l->images->len);
    for(guint k = 0; k < l->images->len; k++)
    {
      const dt_presenter_image_t *i = &g_array_index(l->images, dt_presenter_image_t, k);
      _shown_t *s = &g_array_index(l->shown, _shown_t, k);
      *s = (_shown_t){ .id = dt_frame_id(i->frame), .matrix = i->matrix,
                       .filter = i->filter };
      memcpy(s->clip, i->clip, sizeof(s->clip));
    }
  }

  _replay_overlays(l);
  cairo_surface_flush(l->foreground);

  dt_presenter_layers_t layers = { .image = changed ? l->image : NULL,
                                   .foreground =
                                     cairo_image_surface_get_data(l->foreground),
                                   .width = width,
                                   .height = height,
                                   .scale = l->scale };
  gtk_widget_translate_coordinates(l->widget, l->window->toplevel, 0, 0,
                                   &layers.x, &layers.y);
  if(!l->window->backend->present(l->window->backend, l->widget,
                                  &l->backend_data, &layers))
  {
    // not shown: the image has to go out with the next present
    g_array_set_size(l->shown, 0);
    l->shown_width = l->shown_height = 0;
  }
}

void dt_presenter_layered_end(dt_presenter_layered_t *l,
                              cairo_t *widget_cr,
                              const GArray *images)
{
  g_array_set_size(l->images, 0);
  g_array_append_vals(l->images, images->data, images->len);

  if(images->len)
  {
    l->pending = TRUE;
    return;
  }

  // nothing to show on the layers: what was drawn goes to GTK's surface
  dt_presenter_layered_clear(l);
  cairo_surface_flush(l->foreground);
  cairo_save(widget_cr);
  cairo_set_source_surface(widget_cr, l->foreground, 0, 0);
  cairo_paint(widget_cr);
  cairo_restore(widget_cr);
}

void dt_presenter_layered_clear(dt_presenter_layered_t *l)
{
  l->pending = FALSE;
  g_array_set_size(l->images, 0);
  g_array_set_size(l->shown, 0);
  l->shown_width = l->shown_height = 0;
  if(l->backend_data) l->window->backend->clear(l->window->backend, &l->backend_data);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
