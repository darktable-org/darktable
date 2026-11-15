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
 * cmocka unit tests for the layered presenter's image layer
 * (gui/presenter_layers.c), on random placements: the coverage it makes
 * is what cairo paints of the whole frame, and the image
 * layer it composes into a reservation is a straightforward evaluation, in
 * double precision, of the two-layer rule in
 * dev-doc/display_presentation.md. presenter_layers.c is included for its
 * static helpers, none of which needs a display
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include "gui/presenter_layers.c"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static const cairo_filter_t _filters[] = { CAIRO_FILTER_FAST, CAIRO_FILTER_NEAREST,
                                           CAIRO_FILTER_GOOD, CAIRO_FILTER_BEST,
                                           CAIRO_FILTER_BILINEAR };

typedef enum _shape_t
{
  SHAPE_NONE,
  // a disc of center (clip2[0], clip2[1]) and radius clip2[2]
  SHAPE_DISC,
  // the second rectangle turned by clip2[3] radians about its corner
  SHAPE_TURNED,
} _shape_t;

typedef struct _placement_t
{
  int frame_width, frame_height;
  cairo_matrix_t matrix;
  // clipped as callers clip: after a translation by (tx, ty), to a
  // rectangle (x0, y0, x1, y1), and to a second one if two, then to shape
  double tx, ty, clip[4], clip2[4];
  gboolean two;
  _shape_t shape;
  cairo_filter_t filter;
} _placement_t;

static void _clip(cairo_t *cr,
                  const _placement_t *p)
{
  // not cairo_save(), which would take the clip back at its restore
  cairo_matrix_t matrix;
  cairo_get_matrix(cr, &matrix);
  cairo_translate(cr, p->tx, p->ty);
  cairo_rectangle(cr, p->clip[0], p->clip[1], p->clip[2] - p->clip[0], p->clip[3] - p->clip[1]);
  cairo_clip(cr);
  if(p->two)
  {
    cairo_rectangle(cr, p->clip2[0], p->clip2[1], p->clip2[2] - p->clip2[0],
                    p->clip2[3] - p->clip2[1]);
    cairo_clip(cr);
  }
  if(p->shape == SHAPE_DISC)
  {
    cairo_arc(cr, p->clip2[0], p->clip2[1], p->clip2[2], 0.0, 2.0 * M_PI);
    cairo_clip(cr);
  }
  else if(p->shape == SHAPE_TURNED)
  {
    cairo_translate(cr, p->clip2[0], p->clip2[1]);
    cairo_rotate(cr, p->clip2[3]);
    cairo_rectangle(cr, 0.0, 0.0, p->clip2[2], 0.5 * p->clip2[2]);
    cairo_clip(cr);
  }
  cairo_set_matrix(cr, &matrix);
}

static cairo_surface_t *_opaque_source(const int width,
                                       const int height)
{
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_A8, width, height);
  cairo_surface_flush(s);
  memset(cairo_image_surface_get_data(s), 255,
         (size_t)cairo_image_surface_get_stride(s) * height);
  cairo_surface_mark_dirty(s);
  return s;
}

// a frame placed in a widget of logical size width x height: enlarged,
// reduced, rotated or translated by fractions, clipped at fractions
static void _random_placement(GRand *rand,
                              const int width,
                              const int height,
                              const int max_frame,
                              _placement_t *p)
{
  p->frame_width = g_rand_int_range(rand, 3, max_frame);
  p->frame_height = g_rand_int_range(rand, 3, max_frame);
  const double zoom = exp(g_rand_double_range(rand, log(0.1), log(8.0)));
  const double angle = g_rand_boolean(rand) ? 0.0 : g_rand_double_range(rand, -0.6, 0.6);
  cairo_matrix_init_translate(&p->matrix, g_rand_double_range(rand, 0.0, width),
                              g_rand_double_range(rand, 0.0, height));
  cairo_matrix_rotate(&p->matrix, angle);
  cairo_matrix_scale(&p->matrix, zoom, zoom);
  cairo_matrix_translate(&p->matrix, -0.5 * p->frame_width, -0.5 * p->frame_height);
  p->tx = g_rand_double_range(rand, 0.0, 0.25 * width);
  p->ty = g_rand_double_range(rand, 0.0, 0.25 * height);
  for(int k = 0; k < 2; k++)
  {
    double *c = k ? p->clip2 : p->clip;
    c[0] = g_rand_double_range(rand, -5.0, 0.5 * width);
    c[1] = g_rand_double_range(rand, -5.0, 0.5 * height);
    c[2] = c[0] + g_rand_double_range(rand, 1.0, width);
    c[3] = c[1] + g_rand_double_range(rand, 1.0, height);
  }
  p->two = g_rand_boolean(rand);
  p->shape = SHAPE_NONE;
  // also narrower than a pixel, whole pixels, and none but the widget's
  // edges
  if(!g_rand_int_range(rand, 0, 8)) p->clip[2] = p->clip[0] + g_rand_double_range(rand, 0.05, 1.0);
  if(!g_rand_int_range(rand, 0, 8)) p->clip[3] = p->clip[1] + g_rand_double_range(rand, 0.05, 1.0);
  if(!g_rand_int_range(rand, 0, 5))
    for(int k = 0; k < 4; k++) p->clip[k] = round(p->clip[k] + p->tx) - p->tx;
  if(!g_rand_int_range(rand, 0, 10))
  {
    p->tx = p->ty = 0.0;
    p->clip[0] = p->clip[1] = -10.0;
    p->clip[2] = width + 10.0;
    p->clip[3] = height + 10.0;
    p->two = FALSE;
  }
  // and shapes that are no rectangle, within the rectangles
  else if(!g_rand_int_range(rand, 0, 6))
  {
    p->two = FALSE;
    p->shape = g_rand_boolean(rand) ? SHAPE_DISC : SHAPE_TURNED;
    p->clip2[0] = g_rand_double_range(rand, 0.0, 0.5 * width);
    p->clip2[1] = g_rand_double_range(rand, 0.0, 0.5 * height);
    p->clip2[2] = g_rand_double_range(rand, 2.0, width);
    p->clip2[3] = g_rand_double_range(rand, -1.0, 1.0);
  }
  p->filter = _filters[g_rand_int_range(rand, 0, G_N_ELEMENTS(_filters))];
}

