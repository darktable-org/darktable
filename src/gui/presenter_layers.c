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
#include "common/math.h"
#include "gui/gtk.h"

#if defined(__F16C__) && defined(__AVX__)
#include <immintrin.h>
#endif

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

// what an image covers of its widget: the alpha cairo paints with a
// source of the frame's size and alpha 1, the image's matrix and filter,
// through the image's clip. images placed alike share it
typedef struct _coverage_t
{
  int refs;
  // the placement, and the widget's device scale
  int frame_width, frame_height;
  cairo_matrix_t matrix;
  cairo_filter_t filter;
  int scale;
  // the device pixels (x0, y0, x1, y1) outside which nothing is covered,
  // within the widget as it was drawn, which may be larger than now; all
  // 0 if none
  int box[4];
  // what the clip covers of the box, as _runs()
  uint32_t *clip;
  size_t clip_runs;
  // the coverage in the box, a byte per pixel, on an A8 surface
  cairo_surface_t *plane;
  uint8_t *alpha;
  int stride;
  // its pixels of fractional coverage, 0 < alpha < 255, numbered row by
  // row: the number of the first one of each row of the box, then their
  // count
  int *fractional;
} _coverage_t;

// an image as the layered backend shows it, with a reference held by
// each draw, kept presentation and composition that has it
typedef struct _record_t
{
  int refs;
  _coverage_t *coverage;
  // the foreground's alpha at each pixel of fractional coverage, before
  // the image cut it out: the compose weighs the image with it
  uint8_t *backdrop;
} _record_t;

// what an image layer was composed from, to know when to compose again
typedef struct _shown_t
{
  // a frame and its rendition: a kept frame is converted again for a new
  // output
  uint32_t id;
  const float *pixels;
  _record_t *record;
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
  // and its images, with a reference on each frame and record. while the
  // last draw is kept, spare is the surface the next one draws on
  cairo_surface_t *kept, *spare;
  GArray *kept_images;
  // the images of the last draw, with a reference on each record
  GArray *images;
  // the records of the draw in progress
  GPtrArray *drawing;
  // what the image layer holds, and the encoding it is in
  GArray *shown;
  int shown_width, shown_height, shown_scale;
  dt_presenter_encoding_t shown_encoding;
  // alpha 1 for the coverage sources, the bytes of the largest frame
  uint8_t *opaque;
  size_t opaque_size;
  // drawn and not presented yet
  gboolean pending;
  void *transport_data;
} _widget_t;

// a coverage's key: placement, box and clip
static void _key_clear(_coverage_t *c)
{
  if(c->plane) cairo_surface_destroy(c->plane);
  g_free(c->clip);
}

static void _coverage_unref(_coverage_t *c)
{
  if(!c || --c->refs) return;
  _key_clear(c);
  g_free(c->fractional);
  g_free(c);
}

static _record_t *_record_ref(_record_t *r)
{
  r->refs++;
  return r;
}

static void _record_unref(gpointer data)
{
  _record_t *r = data;
  if(!r || --r->refs) return;
  _coverage_unref(r->coverage);
  g_free(r->backdrop);
  g_free(r);
}

static void _surface_destroy(cairo_surface_t **surface)
{
  if(*surface) cairo_surface_destroy(*surface);
  *surface = NULL;
}

// the kept images, with their references on frames and records
static void _images_unref(GArray *images)
{
  for(guint k = 0; k < images->len; k++)
  {
    dt_presenter_image_t *image = &g_array_index(images, dt_presenter_image_t, k);
    dt_frame_unref(image->frame);
    _record_unref(image->data);
  }
  g_array_set_size(images, 0);
}

static void _shown_clear(_widget_t *l)
{
  for(guint k = 0; k < l->shown->len; k++)
    _record_unref(g_array_index(l->shown, _shown_t, k).record);
  g_array_set_size(l->shown, 0);
  l->shown_width = l->shown_height = 0;
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
    l->drawing = g_ptr_array_new_with_free_func(_record_unref);
    l->shown = g_array_new(FALSE, FALSE, sizeof(_shown_t));
    w->widgets = g_list_prepend(w->widgets, l);
    *data = l;
  }

  const int width = gtk_widget_get_allocated_width(l->widget);
  const int height = gtk_widget_get_allocated_height(l->widget);
  const int scale = gtk_widget_get_scale_factor(l->widget);

  if(l->cr) cairo_destroy(l->cr);
  g_ptr_array_set_size(l->drawing, 0);
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

// cairo's filters reach past the frame's edge by about half a source
// pixel when enlarging and a device pixel when reducing: a box this many
// device pixels larger than the frame holds all its coverage, and pixels
// this far inside it are covered whole (test_presenter_compose checks
// both against cairo)
static double _reach(const cairo_matrix_t *forward)
{
  return 2.0 + fmax(fabs(forward->xx) + fabs(forward->xy),
                    fabs(forward->yx) + fabs(forward->yy));
}

