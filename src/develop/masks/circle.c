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

static void dt_circle_get_distance(float x, int y, float as, dt_masks_form_gui_t *gui, int index, int *inside,
                                   int *inside_border, int *near, int *inside_source)
{
  // initialise returned values
  *inside_source = 0;
  *inside = 0;
  *inside_border = 0;
  *near = -1;

  if(!gui) return;

  float yf = (float)y;
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return;

  // we first check if we are inside the source form
  if(dt_masks_point_in_form_exact(x,yf,gpt->source,1,gpt->source_count))
  {
    *inside_source = 1;
    *inside = 1;
    return;
  }

  // we check if it's inside borders
  if(!dt_masks_point_in_form_exact(x,yf,gpt->border,1,gpt->border_count)) return;

  *inside = 1;
  *near = 0;

  // and we check if it's inside form
  *inside_border = !(dt_masks_point_in_form_near(x,yf,gpt->points,1,gpt->points_count,as,near));
}

static int dt_circle_events_mouse_scrolled(struct dt_iop_module_t *module, float pzx, float pzy, int up,
                                           uint32_t state, dt_masks_form_t *form, int parentid,
                                           dt_masks_form_gui_t *gui, int index)
{
  const float max_mask_border = form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE) ? 0.5f : 1.0f;
  const float max_mask_size = form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE) ? 0.5f : 1.0f;

  // add a preview when creating a circle
  if(gui->creation)
  {
    if((state & GDK_SHIFT_MASK) == GDK_SHIFT_MASK)
    {
      float masks_border = 0.0f;

      if(form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE))
        masks_border = dt_conf_get_float("plugins/darkroom/spots/circle_border");
      else
        masks_border = dt_conf_get_float("plugins/darkroom/masks/circle/border");

      if(up && masks_border > 0.0005f)
        masks_border *= 0.97f;
      else if(!up && masks_border < max_mask_border)
        masks_border *= 1.0f / 0.97f;

      if(form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE))
        dt_conf_set_float("plugins/darkroom/spots/circle_border", masks_border);
      else
        dt_conf_set_float("plugins/darkroom/masks/circle/border", masks_border);
    }
    else if(state == 0)
    {
      float masks_size = 0.0f;

      if(form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE))
        masks_size = dt_conf_get_float("plugins/darkroom/spots/circle_size");
      else
        masks_size = dt_conf_get_float("plugins/darkroom/masks/circle/size");

      if(up && masks_size > 0.001f)
        masks_size *= 0.97f;
      else if(!up && masks_size < max_mask_size)
        masks_size *= 1.0f / 0.97f;

      if(form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE))
        dt_conf_set_float("plugins/darkroom/spots/circle_size", masks_size);
      else
        dt_conf_set_float("plugins/darkroom/masks/circle/size", masks_size);
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
    else
    {
      dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *)(g_list_first(form->points)->data);
      // resize don't care where the mouse is inside a shape
      if((state & GDK_SHIFT_MASK) == GDK_SHIFT_MASK)
      {
        if(up && circle->border > 0.0005f)
          circle->border *= 0.97f;
        else if(!up && circle->border < max_mask_border)
          circle->border *= 1.0f / 0.97f;
        else
          return 1;
        dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
        dt_masks_gui_form_remove(form, gui, index);
        dt_masks_gui_form_create(form, gui, index);
        if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
          dt_conf_set_float("plugins/darkroom/spots/circle_border", circle->border);
        else
          dt_conf_set_float("plugins/darkroom/masks/circle/border", circle->border);
      }
      else if(gui->edit_mode == DT_MASKS_EDIT_FULL)
      {
        if(up && circle->radius > 0.001f)
          circle->radius *= 0.97f;
        else if(!up && circle->radius < max_mask_size)
          circle->radius *= 1.0f / 0.97f;
        else
          return 1;
        dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
        dt_masks_gui_form_remove(form, gui, index);
        dt_masks_gui_form_create(form, gui, index);
        if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
          dt_conf_set_float("plugins/darkroom/spots/circle_size", circle->radius);
        else
          dt_conf_set_float("plugins/darkroom/masks/circle/size", circle->radius);
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

static int dt_circle_events_button_pressed(struct dt_iop_module_t *module, float pzx, float pzy,
                                           double pressure, int which, int type, uint32_t state,
                                           dt_masks_form_t *form, int parentid, dt_masks_form_gui_t *gui,
                                           int index)
{
  if(!gui) return 0;
  if(gui->source_selected && !gui->creation && gui->edit_mode == DT_MASKS_EDIT_FULL)
  {
    dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
    if(!gpt) return 0;
    // we start the form dragging
    gui->source_dragging = TRUE;
    gui->dx = gpt->source[0] - gui->posx;
    gui->dy = gpt->source[1] - gui->posy;
    return 1;
  }
  else if(gui->form_selected && !gui->creation && gui->edit_mode == DT_MASKS_EDIT_FULL)
  {
    dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
    if(!gpt) return 0;
    // we start the form dragging
    gui->form_dragging = TRUE;
    gui->dx = gpt->points[0] - gui->posx;
    gui->dy = gpt->points[1] - gui->posy;
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
    // we create the circle
    dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *)(malloc(sizeof(dt_masks_point_circle_t)));

    // we change the center value
    float wd = darktable.develop->preview_pipe->backbuf_width;
    float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = { pzx * wd, pzy * ht };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);
    circle->center[0] = pts[0] / darktable.develop->preview_pipe->iwidth;
    circle->center[1] = pts[1] / darktable.develop->preview_pipe->iheight;

    if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
    {
      circle->radius = dt_conf_get_float("plugins/darkroom/spots/circle_size");
      circle->border = dt_conf_get_float("plugins/darkroom/spots/circle_border");

      // calculate the source position
      if(form->type & DT_MASKS_CLONE)
      {
        dt_masks_set_source_pos_initial_value(gui, DT_MASKS_CIRCLE, form, pzx, pzy);
      }
      else
      {
        // not used by regular masks
        form->source[0] = form->source[1] = 0.0f;
      }
    }
    else
    {
      circle->radius = dt_conf_get_float("plugins/darkroom/masks/circle/size");
      circle->border = dt_conf_get_float("plugins/darkroom/masks/circle/border");
      // not used for masks
      form->source[0] = form->source[1] = 0.0f;
    }
    form->points = g_list_append(form->points, circle);
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
      gui->creation_module = NULL;
    }
    else
    {
      // we select the new form
      dt_dev_masks_selection_change(darktable.develop, form->formid, TRUE);
    }

    // if we draw a clone circle, we start now the source dragging
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