// the coverage as the layered backend makes it, from a widget's context
// clipped as p asks, where the frame is painted with p's matrix
static _coverage_t *_coverage(const _placement_t *p,
                              const int width,
                              const int height,
                              const int scale)
{
  cairo_surface_t *target =
    cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width * scale, height * scale);
  cairo_surface_set_device_scale(target, scale, scale);
  cairo_t *cr = cairo_create(target);
  _clip(cr, p);
  cairo_set_matrix(cr, &p->matrix);
  _coverage_t key;
  assert_true(_coverage_key(cr, p->frame_width, p->frame_height, &p->matrix, p->filter, scale,
                            width * scale, height * scale, &key));
  cairo_surface_t *source = _opaque_source(p->frame_width, p->frame_height);
  _coverage_t *c = _coverage_new(source, &key);
  cairo_surface_destroy(source);
  assert_non_null(c);
  // nothing was painted on the widget
  cairo_surface_flush(target);
  const uint8_t *data = cairo_image_surface_get_data(target);
  for(size_t k = 0; k < (size_t)cairo_image_surface_get_stride(target) * height * scale; k++)
    assert_int_equal(data[k], 0);
  assert_int_equal(cairo_status(cr), CAIRO_STATUS_SUCCESS);
  cairo_destroy(cr);
  cairo_surface_destroy(target);
  return c;
}

// what cairo paints of the frame, with alpha 1, on a widget's surface
static cairo_surface_t *_cairo_coverage(const _placement_t *p,
                                        const int width,
                                        const int height,
                                        const int scale)
{
  cairo_surface_t *surface =
    cairo_image_surface_create(CAIRO_FORMAT_A8, width * scale, height * scale);
  cairo_surface_set_device_scale(surface, scale, scale);
  cairo_t *cr = cairo_create(surface);
  _clip(cr, p);
  cairo_set_matrix(cr, &p->matrix);
  cairo_surface_t *source = _opaque_source(p->frame_width, p->frame_height);
  cairo_set_source_surface(cr, source, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), p->filter);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(source);
  cairo_surface_flush(surface);
  return surface;
}

static uint8_t _coverage_at(const _coverage_t *c,
                            const int x,
                            const int y)
{
  if(x < c->box[0] || x >= c->box[2] || y < c->box[1] || y >= c->box[3]) return 0;
  return c->alpha[(size_t)c->stride * (y - c->box[1]) + x - c->box[0]];
}

/*
 * the coverage is cairo's paint of the whole frame on the widget's
 * surface, clipped as the caller clipped, but for pixman's fixed-point
 * stepping: along a row it adds a rounded 16.16 increment per pixel from
 * where the composite starts, and an edge tile starts elsewhere than the
 * whole paint. so at every device pixel: nothing outside the box; where
 * both are covered whole or not at all, the same, except with a nearest
 * filter whose sample lies within 0.01 source pixels of the frame's edge,
 * where the stepping decides between in and out; where either is covered
 * in part, apart by an amount that grows with the stepping's run, which
 * is no proven bound: through rectangles at most 5 code values in 40 seeds
 * (8 allowed). through a disc or a turned rectangle cairo's own
 * antialiasing of the clip's edge depends on how it composites, at most
 * 14 there (32 allowed). the fractional numbering counts 0 < alpha < 255
 */
static void test_coverage_is_cairos(void **state)
{
  GRand *rand = g_rand_new_with_seed(7);
  long fractional = 0, whole = 0, close = 0, edge = 0;
  int worst = 0;
  for(int draw = 0; draw < 400; draw++)
  {
    const int scale = draw % 3 == 2 ? 2 : 1;
    const int width = 300 / scale, height = 220 / scale;
    _placement_t p;
    _random_placement(rand, width, height, 400 / scale, &p);
    _coverage_t *c = _coverage(&p, width, height, scale);
    cairo_surface_t *ref = _cairo_coverage(&p, width, height, scale);
    const uint8_t *data = cairo_image_surface_get_data(ref);
    const int stride = cairo_image_surface_get_stride(ref);
    cairo_matrix_t inverse, device;
    cairo_matrix_init_scale(&device, scale, scale);
    cairo_matrix_multiply(&inverse, &p.matrix, &device);
    cairo_matrix_invert(&inverse);
    const gboolean nearest = p.filter == CAIRO_FILTER_FAST || p.filter == CAIRO_FILTER_NEAREST;

    for(int y = 0; y < height * scale; y++)
      for(int x = 0; x < width * scale; x++)
      {
        const int r = data[(size_t)stride * y + x];
        const int a = _coverage_at(c, x, y);
        whole += r == 255;
        if(x < c->box[0] || x >= c->box[2] || y < c->box[1] || y >= c->box[3])
          if(r) fail_msg("draw %d: (%d, %d) is outside the box, cairo covers it", draw, x, y);
        const int d = abs(a - r);
        if(!d) continue;
        if((a && a < 255) || (r && r < 255))
        {
          if(d > (p.shape ? 32 : 8))
            fail_msg("draw %d: (%d, %d) is %d, cairo paints %d (filter %d, scale %d)", draw, x,
                     y, a, r, p.filter, scale);
          close++;
          worst = MAX(worst, d);
          continue;
        }
        double u = x + 0.5, v = y + 0.5;
        cairo_matrix_transform_point(&inverse, &u, &v);
        const double from_edge = fmin(fmin(fabs(u), fabs(u - p.frame_width)),
                                      fmin(fabs(v), fabs(v - p.frame_height)));
        if(!nearest || from_edge > 0.01)
          fail_msg("draw %d: (%d, %d) is %d, cairo paints %d (filter %d, scale %d)", draw, x, y,
                   a, r, p.filter, scale);
        edge++;
      }

    int n = 0;
    for(int y = c->box[1]; y < c->box[3]; y++)
    {
      assert_int_equal(c->fractional[y - c->box[1]], n);
      for(int x = c->box[0]; x < c->box[2]; x++)
      {
        const uint8_t a = _coverage_at(c, x, y);
        n += a && a < 255;
      }
    }
    assert_int_equal(c->fractional[c->box[3] - c->box[1]], n);
    fractional += n;
    cairo_surface_destroy(ref);
    _coverage_unref(c);
  }
  print_message("pixels covered whole %ld, in part %ld; in part and apart: %ld, at most %d;"
                " in or out at a nearest edge: %ld\n", whole, fractional, close, worst, edge);
  // the draws exercised edges and interiors
  assert_true(fractional > 10000);
  assert_true(whole > 1000000);
  g_rand_free(rand);
}