// the rectangle (x0, y0, x1, y1) maps into the frame
static gboolean _maps_inside(const cairo_matrix_t *inverse,
                             const double r[4],
                             const int frame_width,
                             const int frame_height)
{
  const double corners[4][2] = { { r[0], r[1] }, { r[2], r[1] }, { r[0], r[3] }, { r[2], r[3] } };
  for(int k = 0; k < 4; k++)
  {
    double u = corners[k][0], v = corners[k][1];
    cairo_matrix_transform_point(inverse, &u, &v);
    if(u < 0.0 || v < 0.0 || u > frame_width || v > frame_height) return FALSE;
  }
  return TRUE;
}

/*
 * what cr's clip covers within box, device pixels (x0, y0, x1, y1) of the
 * widget: cairo paints alpha 1 into a group of alpha content, which takes
 * cr's clip, of any shape and with fractional edges, as painting on the
 * widget does; it is read, not painted back, which would apply the clip
 * twice. box becomes the group's extents, all 0 if the clip covers
 * nothing there. NULL if out of memory
 */
static cairo_surface_t *_clip_coverage(cairo_t *cr,
                                       int box[4],
                                       const int scale)
{
  cairo_save(cr);
  cairo_identity_matrix(cr);
  cairo_rectangle(cr, (double)box[0] / scale, (double)box[1] / scale,
                  (double)(box[2] - box[0]) / scale, (double)(box[3] - box[1]) / scale);
  cairo_clip(cr);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);
  cairo_push_group_with_content(cr, CAIRO_CONTENT_ALPHA);
  cairo_surface_t *group = cairo_surface_reference(cairo_get_group_target(cr));
  cairo_paint(cr);
  cairo_pattern_destroy(cairo_pop_group(cr));
  cairo_restore(cr);
  cairo_surface_flush(group);

  // a group of an image surface is an A8 image surface, offset to where
  // it lies on the widget's (cairo-default-context.c, push_group), or an
  // empty one if all is clipped
  const gboolean image = cairo_surface_status(group) == CAIRO_STATUS_SUCCESS
    && cairo_surface_get_type(group) == CAIRO_SURFACE_TYPE_IMAGE;
  const int width = image ? cairo_image_surface_get_width(group) : 0;
  const int height = image ? cairo_image_surface_get_height(group) : 0;
  if(!image || (width && height && cairo_image_surface_get_format(group) != CAIRO_FORMAT_A8))
  {
    cairo_surface_destroy(group);
    return NULL;
  }
  if(!width || !height)
  {
    memset(box, 0, 4 * sizeof(int));
    return group;
  }
  double ox, oy;
  cairo_surface_get_device_offset(group, &ox, &oy);
  box[0] = -(int)lround(ox);
  box[1] = -(int)lround(oy);
  box[2] = box[0] + width;
  box[3] = box[1] + height;
  return group;
}

// a plane as runs of equal bytes, row by row, each length << 8 | value:
// two clips cover a box alike if their runs are the same
static uint32_t *_runs(const uint8_t *plane,
                       const int stride,
                       const int width,
                       const int height,
                       size_t *count)
{
  GArray *runs = g_array_new(FALSE, FALSE, sizeof(uint32_t));
  for(int y = 0; y < height; y++)
  {
    const uint8_t *row = plane + (size_t)stride * y;
    for(int x = 0; x < width;)
    {
      const uint8_t v = row[x];
      const uint64_t same = 0x0101010101010101ull * v;
      int end = x + 1;
      for(; end + 8 <= width; end += 8)
      {
        uint64_t word;
        memcpy(&word, row + end, 8);
        if(word != same) break;
      }
      while(end < width && row[end] == v) end++;
      const uint32_t run = (uint32_t)(end - x) << 8 | v;
      g_array_append_val(runs, run);
      x = end;
    }
  }
  *count = runs->len;
  return (uint32_t *)g_array_free(runs, FALSE);
}

/*
 * the key of the coverage of a frame painted on cr: the placement (frame
 * pixels to the widget's user space, which is cr's untransformed one),
 * the box the frame can reach within the widget's device pixels, and in
 * it the clip's coverage, cairo's paint of alpha 1 through cr's clip, which
 * is also the frame's coverage wherever the frame covers whole. FALSE if
 * out of memory
 */
