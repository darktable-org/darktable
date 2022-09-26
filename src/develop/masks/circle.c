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

static void _circle_get_distance(float x, float y, float as, dt_masks_form_gui_t *gui, int index,
                                 int num_points, int *inside, int *inside_border, int *near, int *inside_source, float *dist)
{
  (void)num_points; // unused arg, keep compiler from complaining
  // initialise returned values
  *inside_source = 0;
  *inside = 0;
  *inside_border = 0;
  *near = -1;
  *dist = FLT_MAX;

  if(!gui) return;

  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return;

  // we first check if we are inside the source form
  if(dt_masks_point_in_form_exact(x, y, gpt->source, 1, gpt->source_count))
  {
    *inside_source = 1;
    *inside = 1;

    // distance from source center
    const float cx = x - gpt->source[0];
    const float cy = y - gpt->source[1];
    *dist = sqf(cx) + sqf(cy);

    return;
  }

  // distance from center

  const float cx = x - gpt->points[0];
  const float cy = y - gpt->points[1];
  *dist = sqf(cx) + sqf(cy);

  // we check if it's inside borders
  if(!dt_masks_point_in_form_exact(x, y, gpt->border, 1, gpt->border_count)) return;

  *inside = 1;
  *near = 0;

  // and we check if it's inside form
  *inside_border = !(dt_masks_point_in_form_near(x, y, gpt->points, 1, gpt->points_count, as, near));
}