static int dt_circle_events_button_released(struct dt_iop_module_t *module, float pzx, float pzy, int which,
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
  if(gui->form_dragging)
  {
    // we get the circle
    dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *)(g_list_first(form->points)->data);

    // we end the form dragging
    gui->form_dragging = FALSE;

    // we change the center value
    float wd = darktable.develop->preview_pipe->backbuf_width;
    float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = { pzx * wd + gui->dx, pzy * ht + gui->dy };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);
    circle->center[0] = pts[0] / darktable.develop->preview_pipe->iwidth;
    circle->center[1] = pts[1] / darktable.develop->preview_pipe->iheight;
    dt_dev_add_masks_history_item(darktable.develop, module, TRUE);

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
  else if(gui->source_dragging)
  {
    // we end the form dragging
    gui->source_dragging = FALSE;
    if(gui->scrollx != 0.0 || gui->scrolly != 0.0)
    {
      // if there's no dragging the source is calculated in dt_circle_events_button_pressed()
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
    dt_dev_add_masks_history_item(darktable.develop, module, TRUE);

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

static int dt_circle_events_mouse_moved(struct dt_iop_module_t *module, float pzx, float pzy, double pressure,
                                        int which, dt_masks_form_t *form, int parentid,
                                        dt_masks_form_gui_t *gui, int index)
{
  if(gui->form_dragging || gui->source_dragging)
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
    int in, inb, near, ins;
    dt_circle_get_distance(pzx * darktable.develop->preview_pipe->backbuf_width,
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
    dt_control_queue_redraw_center();
    if(!gui->form_selected && !gui->border_selected) return 0;
    if(gui->edit_mode != DT_MASKS_EDIT_FULL) return 0;
    return 1;
  }
  // add a preview when creating a circle
  else if(gui->creation)
  {
    dt_control_queue_redraw_center();
    return 1;
  }

  return 0;
}

static void dt_circle_events_post_expose(cairo_t *cr, float zoom_scale, dt_masks_form_gui_t *gui, int index)
{
  double dashed[] = { 4.0, 4.0 };
  dashed[0] /= zoom_scale;
  dashed[1] /= zoom_scale;
  int len = sizeof(dashed) / sizeof(dashed[0]);
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);

  // add a preview when creating a circle
  // in creation mode
  if(gui->creation)
  {
    const float pr_d = darktable.develop->preview_downsampling;
    float iwd = darktable.develop->preview_pipe->iwidth;
    float iht = darktable.develop->preview_pipe->iheight;
    const float min_iwd_iht = pr_d * MIN(iwd,iht);
    if(gui->guipoints_count == 0)
    {
      dt_masks_form_t *form = darktable.develop->form_visible;
      if(!form) return;

      float radius1 = 0.0f, radius2 = 0.0f;
      if(form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE))
      {
        radius1 = dt_conf_get_float("plugins/darkroom/spots/circle_size");
        radius2 = dt_conf_get_float("plugins/darkroom/spots/circle_border");
      }
      else
      {
        radius1 = dt_conf_get_float("plugins/darkroom/masks/circle/size");
        radius2 = dt_conf_get_float("plugins/darkroom/masks/circle/border");
      }
      radius2 += radius1;
      radius1 *= min_iwd_iht;
      radius2 *= min_iwd_iht;

      float xpos = 0.0f, ypos = 0.0f;
      if((gui->posx == -1.0f && gui->posy == -1.0f) || gui->mouse_leaved_center)
      {
        const float zoom_x = dt_control_get_dev_zoom_x();
        const float zoom_y = dt_control_get_dev_zoom_y();
        xpos = (.5f + zoom_x) * darktable.develop->preview_pipe->backbuf_width;
        ypos = (.5f + zoom_y) * darktable.develop->preview_pipe->backbuf_height;
      }
      else
      {
        xpos = gui->posx;
        ypos = gui->posy;
      }

      cairo_save(cr);

      // draw circle
      cairo_set_dash(cr, dashed, 0, 0);
      cairo_set_line_width(cr, 3.0 / zoom_scale);
      dt_draw_set_color_overlay(cr, 0.3, 0.8);

      cairo_arc(cr, xpos, ypos, radius1, 0, 2.0 * M_PI);

      cairo_stroke_preserve(cr);
      cairo_set_line_width(cr, 1.0 / zoom_scale);
      dt_draw_set_color_overlay(cr, 0.8, 0.8);
      cairo_stroke(cr);

      // draw border
      cairo_set_dash(cr, dashed, len, 0);
      cairo_set_line_width(cr, 1.0 / zoom_scale);
      dt_draw_set_color_overlay(cr, 0.3, 0.8);

      cairo_arc(cr, xpos, ypos, radius2, 0, 2.0 * M_PI);

      cairo_stroke_preserve(cr);
      cairo_set_line_width(cr, 1.0 / zoom_scale);
      dt_draw_set_color_overlay(cr, 0.8, 0.8);
      cairo_set_dash(cr, dashed, len, 4);
      cairo_stroke(cr);

      // draw a cross where the source will be created
      if(form->type & DT_MASKS_CLONE)
      {
        float x = 0.0f, y = 0.0f;
        dt_masks_calculate_source_pos_value(gui, DT_MASKS_CIRCLE, xpos, ypos, xpos, ypos, &x, &y, FALSE);
        dt_masks_draw_clone_source_pos(cr, zoom_scale, x, y);
      }

      cairo_restore(cr);
    }

    return;
  }

  if(!gpt) return;
  float dx = 0.0f, dy = 0.0f, dxs = 0.0f, dys = 0.0f;
  if((gui->group_selected == index) && gui->form_dragging)
  {
    dx = gui->posx + gui->dx - gpt->points[0];
    dy = gui->posy + gui->dy - gpt->points[1];
  }
  if((gui->group_selected == index) && gui->source_dragging)
  {
    dxs = gui->posx + gui->dx - gpt->source[0];
    dys = gui->posy + gui->dy - gpt->source[1];
  }

  if(gpt->points_count > 6)
  {
    cairo_set_dash(cr, dashed, 0, 0);
    if((gui->group_selected == index) && (gui->form_selected || gui->form_dragging))
      cairo_set_line_width(cr, 5.0 / zoom_scale);
    else
      cairo_set_line_width(cr, 3.0 / zoom_scale);
    dt_draw_set_color_overlay(cr, 0.3, 0.8);
    cairo_move_to(cr, gpt->points[2] + dx, gpt->points[3] + dy);
    for(int i = 2; i < gpt->points_count; i++)
    {
      cairo_line_to(cr, gpt->points[i * 2] + dx, gpt->points[i * 2 + 1] + dy);
    }
    cairo_line_to(cr, gpt->points[2] + dx, gpt->points[3] + dy);
    cairo_stroke_preserve(cr);
    if((gui->group_selected == index) && (gui->form_selected || gui->form_dragging))
      cairo_set_line_width(cr, 2.0 / zoom_scale);
    else
      cairo_set_line_width(cr, 1.0 / zoom_scale);
    dt_draw_set_color_overlay(cr, 0.8, 0.8);
    cairo_stroke(cr);
  }

  // draw border
  if((gui->group_selected == index) && gpt->border_count > 6)
  {
    cairo_set_dash(cr, dashed, len, 0);
    if((gui->group_selected == index) && (gui->border_selected))
      cairo_set_line_width(cr, 2.0 / zoom_scale);
    else
      cairo_set_line_width(cr, 1.0 / zoom_scale);
    dt_draw_set_color_overlay(cr, 0.3, 0.8);

    cairo_move_to(cr, gpt->border[2] + dx, gpt->border[3] + dy);
    for(int i = 2; i < gpt->border_count; i++)
    {
      cairo_line_to(cr, gpt->border[i * 2] + dx, gpt->border[i * 2 + 1] + dy);
    }
    cairo_line_to(cr, gpt->border[2] + dx, gpt->border[3] + dy);

    cairo_stroke_preserve(cr);
    if((gui->group_selected == index) && (gui->border_selected))
      cairo_set_line_width(cr, 2.0 / zoom_scale);
    else
      cairo_set_line_width(cr, 1.0 / zoom_scale);
    dt_draw_set_color_overlay(cr, 0.8, 0.8);
    cairo_set_dash(cr, dashed, len, 4);
    cairo_stroke(cr);
  }

  // draw the source if any
  if(gpt->source_count > 6)
  {
    const float radius = fabs(gpt->points[2] - gpt->points[0]);

    // compute the dest inner circle intersection with the line from source center to dest center.
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

      // (arrowx,arrowy) is the point of intersection, we move it (factor 1.11) a bit farther than the
      // inner circle to avoid superposition.
      float arrowx = gpt->points[0] + 1.11 * radius * cos(cangle) + dx;
      float arrowy = gpt->points[1] + 1.11 * radius * sin(cangle) + dy;

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
      dt_draw_set_color_overlay(cr, 0.3, 0.8);
      cairo_stroke_preserve(cr);
      if((gui->group_selected == index) && (gui->form_selected || gui->form_dragging))
        cairo_set_line_width(cr, 1.0 / zoom_scale);
      else
        cairo_set_line_width(cr, 0.5 / zoom_scale);
      dt_draw_set_color_overlay(cr, 0.8, 0.8);
      cairo_stroke(cr);
    }

    // we draw the source
    cairo_set_dash(cr, dashed, 0, 0);
    if((gui->group_selected == index) && (gui->form_selected || gui->form_dragging))
      cairo_set_line_width(cr, 2.5 / zoom_scale);
    else
      cairo_set_line_width(cr, 1.5 / zoom_scale);
    dt_draw_set_color_overlay(cr, 0.3, 0.8);
    cairo_move_to(cr, gpt->source[2] + dxs, gpt->source[3] + dys);
    for(int i = 2; i < gpt->source_count; i++)
    {
      cairo_line_to(cr, gpt->source[i * 2] + dxs, gpt->source[i * 2 + 1] + dys);
    }
    cairo_line_to(cr, gpt->source[2] + dxs, gpt->source[3] + dys);
    cairo_stroke_preserve(cr);
    if((gui->group_selected == index) && (gui->form_selected || gui->form_dragging))
      cairo_set_line_width(cr, 1.0 / zoom_scale);
    else
      cairo_set_line_width(cr, 0.5 / zoom_scale);
    dt_draw_set_color_overlay(cr, 0.8, 0.8);
    cairo_stroke(cr);
  }
}