static gboolean _coverage_key(cairo_t *cr,
                              const int frame_width,
                              const int frame_height,
                              const cairo_matrix_t *matrix,
                              const cairo_filter_t filter,
                              const int scale,
                              const int width,
                              const int height,
                              _coverage_t *key)
{
  *key = (_coverage_t){ .refs = 1, .frame_width = frame_width, .frame_height = frame_height,
                        .matrix = *matrix, .filter = filter, .scale = scale };
  cairo_matrix_t device, forward, inverse;
  cairo_matrix_init_scale(&device, scale, scale);
  cairo_matrix_multiply(&forward, matrix, &device);
  inverse = forward;
  // a singular placement covers nothing
  if(cairo_matrix_invert(&inverse) != CAIRO_STATUS_SUCCESS) return TRUE;

  double x0 = DBL_MAX, y0 = DBL_MAX, x1 = -DBL_MAX, y1 = -DBL_MAX;
  const double corners[4][2] = { { 0, 0 }, { frame_width, 0 },
                                 { 0, frame_height }, { frame_width, frame_height } };
  for(int k = 0; k < 4; k++)
  {
    double x = corners[k][0], y = corners[k][1];
    cairo_matrix_transform_point(&forward, &x, &y);
    x0 = fmin(x0, x);
    y0 = fmin(y0, y);
    x1 = fmax(x1, x);
    y1 = fmax(y1, y);
  }
  const double reach = _reach(&forward);
  int *const box = key->box;
  box[0] = (int)floor(fmax(x0 - reach, 0.0));
  box[1] = (int)floor(fmax(y0 - reach, 0.0));
  box[2] = (int)ceil(fmin(x1 + reach, width));
  box[3] = (int)ceil(fmin(y1 + reach, height));
  if(box[0] >= box[2] || box[1] >= box[3])
  {
    memset(box, 0, sizeof(key->box));
    return TRUE;
  }

  cairo_surface_t *plane = _clip_coverage(cr, box, scale);
  if(!plane) return FALSE;
  if(box[0] == box[2])
  {
    cairo_surface_destroy(plane);
    return TRUE;
  }
  key->plane = plane;
  key->alpha = cairo_image_surface_get_data(plane);
  key->stride = cairo_image_surface_get_stride(plane);
  key->clip = _runs(key->alpha, key->stride, box[2] - box[0], box[3] - box[1], &key->clip_runs);
  return TRUE;
}

static gboolean _coverage_equal(const _coverage_t *a,
                                const _coverage_t *b)
{
  return a == b
    || (a->frame_width == b->frame_width && a->frame_height == b->frame_height
        && a->filter == b->filter && a->scale == b->scale
        && !memcmp(&a->matrix, &b->matrix, sizeof(cairo_matrix_t))
        && !memcmp(a->box, b->box, sizeof(a->box)) && a->clip_runs == b->clip_runs
        && (!a->clip_runs || !memcmp(a->clip, b->clip, sizeof(uint32_t) * a->clip_runs)));
}

// the pixels of a row of fractional coverage, 0 < alpha < 255
static int _fractional_in(const uint8_t *row,
                          const int width)
{
  int n = 0, x = 0;
  for(uint64_t word; x + 8 <= width; x += 8)
  {
    memcpy(&word, row + x, 8);
    if(word == 0 || word == UINT64_MAX) continue;
    for(int k = 0; k < 8; k++) n += row[x + k] && row[x + k] < 255;
  }
  for(; x < width; x++) n += row[x] && row[x] < 255;
  return n;
}

#define TILE 64

/*
 * the coverage of key, from source, of the frame's size and alpha 1. the
 * key's clip coverage is the coverage of the tiles the frame covers whole;
 * in the others cairo paints the frame unclipped, and the clip's coverage
 * multiplies it, as cairo's paint through a clip does. it takes the key's
 * plane and runs. NULL if out of memory
 */
static _coverage_t *_coverage_new(cairo_surface_t *source,
                                  const _coverage_t *key)
{
  _coverage_t *c = g_malloc(sizeof(_coverage_t));
  *c = *key;
  const int *box = c->box;
  const int bw = box[2] - box[0], bh = box[3] - box[1];
  c->fractional = g_try_new0(int, bh + 1);
  if(!c->fractional)
  {
    _coverage_unref(c);
    return NULL;
  }
  if(!bh) return c;

  cairo_matrix_t device, forward, inverse;
  cairo_matrix_init_scale(&device, c->scale, c->scale);
  cairo_matrix_multiply(&forward, &c->matrix, &device);
  inverse = forward;
  cairo_matrix_invert(&inverse);
  const double reach = _reach(&forward);

  uint8_t frame[TILE * TILE];
  const int tiles_x = (bw + TILE - 1) / TILE, tiles_y = (bh + TILE - 1) / TILE;
  for(int ty = 0; ty < tiles_y; ty++)
    for(int tx = 0; tx < tiles_x; tx++)
    {
      const int px = tx * TILE, py = ty * TILE;
      const int pw = MIN(TILE, bw - px), ph = MIN(TILE, bh - py);
      const double r[4] = { box[0] + px, box[1] + py, box[0] + px + pw, box[1] + py + ph };
      const double reached[4] = { r[0] - reach, r[1] - reach, r[2] + reach, r[3] + reach };
      if(_maps_inside(&inverse, reached, c->frame_width, c->frame_height)) continue;

      // a surface on the tile, which cairo paints as it paints the widget:
      // same user space and device scale
      memset(frame, 0, sizeof(frame));
      cairo_surface_t *tile =
        cairo_image_surface_create_for_data(frame, CAIRO_FORMAT_A8, pw, ph, TILE);
      cairo_surface_set_device_scale(tile, c->scale, c->scale);
      cairo_surface_set_device_offset(tile, -r[0], -r[1]);
      cairo_t *cr = cairo_create(tile);
      cairo_set_matrix(cr, &c->matrix);
      cairo_set_source_surface(cr, source, 0, 0);
      cairo_pattern_set_filter(cairo_get_source(cr), c->filter);
      cairo_paint(cr);
      cairo_destroy(cr);
      cairo_surface_flush(tile);
      cairo_surface_destroy(tile);

      for(int y = 0; y < ph; y++)
      {
        uint8_t *out = c->alpha + (size_t)c->stride * (py + y) + px;
        for(int x = 0; x < pw; x++)
        {
          // rounded to nearest, as pixman's products
          const uint32_t t = out[x] * frame[TILE * y + x] + 128;
          out[x] = (t + (t >> 8)) >> 8;
        }
      }
    }
  cairo_surface_mark_dirty(c->plane);

  int *const fractional = c->fractional;
  const uint8_t *const alpha = c->alpha;
  const int stride = c->stride;
  DT_OMP_FOR()
  for(int y = 0; y < bh; y++)
    fractional[y + 1] = _fractional_in(alpha + (size_t)stride * y, bw);
  for(int y = 0; y < bh; y++) fractional[y + 1] += fractional[y];
  return c;
}