static double _half_value(const uint16_t h)
{
  const int e = (h >> 10) & 0x1f, m = h & 0x3ff;
  const double v = e ? ldexp(1.0 + m / 1024.0, e - 15) : ldexp(m, -24);
  return h & 0x8000 ? -v : v;
}

// v weighed by w, 0 for w = 0 even if v is infinite
static double _reference_weighed(const double v,
                                 const double w)
{
  return w > 0.0 ? w * v : 0.0;
}

// what the compose samples, in double precision
static void _reference_sample(const _source_t *s,
                              const int x,
                              const int y,
                              const double lo,
                              const double hi,
                              double color[3])
{
  double u = x + 0.5, v = y + 0.5;
  cairo_matrix_transform_point(&s->inverse, &u, &v);
  double c[3];
  if(s->nearest)
  {
    const int iu = CLAMP((int)floor(u), 0, s->width - 1);
    const int iv = CLAMP((int)floor(v), 0, s->height - 1);
    for(int k = 0; k < 3; k++) c[k] = s->pixels[4 * ((size_t)iv * s->width + iu) + k];
  }
  else
  {
    const double fu = CLAMP(u - 0.5, 0.0, s->width - 1.0);
    const double fv = CLAMP(v - 0.5, 0.0, s->height - 1.0);
    const int u0 = (int)fu, v0 = (int)fv;
    const int u1 = MIN(u0 + 1, s->width - 1), v1 = MIN(v0 + 1, s->height - 1);
    const double a = fu - u0, b = fv - v0;
#define P(uu, vv, k) (double)s->pixels[4 * ((size_t)(vv) * s->width + (uu)) + (k)]
    for(int k = 0; k < 3; k++)
    {
      c[k] = (1 - b) * ((1 - a) * P(u0, v0, k) + a * P(u1, v0, k))
             + b * ((1 - a) * P(u0, v1, k) + a * P(u1, v1, k));
      // texels of weight 0 do not count, infinite ones neither
      if(isnan(c[k]))
        c[k] = _reference_weighed(P(u0, v0, k), (1 - a) * (1 - b))
               + _reference_weighed(P(u1, v0, k), a * (1 - b))
               + _reference_weighed(P(u0, v1, k), (1 - a) * b)
               + _reference_weighed(P(u1, v1, k), a * b);
    }
#undef P
  }
  for(int k = 0; k < 3; k++) color[k] = isnan(c[k]) ? lo : CLAMP(c[k], lo, hi);
}

typedef enum _backdrop_t
{
  BACKDROP_OPAQUE,
  BACKDROP_NONE,
  BACKDROP_TRANSLUCENT,
} _backdrop_t;

typedef struct _scene_t
{
  int width, height, scale;
  _backdrop_t backdrop;
  int count;
  _placement_t placements[3];
  // a translucent paint after the first image
  gboolean overlay;
} _scene_t;

static float *_random_frame(GRand *rand,
                            const _placement_t *p)
{
  const size_t n = (size_t)4 * p->frame_width * p->frame_height;
  float *pixels = g_new(float, n);
  for(size_t k = 0; k < n; k++)
  {
    const int pick = g_rand_int_range(rand, 0, 400);
    pixels[k] = pick > 3 ? g_rand_double_range(rand, -0.3, 5.0)
                : pick == 3 ? INFINITY : pick == 2 ? -INFINITY : NAN;
  }
  return pixels;
}

static void _translucent_paint(cairo_t *cr,
                               GRand *rand,
                               const int width,
                               const int height)
{
  cairo_set_source_rgba(cr, g_rand_double(rand), g_rand_double(rand), g_rand_double(rand),
                        g_rand_double_range(rand, 0.1, 0.9));
  cairo_rectangle(cr, g_rand_double_range(rand, 0, width / 2.0),
                  g_rand_double_range(rand, 0, height / 2.0),
                  g_rand_double_range(rand, 1, width), g_rand_double_range(rand, 1, height));
  cairo_fill(cr);
}

