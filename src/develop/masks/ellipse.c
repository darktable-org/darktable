/*
    This file is part of darktable,
    copyright (c) 2012--2013 aldric renaudin.
    copyright (c) 2013--2016 Ulrich Pegelow.

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
#include "common/debug.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/masks.h"

static inline void _ellipse_point_transform(const float xref, const float yref, const float x, const float y,
                                            const float sinr, const float cosr, const float scalea,
                                            const float scaleb, const float sinv, const float cosv,
                                            float *xnew, float *ynew)
{
  float xtmp = (scaleb * sinr * sinr + scalea * cosr * cosr) * (x - xref)
               + (scalea * cosr * sinr - scaleb * cosr * sinr) * (y - yref);
  float ytmp = (scalea * cosr * sinr - scaleb * cosr * sinr) * (x - xref)
               + (scalea * sinr * sinr + scaleb * cosr * cosr) * (y - yref);

  *xnew = xref + cosv * xtmp - sinv * ytmp;
  *ynew = yref + sinv * xtmp + cosv * ytmp;
}

// Jordan's point in polygon test
static int dt_ellipse_cross_test(float x, float y, float *point_1, float *point_2)
{
  float x_a = x;
  float y_a = y;
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

  float delta = (x_b - x_a) * (y_c - y_a) - (y_b - y_a) * (x_c - x_a);

  if(delta > 0)
    return -1;
  else if(delta < 0)
    return 1;
  else
    return 0;
}

static int dt_ellipse_point_in_polygon(float x, float y, float *points, int points_count)
{
  int t = -1;

  t *= dt_ellipse_cross_test(x, y, points + 2 * (points_count - 1), points);

  for(int i = 0; i < points_count - 2; i++)
    t *= dt_ellipse_cross_test(x, y, points + 2 * i, points + 2 * (i + 1));

  return t;
}

// check if point is close to path - segment by segment
static int dt_ellipse_point_close_to_path(float x, float y, float as, float *points, int points_count)
{
  float as2 = as * as;

  float lastx = points[2 * (points_count - 1)];
  float lasty = points[2 * (points_count - 1) + 1];

  for(int i = 0; i < points_count; i++)
  {
    float px = points[2 * i];
    float py = points[2 * i + 1];

    float r1 = x - lastx;
    float r2 = y - lasty;
    float r3 = px - lastx;
    float r4 = py - lasty;

    float d = r1 * r3 + r2 * r4;
    float l = r3 * r3 + r4 * r4;
    float p = d / l;

    float xx, yy;

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

    float dx = x - xx;
    float dy = y - yy;

    if(dx * dx + dy * dy < as2) return 1;
  }
  return 0;
}

static void dt_ellipse_get_distance(float x, int y, float as, dt_masks_form_gui_t *gui, int index,
                                    int *inside, int *inside_border, int *near, int *inside_source)
{
  if(!gui) return;

  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return;


  // we first check if we are inside the source form
  if(gpt->source_count > 10)
  {
    if(dt_ellipse_point_in_polygon(x, y, gpt->source + 10, gpt->source_count - 5) >= 0)
    {
      *inside_source = 1;
      *inside = 1;
      *inside_border = 0;
      *near = -1;
      return;
    }
  }

  *inside_source = 0;

  // we check if it's inside borders
  if(dt_ellipse_point_in_polygon(x, y, gpt->border + 10, gpt->border_count - 5) < 0)
  {
    *inside = 0;
    *inside_border = 0;
    *near = -1;
    return;
  }

  *inside = 1;
  *near = 0;
  *inside_border = 1;

  if(dt_ellipse_point_in_polygon(x, y, gpt->points + 10, gpt->points_count - 5) >= 0) *inside_border = 0;
  if(dt_ellipse_point_close_to_path(x, y, as, gpt->points + 10, gpt->points_count - 5)) *near = 1;
}

static void dt_ellipse_draw_shape(cairo_t *cr, double *dashed, const int selected, const float zoom_scale,
                                  const float dx, const float dy, const float xref, const float yref,
                                  const float sinv, const float cosv, const float scalea, const float scaleb,
                                  float *points, const int points_count)
{
  if(points_count <= 10) return;

  const float r = atan2(points[3] - points[1], points[2] - points[0]);
  const float sinr = sin(r);
  const float cosr = cos(r);

  float x = 0.f;
  float y = 0.f;

  cairo_set_dash(cr, dashed, 0, 0);
  if(selected)
    cairo_set_line_width(cr, 5.0 / zoom_scale);
  else
    cairo_set_line_width(cr, 3.0 / zoom_scale);
  cairo_set_source_rgba(cr, .3, .3, .3, .8);

  _ellipse_point_transform(xref, yref, points[10] + dx, points[11] + dy, sinr, cosr, scalea, scaleb, sinv, cosv,
                           &x, &y);
  cairo_move_to(cr, x, y);
  for(int i = 6; i < points_count; i++)
  {
    _ellipse_point_transform(xref, yref, points[i * 2] + dx, points[i * 2 + 1] + dy, sinr, cosr, scalea, scaleb,
                             sinv, cosv, &x, &y);
    cairo_line_to(cr, x, y);
  }
  _ellipse_point_transform(xref, yref, points[10] + dx, points[11] + dy, sinr, cosr, scalea, scaleb, sinv, cosv,
                           &x, &y);
  cairo_line_to(cr, x, y);
  cairo_stroke_preserve(cr);
  if(selected)
    cairo_set_line_width(cr, 2.0 / zoom_scale);
  else
    cairo_set_line_width(cr, 1.0 / zoom_scale);
  cairo_set_source_rgba(cr, .8, .8, .8, .8);
  cairo_stroke(cr);
}

static void dt_ellipse_draw_border(cairo_t *cr, double *dashed, const float len, const int selected,
                                   const float zoom_scale, const float dx, const float dy, const float xref,
                                   const float yref, const float sinv, const float cosv, const float scaleab,
                                   const float scalebb, float *border, const int border_count)
{
  if(border_count <= 10) return;

  const float r = atan2(border[3] - border[1], border[2] - border[0]);
  const float sinr = sin(r);
  const float cosr = cos(r);

  float x = 0.f;
  float y = 0.f;

  cairo_set_dash(cr, dashed, len, 0);
  if(selected)
    cairo_set_line_width(cr, 2.0 / zoom_scale);
  else
    cairo_set_line_width(cr, 1.0 / zoom_scale);
  cairo_set_source_rgba(cr, .3, .3, .3, .8);

  _ellipse_point_transform(xref, yref, border[10] + dx, border[11] + dy, sinr, cosr, scaleab, scalebb, sinv, cosv,
                           &x, &y);
  cairo_move_to(cr, x, y);
  for(int i = 6; i < border_count; i++)
  {
    _ellipse_point_transform(xref, yref, border[i * 2] + dx, border[i * 2 + 1] + dy, sinr, cosr, scaleab, scalebb,
                             sinv, cosv, &x, &y);
    cairo_line_to(cr, x, y);
  }
  _ellipse_point_transform(xref, yref, border[10] + dx, border[11] + dy, sinr, cosr, scaleab, scalebb, sinv, cosv,
                           &x, &y);
  cairo_line_to(cr, x, y);

  cairo_stroke_preserve(cr);
  if(selected)
    cairo_set_line_width(cr, 2.0 / zoom_scale);
  else
    cairo_set_line_width(cr, 1.0 / zoom_scale);
  cairo_set_source_rgba(cr, .8, .8, .8, .8);
  cairo_set_dash(cr, dashed, len, 4);
  cairo_stroke(cr);
}

static int dt_ellipse_get_points(dt_develop_t *dev, float xx, float yy, float radius_a, float radius_b,
                                 float rotation, float **points, int *points_count)
{
  const float wd = dev->preview_pipe->iwidth;
  const float ht = dev->preview_pipe->iheight;
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
  *points = calloc(2 * (l + 5), sizeof(float));
  *points_count = l + 5;

  // now we set the points
  const float x = (*points)[0] = xx * wd;
  const float y = (*points)[1] = yy * ht;

  (*points)[2] = x + a * cos(v);
  (*points)[3] = y + a * sin(v);
  (*points)[4] = x - a * cos(v);
  (*points)[5] = y - a * sin(v);

  (*points)[6] = x + b * cos(v - M_PI / 2.0f);
  (*points)[7] = y + b * sin(v - M_PI / 2.0f);
  (*points)[8] = x - b * cos(v - M_PI / 2.0f);
  (*points)[9] = y - b * sin(v - M_PI / 2.0f);


  for(int i = 5; i < l + 5; i++)
  {
    float alpha = (i - 5) * 2.0 * M_PI / (float)l;
    (*points)[i * 2] = x + a * cosf(alpha) * cosv - b * sinf(alpha) * sinv;
    (*points)[i * 2 + 1] = y + a * cosf(alpha) * sinv + b * sinf(alpha) * cosv;
  }

  // and we transform them with all distorted modules
  if(dt_dev_distort_transform(dev, *points, l + 5)) return 1;

  // if we failed, then free all and return
  free(*points);
  *points = NULL;
  *points_count = 0;
  return 0;
}

// set the initial source position value for a clone mask
static void dt_ellipse_set_source_pos_initial_value(dt_masks_form_gui_t *gui, dt_masks_form_t *form,
                                                    dt_masks_point_ellipse_t *ellipse)
{
  // if this is the first time the relative pos is used
  if(gui->source_pos_type == DT_MASKS_SOURCE_POS_RELATIVE_TEMP)
  {
    // if is has not been defined by the user, set some default
    if(gui->posx_source == -1.f && gui->posy_source == -1.f)
    {
      form->source[0] = ellipse->center[0] + ellipse->radius[0];
      form->source[1] = ellipse->center[1] - ellipse->radius[1];
    }
    else
    {
      // if a position was defined by the user, use the absolute value the first time
      float pts_src[2] = { gui->posx_source, gui->posy_source };
      dt_dev_distort_backtransform(darktable.develop, pts_src, 1);

      form->source[0] = pts_src[0] / darktable.develop->preview_pipe->iwidth;
      form->source[1] = pts_src[1] / darktable.develop->preview_pipe->iheight;
    }

    // save the relative value for the next time
    gui->posx_source = form->source[0] - ellipse->center[0];
    gui->posy_source = form->source[1] - ellipse->center[1];

    gui->source_pos_type = DT_MASKS_SOURCE_POS_RELATIVE;
  }
  else if(gui->source_pos_type == DT_MASKS_SOURCE_POS_RELATIVE)
  {
    // original pos was already defined and relative value calculated, just use it
    form->source[0] = ellipse->center[0] + gui->posx_source;
    form->source[1] = ellipse->center[1] + gui->posy_source;
  }
  else if(gui->source_pos_type == DT_MASKS_SOURCE_POS_ABSOLUTE)
  {
    // an absolute position was defined by the user
    float pts_src[2] = { gui->posx_source, gui->posy_source };
    dt_dev_distort_backtransform(darktable.develop, pts_src, 1);

    form->source[0] = pts_src[0] / darktable.develop->preview_pipe->iwidth;
    form->source[1] = pts_src[1] / darktable.develop->preview_pipe->iheight;
  }
  else
    fprintf(stderr, "unknown source position type\n");
}

static int dt_ellipse_events_mouse_scrolled(struct dt_iop_module_t *module, float pzx, float pzy, int up,
                                            uint32_t state, dt_masks_form_t *form, int parentid,
                                            dt_masks_form_gui_t *gui, int index)
{
  // add a preview when creating an ellipse
  if(gui->creation)
  {
    if((state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK)) == (GDK_SHIFT_MASK | GDK_CONTROL_MASK))
    {
      float rotation;

      if(form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE))
        rotation = dt_conf_get_float("plugins/darkroom/spots/ellipse_rotation");
      else
        rotation = dt_conf_get_float("plugins/darkroom/masks/ellipse/rotation");

      if(up)
        rotation -= 10.f;
      else
        rotation += 10.f;
      rotation = fmodf(rotation, 360.0f);

      if(form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE))
        dt_conf_set_float("plugins/darkroom/spots/ellipse_rotation", rotation);
      else
        dt_conf_set_float("plugins/darkroom/masks/ellipse/rotation", rotation);
    }
    else if((state & GDK_SHIFT_MASK) == GDK_SHIFT_MASK)
    {
      float masks_border;
      int flags;
      float radius_a;
      float radius_b;

      if(form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE))
      {
        masks_border = dt_conf_get_float("plugins/darkroom/spots/ellipse_border");
        flags = dt_conf_get_int("plugins/darkroom/spots/ellipse_flags");
        radius_a = dt_conf_get_float("plugins/darkroom/spots/ellipse_radius_a");
        radius_b = dt_conf_get_float("plugins/darkroom/spots/ellipse_radius_b");
      }
      else
      {
        masks_border = dt_conf_get_float("plugins/darkroom/masks/ellipse/border");
        flags = dt_conf_get_int("plugins/darkroom/masks/ellipse/flags");
        radius_a = dt_conf_get_float("plugins/darkroom/masks/ellipse/radius_a");
        radius_b = dt_conf_get_float("plugins/darkroom/masks/ellipse/radius_b");
      }

      const float reference = (flags & DT_MASKS_ELLIPSE_PROPORTIONAL ? 1.0f / fmin(radius_a, radius_b) : 1.0f);
      if(up && masks_border > 0.001f * reference)
        masks_border *= 0.97f;
      else if(!up && masks_border < 1.0f * reference)
        masks_border *= 1.0f / 0.97f;
      else
        return 1;
      masks_border = CLAMP(masks_border, 0.001f * reference, reference);

      if(form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE))
        dt_conf_set_float("plugins/darkroom/spots/ellipse_border", masks_border);
      else
        dt_conf_set_float("plugins/darkroom/masks/ellipse/border", masks_border);
    }
    else if(state == 0)
    {
      float radius_a;
      float radius_b;

      if(form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE))
      {
        radius_a = dt_conf_get_float("plugins/darkroom/spots/ellipse_radius_a");
        radius_b = dt_conf_get_float("plugins/darkroom/spots/ellipse_radius_b");
      }
      else
      {
        radius_a = dt_conf_get_float("plugins/darkroom/masks/ellipse/radius_a");
        radius_b = dt_conf_get_float("plugins/darkroom/masks/ellipse/radius_b");
      }

      const float oldradius = radius_a;

      if(up && radius_a > 0.001f)
        radius_a *= 0.97f;
      else if(!up && radius_a < 1.0f)
        radius_a *= 1.0f / 0.97f;
      else
        return 1;

      radius_a = CLAMP(radius_a, 0.001f, 1.0f);

      const float factor = radius_a / oldradius;
      radius_b *= factor;

      if(form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE))
      {
        dt_conf_set_float("plugins/darkroom/spots/ellipse_radius_a", radius_a);
        dt_conf_set_float("plugins/darkroom/spots/ellipse_radius_b", radius_b);
      }
      else
      {
        dt_conf_set_float("plugins/darkroom/masks/ellipse/radius_a", radius_a);
        dt_conf_set_float("plugins/darkroom/masks/ellipse/radius_b", radius_b);
      }
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
    if((state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK && !((state & GDK_SHIFT_MASK) == GDK_SHIFT_MASK))
    {
      // we try to change the opacity
      dt_masks_form_change_opacity(form, parentid, up);
    }
    else
    {
      dt_masks_point_ellipse_t *ellipse = (dt_masks_point_ellipse_t *)(g_list_first(form->points)->data);
      if((state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK)) == (GDK_SHIFT_MASK | GDK_CONTROL_MASK)
         && gui->edit_mode == DT_MASKS_EDIT_FULL)
      {
        // we try to change the rotation
        if(up)
          ellipse->rotation -= 10.f;
        else
          ellipse->rotation += 10.f;
        ellipse->rotation = fmodf(ellipse->rotation, 360.0f);

        dt_masks_write_form(form, darktable.develop);
        dt_masks_gui_form_remove(form, gui, index);
        dt_masks_gui_form_create(form, gui, index);
        if(form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE))
          dt_conf_set_float("plugins/darkroom/spots/ellipse_rotation", ellipse->rotation);
        else
          dt_conf_set_float("plugins/darkroom/masks/ellipse/rotation", ellipse->rotation);
      }
      // resize don't care where the mouse is inside a shape
      if((state & GDK_SHIFT_MASK) == GDK_SHIFT_MASK)
      {
        const float reference = (ellipse->flags & DT_MASKS_ELLIPSE_PROPORTIONAL ? 1.0f/fmin(ellipse->radius[0], ellipse->radius[1]) : 1.0f);
        if(up && ellipse->border > 0.001f * reference)
          ellipse->border *= 0.97f;
        else if(!up && ellipse->border < 1.0f * reference)
          ellipse->border *= 1.0f/0.97f;
        else return 1;
        ellipse->border = CLAMP(ellipse->border, 0.001f * reference, reference);
        dt_masks_write_form(form, darktable.develop);
        dt_masks_gui_form_remove(form, gui, index);
        dt_masks_gui_form_create(form, gui, index);
        if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
          dt_conf_set_float("plugins/darkroom/spots/ellipse_border", ellipse->border);
        else
          dt_conf_set_float("plugins/darkroom/masks/ellipse/border", ellipse->border);
      }
      else if(gui->edit_mode == DT_MASKS_EDIT_FULL)
      {
        const float oldradius = ellipse->radius[0];

        if(up && ellipse->radius[0] > 0.001f)
          ellipse->radius[0] *= 0.97f;
        else if(!up && ellipse->radius[0] < 1.0f)
          ellipse->radius[0] *= 1.0f / 0.97f;
        else return 1;

        ellipse->radius[0] = CLAMP(ellipse->radius[0], 0.001f, 1.0f);

        const float factor = ellipse->radius[0] / oldradius;
        ellipse->radius[1] *= factor;

        dt_masks_write_form(form, darktable.develop);
        dt_masks_gui_form_remove(form, gui, index);
        dt_masks_gui_form_create(form, gui, index);
        if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
        {
          dt_conf_set_float("plugins/darkroom/spots/ellipse_radius_a", ellipse->radius[0]);
          dt_conf_set_float("plugins/darkroom/spots/ellipse_radius_b", ellipse->radius[1]);
        }
        else
        {
          dt_conf_set_float("plugins/darkroom/masks/ellipse/radius_a", ellipse->radius[0]);
          dt_conf_set_float("plugins/darkroom/masks/ellipse/radius_b", ellipse->radius[1]);
        }
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

static int dt_ellipse_events_button_pressed(struct dt_iop_module_t *module, float pzx, float pzy,
                                            double pressure, int which, int type, uint32_t state,
                                            dt_masks_form_t *form, int parentid, dt_masks_form_gui_t *gui,
                                            int index)
{
  if(!gui) return 0;
  if(gui->source_selected && !gui->creation && gui->edit_mode == DT_MASKS_EDIT_FULL)
  {
    dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
    if(!gpt) return 0;
    // we start the source dragging
    gui->source_dragging = TRUE;
    gui->dx = gpt->source[0] - gui->posx;
    gui->dy = gpt->source[1] - gui->posy;
    return 1;
  }
  else if(gui->point_selected >= 1 && !gui->creation && gui->edit_mode == DT_MASKS_EDIT_FULL
          && !((state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK))
  {
    dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
    if(!gpt) return 0;
    gui->point_dragging = gui->point_selected;
    gui->dx = gpt->points[0] - gui->posx;
    gui->dy = gpt->points[1] - gui->posy;
    return 1;
  }
  else if(gui->form_selected && !gui->creation && gui->edit_mode == DT_MASKS_EDIT_FULL
          && !((state & GDK_SHIFT_MASK) == GDK_SHIFT_MASK))
  {
    dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
    if(!gpt) return 0;
    // we start the form dragging or rotating
    if((state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK)
      gui->form_rotating = TRUE;
    else
      gui->form_dragging = TRUE;
    gui->dx = gpt->points[0] - gui->posx;
    gui->dy = gpt->points[1] - gui->posy;
    return 1;
  }
  else if(gui->form_selected && !gui->creation && ((state & GDK_SHIFT_MASK) == GDK_SHIFT_MASK))
  {
    dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
    if(!gpt) return 0;

    gui->border_toggling = TRUE;

    return 1;
  }
  else if(gui->creation && (which == 3))
  {
    gui->creation_continuous = FALSE;
    gui->creation_continuous_module = NULL;
    dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
    dt_masks_iop_update(module);
    dt_control_queue_redraw_center();
    return 1;
  }
  else if(gui->creation && which == 1
          && (((state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) == (GDK_CONTROL_MASK | GDK_SHIFT_MASK))
              || ((state & GDK_SHIFT_MASK) == GDK_SHIFT_MASK)))
  {
    // set some absolute or relative position for the source of the clone mask
    if(form->type & DT_MASKS_CLONE) dt_masks_set_source_pos_initial_state(gui, state, pzx, pzy);

    return 1;
  }
  else if(gui->creation)
  {
    dt_iop_module_t *crea_module = gui->creation_module;
    // we create the ellipse
    dt_masks_point_ellipse_t *ellipse
        = (dt_masks_point_ellipse_t *)(malloc(sizeof(dt_masks_point_ellipse_t)));

    // we change the center value
    float wd = darktable.develop->preview_pipe->backbuf_width;
    float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = { pzx * wd, pzy * ht };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);
    ellipse->center[0] = pts[0] / darktable.develop->preview_pipe->iwidth;
    ellipse->center[1] = pts[1] / darktable.develop->preview_pipe->iheight;

    if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
    {
      const float a = dt_conf_get_float("plugins/darkroom/spots/ellipse_radius_a");
      const float b = dt_conf_get_float("plugins/darkroom/spots/ellipse_radius_b");
      const float ellipse_border = dt_conf_get_float("plugins/darkroom/spots/ellipse_border");
      const float rotation = dt_conf_get_float("plugins/darkroom/spots/ellipse_rotation");
      const int flags = dt_conf_get_int("plugins/darkroom/spots/ellipse_flags");
      ellipse->radius[0] = MAX(0.01f, MIN(0.5f, a));
      ellipse->radius[1] = MAX(0.01f, MIN(0.5f, b));
      ellipse->flags = flags;
      ellipse->rotation = rotation;
      const float min_radius = fmin(ellipse->radius[0], ellipse->radius[1]);
      const float reference = (ellipse->flags & DT_MASKS_ELLIPSE_PROPORTIONAL ? 1.0f/min_radius : 1.0f);
      ellipse->border = MAX(0.005f * reference, MIN(0.5f * reference, ellipse_border));
      if(form->type & DT_MASKS_CLONE)
      {
        dt_ellipse_set_source_pos_initial_value(gui, form, ellipse);
      }
      else
      {
        // not used for regular masks
        form->source[0] = form->source[1] = 0.0f;
      }
    }
    else
    {
      const float a = dt_conf_get_float("plugins/darkroom/masks/ellipse/radius_a");
      const float b = dt_conf_get_float("plugins/darkroom/masks/ellipse/radius_b");
      const float ellipse_border = dt_conf_get_float("plugins/darkroom/masks/ellipse/border");
      const float rotation = dt_conf_get_float("plugins/darkroom/masks/ellipse/rotation");
      const int flags = dt_conf_get_int("plugins/darkroom/masks/ellipse/flags");
      ellipse->radius[0] = MAX(0.01f, MIN(0.5f, a));
      ellipse->radius[1] = MAX(0.01f, MIN(0.5f, b));
      ellipse->flags = flags;
      ellipse->rotation = rotation;
      const float min_radius = fmin(ellipse->radius[0], ellipse->radius[1]);
      const float reference = (ellipse->flags & DT_MASKS_ELLIPSE_PROPORTIONAL ? 1.0f/min_radius : 1.0f);
      ellipse->border = MAX(0.005f * reference, MIN(0.5f * reference, ellipse_border));
      // not used for masks
      form->source[0] = form->source[1] = 0.0f;
    }
    form->points = g_list_append(form->points, ellipse);
    dt_masks_gui_form_save_creation(darktable.develop, crea_module, form, gui);

    if(crea_module)
    {
      // we save the move
      dt_dev_add_history_item(darktable.develop, crea_module, TRUE);
      // and we switch in edit mode to show all the forms
      if(gui->creation_continuous)
        dt_masks_set_edit_mode_single_form(crea_module, form->formid, DT_MASKS_EDIT_FULL);
      else
        dt_masks_set_edit_mode(crea_module, DT_MASKS_EDIT_FULL);
      dt_masks_iop_update(crea_module);
      gui->creation_module = NULL;
    }
    else
    {
      // we select the new form
      dt_dev_masks_selection_change(darktable.develop, form->formid, TRUE);
    }

    // if we draw a clone ellipse, we start now the source dragging
    if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
    {
      dt_masks_form_t *grp = darktable.develop->form_visible;
      if(!grp || !(grp->type & DT_MASKS_GROUP)) return 1;
      int pos3 = 0, pos2 = -1;
      GList *fs = g_list_first(grp->points);
      while(fs)
      {
        dt_masks_point_group_t *pt = (dt_masks_point_group_t *)fs->data;
        if(pt->formid == form->formid)
        {
          pos2 = pos3;
          break;
        }
        pos3++;
        fs = g_list_next(fs);
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
    return 1;
  }
  return 0;
}

static int dt_ellipse_events_button_released(struct dt_iop_module_t *module, float pzx, float pzy, int which,
                                             uint32_t state, dt_masks_form_t *form, int parentid,
                                             dt_masks_form_gui_t *gui, int index)
{
  if(which == 3 && parentid > 0 && gui->edit_mode == DT_MASKS_EDIT_FULL)
  {
    // we hide the form
    if(!(darktable.develop->form_visible->type & DT_MASKS_GROUP))
      dt_masks_change_form_gui(NULL);
    else if(g_list_length(darktable.develop->form_visible->points) < 2)
      dt_masks_change_form_gui(NULL);
    else
    {
      dt_masks_clear_form_gui(darktable.develop);
      GList *forms = g_list_first(darktable.develop->form_visible->points);
      while(forms)
      {
        dt_masks_point_group_t *gpt = (dt_masks_point_group_t *)forms->data;
        if(gpt->formid == form->formid)
        {
          darktable.develop->form_visible->points
              = g_list_remove(darktable.develop->form_visible->points, gpt);
          free(gpt);
          break;
        }
        forms = g_list_next(forms);
      }
      gui->edit_mode = DT_MASKS_EDIT_FULL;
    }

    // we remove the shape
    dt_dev_masks_list_remove(darktable.develop, form->formid, parentid);
    dt_masks_form_remove(module, dt_masks_get_from_id(darktable.develop, parentid), form);
    return 1;
  }
  if(gui->form_dragging)
  {
    // we get the ellipse
    dt_masks_point_ellipse_t *ellipse = (dt_masks_point_ellipse_t *)(g_list_first(form->points)->data);

    // we end the form dragging
    gui->form_dragging = FALSE;

    // we change the center value
    float wd = darktable.develop->preview_pipe->backbuf_width;
    float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = { pzx * wd + gui->dx, pzy * ht + gui->dy };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);
    ellipse->center[0] = pts[0] / darktable.develop->preview_pipe->iwidth;
    ellipse->center[1] = pts[1] / darktable.develop->preview_pipe->iheight;
    dt_masks_write_form(form, darktable.develop);

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index);

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
    dt_masks_point_ellipse_t *ellipse = (dt_masks_point_ellipse_t *)(g_list_first(form->points)->data);

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

    if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
    {
      dt_conf_set_int("plugins/darkroom/spots/ellipse_flags", ellipse->flags);
      dt_conf_set_float("plugins/darkroom/spots/ellipse_border", ellipse->border);
    }
    else
    {
      dt_conf_set_int("plugins/darkroom/masks/ellipse/flags", ellipse->flags);
      dt_conf_set_float("plugins/darkroom/masks/ellipse/border", ellipse->border);
    }

    dt_masks_write_form(form, darktable.develop);

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index);

    // we save the new parameters
    dt_masks_update_image(darktable.develop);

    return 1;
  }
  else if(gui->form_rotating && gui->edit_mode == DT_MASKS_EDIT_FULL)
  {
    // we get the ellipse
    dt_masks_point_ellipse_t *ellipse = (dt_masks_point_ellipse_t *)(g_list_first(form->points)->data);

    // we end the form rotating
    gui->form_rotating = FALSE;

    float wd = darktable.develop->preview_pipe->backbuf_width;
    float ht = darktable.develop->preview_pipe->backbuf_height;
    float x = pzx * wd;
    float y = pzy * ht;

    // we need the reference point
    dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
    if(!gpt) return 0;
    float xref = gpt->points[0];
    float yref = gpt->points[1];

    float dv = atan2(y - yref, x - xref) - atan2(-gui->dy, -gui->dx);

    ellipse->rotation += dv / M_PI * 180.0f;
    ellipse->rotation = fmodf(ellipse->rotation, 360.0f);

    if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
      dt_conf_set_float("plugins/darkroom/spots/ellipse_rotation", ellipse->rotation);
    else
      dt_conf_set_float("plugins/darkroom/masks/ellipse/rotation", ellipse->rotation);

    dt_masks_write_form(form, darktable.develop);

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index);

    // we save the rotation
    dt_masks_update_image(darktable.develop);

    return 1;
  }
  else if(gui->point_dragging >= 1 && gui->edit_mode == DT_MASKS_EDIT_FULL)
  {
    // we get the ellipse
    dt_masks_point_ellipse_t *ellipse = (dt_masks_point_ellipse_t *)(g_list_first(form->points)->data);

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
      if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
        dt_conf_set_float("plugins/darkroom/spots/ellipse_radius_a", ellipse->radius[0]);
      else
        dt_conf_set_float("plugins/darkroom/masks/ellipse/radius_a", ellipse->radius[0]);
    }
    else
    {
      ellipse->radius[1] = MAX(0.002f, ellipse->radius[1] * s);
      if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
        dt_conf_set_float("plugins/darkroom/spots/ellipse_radius_b", ellipse->radius[1]);
      else
        dt_conf_set_float("plugins/darkroom/masks/ellipse/radius_b", ellipse->radius[1]);
    }

    dt_masks_write_form(form, darktable.develop);
    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index);

    // we save the rotation
    dt_masks_update_image(darktable.develop);

    return 1;
  }
  else if(gui->source_dragging)
  {
    // we end the form dragging
    gui->source_dragging = FALSE;
    if(gui->scrollx != 0.0 || gui->scrolly != 0.0)
    {
      // if there's no dragging the source is calculated in dt_ellipse_events_button_pressed()
    }
    else
    {
      // we change the center value
      float wd = darktable.develop->preview_pipe->backbuf_width;
      float ht = darktable.develop->preview_pipe->backbuf_height;
      float pts[2] = { pzx * wd + gui->dx, pzy * ht + gui->dy };

      dt_dev_distort_backtransform(darktable.develop, pts, 1);

      form->source[0] = pts[0] / darktable.develop->preview_pipe->iwidth;
      form->source[1] = pts[1] / darktable.develop->preview_pipe->iheight;
    }
    dt_masks_write_form(form, darktable.develop);

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index);

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
  return 0;
}

static int dt_ellipse_events_mouse_moved(struct dt_iop_module_t *module, float pzx, float pzy,
                                         double pressure, int which, dt_masks_form_t *form, int parentid,
                                         dt_masks_form_gui_t *gui, int index)
{
  if(gui->form_dragging || gui->form_rotating || gui->source_dragging || gui->point_dragging >= 1)
  {
    dt_control_queue_redraw_center();
    return 1;
  }
  else if(!gui->creation)
  {
    dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
    int closeup = dt_control_get_dev_closeup();
    float zoom_scale = dt_dev_get_zoom_scale(darktable.develop, zoom, 1<<closeup, 1);
    float as = 0.005f / zoom_scale * darktable.develop->preview_pipe->backbuf_width;
    float x = pzx * darktable.develop->preview_pipe->backbuf_width;
    float y = pzy * darktable.develop->preview_pipe->backbuf_height;
    int in = 0, inb = 0, near = 0, ins = 0; // FIXME gcc7 false-positive
    dt_ellipse_get_distance(pzx * darktable.develop->preview_pipe->backbuf_width,
                            pzy * darktable.develop->preview_pipe->backbuf_height, as, gui, index, &in, &inb,
                            &near, &ins);
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
    if(gui->form_selected)
    {
      dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
      for(int i = 1; i < 5; i++)
      {
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

static void dt_ellipse_events_post_expose(cairo_t *cr, float zoom_scale, dt_masks_form_gui_t *gui, int index)
{
  double dashed[] = { 4.0, 4.0 };
  dashed[0] /= zoom_scale;
  dashed[1] /= zoom_scale;
  int len = sizeof(dashed) / sizeof(dashed[0]);
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);

  float dx = 0.0f, dy = 0.0f, xref = 0.0f, yref = 0.0f;
  float dxs = 0.0f, dys = 0.0f, xrefs = 0.0f, yrefs = 0.0f;
  float sinv = 0.0f, cosv = 1.0f;
  float scalea = 1.0f, scaleb = 1.0f, scaleab = 1.0f, scalebb = 1.0f;

  // add a preview when creating an ellipse
  // in creation mode
  if(gui->creation)
  {
    if(gui->guipoints_count == 0)
    {
      dt_masks_form_t *form = darktable.develop->form_visible;
      if(!form) return;

      float x, y;
      float masks_border;
      int flags;
      float radius_a;
      float radius_b;
      float rotation;

      if(form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE))
      {
        masks_border = dt_conf_get_float("plugins/darkroom/spots/ellipse_border");
        flags = dt_conf_get_int("plugins/darkroom/spots/ellipse_flags");
        radius_a = dt_conf_get_float("plugins/darkroom/spots/ellipse_radius_a");
        radius_b = dt_conf_get_float("plugins/darkroom/spots/ellipse_radius_b");
        rotation = dt_conf_get_float("plugins/darkroom/spots/ellipse_rotation");
      }
      else
      {
        masks_border = dt_conf_get_float("plugins/darkroom/masks/ellipse/border");
        flags = dt_conf_get_int("plugins/darkroom/masks/ellipse/flags");
        radius_a = dt_conf_get_float("plugins/darkroom/masks/ellipse/radius_a");
        radius_b = dt_conf_get_float("plugins/darkroom/masks/ellipse/radius_b");
        rotation = dt_conf_get_float("plugins/darkroom/masks/ellipse/rotation");
      }

      float pzx = gui->posx;
      float pzy = gui->posy;

      if(pzx == -1.f && pzy == -1.f)
      {
        float zoom_x, zoom_y;
        zoom_y = dt_control_get_dev_zoom_y();
        zoom_x = dt_control_get_dev_zoom_x();
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

      draw = dt_ellipse_get_points(darktable.develop, x, y, radius_a, radius_b, rotation, &points, &points_count);
      if(draw && masks_border > 0.f)
      {
        draw = dt_ellipse_get_points(
            darktable.develop, x, y,
            (flags & DT_MASKS_ELLIPSE_PROPORTIONAL ? radius_a * (1.0f + masks_border) : radius_a + masks_border),
            (flags & DT_MASKS_ELLIPSE_PROPORTIONAL ? radius_b * (1.0f + masks_border) : radius_b + masks_border),
            rotation, &border, &border_count);
      }

      if(draw && points_count >= 2)
      {
        xref = points[0];
        yref = points[1];

        dt_ellipse_draw_shape(cr, dashed, 0, zoom_scale, dx, dy, xref, yref, sinv, cosv, scalea, scaleb, points,
                              points_count);
      }
      if(draw && border_count >= 2)
      {
        xref = border[0];
        yref = border[1];

        dt_ellipse_draw_border(cr, dashed, len, 0, zoom_scale, dx, dy, xref, yref, sinv, cosv, scaleab, scalebb,
                               border, border_count);
      }

      // draw a cross where the source will be created
      if(form->type & DT_MASKS_CLONE)
      {
        float x = 0.f, y = 0.f;
        dt_masks_calculate_source_pos_value(gui, DT_MASKS_ELLIPSE, pzx, pzy, pzx, pzy, &x, &y, FALSE);
        dt_masks_draw_clone_source_pos(cr, zoom_scale, x, y);
      }

      if(points) free(points);
      if(border) free(border);
    }
    return;
  }

  if(!gpt) return;

  const float r = atan2(gpt->points[3] - gpt->points[1], gpt->points[2] - gpt->points[0]);
  const float sinr = sin(r);
  const float cosr = cos(r);

  xref = gpt->points[0];
  yref = gpt->points[1];

  if(gpt->source_count > 10)
  {
    xrefs = gpt->source[0];
    yrefs = gpt->source[1];
  }
  if((gui->group_selected == index) && gui->form_dragging)
  {
    dx = gui->posx + gui->dx - xref;
    dy = gui->posy + gui->dy - yref;
  }
  else if((gui->group_selected == index) && gui->source_dragging)
  {
    xrefs = gpt->source[0], yrefs = gpt->source[1];
    dxs = gui->posx + gui->dx - xrefs;
    dys = gui->posy + gui->dy - yrefs;
  }
  else if((gui->group_selected == index) && gui->form_rotating)
  {
    const float v = atan2(gui->posy - yref, gui->posx - xref) - atan2(-gui->dy, -gui->dx);
    sinv = sin(v);
    cosv = cos(v);
  }
  else if((gui->group_selected == index) && (gui->point_dragging >= 1))
  {
    const int k = gui->point_dragging;
    const float rx = gpt->points[k * 2] - xref;
    const float ry = gpt->points[k * 2 + 1] - yref;
    const float bx = gpt->border[k * 2] - xref;
    const float by = gpt->border[k * 2 + 1] - yref;
    const float deltax = gui->posx + gui->dx - xref;
    const float deltay = gui->posy + gui->dy - yref;

    const float r = sqrtf(rx * rx + ry * ry);
    const float b = sqrtf(bx * bx + by * by);
    float d = (rx * deltax + ry * deltay) / r;
    if(r + d < 0) d = -r;

    if(k == 1 || k == 2)
    {
      scalea = r > 0 ? (r + d) / r : 0;
      scaleab = b > 0 ? (b + d) / b : 0;
    }
    else
    {
      scaleb = r > 0 ? (r + d) / r : 0;
      scalebb = b > 0 ? (b + d) / b : 0;
    }
  }

  float x, y;

  // draw shape
  dt_ellipse_draw_shape(cr, dashed, 0, zoom_scale, dx, dy, xref, yref, sinv, cosv, scalea, scaleb, gpt->points,
                        gpt->points_count);

  // draw anchor points
  if(TRUE)
  {
    cairo_set_dash(cr, dashed, 0, 0);
    float anchor_size; // = (gui->form_dragging || gui->form_selected) ? 7.0f / zoom_scale : 5.0f /
                       // zoom_scale;

    for(int i = 1; i < 5; i++)
    {
      cairo_set_source_rgba(cr, .8, .8, .8, .8);

      if(i == gui->point_dragging || i == gui->point_selected)
        anchor_size = 7.0f / zoom_scale;
      else
        anchor_size = 5.0f / zoom_scale;

      _ellipse_point_transform(xref, yref, gpt->points[i * 2] + dx, gpt->points[i * 2 + 1] + dy, sinr, cosr,
                               scalea, scaleb, sinv, cosv, &x, &y);
      cairo_rectangle(cr, x - (anchor_size * 0.5), y - (anchor_size * 0.5), anchor_size, anchor_size);
      cairo_fill_preserve(cr);
      if((gui->group_selected == index) && (i == gui->point_dragging || i == gui->point_selected))
        cairo_set_line_width(cr, 2.0 / zoom_scale);
      if((gui->group_selected == index) && (gui->form_dragging || gui->form_selected))
        cairo_set_line_width(cr, 2.0 / zoom_scale);
      else
        cairo_set_line_width(cr, 1.0 / zoom_scale);
      cairo_set_source_rgba(cr, .3, .3, .3, .8);
      cairo_stroke(cr);
    }
  }

  // draw border
  if(gui->group_selected == index)
  {
    dt_ellipse_draw_border(cr, dashed, len, 0, zoom_scale, dx, dy, xref, yref, sinv, cosv, scaleab, scalebb,
                           gpt->border, gpt->border_count);
  }

  // draw the source if any
  if(gpt->source_count > 10)
  {
    // compute the dest inner ellipse intersection with the line from source center to dest center.
    float cdx = gpt->source[0] + dxs - gpt->points[0] - dx;
    float cdy = gpt->source[1] + dys - gpt->points[1] - dy;

    // we don't draw the line if source==point
    if(cdx != 0.0 && cdy != 0.0)
    {
      cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
      float cangle = atan(cdx / cdy);

      if(cdy > 0)
        cangle = (M_PI / 2) - cangle;
      else
        cangle = -(M_PI / 2) - cangle;

      float arrowx = gpt->points[0] + dx;
      float arrowy = gpt->points[1] + dy;

      cairo_move_to(cr, gpt->source[0] + dxs, gpt->source[1] + dys); // source center
      cairo_line_to(cr, arrowx, arrowy);                             // dest border
      // then draw to line for the arrow itself
      const float arrow_scale = 8.0;
      cairo_move_to(cr, arrowx + arrow_scale * cos(cangle + (0.4)),
                    arrowy + arrow_scale * sin(cangle + (0.4)));
      cairo_line_to(cr, arrowx, arrowy);
      cairo_line_to(cr, arrowx + arrow_scale * cos(cangle - (0.4)),
                    arrowy + arrow_scale * sin(cangle - (0.4)));

      cairo_set_dash(cr, dashed, 0, 0);
      if((gui->group_selected == index) && (gui->form_selected || gui->form_dragging))
        cairo_set_line_width(cr, 2.5 / zoom_scale);
      else
        cairo_set_line_width(cr, 1.5 / zoom_scale);
      cairo_set_source_rgba(cr, .3, .3, .3, .8);
      cairo_stroke_preserve(cr);
      if((gui->group_selected == index) && (gui->form_selected || gui->form_dragging))
        cairo_set_line_width(cr, 1.0 / zoom_scale);
      else
        cairo_set_line_width(cr, 0.5 / zoom_scale);
      cairo_set_source_rgba(cr, .8, .8, .8, .8);
      cairo_stroke(cr);
    }

    // we draw the source
    cairo_set_dash(cr, dashed, 0, 0);
    if((gui->group_selected == index) && (gui->form_selected || gui->form_dragging))
      cairo_set_line_width(cr, 2.5 / zoom_scale);
    else
      cairo_set_line_width(cr, 1.5 / zoom_scale);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    _ellipse_point_transform(xrefs, yrefs, gpt->source[10] + dxs, gpt->source[11] + dys, sinr, cosr, scalea,
                             scaleb, sinv, cosv, &x, &y);
    cairo_move_to(cr, x, y);
    for(int i = 6; i < gpt->source_count; i++)
    {
      _ellipse_point_transform(xrefs, yrefs, gpt->source[i * 2] + dxs, gpt->source[i * 2 + 1] + dys, sinr,
                               cosr, scalea, scaleb, sinv, cosv, &x, &y);
      cairo_line_to(cr, x, y);
    }
    _ellipse_point_transform(xrefs, yrefs, gpt->source[10] + dxs, gpt->source[11] + dys, sinr, cosr, scalea,
                             scaleb, sinv, cosv, &x, &y);
    cairo_line_to(cr, x, y);
    cairo_stroke_preserve(cr);
    if((gui->group_selected == index) && (gui->form_selected || gui->form_dragging))
      cairo_set_line_width(cr, 1.0 / zoom_scale);
    else
      cairo_set_line_width(cr, 0.5 / zoom_scale);
    cairo_set_source_rgba(cr, .8, .8, .8, .8);
    cairo_stroke(cr);
  }
}

static int dt_ellipse_get_source_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                      dt_masks_form_t *form, int *width, int *height, int *posx, int *posy)
{
  // we get the ellipse values
  dt_masks_point_ellipse_t *ellipse = (dt_masks_point_ellipse_t *)(g_list_first(form->points)->data);
  const float wd = piece->pipe->iwidth, ht = piece->pipe->iheight;

  const float total[2] = { (ellipse->flags & DT_MASKS_ELLIPSE_PROPORTIONAL ? ellipse->radius[0] * (1.0f + ellipse->border) : ellipse->radius[0] + ellipse->border) * MIN(wd, ht),
                           (ellipse->flags & DT_MASKS_ELLIPSE_PROPORTIONAL ? ellipse->radius[1] * (1.0f + ellipse->border) : ellipse->radius[1] + ellipse->border) * MIN(wd, ht) };
  const float v1 = ((ellipse->rotation) / 180.0f) * M_PI;
  const float v2 = ((ellipse->rotation - 90.0f) / 180.0f) * M_PI;
  float a, b, v;

  if(total[0] >= total[1])
  {
    a = total[0];
    b = total[1];
    v = v1;
  }
  else
  {
    a = total[1];
    b = total[0];
    v = v2;
  }

  const float sinv = sinf(v);
  const float cosv = cosf(v);

  // how many points do we need ?
  const float lambda = (a - b) / (a + b);
  const int l = (int)(M_PI * (a + b)
                      * (1.0f + (3.0f * lambda * lambda) / (10.0f + sqrtf(4.0f - 3.0f * lambda * lambda))));

  // buffer allocations
  float *points = calloc(2 * (l + 5), sizeof(float));

  // now we set the points
  const float x = points[0] = ellipse->center[0] * wd;
  const float y = points[1] = ellipse->center[1] * ht;

  points[2] = x + a * cos(v);
  points[3] = y + a * sin(v);
  points[4] = x - a * cos(v);
  points[5] = y - a * sin(v);

  points[6] = x + b * cos(v - M_PI / 2.0f);
  points[7] = y + b * sin(v - M_PI / 2.0f);
  points[8] = x - b * cos(v - M_PI / 2.0f);
  points[9] = y - b * sin(v - M_PI / 2.0f);

  for(int i = 1; i < l + 5; i++)
  {
    float alpha = (i - 5) * 2.0 * M_PI / (float)l;
    points[i * 2] = points[0] + a * cosf(alpha) * cosv - b * sinf(alpha) * sinv;
    points[i * 2 + 1] = points[1] + a * cosf(alpha) * sinv + b * sinf(alpha) * cosv;
  }

  // and we transform them with all distorted modules
  if(!dt_dev_distort_transform_plus(darktable.develop, piece->pipe, 0, module->priority, points, l + 5))
  {
    free(points);
    return 0;
  }

  // now we search min and max
  float xmin, xmax, ymin, ymax;
  xmin = ymin = FLT_MAX;
  xmax = ymax = FLT_MIN;
  for(int i = 1; i < l + 5; i++)
  {
    xmin = fminf(points[i * 2], xmin);
    xmax = fmaxf(points[i * 2], xmax);
    ymin = fminf(points[i * 2 + 1], ymin);
    ymax = fmaxf(points[i * 2 + 1], ymax);
  }
  free(points);
  // and we set values
  *posx = xmin;
  *posy = ymin;
  *width = (xmax - xmin);
  *height = (ymax - ymin);
  return 1;
}

static int dt_ellipse_get_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                               int *width, int *height, int *posx, int *posy)
{
  // we get the ellipse values
  dt_masks_point_ellipse_t *ellipse = (dt_masks_point_ellipse_t *)(g_list_first(form->points)->data);

  const float wd = piece->pipe->iwidth, ht = piece->pipe->iheight;

  const float total[2] = { (ellipse->flags & DT_MASKS_ELLIPSE_PROPORTIONAL ? ellipse->radius[0] * (1.0f + ellipse->border) : ellipse->radius[0] + ellipse->border) * MIN(wd, ht),
                           (ellipse->flags & DT_MASKS_ELLIPSE_PROPORTIONAL ? ellipse->radius[1] * (1.0f + ellipse->border) : ellipse->radius[1] + ellipse->border) * MIN(wd, ht) };
  const float v1 = ((ellipse->rotation) / 180.0f) * M_PI;
  const float v2 = ((ellipse->rotation - 90.0f) / 180.0f) * M_PI;
  float a, b, v;

  if(total[0] >= total[1])
  {
    a = total[0];
    b = total[1];
    v = v1;
  }
  else
  {
    a = total[1];
    b = total[0];
    v = v2;
  }

  const float sinv = sinf(v);
  const float cosv = cosf(v);

  // how many points do we need ?
  const float lambda = (a - b) / (a + b);
  const int l = (int)(M_PI * (a + b)
                      * (1.0f + (3.0f * lambda * lambda) / (10.0f + sqrtf(4.0f - 3.0f * lambda * lambda))));

  // buffer allocations
  float *points = calloc(2 * (l + 5), sizeof(float));

  // now we set the points
  const float x = points[0] = ellipse->center[0] * wd;
  const float y = points[1] = ellipse->center[1] * ht;

  points[2] = x + a * cos(v);
  points[3] = y + a * sin(v);
  points[4] = x - a * cos(v);
  points[5] = y - a * sin(v);

  points[6] = x + b * cos(v - M_PI / 2.0f);
  points[7] = y + b * sin(v - M_PI / 2.0f);
  points[8] = x - b * cos(v - M_PI / 2.0f);
  points[9] = y - b * sin(v - M_PI / 2.0f);

  for(int i = 5; i < l + 5; i++)
  {
    float alpha = (i - 5) * 2.0 * M_PI / (float)l;
    points[i * 2] = x + a * cosf(alpha) * cosv - b * sinf(alpha) * sinv;
    points[i * 2 + 1] = y + a * cosf(alpha) * sinv + b * sinf(alpha) * cosv;
  }

  // and we transform them with all distorted modules
  if(!dt_dev_distort_transform_plus(module->dev, piece->pipe, 0, module->priority, points, l + 5))
  {
    free(points);
    return 0;
  }

  // now we search min and max
  float xmin, xmax, ymin, ymax;
  xmin = ymin = FLT_MAX;
  xmax = ymax = FLT_MIN;
  for(int i = 5; i < l + 5; i++)
  {
    xmin = fminf(points[i * 2], xmin);
    xmax = fmaxf(points[i * 2], xmax);
    ymin = fminf(points[i * 2 + 1], ymin);
    ymax = fmaxf(points[i * 2 + 1], ymax);
  }
  free(points);

  // and we set values
  *posx = xmin;
  *posy = ymin;
  *width = (xmax - xmin);
  *height = (ymax - ymin);
  return 1;
}

static int dt_ellipse_get_mask(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                               float **buffer, int *width, int *height, int *posx, int *posy)
{
  double start2 = dt_get_wtime();

  // we get the area
  if(!dt_ellipse_get_area(module, piece, form, width, height, posx, posy)) return 0;

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] ellipse area took %0.04f sec\n", form->name, dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  // we get the ellipse values
  dt_masks_point_ellipse_t *ellipse = (dt_masks_point_ellipse_t *)(g_list_first(form->points)->data);

  // we create a buffer of points with all points in the area
  int w = *width, h = *height;
  float *points = malloc(w * h * 2 * sizeof(float));
  for(int i = 0; i < h; i++)
    for(int j = 0; j < w; j++)
    {
      points[(i * w + j) * 2] = (j + (*posx));
      points[(i * w + j) * 2 + 1] = (i + (*posy));
    }

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] ellipse draw took %0.04f sec\n", form->name, dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  // we back transform all this points
  if(!dt_dev_distort_backtransform_plus(module->dev, piece->pipe, 0, module->priority, points, w * h))
  {
    free(points);
    return 0;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] ellipse transform took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  // we allocate the buffer
  *buffer = calloc(w * h, sizeof(float));

  // we populate the buffer
  const int wi = piece->pipe->iwidth, hi = piece->pipe->iheight;
  const float center[2] = { ellipse->center[0] * wi, ellipse->center[1] * hi };
  const float radius[2] = { ellipse->radius[0] * MIN(wi, hi), ellipse->radius[1] * MIN(wi, hi) };
  const float total[2] =  { (ellipse->flags & DT_MASKS_ELLIPSE_PROPORTIONAL ? ellipse->radius[0] * (1.0f + ellipse->border) : ellipse->radius[0] + ellipse->border) * MIN(wi, hi),
                            (ellipse->flags & DT_MASKS_ELLIPSE_PROPORTIONAL ? ellipse->radius[1] * (1.0f + ellipse->border) : ellipse->radius[1] + ellipse->border) * MIN(wi, hi) };

  float a, b, ta, tb, alpha;

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

  for(int i = 0; i < h; i++)
    for(int j = 0; j < w; j++)
    {
      float x = points[(i * w + j) * 2] - center[0];
      float y = points[(i * w + j) * 2 + 1] - center[1];
      float v = atan2(y, x) - alpha;
      float cosv = cos(v);
      float sinv = sin(v);
      float radius2 = a * a * b * b / (a * a * sinv * sinv + b * b * cosv * cosv);
      float total2 = ta * ta * tb * tb / (ta * ta * sinv * sinv + tb * tb * cosv * cosv);
      float l2 = x * x + y * y;

      if(l2 < radius2)
        (*buffer)[i * w + j] = 1.0f;
      else if(l2 < total2)
      {
        float f = (total2 - l2) / (total2 - radius2);
        (*buffer)[i * w + j] = f * f;
      }
      else
        (*buffer)[i * w + j] = 0.0f;
    }
  free(points);

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] ellipse fill took %0.04f sec\n", form->name, dt_get_wtime() - start2);
//   start2 = dt_get_wtime();

  return 1;
}


static int dt_ellipse_get_mask_roi(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                   dt_masks_form_t *form, const dt_iop_roi_t *roi, float *buffer)
{
  double start2 = dt_get_wtime();

  // we get the ellipse values
  dt_masks_point_ellipse_t *ellipse = (dt_masks_point_ellipse_t *)(g_list_first(form->points)->data);

  // we create a buffer of mesh points for later interpolation. mainly in order to reduce memory footprint
  const int w = roi->width;
  const int h = roi->height;
  const int px = roi->x;
  const int py = roi->y;
  const float iscale = 1.0f / roi->scale;
  const int mesh = 4;
  const int mw = (w + mesh - 1) / mesh + 1;
  const int mh = (h + mesh - 1) / mesh + 1;

  float *points = malloc((size_t)mw * mh * 2 * sizeof(float));
  if(points == NULL) return 0;

#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) shared(points)
#else
#pragma omp parallel for shared(points)
#endif
#endif
  for(int j = 0; j < mh; j++)
    for(int i = 0; i < mw; i++)
    {
      size_t index = (size_t)j * mw + i;
      points[index * 2] = (mesh * i + px) * iscale;
      points[index * 2 + 1] = (mesh * j + py) * iscale;
    }

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] ellipse draw took %0.04f sec\n", form->name, dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  // we back transform all these points
  if(!dt_dev_distort_backtransform_plus(module->dev, piece->pipe, 0, module->priority, points,
                                        (size_t)mw * mh))
  {
    free(points);
    return 0;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] ellipse transform took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  // we populate the buffer
  const int wi = piece->pipe->iwidth, hi = piece->pipe->iheight;
  const float center[2] = { ellipse->center[0] * wi, ellipse->center[1] * hi };
  const float radius[2] = { ellipse->radius[0] * MIN(wi, hi), ellipse->radius[1] * MIN(wi, hi) };
  const float total[2] = { (ellipse->flags & DT_MASKS_ELLIPSE_PROPORTIONAL ? ellipse->radius[0] * (1.0f + ellipse->border) : ellipse->radius[0] + ellipse->border) * MIN(wi, hi),
                           (ellipse->flags & DT_MASKS_ELLIPSE_PROPORTIONAL ? ellipse->radius[1] * (1.0f + ellipse->border) : ellipse->radius[1] + ellipse->border) * MIN(wi, hi) };

  float a, b, ta, tb, alpha;

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

#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) shared(points, a, b, ta, tb, alpha)
#else
#pragma omp parallel for shared(points, a, b, ta, tb, alpha)
#endif
#endif
  for(int i = 0; i < mh; i++)
    for(int j = 0; j < mw; j++)
    {
      size_t index = (size_t)i * mw + j;
      float x = points[index * 2] - center[0];
      float y = points[index * 2 + 1] - center[1];
      float v = atan2(y, x) - alpha;
      float cosv = cos(v);
      float sinv = sin(v);
      float radius2 = a * a * b * b / (a * a * sinv * sinv + b * b * cosv * cosv);
      float total2 = ta * ta * tb * tb / (ta * ta * sinv * sinv + tb * tb * cosv * cosv);
      float l2 = x * x + y * y;

      if(l2 < radius2)
        points[index * 2] = 1.0f;
      else if(l2 < total2)
      {
        float f = (total2 - l2) / (total2 - radius2);
        points[index * 2] = f * f;
      }
      else
        points[index * 2] = 0.0f;
    }

// we fill the output buffer by interpolation
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) shared(points, buffer)
#else
#pragma omp parallel for shared(points, buffer)
#endif
#endif
  for(int j = 0; j < h; j++)
  {
    int jj = j % mesh;
    int mj = j / mesh;
    for(int i = 0; i < w; i++)
    {
      int ii = i % mesh;
      int mi = i / mesh;
      size_t mindex = (size_t)mj * mw + mi;
      buffer[(size_t)j * w + i]
          = (points[mindex * 2] * (mesh - ii) * (mesh - jj) + points[(mindex + 1) * 2] * ii * (mesh - jj)
             + points[(mindex + mw) * 2] * (mesh - ii) * jj + points[(mindex + mw + 1) * 2] * ii * jj)
            / (mesh * mesh);
    }
  }

  free(points);


  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] ellipse fill took %0.04f sec\n", form->name, dt_get_wtime() - start2);
//   start2 = dt_get_wtime();

  return 1;
}



// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