static int dt_circle_get_points(dt_develop_t *dev, float x, float y, float radius, float **points,
                                int *points_count)
{
  float wd = dev->preview_pipe->iwidth;
  float ht = dev->preview_pipe->iheight;

  // how many points do we need ?
  float r = radius * MIN(wd, ht);
  int l = MAX(100, (int)(2.0 * M_PI * r));

  // buffer allocations
  *points = dt_alloc_align(64, 2 * (l + 1) * sizeof(float));
  if(*points == NULL)
  {
    *points_count = 0;
    return 0;
  }
  *points_count = l + 1;

  // now we set the points
  (*points)[0] = x * wd;
  (*points)[1] = y * ht;
  for(int i = 1; i < l + 1; i++)
  {
    float alpha = (i - 1) * 2.0 * M_PI / (float)l;
    (*points)[i * 2] = (*points)[0] + r * cosf(alpha);
    (*points)[i * 2 + 1] = (*points)[1] + r * sinf(alpha);
  }

  // and we transform them with all distorted modules
  if(dt_dev_distort_transform(dev, *points, l + 1)) return 1;

  // if we failed, then free all and return
  dt_free_align(*points);
  *points = NULL;
  *points_count = 0;
  return 0;
}

static int dt_circle_get_source_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                     dt_masks_form_t *form, int *width, int *height, int *posx, int *posy)
{
  // we get the circle values
  dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *)(g_list_first(form->points)->data);
  float wd = piece->pipe->iwidth, ht = piece->pipe->iheight;

  float r = (circle->radius + circle->border) * MIN(wd, ht);
  int l = (int)(2.0 * M_PI * r);
  // buffer allocations
  float *points = dt_alloc_align(64, 2 * (l + 1) * sizeof(float));
  if(points == NULL)
    return 0;

  // now we set the points
  points[0] = form->source[0] * wd;
  points[1] = form->source[1] * ht;
  for(int i = 1; i < l + 1; i++)
  {
    float alpha = (i - 1) * 2.0 * M_PI / (float)l;
    points[i * 2] = points[0] + r * cosf(alpha);
    points[i * 2 + 1] = points[1] + r * sinf(alpha);
  }

  // and we transform them with all distorted modules
  if(!dt_dev_distort_transform_plus(darktable.develop, piece->pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, points, l + 1))
  {
    dt_free_align(points);
    return 0;
  }

  // now we search min and max
  float xmin = 0.0f, xmax = 0.0f, ymin = 0.0f, ymax = 0.0f;
  xmin = ymin = FLT_MAX;
  xmax = ymax = FLT_MIN;
  for(int i = 1; i < l + 1; i++)
  {
    xmin = fminf(points[i * 2], xmin);
    xmax = fmaxf(points[i * 2], xmax);
    ymin = fminf(points[i * 2 + 1], ymin);
    ymax = fmaxf(points[i * 2 + 1], ymax);
  }
  dt_free_align(points);
  // and we set values
  *posx = xmin;
  *posy = ymin;
  *width = (xmax - xmin);
  *height = (ymax - ymin);
  return 1;
}