/*
 * paints a scene as the layered backend does (backdrop, then each image
 * cut out of the foreground, a translucent paint between them if asked),
 * composes it, and compares every channel of every pixel with section 4's
 * rule in its original, non-convex form, evaluated in double precision
 * from the same coverage and backdrop alpha before each cut, then the
 * encoding's matrix:
 *   N <- (a I + (1 - a)(1 - b) N) / (1 - (1 - a) b), I's alpha 1
 * the bound, per channel of encoded value v = sum_j m_j N_j:
 *   |got - v| <= 2^-11 |v| + 2^-24 + 1e-6 max(1, sum_j |m_j| hi)
 * UNORM16 instead clips v to [0, 1] and has rounding error <= 0.5 / 65535.
 * the half float rounding to nearest is within 2^-11 |v| for normal values
 * and 2^-25 below; the rest covers float arithmetic: bilinear weights in
 * float, the lerp and the weight's division, each a few float ulps of
 * values up to hi, the clip's top, then the matrix product
 */
static void _check_scene(GRand *rand,
                         const _scene_t *scene,
                         const dt_presenter_encoding_t *e,
                         double *worst)
{
  const double lo = e->clip[0], hi = e->clip[1];
  const int W = scene->width * scene->scale, H = scene->height * scene->scale;
  cairo_surface_t *fg = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
  cairo_surface_set_device_scale(fg, scene->scale, scene->scale);
  cairo_t *cr = cairo_create(fg);
  if(scene->backdrop == BACKDROP_OPAQUE)
  {
    cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
    cairo_paint(cr);
  }
  else if(scene->backdrop == BACKDROP_TRANSLUCENT)
    _translucent_paint(cr, rand, scene->width, scene->height);
  cairo_surface_flush(fg);

  double *ref = g_new0(double, (size_t)4 * W * H);
  uint8_t *before = g_new(uint8_t, (size_t)W * H);
  _source_t sources[3];
  _record_t records[3];
  float *frames[3];
  cairo_matrix_t device;
  cairo_matrix_init_scale(&device, scene->scale, scene->scale);
  for(int i = 0; i < scene->count; i++)
  {
    const _placement_t *p = &scene->placements[i];
    frames[i] = _random_frame(rand, p);
    _coverage_t *c = _coverage(p, scene->width, scene->height, scene->scale);
    records[i] = (_record_t){ .refs = 1, .coverage = c,
                              .backdrop = g_malloc(c->fractional[c->box[3] - c->box[1]] + 1) };
    _source_t *s = &sources[i];
    *s = (_source_t){ .pixels = frames[i], .width = p->frame_width, .height = p->frame_height,
                      .nearest = p->filter == CAIRO_FILTER_FAST || p->filter == CAIRO_FILTER_NEAREST,
                      .coverage = c, .backdrop = records[i].backdrop };
    cairo_matrix_multiply(&s->inverse, &p->matrix, &device);
    assert_int_equal(cairo_matrix_invert(&s->inverse), CAIRO_STATUS_SUCCESS);

    cairo_surface_flush(fg);
    uint8_t *data = cairo_image_surface_get_data(fg);
    const int stride = cairo_image_surface_get_stride(fg);
    for(int y = 0; y < H; y++)
      for(int x = 0; x < W; x++)
        before[(size_t)y * W + x] = ((uint32_t *)(data + (size_t)stride * y))[x] >> 24;
    _cut(data, W, H, stride, c, records[i].backdrop);
    cairo_surface_mark_dirty(fg);

    for(int y = 0; y < H; y++)
      for(int x = 0; x < W; x++)
      {
        const int A = _coverage_at(c, x, y);
        if(!A) continue;
        const double a = A / 255.0, b = before[(size_t)y * W + x] / 255.0;
        double color[3];
        _reference_sample(s, x, y, lo, hi, color);
        double *n = ref + (size_t)4 * (y * W + x);
        const double d = 1.0 - (1.0 - a) * b;
        for(int k = 0; k < 3; k++) n[k] = (a * color[k] + (1.0 - a) * (1.0 - b) * n[k]) / d;
        n[3] = (a + (1.0 - a) * (1.0 - b) * n[3]) / d;
      }

    if(i == 0 && scene->overlay) _translucent_paint(cr, rand, scene->width, scene->height);
  }

  uint16_t *out = g_new(uint16_t, (size_t)4 * W * H);
  dt_presenter_reservation_t r = { .image = out, .image_stride = (size_t)8 * W,
                                   .encoding = *e };
  for(int storage = DT_PRESENTER_FLOAT16; storage <= DT_PRESENTER_UNORM16; storage++)
  {
    r.encoding.storage = storage;
    const gboolean unorm = storage == DT_PRESENTER_UNORM16;
    assert_true(_compose(sources, scene->count, W, H, &r));
    for(size_t k = 0; k < (size_t)W * H; k++)
    {
      for(int c = 0; c < 4; c++)
      {
        const double *n = ref + 4 * k;
        double want = n[3], weights = 1.0;
        if(c < 3)
        {
          want = weights = 0.0;
          for(int j = 0; j < 3; j++)
          {
            want += (double)e->matrix[c][j] * n[j];
            weights += fabs(e->matrix[c][j]) * hi;
          }
        }
        if(unorm) want = CLAMP(want, 0.0, 1.0);
        const double got = unorm ? out[4 * k + c] / 65535.0 : _half_value(out[4 * k + c]);
        const double error = fabs(got - want);
        const double rounding = unorm ? 0.5 / 65535.0 : ldexp(fabs(want), -11) + ldexp(1.0, -24);
        const double bound = rounding + 1e-6 * fmax(1.0, weights);
        if(error > bound)
          fail_msg("storage %d pixel (%zu, %zu) channel %d: %.9g, reference %.9g", storage,
                   k % W, k / W, c, got, want);
        *worst = fmax(*worst, error - rounding);
      }
      // w stays in [0, 1], so does N's alpha
      assert_true(unorm || _half_value(out[4 * k + 3]) <= 1.0);
      // over an opaque backdrop the image layer is opaque wherever an image
      // covers anything, as everything below the foreground is
      if(scene->backdrop == BACKDROP_OPAQUE && !scene->overlay)
        assert_true(out[4 * k + 3] == 0 || out[4 * k + 3] == (unorm ? 0xffff : 0x3c00));
    }
  }

  for(int i = 0; i < scene->count; i++)
  {
    g_free(frames[i]);
    g_free(records[i].backdrop);
    _coverage_unref(records[i].coverage);
  }
  g_free(out);
  g_free(before);
  g_free(ref);
  cairo_destroy(cr);
  cairo_surface_destroy(fg);
}