static int _circle_events_mouse_scrolled(struct dt_iop_module_t *module, float pzx, float pzy, int up,
                                         uint32_t state, dt_masks_form_t *form, int parentid,
                                         dt_masks_form_gui_t *gui, int index)
{
  const float max_mask_border = form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE) ? 0.5f : 1.0f;
  const float max_mask_size = form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE) ? 0.5f : 1.0f;

  // add a preview when creating a circle
  if(gui->creation)
  {
    float masks_size = dt_conf_get_float(DT_MASKS_CONF(form->type, circle, size));

    if(dt_modifier_is(state, GDK_SHIFT_MASK))
    {
      float masks_border = dt_conf_get_float(DT_MASKS_CONF(form->type, circle, border));

      if(up && masks_border < max_mask_border)
        masks_border *= 1.0f / 0.97f;
      else if(!up && masks_border > 0.0005f)
        masks_border *= 0.97f;

      dt_conf_set_float(DT_MASKS_CONF(form->type, circle, border), masks_border);
      dt_toast_log(_("feather size: %3.2f%%"), (masks_border / masks_size)*100.0f);
    }
    else if(dt_modifier_is(state, 0))
    {
      if(up && masks_size < max_mask_size)
        masks_size *= 1.0f / 0.97f;
      else if(!up && masks_size > 0.001f)
        masks_size *= 0.97f;

      dt_conf_set_float(DT_MASKS_CONF(form->type, circle, size), masks_size);
      dt_toast_log(_("size: %3.2f%%"), masks_size*100.0f);
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
      dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *)((form->points)->data);
      // resize don't care where the mouse is inside a shape
      if(dt_modifier_is(state, GDK_SHIFT_MASK))
      {
        if(up && circle->border < max_mask_border)
          circle->border *= 1.0f / 0.97f;
        else if(!up && circle->border > 0.0005f)
          circle->border *= 0.97f;
        else
          return 1;
        dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
        dt_masks_gui_form_remove(form, gui, index);
        dt_masks_gui_form_create(form, gui, index, module);
        dt_conf_set_float(DT_MASKS_CONF(form->type, circle, border), circle->border);
        dt_toast_log(_("feather size: %3.2f%%"), (circle->border/circle->radius)*100.0f);
      }
      else if(gui->edit_mode == DT_MASKS_EDIT_FULL)
      {
        if(up && circle->radius < max_mask_size)
          circle->radius *= 1.0f / 0.97f;
        else if(!up && circle->radius > 0.001f)
          circle->radius *= 0.97f;
        else
          return 1;
        dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
        dt_masks_gui_form_remove(form, gui, index);
        dt_masks_gui_form_create(form, gui, index, module);
        dt_conf_set_float(DT_MASKS_CONF(form->type, circle, size), circle->radius);
        dt_toast_log(_("size: %3.2f%%"), circle->radius*100.0f);
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

static int _circle_events_button_pressed(struct dt_iop_module_t *module, float pzx, float pzy,
                                         double pressure, int which, int type, uint32_t state,
                                         dt_masks_form_t *form, int parentid, dt_masks_form_gui_t *gui, int index)
{
  if(!gui) return 0;

  if(!gui->creation)
  {
    dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
    if(!gpt) return 0;

    if(gui->edit_mode == DT_MASKS_EDIT_FULL)
    {
      if(gui->source_selected)
      {
        // we start the form dragging
        gui->source_dragging = TRUE;
        gui->dx = gpt->source[0] - gui->posx;
        gui->dy = gpt->source[1] - gui->posy;
        return 1;
      }

      gui->dx = gpt->points[0] - gui->posx;
      gui->dy = gpt->points[1] - gui->posy;

      if(gui->point_selected >= 1)
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
          && ((dt_modifier_is(state, GDK_CONTROL_MASK | GDK_SHIFT_MASK))
              || dt_modifier_is(state, GDK_SHIFT_MASK)))
  {
    // set some absolute or relative position for the source of the clone mask
    if(form->type & DT_MASKS_CLONE) dt_masks_set_source_pos_initial_state(gui, state, pzx, pzy);

    return 1;
  }
  else
  {
    dt_iop_module_t *crea_module = gui->creation_module;
    // we create the circle
    dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *)(malloc(sizeof(dt_masks_point_circle_t)));

    // we change the center value
    const float wd = darktable.develop->preview_pipe->backbuf_width;
    const float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = { pzx * wd, pzy * ht };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);
    circle->center[0] = pts[0] / darktable.develop->preview_pipe->iwidth;
    circle->center[1] = pts[1] / darktable.develop->preview_pipe->iheight;

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
    circle->radius = dt_conf_get_float(DT_MASKS_CONF(form->type, circle, size));
    circle->border = dt_conf_get_float(DT_MASKS_CONF(form->type, circle, border));
    form->points = g_list_append(form->points, circle);
    dt_masks_gui_form_save_creation(darktable.develop, crea_module, form, gui);

    if(crea_module)
    {
      // we save the move
      dt_dev_add_history_item(darktable.develop, crea_module, TRUE);
      // and we switch in edit mode to show all the forms
      // spots and retouch have their own handling of creation_continuous
      if(gui->creation_continuous
         && (strcmp(crea_module->so->op, "spots") == 0
             || strcmp(crea_module->so->op, "retouch") == 0))
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

    // if we draw a clone circle, we start now the source dragging
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

static int _circle_events_button_released(struct dt_iop_module_t *module, float pzx, float pzy, int which,
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
    // we get the circle
    dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *)((form->points)->data);

    // we end the form dragging
    gui->form_dragging = FALSE;

    // we change the center value
    const float wd = darktable.develop->preview_pipe->backbuf_width;
    const float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = { pzx * wd + gui->dx, pzy * ht + gui->dy };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);
    circle->center[0] = pts[0] / darktable.develop->preview_pipe->iwidth;
    circle->center[1] = pts[1] / darktable.develop->preview_pipe->iheight;
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
  else if(gui->source_dragging)
  {
    // we end the form dragging
    gui->source_dragging = FALSE;
    if(gui->scrollx != 0.0 || gui->scrolly != 0.0)
    {
      // if there's no dragging the source is calculated in _circle_events_button_pressed()
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
  else if(gui->point_dragging >= 1 || gui->point_border_dragging >= 1)
  {
    // we end the point dragging
    gui->point_dragging = gui->point_border_dragging = -1;

    dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
    // we save the updated shape
    dt_masks_update_image(darktable.develop);
  }

  return 0;
}

static int _circle_events_mouse_moved(struct dt_iop_module_t *module, float pzx, float pzy, double pressure,
                                      int which, dt_masks_form_t *form, int parentid,
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
      dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *)((form->points)->data);
      circle->center[0] = pts[0] / darktable.develop->preview_pipe->iwidth;
      circle->center[1] = pts[1] / darktable.develop->preview_pipe->iheight;
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
    const float max_mask_size = form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE) ? 0.5f : 1.0f;

    dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *)((form->points)->data);

    // we need the reference points
    dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
    if(!gpt) return 0;

    const int k = gui->point_dragging;
    const float xref = gpt->points[0];
    const float rx = gpt->points[k * 2] - xref;
    const float deltax = gui->posx + gui->dx - xref;

    gui->dx = xref - gui->posx;

    circle->radius = CLAMP(circle->radius * (1.0f + deltax / rx), 0.0005f, max_mask_size);

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index, module);
    dt_control_queue_redraw_center();
    return 1;
  }
  else if(gui->point_border_dragging >= 1)
  {
    const float max_mask_border = form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE) ? 0.5f : 1.0f;

    dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *)((form->points)->data);


    // we need the reference points
    dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
    if(!gpt) return 0;

    const int k = gui->point_border_dragging;
    const float xref = gpt->points[0];
    const float rx = gpt->border[k * 2] - xref;
    const float deltax = gui->posx + gui->dx - xref;

    gui->dx = xref - gui->posx;

    circle->border = CLAMP((circle->radius + circle->border) * (1.0f + deltax / rx) - circle->radius, 0.001f, max_mask_border);

    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index, module);
    dt_control_queue_redraw_center();
    return 1;
  }
  else if(!gui->creation)
  {
    const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
    const int closeup = dt_control_get_dev_closeup();
    const float zoom_scale = dt_dev_get_zoom_scale(darktable.develop, zoom, 1<<closeup, 1);
    const float as = DT_PIXEL_APPLY_DPI(5) / zoom_scale;
    const float x = pzx * darktable.develop->preview_pipe->backbuf_width;
    const float y = pzy * darktable.develop->preview_pipe->backbuf_height;
    int in, inb, near, ins;
    float dist;
    _circle_get_distance(x, y, as, gui, index, 0, &in, &inb, &near, &ins, &dist);
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

    // see if we are close to the anchor points
    gui->point_selected = -1;
    gui->point_border_selected = -1;
    if(gui->form_selected)
    {
      dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);

      // prefer border point over shape itself in case of near overlap for ease of pickup
      if(x - gpt->border[2] > -as && x - gpt->border[2] < as &&
         y - gpt->border[3] > -as && y - gpt->border[3] < as)
      {
        gui->point_border_selected = 1;
      }
      else if(x - gpt->points[2] > -as && x - gpt->points[2] < as &&
              y - gpt->points[3] > -as && y - gpt->points[3] < as)
      {
        gui->point_selected = 1;
      }
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

static void _circle_draw_lines(gboolean borders, gboolean source, cairo_t *cr, double *dashed, const int len,
                               const gboolean selected, const float zoom_scale, float *points,
                               const int points_count)
{
  if(points_count <= 6) return;

  cairo_set_line_width(cr, ((borders ? 2.0 : 3.0) + selected ? 2.0 : 0.0) / (borders || source ? 2.0 : 1.0)/zoom_scale);

  dt_draw_set_color_overlay(cr, FALSE, 0.8);
  cairo_set_dash(cr, dashed, len, 0);

  cairo_move_to(cr, points[2], points[3]);
  for(int i = 2; i < points_count; i++)
  {
    cairo_line_to(cr, points[i * 2], points[i * 2 + 1]);
  }
  cairo_line_to(cr, points[2], points[3]);

  cairo_stroke_preserve(cr);

  cairo_set_line_width(cr, (source ? 0.5 : 1.0) * (selected ? 2.0 : 1.0) / zoom_scale);
  dt_draw_set_color_overlay(cr, TRUE, 0.8);
  cairo_set_dash(cr, dashed, len, 4);
  cairo_stroke(cr);
}

static float *_points_to_transform(float x, float y, float radius, float wd, float ht, int *points_count)
{
  // how many points do we need?
  const float r = radius * MIN(wd, ht);
  const size_t l = (size_t)(2.0f * M_PI * r);
  // allocate buffer
  float *const restrict points = dt_alloc_align_float((l + 1) * 2);
  if(!points)
  {
    *points_count = 0;
    return NULL;
  }
  *points_count = l + 1;

  // now we set the points, first the center, then the circumference
  const float center_x = x * wd;
  const float center_y = y * ht;
  points[0] = center_x;
  points[1] = center_y;
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(l, points, center_x, center_y, r)      \
    schedule(static) if(l > 100) aligned(points:64)
#endif
  for(int i = 1; i < l + 1; i++)
  {
    const float alpha = (i - 1) * 2.0f * M_PI / (float)l;
    points[i * 2] = center_x + r * cosf(alpha);
    points[i * 2 + 1] = center_y + r * sinf(alpha);
  }
  return points;
}

static int _circle_get_points_source(dt_develop_t *dev, float x, float y, float xs, float ys, float radius,
                                     float radius2, float rotation, float **points, int *points_count,
                                     const dt_iop_module_t *module)
{
  (void)radius2; // keep compiler from complaining about unused arg
  (void)rotation;
  const float wd = dev->preview_pipe->iwidth;
  const float ht = dev->preview_pipe->iheight;

  // compute the points of the target (center and circumference of circle)
  // we get the point in RAW image reference
  *points = _points_to_transform(x, y, radius, wd, ht, points_count);
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
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(points_count, points, dx, dy)              \
    schedule(static) if(*points_count > 100) aligned(points:64)
#endif
      for(int i = 0; i < *points_count; i++)
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

static int _circle_get_points(dt_develop_t *dev, float x, float y, float radius, float radius2, float rotation,
                              float **points, int *points_count)
{
  (void)radius2; // keep compiler from complaining about unused arg
  (void)rotation;
  const float wd = dev->preview_pipe->iwidth;
  const float ht = dev->preview_pipe->iheight;

  // compute the points we need to transform (center and circumference of circle)
  *points = _points_to_transform(x, y, radius, wd, ht, points_count);
  if(!*points) return 0;

  // and transform them with all distorted modules
  if(dt_dev_distort_transform(dev, *points, *points_count)) return 1;

  // if we failed, then free all and return
  dt_free_align(*points);
  *points = NULL;
  *points_count = 0;
  return 0;
}

static void _circle_events_post_expose(cairo_t *cr, float zoom_scale, dt_masks_form_gui_t *gui, int index,
                                       int num_points)
{
  (void)num_points; // unused arg, keep compiler from complaining
  double dashed[] = { 4.0, 4.0 };
  dashed[0] /= zoom_scale;
  dashed[1] /= zoom_scale;
  const int len = sizeof(dashed) / sizeof(dashed[0]);
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);

  // add a preview when creating a circle
  // in creation mode
  if(gui->creation)
  {
    if(gui->guipoints_count == 0)
    {
      dt_masks_form_t *form = darktable.develop->form_visible;
      if(!form) return;

      // we get the default radius values
      float radius_a = dt_conf_get_float(DT_MASKS_CONF(form->type, circle, size));
      float radius_b = dt_conf_get_float(DT_MASKS_CONF(form->type, circle, border));
      radius_b += radius_a;

      // we get the circle center
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
      float x = pts[0] / darktable.develop->preview_pipe->iwidth;
      float y = pts[1] / darktable.develop->preview_pipe->iheight;

      // we get all the points, distorted if needed of the sample form
      float *points = NULL;
      int points_count = 0;
      float *border = NULL;
      int border_count = 0;
      int draw = _circle_get_points(darktable.develop, x, y, radius_a, 0.0, 0.0, &points, &points_count);
      if(draw && radius_a != radius_b)
      {
        draw = _circle_get_points(darktable.develop, x, y, radius_b, 0.0, 0.0, &border, &border_count);
      }

      // we draw the form and it's border
      cairo_save(cr);
      // we draw the main shape
      _circle_draw_lines(FALSE, FALSE, cr, dashed, len, FALSE, zoom_scale, points, points_count);
      // we draw the borders
      _circle_draw_lines(TRUE, FALSE, cr, dashed, len, FALSE, zoom_scale, border, border_count);
      cairo_restore(cr);

      // draw a cross where the source will be created
      if(form->type & DT_MASKS_CLONE)
      {
        x = 0.0f;
        y = 0.0f;
        dt_masks_calculate_source_pos_value(gui, DT_MASKS_CIRCLE, pzx, pzy, pzx, pzy, &x, &y, FALSE);
        dt_masks_draw_clone_source_pos(cr, zoom_scale, x, y);
      }

      if(points) dt_free_align(points);
      if(border) dt_free_align(border);
    }

    return;
  }

  if(!gpt) return;
  // we draw the main shape
  const gboolean selected = (gui->group_selected == index) && (gui->form_selected || gui->form_dragging);
  _circle_draw_lines(FALSE, FALSE, cr, dashed, 0, selected, zoom_scale, gpt->points, gpt->points_count);
  // we draw the borders
  if(gui->show_all_feathers || gui->group_selected == index)
  {
    _circle_draw_lines(TRUE, FALSE, cr, dashed, len, gui->border_selected, zoom_scale, gpt->border,
                       gpt->border_count);
          // _ellipse_point_transform(xref, yref, gpt->points[i * 2], gpt->points[i * 2 + 1], sinr, cosr, &x, &y);
    dt_masks_draw_anchor(cr, gui->point_dragging > 0 || gui->point_selected > 0, zoom_scale, gpt->points[2], gpt->points[3]);
      // _ellipse_point_transform(xref, yref, gpt->border[i * 2], gpt->border[i * 2 + 1], sinr, cosr, &x, &y);
    dt_masks_draw_anchor(cr, gui->point_border_dragging > 0 || gui->point_border_selected > 0, zoom_scale, gpt->border[2], gpt->border[3]);
  }

  // draw the source if any
  if(gpt->source_count > 6)
  {
    const float pr_d = darktable.develop->preview_downsampling;
    const float radius = fabs(gpt->points[2] - gpt->points[0]);

    // compute the dest inner circle intersection with the line from source center to dest center.
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

      // (arrowx,arrowy) is the point of intersection, we move it (factor 1.11) a bit farther than the
      // inner circle to avoid superposition.
      const float arrowx = gpt->points[0] + 1.11 * radius * cosf(cangle);
      const float arrowy = gpt->points[1] + 1.11 * radius * sinf(cangle);

      cairo_move_to(cr, gpt->source[0], gpt->source[1]); // source center
      cairo_line_to(cr, arrowx, arrowy);                 // dest border
      // then draw to line for the arrow itself
      const float arrow_scale = 6.0f * pr_d;
      cairo_move_to(cr, arrowx + arrow_scale * cosf(cangle + (0.4f)),
                    arrowy + arrow_scale * sinf(cangle + (0.4f)));
      cairo_line_to(cr, arrowx, arrowy);
      cairo_line_to(cr, arrowx + arrow_scale * cosf(cangle - (0.4f)),
                    arrowy + arrow_scale * sinf(cangle - (0.4f)));

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

    // we only the main shape for the source, no borders
    _circle_draw_lines(FALSE, TRUE, cr, dashed, 0, selected, zoom_scale, gpt->source, gpt->source_count);
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

static int _circle_get_points_border(dt_develop_t *dev, struct dt_masks_form_t *form, float **points,
                                     int *points_count, float **border, int *border_count, int source,
                                     const dt_iop_module_t *module)
{
  dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *)((form->points)->data);
  float x = 0.0f, y = 0.0f;
  x = circle->center[0], y = circle->center[1];
  if(source)
  {
    float xs = form->source[0], ys = form->source[1];
    return _circle_get_points_source(dev, x, y, xs, ys, circle->radius, circle->radius, 0, points, points_count,
                                     module);
  }
  else
  {
    if(form->functions->get_points(dev, x, y, circle->radius, circle->radius, 0, points, points_count))
    {
      if(border)
      {
        float outer_radius = circle->radius + circle->border;
        return form->functions->get_points(dev, x, y, outer_radius, outer_radius, 0, border, border_count);
      }
      else
        return 1;
    }
  }
  return 0;
}

static int _circle_get_source_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                   dt_masks_form_t *form, int *width, int *height, int *posx, int *posy)
{
  // we get the circle values
  dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *)((form->points)->data);
  float wd = piece->pipe->iwidth, ht = piece->pipe->iheight;

  // compute the points we need to transform (center and circumference of circle)
  const float outer_radius = circle->radius + circle->border;
  int num_points;
  float *const restrict points =
    _points_to_transform(form->source[0], form->source[1], outer_radius, wd, ht, &num_points);
  if(points == NULL)
    return 0;

  // and transform them with all distorted modules
  if(!dt_dev_distort_transform_plus(darktable.develop, piece->pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, points, num_points))
  {
    dt_free_align(points);
    return 0;
  }

  _bounding_box(points, num_points, width, height, posx, posy);
  dt_free_align(points);
  return 1;
}