static int dt_circle_get_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                              int *width, int *height, int *posx, int *posy)
{
  // we get the circle values
  dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *)(g_list_first(form->points)->data);
  float wd = piece->pipe->iwidth, ht = piece->pipe->iheight;

  float r = (circle->radius + circle->border) * MIN(wd, ht);
  int l = (int)(2.0 * M_PI * r);
  // buffer allocations
  float *points = dt_alloc_align(64, 2 * (l + 1) * sizeof(float));
  if(points == NULL)
    return 0;

  // now we set the points
  points[0] = circle->center[0] * wd;
  points[1] = circle->center[1] * ht;
  for(int i = 1; i < l + 1; i++)
  {
    float alpha = (i - 1) * 2.0 * M_PI / (float)l;
    points[i * 2] = points[0] + r * cosf(alpha);
    points[i * 2 + 1] = points[1] + r * sinf(alpha);
  }

  // and we transform them with all distorted modules
  if(!dt_dev_distort_transform_plus(module->dev, piece->pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, points, l + 1))
  {
    dt_free_align(points);
    return 0;
  }

  // now we search min and max
  float xmin = 0.0f, xmax = 0.0f, ymin = 0.0f, ymax = 0.0f;
  xmin = ymin = FLT_MAX;
  xmax = ymax = FLT_MIN;
  for(int i = 1; i < l + 1; i++)
  {
    xmin = fminf(points[i * 2], xmin);
    xmax = fmaxf(points[i * 2], xmax);
    ymin = fminf(points[i * 2 + 1], ymin);
    ymax = fmaxf(points[i * 2 + 1], ymax);
  }
  dt_free_align(points);

  // and we set values
  *posx = xmin;
  *posy = ymin;
  *width = (xmax - xmin);
  *height = (ymax - ymin);
  return 1;
}

