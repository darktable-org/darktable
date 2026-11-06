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
 * the layered backend, the GTK3 side of native presentation: GTK3 cannot
 * draw into a surface with a color state of its own, so the image goes on
 * a layer above GTK's surface, and everything that has to appear above the
 * image moves to a second layer above that. a transport of the platform
 * shows them. a GTK4 port, where widgets draw textures with color states,
 * deletes this file.
 */

#include "gui/presenter_layers.h"
#include "common/darktable.h"
#include "gui/gtk.h"

#define POPOVER_KEY "dt-presenter-popover"

typedef struct _window_t
{
  dt_presenter_backend_t backend;
  dt_presenter_window_t *window;
  dt_presenter_transport_t *transport;
  GList *widgets;
  // the layers cover GTK's popovers: replay them, and draw again when one
  // changes. the popovers watched for that, as weak pointers
  gboolean replay_popovers;
  GPtrArray *popovers;
} _window_t;

// what an image layer was composed from, to know when to compose again
typedef struct _shown_t
{
  // a frame and its rendition: a kept frame is converted again for a new
  // output
  uint32_t id;
  const float *pixels;
  cairo_matrix_t matrix;
  double clip[4];
  cairo_filter_t filter;
} _shown_t;

typedef struct _widget_t
{
  GtkWidget *widget;
  _window_t *window;
  // what the widget draws, in device pixels
  cairo_surface_t *foreground;
  cairo_t *cr;
  int width, height, scale;
  // the kept presentation: its foreground, which may be the one above,
  // and its images, with a reference on each frame. while the last draw
  // is kept, spare is the surface the next one draws on
  cairo_surface_t *kept, *spare;
  GArray *kept_images;
  // the images of the last draw
  GArray *images;
  // what the image layer holds
  GArray *shown;
  int shown_width, shown_height, shown_scale;
  float shown_headroom;
  float *image;
  // drawn and not presented yet
  gboolean pending;
  void *transport_data;
} _widget_t;

static void _surface_destroy(cairo_surface_t **surface)
{
  if(*surface) cairo_surface_destroy(*surface);
  *surface = NULL;
}

static void _images_unref(GArray *images)
{
  for(guint k = 0; k < images->len; k++)
    dt_frame_unref(g_array_index(images, dt_presenter_image_t, k).frame);
  g_array_set_size(images, 0);
}

static void _drop_kept(_widget_t *l)
{
  _surface_destroy(&l->kept);
  _surface_destroy(&l->spare);
  _images_unref(l->kept_images);
}

static void _present(_widget_t *l);

void dt_presenter_window_present(dt_presenter_window_t *window)
{
  _window_t *w = (_window_t *)window->backend;
  if(!w) return;
  for(GList *l = w->widgets; l; l = g_list_next(l))
  {
    _widget_t *widget = l->data;
    if(widget->pending) _present(widget);
  }
}

static gboolean _fits(cairo_surface_t *surface,
                      const int width,
                      const int height,
                      const int scale)
{
  double sx = 0.0, sy = 0.0;
  if(surface) cairo_surface_get_device_scale(surface, &sx, &sy);
  return surface && cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS
    && cairo_image_surface_get_width(surface) == width * scale
    && cairo_image_surface_get_height(surface) == height * scale
    && sx == scale;
}

static cairo_t *_begin(dt_presenter_backend_t *self,
                       GtkWidget *widget,
                       void **data,
                       cairo_t *widget_cr)
{
  _window_t *w = (_window_t *)self;
  _widget_t *l = *data;
  if(!l)
  {
    l = g_malloc0(sizeof(_widget_t));
    l->widget = widget;
    l->window = w;
    l->kept_images = g_array_new(FALSE, FALSE, sizeof(dt_presenter_image_t));
    l->images = g_array_new(FALSE, FALSE, sizeof(dt_presenter_image_t));
    l->shown = g_array_new(FALSE, FALSE, sizeof(_shown_t));
    w->widgets = g_list_prepend(w->widgets, l);
    *data = l;
  }

  const int width = gtk_widget_get_allocated_width(l->widget);
  const int height = gtk_widget_get_allocated_height(l->widget);
  const int scale = gtk_widget_get_scale_factor(l->widget);

  if(l->cr) cairo_destroy(l->cr);
  // the last draw is kept: draw on the other surface
  if(l->foreground && l->foreground == l->kept)
  {
    cairo_surface_destroy(l->foreground);
    l->foreground = l->spare;
    l->spare = NULL;
  }
  // a failed allocation leaves cairo's error surface: try again. a context
  // on it is in error too, so presenter.c's user data does not stick and
  // the draw presents nothing
  if(!_fits(l->foreground, width, height, scale))
  {
    _surface_destroy(&l->foreground);
    l->foreground = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                               width * scale, height * scale);
    cairo_surface_set_device_scale(l->foreground, scale, scale);
  }
  l->width = width;
  l->height = height;
  l->scale = scale;
  l->cr = cairo_create(l->foreground);
  cairo_save(l->cr);
  cairo_set_operator(l->cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint(l->cr);
  cairo_restore(l->cr);
  return l->cr;
}

