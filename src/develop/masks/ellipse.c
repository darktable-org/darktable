/*
    This file is part of darktable,
    Copyright (C) 2013-2021 darktable developers.

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
#include "bauhaus/bauhaus.h"
#include "common/debug.h"
#include "common/undo.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/openmp_maths.h"

static inline void _ellipse_point_transform(const float xref, const float yref, const float x, const float y,
                                            const float sinr, const float cosr, float *xnew, float *ynew)
{
  const float xtmp = (sinr * sinr + cosr * cosr) * (x - xref) + (cosr * sinr - cosr * sinr) * (y - yref);
  const float ytmp = (cosr * sinr - cosr * sinr) * (x - xref) + (sinr * sinr + cosr * cosr) * (y - yref);

  *xnew = xref + xtmp;
  *ynew = yref + ytmp;
}

// Jordan's point in polygon test
static int _ellipse_cross_test(float x, float y, float *point_1, float *point_2)
{
  const float x_a = x;
  const float y_a = y;
  float x_b = point_1[0];
  float y_b = point_1[1];
  float x_c = point_2[0];
  float y_c = point_2[1];

  if(y_a == y_b && y_b == y_c)
  {
    if((x_b <= x_a && x_a <= x_c) || (x_c <= x_a && x_a <= x_b))
      return 0;
    else
      return 1;
  }

  if(y_b > y_c)
  {
    float tmp;
    tmp = x_b, x_b = x_c, x_c = tmp;
    tmp = y_b, y_b = y_c, y_c = tmp;
  }

  if(y_a == y_b && x_a == x_b) return 0;

  if(y_a <= y_b || y_a > y_c) return 1;

  const float delta = (x_b - x_a) * (y_c - y_a) - (y_b - y_a) * (x_c - x_a);

  if(delta > 0)
    return -1;
  else if(delta < 0)
    return 1;
  else
    return 0;
}

static int _ellipse_point_in_polygon(float x, float y, float *points, int points_count)
{
  int t = -1;

  t *= _ellipse_cross_test(x, y, points + 2 * (points_count - 1), points);

  for(int i = 0; i < points_count - 2; i++)
    t *= _ellipse_cross_test(x, y, points + 2 * i, points + 2 * (i + 1));

  return t;
}

// check if point is close to path - segment by segment
static int _ellipse_point_close_to_path(float x, float y, float as, float *points, int points_count)
{
  float as2 = as * as;

  const float lastx = points[2 * (points_count - 1)];
  const float lasty = points[2 * (points_count - 1) + 1];

  for(int i = 0; i < points_count; i++)
  {
    const float px = points[2 * i];
    const float py = points[2 * i + 1];

    const float r1 = x - lastx;
    const float r2 = y - lasty;
    const float r3 = px - lastx;
    const float r4 = py - lasty;

    const float d = r1 * r3 + r2 * r4;
    const float l = sqf(r3) + sqf(r4);
    const float p = d / l;

    float xx = 0.0f, yy = 0.0f;

    if(p < 0 || (px == lastx && py == lasty))
    {
      xx = lastx;
      yy = lasty;
    }
    else if(p > 1)
    {
      xx = px;
      yy = py;
    }
    else
    {
      xx = lastx + p * r3;
      yy = lasty + p * r4;
    }

    const float dx = x - xx;
    const float dy = y - yy;

    if(sqf(dx) + sqf(dy) < as2) return 1;
  }
  return 0;
}

static void _ellipse_get_distance(float x, float y, float as, dt_masks_form_gui_t *gui, int index,
                                  int num_points, int *inside, int *inside_border, int *near, int *inside_source, float *dist)
{
  (void)num_points; // unused arg, keep compiler from complaining

  *dist = FLT_MAX;

  if(!gui) return;

  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return;

  // we first check if we are inside the source form
  if(gpt->source_count > 10)
  {
    if(_ellipse_point_in_polygon(x, y, gpt->source + 10, gpt->source_count - 5) >= 0)
    {
      *inside_source = 1;
      *inside = 1;
      *inside_border = 0;
      *near = -1;

      // get the minial dist for center & control points
      for(int k=0; k<5; k++)
      {
        const float cx = x - gpt->source[k * 2];
        const float cy = y - gpt->source[k * 2 + 1];
        const float dd = sqf(cx) + sqf(cy);
        *dist = fminf(*dist, dd);
      }
      return;
    }
  }

  for(int k=0; k<5; k++)
  {
    const float cx = x - gpt->points[k * 2];
    const float cy = y - gpt->points[k * 2 + 1];
    const float dd = sqf(cx) + sqf(cy);
    *dist = fminf(*dist, dd);
  }

  *inside_source = 0;

  // we check if it's inside borders
  if(_ellipse_point_in_polygon(x, y, gpt->border + 10, gpt->border_count - 5) < 0)
  {
    *inside = 0;
    *inside_border = 0;
    *near = -1;
    return;
  }

  *inside = 1;
  *near = 0;
  *inside_border = 1;

  if(_ellipse_point_in_polygon(x, y, gpt->points + 10, gpt->points_count - 5) >= 0) *inside_border = 0;
  if(_ellipse_point_close_to_path(x, y, as, gpt->points + 10, gpt->points_count - 5)) *near = 1;
}

static void _ellipse_draw_shape(gboolean borders, gboolean source, cairo_t *cr, double *dashed, const float len,
                                const int selected, const float zoom_scale,
                                const float xref, const float yref, float *points, const int points_count)
{
  if(points_count <= 10) return;

  const float r = atan2f(points[3] - points[1], points[2] - points[0]);
  const float sinr = sinf(r);
  const float cosr = cosf(r);

  float x = 0.0f;
  float y = 0.0f;

  cairo_set_line_width(cr, ((borders ? 2.0 : 3.0) + selected ? 2.0 : 0.0) / (borders || source ? 2.0 : 1.0)/zoom_scale);

  dt_draw_set_color_overlay(cr, FALSE, 0.8);
  cairo_set_dash(cr, dashed, len, 0);

  _ellipse_point_transform(xref, yref, points[10], points[11], sinr, cosr, &x, &y);
  cairo_move_to(cr, x, y);
  for(int i = 6; i < points_count; i++)
  {
    _ellipse_point_transform(xref, yref, points[i * 2], points[i * 2 + 1], sinr, cosr, &x, &y);
    cairo_line_to(cr, x, y);
  }
  _ellipse_point_transform(xref, yref, points[10], points[11], sinr, cosr, &x, &y);
  cairo_line_to(cr, x, y);
  cairo_stroke_preserve(cr);

  cairo_set_line_width(cr, (source ? 0.5 : 1.0) * (selected ? 2.0 : 1.0) / zoom_scale);

  dt_draw_set_color_overlay(cr, TRUE, 0.8);
  cairo_set_dash(cr, dashed, len, 4);
  cairo_stroke(cr);
}

static float *_points_to_transform(float xx, float yy, float radius_a, float radius_b, float rotation, float wd,
                                   float ht, int *points_count)
{
  const float v1 = (rotation / 180.0f) * M_PI;
  const float v2 = (rotation - 90.0f) / 180.0f * M_PI;
  float a, b, v;

  if(radius_a >= radius_b)
  {
    a = radius_a * MIN(wd, ht);
    b = radius_b * MIN(wd, ht);
    v = v1;
  }
  else
  {
    a = radius_b * MIN(wd, ht);
    b = radius_a * MIN(wd, ht);
    v = v2;
  }

  const float sinv = sinf(v);
  const float cosv = cosf(v);

  // how many points do we need? we only take every nth point and rely on interpolation (only affecting GUI
  // anyhow)
  const int n = 10;
  const float lambda = (a - b) / (a + b);
  const int l = MAX(
      100, (int)((M_PI * (a + b)
                  * (1.0f + (3.0f * lambda * lambda) / (10.0f + sqrtf(4.0f - 3.0f * lambda * lambda)))) / n));

  // buffer allocations
  float *const restrict points = dt_alloc_align_float((size_t)2 * (l + 5));
  if(!points)
  {
    *points_count = 0;
    return 0;
  }
  *points_count = l + 5;

  // now we set the points
  const float x = points[0] = xx * wd;
  const float y = points[1] = yy * ht;

  points[2] = x + a * cosf(v);
  points[3] = y + a * sinf(v);
  points[4] = x - a * cosf(v);
  points[5] = y - a * sinf(v);

  points[6] = x + b * cosf(v - M_PI / 2.0f);
  points[7] = y + b * sinf(v - M_PI / 2.0f);
  points[8] = x - b * cosf(v - M_PI / 2.0f);
  points[9] = y - b * sinf(v - M_PI / 2.0f);


#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(l, points, x, y, a, b, cosv, sinv)  \
    schedule(static) if(l > 100) aligned(points:64)
#endif
  for(int i = 5; i < l + 5; i++)
  {
    const float alpha = (i - 5) * 2.0 * M_PI / (float)l;
    points[i * 2] = x + a * cosf(alpha) * cosv - b * sinf(alpha) * sinv;
    points[i * 2 + 1] = y + a * cosf(alpha) * sinv + b * sinf(alpha) * cosv;
  }

  return points;
}

static int _ellipse_get_points_source(dt_develop_t *dev, float xx, float yy, float xs, float ys, float radius_a,
                                      float radius_b, float rotation, float **points, int *points_count,
                                      const dt_iop_module_t *module)
{
  const float wd = dev->preview_pipe->iwidth;
  const float ht = dev->preview_pipe->iheight;

  // compute the points of the target (center and circumference of circle)
  // we get the point in RAW image reference
  *points = _points_to_transform(xx, yy, radius_a, radius_b, rotation, wd, ht, points_count);
  if(!*points) return 0;

  // we transform with all distortion that happen *before* the module
  // so we have now the TARGET points in module input reference
  if(dt_dev_distort_transform_plus(dev, dev->preview_pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_EXCL,
                                   *points, *points_count))
  {
    // now we move all the points by the shift
    // so we have now the SOURCE points in module input reference
    float pts[2] = { xs * wd, ys * ht };
    if(dt_dev_distort_transform_plus(dev, dev->preview_pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_EXCL,
                                     pts, 1))
    {
      const float dx = pts[0] - (*points)[0];
      const float dy = pts[1] - (*points)[1];
      (*points)[0] = pts[0];
      (*points)[1] = pts[1];
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(points_count, points, dx, dy)              \
    schedule(static) if(*points_count > 100) aligned(points:64)
#endif
      for(int i = 5; i < *points_count; i++)
      {
        (*points)[i * 2] += dx;
        (*points)[i * 2 + 1] += dy;
      }

      // we apply the rest of the distortions (those after the module)
      // so we have now the SOURCE points in final image reference
      if(dt_dev_distort_transform_plus(dev, dev->preview_pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_FORW_INCL,
                                       *points, *points_count))
        return 1;
    }
  }

  // if we failed, then free all and return
  dt_free_align(*points);
  *points = NULL;
  *points_count = 0;
  return 0;
}

static int _ellipse_get_points(dt_develop_t *dev, float xx, float yy, float radius_a, float radius_b,
                               float rotation, float **points, int *points_count)
{
  const float wd = dev->preview_pipe->iwidth;
  const float ht = dev->preview_pipe->iheight;

  *points = _points_to_transform(xx, yy, radius_a, radius_b, rotation, wd, ht, points_count);
  if(!*points) return 0;

  // and we transform them with all distorted modules
  if(dt_dev_distort_transform(dev, *points, *points_count)) return 1;

  // if we failed, then free all and return
  dt_free_align(*points);
  *points = NULL;
  *points_count = 0;
  return 0;
}

static int _ellipse_get_points_border(dt_develop_t *dev, struct dt_masks_form_t *form, float **points,
                                      int *points_count, float **border, int *border_count, int source,
                                      const dt_iop_module_t *module)
{
  dt_masks_point_ellipse_t *ellipse = (dt_masks_point_ellipse_t *)((form->points)->data);
  const float x = ellipse->center[0], y = ellipse->center[1];
  const float a = ellipse->radius[0], b = ellipse->radius[1];

  if(source)
  {
    float xs = form->source[0], ys = form->source[1];
    return _ellipse_get_points_source(dev, x, y, xs, ys, a, b, ellipse->rotation, points, points_count, module);
  }
  else
  {
    if(_ellipse_get_points(dev, x, y, a, b, ellipse->rotation, points, points_count))
    {
      if(border)
      {
        const int prop = ellipse->flags & DT_MASKS_ELLIPSE_PROPORTIONAL;
        return _ellipse_get_points(dev, x, y, (prop ? a * (1.0f + ellipse->border) : a + ellipse->border),
                                   (prop ? b * (1.0f + ellipse->border) : b + ellipse->border), ellipse->rotation,
                                   border, border_count);
      }
      else
        return 1;
    }
  }
  return 0;
}

static int _ellipse_events_mouse_scrolled(struct dt_iop_module_t *module, float pzx, float pzy, int up,
                                          uint32_t state, dt_masks_form_t *form, int parentid,
                                          dt_masks_form_gui_t *gui, int index)
{
  const float radius_limit = form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE) ? 0.5f : 1.0f;
  // add a preview when creating an ellipse
  if(gui->creation)
  {
    float radius_a = dt_conf_get_float(DT_MASKS_CONF(form->type, ellipse, radius_a));
    float radius_b = dt_conf_get_float(DT_MASKS_CONF(form->type, ellipse, radius_b));

    if(dt_modifier_is(state, GDK_SHIFT_MASK | GDK_CONTROL_MASK))
    {
      float rotation = dt_conf_get_float(DT_MASKS_CONF(form->type, ellipse, rotation));

      if(up)
        rotation += 10.f;
      else
        rotation -= 10.f;
      rotation = fmodf(rotation, 360.0f);

      dt_conf_set_float(DT_MASKS_CONF(form->type, ellipse, rotation), rotation);

      dt_toast_log(_("rotation: %3.f°"), rotation);
    }
    else if(dt_modifier_is(state, GDK_SHIFT_MASK))
    {
      float masks_border = dt_conf_get_float(DT_MASKS_CONF(form->type, ellipse, border));
      int flags = dt_conf_get_int(DT_MASKS_CONF(form->type, ellipse, flags));
      radius_a = dt_conf_get_float(DT_MASKS_CONF(form->type, ellipse, radius_a));
      radius_b = dt_conf_get_float(DT_MASKS_CONF(form->type, ellipse, radius_b));

      const float reference = (flags & DT_MASKS_ELLIPSE_PROPORTIONAL ? 1.0f / fmin(radius_a, radius_b) : 1.0f);
      if(!up && masks_border > 0.001f * reference)
        masks_border *= 0.97f;
      else if(up && masks_border < radius_limit * reference)
        masks_border *= 1.0f / 0.97f;
      else
        return 1;
      masks_border = CLAMP(masks_border, 0.001f * reference, reference);

      dt_conf_set_float(DT_MASKS_CONF(form->type, ellipse, border), masks_border);

      dt_toast_log(_("feather size: %3.2f%%"), (masks_border/fmaxf(radius_a, radius_b))*100.0f);
    }
    else if(dt_modifier_is(state, 0))
    {
      const float oldradius = radius_a;

      if(!up && radius_a > 0.001f)
        radius_a *= 0.97f;
      else if(up && radius_a < radius_limit)
        radius_a *= 1.0f / 0.97f;
      else
        return 1;

      radius_a = CLAMP(radius_a, 0.001f, radius_limit);

      const float factor = radius_a / oldradius;
      radius_b *= factor;

      dt_conf_set_float(DT_MASKS_CONF(form->type, ellipse, radius_a), radius_a);
      dt_conf_set_float(DT_MASKS_CONF(form->type, ellipse, radius_b), radius_b);
      dt_toast_log(_("size: %3.2f%%"), fmaxf(radius_a, radius_b)*100);
    }
    return 1;
  }

  if(gui->form_selected)
  {
    // we register the current position
    if(gui->scrollx == 0.0f && gui->scrolly == 0.0f)
    {
      gui->scrollx = pzx;
      gui->scrolly = pzy;
    }
    if(dt_modifier_is(state, GDK_CONTROL_MASK))
    {
      // we try to change the opacity
      dt_masks_form_change_opacity(form, parentid, up ? 0.05f : -0.05f);
    }
    else
    {
      dt_masks_point_ellipse_t *ellipse = (dt_masks_point_ellipse_t *)((form->points)->data);
      if(dt_modifier_is(state, GDK_SHIFT_MASK | GDK_CONTROL_MASK)
         && gui->edit_mode == DT_MASKS_EDIT_FULL)
      {
        // we try to change the rotation
        if(up)
          ellipse->rotation += 10.f;
        else
          ellipse->rotation -= 10.f;
        ellipse->rotation = fmodf(ellipse->rotation, 360.0f);

        dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
        dt_masks_gui_form_remove(form, gui, index);
        dt_masks_gui_form_create(form, gui, index, module);
        dt_conf_set_float(DT_MASKS_CONF(form->type, ellipse, rotation), ellipse->rotation);
        dt_toast_log(_("rotation: %3.f°"), ellipse->rotation);
      }
      // resize don't care where the mouse is inside a shape
      if(dt_modifier_is(state, GDK_SHIFT_MASK))
      {
        const float reference = (ellipse->flags & DT_MASKS_ELLIPSE_PROPORTIONAL ? 1.0f/fmin(ellipse->radius[0], ellipse->radius[1]) : 1.0f);
        if(!up && ellipse->border > 0.001f * reference)
          ellipse->border *= 0.97f;
        else if(up && ellipse->border < radius_limit * reference)
          ellipse->border *= 1.0f/0.97f;
        else return 1;
        ellipse->border = CLAMP(ellipse->border, 0.001f * reference, radius_limit *reference);
        dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
        dt_masks_gui_form_remove(form, gui, index);
        dt_masks_gui_form_create(form, gui, index, module);
        dt_conf_set_float(DT_MASKS_CONF(form->type, ellipse, border), ellipse->border);
        dt_toast_log(_("feather size: %3.2f%%"), ellipse->border*100.0f);
      }
      else if(gui->edit_mode == DT_MASKS_EDIT_FULL && dt_modifier_is(state, 0))
      {
        const float oldradius = ellipse->radius[0];

        if(!up && ellipse->radius[0] > 0.001f)
          ellipse->radius[0] *= 0.97f;
        else if(up && ellipse->radius[0] < radius_limit)
          ellipse->radius[0] *= 1.0f / 0.97f;
        else return 1;

        ellipse->radius[0] = CLAMP(ellipse->radius[0], 0.001f, radius_limit);

        const float factor = ellipse->radius[0] / oldradius;
        ellipse->radius[1] *= factor;

        dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
        dt_masks_gui_form_remove(form, gui, index);
        dt_masks_gui_form_create(form, gui, index, module);
        dt_conf_set_float(DT_MASKS_CONF(form->type, ellipse, radius_a), ellipse->radius[0]);
        dt_conf_set_float(DT_MASKS_CONF(form->type, ellipse, radius_b), ellipse->radius[1]);
        dt_toast_log(_("size: %3.2f%%"), fmaxf(ellipse->radius[0], ellipse->radius[1])*100);
      }
      else if(!dt_modifier_is(state, 0))
      {
        // user is holding down a modifier key, but we didn't handle that particular combination
        // say we've processed the scroll event so that the image is not zoomed instead
        return 1;
      }
      else
      {
        return 0;
      }
      dt_masks_update_image(darktable.develop);
    }
    return 1;
  }
  return 0;
}

static int _ellipse_events_button_pressed(struct dt_iop_module_t *module, float pzx, float pzy,
                                          double pressure, int which, int type, uint32_t state,
                                          dt_masks_form_t *form, int parentid, dt_masks_form_gui_t *gui,
                                          int index)
{
  if(!gui) return 0;

  if(!gui->creation)
  {
    dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
    if(!gpt) return 0;

    if(gui->form_selected && dt_modifier_is(state, GDK_SHIFT_MASK))
    {
      gui->border_toggling = TRUE;
      return 1;
    }
    else if(gui->edit_mode == DT_MASKS_EDIT_FULL)
    {
      if(gui->source_selected)
      {
        gui->dx = gpt->source[0] - gui->posx;
        gui->dy = gpt->source[1] - gui->posy;

        gui->source_dragging = TRUE;
        return 1;
      }

      gui->dx = gpt->points[0] - gui->posx;
      gui->dy = gpt->points[1] - gui->posy;

      if(gui->form_selected && dt_modifier_is(state, GDK_CONTROL_MASK))
      {
        gui->form_rotating = TRUE;
        return 1;
      }
      else if(gui->point_selected >= 1)
      {
        gui->point_dragging = gui->point_selected;
        return 1;
      }
      else if(gui->point_border_selected >= 1)
      {
        gui->point_border_dragging = gui->point_border_selected;
        return 1;
      }
      else if(gui->form_selected)
      {
        gui->form_dragging = TRUE;
        return 1;
      }
    }
  }
  else if(which == 3)
  {
    gui->creation_continuous = FALSE;
    gui->creation_continuous_module = NULL;
    dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
    dt_masks_iop_update(module);
    dt_control_queue_redraw_center();
    return 1;
  }
  else if(which == 1
          && (dt_modifier_is(state, GDK_CONTROL_MASK | GDK_SHIFT_MASK) || dt_modifier_is(state, GDK_SHIFT_MASK)))
  {
    // set some absolute or relative position for the source of the clone mask
    if(form->type & DT_MASKS_CLONE) dt_masks_set_source_pos_initial_state(gui, state, pzx, pzy);

    return 1;
  }
  else
  {
    dt_iop_module_t *crea_module = gui->creation_module;
    // we create the ellipse
    dt_masks_point_ellipse_t *ellipse
        = (dt_masks_point_ellipse_t *)(malloc(sizeof(dt_masks_point_ellipse_t)));

    // we change the center value
    const float wd = darktable.develop->preview_pipe->backbuf_width;
    const float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = { pzx * wd, pzy * ht };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);
    ellipse->center[0] = pts[0] / darktable.develop->preview_pipe->iwidth;
    ellipse->center[1] = pts[1] / darktable.develop->preview_pipe->iheight;

    if(form->type & DT_MASKS_CLONE)
    {
      dt_masks_set_source_pos_initial_value(gui, DT_MASKS_ELLIPSE, form, pzx, pzy);
    }
    else
    {
      // not used for regular masks
      form->source[0] = form->source[1] = 0.0f;
    }
    ellipse->radius[0] = dt_conf_get_float(DT_MASKS_CONF(form->type, ellipse, radius_a));
    ellipse->radius[1] = dt_conf_get_float(DT_MASKS_CONF(form->type, ellipse, radius_b));
    ellipse->border = dt_conf_get_float(DT_MASKS_CONF(form->type, ellipse, border));
    ellipse->rotation = dt_conf_get_float(DT_MASKS_CONF(form->type, ellipse, rotation));
    ellipse->flags = dt_conf_get_int(DT_MASKS_CONF(form->type, ellipse, flags));
    form->points = g_list_append(form->points, ellipse);
    dt_masks_gui_form_save_creation(darktable.develop, crea_module, form, gui);

    if(crea_module)
    {
      // we save the move
      dt_dev_add_history_item(darktable.develop, crea_module, TRUE);
      // and we switch in edit mode to show all the forms
      // spots and retouch have their own handling of creation_continuous
      if(gui->creation_continuous && ( strcmp(crea_module->so->op, "spots") == 0 || strcmp(crea_module->so->op, "retouch") == 0))
        dt_masks_set_edit_mode_single_form(crea_module, form->formid, DT_MASKS_EDIT_FULL);
      else if(!gui->creation_continuous)
        dt_masks_set_edit_mode(crea_module, DT_MASKS_EDIT_FULL);
      dt_masks_iop_update(crea_module);
      dt_dev_masks_selection_change(darktable.develop, crea_module, form->formid, TRUE);
      gui->creation_module = NULL;
    }
    else
    {
      // we select the new form
      dt_dev_masks_selection_change(darktable.develop, NULL, form->formid, TRUE);
    }

    // if we draw a clone ellipse, we start now the source dragging
    if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
    {
      dt_masks_form_t *grp = darktable.develop->form_visible;
      if(!grp || !(grp->type & DT_MASKS_GROUP)) return 1;
      int pos3 = 0, pos2 = -1;
      for(GList *fs = grp->points; fs; fs = g_list_next(fs))
      {
        dt_masks_point_group_t *pt = (dt_masks_point_group_t *)fs->data;
        if(pt->formid == form->formid)
        {
          pos2 = pos3;
          break;
        }
        pos3++;
      }
      if(pos2 < 0) return 1;
      dt_masks_form_gui_t *gui2 = darktable.develop->form_gui;
      if(!gui2) return 1;
      if(form->type & DT_MASKS_CLONE)
        gui2->source_dragging = TRUE;
      else
        gui2->form_dragging = TRUE;
      gui2->group_edited = gui2->group_selected = pos2;
      gui2->posx = pzx * darktable.develop->preview_pipe->backbuf_width;
      gui2->posy = pzy * darktable.develop->preview_pipe->backbuf_height;
      gui2->dx = 0.0;
      gui2->dy = 0.0;
      gui2->scrollx = pzx;
      gui2->scrolly = pzy;
      gui2->form_selected = TRUE; // we also want to be selected after button released

      dt_masks_select_form(module, dt_masks_get_from_id(darktable.develop, form->formid));
    }
    //spot and retouch manage creation_continuous in their own way
    if(crea_module && gui->creation_continuous && strcmp(crea_module->so->op, "spots") != 0 && strcmp(crea_module->so->op, "retouch") != 0)
    {
      dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)crea_module->blend_data;
      for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
        if(bd->masks_type[n] == form->type)
          gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_shapes[n]), TRUE);

      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), FALSE);
      dt_masks_form_t *newform = dt_masks_create(form->type);
      dt_masks_change_form_gui(newform);
      darktable.develop->form_gui->creation = TRUE;
      darktable.develop->form_gui->creation_module = crea_module;
      darktable.develop->form_gui->creation_continuous = TRUE;
      darktable.develop->form_gui->creation_continuous_module = crea_module;
    }
    return 1;
  }
  return 0;
}

static int _ellipse_events_button_released(struct dt_iop_module_t *module, float pzx, float pzy, int which,
                                           uint32_t state, dt_masks_form_t *form, int parentid,
                                           dt_masks_form_gui_t *gui, int index)
{
  if(which == 3 && parentid > 0 && gui->edit_mode == DT_MASKS_EDIT_FULL)
  {
    // we hide the form
    if(!(darktable.develop->form_visible->type & DT_MASKS_GROUP))
      dt_masks_change_form_gui(NULL);
    else if(g_list_shorter_than(darktable.develop->form_visible->points, 2))
      dt_masks_change_form_gui(NULL);
    else
    {
      dt_masks_clear_form_gui(darktable.develop);
      for(GList *forms = darktable.develop->form_visible->points; forms; forms = g_list_next(forms))
      {
        dt_masks_point_group_t *gpt = (dt_masks_point_group_t *)forms->data;
        if(gpt->formid == form->formid)
        {
          darktable.develop->form_visible->points
              = g_list_remove(darktable.develop->form_visible->points, gpt);
          free(gpt);
          break;
        }
      }
      gui->edit_mode = DT_MASKS_EDIT_FULL;
    }

    // we remove the shape
    dt_masks_form_remove(module, dt_masks_get_from_id(darktable.develop, parentid), form);
    return 1;
  }
  if(gui->form_dragging)
  {
    // we get the ellipse
    dt_masks_point_ellipse_t *ellipse = (dt_masks_point_ellipse_t *)((form->points)->data);

    // we end the form dragging
    gui->form_dragging = FALSE;

    // we change the center value
    const float wd = darktable.develop->preview_pipe->backbuf_width;
    const float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = { pzx * wd + gui->dx, pzy * ht + gui->dy };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);
    ellipse->center[0] = pts[0] / darktable.develop->preview_pipe->iwidth;
    ellipse->center[1] = pts[1] / darktable.develop->preview_pipe->iheight;
    dt_dev_add_masks_history_item(darktable.develop, module, TRUE);

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index, module);

    // we save the move
    dt_masks_update_image(darktable.develop);

    if(gui->creation_continuous)
    {
      dt_masks_form_t *form_new = dt_masks_create(form->type);
      dt_masks_change_form_gui(form_new);

      darktable.develop->form_gui->creation = TRUE;
      darktable.develop->form_gui->creation_module = gui->creation_continuous_module;
    }
    return 1;
  }
  else if(gui->border_toggling)
  {
    // we get the ellipse
    dt_masks_point_ellipse_t *ellipse = (dt_masks_point_ellipse_t *)((form->points)->data);

    // we end the border toggling
    gui->border_toggling = FALSE;

    // toggle feathering type of border and adjust border radius accordingly
    if(ellipse->flags & DT_MASKS_ELLIPSE_PROPORTIONAL)
    {
      const float min_radius = fmin(ellipse->radius[0], ellipse->radius[1]);
      ellipse->border = ellipse->border * min_radius;
      ellipse->border = CLAMP(ellipse->border, 0.001f, 1.0f);

      ellipse->flags &= ~DT_MASKS_ELLIPSE_PROPORTIONAL;
    }
    else
    {
      const float min_radius = fmin(ellipse->radius[0], ellipse->radius[1]);
      ellipse->border = ellipse->border/min_radius;
      ellipse->border = CLAMP(ellipse->border, 0.001f/min_radius, 1.0f/min_radius);

      ellipse->flags |= DT_MASKS_ELLIPSE_PROPORTIONAL;
    }

    dt_conf_set_int(DT_MASKS_CONF(form->type, ellipse, flags), ellipse->flags);
    dt_conf_set_float(DT_MASKS_CONF(form->type, ellipse, border), ellipse->border);

    dt_dev_add_masks_history_item(darktable.develop, module, TRUE);

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index, module);

    // we save the new parameters
    dt_masks_update_image(darktable.develop);

    return 1;
  }
  else if(gui->form_rotating && gui->edit_mode == DT_MASKS_EDIT_FULL)
  {
    // we get the ellipse
    dt_masks_point_ellipse_t *ellipse = (dt_masks_point_ellipse_t *)((form->points)->data);

    // we end the form rotating
    gui->form_rotating = FALSE;

    const float wd = darktable.develop->preview_pipe->backbuf_width;
    const float ht = darktable.develop->preview_pipe->backbuf_height;
    const float x = pzx * wd;
    const float y = pzy * ht;

    // we need the reference point
    dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
    if(!gpt) return 0;

    // ellipse center
    const float xref = gpt->points[0];
    const float yref = gpt->points[1];

    const float pts[8] = { xref, yref, x , y, 0, 0, gui->dx, gui->dy };

    const float dv = atan2f(pts[3] - pts[1], pts[2] - pts[0]) - atan2f(-(pts[7] - pts[5]), -(pts[6] - pts[4]));

    float pts2[8] = { xref, yref, x , y, xref+10.0f, yref, xref, yref+10.0f };
    dt_dev_distort_backtransform(darktable.develop, pts2, 4);

    float check_angle = atan2f(pts2[7] - pts2[1], pts2[6] - pts2[0]) - atan2f(pts2[5] - pts2[1], pts2[4] - pts2[0]);
    // Normalize to the range -180 to 180 degrees
    check_angle = atan2f(sinf(check_angle), cosf(check_angle));
    if(check_angle < 0)
      ellipse->rotation -= dv / M_PI * 180.0f;
    else
      ellipse->rotation += dv / M_PI * 180.0f;

    dt_conf_set_float(DT_MASKS_CONF(form->type, ellipse, rotation), ellipse->rotation);

    dt_dev_add_masks_history_item(darktable.develop, module, TRUE);

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index, module);

    // we save the rotation
    dt_masks_update_image(darktable.develop);

    return 1;
  }
  else if(gui->point_dragging >= 1 && gui->edit_mode == DT_MASKS_EDIT_FULL)
  {
    // we get the ellipse
    dt_masks_point_ellipse_t *ellipse = (dt_masks_point_ellipse_t *)((form->points)->data);

    const int k = gui->point_dragging;

    // we end the point dragging
    gui->point_dragging = -1;

    // we need the reference points
    dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
    if(!gpt) return 0;

    const float xref = gpt->points[0];
    const float yref = gpt->points[1];
    const float rx = gpt->points[k * 2] - xref;
    const float ry = gpt->points[k * 2 + 1] - yref;
    const float deltax = gui->posx + gui->dx - xref;
    const float deltay = gui->posy + gui->dy - yref;

    const float r = sqrtf(rx * rx + ry * ry);
    const float d = (rx * deltax + ry * deltay) / r;
    const float s = fmaxf(r > 0.0f ? (r + d) / r : 0.0f, 0.0f);

    // make sure we adjust the right radius: anchor points and 1 and 2 correspond to the ellipse's longer axis
    if(((k == 1 || k == 2) && ellipse->radius[0] > ellipse->radius[1])
       || ((k == 3 || k == 4) && ellipse->radius[0] <= ellipse->radius[1]))
    {
      ellipse->radius[0] = MAX(0.002f, ellipse->radius[0] * s);
      dt_conf_set_float(DT_MASKS_CONF(form->type, ellipse, radius_a), ellipse->radius[0]);
    }
    else
    {
      ellipse->radius[1] = MAX(0.002f, ellipse->radius[1] * s);
      dt_conf_set_float(DT_MASKS_CONF(form->type, ellipse, radius_b), ellipse->radius[1]);
    }

    dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index, module);

    // we save the updated shape
    dt_masks_update_image(darktable.develop);

    return 1;
  }
  else if(gui->point_border_dragging >= 1 && gui->edit_mode == DT_MASKS_EDIT_FULL)
  {
    // we get the ellipse
    dt_masks_point_ellipse_t *ellipse = (dt_masks_point_ellipse_t *)((form->points)->data);

    const int k = gui->point_border_dragging;

    // we end the point dragging
    gui->point_border_dragging = -1;

    // we need the reference points
    dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
    if(!gpt) return 0;

    const float xref = gpt->points[0];
    const float yref = gpt->points[1];
    const float rx = gpt->border[k * 2] - xref;
    const float ry = gpt->border[k * 2 + 1] - yref;
    const float deltax = gui->posx + gui->dx - xref;
    const float deltay = gui->posy + gui->dy - yref;

    const float r = sqrtf(rx * rx + ry * ry);
    const float d = (rx * deltax + ry * deltay) / r;
    const float s = fmaxf(r > 0.0f ? (r + d) / r : 0.0f, 0.0f);

    const float radius_limit = form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE) ? 0.5f : 1.0f;
    const int prop = ellipse->flags & DT_MASKS_ELLIPSE_PROPORTIONAL;
    const float reference = (prop ? 1.0f/fmin(ellipse->radius[0], ellipse->radius[1]) : 1.0f);

    ellipse->border = CLAMP(prop ? (1.0f + ellipse->border) * s - 1.0f
                                 : ((gui->point_border_dragging >= 3) ^ (ellipse->radius[0] > ellipse->radius[1]))
                                 ? (ellipse->radius[0] + ellipse->border) * s - ellipse->radius[0]
                                 : (ellipse->radius[1] + ellipse->border) * s - ellipse->radius[1],
                            0.001f * reference, radius_limit *reference);

    dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index, module);

    // we save the updated shape
    dt_masks_update_image(darktable.develop);

    return 1;
  }
  else if(gui->source_dragging)
  {
    // we end the form dragging
    gui->source_dragging = FALSE;
    if(gui->scrollx != 0.0 || gui->scrolly != 0.0)
    {
      // if there's no dragging the source is calculated in _ellipse_events_button_pressed()
    }
    else
    {
      // we change the center value
      const float wd = darktable.develop->preview_pipe->backbuf_width;
      const float ht = darktable.develop->preview_pipe->backbuf_height;
      float pts[2] = { pzx * wd + gui->dx, pzy * ht + gui->dy };

      dt_dev_distort_backtransform(darktable.develop, pts, 1);

      form->source[0] = pts[0] / darktable.develop->preview_pipe->iwidth;
      form->source[1] = pts[1] / darktable.develop->preview_pipe->iheight;
    }
    dt_dev_add_masks_history_item(darktable.develop, module, TRUE);

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index, module);

    // we save the move
    dt_masks_update_image(darktable.develop);

    if(gui->creation_continuous)
    {
      dt_masks_form_t *form_new = dt_masks_create(form->type);
      dt_masks_change_form_gui(form_new);

      darktable.develop->form_gui->creation = TRUE;
      darktable.develop->form_gui->creation_module = gui->creation_continuous_module;
    }

    // and select the source as default, if the mouse is not moved we are inside the
    // source and so want to move the source.
    gui->form_selected = TRUE;
    gui->source_selected = TRUE;
    gui->border_selected = FALSE;

    return 1;
  }
  return 0;
}

static int _ellipse_events_mouse_moved(struct dt_iop_module_t *module, float pzx, float pzy,
                                       double pressure, int which, dt_masks_form_t *form, int parentid,
                                       dt_masks_form_gui_t *gui, int index)
{
  if(gui->form_dragging || gui->source_dragging)
  {
    const float wd = darktable.develop->preview_pipe->backbuf_width;
    const float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = { pzx * wd + gui->dx, pzy * ht + gui->dy };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);
    if(gui->form_dragging)
    {
      dt_masks_point_ellipse_t *ellipse = (dt_masks_point_ellipse_t *)((form->points)->data);
      ellipse->center[0] = pts[0] / darktable.develop->preview_pipe->iwidth;
      ellipse->center[1] = pts[1] / darktable.develop->preview_pipe->iheight;
    }
    else
    {
      form->source[0] = pts[0] / darktable.develop->preview_pipe->iwidth;
      form->source[1] = pts[1] / darktable.develop->preview_pipe->iheight;
    }

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index, module);
    dt_control_queue_redraw_center();
    return 1;
  }
  else if(gui->point_dragging >= 1)
  {
    dt_masks_point_ellipse_t *ellipse = (dt_masks_point_ellipse_t *)((form->points)->data);
    const int k = gui->point_dragging;

    // we need the reference points
    dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
    if(!gpt) return 0;

    const float xref = gpt->points[0];
    const float yref = gpt->points[1];
    const float rx = gpt->points[k * 2] - xref;
    const float ry = gpt->points[k * 2 + 1] - yref;
    const float deltax = gui->posx + gui->dx - xref;
    const float deltay = gui->posy + gui->dy - yref;

    // we remap dx, dy to the right values, as it will be used in next movements
    gui->dx = xref - gui->posx;
    gui->dy = yref - gui->posy;

    const float r = sqrtf(rx * rx + ry * ry);
    const float d = (rx * deltax + ry * deltay) / r;
    const float s = fmaxf(r > 0.0f ? (r + d) / r : 0.0f, 0.0f);

    // make sure we adjust the right radius: anchor points and 1 and 2 correspond to the ellipse's longer axis
    const gboolean dir = (ellipse->radius[0] > ellipse->radius[1]);
    if(((k == 1 || k == 2) && ellipse->radius[0] > ellipse->radius[1])
       || ((k == 3 || k == 4) && ellipse->radius[0] <= ellipse->radius[1]))
    {
      ellipse->radius[0] = MAX(0.002f, ellipse->radius[0] * s);
      dt_conf_set_float(DT_MASKS_CONF(form->type, ellipse, radius_a), ellipse->radius[0]);
    }
    else
    {
      ellipse->radius[1] = MAX(0.002f, ellipse->radius[1] * s);
      dt_conf_set_float(DT_MASKS_CONF(form->type, ellipse, radius_b), ellipse->radius[1]);
    }

    // as point 1 an 2 always correspond to the longer axis, point number may change when recreating the form
    // this happen if radius values order change
    if(dir != (ellipse->radius[0] > ellipse->radius[1]))
    {
      if(dir)
      {
        if(k == 1)
          gui->point_dragging = 4;
        else if(k == 2)
          gui->point_dragging = 3;
        else if(k == 3)
          gui->point_dragging = 1;
        else if(k == 4)
          gui->point_dragging = 2;
      }
      else
      {
        if(k == 1)
          gui->point_dragging = 3;
        else if(k == 2)
          gui->point_dragging = 4;
        else if(k == 3)
          gui->point_dragging = 2;
        else if(k == 4)
          gui->point_dragging = 1;
      }
    }

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index, module);
    dt_control_queue_redraw_center();
    return 1;
  }
  else if(gui->point_border_dragging >= 1)
  {
    dt_masks_point_ellipse_t *ellipse = (dt_masks_point_ellipse_t *)((form->points)->data);
    const int k = gui->point_border_dragging;

    // we need the reference points
    dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
    if(!gpt) return 0;

    const float xref = gpt->points[0];
    const float yref = gpt->points[1];
    const float rx = gpt->border[k * 2] - xref;
    const float ry = gpt->border[k * 2 + 1] - yref;
    const float deltax = gui->posx + gui->dx - xref;
    const float deltay = gui->posy + gui->dy - yref;

    // we remap dx, dy to the right values, as it will be used in next movements
    gui->dx = xref - gui->posx;
    gui->dy = yref - gui->posy;

    const float r = sqrtf(rx * rx + ry * ry);
    const float d = (rx * deltax + ry * deltay) / r;
    const float s = fmaxf(r > 0.0f ? (r + d) / r : 0.0f, 0.0f);

    const float radius_limit = form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE) ? 0.5f : 1.0f;
    const int prop = ellipse->flags & DT_MASKS_ELLIPSE_PROPORTIONAL;
    const float reference = (prop ? 1.0f/fmin(ellipse->radius[0], ellipse->radius[1]) : 1.0f);

    ellipse->border = CLAMP(prop ? (1.0f + ellipse->border) * s - 1.0f
                                 : ((gui->point_border_dragging >= 3) ^ (ellipse->radius[0] > ellipse->radius[1]))
                                 ? (ellipse->radius[0] + ellipse->border) * s - ellipse->radius[0]
                                 : (ellipse->radius[1] + ellipse->border) * s - ellipse->radius[1],
                            0.001f * reference, radius_limit *reference);

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index, module);
    dt_control_queue_redraw_center();
    return 1;
  }
  else if(gui->form_rotating)
  {
    dt_masks_point_ellipse_t *ellipse = (dt_masks_point_ellipse_t *)((form->points)->data);

    const float wd = darktable.develop->preview_pipe->backbuf_width;
    const float ht = darktable.develop->preview_pipe->backbuf_height;
    const float x = pzx * wd;
    const float y = pzy * ht;

    // we need the reference point
    dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
    if(!gpt) return 0;

    // ellipse center
    const float xref = gpt->points[0];
    const float yref = gpt->points[1];

    const float pts[8] = { xref, yref, x, y, 0, 0, gui->dx, gui->dy };

    const float dv = atan2f(pts[3] - pts[1], pts[2] - pts[0]) - atan2f(-(pts[7] - pts[5]), -(pts[6] - pts[4]));

    float pts2[8] = { xref, yref, x, y, xref + 10.0f, yref, xref, yref + 10.0f };
    dt_dev_distort_backtransform(darktable.develop, pts2, 4);

    float check_angle = atan2f(pts2[7] - pts2[1], pts2[6] - pts2[0]) - atan2f(pts2[5] - pts2[1], pts2[4] - pts2[0]);
    // Normalize to the range -180 to 180 degrees
    check_angle = atan2f(sinf(check_angle), cosf(check_angle));
    if(check_angle < 0)
      ellipse->rotation -= dv / M_PI * 180.0f;
    else
      ellipse->rotation += dv / M_PI * 180.0f;

    dt_conf_set_float(DT_MASKS_CONF(form->type, ellipse, rotation), ellipse->rotation);

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index, module);

    // we remap dx, dy to the right values, as it will be used in next movements
    gui->dx = xref - gui->posx;
    gui->dy = yref - gui->posy;

    dt_control_queue_redraw_center();
    return 1;
  }
  else if(!gui->creation)
  {
    const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
    const int closeup = dt_control_get_dev_closeup();
    const float zoom_scale = dt_dev_get_zoom_scale(darktable.develop, zoom, 1<<closeup, 1);
    const float as = DT_PIXEL_APPLY_DPI(5) / zoom_scale;  // transformed to backbuf dimensions
    const float x = pzx * darktable.develop->preview_pipe->backbuf_width;
    const float y = pzy * darktable.develop->preview_pipe->backbuf_height;

    int in = 0, inb = 0, near = 0, ins = 0; // FIXME gcc7 false-positive
    float dist = 0.0f;
    _ellipse_get_distance(x, y, as, gui, index, 0, &in, &inb, &near, &ins, &dist);
    if(ins)
    {
      gui->form_selected = TRUE;
      gui->source_selected = TRUE;
      gui->border_selected = FALSE;
    }
    else if(inb)
    {
      gui->form_selected = TRUE;
      gui->border_selected = TRUE;
      gui->source_selected = FALSE;
    }
    else if(in)
    {
      gui->form_selected = TRUE;
      gui->border_selected = FALSE;
      gui->source_selected = FALSE;
    }
    else
    {
      gui->form_selected = FALSE;
      gui->border_selected = FALSE;
      gui->source_selected = FALSE;
    }

    // see if we are close to one of the anchor points
    gui->point_selected = -1;
    gui->point_border_selected = -1;
    if(gui->form_selected)
    {
      dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
      for(int i = 1; i < 5; i++)
      {
        // prefer border points over shape itself in case of near overlap for ease of pickup
        if(x - gpt->border[i * 2] > -as && x - gpt->border[i * 2] < as && y - gpt->border[i * 2 + 1] > -as
           && y - gpt->border[i * 2 + 1] < as)
        {
          gui->point_border_selected = i;
          break;
        }
        if(x - gpt->points[i * 2] > -as && x - gpt->points[i * 2] < as && y - gpt->points[i * 2 + 1] > -as
           && y - gpt->points[i * 2 + 1] < as)
        {
          gui->point_selected = i;
          break;
        }
      }
    }

    dt_control_queue_redraw_center();
    if(!gui->form_selected && !gui->border_selected) return 0;
    if(gui->edit_mode != DT_MASKS_EDIT_FULL) return 0;
    return 1;
  }
  // add a preview when creating an ellipse
  else if(gui->creation)
  {
    dt_control_queue_redraw_center();
    return 1;
  }

  return 0;
}

static void _ellipse_events_post_expose(cairo_t *cr, float zoom_scale, dt_masks_form_gui_t *gui, int index,
                                        int num_points)
{
  (void)num_points; //unused arg, keep compiler from complaining
  double dashed[] = { 4.0, 4.0 };
  dashed[0] /= zoom_scale;
  dashed[1] /= zoom_scale;
  const int len = sizeof(dashed) / sizeof(dashed[0]);
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);

  float xref = 0.0f, yref = 0.0f;
  float xrefs = 0.0f, yrefs = 0.0f;

  // add a preview when creating an ellipse
  // in creation mode
  if(gui->creation)
  {
    if(gui->guipoints_count == 0)
    {
      dt_masks_form_t *form = darktable.develop->form_visible;
      if(!form) return;

      float x = 0.0f, y = 0.0f;

      float masks_border = dt_conf_get_float(DT_MASKS_CONF(form->type, ellipse, border));
      int flags = dt_conf_get_int(DT_MASKS_CONF(form->type, ellipse, flags));
      float radius_a = dt_conf_get_float(DT_MASKS_CONF(form->type, ellipse, radius_a));
      float radius_b = dt_conf_get_float(DT_MASKS_CONF(form->type, ellipse, radius_b));
      float rotation = dt_conf_get_float(DT_MASKS_CONF(form->type, ellipse, rotation));

      float pzx = gui->posx;
      float pzy = gui->posy;

      if((pzx == -1.f && pzy == -1.f) || gui->mouse_leaved_center)
      {
        const float zoom_x = dt_control_get_dev_zoom_x();
        const float zoom_y = dt_control_get_dev_zoom_y();
        pzx = (.5f + zoom_x) * darktable.develop->preview_pipe->backbuf_width;
        pzy = (.5f + zoom_y) * darktable.develop->preview_pipe->backbuf_height;
      }

      float pts[2] = { pzx, pzy };
      dt_dev_distort_backtransform(darktable.develop, pts, 1);
      x = pts[0] / darktable.develop->preview_pipe->iwidth;
      y = pts[1] / darktable.develop->preview_pipe->iheight;

      float *points = NULL;
      int points_count = 0;
      float *border = NULL;
      int border_count = 0;

      int draw = 0;

      draw = _ellipse_get_points(darktable.develop, x, y, radius_a, radius_b, rotation, &points, &points_count);
      if(draw && masks_border > 0.f)
      {
        draw = _ellipse_get_points(
            darktable.develop, x, y,
            (flags & DT_MASKS_ELLIPSE_PROPORTIONAL ? radius_a * (1.0f + masks_border) : radius_a + masks_border),
            (flags & DT_MASKS_ELLIPSE_PROPORTIONAL ? radius_b * (1.0f + masks_border) : radius_b + masks_border),
            rotation, &border, &border_count);
      }

      if(draw && points_count >= 2)
      {
        xref = points[0];
        yref = points[1];

        _ellipse_draw_shape(FALSE, FALSE, cr, dashed, len, FALSE, zoom_scale, xref, yref, points, points_count);
      }
      if(draw && border_count >= 2)
      {
        xref = border[0];
        yref = border[1];

        _ellipse_draw_shape(TRUE, FALSE, cr, dashed, len, FALSE, zoom_scale, xref, yref, border, border_count);
      }

      // draw a cross where the source will be created
      if(form->type & DT_MASKS_CLONE)
      {
        x = 0.0f;
        y = 0.0f;
        dt_masks_calculate_source_pos_value(gui, DT_MASKS_ELLIPSE, pzx, pzy, pzx, pzy, &x, &y, FALSE);
        dt_masks_draw_clone_source_pos(cr, zoom_scale, x, y);
      }

      if(points) dt_free_align(points);
      if(border) dt_free_align(border);
    }
    return;
  } // gui->creation

  if(!gpt) return;

  const float r = atan2f(gpt->points[3] - gpt->points[1], gpt->points[2] - gpt->points[0]);
  const float sinr = sinf(r);
  const float cosr = cosf(r);

  xref = gpt->points[0];
  yref = gpt->points[1];

  if(gpt->source_count > 10)
  {
    xrefs = gpt->source[0];
    yrefs = gpt->source[1];
  }

  // draw shape
  const gboolean selected = (gui->group_selected == index) && (gui->form_selected || gui->form_dragging);
  _ellipse_draw_shape(FALSE, FALSE, cr, dashed, 0, selected, zoom_scale, xref, yref, gpt->points, gpt->points_count);

  // draw border
  if(gui->show_all_feathers || gui->group_selected == index)
  {
    _ellipse_draw_shape(TRUE, FALSE, cr, dashed, len, gui->border_selected, zoom_scale, xref, yref, gpt->border, gpt->border_count);

    // draw anchor points
    for(int i = 1; i < 5; i++)
    {
      float x, y;
      _ellipse_point_transform(xref, yref, gpt->points[i * 2], gpt->points[i * 2 + 1], sinr, cosr, &x, &y);
      dt_masks_draw_anchor(cr, i == gui->point_dragging || i == gui->point_selected, zoom_scale, x, y);
      _ellipse_point_transform(xref, yref, gpt->border[i * 2], gpt->border[i * 2 + 1], sinr, cosr, &x, &y);
      dt_masks_draw_anchor(cr, i == gui->point_border_dragging || i == gui->point_border_selected, zoom_scale, x, y);
    }
  }

  // draw the source if any
  if(gpt->source_count > 10)
  {
    const float pr_d = darktable.develop->preview_downsampling;
    // compute the dest inner ellipse intersection with the line from source center to dest center.
    const float cdx = gpt->source[0] - gpt->points[0];
    const float cdy = gpt->source[1] - gpt->points[1];

    // we don't draw the line if source==point
    if(cdx != 0.0 && cdy != 0.0)
    {
      cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
      float cangle = atanf(cdx / cdy);

      if(cdy > 0)
        cangle = (M_PI / 2) - cangle;
      else
        cangle = -(M_PI / 2) - cangle;

      // compute raidus a & radius b. at this stage this must be computed from the list
      // of transformed point for drawing the ellipse.

      const float bot_x = gpt->points[2];
      const float bot_y = gpt->points[3];
      const float rgt_x = gpt->points[6];
      const float rgt_y = gpt->points[7];
      const float cnt_x = gpt->points[0];
      const float cnt_y = gpt->points[1];

      const float adx = cnt_x - bot_x;
      const float ady = cnt_y - bot_y;
      const float a = sqrtf(adx * adx + ady * ady);

      const float bdx = cnt_x - rgt_x;
      const float bdy = cnt_y - rgt_y;
      const float b = sqrtf(bdx * bdx + bdy * bdy);

      // takes the biggest radius, should always been a as the points are arranged
      const float radius = MAX(a, b);

      // the top/left/bottom/right controls of the ellipse are not always at the
      // same place in g->points[], it depends on the rotation of the ellipse which
      // is not recorded anywhere. Let's use a stupid search to find the closest
      // point on the border where to attach the arrow.

      const float cosc = cosf(cangle);
      const float sinc = sinf(cangle);
      const float step = radius / 259.f;

      float dist = FLT_MAX;
      float arrowx = 0.0f;
      float arrowy = 0.0f;

      for(int k=1; k<gpt->source_count; k+=2)
      {
        const float px = gpt->points[k*2];
        const float py = gpt->points[k*2 + 1];

        float rr = 0.01f;
        while(rr < radius)
        {
          const float epx = cnt_x + rr * cosc;
          const float epy = cnt_y + rr * sinc;
          const float edist = sqf(epx - px) + sqf(epy - py);

          if(edist < dist)
          {
            dist = edist;
            arrowx = cnt_x + (rr + 1.11) * cosc;
            arrowy = cnt_y + (rr + 1.11) * sinc;
          }
          rr += step;
        }
      }

      cairo_move_to(cr, gpt->source[0], gpt->source[1]); // source center
      cairo_line_to(cr, arrowx, arrowy);                 // dest border
      // then draw to line for the arrow itself
      const float arrow_scale = 6.0 * pr_d;

      cairo_move_to(cr, arrowx + arrow_scale * cosf(cangle + (0.4)),
                    arrowy + arrow_scale * sinf(cangle + (0.4)));
      cairo_line_to(cr, arrowx, arrowy);
      cairo_line_to(cr, arrowx + arrow_scale * cosf(cangle - (0.4)),
                    arrowy + arrow_scale * sinf(cangle - (0.4)));

      cairo_set_dash(cr, dashed, 0, 0);
      if((gui->group_selected == index) && (gui->form_selected || gui->form_dragging))
        cairo_set_line_width(cr, 2.5 / zoom_scale);
      else
        cairo_set_line_width(cr, 1.5 / zoom_scale);
      dt_draw_set_color_overlay(cr, FALSE, 0.8);
      cairo_stroke_preserve(cr);
      if((gui->group_selected == index) && (gui->form_selected || gui->form_dragging))
        cairo_set_line_width(cr, 1.0 / zoom_scale);
      else
        cairo_set_line_width(cr, 0.5 / zoom_scale);
      dt_draw_set_color_overlay(cr, TRUE, 0.8);
      cairo_stroke(cr);
    }

    // we draw the source
    _ellipse_draw_shape(FALSE, TRUE, cr, dashed, 0, selected, zoom_scale, xrefs, yrefs, gpt->source, gpt->source_count);
   }
}

static void _bounding_box(const float *const points, int num_points, int *width, int *height, int *posx, int *posy)
{
  // search for min/max X and Y coordinates
  float xmin = FLT_MAX, xmax = FLT_MIN, ymin = FLT_MAX, ymax = FLT_MIN;
  for(int i = 1; i < num_points; i++) // skip point[0], which is circle's center
  {
    xmin = fminf(points[i * 2], xmin);
    xmax = fmaxf(points[i * 2], xmax);
    ymin = fminf(points[i * 2 + 1], ymin);
    ymax = fmaxf(points[i * 2 + 1], ymax);
  }
  // set the min/max values we found
  *posx = xmin;
  *posy = ymin;
  *width = (xmax - xmin);
  *height = (ymax - ymin);
}

static void _fill_mask(const size_t numpoints, float *const bufptr, const float *const points,
                       const float *const center, const float a, const float b, const float ta, const float tb,
                       const float alpha, const size_t out_scale)
{
  const float a2 = a * a;
  const float b2 = b * b;
  const float ta2 = ta * ta;
  const float tb2 = tb * tb;
  const float cos_alpha = cosf(alpha);
  const float sin_alpha = sinf(alpha);

  // Determine the strength of the mask for each of the distorted points.  If inside the border of the ellipse,
  // the strength is always 1.0; if outside the falloff region, it is 0.0, and in between it falls off quadratically.
  // To compute this, we need to do the equivalent of projecting the vector from the center of the ellipse to the
  // given point until it intersect the ellipse and the outer edge of the falloff, respectively.  The ellipse can
  // be rotated, but we can compensate for that by applying a rotation matrix for the same rotation in the opposite
  // direction before projecting the vector.
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(numpoints, bufptr, points, center, alpha, a2, b2, ta2, tb2, cos_alpha, sin_alpha, out_scale) \
  schedule(static)
#else
#pragma omp parallel for shared(points)
#endif
#endif
  for(size_t i = 0; i < numpoints; i++)
    {
      const float x = points[2 * i] - center[0];
      const float y = points[2 * i + 1] - center[1];
      // find the square of the distance from the center
      const float l2 = x * x + y * y;
      const float l = sqrtf(l2);
      // normalize the point's coordinate to form a unit vector, taking care not to divide by zero
      const float x_norm = l ? x / l : 0.0f;
      const float y_norm = l ? y / l : 1.0f;  // ensure we don't get 0 for both sine and cosine below
      // apply the rotation matrix
      const float x_rot = x_norm * cos_alpha + y_norm * sin_alpha;
      const float y_rot = -x_norm * sin_alpha + y_norm * cos_alpha;
      // at this point, x_rot = cos(v) and y_rot = sin(v) since they are on the unit circle; we need the squared values
      const float cosv2 = x_rot * x_rot;
      const float sinv2 = y_rot * y_rot;

      // project the rotated unit vector out to the ellipse and the outer border
      const float radius2 = a2 * b2 / (a2 * sinv2 + b2 * cosv2);
      const float total2 = ta2 * tb2 / (ta2 * sinv2 + tb2 * cosv2);

      // quadratic falloff between the ellipses's radius and the radius of the outside of the feathering
      // ratio = 0.0 at the outer border, >= 1.0 within the ellipse, negative outside the falloff
      const float ratio = (total2 - l2) / (total2 - radius2);
      // enforce 1.0 inside the ellipse and 0.0 outside the feathering
      const float f = CLIP(ratio);
      bufptr[i << out_scale] = f * f;
    }
}

static float *const _ellipse_points_to_transform(const float center_x, const float center_y, const float dim1, const float dim2,
                                                 const float rotation, const float wd, const float ht, size_t *point_count)
{

  const float v1 = ((rotation) / 180.0f) * M_PI;
  const float v2 = ((rotation - 90.0f) / 180.0f) * M_PI;
  float a = 0.0f, b = 0.0f, v = 0.0f;

  if(dim1 >= dim2)
  {
    a = dim1;
    b = dim2;
    v = v1;
  }
  else
  {
    a = dim2;
    b = dim1;
    v = v2;
  }

  const float sinv = sinf(v);
  const float cosv = cosf(v);

  // how many points do we need ?
  const float lambda = (a - b) / (a + b);
  const int l = (int)(M_PI * (a + b)
                      * (1.0f + (3.0f * lambda * lambda) / (10.0f + sqrtf(4.0f - 3.0f * lambda * lambda))));

  // buffer allocation
  float *points = dt_alloc_align_float((size_t) 2 * (l + 5));
  if(points == NULL)
    return NULL;
  *point_count = l + 5;

  // now we set the points - first the center
  const float x = points[0] = center_x * wd;
  const float y = points[1] = center_y * ht;
  // then the control node points (ends of semimajor/semiminor axes)
  points[2] = x + a * cosf(v);
  points[3] = y + a * sinf(v);
  points[4] = x - a * cosf(v);
  points[5] = y - a * sinf(v);
  points[6] = x + b * cosf(v - M_PI / 2.0f);
  points[7] = y + b * sinf(v - M_PI / 2.0f);
  points[8] = x - b * cosf(v - M_PI / 2.0f);
  points[9] = y - b * sinf(v - M_PI / 2.0f);
  // and finally the regularly-spaced points on the circumference
  for(int i = 5; i < l + 5; i++)
  {
    float alpha = (i - 5) * 2.0 * M_PI / (float)l;
    points[i * 2] = x + a * cosf(alpha) * cosv - b * sinf(alpha) * sinv;
    points[i * 2 + 1] = y + a * cosf(alpha) * sinv + b * sinf(alpha) * cosv;
  }
  return points;
}

static int _ellipse_get_source_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                    dt_masks_form_t *form, int *width, int *height, int *posx, int *posy)
{
  // we get the ellipse values
  dt_masks_point_ellipse_t *ellipse = (dt_masks_point_ellipse_t *)((form->points)->data);
  const float wd = piece->pipe->iwidth, ht = piece->pipe->iheight;
  const int prop = ellipse->flags & DT_MASKS_ELLIPSE_PROPORTIONAL;
  const float total[2] = { (prop ? ellipse->radius[0] * (1.0f + ellipse->border) : ellipse->radius[0] + ellipse->border) * MIN(wd, ht),
                           (prop ? ellipse->radius[1] * (1.0f + ellipse->border) : ellipse->radius[1] + ellipse->border) * MIN(wd, ht) };

  // next we compute the points to be transformed
  size_t point_count = 0;
  float *const restrict points
    = _ellipse_points_to_transform(form->source[0], form->source[1], total[0], total[1], ellipse->rotation, wd, ht, &point_count);
  if(!points)
    return 0;

  // and we transform them with all distorted modules
  if(!dt_dev_distort_transform_plus(darktable.develop, piece->pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, points, point_count))
  {
    dt_free_align(points);
    return 0;
  }

  // finally, find the extreme left/right and top/bottom points
  _bounding_box(points, point_count, width, height, posx, posy);
  dt_free_align(points);
  return 1;
}

static int _ellipse_get_area(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                             dt_masks_form_t *const form,
                             int *width, int *height, int *posx, int *posy)
{
  // we get the ellipse values
  dt_masks_point_ellipse_t *ellipse = (dt_masks_point_ellipse_t *)((form->points)->data);
  const float wd = piece->pipe->iwidth, ht = piece->pipe->iheight;
  const int prop = ellipse->flags & DT_MASKS_ELLIPSE_PROPORTIONAL;
  const float total[2] = { (prop ? ellipse->radius[0] * (1.0f + ellipse->border) : ellipse->radius[0] + ellipse->border) * MIN(wd, ht),
                           (prop ? ellipse->radius[1] * (1.0f + ellipse->border) : ellipse->radius[1] + ellipse->border) * MIN(wd, ht) };

  // next we compute the points to be transformed
  size_t point_count = 0;
  float *const restrict points
    = _ellipse_points_to_transform(ellipse->center[0], ellipse->center[1], total[0], total[1], ellipse->rotation, wd, ht, &point_count);
  if(!points)
    return 0;

  // and we transform them with all distorted modules
  if(!dt_dev_distort_transform_plus(module->dev, piece->pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, points, point_count))
  {
    dt_free_align(points);
    return 0;
  }

  // finally, find the extreme left/right and top/bottom points
  _bounding_box(points, point_count, width, height, posx, posy);
  dt_free_align(points);
  return 1;
}

static int _ellipse_get_mask(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                             dt_masks_form_t *const form,
                             float **buffer, int *width, int *height, int *posx, int *posy)
{
  double start2 = 0.0;
  if(darktable.unmuted & DT_DEBUG_PERF) start2 = dt_get_wtime();

  // we get the area
  if(!_ellipse_get_area(module, piece, form, width, height, posx, posy)) return 0;

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] ellipse area took %0.04f sec\n", form->name, dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we get the ellipse values
  dt_masks_point_ellipse_t *ellipse = (dt_masks_point_ellipse_t *)((form->points)->data);

  // we create a buffer of points with all points in the area
  int w = *width, h = *height;
  float *points = dt_alloc_align_float((size_t)2 * w * h);
  if(points == NULL)
    return 0;

  for(int i = 0; i < h; i++)
    for(int j = 0; j < w; j++)
    {
      points[(i * w + j) * 2] = (j + (*posx));
      points[(i * w + j) * 2 + 1] = (i + (*posy));
    }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] ellipse draw took %0.04f sec\n", form->name, dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we back transform all this points
  if(!dt_dev_distort_backtransform_plus(module->dev, piece->pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, points, (size_t)w * h))
  {
    dt_free_align(points);
    return 0;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] ellipse transform took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we allocate the buffer
  *buffer = dt_alloc_align_float((size_t)w * h);
  if(*buffer == NULL)
  {
    dt_free_align(points);
    return 0;
  }

  // we populate the buffer
  const int wi = piece->pipe->iwidth, hi = piece->pipe->iheight;
  const float center[2] = { ellipse->center[0] * wi, ellipse->center[1] * hi };
  const float radius[2] = { ellipse->radius[0] * MIN(wi, hi), ellipse->radius[1] * MIN(wi, hi) };
  const float total[2] =  { (ellipse->flags & DT_MASKS_ELLIPSE_PROPORTIONAL ? ellipse->radius[0] * (1.0f + ellipse->border) : ellipse->radius[0] + ellipse->border) * MIN(wi, hi),
                            (ellipse->flags & DT_MASKS_ELLIPSE_PROPORTIONAL ? ellipse->radius[1] * (1.0f + ellipse->border) : ellipse->radius[1] + ellipse->border) * MIN(wi, hi) };

  float a = 0.0F, b = 0.0F, ta = 0.0F, tb = 0.0F, alpha = 0.0F;

  if(radius[0] >= radius[1])
  {
    a = radius[0];
    b = radius[1];
    ta = total[0];
    tb = total[1];
    alpha = (ellipse->rotation / 180.0f) * M_PI;
  }
  else
  {
    a = radius[1];
    b = radius[0];
    ta = total[1];
    tb = total[0];
    alpha = ((ellipse->rotation - 90.0f) / 180.0f) * M_PI;
  }

  float *const bufptr = *buffer;

  _fill_mask((size_t)(h)*w, bufptr, points, center, a, b, ta, tb, alpha, 0);

  dt_free_align(points);

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] ellipse fill took %0.04f sec\n", form->name, dt_get_wtime() - start2);

  return 1;
}

static int _ellipse_get_mask_roi(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                                 dt_masks_form_t *const form, const dt_iop_roi_t *roi, float *buffer)
{
  double start1 = 0.0;
  double start2 = start1;
  if(darktable.unmuted & DT_DEBUG_PERF) start2 = start1 = dt_get_wtime();

  // we get the ellipse parameters
  dt_masks_point_ellipse_t *ellipse = (dt_masks_point_ellipse_t *)((form->points)->data);
  const int wi = piece->pipe->iwidth, hi = piece->pipe->iheight;
  const float center[2] = { ellipse->center[0] * wi, ellipse->center[1] * hi };
  const float radius[2] = { ellipse->radius[0] * MIN(wi, hi), ellipse->radius[1] * MIN(wi, hi) };
  const float total[2] = { (ellipse->flags & DT_MASKS_ELLIPSE_PROPORTIONAL ? ellipse->radius[0] * (1.0f + ellipse->border) : ellipse->radius[0] + ellipse->border) * MIN(wi, hi),
                           (ellipse->flags & DT_MASKS_ELLIPSE_PROPORTIONAL ? ellipse->radius[1] * (1.0f + ellipse->border) : ellipse->radius[1] + ellipse->border) * MIN(wi, hi) };

  const float a = radius[0];
  const float b = radius[1];
  const float ta = total[0];
  const float tb = total[1];
  const float alpha = (ellipse->rotation / 180.0f) * M_PI;
  const float cosa = cosf(alpha);
  const float sina = sinf(alpha);

  // we create a buffer of grid points for later interpolation: higher speed and reduced memory footprint;
  // we match size of buffer to bounding box around the shape
  const int w = roi->width;
  const int h = roi->height;
  const int px = roi->x;
  const int py = roi->y;
  const float iscale = 1.0f / roi->scale;
  const int grid = CLAMP((10.0f * roi->scale + 2.0f) / 3.0f, 1, 4); // scale dependent resolution
  const int gw = (w + grid - 1) / grid + 1;  // grid dimension of total roi
  const int gh = (h + grid - 1) / grid + 1;  // grid dimension of total roi

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] ellipse init took %0.04f sec\n", form->name, dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we look at the outer line of the shape - no effects outside of this ellipse;
  // we need many points as we do not know how the ellipse might get distorted in the pixelpipe
  const float lambda = (ta - tb) / (ta + tb);
  const int l = (int)(M_PI * (ta + tb) * (1.0f + (3.0f * lambda * lambda) / (10.0f + sqrtf(4.0f - 3.0f * lambda * lambda))));
  const size_t ellpts = MIN(360, l);
  float *ell = dt_alloc_align_float(ellpts * 2);
  if(ell == NULL) return 0;

#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ellpts, center, ta, tb, cosa, sina) \
  shared(ell)
#else
#pragma omp parallel for shared(points)
#endif
#endif
  for(int n = 0; n < ellpts; n++)
  {
    const float phi = (2.0f * M_PI * n) / ellpts;
    const float cosp = cosf(phi);
    const float sinp = sinf(phi);
    ell[2 * n] = center[0] + ta * cosa * cosp - tb * sina * sinp;
    ell[2 * n + 1] = center[1] + ta * sina * cosp + tb * cosa * sinp;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] ellipse outline took %0.04f sec\n", form->name, dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we transform the outline from input image coordinates to current position in pixelpipe
  if(!dt_dev_distort_transform_plus(module->dev, piece->pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, ell,
                                        ellpts))
  {
    dt_free_align(ell);
    return 0;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] ellipse outline transform took %0.04f sec\n", form->name, dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we get the min/max values ...
  float xmin = FLT_MAX, ymin = FLT_MAX, xmax = FLT_MIN, ymax = FLT_MIN;
  for(int n = 0; n < ellpts; n++)
  {
    // just in case that transform throws surprising values
    if(!(isnormal(ell[2 * n]) && isnormal(ell[2 * n + 1]))) continue;

    xmin = MIN(xmin, ell[2 * n]);
    xmax = MAX(xmax, ell[2 * n]);
    ymin = MIN(ymin, ell[2 * n + 1]);
    ymax = MAX(ymax, ell[2 * n + 1]);
  }

#if 0
  printf("xmin %f, xmax %f, ymin %f, ymax %f\n", xmin, xmax, ymin, ymax);
  printf("wi %d, hi %d, iscale %f\n", wi, hi, iscale);
  printf("w %d, h %d, px %d, py %d\n", w, h, px, py);
#endif

  // ... and calculate the bounding box with a bit of reserve
  const int bbxm = CLAMP((int)floorf(xmin / iscale - px) / grid - 1, 0, gw - 1);
  const int bbXM = CLAMP((int)ceilf(xmax / iscale - px) / grid + 2, 0, gw - 1);
  const int bbym = CLAMP((int)floorf(ymin / iscale - py) / grid - 1, 0, gh - 1);
  const int bbYM = CLAMP((int)ceilf(ymax / iscale - py) / grid + 2, 0, gh - 1);
  const int bbw = bbXM - bbxm + 1;
  const int bbh = bbYM - bbym + 1;

#if 0
  printf("bbxm %d, bbXM %d, bbym %d, bbYM %d\n", bbxm, bbXM, bbym, bbYM);
  printf("gw %d, gh %d, bbw %d, bbh %d\n", gw, gh, bbw, bbh);
#endif

  dt_free_align(ell);

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] ellipse bounding box took %0.04f sec\n", form->name, dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // check if there is anything to do at all;
  // only if width and height of bounding box is 2 or greater the shape lies inside of roi and requires action
  if(bbw <= 1 || bbh <= 1)
    return 1;

  float *points = dt_alloc_align_float((size_t)2 * bbw * bbh);
  if(points == NULL) return 0;

  // we populate the grid points in module coordinates
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(grid, bbxm, bbym, bbXM, bbYM, bbw, iscale, px, py) \
  shared(points) schedule(static) collapse(2)
#else
#pragma omp parallel for shared(points)
#endif
#endif
  for(int j = bbym; j <= bbYM; j++)
    for(int i = bbxm; i <= bbXM; i++)
    {
      const size_t index = (size_t)(j - bbym) * bbw + i - bbxm;
      points[index * 2] = (grid * i + px) * iscale;
      points[index * 2 + 1] = (grid * j + py) * iscale;
    }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] ellipse grid took %0.04f sec\n", form->name, dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we back transform all these points to the input image coordinates
  if(!dt_dev_distort_backtransform_plus(module->dev, piece->pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, points,
                                        (size_t)bbw * bbh))
  {
    dt_free_align(points);
    return 0;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] ellipse transform took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we calculate the mask values at the transformed points;
  // re-use the points array for results; this requires out_scale==1 to double the offsets at which they are stored
  _fill_mask((size_t)(bbh)*bbw, points, points, center, a, b, ta, tb, alpha, 1);

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] ellipse draw took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we fill the pre-initialized output buffer by interpolation;
  // we only need to take the contents of our bounding box into account
  const int endx = MIN(w, bbXM * grid);
  const int endy = MIN(h, bbYM * grid);
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(grid, bbxm, bbym, bbw, endx, endy, w) \
  shared(buffer, points)
#else
#pragma omp parallel for shared(buffer)
#endif
#endif
  for(int j = bbym * grid; j < endy; j++)
  {
    const int jj = j % grid;
    const int mj = j / grid - bbym;
    for(int i = bbxm * grid; i < endx; i++)
    {
      const int ii = i % grid;
      const int mi = i / grid - bbxm;
      const size_t mindex = (size_t)mj * bbw + mi;
      buffer[(size_t)j * w + i]
          = (points[mindex * 2] * (grid - ii) * (grid - jj) + points[(mindex + 1) * 2] * ii * (grid - jj)
             + points[(mindex + bbw) * 2] * (grid - ii) * jj + points[(mindex + bbw + 1) * 2] * ii * jj)
            / (grid * grid);
    }
  }

  dt_free_align(points);

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] ellipse fill took %0.04f sec\n", form->name, dt_get_wtime() - start2);
    dt_print(DT_DEBUG_MASKS, "[masks %s] ellipse total render took %0.04f sec\n", form->name,
             dt_get_wtime() - start1);
  }
  return 1;
}

static GSList *_ellipse_setup_mouse_actions(const struct dt_masks_form_t *const form)
{
  GSList *lm = NULL;
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_SCROLL, 0, _("[ELLIPSE] change size"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_SCROLL, GDK_SHIFT_MASK, _("[ELLIPSE] change feather size"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_SCROLL, GDK_SHIFT_MASK|GDK_CONTROL_MASK, _("[ELLIPSE] rotate shape"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_SCROLL, GDK_CONTROL_MASK, _("[ELLIPSE] change opacity"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_LEFT, GDK_SHIFT_MASK, _("[ELLIPSE] switch feathering mode"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_LEFT_DRAG, GDK_CONTROL_MASK, _("[ELLIPSE] rotate shape"));
  return lm;
}

static void _ellipse_set_form_name(struct dt_masks_form_t *const form, const size_t nb)
{
  snprintf(form->name, sizeof(form->name), _("ellipse #%d"), (int)nb);
}

static void _ellipse_duplicate_points(dt_develop_t *const dev, dt_masks_form_t *const base, dt_masks_form_t *const dest)
{
  (void)dev; // unused arg, keep compiler from complaining
  for(GList *pts = base->points; pts; pts = g_list_next(pts))
  {
    dt_masks_point_ellipse_t *pt = (dt_masks_point_ellipse_t *)pts->data;
    dt_masks_point_ellipse_t *npt = (dt_masks_point_ellipse_t *)malloc(sizeof(dt_masks_point_ellipse_t));
    memcpy(npt, pt, sizeof(dt_masks_point_ellipse_t));
    dest->points = g_list_append(dest->points, npt);
  }
}

static void _ellipse_initial_source_pos(const float iwd, const float iht, float *x, float *y)
{
  const float radius_a = dt_conf_get_float("plugins/darkroom/spots/ellipse_radius_a");
  const float radius_b = dt_conf_get_float("plugins/darkroom/spots/ellipse_radius_b");

  *x = (radius_a * iwd);
  *y = -(radius_b * iht);
}

static void _ellipse_set_hint_message(const dt_masks_form_gui_t *const gui, const dt_masks_form_t *const form,
                                        const int opacity, char *const restrict msgbuf, const size_t msgbuf_len)
{
  if(gui->creation)
    g_snprintf(msgbuf, msgbuf_len,
               _("<b>size</b>: scroll, <b>feather size</b>: shift+scroll\n"
                 "<b>rotation</b>: ctrl+shift+scroll, <b>opacity</b>: ctrl+scroll (%d%%)"), opacity);
  else if(gui->point_selected >= 0)
    g_strlcat(msgbuf, _("<b>rotate</b>: ctrl+drag"), msgbuf_len);
  else if(gui->form_selected)
    g_snprintf(msgbuf, msgbuf_len,
               _("<b>feather mode</b>: shift+click, <b>rotate</b>: ctrl+drag\n"
                 "<b>size</b>: scroll, <b>feather size</b>: shift+scroll, <b>opacity</b>: ctrl+scroll (%d%%)"), opacity);
}

static void _ellipse_sanitize_config(dt_masks_type_t type)
{
  dt_conf_get_and_sanitize_float(DT_MASKS_CONF(type, ellipse, rotation), 0.0f, 360.f);
  int flags = dt_conf_get_and_sanitize_int(DT_MASKS_CONF(type, ellipse, flags), DT_MASKS_ELLIPSE_EQUIDISTANT, DT_MASKS_ELLIPSE_PROPORTIONAL);
  float radius_a = dt_conf_get_float(DT_MASKS_CONF(type, ellipse, radius_a));
  float radius_b = dt_conf_get_float(DT_MASKS_CONF(type, ellipse, radius_b));
  float border = dt_conf_get_float(DT_MASKS_CONF(type, ellipse, border));

  const float ratio = radius_a / radius_b;

  if(radius_a > radius_b)
  {
    radius_a = CLAMPS(radius_a, 0.001f, 0.5f);
    radius_b = radius_a / ratio;
  }
  else
  {
    radius_b = CLAMPS(radius_b, 0.001f, 0.5);
    radius_a = ratio * radius_b;
  }

  const float reference = (flags & DT_MASKS_ELLIPSE_PROPORTIONAL ? 1.0f / fmin(radius_a, radius_b) : 1.0f);
  border = CLAMPS(border, 0.001f * reference, reference);

  DT_CONF_SET_SANITIZED_FLOAT(DT_MASKS_CONF(type, ellipse, radius_a), radius_a, 0.001f, 0.5f);
  DT_CONF_SET_SANITIZED_FLOAT(DT_MASKS_CONF(type, ellipse, radius_b), radius_b, 0.001f, 0.5f);
  DT_CONF_SET_SANITIZED_FLOAT(DT_MASKS_CONF(type, ellipse, border), border, 0.001f, reference);
}

static void _ellipse_modify_property(dt_masks_form_t *const form, dt_masks_property_t prop, float old_val, float new_val, float *sum, int *count, float *min, float *max)
{
  float ratio = (!old_val || !new_val) ? 1.0f : new_val / old_val;

  dt_masks_point_ellipse_t *ellipse = (form->points)->data;

  const float radius_limit = form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE) ? 0.5f : 1.0f;

  switch(prop)
  {
    case DT_MASKS_PROPERTY_SIZE:;
      const float oldradius0 = ellipse->radius[0], oldradius1 = ellipse->radius[1];
      ellipse->radius[0] = CLAMP(ellipse->radius[0] * ratio                        , 0.001f, radius_limit);
      ellipse->radius[1] = CLAMP(ellipse->radius[1] * ellipse->radius[0]/oldradius0, 0.001f, radius_limit);
      ellipse->radius[0] = oldradius0 * ellipse->radius[1]/oldradius1;
      *sum += fmaxf(ellipse->radius[0], ellipse->radius[0]);
      *max = fminf(*max, fminf(radius_limit / ellipse->radius[0], radius_limit / ellipse->radius[1]));
      *min = fmaxf(*min, fmaxf(0.001f / ellipse->radius[0], 0.001f / ellipse->radius[1]));
      ++*count;
      break;
    case DT_MASKS_PROPERTY_FEATHER:;
      const float reference = (ellipse->flags & DT_MASKS_ELLIPSE_PROPORTIONAL ? 1.0f/fmin(ellipse->radius[0], ellipse->radius[1]) : 1.0f);
      ellipse->border = CLAMP(ellipse->border * ratio, 0.001f * reference, radius_limit * reference);
      *sum += ellipse->border;
      *max = fminf(*max, radius_limit * reference / ellipse->border);
      *min = fmaxf(*min, 0.001f * reference / ellipse->border);
      ++*count;
      break;
    case DT_MASKS_PROPERTY_ROTATION:
      *sum += (ellipse->rotation = fmodf(ellipse->rotation + new_val - old_val, 360.0f));
      ++*count;
      break;
    default:;
  }
}

// The function table for ellipses.  This must be public, i.e. no "static" keyword.
const dt_masks_functions_t dt_masks_functions_ellipse = {
  .point_struct_size = sizeof(struct dt_masks_point_ellipse_t),
  .sanitize_config = _ellipse_sanitize_config,
  .setup_mouse_actions = _ellipse_setup_mouse_actions,
  .set_form_name = _ellipse_set_form_name,
  .set_hint_message = _ellipse_set_hint_message,
  .modify_property = _ellipse_modify_property,
  .duplicate_points = _ellipse_duplicate_points,
  .initial_source_pos = _ellipse_initial_source_pos,
  .get_distance = _ellipse_get_distance,
  .get_points = _ellipse_get_points,
  .get_points_border = _ellipse_get_points_border,
  .get_mask = _ellipse_get_mask,
  .get_mask_roi = _ellipse_get_mask_roi,
  .get_area = _ellipse_get_area,
  .get_source_area = _ellipse_get_source_area,
  .mouse_moved = _ellipse_events_mouse_moved,
  .mouse_scrolled = _ellipse_events_mouse_scrolled,
  .button_pressed = _ellipse_events_button_pressed,
  .button_released = _ellipse_events_button_released,
  .post_expose = _ellipse_events_post_expose
};

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