static int _circle_get_area(const dt_iop_module_t *const restrict module,
                            const dt_dev_pixelpipe_iop_t *const restrict piece,
                            dt_masks_form_t *const restrict form,
                            int *width, int *height, int *posx, int *posy)
{
  // we get the circle values
  dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *)((form->points)->data);
  float wd = piece->pipe->iwidth, ht = piece->pipe->iheight;

  // compute the points we need to transform (center and circumference of circle)
  const float outer_radius = circle->radius + circle->border;
  int num_points;
  float *const restrict points =
    _points_to_transform(circle->center[0], circle->center[1], outer_radius, wd, ht, &num_points);
  if(points == NULL)
    return 0;

  // and transform them with all distorted modules
  if(!dt_dev_distort_transform_plus(module->dev, piece->pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, points, num_points))
  {
    dt_free_align(points);
    return 0;
  }

  _bounding_box(points, num_points, width, height, posx, posy);
  dt_free_align(points);
  return 1;
}

static int _circle_get_mask(const dt_iop_module_t *const restrict module,
                            const dt_dev_pixelpipe_iop_t *const restrict piece,
                            dt_masks_form_t *const restrict form,
                            float **buffer, int *width, int *height, int *posx, int *posy)
{
  double start2 = 0.0;
  if(darktable.unmuted & DT_DEBUG_PERF) start2 = dt_get_wtime();

  // we get the area
  if(!_circle_get_area(module, piece, form, width, height, posx, posy)) return 0;

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle area took %0.04f sec\n", form->name, dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we get the circle values
  dt_masks_point_circle_t *const restrict circle = (dt_masks_point_circle_t *)((form->points)->data);

  // we create a buffer of points with all points in the area
  const int w = *width, h = *height;
  float *const restrict points = dt_alloc_align_float((size_t)w * h * 2);
  if(points == NULL)
    return 0;

  const float pos_x = *posx;
  const float pos_y = *posy;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(h, w) \
  dt_omp_sharedconst(points, pos_x, pos_y) \
  schedule(static) if(h*w > 50000) num_threads(MIN(darktable.num_openmp_threads,(h*w)/20000))
#endif
  for(int i = 0; i < h; i++)
  {
    float *const restrict p = points + 2 * i * w;
    const float y = i + pos_y;
#ifdef _OPENMP
#pragma omp simd aligned(points : 64)
#endif
    for(int j = 0; j < w; j++)
    {
      p[2*j] = pos_x + j;
      p[2*j + 1] = y;
    }
  }
  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle draw took %0.04f sec\n", form->name, dt_get_wtime() - start2);

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
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle transform took %0.04f sec\n", form->name,
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
  float *const restrict ptbuffer = *buffer;
  const int wi = piece->pipe->iwidth, hi = piece->pipe->iheight;
  const int mindim = MIN(wi, hi);
  const float centerx = circle->center[0] * wi;
  const float centery = circle->center[1] * hi;
  const float radius2 = circle->radius * mindim * circle->radius * mindim;
  const float total2 = (circle->radius + circle->border) * mindim * (circle->radius + circle->border) * mindim;
  const float border2 = total2 - radius2;
  const float *const points_y = points + 1;
#ifdef _OPENMP
#pragma omp parallel for default(none)  \
  dt_omp_firstprivate(h, w) \
  dt_omp_sharedconst(border2, total2, centerx, centery, points, points_y, ptbuffer) \
  schedule(simd:static) if(h*w > 50000) num_threads(MIN(darktable.num_openmp_threads,(h*w)/20000))
#endif
  for(int i = 0 ; i < h*w; i++)
  {
    // find the square of the distance from the center
    const float l2 = sqf(points[2 * i] - centerx) + sqf(points_y[2 * i] - centery);
    // quadratic falloff between the circle's radius and the radius of the outside of the feathering
    const float ratio = (total2 - l2) / border2;
    // enforce 1.0 inside the circle and 0.0 outside the feathering
    const float f = CLIP(ratio);
    ptbuffer[i] = sqf(f);
  }

  dt_free_align(points);

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle fill took %0.04f sec\n", form->name, dt_get_wtime() - start2);

  return 1;
}