// how Wayland encodes for a headroom: clip to [0, headroom], times 1 / headroom
static dt_presenter_encoding_t _diagonal(const float hi,
                                         const float factor)
{
  return (dt_presenter_encoding_t){ .clip = { 0.0f, hi },
                                    .matrix = { { factor, 0.0f, 0.0f },
                                                { 0.0f, factor, 0.0f },
                                                { 0.0f, 0.0f, factor } } };
}

// scRGB as a Windows transport would encode BT.2020: clip to [0, 4], BT.2020
// to BT.709 primaries times the SDR white (200 nits / 80), which goes
// negative for wide gamut colors
static const dt_presenter_encoding_t _scrgb = {
  .clip = { 0.0f, 4.0f },
  .matrix = { { 2.5f * 1.660491f, 2.5f * -0.587641f, 2.5f * -0.072850f },
              { 2.5f * -0.124550f, 2.5f * 1.132900f, 2.5f * -0.008349f },
              { 2.5f * -0.018151f, 2.5f * -0.100579f, 2.5f * 1.118730f } }
};

static void test_compose_random(void **state)
{
  GRand *rand = g_rand_new_with_seed(11);
  double worst = 0.0;
  for(int n = 0; n < 240; n++)
  {
    _scene_t scene = { .scale = n % 4 == 3 ? 2 : 1, .backdrop = n % 3,
                       .count = 1 + n % 3, .overlay = n % 5 == 1 };
    scene.width = 160 / scene.scale;
    scene.height = 120 / scene.scale;
    for(int i = 0; i < scene.count; i++)
    {
      _random_placement(rand, scene.width, scene.height, 200 / scene.scale,
                        &scene.placements[i]);
      // nearest and bilinear sampling, whatever cairo's filter
      if(n % 2) scene.placements[i].filter = CAIRO_FILTER_FAST;
    }
    // HDR as Wayland encodes it (scale 1 / max), with scale 1, and scRGB
    const float max = n % 2 ? 4.0f : 1.0f;
    const dt_presenter_encoding_t e = n % 11 == 5 ? _scrgb
                                      : _diagonal(max, n % 7 == 0 ? 1.0f : 1.0f / max);
    _check_scene(rand, &scene, &e, &worst);
  }
  print_message("worst error beyond storage rounding: %.3g\n", worst);
  g_rand_free(rand);
}

/*
 * the darkroom's preview fallback under the full image: one clip, whose
 * edges fall on half pixels, for an enlarged preview (nearest) and the full
 * frame at 1:1 on top, over an opaque background and under a translucent
 * picker box. at the shared clip edge the full image paints over a
 * backdrop alpha 1 - c that the preview's cut left
 */
static void test_compose_preview_fallback(void **state)
{
  GRand *rand = g_rand_new_with_seed(13);
  double worst = 0.0;
  for(int n = 0; n < 40; n++)
  {
    _scene_t scene = { .width = 180, .height = 130, .scale = 1 + n % 2,
                       .backdrop = BACKDROP_OPAQUE, .count = 2, .overlay = n % 4 == 1 };
    const double clip[4] = { 10.5, 7.5, 169.5, 122.5 };
    _placement_t *preview = &scene.placements[0], *full = &scene.placements[1];
    *preview = (_placement_t){ .frame_width = 60, .frame_height = 45,
                               .filter = CAIRO_FILTER_FAST };
    const double zoom = 3.0 + 0.1 * n;
    cairo_matrix_init_translate(&preview->matrix, 90.0, 65.0);
    cairo_matrix_scale(&preview->matrix, zoom, zoom);
    cairo_matrix_translate(&preview->matrix, -30.0, -22.5);
    *full = (_placement_t){ .frame_width = 159 * scene.scale,
                            .frame_height = 115 * scene.scale,
                            .filter = n % 3 ? CAIRO_FILTER_FAST : CAIRO_FILTER_GOOD };
    cairo_matrix_init_translate(&full->matrix, 10.5 + 0.25 * (n % 3), 7.5);
    cairo_matrix_scale(&full->matrix, 1.0 / scene.scale, 1.0 / scene.scale);
    memcpy(preview->clip, clip, sizeof(clip));
    memcpy(full->clip, clip, sizeof(clip));
    // the darkroom's clip edges, at half the difference of two widths
    assert_true(clip[0] != floor(clip[0]));
    const dt_presenter_encoding_t e = _diagonal(4.0f, 0.25f);
    _check_scene(rand, &scene, &e, &worst);
  }
  print_message("worst error beyond storage rounding: %.3g\n", worst);
  g_rand_free(rand);
}

/*
 * clips that cairo covers in part across a whole tile or more, over a
 * frame that covers the widget whole at 1:1, where the coverage is the
 * clip's: equal to cairo's paint at every pixel. a clip a quarter pixel
 * wide, [40, 40.25], covers a quarter of that column and nothing of the
 * next, and a disc and a turned rectangle cover what cairo covers
 */
