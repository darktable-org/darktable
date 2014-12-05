/*
    This file is part of darktable,
    copyright (c) 2012--2013 aldric renaudin.
    copyright (c) 2013 Ulrich Pegelow.

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
#include "develop/imageop.h"
#include "develop/blend.h"
#include "control/control.h"
#include "control/conf.h"
#include "develop/masks.h"
#include "common/debug.h"


static inline void _gradient_point_transform(const float xref, const float yref, const float x, const float y,
                                             const float sinv, const float cosv, float *xnew, float *ynew)
{
  *xnew = xref + cosv * (x - xref) - sinv * (y - yref);
  *ynew = yref + sinv * (x - xref) + cosv * (y - yref);
}


static void dt_gradient_get_distance(float x, float y, float as, dt_masks_form_gui_t *gui, int index,
                                     int *inside, int *inside_border, int *near, int *inside_source)
{
  if(!gui) return;

  *inside = *inside_border = *inside_source = 0;
  *near = -1;

  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return;

  const float as2 = as * as;

  // check if we are close to pivot or anchor
  if((x - gpt->points[0]) * (x - gpt->points[0]) + (y - gpt->points[1]) * (y - gpt->points[1]) < as2
     || (x - gpt->points[2]) * (x - gpt->points[2]) + (y - gpt->points[3]) * (y - gpt->points[3]) < as2
     || (x - gpt->points[4]) * (x - gpt->points[4]) + (y - gpt->points[5]) * (y - gpt->points[5]) < as2)
  {
    *inside = 1;
    return;
  }

  // check if we are close to borders
  for(int i = 0; i < gpt->border_count; i++)
  {
    if((x - gpt->border[i * 2]) * (x - gpt->border[i * 2])
       + (y - gpt->border[i * 2 + 1]) * (y - gpt->border[i * 2 + 1]) < as2)
    {
      *inside_border = 1;
      return;
    }
  }

  // check if we are close to main line
  for(int i = 3; i < gpt->points_count; i++)
  {
    if((x - gpt->points[i * 2]) * (x - gpt->points[i * 2])
       + (y - gpt->points[i * 2 + 1]) * (y - gpt->points[i * 2 + 1]) < as2)
    {
      *inside = 1;
      return;
    }
  }
}


static int dt_gradient_events_mouse_scrolled(struct dt_iop_module_t *module, float pzx, float pzy, int up,
                                             uint32_t state, dt_masks_form_t *form, int parentid,
                                             dt_masks_form_gui_t *gui, int index)
{
  if(gui->form_selected)
  {
    // we register the current position
    if(gui->scrollx == 0.0f && gui->scrolly == 0.0f)
    {
      gui->scrollx = pzx;
      gui->scrolly = pzy;
    }
    if((state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK)
    {
      // we try to change the opacity
      dt_masks_form_change_opacity(form, parentid, up);
    }
    else if(gui->edit_mode == DT_MASKS_EDIT_FULL)
    {
      dt_masks_point_gradient_t *gradient = (dt_masks_point_gradient_t *)(g_list_first(form->points)->data);
      if(up)
        gradient->compression = fmaxf(gradient->compression, 0.001f) * 0.8f;
      else
        gradient->compression = fminf(fmaxf(gradient->compression, 0.001f) * 1.0f / 0.8f, 1.0f);
      dt_masks_write_form(form, darktable.develop);
      dt_masks_gui_form_remove(form, gui, index);
      dt_masks_gui_form_create(form, gui, index);
      dt_conf_set_float("plugins/darkroom/masks/gradient/compression", gradient->compression);
      dt_masks_update_image(darktable.develop);
    }
    return 1;
  }
  return 0;
}

static int dt_gradient_events_button_pressed(struct dt_iop_module_t *module, float pzx, float pzy,
                                             double pressure, int which, int type, uint32_t state,
                                             dt_masks_form_t *form, int parentid, dt_masks_form_gui_t *gui,
                                             int index)
{
  if(!gui) return 0;
  if(!gui->creation && gui->edit_mode == DT_MASKS_EDIT_FULL)
  {
    dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
    if(!gpt) return 0;
    // we start the form rotating or dragging
    if(gui->pivot_selected)
      gui->form_rotating = TRUE;
    else
      gui->form_dragging = TRUE;
    gui->posx = pzx * darktable.develop->preview_pipe->backbuf_width;
    gui->posy = pzy * darktable.develop->preview_pipe->backbuf_height;
    gui->dx = gpt->points[0] - gui->posx;
    gui->dy = gpt->points[1] - gui->posy;
    return 1;
  }
  else if(gui->creation && (which == 3))
  {
    darktable.develop->form_visible = NULL;
    dt_masks_clear_form_gui(darktable.develop);
    dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
    dt_masks_iop_update(module);
    dt_control_queue_redraw_center();
    return 1;
  }
  else if(gui->creation)
  {
    dt_iop_module_t *crea_module = gui->creation_module;
    // we create the circle
    dt_masks_point_gradient_t *gradient
        = (dt_masks_point_gradient_t *)(malloc(sizeof(dt_masks_point_gradient_t)));

    // we change the offset value
    float wd = darktable.develop->preview_pipe->backbuf_width;
    float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = { pzx * wd, pzy * ht };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);
    gradient->anchor[0] = pts[0] / darktable.develop->preview_pipe->iwidth;
    gradient->anchor[1] = pts[1] / darktable.develop->preview_pipe->iheight;

    const float compression = MIN(1.0f, dt_conf_get_float("plugins/darkroom/masks/gradient/compression"));
    const float steepness = 0.0f; // MIN(1.0f,dt_conf_get_float("plugins/darkroom/masks/gradient/steepness"));
                                  // // currently not used
    const float rotation = dt_conf_get_float("plugins/darkroom/masks/gradient/rotation");

    gradient->rotation = rotation;
    gradient->compression = MAX(0.0f, compression);
    gradient->steepness = MAX(0.0f, steepness);
    // not used for masks
    form->source[0] = form->source[1] = 0.0f;


    form->points = g_list_append(form->points, gradient);
    dt_masks_gui_form_save_creation(crea_module, form, gui);

    if(crea_module)
    {
      // we save the move
      dt_dev_add_history_item(darktable.develop, crea_module, TRUE);
      // and we switch in edit mode to show all the forms
      dt_masks_set_edit_mode(crea_module, DT_MASKS_EDIT_FULL);
      dt_masks_iop_update(crea_module);
      gui->creation_module = NULL;
    }
    else
    {
      // we select the new form
      dt_dev_masks_selection_change(darktable.develop, form->formid, TRUE);
    }

    return 1;
  }
  return 0;
}

static int dt_gradient_events_button_released(struct dt_iop_module_t *module, float pzx, float pzy, int which,
                                              uint32_t state, dt_masks_form_t *form, int parentid,
                                              dt_masks_form_gui_t *gui, int index)
{
  if(which == 3 && parentid > 0 && gui->edit_mode == DT_MASKS_EDIT_FULL)
  {
    dt_masks_clear_form_gui(darktable.develop);
    // we hide the form
    if(!(darktable.develop->form_visible->type & DT_MASKS_GROUP))
      darktable.develop->form_visible = NULL;
    else if(g_list_length(darktable.develop->form_visible->points) < 2)
      darktable.develop->form_visible = NULL;
    else
    {
      GList *forms = g_list_first(darktable.develop->form_visible->points);
      while(forms)
      {
        dt_masks_point_group_t *gpt = (dt_masks_point_group_t *)forms->data;
        if(gpt->formid == form->formid)
        {
          darktable.develop->form_visible->points
              = g_list_remove(darktable.develop->form_visible->points, gpt);
          break;
        }
        forms = g_list_next(forms);
      }
    }

    // we remove the shape
    dt_masks_form_remove(module, dt_masks_get_from_id(darktable.develop, parentid), form);
    dt_dev_masks_list_remove(darktable.develop, form->formid, parentid);
    return 1;
  }

  if(gui->form_dragging && gui->edit_mode == DT_MASKS_EDIT_FULL)
  {
    // we get the gradient
    dt_masks_point_gradient_t *gradient = (dt_masks_point_gradient_t *)(g_list_first(form->points)->data);

    // we end the form dragging
    gui->form_dragging = FALSE;

    // we change the center value
    float wd = darktable.develop->preview_pipe->backbuf_width;
    float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = { pzx * wd + gui->dx, pzy * ht + gui->dy };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);

    gradient->anchor[0] = pts[0] / darktable.develop->preview_pipe->iwidth;
    gradient->anchor[1] = pts[1] / darktable.develop->preview_pipe->iheight;
    dt_masks_write_form(form, darktable.develop);

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index);

    // we save the move
    dt_masks_update_image(darktable.develop);

    return 1;
  }
  else if(gui->form_rotating && gui->edit_mode == DT_MASKS_EDIT_FULL)
  {

    // we get the gradient
    dt_masks_point_gradient_t *gradient = (dt_masks_point_gradient_t *)(g_list_first(form->points)->data);

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

    gradient->rotation -= dv / M_PI * 180.0f;
    dt_masks_write_form(form, darktable.develop);

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index);

    // we save the rotation
    dt_masks_update_image(darktable.develop);

    return 1;
  }

  return 0;
}

static int dt_gradient_events_mouse_moved(struct dt_iop_module_t *module, float pzx, float pzy,
                                          double pressure, int which, dt_masks_form_t *form, int parentid,
                                          dt_masks_form_gui_t *gui, int index)
{
  if(gui->form_dragging || gui->form_rotating)
  {
    gui->posx = pzx * darktable.develop->preview_pipe->backbuf_width;
    gui->posy = pzy * darktable.develop->preview_pipe->backbuf_height;
    dt_control_queue_redraw_center();
    return 1;
  }
  else if(!gui->creation)
  {
    dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
    int closeup = dt_control_get_dev_closeup();
    float zoom_scale = dt_dev_get_zoom_scale(darktable.develop, zoom, closeup ? 2 : 1, 1);
    float as = 0.005f / zoom_scale * darktable.develop->preview_pipe->backbuf_width;
    int in, inb, near, ins;
    float x = pzx * darktable.develop->preview_pipe->backbuf_width;
    float y = pzy * darktable.develop->preview_pipe->backbuf_height;
    dt_gradient_get_distance(x, y, as, gui, index, &in, &inb, &near, &ins);

    dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);

    if(gpt
       && (x - gpt->points[2]) * (x - gpt->points[2]) + (y - gpt->points[3]) * (y - gpt->points[3]) < as * as)
    {
      gui->pivot_selected = TRUE;
      gui->form_selected = TRUE;
      gui->border_selected = FALSE;
    }
    else if(gpt
            && (x - gpt->points[4]) * (x - gpt->points[4]) + (y - gpt->points[5]) * (y - gpt->points[5])
               < as * as)
    {
      gui->pivot_selected = TRUE;
      gui->form_selected = TRUE;
      gui->border_selected = FALSE;
    }
    else if(in)
    {
      gui->pivot_selected = FALSE;
      gui->form_selected = TRUE;
      gui->border_selected = FALSE;
    }
    else if(inb)
    {
      gui->pivot_selected = FALSE;
      gui->form_selected = TRUE;
      gui->border_selected = TRUE;
    }
    else
    {
      gui->pivot_selected = FALSE;
      gui->form_selected = FALSE;
      gui->border_selected = FALSE;
    }

    dt_control_queue_redraw_center();
    if(!gui->form_selected && !gui->border_selected) return 0;
    if(gui->edit_mode != DT_MASKS_EDIT_FULL) return 0;
    return 1;
  }

  return 0;
}

static void dt_gradient_events_post_expose(cairo_t *cr, float zoom_scale, dt_masks_form_gui_t *gui, int index)
{
  double dashed[] = { 4.0, 4.0 };
  dashed[0] /= zoom_scale;
  dashed[1] /= zoom_scale;
  int len = sizeof(dashed) / sizeof(dashed[0]);
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return;
  float dx = 0.0f, dy = 0.0f, sinv = 0.0f, cosv = 1.0f, xref = gpt->points[0], yref = gpt->points[1];
  if((gui->group_selected == index) && gui->form_dragging)
  {
    dx = gui->posx + gui->dx - xref;
    dy = gui->posy + gui->dy - yref;
  }
  else if((gui->group_selected == index) && gui->form_rotating)
  {
    float v = atan2(gui->posy - yref, gui->posx - xref) - atan2(-gui->dy, -gui->dx);
    sinv = sin(v);
    cosv = cos(v);
  }

  float x, y;

  // draw line
  if(gpt->points_count > 4)
  {
    cairo_set_dash(cr, dashed, 0, 0);
    if((gui->group_selected == index) && (gui->form_selected || gui->form_dragging))
      cairo_set_line_width(cr, 5.0 / zoom_scale);
    else
      cairo_set_line_width(cr, 3.0 / zoom_scale);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    _gradient_point_transform(xref, yref, gpt->points[6] + dx, gpt->points[7] + dy, sinv, cosv, &x, &y);
    cairo_move_to(cr, x, y);
    for(int i = 5; i < gpt->points_count; i++)
    {
      _gradient_point_transform(xref, yref, gpt->points[i * 2] + dx, gpt->points[i * 2 + 1] + dy, sinv, cosv,
                                &x, &y);
      cairo_line_to(cr, x, y);
    }
    cairo_stroke_preserve(cr);
    if((gui->group_selected == index) && (gui->form_selected || gui->form_dragging))
      cairo_set_line_width(cr, 2.0 / zoom_scale);
    else
      cairo_set_line_width(cr, 1.0 / zoom_scale);
    cairo_set_source_rgba(cr, .8, .8, .8, .8);
    cairo_stroke(cr);
  }

  // draw border
  if((gui->group_selected == index) && gpt->border_count > 3)
  {
    int count = 0;
    float *border = gpt->border;
    int border_count = gpt->border_count;


    while(count < border_count)
    {
      cairo_set_dash(cr, dashed, len, 0);
      if((gui->group_selected == index) && (gui->border_selected))
        cairo_set_line_width(cr, 2.0 / zoom_scale);
      else
        cairo_set_line_width(cr, 1.0 / zoom_scale);
      cairo_set_source_rgba(cr, .3, .3, .3, .8);

      _gradient_point_transform(xref, yref, gpt->border[count * 2] + dx, gpt->border[count * 2 + 1] + dy,
                                sinv, cosv, &x, &y);
      cairo_move_to(cr, x, y);
      count++;
      for(; count < border_count && !isinf(border[count * 2]); count++)
      {
        _gradient_point_transform(xref, yref, gpt->border[count * 2] + dx, gpt->border[count * 2 + 1] + dy,
                                  sinv, cosv, &x, &y);
        cairo_line_to(cr, x, y);
      }
      cairo_stroke_preserve(cr);
      if((gui->group_selected == index) && (gui->border_selected))
        cairo_set_line_width(cr, 2.0 / zoom_scale);
      else
        cairo_set_line_width(cr, 1.0 / zoom_scale);
      cairo_set_source_rgba(cr, .8, .8, .8, .8);
      cairo_set_dash(cr, dashed, len, 4);
      cairo_stroke(cr);

      if(isinf(border[count * 2])) count++;
    }
  }

  // draw anchor point
  if(TRUE)
  {
    cairo_set_dash(cr, dashed, 0, 0);
    float anchor_size = (gui->form_dragging || gui->form_selected) ? 7.0f / zoom_scale : 5.0f / zoom_scale;
    cairo_set_source_rgba(cr, .8, .8, .8, .8);
    _gradient_point_transform(xref, yref, gpt->points[0] + dx, gpt->points[1] + dy, sinv, cosv, &x, &y);
    cairo_rectangle(cr, x - (anchor_size * 0.5), y - (anchor_size * 0.5), anchor_size, anchor_size);
    cairo_fill_preserve(cr);

    if((gui->group_selected == index) && (gui->form_dragging || gui->form_selected))
      cairo_set_line_width(cr, 2.0 / zoom_scale);
    else
      cairo_set_line_width(cr, 1.0 / zoom_scale);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    cairo_stroke(cr);
  }


  // draw pivot points
  if(TRUE)
  {
    cairo_set_dash(cr, dashed, 0, 0);
    if((gui->group_selected == index) && (gui->border_selected))
      cairo_set_line_width(cr, 2.0 / zoom_scale);
    else
      cairo_set_line_width(cr, 1.0 / zoom_scale);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);

    cairo_set_source_rgba(cr, .8, .8, .8, .8);
    _gradient_point_transform(xref, yref, gpt->points[0] + dx, gpt->points[1] + dy, sinv, cosv, &x, &y);
    cairo_move_to(cr, x, y);
    _gradient_point_transform(xref, yref, gpt->points[2] + dx, gpt->points[3] + dy, sinv, cosv, &x, &y);
    cairo_line_to(cr, x, y);
    cairo_set_line_width(cr, 1.0 / zoom_scale);
    cairo_stroke(cr);

    _gradient_point_transform(xref, yref, gpt->points[2] + dx, gpt->points[3] + dy, sinv, cosv, &x, &y);
    cairo_arc(cr, x, y, 3.0f / zoom_scale, 0, 2.0f * M_PI);
    cairo_fill_preserve(cr);

    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    cairo_stroke(cr);

    cairo_set_source_rgba(cr, .8, .8, .8, .8);
    _gradient_point_transform(xref, yref, gpt->points[0] + dx, gpt->points[1] + dy, sinv, cosv, &x, &y);
    cairo_move_to(cr, x, y);
    _gradient_point_transform(xref, yref, gpt->points[4] + dx, gpt->points[5] + dy, sinv, cosv, &x, &y);
    cairo_line_to(cr, x, y);
    cairo_stroke(cr);

    _gradient_point_transform(xref, yref, gpt->points[4] + dx, gpt->points[5] + dy, sinv, cosv, &x, &y);
    cairo_arc(cr, x, y, 3.0f / zoom_scale, 0, 2.0f * M_PI);
    cairo_fill_preserve(cr);

    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    cairo_stroke(cr);
  }
}


static int dt_gradient_get_points(dt_develop_t *dev, float x, float y, float rotation, float **points,
                                  int *points_count)
{
  *points = NULL;
  *points_count = 0;

  const float wd = dev->preview_pipe->iwidth;
  const float ht = dev->preview_pipe->iheight;
  const float distance = 0.1f * fminf(wd, ht);

  const float xmax = wd - 1.0f;
  const float ymax = ht - 1.0f;

  const float v = (-rotation / 180.0f) * M_PI;
  const float cosv = cos(v);
  const float sinv = sin(v);
  const float offset = sinv * x * wd - cosv * y * ht;

  // find intercept points of straight line and image borders
  int intercept_count = 0;
  float intercept[4];
  int l;
  float delta_x, delta_y;

  if(sinv == 0.0f)
  {
    float is = -offset / cosv;
    if(is >= 0.0f && is <= ymax)
    {
      intercept[0] = 0;
      intercept[1] = is;
      intercept[2] = xmax;
      intercept[3] = is;
      intercept_count = 2;
    }
  }
  else if(cosv == 0.0f)
  {
    float is = offset / sinv;
    if(is >= 0.0f && is <= xmax)
    {
      intercept[0] = is;
      intercept[1] = 0;
      intercept[2] = is;
      intercept[3] = ymax;
      intercept_count = 2;
    }
  }
  else
  {
    float is = -offset / cosv;
    if(is >= 0.0f && is <= ymax)
    {
      intercept[0] = 0;
      intercept[1] = is;
      intercept_count++;
    }
    is = (xmax * sinv - offset) / cosv;
    if(is >= 0.0f && is <= ymax)
    {
      intercept[intercept_count * 2] = xmax;
      intercept[intercept_count * 2 + 1] = is;
      intercept_count++;
    }
    is = offset / sinv;
    if(is >= 0.0f && is <= xmax && intercept_count < 2)
    {
      intercept[intercept_count * 2] = is;
      intercept[intercept_count * 2 + 1] = 0;
      intercept_count++;
    }
    is = (ymax * cosv + offset) / sinv;
    if(is >= 0.0f && is <= xmax && intercept_count < 2)
    {
      intercept[intercept_count * 2] = is;
      intercept[intercept_count * 2 + 1] = ymax;
      intercept_count++;
    }
  }

  // how many points do we need ?
  if(intercept_count != 2)
  {
    l = 0;
    delta_x = delta_y = 0.0f;
  }
  else
  {
    l = (int)ceilf(sqrt((intercept[2] - intercept[0]) * (intercept[2] - intercept[0])
                        + (intercept[3] - intercept[1]) * (intercept[3] - intercept[1])));
    delta_x = (intercept[2] - intercept[0] != 0.0f) ? (intercept[2] - intercept[0]) / l : 0.0f;
    delta_y = (intercept[3] - intercept[1] != 0.0f) ? (intercept[3] - intercept[1]) / l : 0.0f;
  }

  // buffer allocations
  *points = calloc(2 * (l + 3), sizeof(float));
  if(*points == NULL) return 0;
  *points_count = l + 3;

  // we set the anchor point
  (*points)[0] = x * wd;
  (*points)[1] = y * ht;

  // we set the pivot points
  const float v1 = (-(rotation - 90.0f) / 180.0f) * M_PI;
  const float x1 = x * wd + distance * cos(v1);
  const float y1 = y * ht + distance * sin(v1);
  (*points)[2] = x1;
  (*points)[3] = y1;
  const float v2 = (-(rotation + 90.0f) / 180.0f) * M_PI;
  const float x2 = x * wd + distance * cos(v2);
  const float y2 = y * ht + distance * sin(v2);
  (*points)[4] = x2;
  (*points)[5] = y2;

  // we set the line point
  float xx = intercept[0];
  float yy = intercept[1];
  for(int i = 3; i < l + 3; i++)
  {
    (*points)[i * 2] = xx;
    (*points)[i * 2 + 1] = yy;
    xx += delta_x;
    yy += delta_y;
  }

  // and we transform them with all distorted modules
  if(dt_dev_distort_transform(dev, *points, l + 3)) return 1;

  // if we failed, then free all and return
  free(*points);
  *points = NULL;
  *points_count = 0;
  return 0;
}

static int dt_gradient_get_points_border(dt_develop_t *dev, float x, float y, float rotation, float distance,
                                         float **points, int *points_count)
{
  *points = NULL;
  *points_count = 0;

  float *points1 = NULL, *points2 = NULL;
  int points_count1 = 0, points_count2 = 0;

  const float wd = dev->preview_pipe->iwidth;
  const float ht = dev->preview_pipe->iheight;
  const float scale = sqrtf(wd * wd + ht * ht);

  const float v1 = (-(rotation - 90.0f) / 180.0f) * M_PI;

  float x1 = (x * wd + distance * scale * cos(v1)) / wd;
  float y1 = (y * ht + distance * scale * sin(v1)) / ht;

  int r1 = dt_gradient_get_points(dev, x1, y1, rotation, &points1, &points_count1);

  const float v2 = (-(rotation + 90.0f) / 180.0f) * M_PI;

  float x2 = (x * wd + distance * scale * cos(v2)) / wd;
  float y2 = (y * ht + distance * scale * sin(v2)) / ht;

  int r2 = dt_gradient_get_points(dev, x2, y2, rotation, &points2, &points_count2);

  if(r1 && r2 && points_count1 > 4 && points_count2 > 4)
  {
    int k = 0;
    *points = malloc(2 * ((points_count1 - 3) + (points_count2 - 3) + 1) * sizeof(float));
    if(*points == NULL) return 0;
    *points_count = (points_count1 - 3) + (points_count2 - 3) + 1;
    for(int i = 3; i < points_count1; i++)
    {
      (*points)[k * 2] = points1[i * 2];
      (*points)[k * 2 + 1] = points1[i * 2 + 1];
      k++;
    }
    (*points)[k * 2] = (*points)[k * 2 + 1] = INFINITY;
    k++;
    for(int i = 3; i < points_count2; i++)
    {
      (*points)[k * 2] = points2[i * 2];
      (*points)[k * 2 + 1] = points2[i * 2 + 1];
      k++;
    }
    free(points1);
    free(points2);
    return 1;
  }
  else if(r1 && points_count1 > 4)
  {
    int k = 0;
    *points = malloc(2 * ((points_count1 - 3)) * sizeof(float));
    if(*points == NULL) return 0;
    *points_count = points_count1 - 3;
    for(int i = 3; i < points_count1; i++)
    {
      (*points)[k * 2] = points1[i * 2];
      (*points)[k * 2 + 1] = points1[i * 2 + 1];
      k++;
    }
    free(points1);
    return 1;
  }
  else if(r2 && points_count2 > 4)
  {
    int k = 0;
    *points = malloc(2 * ((points_count2 - 3)) * sizeof(float));
    if(*points == NULL) return 0;
    *points_count = points_count2 - 3;

    for(int i = 3; i < points_count2; i++)
    {
      (*points)[k * 2] = points2[i * 2];
      (*points)[k * 2 + 1] = points2[i * 2 + 1];
      k++;
    }
    free(points2);
    return 1;
  }

  return 0;
}

static int dt_gradient_get_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                                int *width, int *height, int *posx, int *posy)
{
  float wd = piece->pipe->iwidth, ht = piece->pipe->iheight;

  float points[8];

  // now we set the points
  points[0] = 0;
  points[1] = 0;
  points[2] = wd;
  points[3] = 0;
  points[4] = wd;
  points[5] = ht;
  points[6] = 0;
  points[7] = ht;

  // and we transform them with all distorted modules
  if(!dt_dev_distort_transform_plus(module->dev, piece->pipe, 0, module->priority, points, 4)) return 0;

  // now we search min and max
  float xmin, xmax, ymin, ymax;
  xmin = ymin = FLT_MAX;
  xmax = ymax = FLT_MIN;
  for(int i = 0; i < 4; i++)
  {
    xmin = fminf(points[i * 2], xmin);
    xmax = fmaxf(points[i * 2], xmax);
    ymin = fminf(points[i * 2 + 1], ymin);
    ymax = fmaxf(points[i * 2 + 1], ymax);
  }

  // and we set values
  *posx = xmin;
  *posy = ymin;
  *width = (xmax - xmin);
  *height = (ymax - ymin);
  return 1;
}


static int dt_gradient_get_mask(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                                float **buffer, int *width, int *height, int *posx, int *posy)
{
  double start2 = dt_get_wtime();

  // we get the area
  if(!dt_gradient_get_area(module, piece, form, width, height, posx, posy)) return 0;

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] gradient area took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  // we get the gradient values
  dt_masks_point_gradient_t *gradient = (dt_masks_point_gradient_t *)(g_list_first(form->points)->data);

  // we create a buffer of mesh points for later interpolation. mainly in order to reduce memory footprint
  const int w = *width;
  const int h = *height;
  const int px = *posx;
  const int py = *posy;
  const int mesh = 8;
  const int mw = (w + mesh - 1) / mesh + 1;
  const int mh = (h + mesh - 1) / mesh + 1;

  float *points = malloc(mw * mh * 2 * sizeof(float));
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
      points[(j * mw + i) * 2] = (mesh * i + px);
      points[(j * mw + i) * 2 + 1] = (mesh * j + py);
    }

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] gradient draw took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  // we backtransform all these points
  if(!dt_dev_distort_backtransform_plus(module->dev, piece->pipe, 0, module->priority, points, mw * mh))
  {
    free(points);
    return 0;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] gradient transform took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  // we calculate the mask at mesh points and recycle point buffer to store results
  const float wd = piece->pipe->iwidth;
  const float ht = piece->pipe->iheight;
  const float hwscale = 1.0f / sqrtf(wd * wd + ht * ht);
  const float v = (-gradient->rotation / 180.0f) * M_PI;
  const float sinv = sin(v);
  const float cosv = cos(v);
  const float offset = sinv * gradient->anchor[0] * wd - cosv * gradient->anchor[1] * ht;
  const float compression = fmaxf(gradient->compression, 0.001f);
  const float cs = powf(10.0f, gradient->steepness);
  const float steepness = cs * cs - 1.0f;
  const float normf = 0.5f * cs / compression;

#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) shared(points)
#else
#pragma omp parallel for shared(points)
#endif
#endif
  for(int j = 0; j < mh; j++)
  {
    for(int i = 0; i < mw; i++)
    {
      float x = points[(j * mw + i) * 2];
      float y = points[(j * mw + i) * 2 + 1];

      float distance = (sinv * x - cosv * y - offset) * hwscale;
      float value = normf * distance / sqrtf(1.0f + steepness * distance * distance) + 0.5f;

      points[(j * mw + i) * 2] = (value < 0.0f) ? 0.0f : ((value > 1.0f) ? 1.0f : value);
    }
  }

  // we allocate the buffer
  *buffer = calloc(w * h, sizeof(float));
  if(*buffer == NULL)
  {
    free(points);
    return 0;
  }

// we fill the mask buffer by interpolation
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
      (*buffer)[j * w + i] = (points[(mj * mw + mi) * 2] * (mesh - ii) * (mesh - jj)
                              + points[(mj * mw + mi + 1) * 2] * ii * (mesh - jj)
                              + points[((mj + 1) * mw + mi) * 2] * (mesh - ii) * jj
                              + points[((mj + 1) * mw + mi + 1) * 2] * ii * jj) / (mesh * mesh);
    }
  }

  free(points);

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] gradient fill took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  return 1;
}


static int dt_gradient_get_mask_roi(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                    dt_masks_form_t *form, const dt_iop_roi_t *roi, float *buffer)
{
  double start2 = dt_get_wtime();

  // we get the gradient values
  dt_masks_point_gradient_t *gradient = (dt_masks_point_gradient_t *)(g_list_first(form->points)->data);

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
    dt_print(DT_DEBUG_MASKS, "[masks %s] gradient draw took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  // we backtransform all these points
  if(!dt_dev_distort_backtransform_plus(module->dev, piece->pipe, 0, module->priority, points,
                                        (size_t)mw * mh))
  {
    free(points);
    return 0;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] gradient transform took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  // we calculate the mask at mesh points and recycle point buffer to store results
  const float wd = piece->pipe->iwidth;
  const float ht = piece->pipe->iheight;
  const float hwscale = 1.0f / sqrtf(wd * wd + ht * ht);
  const float v = (-gradient->rotation / 180.0f) * M_PI;
  const float sinv = sin(v);
  const float cosv = cos(v);
  const float offset = sinv * gradient->anchor[0] * wd - cosv * gradient->anchor[1] * ht;
  const float compression = fmaxf(gradient->compression, 0.001f);
  const float cs = powf(10.0f, gradient->steepness);
  const float steepness = cs * cs - 1.0f;
  const float normf = 0.5f * cs / compression;

#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) shared(points)
#else
#pragma omp parallel for shared(points)
#endif
#endif
  for(int j = 0; j < mh; j++)
  {
    for(int i = 0; i < mw; i++)
    {
      size_t index = (size_t)j * mw + i;
      float x = points[index * 2];
      float y = points[index * 2 + 1];

      float distance = (sinv * x - cosv * y - offset) * hwscale;
      float value = normf * distance / sqrtf(1.0f + steepness * distance * distance) + 0.5f;

      points[index * 2] = (value < 0.0f) ? 0.0f : ((value > 1.0f) ? 1.0f : value);
    }
  }

// we fill the mask buffer by interpolation
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
    dt_print(DT_DEBUG_MASKS, "[masks %s] gradient fill took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  return 1;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