static _coverage_t *_coverage_find(const GArray *images,
                                   const _coverage_t *key)
{
  for(guint k = 0; k < images->len; k++)
  {
    const _record_t *r = g_array_index(images, dt_presenter_image_t, k).data;
    if(r && _coverage_equal(r->coverage, key)) return r->coverage;
  }
  return NULL;
}

// a source of the frame's size and alpha 1: the widget's opaque buffer,
// which grows to the bytes of the largest frame
static cairo_surface_t *_opaque(_widget_t *l,
                                const int width,
                                const int height)
{
  const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_A8, width);
  const size_t size = (size_t)stride * height;
  if(size > l->opaque_size)
  {
    g_free(l->opaque);
    l->opaque = g_try_malloc(size);
    l->opaque_size = l->opaque ? size : 0;
    if(!l->opaque) return NULL;
    memset(l->opaque, 255, size);
  }
  return cairo_image_surface_create_for_data(l->opaque, CAIRO_FORMAT_A8, width, height, stride);
}

// the coverage of image, clipped as cr is, shared with an image of this
// draw, the last one or the kept one placed alike
static _coverage_t *_coverage_get(_widget_t *l,
                                  cairo_t *cr,
                                  const dt_presenter_image_t *image)
{
  const dt_frame_geometry_t *frame = dt_frame_geometry(image->frame);
  _coverage_t key;
  if(!_coverage_key(cr, frame->width, frame->height, &image->matrix, image->filter, l->scale,
                    l->width * l->scale, l->height * l->scale, &key))
    return NULL;

  _coverage_t *c = NULL;
  for(guint k = 0; k < l->drawing->len && !c; k++)
  {
    _record_t *r = g_ptr_array_index(l->drawing, k);
    if(_coverage_equal(r->coverage, &key)) c = r->coverage;
  }
  if(!c) c = _coverage_find(l->images, &key);
  if(!c) c = _coverage_find(l->kept_images, &key);
  if(c)
  {
    _key_clear(&key);
    c->refs++;
    return c;
  }

  cairo_surface_t *source = _opaque(l, frame->width, frame->height);
  if(!source)
  {
    _key_clear(&key);
    return NULL;
  }
  c = _coverage_new(source, &key);
  cairo_surface_destroy(source);
  return c;
}