static void test_coverage_of_clips(void **state)
{
  for(int scale = 1; scale <= 2; scale++)
    for(int shape = 0; shape < 3; shape++)
    {
      const int width = 120, height = 90;
      _placement_t p = { .frame_width = (width + 20) * scale, .frame_height = (height + 20) * scale,
                         .filter = CAIRO_FILTER_FAST, .clip = { 40.0, 10.5, 40.25, 80.0 } };
      cairo_matrix_init_translate(&p.matrix, -10.0, -10.0);
      cairo_matrix_scale(&p.matrix, 1.0 / scale, 1.0 / scale);
      if(shape)
      {
        p.clip[0] = 5.5;
        p.clip[2] = 110.0;
        p.shape = shape == 1 ? SHAPE_DISC : SHAPE_TURNED;
        p.clip2[0] = 50.3;
        p.clip2[1] = 40.6;
        p.clip2[2] = 37.0;
        p.clip2[3] = 0.4;
      }
      _coverage_t *c = _coverage(&p, width, height, scale);
      cairo_surface_t *ref = _cairo_coverage(&p, width, height, scale);
      const uint8_t *data = cairo_image_surface_get_data(ref);
      const int stride = cairo_image_surface_get_stride(ref);
      for(int y = 0; y < height * scale; y++)
        for(int x = 0; x < width * scale; x++)
        {
          const int r = data[(size_t)stride * y + x], a = _coverage_at(c, x, y);
          if(a != r)
            fail_msg("shape %d, scale %d: (%d, %d) is %d, cairo paints %d", shape, scale, x, y,
                     a, r);
        }
      if(!shape)
      {
        // a quarter of the column, in rows the clip covers whole
        const int column = 40 * scale, row = 40 * scale;
        assert_true(abs(_coverage_at(c, column, row) - 64 * scale) <= 1);
        assert_int_equal(_coverage_at(c, column + 1, row), 0);
        assert_int_equal(_coverage_at(c, column - 1, row), 0);
      }
      cairo_surface_destroy(ref);
      _coverage_unref(c);
    }
}

// a frame of one row, sampled at device pixels (x, 0) with the frame moved
// left by shift, through the compose's sampling
static void _sample_row(const float *row,
                        const int width,
                        const double shift,
                        const gboolean nearest,
                        const int x,
                        float color[3])
{
  float *pixels = g_new(float, (size_t)4 * width);
  for(int k = 0; k < width; k++)
    for(int c = 0; c < 4; c++) pixels[4 * k + c] = c < 3 ? row[k] : 1.0f;
  _source_t s = { .pixels = pixels, .width = width, .height = 1, .nearest = nearest };
  cairo_matrix_init_translate(&s.inverse, shift, 0.0);
  _sample(&s, x, 0, 0.0f, 4.0f, color);
  g_free(pixels);
}

/*
 * texels that are not finite: an infinite one makes an infinite sample
 * where it weighs, clipped to the range, and does not count where its
 * weight is 0; +inf and -inf together, and NaN where it weighs, give the
 * range's bottom. bilinear weights of 0 come from sampling at a texel's
 * center, here and in the second, missing row of a frame of one row
 */
static void test_sample_nonfinite(void **state)
{
  const float lo = 0.0f, hi = 4.0f;
  const struct
  {
    float row[3];
    double shift;
    gboolean nearest;
    int x;
    float want;
  } cases[] = {
    { { 0.5f, INFINITY, 0.25f }, 0.0, FALSE, 0, 0.5f },
    { { 0.5f, INFINITY, 0.25f }, 0.0, FALSE, 1, hi },
    { { 0.5f, INFINITY, 0.25f }, 0.0, FALSE, 2, 0.25f },
    { { 0.5f, INFINITY, 0.25f }, 0.25, FALSE, 0, hi },
    { { 0.5f, -INFINITY, 0.25f }, 0.25, FALSE, 0, lo },
    { { 0.5f, -INFINITY, 0.25f }, 0.0, FALSE, 2, 0.25f },
    { { INFINITY, -INFINITY, 0.25f }, 0.25, FALSE, 0, lo },
    { { INFINITY, -INFINITY, 0.25f }, 0.0, FALSE, 0, hi },
    { { 0.5f, NAN, 0.25f }, 0.0, FALSE, 0, 0.5f },
    { { 0.5f, NAN, 0.25f }, 0.25, FALSE, 0, lo },
    { { 0.5f, INFINITY, 0.25f }, 0.0, TRUE, 1, hi },
    { { 0.5f, -INFINITY, 0.25f }, 0.0, TRUE, 1, lo },
    { { 0.5f, NAN, 0.25f }, 0.0, TRUE, 1, lo },
    { { 0.5f, NAN, 0.25f }, 0.0, TRUE, 2, 0.25f },
  };
  for(int k = 0; k < G_N_ELEMENTS(cases); k++)
  {
    float color[3];
    _sample_row(cases[k].row, 3, cases[k].shift, cases[k].nearest, cases[k].x, color);
    for(int c = 0; c < 3; c++)
      if(color[c] != cases[k].want)
        fail_msg("case %d: %g, want %g", k, color[c], cases[k].want);
  }
}

static void _check_halves(const float *row,
                          const int n,
                          const float scale[4],
                          uint16_t *got)
{
  _halves(row, scale, got, n / 4);
  for(int k = 0; k < n; k++)
    if(got[k] != _half(scale[k % 4] * row[k]))
      fail_msg("%.9g times %g: %04x, _half() %04x", row[k], scale[k % 4], got[k],
               _half(scale[k % 4] * row[k]));
}

/*
 * the half floats of a row, eight at a time where the build has F16C, are
 * _half()'s: every 61st float of [0, 65504), the ties between two halves,
 * subnormal and normal, and -0, as they are and scaled
 */