// the device pixels an image covers lie in box (x0, y0, x1, y1), and are
// those whose center inverse maps into the frame
static gboolean _coverage(const _widget_t *l,
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

static void _cut_out(_widget_t *l,
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

// the frame as the window shows it, float for the compose
static gboolean _rendition(const _widget_t *l,
                           dt_presenter_image_t *image)
{
  dt_display_output_t output;
  dt_presenter_window_output(l->window->window, &output);
  image->pixels = dt_frame_float(image->frame, &output.image);
  return image->pixels != NULL;
}

// an image takes the pixels it covers: clear them in the foreground
static gboolean _image(dt_presenter_backend_t *self,
                       void *data,
                       cairo_t *cr,
                       const double x,
                       const double y,
                       dt_presenter_image_t *image)
{
  _widget_t *l = data;
  if(!_rendition(l, image)) return FALSE;
  _cut_out(l, image);
  return TRUE;
}

// the kept foreground, with its images cut out and everything drawn above
// them, replaces what this draw has drawn so far; its images go below
// whatever this draw places later
static gboolean _keep(dt_presenter_backend_t *self,
                      void *data,
                      cairo_t *cr,
                      const cairo_matrix_t *base,
                      GArray *images)
{
  _widget_t *l = data;
  if(!l || !l->kept) return FALSE;

  cairo_save(cr);
  cairo_set_matrix(cr, base);
  cairo_set_source_surface(cr, l->kept, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
  double sx, sy;
  cairo_surface_get_device_scale(l->kept, &sx, &sy);
  cairo_rectangle(cr, 0, 0, cairo_image_surface_get_width(l->kept) / sx,
                  cairo_image_surface_get_height(l->kept) / sy);
  cairo_clip(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_paint(cr);
  cairo_restore(cr);

  for(guint k = 0; k < l->kept_images->len; k++)
  {
    // in the output the window has now, which may not be the one the kept
    // draw had
    dt_presenter_image_t image = g_array_index(l->kept_images, dt_presenter_image_t, k);
    if(_rendition(l, &image)) g_array_append_val(images, image);
  }
  return TRUE;
}

static void _compose(_widget_t *l,
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

static gboolean _image_changed(const _widget_t *l,
                               const int width,
                               const int height,
                               const float headroom)
{
  if(width != l->shown_width || height != l->shown_height || l->scale != l->shown_scale
     || headroom != l->shown_headroom || l->images->len != l->shown->len)
    return TRUE;
  for(guint k = 0; k < l->images->len; k++)
  {
    const dt_presenter_image_t *i = &g_array_index(l->images, dt_presenter_image_t, k);
    const _shown_t *s = &g_array_index(l->shown, _shown_t, k);
    if(dt_frame_id(i->frame) != s->id || i->pixels != s->pixels || i->filter != s->filter
       || memcmp(&i->matrix, &s->matrix, sizeof(cairo_matrix_t))
       || memcmp(i->clip, s->clip, sizeof(s->clip)))
      return TRUE;
  }
  return FALSE;
}

// what a replay covers in a kept foreground, put back after the present
static cairo_surface_t *_save(const _widget_t *l,
                              const int x,
                              const int y,
                              const int width,
                              const int height)
{
  const int x0 = MAX(0, x), y0 = MAX(0, y);
  const int x1 = MIN(l->width, x + width), y1 = MIN(l->height, y + height);
  if(x0 >= x1 || y0 >= y1) return NULL;

  const int s = l->scale;
  cairo_surface_t *patch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                      (x1 - x0) * s, (y1 - y0) * s);
  cairo_surface_set_device_scale(patch, s, s);
  // in the widget's user space, as the foreground
  cairo_surface_set_device_offset(patch, -x0 * s, -y0 * s);
  cairo_t *cr = cairo_create(patch);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface(cr, l->foreground, 0, 0);
  cairo_paint(cr);
  cairo_destroy(cr);
  return patch;
}

static void _restore(_widget_t *l,
                     GSList *saved)
{
  cairo_t *cr = cairo_create(l->foreground);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  // the last saved first: it saved what the replays before it drew
  for(GSList *p = saved; p; p = g_slist_next(p))
  {
    cairo_surface_t *patch = p->data;
    double ox, oy, sx, sy;
    cairo_surface_get_device_offset(patch, &ox, &oy);
    cairo_surface_get_device_scale(patch, &sx, &sy);
    cairo_save(cr);
    cairo_rectangle(cr, -ox / sx, -oy / sy, cairo_image_surface_get_width(patch) / sx,
                    cairo_image_surface_get_height(patch) / sy);
    cairo_clip(cr);
    cairo_set_source_surface(cr, patch, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
  }
  cairo_destroy(cr);
  g_slist_free_full(saved, (GDestroyNotify)cairo_surface_destroy);
}

typedef struct _gather_t
{
  // the container's child on the way to the presenting widget
  GtkWidget *child;
  gboolean popovers, above;
  GPtrArray *widgets;
} _gather_t;

static void _gather(GtkWidget *widget,
                    gpointer data)
{
  _gather_t *g = data;
  if(widget == g->child)
    g->above = TRUE;
  else if(g->popovers ? GTK_IS_POPOVER(widget) : g->above)
    g_ptr_array_add(g->widgets, widget);
}

// what GTK3 draws above widget, bottom to top: the children that each
// GtkOverlay on the way up stacks above it, and with popovers, the
// toplevel's popovers, which GtkWindow draws above its content and lists
// bottom to top. gtk_container_forall() gives a container's children in
// the order it draws them, internal ones such as popovers included
static void _above(GtkWidget *widget,
                   const gboolean popovers,
                   GPtrArray *widgets)
{
  GtkWidget *child = widget;
  for(GtkWidget *parent = gtk_widget_get_parent(child);
      parent;
      child = parent, parent = gtk_widget_get_parent(parent))
  {
    const gboolean toplevel = popovers && GTK_IS_WINDOW(parent);
    if(!toplevel && !GTK_IS_OVERLAY(parent)) continue;

    _gather_t g = { .child = child, .popovers = toplevel, .widgets = widgets };
    gtk_container_forall(GTK_CONTAINER(parent), _gather, &g);
  }
}

static void _redraw_widgets(_window_t *w)
{
  for(GList *l = w->widgets; l; l = g_list_next(l))
    gtk_widget_queue_draw(((_widget_t *)l->data)->widget);
}

static void _popover_mapped(GtkWidget *popover,
                            _window_t *w)
{
  _redraw_widgets(w);
}

static void _popover_allocated(GtkWidget *popover,
                               GdkRectangle *allocation,
                               _window_t *w)
{
  _redraw_widgets(w);
}

static void _popover_invalidated(GdkWindow *window,
                                 cairo_region_t *region)
{
  _window_t *w = g_object_get_data(G_OBJECT(window), POPOVER_KEY);
  if(w) _redraw_widgets(w);
}

// a popover drawn into the toplevel is not part of any presenting widget,
// whose draws are what present the layers: draw them again when it is
// shown, hidden, moved or animated, or its content changes
static void _watch_popover(_window_t *w,
                           GtkWidget *popover)
{
  if(g_object_get_data(G_OBJECT(popover), POPOVER_KEY) != w)
  {
    g_object_set_data(G_OBJECT(popover), POPOVER_KEY, w);
    g_signal_connect(popover, "map", G_CALLBACK(_popover_mapped), w);
    g_signal_connect(popover, "unmap", G_CALLBACK(_popover_mapped), w);
    g_signal_connect(popover, "size-allocate", G_CALLBACK(_popover_allocated), w);
    GtkWidget **slot = g_new(GtkWidget *, 1);
    *slot = popover;
    g_object_add_weak_pointer(G_OBJECT(popover), (gpointer *)slot);
    g_ptr_array_add(w->popovers, slot);
  }
  // its window exists while it is realized, and may be another one later
  GdkWindow *window = gtk_widget_get_window(popover);
  if(window && g_object_get_data(G_OBJECT(window), POPOVER_KEY) != w)
  {
    g_object_set_data(G_OBJECT(window), POPOVER_KEY, w);
    gdk_window_set_invalidate_handler(window, _popover_invalidated);
  }
}

static void _unwatch_popovers(_window_t *w)
{
  for(guint k = 0; k < w->popovers->len; k++)
  {
    GtkWidget **slot = g_ptr_array_index(w->popovers, k);
    if(*slot)
    {
      g_signal_handlers_disconnect_by_data(*slot, w);
      g_object_set_data(G_OBJECT(*slot), POPOVER_KEY, NULL);
      GdkWindow *window = gtk_widget_get_window(*slot);
      if(window && g_object_get_data(G_OBJECT(window), POPOVER_KEY) == w)
      {
        gdk_window_set_invalidate_handler(window, NULL);
        g_object_set_data(G_OBJECT(window), POPOVER_KEY, NULL);
      }
      g_object_remove_weak_pointer(G_OBJECT(*slot), (gpointer *)slot);
    }
    g_free(slot);
  }
  g_ptr_array_set_size(w->popovers, 0);
}

// where GTK draws widget, in the presenting widget's coordinates. a
// popover is allocated in a window of its own, a child of the toplevel's
// (a subsurface on Wayland), at the position GtkWindow gives it
static gboolean _origin(const _widget_t *l,
                        GtkWidget *widget,
                        int *x,
                        int *y)
{
  if(!GTK_IS_POPOVER(widget))
    return gtk_widget_translate_coordinates(widget, l->widget, 0, 0, x, y);

  GdkWindow *window = gtk_widget_get_parent_window(widget);
  if(!window) return FALSE;
  int wx, wy;
  gdk_window_get_position(window, &wx, &wy);
  GtkAllocation a;
  gtk_widget_get_allocation(widget, &a);
  return gtk_widget_translate_coordinates(l->window->window->toplevel, l->widget,
                                          wx + a.x, wy + a.y, x, y);
}

// GTK3 draws what it stacks above a presenting widget into its own
// surface, which the layers cover: draw it into the foreground too. the
// real widgets stay below and keep getting the input, since the layers
// take none. with saved, what each replay covers is saved first
static void _replay_above(_widget_t *l,
                          GSList **saved)
{
  GPtrArray *above = g_ptr_array_new();
  _above(l->widget, l->window->replay_popovers, above);
  for(guint k = 0; k < above->len; k++)
  {
    GtkWidget *widget = g_ptr_array_index(above, k);
    if(GTK_IS_POPOVER(widget)) _watch_popover(l->window, widget);

    // GTK draws a widget within its clip, which a CSS shadow can take
    // beyond its allocation (gtkwidget.c, gtk_widget_draw_internal())
    int x, y;
    GtkAllocation a, clip;
    gtk_widget_get_allocation(widget, &a);
    gtk_widget_get_clip(widget, &clip);
    if(!gtk_widget_is_drawable(widget) || !_origin(l, widget, &x, &y)) continue;
    const int cx = x + clip.x - a.x, cy = y + clip.y - a.y;
    if(cx >= l->width || cy >= l->height || cx + clip.width <= 0 || cy + clip.height <= 0)
      continue;

    if(saved)
    {
      cairo_surface_t *patch = _save(l, cx, cy, clip.width, clip.height);
      if(patch) *saved = g_slist_prepend(*saved, patch);
    }
    cairo_save(l->cr);
    cairo_translate(l->cr, x, y);
    gtk_widget_draw(widget, l->cr);
    cairo_restore(l->cr);
  }
  g_ptr_array_free(above, TRUE);
}

static void _present(_widget_t *l)
{
  l->pending = FALSE;
  const int width = l->width * l->scale;
  const int height = l->height * l->scale;

  dt_presenter_transport_t *transport = l->window->transport;
  // a new headroom encodes the image again, also when the frames stay
  const float headroom = transport->headroom(transport);
  const gboolean changed = _image_changed(l, width, height, headroom);
  if(changed)
  {
    _compose(l, width, height);
    if(!l->image) return;
    l->shown_scale = l->scale;
    l->shown_headroom = headroom;
    g_array_set_size(l->shown, l->images->len);
    for(guint k = 0; k < l->images->len; k++)
    {
      const dt_presenter_image_t *i = &g_array_index(l->images, dt_presenter_image_t, k);
      _shown_t *s = &g_array_index(l->shown, _shown_t, k);
      *s = (_shown_t){ .id = dt_frame_id(i->frame), .pixels = i->pixels,
                       .matrix = i->matrix, .filter = i->filter };
      memcpy(s->clip, i->clip, sizeof(s->clip));
    }
  }

  // a kept foreground is the widget's drawing alone: what is replayed
  // above it is not part of the presentation a later draw shows again
  GSList *saved = NULL;
  const gboolean kept = l->foreground == l->kept;
  _replay_above(l, kept ? &saved : NULL);
  cairo_surface_flush(l->foreground);

  dt_presenter_layers_t layers = { .image = changed ? l->image : NULL,
                                   .foreground =
                                     cairo_image_surface_get_data(l->foreground),
                                   .width = width,
                                   .height = height,
                                   .scale = l->scale,
                                   .headroom = headroom };
  gtk_widget_translate_coordinates(l->widget, l->window->window->toplevel, 0, 0,
                                   &layers.x, &layers.y);
  if(!transport->present(transport, l->widget, &l->transport_data, &layers))
  {
    // not shown: the image has to go out with the next present
    g_array_set_size(l->shown, 0);
    l->shown_width = l->shown_height = 0;
  }
  if(saved) _restore(l, saved);
}

static void _hide(_widget_t *l)
{
  l->pending = FALSE;
  g_array_set_size(l->images, 0);
  g_array_set_size(l->shown, 0);
  l->shown_width = l->shown_height = 0;
  dt_presenter_transport_t *transport = l->window->transport;
  if(l->transport_data) transport->clear(transport, &l->transport_data);
}

static void _end(dt_presenter_backend_t *self,
                 void *data,
                 cairo_t *widget_cr,
                 const cairo_matrix_t *base,
                 const GArray *images,
                 const dt_presenter_kept_t kept)
{
  _widget_t *l = data;
  g_array_set_size(l->images, 0);
  g_array_append_vals(l->images, images->data, images->len);

  if(kept == DT_PRESENTER_KEPT_NONE)
    _drop_kept(l);
  else if(kept == DT_PRESENTER_KEPT_THIS)
  {
    // begin() drew on another surface than the kept one, which becomes the
    // spare: two surfaces alternate while every draw is kept
    _surface_destroy(&l->spare);
    l->spare = l->kept;
    l->kept = cairo_surface_reference(l->foreground);
    for(guint k = 0; k < images->len; k++)
      dt_frame_ref(g_array_index(images, dt_presenter_image_t, k).frame);
    _images_unref(l->kept_images);
    g_array_append_vals(l->kept_images, images->data, images->len);
  }

  if(images->len)
  {
    l->pending = TRUE;
    return;
  }

  // nothing to show on the layers: what was drawn goes to GTK's surface
  _hide(l);
  cairo_surface_flush(l->foreground);
  cairo_save(widget_cr);
  cairo_set_source_surface(widget_cr, l->foreground, 0, 0);
  cairo_paint(widget_cr);
  cairo_restore(widget_cr);
}

static void _clear(dt_presenter_backend_t *self,
                   void *data)
{
  _widget_t *l = data;
  if(!l) return;
  _hide(l);
  _drop_kept(l);
}

static void _release(dt_presenter_backend_t *self,
                     void **data)
{
  _window_t *w = (_window_t *)self;
  _widget_t *l = *data;
  if(!l) return;
  if(l->transport_data) w->transport->release(w->transport, &l->transport_data);
  if(l->cr) cairo_destroy(l->cr);
  _surface_destroy(&l->foreground);
  _drop_kept(l);
  g_array_free(l->kept_images, TRUE);
  g_array_free(l->images, TRUE);
  g_array_free(l->shown, TRUE);
  dt_free_align(l->image);
  w->widgets = g_list_remove(w->widgets, l);
  g_free(l);
  *data = NULL;
}

static void _free(dt_presenter_backend_t *self)
{
  _window_t *w = (_window_t *)self;
  _unwatch_popovers(w);
  g_ptr_array_free(w->popovers, TRUE);
  w->transport->free(w->transport);
  g_list_free(w->widgets);
  g_free(w);
}

dt_presenter_backend_t *dt_presenter_layered_new(dt_presenter_window_t *window)
{
  dt_presenter_transport_t *transport = NULL;
#ifdef HAVE_WAYLAND
  transport = dt_presenter_wayland_new(window->toplevel, window);
  // the window paints with cairo on its system display profile. NULL takes
  // back what a failed setup reported
  if(!transport) dt_presenter_window_set_output(window, NULL);
#endif
  if(!transport) return NULL;

  _window_t *w = g_malloc0(sizeof(_window_t));
  w->window = window;
  w->transport = transport;
  w->replay_popovers = !transport->popovers_above;
  w->popovers = g_ptr_array_new();
  w->backend = (dt_presenter_backend_t){ .begin = _begin,
                                         .image = _image,
                                         .keep = _keep,
                                         .end = _end,
                                         .clear = _clear,
                                         .release = _release,
                                         .free = _free };
  return &w->backend;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
