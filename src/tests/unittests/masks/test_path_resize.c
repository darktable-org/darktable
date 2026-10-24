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

// Grow/shrink of a path (rasterize, offset, re-vectorize), as the scroll wheel
// and the mask manager's slider drive it through the form's resize().
//
// What a user sees while scrolling is a sequence of shapes, so that is what is
// measured, on the curve the path actually draws (its cubic segments), not on
// its node list:
//
//   * fidelity: every point of the resized outline lies at the requested
//     distance from the original outline, on the requested side. A thinning
//     that keeps nodes but loses their handles shows up here as straight
//     chords cutting across the curve
//   * stability: one scroll step moves the outline by about one step, however
//     many nodes it takes to draw it, and the node count does not swing
//     between steps (nodes appearing and vanishing as the wheel turns)

#include "flexi_fixture.h"
#include "test_path_resize_ai_mask.h"

#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>

#define IMG_W 6000.0f
#define IMG_H 4000.0f

// the offset error the raster pipeline may add, in image pixels: the 2048 px
// tracing grid of a shape this size is ~1 image px, plus the fit tolerance
#define TOL_PX 2.5f

// points sampled per cubic segment
#define SEG_SAMPLES 48

static dt_dev_pixelpipe_t _pipe;
static dt_develop_t _dev;

static int _setup(void **state)
{
  flexi_conf_init();
  // the parallel loops size their thread teams from it
  darktable.num_openmp_threads = dt_get_num_procs();
  memset(&_dev, 0, sizeof(_dev));
  memset(&_pipe, 0, sizeof(_pipe));
  _pipe.iwidth = IMG_W;
  _pipe.iheight = IMG_H;
  _pipe.processed_width = IMG_W;
  _pipe.processed_height = IMG_H;
  _pipe.iscale = 1.0f;
  _dev.preview_pipe = &_pipe;
  darktable.develop = &_dev;
  return 0;
}

static int _teardown(void **state)
{
  darktable.develop = NULL;
  flexi_conf_cleanup();
  return 0;
}

typedef struct _pt_t
{
  double x, y; // image pixels
} _pt_t;

// an ellipse of semi-axes a x b px, drawn with n nodes. The handles are the
// standard cubic arc approximation, so the baseline is the ellipse to within a
// small fraction of a pixel and the tests need no reference but the shape itself
static dt_masks_form_t *_ellipse_path(const float a, const float b, const int n)
{
  dt_masks_form_t *form = dt_masks_create(DT_MASKS_PATH);
  const double k = 4.0 / 3.0 * tan(M_PI / (2.0 * n));
  for(int i = 0; i < n; i++)
  {
    const double t = 2.0 * M_PI * i / n;
    const double x = a * cos(t), y = b * sin(t);
    const double dx = -a * sin(t), dy = b * cos(t);
    dt_masks_point_path_t *pt = calloc(1, sizeof(*pt));
    pt->corner[0] = (IMG_W / 2 + x) / IMG_W;
    pt->corner[1] = (IMG_H / 2 + y) / IMG_H;
    pt->ctrl1[0] = (IMG_W / 2 + x - k * dx) / IMG_W;
    pt->ctrl1[1] = (IMG_H / 2 + y - k * dy) / IMG_H;
    pt->ctrl2[0] = (IMG_W / 2 + x + k * dx) / IMG_W;
    pt->ctrl2[1] = (IMG_H / 2 + y + k * dy) / IMG_H;
    pt->border[0] = pt->border[1] = 0.05f;
    pt->state = DT_MASKS_POINT_STATE_USER;
    form->points = g_list_append(form->points, pt);
  }
  return form;
}

// a closed shape r(t), t in [0, 2pi), drawn with n nodes whose handles are the
// finite-difference tangent of the shape, so it follows r(t) closely for the n
// used here
typedef double (*_radius_fn)(const double t);