static int _circle_get_mask_roi(const dt_iop_module_t *const restrict module,
                                const dt_dev_pixelpipe_iop_t *const restrict piece,
                                dt_masks_form_t *const form, const dt_iop_roi_t *const roi,
                                float *const restrict buffer)
{
  double start1 = 0.0;
  double start2 = start1;

  if(darktable.unmuted & DT_DEBUG_PERF) start2 = start1 = dt_get_wtime();

  // we get the circle parameters
  dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *)((form->points)->data);
  const int wi = piece->pipe->iwidth, hi = piece->pipe->iheight;
  const float centerx = circle->center[0] * wi;
  const float centery = circle->center[1] * hi;
  const int mindim = MIN(wi, hi);
  const float radius2 = circle->radius * mindim * circle->radius * mindim;
  const float total = (circle->radius + circle->border) * mindim;
  const float total2 = total * total;
  const float border2 = total2 - radius2;

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
  memset(buffer, 0, sizeof(float) * w * h);

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle init took %0.04f sec\n", form->name, dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we look at the outer circle of the shape - no effects outside of this circle;
  // we need many points as we do not know how the circle might get distorted in the pixelpipe
  const size_t circpts = dt_masks_roundup(MIN(360, 2 * M_PI * total2), 8);
  float *const restrict circ = dt_alloc_align_float(circpts * 2);
  if(circ == NULL) return 0;