// the image takes what it covers: F <- (1 - a) F, after keeping the
// foreground's alpha where a is fractional, which weighs the image in the
// compose. data is the foreground's, ARGB32 of width x height
static void _cut(uint8_t *const data,
                 const int width,
                 const int height,
                 const int stride,
                 const _coverage_t *c,
                 uint8_t *const backdrop)
{
  const int *box = c->box;
  const int x1 = MIN(box[2], width), y1 = MIN(box[3], height);
  DT_OMP_FOR()
  for(int y = box[1]; y < y1; y++)
  {
    uint32_t *row = (uint32_t *)(data + (size_t)stride * y);
    const uint8_t *alpha = c->alpha + (size_t)c->stride * (y - box[1]) - box[0];
    int k = c->fractional[y - box[1]];
    // a row cut short leaves its last numbers unused
    for(int x = box[0]; x < x1; x++)
    {
      const uint32_t a = alpha[x];
      if(a == 0) continue;
      if(a == 255)
      {
        row[x] = 0;
        continue;
      }
      const uint32_t p = row[x];
      backdrop[k++] = p >> 24;
      uint32_t cut = 0;
      for(int s = 0; s < 32; s += 8)
      {
        // (p * (255 - a)) / 255 rounded to nearest, as cairo's own products
        const uint32_t t = ((p >> s) & 0xff) * (255 - a) + 128;
        cut |= ((t + (t >> 8)) >> 8) << s;
      }
      row[x] = cut;
    }
  }
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

// paint image, clipped as cr is, where it covers the foreground: cut it
// out and record it
static gboolean _place(_widget_t *l,
                       cairo_t *cr,
                       dt_presenter_image_t *image)
{
  _coverage_t *c = _coverage_get(l, cr, image);
  if(!c) return FALSE;

  _record_t *r = g_malloc0(sizeof(_record_t));
  r->refs = 1;
  r->coverage = c;
  r->backdrop = g_malloc(c->fractional[c->box[3] - c->box[1]]);
  cairo_surface_flush(l->foreground);
  _cut(cairo_image_surface_get_data(l->foreground), l->width * l->scale,
       l->height * l->scale, cairo_image_surface_get_stride(l->foreground), c, r->backdrop);
  cairo_surface_mark_dirty(l->foreground);
  g_ptr_array_add(l->drawing, r);
  image->data = r;
  return TRUE;
}

static gboolean _image(dt_presenter_backend_t *self,
                       void *data,
                       cairo_t *cr,
                       const double x,
                       const double y,
                       dt_presenter_image_t *image)
{
  _widget_t *l = data;
  return _rendition(l, image) && _place(l, cr, image);
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

  // kept at another device scale, its images' cuts and coverages are of
  // that scale, and the clips they were painted through are gone: it
  // cannot be shown again
  double sx, sy;
  cairo_surface_get_device_scale(l->kept, &sx, &sy);
  if(sx != l->scale)
  {
    _drop_kept(l);
    return FALSE;
  }

  cairo_save(cr);
  cairo_set_matrix(cr, base);
  cairo_set_source_surface(cr, l->kept, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
  cairo_rectangle(cr, 0, 0, cairo_image_surface_get_width(l->kept) / sx,
                  cairo_image_surface_get_height(l->kept) / sy);
  cairo_clip(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_paint(cr);
  cairo_restore(cr);

  for(guint k = 0; k < l->kept_images->len; k++)
  {
    // in the output the window has now, which may not be the one the kept
    // draw had. their records stay: the kept foreground is the one they cut
    dt_presenter_image_t image = g_array_index(l->kept_images, dt_presenter_image_t, k);
    if(!_rendition(l, &image)) continue;
    g_ptr_array_add(l->drawing, _record_ref(image.data));
    g_array_append_val(images, image);
  }
  return TRUE;
}

// an image as the compose reads it
typedef struct _source_t
{
  const float *pixels;
  int width, height;
  // device pixels to frame pixels
  cairo_matrix_t inverse;
  gboolean nearest;
  // a translation by whole texels: the sample at device pixel (x, y) is
  // the texel (x + du, y + dv), clamped to the frame (_shift())
  gboolean shifted;
  int du, dv;
  const _coverage_t *coverage;
  const uint8_t *backdrop;
} _source_t;

// 0 <= v < 65520 as an IEEE half float, rounded to nearest even
static inline uint16_t _half(const float v)
{
  // below the smallest normal half: a multiple of 2^-24
  if(v < 6.103515625e-05f) return (uint16_t)lrintf(v * 16777216.0f);

  union { float f; uint32_t u; } x = { .f = v };
  const uint32_t u = x.u + 0x0fff + ((x.u >> 13) & 1);
  return (uint16_t)((u >> 13) - ((127 - 15) << 10));
}

// _half() of each channel k of a row of width pixels times scale[k]: eight
// at once where the build targets F16C, which rounds alike
static void _halves(const float *const p,
                    const float scale[4],
                    uint16_t *const out,
                    const int width)
{
  int x = 0;
#if defined(__F16C__) && defined(__AVX__)
  const __m256 s = _mm256_setr_ps(scale[0], scale[1], scale[2], scale[3],
                                  scale[0], scale[1], scale[2], scale[3]);
  // the instruction keeps the sign of -0, which _half() drops
  const __m128i magnitude = _mm_set1_epi16(0x7fff);
  for(; x + 2 <= width; x += 2)
  {
    const __m256 v = _mm256_mul_ps(s, _mm256_loadu_ps(p + 4 * x));
    const __m128i h = _mm256_cvtps_ph(v, _MM_FROUND_TO_NEAREST_INT);
    _mm_storeu_si128((__m128i *)(out + 4 * x), _mm_and_si128(h, magnitude));
  }
#endif
  for(; x < width; x++)
    for(int k = 0; k < 4; k++) out[4 * x + k] = _half(scale[k] * p[4 * x + k]);
}

// a finite v as an IEEE half float, the largest finite one beyond it
static inline uint16_t _signed_half(const float v)
{
  return (v < 0.0f ? 0x8000 : 0) | _half(fminf(fabsf(v), 65504.0f));
}

static inline uint16_t _unorm(const float v)
{
  return (uint16_t)(CLAMPF(v, 0.0f, 1.0f) * 65535.0f + 0.5f);
}

// v weighed by w > 0, 0 for w = 0 even if v is infinite
static inline float _weighed(const float v,
                             const float w)
{
  return w > 0.0f ? w * v : 0.0f;
}

// the color of texel (iu, iv), clamped to the frame
static inline void _texel(const _source_t *s,
                          const int iu,
                          const int iv,
                          float c[3])
{
  const size_t k = (size_t)CLAMP(iv, 0, s->height - 1) * s->width + CLAMP(iu, 0, s->width - 1);
  for(int j = 0; j < 3; j++) c[j] = s->pixels[4 * k + j];
}

// the bilinear interpolation of the frame at (u, v), padded at the edges:
// the straight color of cairo's EXTEND_NONE there, whose premultiplied one
// the coverage scales
static inline void _bilinear(const _source_t *s,
                             const double u,
                             const double v,
                             float c[3])
{
  const double fu = CLAMP(u - 0.5, 0.0, s->width - 1.0);
  const double fv = CLAMP(v - 0.5, 0.0, s->height - 1.0);
  const int u0 = (int)fu, v0 = (int)fv;
  const int u1 = MIN(u0 + 1, s->width - 1), v1 = MIN(v0 + 1, s->height - 1);
  const float a = fu - u0, b = fv - v0;
  const float *p00 = s->pixels + (size_t)4 * ((size_t)v0 * s->width + u0);
  const float *p01 = s->pixels + (size_t)4 * ((size_t)v0 * s->width + u1);
  const float *p10 = s->pixels + (size_t)4 * ((size_t)v1 * s->width + u0);
  const float *p11 = s->pixels + (size_t)4 * ((size_t)v1 * s->width + u1);
  for(int k = 0; k < 3; k++)
  {
    c[k] = (1.0f - b) * ((1.0f - a) * p00[k] + a * p01[k])
           + b * ((1.0f - a) * p10[k] + a * p11[k]);
    // an infinite texel of weight 0 makes NaN: leave out the texels of
    // weight 0, as for any finite value there
    if(dt_isnan(c[k]))
      c[k] = _weighed(p00[k], (1.0f - a) * (1.0f - b)) + _weighed(p01[k], a * (1.0f - b))
             + _weighed(p10[k], (1.0f - a) * b) + _weighed(p11[k], a * b);
  }
}

// the straight color at device pixel (x, y), clipped to [lo, hi] (NaN to
// lo): outside the volume an encoding declares, colorimetry is undefined
static inline void _sample(const _source_t *s,
                           const int x,
                           const int y,
                           const float lo,
                           const float hi,
                           float color[3])
{
  float c[3];
  if(s->shifted)
    _texel(s, x + s->du, y + s->dv, c);
  else
  {
    double u = x + 0.5, v = y + 0.5;
    cairo_matrix_transform_point(&s->inverse, &u, &v);
    if(s->nearest)
      _texel(s, (int)floor(u), (int)floor(v), c);
    else
      _bilinear(s, u, v, c);
  }
  // CLAMPF takes NaN to lo
  for(int k = 0; k < 3; k++) color[k] = CLAMPF(c[k], lo, hi);
}

/*
 * the image layer N, composed into the reservation from the sources in
 * paint order. with the foreground F above, F over N shows what cairo
 * would paint (dev-doc/display_presentation.md): at each image of
 * coverage a over a foreground of alpha b before its cut,
 * N <- N + w (I - N), w = a / (a + (1 - a)(1 - b)), on all four channels
 * with I's alpha 1. w comes from the a and b the cut applied and is in
 * [0, 1], so N's alpha stays in [0, 1] and its color in the clip range.
 * the images accumulate in float, and the encoding's matrix applies before
 * the one quantization, so overlapping images round once. FALSE if out of
 * memory
 */
static gboolean _compose(const _source_t *sources,
                         const int count,
                         const int width,
                         const int height,
                         const dt_presenter_reservation_t *r)
{
  size_t padded;
  float *const rows = dt_alloc_perthread_float((size_t)4 * width, &padded);
  if(!rows) return FALSE;

  const float lo = r->encoding.clip[0], hi = r->encoding.clip[1];
  const gboolean unorm = r->encoding.storage == DT_PRESENTER_UNORM16;
  float m[3][3];
  memcpy(m, r->encoding.matrix, sizeof(m));
  // a diagonal map of values that stay positive, as Wayland's, quantizes
  // without the products that are 0 and the sign; the full map takes half
  // as long again
  gboolean diagonal = lo >= 0.0f;
  for(int k = 0; k < 3; k++)
    for(int j = 0; j < 3; j++)
      diagonal &= j == k ? m[k][j] >= 0.0f && m[k][j] * hi < 65504.0f : m[k][j] == 0.0f;
  const float scale[4] = { m[0][0], m[1][1], m[2][2], 1.0f };
  DT_OMP_FOR()
  for(int y = 0; y < height; y++)
  {
    uint16_t *const out = (uint16_t *)((uint8_t *)r->image + r->image_stride * y);
    float *const n = dt_get_perthread(rows, padded);
    gboolean covered = FALSE;
    for(int s = 0; s < count; s++)
    {
      const _source_t *src = sources + s;
      const _coverage_t *c = src->coverage;
      if(y < c->box[1] || y >= c->box[3]) continue;
      if(!covered) memset(n, 0, sizeof(float) * 4 * width);
      covered = TRUE;
      const uint8_t *alpha = c->alpha + (size_t)c->stride * (y - c->box[1]) - c->box[0];
      const uint8_t *backdrop = src->backdrop + c->fractional[y - c->box[1]];
      // a kept image may be drawn for a larger widget than now
      const int x1 = MIN(c->box[2], width);
      for(int x = c->box[0]; x < x1; x++)
      {
        const int a = alpha[x];
        if(a == 0) continue;
        float *const px = n + (size_t)4 * x;
        float color[3];
        _sample(src, x, y, lo, hi, color);
        if(a == 255)
        {
          for(int k = 0; k < 3; k++) px[k] = color[k];
          px[3] = 1.0f;
          continue;
        }
        const int b = *backdrop++;
        const float w = (float)(255 * a) / (float)(255 * a + (255 - a) * (255 - b));
        for(int k = 0; k < 3; k++) px[k] += w * (color[k] - px[k]);
        px[3] += w * (1.0f - px[3]);
      }
    }
    if(!covered)
    {
      memset(out, 0, sizeof(uint16_t) * 4 * width);
      continue;
    }
    if(diagonal)
    {
      if(unorm)
        for(int x = 0; x < width; x++)
          for(int k = 0; k < 4; k++) out[4 * x + k] = _unorm(scale[k] * n[4 * x + k]);
      else
        _halves(n, scale, out, width);
      continue;
    }
    for(int x = 0; x < width; x++)
    {
      const float *p = n + (size_t)4 * x;
      for(int k = 0; k < 3; k++)
      {
        const float v = m[k][0] * p[0] + m[k][1] * p[1] + m[k][2] * p[2];
        out[4 * x + k] = unorm ? _unorm(v) : _signed_half(v);
      }
      out[4 * x + 3] = unorm ? _unorm(p[3]) : _half(p[3]);
    }
  }
  dt_free_align(rows);
  return TRUE;
}

// the d with floor(u) = x + d for every 0 <= x < extent, where u =
// (x + 0.5) + t as cairo sums it in double: the texel a nearest sample
// reads under a translation by t. a bilinear sample reads that texel alone
// where t is whole. FALSE where it is not certain
static gboolean _offset(const double t,
                        const int extent,
                        const gboolean nearest,
                        int *d)
{
  // the sums then round by at most 2^-32 (and NaN fails)
  if(!(fabs(t) < 0x1p20) || extent > 0x100000) return FALSE;
  if(t == floor(t))
  {
    *d = (int)t;
    return TRUE;
  }
  if(!nearest) return FALSE;
  // on the half pixel grid u is exact; elsewhere its rounding must not
  // reach a whole number, which a margin far wider keeps clear of
  const double f = floor(t + 0.5), e = t + 0.5 - f;
  if(2.0 * t != floor(2.0 * t) && (e < 0x1p-24 || e > 1.0 - 0x1p-24)) return FALSE;
  *d = (int)f;
  return TRUE;
}

// whether a source of a width x height widget is a translation the
// compose reads without the transform, each sample one texel for either
// filter: the texel _sample() reads through the transform
static gboolean _shift(_source_t *s,
                       const int width,
                       const int height)
{
  const cairo_matrix_t *i = &s->inverse;
  s->shifted = i->xx == 1.0 && i->yy == 1.0 && i->xy == 0.0 && i->yx == 0.0
               && _offset(i->x0, width, s->nearest, &s->du)
               && _offset(i->y0, height, s->nearest, &s->dv);
  return s->shifted;
}

static gboolean _compose_widget(const _widget_t *l,
                                const int width,
                                const int height,
                                const dt_presenter_reservation_t *r)
{
  _source_t *sources = g_new(_source_t, l->images->len);
  int count = 0;
  cairo_matrix_t device;
  cairo_matrix_init_scale(&device, l->scale, l->scale);
  for(guint k = 0; k < l->images->len; k++)
  {
    const dt_presenter_image_t *image = &g_array_index(l->images, dt_presenter_image_t, k);
    const _record_t *record = image->data;
    const dt_frame_geometry_t *frame = dt_frame_geometry(image->frame);
    _source_t *s = &sources[count];
    *s = (_source_t){ .pixels = image->pixels, .width = frame->width,
                      .height = frame->height,
                      .nearest = image->filter == CAIRO_FILTER_FAST
                                 || image->filter == CAIRO_FILTER_NEAREST,
                      .coverage = record->coverage, .backdrop = record->backdrop };
    cairo_matrix_multiply(&s->inverse, &image->matrix, &device);
    // a singular placement covers nothing
    if(cairo_matrix_invert(&s->inverse) != CAIRO_STATUS_SUCCESS) continue;
    _shift(s, width, height);
    count++;
  }
  const gboolean done = _compose(sources, count, width, height, r);
  g_free(sources);
  return done;
}

static gboolean _same(const _record_t *a,
                      const _record_t *b)
{
  if(a == b) return TRUE;
  const _coverage_t *ca = a->coverage;
  if(!_coverage_equal(ca, b->coverage)) return FALSE;
  const int count = ca->fractional[ca->box[3] - ca->box[1]];
  return !count || !memcmp(a->backdrop, b->backdrop, count);
}

static gboolean _image_changed(const _widget_t *l,
                               const int width,
                               const int height,
                               const dt_presenter_reservation_t *r)
{
  if(width != l->shown_width || height != l->shown_height || l->scale != l->shown_scale
     || memcmp(&r->encoding, &l->shown_encoding, sizeof(r->encoding))
     || l->images->len != l->shown->len)
    return TRUE;
  for(guint k = 0; k < l->images->len; k++)
  {
    const dt_presenter_image_t *i = &g_array_index(l->images, dt_presenter_image_t, k);
    const _shown_t *s = &g_array_index(l->shown, _shown_t, k);
    if(dt_frame_id(i->frame) != s->id || i->pixels != s->pixels || !_same(i->data, s->record))
      return TRUE;
  }
  return FALSE;
}

static void _shown_set(_widget_t *l,
                       const int width,
                       const int height,
                       const dt_presenter_reservation_t *r)
{
  for(guint k = 0; k < l->images->len; k++)
    _record_ref(g_array_index(l->images, dt_presenter_image_t, k).data);
  _shown_clear(l);
  l->shown_width = width;
  l->shown_height = height;
  l->shown_scale = l->scale;
  l->shown_encoding = r->encoding;
  g_array_set_size(l->shown, l->images->len);
  for(guint k = 0; k < l->images->len; k++)
  {
    const dt_presenter_image_t *i = &g_array_index(l->images, dt_presenter_image_t, k);
    g_array_index(l->shown, _shown_t, k) =
      (_shown_t){ .id = dt_frame_id(i->frame), .pixels = i->pixels, .record = i->data };
  }
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
  // not on l->cr: the view may have left a clip or transformation there
  // (dt_view_manager_expose(), views/view.c), which GTK's own draw of
  // these widgets does not have
  cairo_t *cr = cairo_create(l->foreground);
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
    cairo_save(cr);
    cairo_translate(cr, x, y);
    gtk_widget_draw(widget, cr);
    cairo_restore(cr);
  }
  cairo_destroy(cr);
  g_ptr_array_free(above, TRUE);
}

static void _present(_widget_t *l)
{
  l->pending = FALSE;
  const int width = l->width * l->scale;
  const int height = l->height * l->scale;
  dt_presenter_layers_t layers = { .width = width, .height = height, .scale = l->scale };
  gtk_widget_translate_coordinates(l->widget, l->window->window->toplevel, 0, 0,
                                   &layers.x, &layers.y);

  // the whole present or nothing: the next frame tries again
  dt_presenter_transport_t *transport = l->window->transport;
  dt_presenter_reservation_t r;
  if(!transport->reserve(transport, l->widget, &l->transport_data, &layers, &r))
  {
    gtk_widget_queue_draw(l->widget);
    return;
  }
  const gboolean changed = _image_changed(l, width, height, &r);
  // no image buffer, or no memory to compose it
  if(changed && (!r.image || !_compose_widget(l, width, height, &r)))
  {
    gtk_widget_queue_draw(l->widget);
    return;
  }
  if(changed) _shown_set(l, width, height, &r);

  // a kept foreground is the widget's drawing alone: what is replayed
  // above it is not part of the presentation a later draw shows again
  GSList *saved = NULL;
  const gboolean kept = l->foreground == l->kept;
  _replay_above(l, kept ? &saved : NULL);
  cairo_surface_flush(l->foreground);
  const uint8_t *foreground = cairo_image_surface_get_data(l->foreground);
  const size_t stride = cairo_image_surface_get_stride(l->foreground);
  if(r.foreground_stride == stride)
    memcpy(r.foreground, foreground, stride * height);
  else
    for(int y = 0; y < height; y++)
      memcpy(r.foreground + r.foreground_stride * y, foreground + stride * y, (size_t)4 * width);
  transport->commit(transport, &l->transport_data, changed);
  if(saved) _restore(l, saved);
}

static void _hide(_widget_t *l)
{
  l->pending = FALSE;
  for(guint k = 0; k < l->images->len; k++)
    _record_unref(g_array_index(l->images, dt_presenter_image_t, k).data);
  g_array_set_size(l->images, 0);
  _shown_clear(l);
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
  for(guint k = 0; k < images->len; k++)
    _record_ref(g_array_index(images, dt_presenter_image_t, k).data);
  for(guint k = 0; k < l->images->len; k++)
    _record_unref(g_array_index(l->images, dt_presenter_image_t, k).data);
  g_array_set_size(l->images, 0);
  g_array_append_vals(l->images, images->data, images->len);
  g_ptr_array_set_size(l->drawing, 0);

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
    {
      const dt_presenter_image_t *image = &g_array_index(images, dt_presenter_image_t, k);
      dt_frame_ref(image->frame);
      _record_ref(image->data);
    }
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
  for(guint k = 0; k < l->images->len; k++)
    _record_unref(g_array_index(l->images, dt_presenter_image_t, k).data);
  _shown_clear(l);
  g_array_free(l->kept_images, TRUE);
  g_array_free(l->images, TRUE);
  g_ptr_array_free(l->drawing, TRUE);
  g_array_free(l->shown, TRUE);
  g_free(l->opaque);
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

static void _target(dt_presenter_backend_t *self,
                    const float headroom)
{
  _window_t *w = (_window_t *)self;
  if(w->transport->target) w->transport->target(w->transport, headroom);
}

// a new platform goes here behind its build option, NULL with no reason
// for windows not on it
dt_presenter_platform_t *dt_presenter_platform_new(dt_presenter_window_t *window,
                                                   gchar **reason)
{
#ifdef HAVE_WAYLAND
  return dt_presenter_wayland_new(window->toplevel, window, reason);
#else
  *reason = NULL;
  return NULL;
#endif
}

dt_presenter_backend_t *dt_presenter_layered_new(dt_presenter_window_t *window,
                                                 gchar **reason)
{
  dt_presenter_transport_t *transport = window->platform->transport(window->platform,
                                                                    reason);
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
                                         .free = _free,
                                         .target = _target };
  return &w->backend;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