static dt_masks_form_t *_polar_path(const _radius_fn r, const int n)
{
  dt_masks_form_t *form = dt_masks_create(DT_MASKS_PATH);
  const double dt = 2.0 * M_PI / n, e = 1e-4;
  for(int i = 0; i < n; i++)
  {
    const double t = dt * i;
    const double x = r(t) * cos(t), y = r(t) * sin(t);
    const double dx = (r(t + e) * cos(t + e) - r(t - e) * cos(t - e)) / (2 * e);
    const double dy = (r(t + e) * sin(t + e) - r(t - e) * sin(t - e)) / (2 * e);
    dt_masks_point_path_t *pt = calloc(1, sizeof(*pt));
    pt->corner[0] = (IMG_W / 2 + x) / IMG_W;
    pt->corner[1] = (IMG_H / 2 + y) / IMG_H;
    pt->ctrl1[0] = (IMG_W / 2 + x - dx * dt / 3) / IMG_W;
    pt->ctrl1[1] = (IMG_H / 2 + y - dy * dt / 3) / IMG_H;
    pt->ctrl2[0] = (IMG_W / 2 + x + dx * dt / 3) / IMG_W;
    pt->ctrl2[1] = (IMG_H / 2 + y + dy * dt / 3) / IMG_H;
    pt->border[0] = pt->border[1] = 0.05f;
    pt->state = DT_MASKS_POINT_STATE_USER;
    form->points = g_list_append(form->points, pt);
  }
  return form;
}

// a bean: round, with a deep dent on one side
static double _bean(const double t)
{
  return 450.0 * (1.0 - 0.4 * pow(cos(t / 2.0), 16.0));
}

// a square of side `side` px with sharp corners (handles on their nodes)
static dt_masks_form_t *_square_path(const float side)
{
  dt_masks_form_t *form = dt_masks_create(DT_MASKS_PATH);
  const float h = side / 2;
  const float xy[4][2] = { { -h, -h }, { h, -h }, { h, h }, { -h, h } };
  for(int i = 0; i < 4; i++)
  {
    dt_masks_point_path_t *pt = calloc(1, sizeof(*pt));
    pt->corner[0] = pt->ctrl1[0] = pt->ctrl2[0] = (IMG_W / 2 + xy[i][0]) / IMG_W;
    pt->corner[1] = pt->ctrl1[1] = pt->ctrl2[1] = (IMG_H / 2 + xy[i][1]) / IMG_H;
    pt->border[0] = pt->border[1] = 0.05f;
    pt->state = DT_MASKS_POINT_STATE_USER;
    form->points = g_list_append(form->points, pt);
  }
  return form;
}

// the outline the path draws, in image pixels
static GArray *_outline(const dt_masks_form_t *form)
{
  GArray *out = g_array_new(FALSE, FALSE, sizeof(_pt_t));
  for(const GList *l = form->points; l; l = g_list_next(l))
  {
    const dt_masks_point_path_t *a = l->data;
    const dt_masks_point_path_t *b = g_list_next_wraparound(l, form->points)->data;
    for(int k = 0; k < SEG_SAMPLES; k++)
    {
      const double t = (double)k / SEG_SAMPLES, u = 1.0 - t;
      const double w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
      const _pt_t p = {
        (w0 * a->corner[0] + w1 * a->ctrl2[0] + w2 * b->ctrl1[0] + w3 * b->corner[0]) * IMG_W,
        (w0 * a->corner[1] + w1 * a->ctrl2[1] + w2 * b->ctrl1[1] + w3 * b->corner[1]) * IMG_H
      };
      g_array_append_val(out, p);
    }
  }
  return out;
}

static GArray *_copy_outline(const GArray *o)
{
  GArray *c = g_array_new(FALSE, FALSE, sizeof(_pt_t));
  g_array_append_vals(c, o->data, o->len);
  return c;
}

// distance from p to the polyline `o` (closed), segment-exact
static double _dist_to(const GArray *o, const _pt_t p)
{
  double best = 1e30;
  for(guint i = 0; i < o->len; i++)
  {
    const _pt_t a = g_array_index(o, _pt_t, i);
    const _pt_t b = g_array_index(o, _pt_t, (i + 1) % o->len);
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double l2 = dx * dx + dy * dy;
    double t = l2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2 : 0.0;
    t = CLAMP(t, 0.0, 1.0);
    const double ex = a.x + t * dx - p.x, ey = a.y + t * dy - p.y;
    best = fmin(best, ex * ex + ey * ey);
  }
  return sqrt(best);
}