#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(circpts, centerx, centery, total) \
  dt_omp_sharedconst(circ) schedule(static) if(circpts/8 > 1000)
#else
#pragma omp parallel for shared(points) schedule(static)
#endif
#endif
  for(int n = 0; n < circpts / 8; n++)
  {
    const float phi = (2.0f * M_PI * n) / circpts;
    const float x = total * cosf(phi);
    const float y = total * sinf(phi);
    const float cx = centerx;
    const float cy = centery;
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
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle outline took %0.04f sec\n", form->name, dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

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
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle bounding box took %0.04f sec\n", form->name, dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // check if there is anything to do at all;
  // only if width and height of bounding box is 2 or greater the shape lies inside of roi and requires action
  if(bbw <= 1 || bbh <= 1)
    return 1;

  float *const restrict points = dt_alloc_align_float((size_t)bbw * bbh * 2);
  if(points == NULL) return 0;

  // we populate the grid points in module coordinates
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(iscale, bbxm, bbym, bbXM, bbYM, bbw, px, py, grid) \
  dt_omp_sharedconst(points) \
  schedule(static) collapse(2) if(bbw*bbh > 50000)
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
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle grid took %0.04f sec\n", form->name, dt_get_wtime() - start2);
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
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle transform took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we calculate the mask values at the transformed points;
  // for results: re-use the points array
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(bbh, bbw, centerx, centery, border2, total2) \
  dt_omp_sharedconst(points) \
  schedule(static) collapse(2) if(bbh*bbw > 50000) num_threads(MIN(darktable.num_openmp_threads,(h*w)/20000))
#else
#pragma omp parallel for shared(points)
#endif
#endif
  for(int j = 0; j < bbh; j++)
    for(int i = 0; i < bbw; i++)
    {
      const size_t index = (size_t)j * bbw + i;
      // find the square of the distance from the center
      const float l2 = sqf(points[2 * index] - centerx) + sqf(points[2 * index + 1] - centery);
      // quadratic falloff between the circle's radius and the radius of the outside of the feathering
      const float ratio = (total2 - l2) / border2;
      // enforce 1.0 inside the circle and 0.0 outside the feathering
      const float f = CLAMP(ratio, 0.0f, 1.0f);
      points[2*index] = f * f;
    }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle draw took %0.04f sec\n", form->name,
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
  dt_omp_sharedconst(buffer, points) schedule(static)
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

static GSList *_circle_setup_mouse_actions(const struct dt_masks_form_t *const form)
{
  GSList *lm = NULL;
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_SCROLL, 0, _("[CIRCLE] change size"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_SCROLL, GDK_SHIFT_MASK, _("[CIRCLE] change feather size"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_SCROLL, GDK_CONTROL_MASK, _("[CIRCLE] change opacity"));
  return lm;
}

static void _circle_sanitize_config(dt_masks_type_t type)
{
  dt_conf_get_and_sanitize_float(DT_MASKS_CONF(type, circle, size), 0.001f, 0.5f);
  dt_conf_get_and_sanitize_float(DT_MASKS_CONF(type, circle, border), 0.0005f, 0.5f);
}

static void _circle_set_form_name(struct dt_masks_form_t *const form, const size_t nb)
{
  snprintf(form->name, sizeof(form->name), _("circle #%d"), (int)nb);
}

static void _circle_set_hint_message(const dt_masks_form_gui_t *const gui, const dt_masks_form_t *const form,
                                     const int opacity, char *const restrict msgbuf, const size_t msgbuf_len)
{
  // circle has same controls on creation and on edit
  g_snprintf(msgbuf, msgbuf_len,
             _("<b>size</b>: scroll, <b>feather size</b>: shift+scroll\n"
               "<b>opacity</b>: ctrl+scroll (%d%%)"), opacity);
}

static void _circle_duplicate_points(dt_develop_t *dev, dt_masks_form_t *const base, dt_masks_form_t *const dest)
{
  (void)dev; // unused arg, keep compiler from complaining
  for(GList *pts = base->points; pts; pts = g_list_next(pts))
  {
    dt_masks_point_circle_t *pt = (dt_masks_point_circle_t *)pts->data;
    dt_masks_point_circle_t *npt = (dt_masks_point_circle_t *)malloc(sizeof(dt_masks_point_circle_t));
    memcpy(npt, pt, sizeof(dt_masks_point_circle_t));
    dest->points = g_list_append(dest->points, npt);
  }
}

static void _circle_modify_property(dt_masks_form_t *const form, dt_masks_property_t prop, float old_val, float new_val, float *sum, int *count, float *min, float *max)
{
  float ratio = (!old_val || !new_val) ? 1.0f : new_val / old_val;

  dt_masks_point_circle_t *circle = (form->points)->data;

  switch(prop)
  {
    case DT_MASKS_PROPERTY_SIZE:;
      const float max_mask_size = form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE) ? 0.5f : 1.0f;
      circle->radius = CLAMP(circle->radius * ratio, 0.001f, max_mask_size);
      *sum += circle->radius;
      *max = fminf(*max, max_mask_size / circle->radius);
      *min = fmaxf(*min, 0.001f / circle->radius);
      ++*count;
      break;
    case DT_MASKS_PROPERTY_FEATHER:;
      const float max_mask_border = form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE) ? 0.5f : 1.0f;
      circle->border = CLAMP(circle->border * ratio, 0.0005f, max_mask_border);
      *sum += circle->border / circle->radius;
      *max = fminf(*max, max_mask_border / circle->border);
      *min = fmaxf(*min, 0.0005f / circle->border);
      ++*count;
      break;
    default:;
  }
}