static void test_halves(void **state)
{
  enum { N = 4096 };
  float *row = g_new(float, 4 * N);
  uint16_t *got = g_new(uint16_t, 4 * N);
  const float scales[][4] = { { 1.0f, 1.0f, 1.0f, 1.0f }, { 0.25f, 0.5f, 1.0f / 3.0f, 1.0f } };
  long count = 0;
  for(int s = 0; s < G_N_ELEMENTS(scales); s++)
  {
    // 65504 is 0x477fe000
    for(uint32_t u = 0; u < 0x477fe000u;)
    {
      int n = 0;
      for(; n < 4 * N && u < 0x477fe000u; n++, u += 61) memcpy(row + n, &u, sizeof(float));
      while(n % 4) row[n++] = 0.0f;
      _check_halves(row, n, scales[s], got);
      count += n;
    }
    int n = 0;
    for(int k = 0; k < 1024; k++) row[n++] = ldexpf(k + 0.5f, -24);
    for(int e = -14; e < 16; e++)
    {
      row[n++] = ldexpf(1.0f + 1.0f / 2048.0f, e);
      row[n++] = ldexpf(1.0f + 3.0f / 2048.0f, e);
    }
    // by its bits: -ffast-math need not keep the sign of a -0.0f constant
    const uint32_t negative_zero = 0x80000000u;
    do memcpy(row + n++, &negative_zero, sizeof(float));
    while(n % 4);
    _check_halves(row, n, scales[s], got);
    count += n;
  }
  print_message("%ld values\n", count);
  g_free(row);
  g_free(got);
}

// a frame translated by (tx, ty) logical pixels, at 1:1 in device pixels
static void _translated(_placement_t *p,
                        const int scale,
                        const double tx,
                        const double ty)
{
  cairo_matrix_init_translate(&p->matrix, tx, ty);
  cairo_matrix_scale(&p->matrix, 1.0 / scale, 1.0 / scale);
}

/*
 * translations the compose reads without the transform (_shift()) compose
 * the same bytes as through it: nearest by any offset, bilinear by whole
 * device pixels, through clips at fractions (edge coverage), of frames
 * smaller and larger than the widget, kept coverages of a larger widget,
 * values beyond the clip, infinite and NaN, under each other and under a
 * general placement. a scale, a rotation, bilinear at a fraction and
 * nearest within rounding of a texel's edge keep the general path
 */
static void test_compose_translation(void **state)
{
  GRand *rand = g_rand_new_with_seed(17);
  int shifted = 0;
  for(int n = 0; n < 160; n++)
  {
    const int scale = 1 + n % 2, width = 150 / scale, height = 110 / scale;
    // a kept coverage, made for a widget larger than the compose's
    const int grown = n % 5 == 4 ? 12 : 0;
    const int W = width * scale, H = height * scale;
    const int count = 1 + n % 3;
    _placement_t placements[3];
    _source_t sources[3];
    _record_t records[3];
    float *frames[3];
    for(int i = 0; i < count; i++)
    {
      _placement_t *p = &placements[i];
      _random_placement(rand, width + grown, height + grown, 300 / scale, p);
      // frames smaller than the widget and larger
      p->frame_width = g_rand_int_range(rand, 3, n % 2 ? 60 : W + 80);
      p->frame_height = g_rand_int_range(rand, 3, n % 2 ? 50 : H + 80);
      const gboolean nearest = i == 2 || n % 4 < 2;
      p->filter = nearest ? CAIRO_FILTER_FAST : CAIRO_FILTER_GOOD;
      // a third image placed in general, under or above the others
      if(i < 2)
      {
        // whole device pixels, and for nearest half ones and any fraction
        const int grid = nearest ? (n % 3 == 0 ? 0 : 2) : 1;
        double t[2];
        for(int k = 0; k < 2; k++)
        {
          t[k] = g_rand_double_range(rand, -0.6, 0.4) * (k ? H : W);
          t[k] = grid ? round(t[k] * grid) / grid : t[k];
        }
        _translated(p, scale, t[0] / scale, t[1] / scale);
      }
      frames[i] = _random_frame(rand, p);
      _coverage_t *c = _coverage(p, width + grown, height + grown, scale);
      records[i] = (_record_t){ .refs = 1, .coverage = c,
                                .backdrop = g_malloc(c->fractional[c->box[3] - c->box[1]] + 1) };
      for(int k = 0; k < c->fractional[c->box[3] - c->box[1]]; k++)
        records[i].backdrop[k] = g_rand_int_range(rand, 0, 256);
      _source_t *s = &sources[i];
      *s = (_source_t){ .pixels = frames[i], .width = p->frame_width, .height = p->frame_height,
                        .nearest = nearest, .coverage = c, .backdrop = records[i].backdrop };
      cairo_matrix_t device;
      cairo_matrix_init_scale(&device, scale, scale);
      cairo_matrix_multiply(&s->inverse, &p->matrix, &device);
      assert_int_equal(cairo_matrix_invert(&s->inverse), CAIRO_STATUS_SUCCESS);
      const gboolean translation = i < 2 && s->inverse.xx == 1.0 && s->inverse.yy == 1.0
                                   && s->inverse.xy == 0.0 && s->inverse.yx == 0.0;
      if(_shift(s, W, H)) shifted++;
      else if(translation && i < 2)
        fail_msg("scene %d: a translation by (%.17g, %.17g), %s, takes the general path", n,
                 s->inverse.x0, s->inverse.y0, nearest ? "nearest" : "bilinear");
    }
    // under each other in either order
    if(n % 2)
    {
      const _source_t t = sources[0];
      sources[0] = sources[count - 1];
      sources[count - 1] = t;
    }

    const dt_presenter_encoding_t e = n % 7 == 3 ? _scrgb : _diagonal(4.0f, 0.25f);
    uint16_t *fast = g_new(uint16_t, (size_t)4 * W * H), *general = g_new(uint16_t, (size_t)4 * W * H);
    const dt_presenter_reservation_t rf = { .image = fast, .image_stride = (size_t)8 * W, .encoding = e };
    const dt_presenter_reservation_t rg = { .image = general, .image_stride = (size_t)8 * W, .encoding = e };
    assert_true(_compose(sources, count, W, H, &rf));
    for(int i = 0; i < count; i++) sources[i].shifted = FALSE;
    assert_true(_compose(sources, count, W, H, &rg));
    for(size_t k = 0; k < (size_t)4 * W * H; k++)
      if(fast[k] != general[k])
        fail_msg("scene %d: pixel (%zu, %zu) channel %zu: %04x, through the transform %04x", n,
                 k / 4 % W, k / 4 / W, k % 4, fast[k], general[k]);
    g_free(fast);
    g_free(general);
    for(int i = 0; i < count; i++)
    {
      g_free(frames[i]);
      g_free(records[i].backdrop);
      _coverage_unref(records[i].coverage);
    }
  }
  print_message("%d translated sources\n", shifted);
  assert_true(shifted > 150);

  // placements the compose transforms
  const struct
  {
    cairo_matrix_t inverse;
    gboolean nearest;
  } general[] = {
    { { 1.0 + 0x1p-40, 0.0, 0.0, 1.0, 3.0, 4.0 }, TRUE },
    { { 2.0, 0.0, 0.0, 2.0, 0.0, 0.0 }, TRUE },
    { { 0.5, 0.0, 0.0, 0.5, 0.0, 0.0 }, FALSE },
    { { 1.0, 0.0, 0x1p-30, 1.0, 3.0, 4.0 }, TRUE },
    { { 0.8, -0.6, 0.6, 0.8, 0.0, 0.0 }, TRUE },
    { { 1.0, 0.0, 0.0, 1.0, 3.5, 4.0 }, FALSE },
    { { 1.0, 0.0, 0.0, 1.0, 3.0, 4.25 }, FALSE },
    // within rounding of a texel's edge
    { { 1.0, 0.0, 0.0, 1.0, 2.5 - 0x1p-40, 4.0 }, TRUE },
    { { 1.0, 0.0, 0.0, 1.0, 3.0, -1.5 + 0x1p-40 }, TRUE },
    { { 1.0, 0.0, 0.0, 1.0, 0x1p21, 4.0 }, TRUE },
    { { 1.0, 0.0, 0.0, 1.0, NAN, 4.0 }, TRUE },
  };
  for(int k = 0; k < G_N_ELEMENTS(general); k++)
  {
    _source_t s = { .inverse = general[k].inverse, .nearest = general[k].nearest,
                    .width = 10, .height = 10 };
    if(_shift(&s, 200, 100)) fail_msg("placement %d takes the translation's path", k);
  }
  // and the ones it reads directly, with their offsets
  const struct
  {
    double x0, y0;
    gboolean nearest;
    int du, dv;
  } direct[] = {
    { 3.0, -4.0, FALSE, 3, -4 },  { 3.0, -4.0, TRUE, 3, -4 },  { 3.5, -4.5, TRUE, 4, -4 },
    { 3.25, -4.75, TRUE, 3, -5 }, { -0.5, 0.49, TRUE, 0, 0 },  { 2.5 - 0x1p-20, 0.0, TRUE, 2, 0 },
  };
  for(int k = 0; k < G_N_ELEMENTS(direct); k++)
  {
    _source_t s = { .nearest = direct[k].nearest, .width = 10, .height = 10 };
    cairo_matrix_init_translate(&s.inverse, direct[k].x0, direct[k].y0);
    assert_true(_shift(&s, 200, 100));
    assert_int_equal(s.du, direct[k].du);
    assert_int_equal(s.dv, direct[k].dv);
  }
  g_rand_free(rand);
}