static gboolean _inside(const GArray *o, const _pt_t p)
{
  gboolean in = FALSE;
  for(guint i = 0, j = o->len - 1; i < o->len; j = i++)
  {
    const _pt_t a = g_array_index(o, _pt_t, i), b = g_array_index(o, _pt_t, j);
    if((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
      in = !in;
  }
  return in;
}

// largest |signed distance to `base` - offset| over the outline `o`; the
// signed distance is positive outside `base`
static double _offset_error(const GArray *o, const GArray *base, const double offset)
{
  double worst = 0.0;
  for(guint i = 0; i < o->len; i++)
  {
    const _pt_t p = g_array_index(o, _pt_t, i);
    const double d = _dist_to(base, p) * (_inside(base, p) ? -1.0 : 1.0);
    worst = fmax(worst, fabs(d - offset));
  }
  return worst;
}

// largest distance from a point of `a` to the curve `b`, and the other way
static double _hausdorff(const GArray *a, const GArray *b)
{
  double worst = 0.0;
  for(guint i = 0; i < a->len; i++)
    worst = fmax(worst, _dist_to(b, g_array_index(a, _pt_t, i)));
  for(guint i = 0; i < b->len; i++)
    worst = fmax(worst, _dist_to(a, g_array_index(b, _pt_t, i)));
  return worst;
}

static void _free_form(dt_masks_form_t *form)
{
  dt_masks_free_form(form);
}

// resize by `amount` px and return the outline, asserting it succeeded
static GArray *_resized(dt_masks_form_t *form, const int amount)
{
  assert_true(form->functions->resize(form, amount, FALSE));
  return _outline(form);
}

static void _check_offsets(dt_masks_form_t *form, const char *what, const int *amounts, const int count)
{
  GArray *base = _outline(form);

  for(int i = 0; i < count; i++)
  {
    GArray *o = _resized(form, amounts[i]);
    const double err = _offset_error(o, base, amounts[i]);
    print_message("  %s, offset %+d: %u nodes, worst error %.2f px\n",
                  what, amounts[i], g_list_length(form->points), err);
    assert_true(err <= TOL_PX);
    g_array_free(o, TRUE);
  }
  g_array_free(base, TRUE);
  _free_form(form);
}

static void test_circle_offset_is_exact(void **state)
{
  const int amounts[] = { 20, 60, 150, -20, -60, -150 };
  _check_offsets(_ellipse_path(500, 500, 8), "circle", amounts, 6);
}

// the long thin shape of an eye: strong curvature at the tips, where a node
// thinning is most tempted to cut a corner
static void test_lens_offset_is_exact(void **state)
{
  const int amounts[] = { 10, 40, 100, -10, -40, -80 };
  _check_offsets(_ellipse_path(700, 160, 10), "lens", amounts, 6);
}

// a shrink keeps a square's corners sharp and a grow rounds them; rounding them
// on the way in would put the outline 40% too far from the original there
// a path drawn the other way round is resized the same way
static void test_reverse_winding_is_resized_the_same(void **state)
{
  dt_masks_form_t *form = _ellipse_path(700, 160, 10);
  form->points = g_list_reverse(form->points);
  for(GList *l = form->points; l; l = g_list_next(l))
  {
    dt_masks_point_path_t *pt = l->data;
    for(int k = 0; k < 2; k++)
    {
      const float t = pt->ctrl1[k];
      pt->ctrl1[k] = pt->ctrl2[k];
      pt->ctrl2[k] = t;
    }
  }
  const int amounts[] = { 40, -40 };
  _check_offsets(form, "reversed lens", amounts, 2);
}

// both curve smoothing preferences stay close to the requested offset
static void test_smoothing_preferences_stay_faithful(void **state)
{
  const char *levels[] = { "sharp", "smooth" };
  const int amounts[] = { 25, -25 };
  for(int i = 0; i < 2; i++)
  {
    dt_conf_set_string("masks/path_resize_curve_smoothing", levels[i]);
    _check_offsets(_polar_path(_bean, 24), levels[i], amounts, 2);
  }
  dt_conf_set_string("masks/path_resize_curve_smoothing", "smooth");
}

static void test_square_keeps_and_rounds_its_corners(void **state)
{
  const int amounts[] = { 30, 120, -30, -120 };
  _check_offsets(_square_path(800), "square", amounts, 4);
}

// the dent is concave, where an offset curve is the easiest to get wrong
static void test_dented_shape_offset_is_exact(void **state)
{
  const int amounts[] = { 15, 60, -15, -60 };
  _check_offsets(_polar_path(_bean, 24), "bean", amounts, 4);
}

// a shrink past what the shape can take is refused and leaves it alone
static void test_shrinking_to_nothing_is_refused(void **state)
{
  dt_masks_form_t *form = _ellipse_path(300, 300, 8);
  GArray *before = _outline(form);
  assert_false(form->functions->resize(form, -400, FALSE));
  GArray *after = _outline(form);
  assert_true(_hausdorff(before, after) < 0.01);
  g_array_free(before, TRUE);
  g_array_free(after, TRUE);
  _free_form(form);
}

// scrolling: consecutive steps move the outline by about one step, and the
// node count does not swing from step to step
// `slack` is what one step may move the outline beyond its size: about two
// raster pixels, which on a large path are bigger than image pixels
static void _check_steps(dt_masks_form_t *form,
                         const char *what,
                         const int step,
                         const int max_swing,
                         const double slack)
{

  // the first step is left out of the node count: a sharp corner turning into
  // a round one takes nodes, once, and that is not what is being measured
  GArray *prev = _outline(form);
  int prev_nodes = -1;
  int worst_swing = 0;
  for(int s = 1; s <= 12; s++)
  {
    GArray *o = _resized(form, s * step);
    const int nodes = (int)g_list_length(form->points);
    const double moved = _hausdorff(prev, o);
    print_message("  %s step %2d: %3d nodes, moved %.2f px (step %d)\n",
                  what, s, nodes, moved, step);
    assert_true(moved <= step + slack);
    if(prev_nodes >= 0)
      worst_swing = MAX(worst_swing, abs(nodes - prev_nodes));
    g_array_free(prev, TRUE);
    prev = _copy_outline(o);
    prev_nodes = nodes;
    g_array_free(o, TRUE);
  }
  assert_true(worst_swing <= max_swing);
  g_array_free(prev, TRUE);
  _free_form(form);
}

static void test_scrolling_circle_is_steady(void **state)
{
  _check_steps(_ellipse_path(500, 500, 8), "circle", 8, 2, 2.0);
}

static void test_scrolling_bean_is_steady(void **state)
{
  _check_steps(_polar_path(_bean, 24), "bean", 8, 2, 2.0);
}

static void test_scrolling_square_is_steady(void **state)
{
  // the corners are tight arcs on the first steps and take a node or two more
  // there than they do once they are wide
  _check_steps(_square_path(800), "square", 8, 4, 2.0);
}

// a large path is traced on a raster coarser than its pixels, where the
// outline's own wobble used to switch nodes on and off from step to step
static double _big_bean(const double t)
{
  return 3.0 * _bean(t);
}

static void test_scrolling_a_large_path_is_steady(void **state)
{
  _check_steps(_ellipse_path(2800, 1800, 12), "large ellipse", 6, 0, 3.5);
}

static void test_scrolling_a_large_sparse_path_is_steady(void **state)
{
  _check_steps(_polar_path(_big_bean, 12), "large bean", 6, 1, 3.5);
}

static void test_scrolling_lens_is_steady(void **state)
{
  _check_steps(_ellipse_path(700, 160, 10), "lens", 8, 2, 2.0);
}

// going back and forth lands on the shape it left
static void test_revisiting_an_offset_restores_it(void **state)
{
  dt_masks_form_t *form = _ellipse_path(700, 160, 10);
  GArray *first = _resized(form, 30);
  GArray *other = _resized(form, 90);
  GArray *again = _resized(form, 30);
  assert_true(_hausdorff(first, again) < 0.01);
  assert_true(_hausdorff(first, other) > 10.0);
  g_array_free(first, TRUE);
  g_array_free(other, TRUE);
  g_array_free(again, TRUE);
  _free_form(form);
}

// a shape cached under one setting is not handed back under another
static void test_changing_a_setting_is_not_answered_from_the_cache(void **state)
{
  dt_masks_form_t *form = _ellipse_path(500, 500, 8);
  assert_true(form->functions->resize(form, 40, FALSE));
  const guint smooth = g_list_length(form->points);
  assert_true(form->functions->resize(form, 80, FALSE));

  dt_conf_set_string("masks/path_resize_curve_smoothing", "sharp");
  assert_true(form->functions->resize(form, 40, FALSE));
  assert_true(g_list_length(form->points) > smooth);
  dt_conf_set_string("masks/path_resize_curve_smoothing", "smooth");
  _free_form(form);
}

// moving a path keeps what it has grown or shrunk: 0 restores its original
// outline where it now is, and an offset it was at returns as it was
static void test_moving_a_path_keeps_its_original_outline(void **state)
{
  dt_masks_form_t *form = _ellipse_path(700, 160, 10);
  GArray *original = _outline(form);
  GArray *grown = _resized(form, 40);

  const float dx = 0.05f, dy = -0.03f;
  for(GList *l = form->points; l; l = g_list_next(l))
  {
    dt_masks_point_path_t *pt = l->data;
    pt->corner[0] += dx;
    pt->corner[1] += dy;
    pt->ctrl1[0] += dx;
    pt->ctrl1[1] += dy;
    pt->ctrl2[0] += dx;
    pt->ctrl2[1] += dy;
  }
  dt_masks_path_resize_follow_move(form->formid, dx, dy);

  GArray *back = _resized(form, 0);
  GArray *grown_again = _resized(form, 40);
  for(guint i = 0; i < original->len; i++)
  {
    _pt_t *p = &g_array_index(original, _pt_t, i);
    p->x += dx * IMG_W;
    p->y += dy * IMG_H;
  }
  for(guint i = 0; i < grown->len; i++)
  {
    _pt_t *p = &g_array_index(grown, _pt_t, i);
    p->x += dx * IMG_W;
    p->y += dy * IMG_H;
  }
  assert_true(_hausdorff(back, original) < 0.01);
  assert_true(_hausdorff(grown_again, grown) < 0.01);
  g_array_free(original, TRUE);
  g_array_free(grown, TRUE);
  g_array_free(back, TRUE);
  g_array_free(grown_again, TRUE);
  _free_form(form);
}

// an AI object's outline is a bitmap trace: hundreds of nodes on a stair-stepped
// edge, with two thin stalks. Its nodes land scattered on the offset outline,
// and the ones that survive must still be fitted to it
static dt_masks_form_t *_ai_mask_path(void)
{
  dt_masks_form_t *form = dt_masks_create(DT_MASKS_PATH);
  for(size_t i = 0; i < G_N_ELEMENTS(_ai_mask); i++)
  {
    dt_masks_point_path_t *pt = calloc(1, sizeof(*pt));
    pt->corner[0] = _ai_mask[i][0];
    pt->corner[1] = _ai_mask[i][1];
    pt->ctrl1[0] = _ai_mask[i][2];
    pt->ctrl1[1] = _ai_mask[i][3];
    pt->ctrl2[0] = _ai_mask[i][4];
    pt->ctrl2[1] = _ai_mask[i][5];
    pt->border[0] = pt->border[1] = 0.0005f;
    pt->state = DT_MASKS_POINT_STATE_USER;
    form->points = g_list_append(form->points, pt);
  }
  return form;
}

static void test_an_ai_mask_follows_its_offset(void **state)
{
  dt_masks_form_t *form = _ai_mask_path();
  GArray *base = _outline(form);
  const guint nodes = g_list_length(form->points);

  // the way the wheel goes up and comes back
  const int amounts[] = { 10, 14, 10, 22, 34, 22, 14, 6, 2, 0, -4, -10, -4 };
  for(size_t i = 0; i < G_N_ELEMENTS(amounts); i++)
  {
    GArray *o = _resized(form, amounts[i]);
    double worst = 0.0;
    for(guint k = 0; k < o->len; k += 2)
      worst = fmax(worst, fabs(_dist_to(base, g_array_index(o, _pt_t, k)) - abs(amounts[i])));
    print_message("  AI mask, offset %+d: %u nodes, worst error %.2f px\n", amounts[i],
                  g_list_length(form->points), worst);
    // a few raster pixels of the fit tolerance, on this large shape
    assert_true(worst <= 5.0);
    // thinned by the fit, not collapsed to a handful of nodes
    assert_true(g_list_length(form->points) > nodes / 2);
    g_array_free(o, TRUE);
  }
  g_array_free(base, TRUE);
  _free_form(form);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_circle_offset_is_exact),
    cmocka_unit_test(test_lens_offset_is_exact),
    cmocka_unit_test(test_reverse_winding_is_resized_the_same),
    cmocka_unit_test(test_smoothing_preferences_stay_faithful),
    cmocka_unit_test(test_square_keeps_and_rounds_its_corners),
    cmocka_unit_test(test_dented_shape_offset_is_exact),
    cmocka_unit_test(test_shrinking_to_nothing_is_refused),
    cmocka_unit_test(test_scrolling_circle_is_steady),
    cmocka_unit_test(test_scrolling_lens_is_steady),
    cmocka_unit_test(test_scrolling_bean_is_steady),
    cmocka_unit_test(test_scrolling_square_is_steady),
    cmocka_unit_test(test_scrolling_a_large_path_is_steady),
    cmocka_unit_test(test_scrolling_a_large_sparse_path_is_steady),
    cmocka_unit_test(test_revisiting_an_offset_restores_it),
    cmocka_unit_test(test_changing_a_setting_is_not_answered_from_the_cache),
    cmocka_unit_test(test_moving_a_path_keeps_its_original_outline),
    cmocka_unit_test(test_an_ai_mask_follows_its_offset),
  };
  return cmocka_run_group_tests(tests, _setup, _teardown);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