static void _circle_initial_source_pos(const float iwd, const float iht, float *x, float *y)
{
  const float radius = MIN(0.5f, dt_conf_get_float("plugins/darkroom/spots/circle_size"));

  *x = (radius * iwd);
  *y = -(radius * iht);
}

// The function table for circles.  This must be public, i.e. no "static" keyword.
const dt_masks_functions_t dt_masks_functions_circle = {
  .point_struct_size = sizeof(struct dt_masks_point_circle_t),
  .sanitize_config = _circle_sanitize_config,
  .setup_mouse_actions = _circle_setup_mouse_actions,
  .set_form_name = _circle_set_form_name,
  .set_hint_message = _circle_set_hint_message,
  .modify_property = _circle_modify_property,
  .duplicate_points = _circle_duplicate_points,
  .initial_source_pos = _circle_initial_source_pos,
  .get_distance = _circle_get_distance,
  .get_points = _circle_get_points,
  .get_points_border = _circle_get_points_border,
  .get_mask = _circle_get_mask,
  .get_mask_roi = _circle_get_mask_roi,
  .get_area = _circle_get_area,
  .get_source_area = _circle_get_source_area,
  .mouse_moved = _circle_events_mouse_moved,
  .mouse_scrolled = _circle_events_mouse_scrolled,
  .button_pressed = _circle_events_button_pressed,
  .button_released = _circle_events_button_released,
  .post_expose = _circle_events_post_expose
};



// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