static int dt_circle_get_mask(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                              float **buffer, int *width, int *height, int *posx, int *posy)
{
  double start2 = dt_get_wtime();

  // we get the area
  if(!dt_circle_get_area(module, piece, form, width, height, posx, posy)) return 0;

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle area took %0.04f sec\n", form->name, dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  // we get the circle values
  dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *)(g_list_first(form->points)->data);

  // we create a buffer of points with all points in the area
  int w = *width, h = *height;
  float *points = dt_alloc_align(64, w * h * 2 * sizeof(float));
  if(points == NULL)
    return 0;

  for(int i = 0; i < h; i++)
    for(int j = 0; j < w; j++)
    {
      points[(i * w + j) * 2] = (j + (*posx));
      points[(i * w + j) * 2 + 1] = (i + (*posy));
    }

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle draw took %0.04f sec\n", form->name, dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  // we back transform all this points
  if(!dt_dev_distort_backtransform_plus(module->dev, piece->pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, points, w * h))
  {
    dt_free_align(points);
    return 0;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle transform took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  // we allocate the buffer
  *buffer = dt_alloc_align(64, w * h * sizeof(float));
  if(*buffer == NULL)
  {
    dt_free_align(points);
    return 0;
  }

  // we populate the buffer
  int wi = piece->pipe->iwidth, hi = piece->pipe->iheight;
  float center[2] = { circle->center[0] * wi, circle->center[1] * hi };
  float radius2 = circle->radius * MIN(wi, hi) * circle->radius * MIN(wi, hi);
  float total2 = (circle->radius + circle->border) * MIN(wi, hi) * (circle->radius + circle->border)
                 * MIN(wi, hi);
  for(int i = 0; i < h; i++)
    for(int j = 0; j < w; j++)
    {
      float x = points[(i * w + j) * 2];
      float y = points[(i * w + j) * 2 + 1];
      float l2 = (x - center[0]) * (x - center[0]) + (y - center[1]) * (y - center[1]);
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
  dt_free_align(points);

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle fill took %0.04f sec\n", form->name, dt_get_wtime() - start2);
//   start2 = dt_get_wtime();

  return 1;
}


static int dt_circle_get_mask_roi(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                  dt_masks_form_t *form, const dt_iop_roi_t *roi, float *buffer)
{
  double start1 = dt_get_wtime();
  double start2 = start1;

  // we get the circle parameters
  dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *)(g_list_first(form->points)->data);
  const int wi = piece->pipe->iwidth, hi = piece->pipe->iheight;
  const float center[2] = { circle->center[0] * wi, circle->center[1] * hi };
  const float radius2 = circle->radius * MIN(wi, hi) * circle->radius * MIN(wi, hi);
  const float total = (circle->radius + circle->border) * MIN(wi, hi);
  const float total2 = total * total;

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

  // initialize output buffer with zero
  memset(buffer, 0, (size_t)w * h * sizeof(float));

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle init took %0.04f sec\n", form->name, dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  // we look at the outer circle of the shape - no effects outside of this circle;
  // we need many points as we do not know how the circle might get distorted in the pixelpipe
  const size_t circpts = dt_masks_roundup(MIN(360, 2 * M_PI * total2), 8);
  float *circ = dt_alloc_align(64, circpts * 2 * sizeof(float));
  if(circ == NULL) return 0;

#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(circpts, center, total) \
  shared(circ)
#else
#pragma omp parallel for shared(points)
#endif
#endif
  for(int n = 0; n < circpts / 8; n++)
  {
    const float phi = (2.0f * M_PI * n) / circpts;
    const float x = total * cos(phi);
    const float y = total * sin(phi);
    const float cx = center[0];
    const float cy = center[1];
    const int index_x = 2 * n * 8;
    const int index_y = 2 * n * 8 + 1;
    // take advantage of symmetry
    circ[index_x] = cx + x;
    circ[index_y] = cy + y;
    circ[index_x + 2] = cx + x;
    circ[index_y + 2] = cy - y;
    circ[index_x + 4] = cx - x;
    circ[index_y + 4] = cy + y;
    circ[index_x + 6] = cx - x;
    circ[index_y + 6] = cy - y;
    circ[index_x + 8] = cx + y;
    circ[index_y + 8] = cy + x;
    circ[index_x + 10] = cx + y;
    circ[index_y + 10] = cy - x;
    circ[index_x + 12] = cx - y;
    circ[index_y + 12] = cy + x;
    circ[index_x + 14] = cx - y;
    circ[index_y + 14] = cy - x;
  }

  // we transform the outer circle from input image coordinates to current point in pixelpipe
  if(!dt_dev_distort_transform_plus(module->dev, piece->pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, circ,
                                        circpts))
  {
    dt_free_align(circ);
    return 0;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle outline took %0.04f sec\n", form->name, dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  // we get the min/max values ...
  float xmin = FLT_MAX, ymin = FLT_MAX, xmax = FLT_MIN, ymax = FLT_MIN;
  for(int n = 0; n < circpts; n++)
  {
    // just in case that transform throws surprising values
    if(!(isnormal(circ[2 * n]) && isnormal(circ[2 * n + 1]))) continue;

    xmin = MIN(xmin, circ[2 * n]);
    xmax = MAX(xmax, circ[2 * n]);
    ymin = MIN(ymin, circ[2 * n + 1]);
    ymax = MAX(ymax, circ[2 * n + 1]);
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

  dt_free_align(circ);

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle bounding box took %0.04f sec\n", form->name, dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  // check if there is anything to do at all;
  // only if width and height of bounding box is 2 or greater the shape lies inside of roi and requires action
  if(bbw <= 1 || bbh <= 1)
    return 1;


  float *points = dt_alloc_align(64, (size_t)bbw * bbh * 2 * sizeof(float));
  if(points == NULL) return 0;

  // we populate the grid points in module coordinates
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(iscale, bbxm, bbym, bbXM, bbYM, bbw, px, py, grid) \
  shared(points)
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
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle grid took %0.04f sec\n", form->name, dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  // we back transform all these points to the input image coordinates
  if(!dt_dev_distort_backtransform_plus(module->dev, piece->pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, points,
                                        (size_t)bbw * bbh))
  {
    dt_free_align(points);
    return 0;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle transform took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
  start2 = dt_get_wtime();


  // we calculate the mask values at the transformed points;
  // for results: re-use the points array
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(bbh, bbw, center, radius2, total2) \
  shared(points)
#else
#pragma omp parallel for shared(points)
#endif
#endif
  for(int j = 0; j < bbh; j++)
    for(int i = 0; i < bbw; i++)
    {
      const size_t index = (size_t)j * bbw + i;
      const float x = points[index * 2];
      const float y = points[index * 2 + 1];
      const float l2 = (x - center[0]) * (x - center[0]) + (y - center[1]) * (y - center[1]);
      if(l2 < radius2)
        points[index * 2] = 1.0f;
      else if(l2 < total2)
      {
        const float f = (total2 - l2) / (total2 - radius2);
        points[index * 2] = f * f;
      }
      else
        points[index * 2] = 0.0f;
    }

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle draw took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
  start2 = dt_get_wtime();

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
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle fill took %0.04f sec\n", form->name, dt_get_wtime() - start2);
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle total render took %0.04f sec\n", form->name,
             dt_get_wtime() - start1);
  }

  return 1;
}



// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