/*
 * a presentation kept at another device scale is dropped when it would be
 * shown: its foreground was cut by coverages of that scale, and cutting it
 * again for the new one would cut twice. at its own scale it is painted
 */
static void test_keep_scale(void **state)
{
  _widget_t l = { .width = 60, .height = 40, .scale = 2 };
  l.kept_images = g_array_new(FALSE, FALSE, sizeof(dt_presenter_image_t));
  l.drawing = g_ptr_array_new_with_free_func(_record_unref);
  GArray *images = g_array_new(FALSE, FALSE, sizeof(dt_presenter_image_t));
  cairo_surface_t *target = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 120, 80);
  cairo_surface_set_device_scale(target, 2, 2);
  cairo_t *cr = cairo_create(target);
  cairo_matrix_t base;
  cairo_matrix_init_identity(&base);

  for(int scale = 1; scale <= 2; scale++)
  {
    l.kept = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 60 * scale, 40 * scale);
    cairo_surface_set_device_scale(l.kept, scale, scale);
    cairo_t *k = cairo_create(l.kept);
    cairo_set_source_rgb(k, 1.0, 0.0, 0.0);
    cairo_paint(k);
    cairo_destroy(k);
    const gboolean shown = _keep(NULL, &l, cr, &base, images);
    assert_int_equal(shown, scale == 2);
    assert_int_equal(l.kept != NULL, scale == 2);
  }
  cairo_surface_flush(target);
  assert_int_equal(((uint32_t *)cairo_image_surface_get_data(target))[0], 0xffff0000);

  _drop_kept(&l);
  g_array_free(l.kept_images, TRUE);
  g_ptr_array_free(l.drawing, TRUE);
  g_array_free(images, TRUE);
  cairo_destroy(cr);
  cairo_surface_destroy(target);
}

int main(int argc, char *argv[])
{
  darktable.num_openmp_threads = dt_get_num_procs();
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_coverage_is_cairos),
    cmocka_unit_test(test_coverage_of_clips),
    cmocka_unit_test(test_sample_nonfinite),
    cmocka_unit_test(test_compose_random),
    cmocka_unit_test(test_compose_preview_fallback),
    cmocka_unit_test(test_halves),
    cmocka_unit_test(test_compose_translation),
    cmocka_unit_test(test_keep_scale),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
