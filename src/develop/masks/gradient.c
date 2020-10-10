/*
    This file is part of darktable,
    Copyright (C) 2013-2020 darktable developers.

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

  const dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
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
  if(gui->creation)
  {
    if((state & GDK_SHIFT_MASK) == GDK_SHIFT_MASK)
    {
      float compression = MIN(1.0f, dt_conf_get_float("plugins/darkroom/masks/gradient/compression"));
      if(up)
        compression = fmaxf(compression, 0.001f) * 0.8f;
      else
        compression = fminf(fmaxf(compression, 0.001f) * 1.0f / 0.8f, 1.0f);
      dt_conf_set_float("plugins/darkroom/masks/gradient/compression", compression);
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
    if((state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK)
    {
      // we try to change the opacity
      dt_masks_form_change_opacity(form, parentid, up);
    }
    else if((state & GDK_SHIFT_MASK) == GDK_SHIFT_MASK)
    {
      dt_masks_point_gradient_t *gradient = (dt_masks_point_gradient_t *)(g_list_first(form->points)->data);
      if(up)
        gradient->compression = fmaxf(gradient->compression, 0.001f) * 0.8f;
      else
        gradient->compression = fminf(fmaxf(gradient->compression, 0.001f) * 1.0f / 0.8f, 1.0f);
      dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
      dt_masks_gui_form_remove(form, gui, index);
      dt_masks_gui_form_create(form, gui, index);
      dt_conf_set_float("plugins/darkroom/masks/gradient/compression", gradient->compression);
      dt_masks_update_image(darktable.develop);
    }
    else if(gui->edit_mode == DT_MASKS_EDIT_FULL)
    {
      dt_masks_point_gradient_t *gradient = (dt_masks_point_gradient_t *)(g_list_first(form->points)->data);
      if(up)
        gradient->curvature = fminf(gradient->curvature + 0.05f, 2.0f);
      else
        gradient->curvature = fmaxf(gradient->curvature - 0.05f, -2.0f);
      dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
      dt_masks_gui_form_remove(form, gui, index);
      dt_masks_gui_form_create(form, gui, index);
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

  if(which == 1 && type == GDK_2BUTTON_PRESS)
  {
    // double-click resets curvature
    dt_masks_point_gradient_t *gradient = (dt_masks_point_gradient_t *)(g_list_first(form->points)->data);

    gradient->curvature = 0.0f;
    dt_dev_add_masks_history_item(darktable.develop, module, TRUE);

    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index);

    dt_masks_update_image(darktable.develop);

    return 1;
  }
  else if(!gui->creation && ((state & GDK_SHIFT_MASK) == GDK_SHIFT_MASK))
  {
    dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
    if(!gpt) return 0;

    gui->gradient_toggling = TRUE;

    return 1;
  }
  else if(!gui->creation && gui->edit_mode == DT_MASKS_EDIT_FULL)
  {
    const dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
    if(!gpt) return 0;
    // we start the form rotating or dragging
    if(gui->pivot_selected)
      gui->form_rotating = TRUE;
    else
      gui->form_dragging = TRUE;
    gui->dx = gpt->points[0] - gui->posx;
    gui->dy = gpt->points[1] - gui->posy;
    return 1;
  }
  else if(gui->creation && (which == 3))
  {
    dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
    dt_masks_iop_update(module);
    dt_control_queue_redraw_center();
    return 1;
  }
  else if(gui->creation)
  {
    gui->posx_source = gui->posx;
    gui->posy_source = gui->posy;
    gui->form_dragging = TRUE;
  }
  return 0;
}

static int dt_gradient_events_button_released(struct dt_iop_module_t *module, float pzx, float pzy, int which,
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
    dt_masks_form_remove(module, dt_masks_get_from_id(darktable.develop, parentid), form);
    return 1;
  }

  if(gui->form_dragging && gui->edit_mode == DT_MASKS_EDIT_FULL)
  {
    // we get the gradient
    dt_masks_point_gradient_t *gradient = (dt_masks_point_gradient_t *)(g_list_first(form->points)->data);

    // we end the form dragging
    gui->form_dragging = FALSE;

    // we change the center value
    const float wd = darktable.develop->preview_pipe->backbuf_width;
    const float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = { pzx * wd + gui->dx, pzy * ht + gui->dy };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);

    gradient->anchor[0] = pts[0] / darktable.develop->preview_pipe->iwidth;
    gradient->anchor[1] = pts[1] / darktable.develop->preview_pipe->iheight;
    dt_dev_add_masks_history_item(darktable.develop, module, TRUE);

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

    const float wd = darktable.develop->preview_pipe->backbuf_width;
    const float ht = darktable.develop->preview_pipe->backbuf_height;
    const float x = pzx * wd;
    const float y = pzy * ht;

    // we need the reference point
    dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
    if(!gpt) return 0;
    const float xref = gpt->points[0];
    const float yref = gpt->points[1];

    float pts[8] = { xref, yref, x , y, 0, 0, gui->dx, gui->dy };

    const float dv = atan2(pts[3] - pts[1], pts[2] - pts[0]) - atan2(-(pts[7] - pts[5]), -(pts[6] - pts[4]));

    gradient->rotation -= dv / M_PI * 180.0f;
    dt_dev_add_masks_history_item(darktable.develop, module, TRUE);

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index);

    // we save the rotation
    dt_masks_update_image(darktable.develop);

    return 1;
  }
  else if(gui->gradient_toggling)
  {
    // we get the gradient
    dt_masks_point_gradient_t *gradient = (dt_masks_point_gradient_t *)(g_list_first(form->points)->data);

    // we end the gradient toggling
    gui->gradient_toggling = FALSE;

    // toggle transition type of gradient
    if(gradient->state == DT_MASKS_GRADIENT_STATE_LINEAR)
      gradient->state = DT_MASKS_GRADIENT_STATE_SIGMOIDAL;
    else
      gradient->state = DT_MASKS_GRADIENT_STATE_LINEAR;

    dt_dev_add_masks_history_item(darktable.develop, module, TRUE);

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index);

    // we save the new parameters
    dt_masks_update_image(darktable.develop);

    return 1;
  }
  else if(gui->creation)
  {
    const float wd = darktable.develop->preview_pipe->backbuf_width;
    const float ht = darktable.develop->preview_pipe->backbuf_height;

    // get the rotation angle only if we are not too close from starting point
    dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
    const int closeup = dt_control_get_dev_closeup();
    const float zoom_scale = dt_dev_get_zoom_scale(darktable.develop, zoom, 1 << closeup, 1);
    const float diff = 5.0f * zoom_scale;
    float x0 = 0.0f, y0 = 0.0f;
    float rotation = 0.0f;
    if(!gui->form_dragging
       || (gui->posx_source - gui->posx > -diff && gui->posx_source - gui->posx < diff
           && gui->posy_source - gui->posy > -diff && gui->posy_source - gui->posy < diff))
    {
      rotation = -1.0f;
      x0 = pzx * wd;
      y0 = pzy * ht;
    }
    else
    {
      rotation = 99.0f; // dummy value, we need to recompute after distort_backtransform
      x0 = gui->posx_source;
      y0 = gui->posy_source;
    }

    gui->form_dragging = FALSE;
    dt_iop_module_t *crea_module = gui->creation_module;
    // we create the gradient
    dt_masks_point_gradient_t *gradient = (dt_masks_point_gradient_t *)(malloc(sizeof(dt_masks_point_gradient_t)));

    // we change the offset value
    float pts[8] = { x0, y0, pzx * wd, pzy * ht, x0 - 100, y0 -100, x0 + 100, y0 + 100 };
    dt_dev_distort_backtransform(darktable.develop, pts, 4);
    gradient->anchor[0] = pts[0] / darktable.develop->preview_pipe->iwidth;
    gradient->anchor[1] = pts[1] / darktable.develop->preview_pipe->iheight;

    rotation = atan2(pts[7] - pts[5], pts[6] - pts[4]) - atan2(200, 200);

    const float compression = MIN(1.0f, dt_conf_get_float("plugins/darkroom/masks/gradient/compression"));

    gradient->rotation = -rotation / M_PI * 180.0f;
    gradient->compression = MAX(0.0f, compression);
    gradient->steepness = 0.0f;
    gradient->curvature = 0.0f;
    gradient->state = DT_MASKS_GRADIENT_STATE_SIGMOIDAL;
    // not used for masks
    form->source[0] = form->source[1] = 0.0f;

    form->points = g_list_append(form->points, gradient);
    dt_masks_gui_form_save_creation(darktable.develop, crea_module, form, gui);

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

    if(crea_module && gui->creation_continuous)
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

static int dt_gradient_events_mouse_moved(struct dt_iop_module_t *module, float pzx, float pzy,
                                          double pressure, int which, dt_masks_form_t *form, int parentid,
                                          dt_masks_form_gui_t *gui, int index)
{
  if(gui->form_dragging || gui->form_rotating)
  {
    dt_control_queue_redraw_center();
    return 1;
  }
  else if(!gui->creation)
  {
    const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
    const int closeup = dt_control_get_dev_closeup();
    const float zoom_scale = dt_dev_get_zoom_scale(darktable.develop, zoom, 1<<closeup, 1);
    const float as = 0.005f / zoom_scale * darktable.develop->preview_pipe->backbuf_width;
    int in, inb, near, ins;
    const float x = pzx * darktable.develop->preview_pipe->backbuf_width;
    const float y = pzy * darktable.develop->preview_pipe->backbuf_height;
    dt_gradient_get_distance(x, y, as, gui, index, &in, &inb, &near, &ins);

    const dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);

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
  // add a preview when creating a gradient
  else if(gui->creation)
  {
    dt_control_queue_redraw_center();
    return 1;
  }

  return 0;
}

// check if (x,y) lies within reasonable limits relative to image frame
static inline int _gradient_is_canonical(const float x, const float y, const float wd, const float ht)
{
  return (isnormal(x) && isnormal(y) && x >= -wd && x <= 2 * wd && y >= -ht && y <= 2 * ht) ? TRUE : FALSE;
}


static void dt_gradient_events_post_expose(cairo_t *cr, float zoom_scale, dt_masks_form_gui_t *gui, int index)
{
  double dashed[] = { 4.0, 4.0 };
  dashed[0] /= zoom_scale;
  dashed[1] /= zoom_scale;
  const int len = sizeof(dashed) / sizeof(dashed[0]);

  // preview gradient creation
  if(gui->creation)
  {
    const float pr_dn = darktable.develop->preview_downsampling;
    const float iwd = pr_dn * darktable.develop->preview_pipe->iwidth;
    const float iht = pr_dn * darktable.develop->preview_pipe->iheight;
    const float compression = MIN(1.0f, dt_conf_get_float("plugins/darkroom/masks/gradient/compression"));
    const float distance = 0.1f * MIN(iwd, iht);
    const float scale = sqrtf(iwd * iwd + iht * iht);

    float xpos = 0.0f, ypos = 0.0f, xpos0 = 0.0f, ypos0 = 0.0f;
    const float zoom_x = dt_control_get_dev_zoom_x();
    const float zoom_y = dt_control_get_dev_zoom_y();
    if((gui->posx == -1.0f && gui->posy == -1.0f) || gui->mouse_leaved_center)
    {
      xpos = (.5f + zoom_x) * darktable.develop->preview_pipe->backbuf_width;
      ypos = (.5f + zoom_y) * darktable.develop->preview_pipe->backbuf_height;
    }
    else
    {
      xpos = gui->posx;
      ypos = gui->posy;
    }

    // get the rotation angle only if we are not too close from starting point
    const float diff = 5.0f * zoom_scale;
    float rotation = 0.0f;
    if(!gui->form_dragging
       || (gui->posx_source - gui->posx > -diff && gui->posx_source - gui->posx < diff
           && gui->posy_source - gui->posy > -diff && gui->posy_source - gui->posy < diff))
    {
      rotation = 0.0f;
      xpos0 = xpos;
      ypos0 = ypos;
    }
    else
    {
      rotation = atan2f(gui->posy - gui->posy_source, gui->posx - gui->posx_source);
      xpos0 = gui->posx_source;
      ypos0 = gui->posy_source;
    }
    const float trotation = tanf(rotation);

    cairo_save(cr);

    // draw main line
    cairo_set_line_width(cr, 5.0 / zoom_scale);
    dt_draw_set_color_overlay(cr, 0.3, 0.8);

    cairo_move_to(cr, 0.0f, ypos - xpos * trotation);
    cairo_line_to(cr, darktable.develop->preview_pipe->backbuf_width,
                  ypos + (darktable.develop->preview_pipe->backbuf_width - xpos) * trotation);
    cairo_stroke_preserve(cr);
    cairo_set_line_width(cr, 2.0 / zoom_scale);
    dt_draw_set_color_overlay(cr, 0.8, 0.8);
    cairo_stroke(cr);

    // draw the arrow
    float anchor_x = 0.0f, anchor_y = 0.0f;
    float pivot_start_x = 0.0f, pivot_start_y = 0.0f;
    float pivot_end_x = 0.0f, pivot_end_y = 0.0f;
    anchor_x = xpos0;
    anchor_y = ypos0;
    pivot_start_x = xpos0 + sinf(rotation) * distance;
    pivot_end_x = xpos0 - sinf(rotation) * distance;
    pivot_start_y = ypos0 - cosf(rotation) * distance;
    pivot_end_y = ypos0 + cosf(rotation) * distance;
    cairo_set_dash(cr, dashed, 0, 0);
    cairo_set_line_width(cr, 2.0 / zoom_scale);
    dt_draw_set_color_overlay(cr, 0.3, 0.8);

    // from start to end
    dt_draw_set_color_overlay(cr, 0.8, 0.8);
    cairo_line_to(cr, pivot_start_x, pivot_start_y);
    cairo_line_to(cr, pivot_end_x, pivot_end_y);
    cairo_stroke(cr);

    // start side of the gradient
    dt_draw_set_color_overlay(cr, 0.3, 0.8);
    cairo_arc(cr, pivot_start_x, pivot_start_y, 3.0f / zoom_scale, 0, 2.0f * M_PI);
    cairo_fill_preserve(cr);
    cairo_stroke(cr);

    // end side of the gradient
    cairo_arc(cr, pivot_end_x, pivot_end_y, 1.0f / zoom_scale, 0, 2.0f * M_PI);
    cairo_fill_preserve(cr);
    dt_draw_set_color_overlay(cr, 0.3, 0.8);
    cairo_stroke(cr);

    // draw arrow on the end of the gradient to clearly display the direction

    // size & width of the arrow
    const float arrow_angle = 0.25;
    const float arrow_length = 15.0 / zoom_scale;

    const float a_dx = anchor_x - pivot_end_x;
    const float a_dy = pivot_end_y - anchor_y;
    const float angle = atan2(a_dx, a_dy) - M_PI / 2.0;

    const float arrow_x1 = pivot_end_x + (arrow_length * cos(angle + arrow_angle));
    const float arrow_x2 = pivot_end_x + (arrow_length * cos(angle - arrow_angle));
    const float arrow_y1 = pivot_end_y + (arrow_length * sin(angle + arrow_angle));
    const float arrow_y2 = pivot_end_y + (arrow_length * sin(angle - arrow_angle));

    dt_draw_set_color_overlay(cr, 0.8, 0.8);
    cairo_move_to(cr, pivot_end_x, pivot_end_y);
    cairo_line_to(cr, arrow_x1, arrow_y1);
    cairo_line_to(cr, arrow_x2, arrow_y2);
    cairo_line_to(cr, pivot_end_x, pivot_end_y);
    cairo_close_path(cr);
    cairo_fill_preserve(cr);
    cairo_stroke(cr);

    // and the border
    pivot_start_x = xpos0 + sinf(rotation) * compression * scale;
    pivot_end_x = xpos0 - sinf(rotation) * compression * scale;
    pivot_start_y = ypos0 - cosf(rotation) * compression * scale;
    pivot_end_y = ypos0 + cosf(rotation) * compression * scale;
    cairo_set_dash(cr, dashed, len, 0);
    cairo_set_line_width(cr, 2.0 / zoom_scale);
    dt_draw_set_color_overlay(cr, 0.3, 0.8);
    cairo_move_to(cr, 0.0f, pivot_start_y - pivot_start_x * trotation);
    cairo_line_to(cr, darktable.develop->preview_pipe->backbuf_width,
                  pivot_start_y + (darktable.develop->preview_pipe->backbuf_width - pivot_start_x) * trotation);
    cairo_stroke_preserve(cr);
    dt_draw_set_color_overlay(cr, 0.8, 0.8);
    cairo_set_dash(cr, dashed, len, 4);
    cairo_stroke(cr);
    dt_draw_set_color_overlay(cr, 0.3, 0.8);
    cairo_move_to(cr, 0.0f, pivot_end_y - pivot_end_x * trotation);
    cairo_line_to(cr, darktable.develop->preview_pipe->backbuf_width,
                  pivot_end_y + (darktable.develop->preview_pipe->backbuf_width - pivot_end_x) * trotation);
    cairo_stroke_preserve(cr);
    dt_draw_set_color_overlay(cr, 0.8, 0.8);
    cairo_set_dash(cr, dashed, len, 4);
    cairo_stroke(cr);

    cairo_restore(cr);
    return;
  }
  const dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return;
  float dx = 0.0f, dy = 0.0f, sinv = 0.0f, cosv = 1.0f;
  const float xref = gpt->points[0];
  const float yref = gpt->points[1];

  if((gui->group_selected == index) && gui->form_dragging)
  {
    dx = gui->posx + gui->dx - xref;
    dy = gui->posy + gui->dy - yref;
  }
  else if((gui->group_selected == index) && gui->form_rotating)
  {
    const float v = atan2(gui->posy - yref, gui->posx - xref) - atan2(-gui->dy, -gui->dx);
    sinv = sin(v);
    cosv = cos(v);
  }

  // draw line
  if(gpt->points_count > 4)
  {
    const float *points = gpt->points + 6;
    const int points_count = gpt->points_count - 3;
    const float wd = darktable.develop->preview_pipe->iwidth;
    const float ht = darktable.develop->preview_pipe->iheight;

    int count = 0;
    float x = 0.0f, y = 0.0f;

    while(count < points_count)
    {
      if(!isnormal(points[count * 2]))
      {
        count++;
        continue;
      }

      _gradient_point_transform(xref, yref, points[count * 2] + dx, points[count * 2 + 1] + dy, sinv, cosv, &x, &y);

      if(!_gradient_is_canonical(x, y, wd, ht))
      {
        count++;
        continue;
      }

      cairo_set_dash(cr, dashed, 0, 0);
      if((gui->group_selected == index) && (gui->form_selected || gui->form_dragging))
        cairo_set_line_width(cr, 5.0 / zoom_scale);
      else
        cairo_set_line_width(cr, 3.0 / zoom_scale);
      dt_draw_set_color_overlay(cr, 0.3, 0.8);

      cairo_move_to(cr, x, y);

      count++;
      for(; count < points_count && isnormal(points[count * 2]); count++)
      {
        _gradient_point_transform(xref, yref, points[count * 2] + dx, points[count * 2 + 1] + dy, sinv, cosv,
                                &x, &y);

        if(!_gradient_is_canonical(x, y, wd, ht))
          break;

        cairo_line_to(cr, x, y);
      }
      cairo_stroke_preserve(cr);
      if((gui->group_selected == index) && (gui->form_selected || gui->form_dragging))
        cairo_set_line_width(cr, 2.0 / zoom_scale);
      else
        cairo_set_line_width(cr, 1.0 / zoom_scale);
      dt_draw_set_color_overlay(cr, 0.8, 0.8);
      cairo_stroke(cr);
    }
  }

  // draw border
  if((gui->group_selected == index) && gpt->border_count > 3)
  {
    const float *border = gpt->border;
    const int border_count = gpt->border_count;
    const float wd = darktable.develop->preview_pipe->iwidth;
    const float ht = darktable.develop->preview_pipe->iheight;

    int count = 0;
    float x = 0.0f, y = 0.0f;

    while(count < border_count)
    {
      if(!isnormal(border[count * 2]))
      {
        count++;
        continue;
      }

      _gradient_point_transform(xref, yref, border[count * 2] + dx, border[count * 2 + 1] + dy, sinv, cosv, &x, &y);

      if(!_gradient_is_canonical(x, y, wd, ht))
      {
        count++;
        continue;
      }

      cairo_set_dash(cr, dashed, len, 0);
      if((gui->group_selected == index) && (gui->border_selected))
        cairo_set_line_width(cr, 2.0 / zoom_scale);
      else
        cairo_set_line_width(cr, 1.0 / zoom_scale);
      dt_draw_set_color_overlay(cr, 0.3, 0.8);

      cairo_move_to(cr, x, y);

      count++;
      for(; count < border_count && isnormal(border[count * 2]); count++)
      {
        _gradient_point_transform(xref, yref, border[count * 2] + dx, border[count * 2 + 1] + dy,
                                  sinv, cosv, &x, &y);

        if(!_gradient_is_canonical(x, y, wd, ht))
          break;

        cairo_line_to(cr, x, y);
      }
      cairo_stroke_preserve(cr);
      if((gui->group_selected == index) && (gui->border_selected))
        cairo_set_line_width(cr, 2.0 / zoom_scale);
      else
        cairo_set_line_width(cr, 1.0 / zoom_scale);
      dt_draw_set_color_overlay(cr, 0.8, 0.8);
      cairo_set_dash(cr, dashed, len, 4);
      cairo_stroke(cr);
    }
  }

  float anchor_x = 0.0f, anchor_y = 0.0f;
  float pivot_start_x = 0.0f, pivot_start_y = 0.0f;
  float pivot_end_x = 0.0f, pivot_end_y = 0.0f;

  _gradient_point_transform(xref, yref, gpt->points[0] + dx, gpt->points[1] + dy, sinv, cosv, &anchor_x, &anchor_y);
  _gradient_point_transform(xref, yref, gpt->points[2] + dx, gpt->points[3] + dy, sinv, cosv, &pivot_end_x, &pivot_end_y);
  _gradient_point_transform(xref, yref, gpt->points[4] + dx, gpt->points[5] + dy, sinv, cosv, &pivot_start_x, &pivot_start_y);

  // draw anchor point
  {
    cairo_set_dash(cr, dashed, 0, 0);
    const float anchor_size = (gui->form_dragging || gui->form_selected) ? 7.0f / zoom_scale : 5.0f / zoom_scale;
    dt_draw_set_color_overlay(cr, 0.8, 0.8);
    cairo_rectangle(cr, anchor_x - (anchor_size * 0.5), anchor_y - (anchor_size * 0.5), anchor_size, anchor_size);
    cairo_fill_preserve(cr);

    if((gui->group_selected == index) && (gui->form_dragging || gui->form_selected))
      cairo_set_line_width(cr, 2.0 / zoom_scale);
    else
      cairo_set_line_width(cr, 1.0 / zoom_scale);
    dt_draw_set_color_overlay(cr, 0.3, 0.8);
    cairo_stroke(cr);
  }


  // draw pivot points
  {
    cairo_set_dash(cr, dashed, 0, 0);
    if((gui->group_selected == index) && (gui->border_selected))
      cairo_set_line_width(cr, 2.0 / zoom_scale);
    else
      cairo_set_line_width(cr, 1.0 / zoom_scale);
    dt_draw_set_color_overlay(cr, 0.3, 0.8);

    // from start to end
    dt_draw_set_color_overlay(cr, 0.8, 0.8);
    cairo_move_to(cr, pivot_start_x, pivot_start_y);
    cairo_line_to(cr, pivot_end_x, pivot_end_y);
    cairo_stroke(cr);

    // start side of the gradient
    dt_draw_set_color_overlay(cr, 0.3, 0.8);
    cairo_arc(cr, pivot_start_x, pivot_start_y, 3.0f / zoom_scale, 0, 2.0f * M_PI);
    cairo_fill_preserve(cr);
    cairo_stroke(cr);

    // end side of the gradient
    cairo_arc(cr, pivot_end_x, pivot_end_y, 1.0f / zoom_scale, 0, 2.0f * M_PI);
    cairo_fill_preserve(cr);
    dt_draw_set_color_overlay(cr, 0.3, 0.8);
    cairo_stroke(cr);

    // draw arrow on the end of the gradient to clearly display the direction

    // size & width of the arrow
    const float arrow_angle = 0.25;
    const float arrow_length = 15.0 / zoom_scale;

    const float a_dx = anchor_x - pivot_end_x;
    const float a_dy = pivot_end_y - anchor_y;
    const float angle = atan2(a_dx, a_dy) - M_PI/2.0;

    const float arrow_x1 = pivot_end_x + (arrow_length * cos(angle + arrow_angle));
    const float arrow_x2 = pivot_end_x + (arrow_length * cos(angle - arrow_angle));
    const float arrow_y1 = pivot_end_y + (arrow_length * sin(angle + arrow_angle));
    const float arrow_y2 = pivot_end_y + (arrow_length * sin(angle - arrow_angle));

    dt_draw_set_color_overlay(cr, 0.8, 0.8);
    cairo_move_to(cr, pivot_end_x, pivot_end_y);
    cairo_line_to(cr, arrow_x1, arrow_y1);
    cairo_line_to(cr, arrow_x2, arrow_y2);
    cairo_line_to(cr, pivot_end_x, pivot_end_y);
    cairo_close_path(cr);
    cairo_fill_preserve(cr);
    cairo_stroke(cr);
  }
}


static int dt_gradient_get_points(dt_develop_t *dev, float x, float y, float rotation, float curvature,
                                  float **points, int *points_count)
{
  *points = NULL;
  *points_count = 0;

  const float wd = dev->preview_pipe->iwidth;
  const float ht = dev->preview_pipe->iheight;
  const float scale = sqrtf(wd * wd + ht * ht);
  const float distance = 0.1f * fminf(wd, ht);

  const float v = (-rotation / 180.0f) * M_PI;
  const float cosv = cos(v);
  const float sinv = sin(v);

  const int count = sqrtf(wd * wd + ht * ht) + 3;
  *points = dt_alloc_align(64, 2 * count * sizeof(float));
  if(*points == NULL) return 0;
  memset(*points, 0, 2 * count * sizeof(float));


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

  *points_count = 3;

  // we set the line point
  const float xstart = fabs(curvature) > 1.0f ? -sqrtf(1.0f / fabsf(curvature)) : -1.0f;
  const float xdelta = -2.0f * xstart / (count - 3);

  int in_frame = FALSE;
  for(int i = 3; i < count; i++)
  {
    const float xi = xstart + (i - 3) * xdelta;
    const float yi = curvature * xi * xi;
    const float xii = (cosv * xi + sinv * yi) * scale;
    const float yii = (sinv * xi - cosv * yi) * scale;
    const float xiii = xii + x * wd;
    const float yiii = yii + y * ht;

    // don't generate guide points if they extend too far beyond the image frame;
    // this is to avoid that modules like lens correction fail on out of range coordinates
    if(xiii < -wd || xiii > 2 * wd || yiii < -ht || yiii > 2 * ht)
    {
      if(!in_frame)
        continue;         // we have not entered the frame yet
      else
        break;            // we have left the frame
    }
    else
      in_frame = TRUE;    // we are in the frame

    (*points)[*points_count * 2] = xiii;
    (*points)[*points_count * 2 + 1] = yiii;
    (*points_count)++;
  }

  // and we transform them with all distorted modules
  if(dt_dev_distort_transform(dev, *points, *points_count)) return 1;

  // if we failed, then free all and return
  dt_free_align(*points);
  *points = NULL;
  *points_count = 0;
  return 0;
}

static int dt_gradient_get_points_border(dt_develop_t *dev, float x, float y, float rotation, float distance,
                                         float curvature, float **points, int *points_count)
{
  *points = NULL;
  *points_count = 0;

  float *points1 = NULL, *points2 = NULL;
  int points_count1 = 0, points_count2 = 0;

  const float wd = dev->preview_pipe->iwidth;
  const float ht = dev->preview_pipe->iheight;
  const float scale = sqrtf(wd * wd + ht * ht);

  const float v1 = (-(rotation - 90.0f) / 180.0f) * M_PI;

  const float x1 = (x * wd + distance * scale * cos(v1)) / wd;
  const float y1 = (y * ht + distance * scale * sin(v1)) / ht;

  const int r1 = dt_gradient_get_points(dev, x1, y1, rotation, curvature, &points1, &points_count1);

  const float v2 = (-(rotation + 90.0f) / 180.0f) * M_PI;

  const float x2 = (x * wd + distance * scale * cos(v2)) / wd;
  const float y2 = (y * ht + distance * scale * sin(v2)) / ht;

  const int r2 = dt_gradient_get_points(dev, x2, y2, rotation, curvature, &points2, &points_count2);

  int res = 0;

  if(r1 && r2 && points_count1 > 4 && points_count2 > 4)
  {
    int k = 0;
    *points = dt_alloc_align(64, 2 * ((points_count1 - 3) + (points_count2 - 3) + 1) * sizeof(float));
    if(*points == NULL) goto end;
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
    res = 1;
    goto end;
  }
  else if(r1 && points_count1 > 4)
  {
    int k = 0;
    *points = dt_alloc_align(64, 2 * ((points_count1 - 3)) * sizeof(float));
    if(*points == NULL) goto end;
    *points_count = points_count1 - 3;
    for(int i = 3; i < points_count1; i++)
    {
      (*points)[k * 2] = points1[i * 2];
      (*points)[k * 2 + 1] = points1[i * 2 + 1];
      k++;
    }
    res = 1;
    goto end;
  }
  else if(r2 && points_count2 > 4)
  {
    int k = 0;
    *points = dt_alloc_align(64, 2 * ((points_count2 - 3)) * sizeof(float));
    if(*points == NULL) goto end;
    *points_count = points_count2 - 3;

    for(int i = 3; i < points_count2; i++)
    {
      (*points)[k * 2] = points2[i * 2];
      (*points)[k * 2 + 1] = points2[i * 2 + 1];
      k++;
    }
    res = 1;
    goto end;
  }

end:
  dt_free_align(points1);
  dt_free_align(points2);

  return res;
}

static int dt_gradient_get_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                                int *width, int *height, int *posx, int *posy)
{
  const float wd = piece->pipe->iwidth, ht = piece->pipe->iheight;

  float points[8] = { 0.0f, 0.0f, wd, 0.0f, wd, ht, 0.0f, ht };

  // and we transform them with all distorted modules
  if(!dt_dev_distort_transform_plus(module->dev, piece->pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, points, 4)) return 0;

  // now we search min and max
  float xmin = 0.0f, xmax = 0.0f, ymin = 0.0f, ymax = 0.0f;
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

// caller needs to make sure that input remains within bounds
static inline float dt_gradient_lookup(const float *lut, const float i)
{
  const int bin0 = i;
  const int bin1 = i + 1;
  const float f = i - bin0;
  return lut[bin1] * f + lut[bin0] * (1.0f - f);
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

  // we create a buffer of grid points for later interpolation. mainly in order to reduce memory footprint
  const int w = *width;
  const int h = *height;
  const int px = *posx;
  const int py = *posy;
  const int grid = 8;
  const int gw = (w + grid - 1) / grid + 1;
  const int gh = (h + grid - 1) / grid + 1;

  float *points = dt_alloc_align(64, gw * gh * 2 * sizeof(float));
  if(points == NULL) return 0;

#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(grid, gh, gw, px, py) \
  shared(points)
#else
#pragma omp parallel for shared(points)
#endif
#endif
  for(int j = 0; j < gh; j++)
    for(int i = 0; i < gw; i++)
    {
      points[(j * gw + i) * 2] = (grid * i + px);
      points[(j * gw + i) * 2 + 1] = (grid * j + py);
    }

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] gradient draw took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  // we backtransform all these points
  if(!dt_dev_distort_backtransform_plus(module->dev, piece->pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, points, (size_t)gw * gh))
  {
    dt_free_align(points);
    return 0;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] gradient transform took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  // we calculate the mask at grid points and recycle point buffer to store results
  const float wd = piece->pipe->iwidth;
  const float ht = piece->pipe->iheight;
  const float hwscale = 1.0f / sqrtf(wd * wd + ht * ht);
  const float ihwscale = 1.0f / hwscale;
  const float v = (-gradient->rotation / 180.0f) * M_PI;
  const float sinv = sin(v);
  const float cosv = cos(v);
  const float xoffset = cosv * gradient->anchor[0] * wd + sinv * gradient->anchor[1] * ht;
  const float yoffset = sinv * gradient->anchor[0] * wd - cosv * gradient->anchor[1] * ht;
  const float compression = fmaxf(gradient->compression, 0.001f);
  const float normf = 1.0f / compression;
  const float curvature = gradient->curvature;
  const dt_masks_gradient_states_t state = gradient->state;

  const int lutmax = ceilf(4 * compression * ihwscale);
  const int lutsize = 2 * lutmax + 2;
  float *lut = dt_alloc_align(64, lutsize * sizeof(float));
  if(lut == NULL)
  {
    dt_free_align(points);
    return 0;
  }

#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(lutsize, lutmax, hwscale, state, normf, compression) \
  shared(lut)
#else
#pragma omp parallel for shared(points)
#endif
#endif
  for(int n = 0; n < lutsize; n++)
  {
    const float distance = (n - lutmax) * hwscale;
    const float value = 0.5f + 0.5f * ((state == DT_MASKS_GRADIENT_STATE_LINEAR) ? normf * distance: erff(distance / compression));
    lut[n] = (value < 0.0f) ? 0.0f : ((value > 1.0f) ? 1.0f : value);
  }

  // center lut around zero
  float *clut = lut + lutmax;


#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(gh, gw, sinv, cosv, xoffset, yoffset, hwscale, ihwscale, curvature, compression) \
  shared(points, clut)
#else
#pragma omp parallel for shared(points)
#endif
#endif
  for(int j = 0; j < gh; j++)
  {
    for(int i = 0; i < gw; i++)
    {
      const float x = points[(j * gw + i) * 2];
      const float y = points[(j * gw + i) * 2 + 1];

      const float x0 = (cosv * x + sinv * y - xoffset) * hwscale;
      const float y0 = (sinv * x - cosv * y - yoffset) * hwscale;

      const float distance = y0 - curvature * x0 * x0;

      points[(j * gw + i) * 2] = (distance <= -4.0f * compression) ? 0.0f :
                                    ((distance >= 4.0f * compression) ? 1.0f : dt_gradient_lookup(clut, distance * ihwscale));
    }
  }

  dt_free_align(lut);

  // we allocate the buffer
  *buffer = dt_alloc_align(64, w * h * sizeof(float));
  if(*buffer == NULL)
  {
    dt_free_align(points);
    return 0;
  }
  memset(*buffer, 0, w * h * sizeof(float));

// we fill the mask buffer by interpolation
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(h, w, gw, grid) \
  shared(buffer, points)
#else
#pragma omp parallel for shared(points, buffer)
#endif
#endif
  for(int j = 0; j < h; j++)
  {
    const int jj = j % grid;
    const int mj = j / grid;
    for(int i = 0; i < w; i++)
    {
      const int ii = i % grid;
      const int mi = i / grid;
      (*buffer)[j * w + i] = (points[(mj * gw + mi) * 2] * (grid - ii) * (grid - jj)
                              + points[(mj * gw + mi + 1) * 2] * ii * (grid - jj)
                              + points[((mj + 1) * gw + mi) * 2] * (grid - ii) * jj
                              + points[((mj + 1) * gw + mi + 1) * 2] * ii * jj) / (grid * grid);
    }
  }

  dt_free_align(points);

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] gradient fill took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
//   start2 = dt_get_wtime();

  return 1;
}


static int dt_gradient_get_mask_roi(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                    dt_masks_form_t *form, const dt_iop_roi_t *roi, float *buffer)
{
  double start2 = dt_get_wtime();

  // we get the gradient values
  const dt_masks_point_gradient_t *gradient = (dt_masks_point_gradient_t *)(g_list_first(form->points)->data);

  // we create a buffer of grid points for later interpolation. mainly in order to reduce memory footprint
  const int w = roi->width;
  const int h = roi->height;
  const int px = roi->x;
  const int py = roi->y;
  const float iscale = 1.0f / roi->scale;
  const int grid = CLAMP((10.0f*roi->scale + 2.0f) / 3.0f, 1, 4);
  const int gw = (w + grid - 1) / grid + 1;
  const int gh = (h + grid - 1) / grid + 1;

  float *points = dt_alloc_align(64, (size_t)gw * gh * 2 * sizeof(float));
  if(points == NULL) return 0;

#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(iscale, gh, gw, py, px, grid) \
  shared(points)
#else
#pragma omp parallel for shared(points)
#endif
#endif
  for(int j = 0; j < gh; j++)
    for(int i = 0; i < gw; i++)
    {

      const size_t index = (size_t)j * gw + i;
      points[index * 2] = (grid * i + px) * iscale;
      points[index * 2 + 1] = (grid * j + py) * iscale;
    }

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] gradient draw took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  // we backtransform all these points
  if(!dt_dev_distort_backtransform_plus(module->dev, piece->pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, points,
                                        (size_t)gw * gh))
  {
    dt_free_align(points);
    return 0;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] gradient transform took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  // we calculate the mask at grid points and recycle point buffer to store results
  const float wd = piece->pipe->iwidth;
  const float ht = piece->pipe->iheight;
  const float hwscale = 1.0f / sqrtf(wd * wd + ht * ht);
  const float ihwscale = 1.0f / hwscale;
  const float v = (-gradient->rotation / 180.0f) * M_PI;
  const float sinv = sin(v);
  const float cosv = cos(v);
  const float xoffset = cosv * gradient->anchor[0] * wd + sinv * gradient->anchor[1] * ht;
  const float yoffset = sinv * gradient->anchor[0] * wd - cosv * gradient->anchor[1] * ht;
  const float compression = fmaxf(gradient->compression, 0.001f);
  const float normf = 1.0f / compression;
  const float curvature = gradient->curvature;
  const dt_masks_gradient_states_t state = gradient->state;

  const int lutmax = ceilf(4 * compression * ihwscale);
  const int lutsize = 2 * lutmax + 2;
  float *lut = dt_alloc_align(64, lutsize * sizeof(float));
  if(lut == NULL)
  {
    dt_free_align(points);
    return 0;
  }

#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(lutsize, lutmax, hwscale, state, normf, compression) \
  shared(lut)
#else
#pragma omp parallel for shared(points)
#endif
#endif
  for(int n = 0; n < lutsize; n++)
  {
    const float distance = (n - lutmax) * hwscale;
    const float value = 0.5f + 0.5f * ((state == DT_MASKS_GRADIENT_STATE_LINEAR) ? normf * distance: erff(distance / compression));
    lut[n] = (value < 0.0f) ? 0.0f : ((value > 1.0f) ? 1.0f : value);
  }

  // center lut around zero
  float *clut = lut + lutmax;

#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(gh, gw, sinv, cosv, xoffset, yoffset, hwscale, ihwscale, curvature, compression) \
  shared(points, clut)
#else
#pragma omp parallel for shared(points)
#endif
#endif
  for(int j = 0; j < gh; j++)
  {
    for(int i = 0; i < gw; i++)
    {
      const size_t index = (size_t)j * gw + i;
      const float x = points[index * 2];
      const float y = points[index * 2 + 1];

      const float x0 = (cosv * x + sinv * y - xoffset) * hwscale;
      const float y0 = (sinv * x - cosv * y - yoffset) * hwscale;

      const float distance = y0 - curvature * x0 * x0;

      points[index * 2] = (distance <= -4.0f * compression) ? 0.0f : ((distance >= 4.0f * compression) ? 1.0f : dt_gradient_lookup(clut, distance * ihwscale));
    }
  }

  dt_free_align(lut);

// we fill the mask buffer by interpolation
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(h, w, grid, gw) \
  shared(buffer, points)
#else
#pragma omp parallel for shared(points, buffer)
#endif
#endif
  for(int j = 0; j < h; j++)
  {
    const int jj = j % grid;
    const int mj = j / grid;
    for(int i = 0; i < w; i++)
    {
      const int ii = i % grid;
      const int mi = i / grid;
      const size_t mindex = (size_t)mj * gw + mi;
      buffer[(size_t)j * w + i]
          = (points[mindex * 2] * (grid - ii) * (grid - jj) + points[(mindex + 1) * 2] * ii * (grid - jj)
             + points[(mindex + gw) * 2] * (grid - ii) * jj + points[(mindex + gw + 1) * 2] * ii * jj)
            / (grid * grid);
    }
  }

  dt_free_align(points);

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] gradient fill took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
//   start2 = dt_get_wtime();

  return 1;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
