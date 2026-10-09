/*
    This file is part of darktable,
    Copyright (C) 2013-2026 darktable developers.

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
#include "develop/masks/group_internal.h"

// the AI object `fpt` is a path of, or NULL. In the flattened edit group a
// point's parentid is its own parent (dt_masks_group_ungroup), which for a
// path of an object is the object
static dt_masks_form_t *_object_of(const dt_masks_point_group_t *fpt)
{
  dt_masks_form_t *parent = dt_masks_get_from_id(darktable.develop, fpt->parentid);
  return (parent && (parent->type & DT_MASKS_OBJECT)) ? parent : NULL;
}

dt_masks_form_t *dt_masks_bundle_of(const dt_masks_point_group_t *fpt)
{
  dt_masks_form_t *object = _object_of(fpt);
  const dt_masks_form_gui_t *gui = darktable.develop->form_gui;
  return (object && gui && gui->entered_object == object->formid) ? NULL : object;
}

gboolean dt_masks_object_center(const dt_masks_form_t *object, double *cx, double *cy)
{
  double sx = 0.0, sy = 0.0;
  int npts = 0;
  for(GList *l = object->points; l; l = g_list_next(l))
  {
    const dt_masks_point_group_t *pt = l->data;
    const dt_masks_form_t *child = dt_masks_get_from_id(darktable.develop, pt->formid);
    if(!child) continue;
    for(GList *p = child->points; p; p = g_list_next(p))
    {
      const dt_masks_point_path_t *pp = p->data;
      sx += pp->corner[0];
      sy += pp->corner[1];
      npts++;
    }
  }
  if(npts == 0) return FALSE;
  *cx = sx / npts;
  *cy = sy / npts;
  return TRUE;
}

gboolean dt_masks_gui_step_object(dt_iop_module_t *module,
                                  dt_masks_form_gui_t *gui,
                                  const dt_mask_id_t hit_object,
                                  const gboolean primary,
                                  const gboolean double_click)
{
  if(!gui || !primary) return FALSE;
  const dt_mask_id_t was_entered = gui->entered_object;
  if(dt_is_valid_maskid(gui->entered_object) && hit_object != gui->entered_object)
    gui->entered_object = INVALID_MASKID;
  const gboolean step_in = double_click && dt_is_valid_maskid(hit_object)
                           && gui->entered_object != hit_object;
  if(step_in) gui->entered_object = hit_object;
  if(gui->entered_object != was_entered)
  {
    dt_control_queue_redraw_center();
    dt_iop_gui_blend_masks_entered_object_changed(module);
  }
  return step_in;
}

// after an edit of a whole AI object changed its paths' points, rebuild their
// outlines in gui->points: dt_masks_gui_form_create() always recomputes,
// unlike dt_masks_gui_form_test_create()
static void _bundle_refresh_children(dt_masks_form_t *scratch_grp,
                                     const dt_masks_form_t *bundle,
                                     dt_masks_form_gui_t *gui,
                                     const dt_iop_module_t *module)
{
  int pos = 0;
  for(GList *l = scratch_grp->points; l; l = g_list_next(l), pos++)
  {
    const dt_masks_point_group_t *pt = l->data;
    if(pt->parentid != bundle->formid) continue;
    dt_masks_form_t *child = dt_masks_get_from_id(darktable.develop, pt->formid);
    if(child) dt_masks_gui_form_create(child, gui, pos, module);
  }
}

// a scroll changed the paths of the whole AI object `bundle`, a member of the
// canvas group `form`: record it and show it
static void _bundle_scrolled(dt_iop_module_t *module,
                             dt_masks_form_t *form,
                             const dt_masks_form_t *bundle,
                             dt_masks_form_gui_t *gui)
{
  dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
  _bundle_refresh_children(form, bundle, gui, module);
  dt_masks_iop_update(module);
  dt_control_queue_redraw_center();
}

// scale the property `prop` of every path of the AI object `bundle` by one
// scroll step, as the panel's slider for it does
static void _bundle_scroll_property(dt_iop_module_t *module,
                                    dt_masks_form_t *form,
                                    dt_masks_form_t *bundle,
                                    dt_masks_form_gui_t *gui,
                                    const dt_masks_property_t prop,
                                    const int up)
{
  const float ratio = up ? 1.0f / 0.97f : 0.97f;
  float sum = 0.0f, minv = 0.0f, maxv = 0.0f;
  int count = 0;
  bundle->functions->modify_property(bundle, prop, 1.0f, ratio, &sum, &count, &minv, &maxv);
  _bundle_scrolled(module, form, bundle, gui);
}

static int _group_events_mouse_scrolled(dt_iop_module_t *module,
                                        const float pzx,
                                        const float pzy,
                                        const int up,
                                        const uint32_t state,
                                        dt_masks_form_t *form,
                                        const int unused1,
                                        dt_masks_form_gui_t *gui,
                                        const int unused)
{
  if(gui->group_edited >= 0)
  {
    // we get the form
    dt_masks_point_group_t *fpt = g_list_nth_data(form->points, gui->group_edited);
    // the canvas list can be rebuilt between a press and its release (a
    // selection change narrowing solo edit, say): the index then names nothing
    if(!fpt) return 0;
    dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
    if(!sel || !sel->functions) return 0;

    // an AI object's scroll gestures. dt_modifier_is() matches the modifiers
    // exactly, so none, ctrl, shift and ctrl+shift are told apart by one
    // if/else-if chain, as in path.c: separate conditions would overlap.
    // ctrl+scroll changes the object's opacity, as the panel's opacity control
    // for it does, even inside the object: a path's own opacity within the
    // object is shown nowhere
    dt_masks_form_t *object = _object_of(fpt);
    if(object && dt_modifier_is(state, GDK_CONTROL_MASK))
    {
      const float amount = up ? 0.05f : -0.05f;
      dt_masks_form_change_opacity(module, object, module->blend_params->mask_id, amount);
      dt_masks_iop_update(module);
      dt_control_queue_redraw_center();
      return 1;
    }
    dt_masks_form_t *bundle = dt_masks_bundle_of(fpt);
    if(bundle && !dt_modifier_is(state, GDK_CONTROL_MASK))
    {
      if(dt_modifier_is(state, GDK_SHIFT_MASK) && bundle->functions
         && bundle->functions->modify_property)
      {
        // shift+scroll feathers every path by one step, as the panel's
        // feather slider does
        _bundle_scroll_property(module, form, bundle, gui, DT_MASKS_PROPERTY_FEATHER, up);
        return 1;
      }
      else if(dt_modifier_is(state, GDK_CONTROL_MASK | GDK_SHIFT_MASK)
              && gui->edit_mode == DT_MASKS_EDIT_FULL && bundle->functions
              && bundle->functions->modify_property)
      {
        // ctrl+shift+scroll scales the object about its center by one step,
        // as path.c's centroid resize does for a path and the panel's size
        // slider does for the object
        _bundle_scroll_property(module, form, bundle, gui, DT_MASKS_PROPERTY_SIZE, up);
        return 1;
      }
      else if(gui->edit_mode == DT_MASKS_EDIT_FULL && bundle->functions
              && bundle->functions->resize && bundle->functions->resize_get)
      {
        // plain scroll grows or shrinks the object, as the panel's "shrink
        // or grow" slider does (_object_bundle_resize in object.c)
        const gboolean use_percent = !g_strcmp0(
          dt_conf_get_string_const("masks/path_resize_unit"), "% of path size");
        float amount = 0.0f;
        bundle->functions->resize_get(bundle, use_percent, &amount);
        const int new_amount = (int)roundf(amount) + (up ? 1 : -1);
        if(bundle->functions->resize(bundle, new_amount, use_percent))
          _bundle_scrolled(module, form, bundle, gui);
        return 1;
      }
    }
    // anything else goes to the shape's own handler
    return sel->functions->mouse_scrolled(module, pzx, pzy, up, state, sel, fpt->parentid,
                                          gui, gui->group_edited);
  }
  return 0;
}

static int _group_events_mouse_moved(dt_iop_module_t *module,
                                     const float pzx,
                                     const float pzy,
                                     const double pressure,
                                     const int which,
                                     const float zoom_scale,
                                     dt_masks_form_t *form,
                                     const int unused1,
                                     dt_masks_form_gui_t *gui,
                                     const int unused2);
static inline gboolean _is_handling_form(dt_masks_form_gui_t *gui);

static int _group_events_button_pressed(dt_iop_module_t *module,
                                        const float pzx,
                                        const float pzy,
                                        const double pressure,
                                        const int which,
                                        const int type,
                                        const uint32_t state,
                                        dt_masks_form_t *form,
                                        const int unused1,
                                        dt_masks_form_gui_t *gui,
                                        const int unused2)
{
  // the shape under the pointer is only picked as the pointer moves, so a
  // canvas rebuilt since knows none: the panel's rebuild after stepping into
  // an object is one. Pick it here, or the press lands on empty canvas, steps
  // out of the object and the next press drags the whole object
  if(gui->group_selected < 0 && !_is_handling_form(gui))
    _group_events_mouse_moved(module, pzx, pzy, pressure, state,
                              dt_dev_get_zoom_scale_full(), form, 0, gui, 0);

  // double-click on an AI object steps into it: its paths are then picked,
  // edited and removed one by one (see dt_masks_bundle_of). A click anywhere
  // else steps back out, and still does what it would have done
  const dt_masks_point_group_t *hit =
    gui->group_selected >= 0 ? g_list_nth_data(form->points, gui->group_selected) : NULL;
  const dt_masks_form_t *hit_object = hit ? _object_of(hit) : NULL;
  const dt_mask_id_t was_entered = gui->entered_object;
  const gboolean stepped_in =
    dt_masks_gui_step_object(module, gui,
                             hit_object ? hit_object->formid : INVALID_MASKID,
                             which == GDK_BUTTON_PRIMARY, type == GDK_2BUTTON_PRESS);
  if(stepped_in) return 1;

  // stepping out on empty canvas leaves the object selected, as one unit
  // again, rather than nothing (the click itself just cleared the selection,
  // see dt_masks_events_button_pressed)
  if(dt_is_valid_maskid(was_entered) && !dt_is_valid_maskid(gui->entered_object)
     && gui->group_selected < 0)
  {
    const dt_masks_form_t *object = dt_masks_get_from_id(darktable.develop, was_entered);
    if(object) dt_masks_select_form(module, object);
  }
  // outside any object, a plain click on empty canvas selects nothing, group
  // included
  else if(!dt_is_valid_maskid(was_entered) && gui->group_selected < 0
          && which == GDK_BUTTON_PRIMARY && type == GDK_BUTTON_PRESS)
    dt_iop_gui_blend_masks_clear_selection(module);

  if(gui->group_edited != gui->group_selected)
  {
    // we set the selected form in edit mode
    gui->group_edited = gui->group_selected;
    // we initialise some variable
    gui->dx = gui->dy = 0.0f;
    gui->form_selected = FALSE;
    gui->border_selected = FALSE;
    gui->form_dragging = FALSE;
    gui->form_rotating = FALSE;
    gui->source_rotating = FALSE;
    gui->counter_rotate_source = FALSE;
    gui->pivot_selected = FALSE;
    gui->point_border_selected = -1;
    gui->seg_selected = -1;
    gui->point_selected = -1;
    gui->feather_selected = -1;
    gui->point_border_dragging = -1;
    gui->seg_dragging = -1;
    gui->feather_dragging = -1;
    gui->point_dragging = -1;

    dt_control_queue_redraw_center();
    return 1;
  }
  if(gui->group_edited >= 0)
  {
    // we get the form
    dt_masks_point_group_t *fpt = g_list_nth_data(form->points, gui->group_edited);
    if(!fpt) return 0;
    dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
    if(!sel) return 0;
    if(sel->functions)
    {
      // did we asked for feather only?
      if(dt_modifier_is(state, GDK_SHIFT_MASK) ^ gui->select_only_border)
      {
        // then make sure we try to select the feather point
        gui->select_only_border = dt_modifier_is(state, GDK_SHIFT_MASK);
        // no dt_dev_get_zoom_scale_full(): global_mutex under history_mutex can deadlock
        dt_dev_viewport_t *port = &darktable.develop->full;
        const float zoom_scale =
          dt_dev_get_zoom_scale(port, port->zoom, 1 << port->closeup, TRUE);
        sel->functions->mouse_moved(module, pzx, pzy, pressure,
                                    which, zoom_scale, sel, fpt->parentid,
                                    gui, gui->group_edited);
      }

      const int ret =
        sel->functions->button_pressed(module, pzx, pzy, pressure, which, type, state,
                                       sel, fpt->parentid, gui, gui->group_edited);
      // the click selected one path of an AI object: select the object as one
      // unit, as its row in the panel does
      if(darktable.develop->mask_form_selected_id == sel->formid)
      {
        dt_masks_form_t *bundle = dt_masks_bundle_of(fpt);
        if(bundle) dt_masks_select_form(module, bundle);
      }
      return ret;
    }
  }
  return 0;
}

static int _group_events_button_released(dt_iop_module_t *module,
                                         const float pzx,
                                         const float pzy,
                                         const int which,
                                         const uint32_t state,
                                         dt_masks_form_t *form,
                                         const int unused1,
                                         dt_masks_form_gui_t *gui,
                                         const int unused2)
{
  if(gui->group_edited >= 0)
  {
    // we get the form
    dt_masks_point_group_t *fpt = g_list_nth_data(form->points, gui->group_edited);
    if(!fpt) return 0;
    dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
    if(!sel || !sel->functions) return 0;

    // rotating an AI object updates no panel control while the drag lasts
    // (_bundle_rotate_step), so they are updated once it ends
    const gboolean was_rotating = gui->form_rotating;
    const dt_masks_form_t *bundle = was_rotating ? dt_masks_bundle_of(fpt) : NULL;
    const int ret = sel->functions->button_released(
      module, pzx, pzy, which, state, sel, fpt->parentid, gui, gui->group_edited);
    if(bundle) dt_masks_iop_update(module);
    return ret;
  }
  return 0;
}

static inline gboolean _is_handling_form(dt_masks_form_gui_t *gui)
{
  return gui->form_dragging
    || gui->source_dragging
    || gui->gradient_toggling
    || gui->form_rotating
    || gui->source_rotating
    || (gui->point_edited != -1)
    || (gui->point_dragging != -1)
    || (gui->feather_dragging != -1)
    || (gui->point_border_dragging != -1)
    || (gui->seg_dragging != -1);
}

// a rotation drag on a path of an AI object rotates the whole object about
// its center, as the panel's rotation slider does
// (_object_bundle_modify_property), not the path about its own. Always
// handles the motion: returns 1
static int _bundle_rotate_step(dt_iop_module_t *module,
                               dt_masks_form_t *bundle,
                               dt_masks_form_t *scratch_grp,
                               dt_masks_form_gui_t *gui,
                               const float pzx,
                               const float pzy)
{
  float wd, ht, iwidth, iheight;
  dt_masks_get_image_size(&wd, &ht, &iwidth, &iheight);
  if(iwidth <= 0.0f || iheight <= 0.0f || wd <= 0.0f || ht <= 0.0f)
  {
    gui->dx = pzx;
    gui->dy = pzy;
    return 1;
  }

  // the object's center, as _object_bundle_modify_property takes it
  double cx, cy;
  if(!dt_masks_object_center(bundle, &cx, &cy))
  {
    gui->dx = pzx;
    gui->dy = pzy;
    return 1;
  }

  // the center on screen, where the pointer's angle is measured, so the drag
  // pivots where the object is shown
  float piv[2] = { (float)cx * iwidth, (float)cy * iheight };
  dt_dev_distort_transform(darktable.develop, piv, 1);

  const float cmx = pzx * wd, cmy = pzy * ht;
  const float pmx = gui->dx * wd, pmy = gui->dy * ht;
  float dv = atan2f(cmy - piv[1], cmx - piv[0]) - atan2f(pmy - piv[1], pmx - piv[0]);
  if(fabsf(dv) > M_PI_F) dv -= copysignf(DT_2PI_F, dv);
  const float dv_deg = rad2degf(dv);

  if(dv_deg != 0.0f && bundle->functions && bundle->functions->modify_property)
  {
    float sum = 0.0f, minv = 0.0f, maxv = 0.0f;
    int count = 0;
    bundle->functions->modify_property(bundle, DT_MASKS_PROPERTY_ROTATION, 0.0f, dv_deg,
                                       &sum, &count, &minv, &maxv);
    _bundle_refresh_children(scratch_grp, bundle, gui, module);
  }

  gui->dx = pzx;
  gui->dy = pzy;
  dt_control_queue_redraw_center();
  return 1;
}

// highlight the hovered shape's row in the panel (or its folded cluster's
// header), or clear it for INVALID_MASKID, when the hovered shape changes
static void _group_hover_form(dt_iop_module_t *module,
                              dt_masks_form_gui_t *gui,
                              const dt_mask_id_t formid)
{
  if(gui->canvas_hover_formid == formid) return;
  gui->canvas_hover_formid = formid;
  dt_iop_gui_blend_masks_hover_form(module, formid);
}

// does the cursor sit on any editable part of the form whose selection flags
// were just refreshed? mirrors what the per-form mouse_moved implementations set
static gboolean _form_under_cursor(const dt_masks_form_gui_t *gui)
{
  return gui->form_selected
    || gui->border_selected
    || gui->source_selected
    || gui->pivot_selected
    || gui->point_selected >= 0
    || gui->feather_selected >= 0
    || gui->seg_selected >= 0
    || gui->point_border_selected >= 0;
}

static int _group_events_mouse_moved(dt_iop_module_t *module,
                                     const float pzx,
                                     const float pzy,
                                     const double pressure,
                                     const int which,
                                     const float zoom_scale,
                                     dt_masks_form_t *form,
                                     const int unused1,
                                     dt_masks_form_gui_t *gui,
                                     const int unused2)
{
  const float as = dt_masks_sensitive_dist(zoom_scale);

  // the outlines hit-testing reads are made when the canvas is drawn: after a
  // rebuild (dt_masks_change_form_gui, the panel's rebuild after stepping into
  // an AI object among them) there are none until the next draw, and a hover
  // or press before it would find nothing under the pointer. A no-op while
  // they are current
  if(!gui->creation) dt_masks_gui_form_test_create(form, gui, module);

  // we first don't do anything if we are inside a scrolling session

  if(gui->scrollx != 0.0f && gui->scrolly != 0.0f)
  {
    const float as2 = 0.015f / zoom_scale;
    if((gui->scrollx - pzx < as2 && gui->scrollx - pzx > -as2)
       && (gui->scrolly - pzy < as2 && gui->scrolly - pzy > -as2))
      return 1;
    gui->scrollx = gui->scrolly = 0.0f;
  }

  // if a form is in edit mode and we are dragging, don't try to
  // select another form
  if(gui->group_edited >= 0 && _is_handling_form(gui))
  {
    // we get the form
    dt_masks_point_group_t *fpt = g_list_nth_data(form->points, gui->group_edited);
    if(!fpt) return 0;
    dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
    if(!sel) return 0;

    // a rotation drag on a path of an AI object rotates the object
    // (_bundle_rotate_step)
    if(gui->form_rotating)
    {
      dt_masks_form_t *rot_bundle = dt_masks_bundle_of(fpt);
      if(rot_bundle) return _bundle_rotate_step(module, rot_bundle, form, gui, pzx, pzy);
    }

    // dragging a path of an AI object by its body moves the whole object:
    // how far path.c moves the path's first point is applied to the other
    // paths. Node, feather and segment drags stay on the one path
    dt_masks_form_t *bundle = gui->form_dragging ? dt_masks_bundle_of(fpt) : NULL;
    float anchor_before[2] = { 0.0f, 0.0f };
    if(bundle && sel->points)
    {
      const dt_masks_point_path_t *p0 = sel->points->data;
      anchor_before[0] = p0->corner[0];
      anchor_before[1] = p0->corner[1];
    }

    int rep = 0;
    if(sel->functions)
      rep = sel->functions->mouse_moved(module, pzx, pzy, pressure, which, zoom_scale, sel, fpt->parentid,
                                        gui, gui->group_edited);

    if(bundle && sel->points)
    {
      const dt_masks_point_path_t *p0 = sel->points->data;
      const float dx = p0->corner[0] - anchor_before[0];
      const float dy = p0->corner[1] - anchor_before[1];
      if(dx != 0.0f || dy != 0.0f)
      {
        for(GList *l = bundle->points; l; l = g_list_next(l))
        {
          const dt_masks_point_group_t *cpt = l->data;
          if(cpt->formid == sel->formid) continue; // already moved above
          dt_masks_form_t *sib = dt_masks_get_from_id(darktable.develop, cpt->formid);
          if(!sib) continue;
          for(GList *pp = sib->points; pp; pp = g_list_next(pp))
          {
            dt_masks_point_path_t *pt = pp->data;
            pt->corner[0] += dx;
            pt->corner[1] += dy;
            pt->ctrl1[0] += dx;
            pt->ctrl1[1] += dy;
            pt->ctrl2[0] += dx;
            pt->ctrl2[1] += dy;
          }
        }
        _bundle_refresh_children(form, bundle, gui, module);
      }
    }

    if(rep) return 1;
    // if a point is in state editing, then we don't want that another
    // form can be selected
    if(gui->point_edited >= 0)
    {
      // the code below that syncs the row highlight is skipped while a node
      // holds the selection, so sync it here from the hover flags the call
      // above set. Otherwise canvas_hover_formid stays on the last shape and
      // keeps the panel's halo off (_flexi_shape_highlighted in gui/gtk.c)
      _group_hover_form(module, gui, _form_under_cursor(gui) ? sel->formid : INVALID_MASKID);
      return 0;
    }
  }

  // now we check if we are near a form
  int pos = 0;
  gui->form_selected = gui->border_selected = FALSE;
  gui->source_selected = gui->source_dragging = FALSE;
  gui->pivot_selected = FALSE;
  gui->feather_selected = -1;
  gui->point_edited = gui->point_selected = -1;
  gui->seg_selected = -1;
  gui->point_border_selected = -1;
  gui->group_edited = gui->group_selected = -1;
  gui->select_only_border = dt_modifier_is(which, GDK_SHIFT_MASK);

  dt_masks_form_t *sel = NULL;
  dt_masks_point_group_t *sel_fpt = NULL;
  int sel_pos = 0;
  float sel_dist = FLT_MAX;

  for(GList *fpts = form->points; fpts; fpts = g_list_next(fpts))
  {
    dt_masks_point_group_t *fpt = fpts->data;
    dt_masks_form_t *frm = dt_masks_get_from_id(darktable.develop, fpt->formid);
    // a hidden or disabled shape draws no outline and cannot be edited on the
    // canvas: it is never the shape under the cursor
    if(fpt->state & (DT_MASKS_STATE_HIDDEN | DT_MASKS_STATE_DISABLE))
    {
      pos++;
      continue;
    }
    int inside, inside_border, near, inside_source;
    float dist = FLT_MAX;
    inside = inside_border = inside_source = 0;
    near = -1;

    float wd, ht;
    dt_masks_get_image_size(&wd, &ht, NULL, NULL);
    const float xx = pzx * wd,
                yy = pzy * ht;
    if(frm && frm->functions && frm->functions->get_distance)
      frm->functions->get_distance(xx, yy, as, gui, pos, g_list_length(frm->points),
                                   &inside, &inside_border, &near, &inside_source, &dist);

    if(inside || inside_border || near >= 0 || inside_source)
    {
      if(sel_dist > dist)
      {
        sel = frm;
        sel_dist = dist;
        sel_pos = pos;
        sel_fpt = fpt;
      }
    }
    pos++;
  }

  if(sel && sel->functions)
  {
    gui->group_edited = gui->group_selected = sel_pos;
    _group_hover_form(module, gui, sel_fpt->formid);
    return sel->functions->mouse_moved(module, pzx, pzy, pressure, which, zoom_scale,
                                       sel, sel_fpt->parentid, gui, gui->group_edited);
  }

  // nothing under the cursor: drop the list row hover highlight
  _group_hover_form(module, gui, INVALID_MASKID);

  return 0;
}

// does the panel ask for this shape to be highlighted: its row or a cluster
// header holding it is hovered? `parentid` makes the row of an AI object
// highlight all of its paths
static gboolean _panel_hovered(const dt_masks_form_gui_t *gui,
                               const dt_mask_id_t formid,
                               const dt_mask_id_t parentid)
{
  for(const GList *l = gui->panel_hover_formids; l; l = g_list_next(l))
  {
    const dt_mask_id_t id = GPOINTER_TO_INT(l->data);
    if(id == formid || id == parentid) return TRUE;
  }
  return FALSE;
}

// is this shape soloed or solo-edited in the panel? Unlike a hover, this
// highlight stays whatever the pointer does
static gboolean _panel_soloed(const dt_masks_form_gui_t *gui,
                              const dt_mask_id_t formid,
                              const dt_mask_id_t parentid)
{
  for(const GList *l = gui->solo_formids; l; l = g_list_next(l))
  {
    const dt_mask_id_t id = GPOINTER_TO_INT(l->data);
    if(id == formid || id == parentid) return TRUE;
  }
  return FALSE;
}

void dt_group_events_post_expose(cairo_t *cr,
                                 const float zoom_scale,
                                 dt_masks_form_t *form,
                                 dt_masks_form_gui_t *gui)
{
  // base_sel is the canvas hover: the shape currently under the cursor (or -1).
  // A hovered shape always wins; the persistent panel selection is only drawn
  // when nothing at all is being hovered (no canvas hover, no list-row hover).
  const int base_sel = gui->group_selected;
  const gboolean any_list_hover = gui->panel_hover_formids != NULL;

  // a hovered path of an AI object highlights all its paths: the object acts
  // as one unit (dt_masks_bundle_of)
  dt_mask_id_t base_sel_bundle = INVALID_MASKID;
  if(base_sel >= 0)
  {
    const dt_masks_point_group_t *base_fpt = g_list_nth_data(form->points, base_sel);
    const dt_masks_form_t *bundle = base_fpt ? dt_masks_bundle_of(base_fpt) : NULL;
    if(bundle) base_sel_bundle = bundle->formid;
  }

  int pos = 0;
  for(GList *fpts = form->points; fpts; fpts = g_list_next(fpts))
  {
    dt_masks_point_group_t *fpt = fpts->data;
    dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
    if(!sel) return;
    // a hidden or disabled shape draws nothing, as it renders nothing. Only the
    // draw is skipped, so that pos stays in step with gui->points
    if(sel->functions && !(fpt->state & (DT_MASKS_STATE_HIDDEN | DT_MASKS_STATE_DISABLE)))
    {
      // decide whether this shape draws its own highlight (feather + anchors) by
      // posing as the selected group member for the duration of its post_expose
      // call only: a hovered list row/cluster member, else -- when nothing is
      // hovered -- the persistently selected shape.
      int eff = base_sel;
      // a row hover must look like a canvas hover, which takes two flags:
      // group_selected brings out the feather and the anchors, form_selected
      // the bold outline (the `selected` test in circle.c's post_expose and
      // the other shapes'). Both are set for this one call only, so nothing
      // outside the draw sees a hover the pointer never made
      gboolean bold = FALSE;
      // inside the object stepped into, its paths are highlighted one by one:
      // being soloed or selected as a whole no longer lights them all up, or
      // the one under the cursor could not be told apart
      const dt_mask_id_t unit =
        fpt->parentid == gui->entered_object ? INVALID_MASKID : fpt->parentid;
      if(_panel_hovered(gui, fpt->formid, fpt->parentid))
      {
        eff = pos;
        bold = TRUE;
      }
      else if(_panel_soloed(gui, fpt->formid, unit))
        eff = pos;
      else if(!any_list_hover && base_sel < 0
              && dt_is_valid_maskid(gui->panel_selected_formid)
              && (fpt->formid == gui->panel_selected_formid
                  || unit == gui->panel_selected_formid))
        eff = pos;
      else if(dt_is_valid_maskid(base_sel_bundle) && fpt->parentid == base_sel_bundle)
        eff = pos;
      const gboolean base_form_selected = gui->form_selected;
      gui->group_selected = eff;
      if(bold) gui->form_selected = TRUE;
      sel->functions->post_expose(cr, zoom_scale, gui, pos, g_list_length(sel->points));
      gui->group_selected = base_sel;
      if(bold) gui->form_selected = base_form_selected;
    }
    pos++;
  }
}

static void _inverse_mask(const dt_iop_module_t *const module,
                          const dt_dev_pixelpipe_iop_t *const piece,
                          dt_masks_form_t *const form,
                          float **buffer,
                          int *width,
                          int *height,
                          int *posx,
                          int *posy)
{
  // we create a new buffer
  const int wt = piece->iwidth;
  const int ht = piece->iheight;
  float *buf = dt_alloc_align_float((size_t)ht * wt);

  // we fill this buffer
  for(int yy = 0; yy < MIN(*posy, ht); yy++)
  {
    for(int xx = 0; xx < wt; xx++) buf[(size_t)yy * wt + xx] = 1.0f;
  }

  for(int yy = MAX(*posy, 0); yy < MIN(ht, (*posy) + (*height)); yy++)
  {
    for(int xx = 0; xx < MIN((*posx), wt); xx++)
      buf[(size_t)yy * wt + xx] = 1.0f;
    for(int xx = MAX((*posx), 0); xx < MIN(wt, (*posx) + (*width)); xx++)
      buf[(size_t)yy * wt + xx] = 1.0f - (*buffer)[((size_t)yy - (*posy)) * (*width) + xx - (*posx)];
    for(int xx = MAX((*posx) + (*width), 0); xx < wt; xx++)
      buf[(size_t)yy * wt + xx] = 1.0f;
  }

  for(int yy = MAX((*posy) + (*height), 0); yy < ht; yy++)
  {
    for(int xx = 0; xx < wt; xx++) buf[(size_t)yy * wt + xx] = 1.0f;
  }

  // we free the old buffer
  dt_free_align(*buffer);
  (*buffer) = buf;

  // we return correct values for positions;
  *posx = *posy = 0;
  *width = wt;
  *height = ht;
}

static int _group_get_mask(const dt_iop_module_t *const module,
                           const dt_dev_pixelpipe_iop_t *const piece,
                           dt_masks_form_t *const form,
                           float **buffer,
                           int *width,
                           int *height,
                           int *posx,
                           int *posy)
{
  // we allocate buffers and values
  const guint nb = g_list_length(form->points);
  if(nb == 0) return 0;

  float **bufs = calloc(nb, sizeof(float *));
  int *w = malloc(sizeof(int) * nb);
  int *h = malloc(sizeof(int) * nb);
  int *px = malloc(sizeof(int) * nb);
  int *py = malloc(sizeof(int) * nb);
  int *ok = malloc(sizeof(int) * nb);
  int *states = malloc(sizeof(int) * nb);
  float *op = malloc(sizeof(float) * nb);

  // and we get all masks
  int pos = 0;
  int nb_ok = 0;
  for(GList *fpts = form->points; fpts; fpts = g_list_next(fpts))
  {
    dt_masks_point_group_t *fpt = fpts->data;
    dt_masks_form_t *sel = dt_masks_get_from_id_ext(piece->pipe->forms, fpt->formid);
    // a hidden or disabled shape contributes nothing
    if(sel && !(fpt->state & (DT_MASKS_STATE_HIDDEN | DT_MASKS_STATE_DISABLE)))
    {
      ok[pos] = dt_masks_get_mask(module, piece, sel, &bufs[pos],
                                  &w[pos], &h[pos], &px[pos], &py[pos]);
      if(fpt->state & DT_MASKS_STATE_INVERSE)
      {
        double start = dt_get_wtime();
        _inverse_mask(module, piece, sel, &bufs[pos], &w[pos], &h[pos], &px[pos], &py[pos]);
        dt_print(DT_DEBUG_MASKS | DT_DEBUG_PERF,
                 "[masks %s] inverse took %0.04f sec",
                 sel->name, dt_get_lap_time(&start));
      }
      op[pos] = fpt->opacity;
      states[pos] = fpt->state;
      if(ok[pos]) nb_ok++;
    }
    else
    {
      // hidden or missing form: takes no slot in the composite
      ok[pos] = 0;
    }
    pos++;
  }
  if(nb_ok == 0) goto error;

  // now we get the min, max, width, height of the final mask
  int l = INT_MAX, r = INT_MIN, t = INT_MAX, b = INT_MIN;
  for(int i = 0; i < nb; i++)
  {
    if(!ok[i]) continue;
    l = MIN(l, px[i]);
    t = MIN(t, py[i]);
    r = MAX(r, px[i] + w[i]);
    b = MAX(b, py[i] + h[i]);
  }
  *posx = l;
  *posy = t;
  *width = r - l;
  *height = b - t;

  // we allocate the buffer
  *buffer = dt_alloc_align_float((size_t)(r - l) * (b - t));

  // and we copy each buffer inside, row by row
  // the first *visible* shape always composites as a plain copy onto the
  // (uninitialized) buffer, whatever its explicit operator, so that no
  // operator reads the uninitialized values
  gboolean first_visible = TRUE;
  for(int i = 0; i < nb; i++)
  {
    if(!ok[i]) continue; // hidden/missing form: nothing to composite
    double start = dt_get_debug_wtime();
    if(!first_visible && (states[i] & (DT_MASKS_STATE_UNION | DT_MASKS_STATE_SUM)))
    {
      for(int y = 0; y < h[i]; y++)
      {
        for(int x = 0; x < w[i]; x++)
        {
          (*buffer)[(py[i] + y - t) * (r - l) + px[i] + x - l]
              = fmaxf((*buffer)[(py[i] + y - t) * (r - l) + px[i] + x - l],
                      bufs[i][y * w[i] + x] * op[i]);
        }
      }
    }
    else if(!first_visible && (states[i] & DT_MASKS_STATE_INTERSECTION))
    {
      for(int y = 0; y < b - t; y++)
      {
        for(int x = 0; x < r - l; x++)
        {
          const float b1 = (*buffer)[y * (r - l) + x];
          float b2 = 0.0f;
          if(y + t - py[i] >= 0
             && y + t - py[i] < h[i]
             && x + l - px[i] >= 0
             && x + l - px[i] < w[i])
            b2 = bufs[i][(y + t - py[i]) * w[i] + x + l - px[i]];
          if(b1 > 0.0f && b2 > 0.0f)
            (*buffer)[y * (r - l) + x] = fminf(b1, b2 * op[i]);
          else
            (*buffer)[y * (r - l) + x] = 0.0f;
        }
      }
    }
    else if(!first_visible && (states[i] & DT_MASKS_STATE_DIFFERENCE))
    {
      for(int y = 0; y < h[i]; y++)
      {
        for(int x = 0; x < w[i]; x++)
        {
          const float b1 = (*buffer)[(py[i] + y - t) * (r - l) + px[i] + x - l];
          const float b2 = bufs[i][y * w[i] + x] * op[i];
          if(b1 > 0.0f && b2 > 0.0f)
            (*buffer)[(py[i] + y - t) * (r - l) + px[i] + x - l] = b1 * (1.0f - b2);
        }
      }
    }
    else if(!first_visible && (states[i] & DT_MASKS_STATE_EXCLUSION))
    {
      for(int y = 0; y < h[i]; y++)
      {
        for(int x = 0; x < w[i]; x++)
        {
          const float b1 = (*buffer)[(py[i] + y - t) * (r - l) + px[i] + x - l];
          const float b2 = bufs[i][y * w[i] + x] * op[i];
          if(b1 > 0.0f && b2 > 0.0f)
            (*buffer)[(py[i] + y - t) * (r - l) + px[i] + x - l] =
              fmaxf((1.0f - b1) * b2, b1 * (1.0f - b2));
          else
            (*buffer)[(py[i] + y - t) * (r - l) + px[i] + x - l]
                = fmaxf((*buffer)[(py[i] + y - t) * (r - l) + px[i] + x - l],
                        bufs[i][y * w[i] + x] * op[i]);
        }
      }
    }
    else // if we are here, this mean that we just have to copy the shape and null other parts
    {
      for(int y = 0; y < b - t; y++)
      {
        for(int x = 0; x < r - l; x++)
        {
          float b2 = 0.0f;
          if(y + t - py[i] >= 0
             && y + t - py[i] < h[i]
             && x + l - px[i] >= 0
             && x + l - px[i] < w[i])
            b2 = bufs[i][(y + t - py[i]) * w[i] + x + l - px[i]];
          (*buffer)[y * (r - l) + x] = b2 * op[i];
        }
      }
    }

    dt_print(DT_DEBUG_MASKS | DT_DEBUG_PERF,
             "[masks %d] combine took %0.04f sec",
             i, dt_get_lap_time(&start));
    first_visible = FALSE;
  }

  free(op);
  free(states);
  free(ok);
  free(py);
  free(px);
  free(h);
  free(w);
  for(int i = 0; i < nb; i++) dt_free_align(bufs[i]);
  free(bufs);
  return 1;

error:
  free(op);
  free(states);
  free(ok);
  free(py);
  free(px);
  free(h);
  free(w);
  for(int i = 0; i < nb; i++) dt_free_align(bufs[i]);
  free(bufs);
  return 0;
}

void dt_masks_combine_maximum(float *const restrict dest,
                              float *const restrict newmask,
                              const size_t npixels,
                              const float opacity,
                              const int inverted)
{
  if(inverted)
  {
    DT_OMP_FOR_SIMD(dt_omp_sharedconst(dest, newmask) aligned(dest, newmask : 64))
    for(size_t index = 0; index < npixels; index++)
    {
      const float mask = opacity * (1.0f - newmask[index]);
      dest[index] = MAX(dest[index], mask);
    }
  }
  else
  {
    DT_OMP_FOR_SIMD(aligned(dest, newmask : 64))
    for(size_t index = 0; index < npixels; index++)
    {
      const float mask = opacity * newmask[index];
      dest[index] = MAX(dest[index], mask);
    }
  }
}

void dt_masks_combine_minimum(float *const restrict dest,
                              float *const restrict newmask,
                              const size_t npixels,
                              const float opacity,
                              const int inverted)
{
  if(inverted)
  {
    DT_OMP_FOR_SIMD(aligned(dest, newmask : 64))
    for(size_t index = 0; index < npixels; index++)
    {
      const float mask = opacity * (1.0f - newmask[index]);
      dest[index] = MIN(MAX(dest[index], 0.0f), MAX(mask, 0.0f));
    }
  }
  else
  {
    DT_OMP_FOR_SIMD(aligned(dest, newmask : 64))
    for(size_t index = 0; index < npixels; index++)
    {
      const float mask = opacity * newmask[index];
      dest[index] = MIN(MAX(dest[index], 0.0f), MAX(mask, 0.0f));
    }
  }
}

DT_OMP_DECLARE_SIMD()
static inline int both_positive(const float val1, const float val2)
{
  // this needs to be a separate inline function to convince the compiler to vectorize
  return (val1 > 0.0f) && (val2 > 0.0f);
}

void dt_masks_combine_difference(float *const restrict dest,
                                 float *const restrict newmask,
                                 const size_t npixels,
                                 const float opacity,
                                 const int inverted)
{
  if(inverted)
  {
    DT_OMP_FOR_SIMD(aligned(dest, newmask : 64))
    for(size_t index = 0; index < npixels; index++)
    {
      const float mask = opacity * (1.0f - newmask[index]);
      dest[index] *= (1.0f - mask * both_positive(dest[index],mask));
    }
  }
  else
  {
    DT_OMP_FOR_SIMD(aligned(dest, newmask : 64))
    for(size_t index = 0; index < npixels; index++)
    {
      const float mask = opacity * newmask[index];
      dest[index] *= (1.0f - mask * both_positive(dest[index],mask));
    }
  }
}

void dt_masks_combine_sum(float *const restrict dest,
                          float *const restrict newmask,
                          const size_t npixels,
                          const float opacity,
                          const int inverted)
{
  if(inverted)
  {
    DT_OMP_FOR_SIMD(aligned(dest, newmask : 64))
    for(size_t index = 0; index < npixels; index++)
    {
      const float mask = opacity * (1.0f - newmask[index]);
      dest[index] = MIN(1.0f, dest[index] + mask);
    }
  }
  else
  {
    DT_OMP_FOR_SIMD(aligned(dest, newmask : 64))
    for(size_t index = 0; index < npixels; index++)
    {
      const float mask = opacity * newmask[index];
      dest[index] = MIN(1.0f, dest[index] + mask);
    }
  }
}

void dt_masks_combine_exclusion(float *const restrict dest,
                                float *const restrict newmask,
                                const size_t npixels,
                                const float opacity,
                                const int inverted)
{
  if(inverted)
  {
    DT_OMP_FOR_SIMD(aligned(dest, newmask : 64))
    for(size_t index = 0; index < npixels; index++)
    {
      const float mask = opacity * (1.0f - newmask[index]);
      const float pos = both_positive(dest[index], mask);
      const float neg = (1.0f - pos);
      const float b1 = dest[index];
      dest[index] = pos * MAX((1.0f - b1) * mask,
                              b1 * (1.0f - mask)) + neg * MAX(b1, mask);
    }
  }
  else
  {
    DT_OMP_FOR_SIMD(aligned(dest, newmask : 64))
    for(size_t index = 0; index < npixels; index++)
    {
      const float mask = opacity * newmask[index];
      const float pos = both_positive(dest[index], mask);
      const float neg = (1.0f - pos);
      const float b1 = dest[index];
      dest[index] = pos * MAX((1.0f - b1) * mask, b1 * (1.0f - mask)) + neg * MAX(b1, mask);
    }
  }
}

void dt_masks_combine_product(float *const restrict dest,
                              float *const restrict newmask,
                              const size_t npixels,
                              const float opacity,
                              const int inverted)
{
  // multiply the accumulator by this shape, as a classic multi-channel
  // parametric mask combines its channels. Onto the empty base this gives 0,
  // as intersection does
  if(inverted)
  {
    DT_OMP_FOR_SIMD(aligned(dest, newmask : 64))
    for(size_t index = 0; index < npixels; index++)
    {
      const float mask = opacity * (1.0f - newmask[index]);
      dest[index] *= mask;
    }
  }
  else
  {
    DT_OMP_FOR_SIMD(aligned(dest, newmask : 64))
    for(size_t index = 0; index < npixels; index++)
    {
      const float mask = opacity * newmask[index];
      dest[index] *= mask;
    }
  }
}

// soft union ("screen"): a + b - ab, a flexi group operator. Like union it is
// associative and commutative, with the empty mask as identity, but not
// idempotent, so feathered overlaps build up smoothly instead of leaving the
// crease max() makes
void dt_masks_combine_screen(float *const restrict dest,
                             float *const restrict newmask,
                             const size_t npixels,
                             const float opacity,
                             const int inverted)
{
  if(inverted)
  {
    DT_OMP_FOR_SIMD(aligned(dest, newmask : 64))
    for(size_t index = 0; index < npixels; index++)
    {
      const float mask = opacity * (1.0f - newmask[index]);
      const float d = dest[index];
      dest[index] = d + mask - d * mask;
    }
  }
  else
  {
    DT_OMP_FOR_SIMD(aligned(dest, newmask : 64))
    for(size_t index = 0; index < npixels; index++)
    {
      const float mask = opacity * newmask[index];
      const float d = dest[index];
      dest[index] = d + mask - d * mask;
    }
  }
}

// Flexi group fold (flexi masks only). A group's list starts with its marker,
// which holds the group's settings, followed by its members. The members fold
// into the mask in list order with the group's operator (see the list below);
// the result is then refined once with the refinement the marker holds,
// inverted and scaled. A nested group renders through here as a member of its
// parent. A group with no visible member contributes nothing: the caller skips
// it, so an empty intersection group never blanks the mask. Classic masks render
// through the sequential fold below
static int _group_get_mask_roi_flexi(const dt_iop_module_t *const restrict module,
                                     const dt_dev_pixelpipe_iop_t *const restrict piece,
                                     dt_masks_form_t *const form,
                                     const dt_iop_roi_t *const roi,
                                     float *const restrict buffer)
{
  // the group's marker heads its list, holding the group's settings
  // (dev-doc/masks_data_model.md)
  if(!form->points || !dt_masks_point_is_marker(form->points->data)) return 0;
  const dt_masks_point_group_t *const head = form->points->data;
  GList *fpts = form->points->next;

  const int width = roi->width;
  const int height = roi->height;
  const size_t npixels = (size_t)width * height;

  float *const restrict bufs = dt_alloc_align_float(npixels); // one raw member
  if(bufs == NULL) return 0;

  // the refinements previewed as off: the pipe's snapshot, committed with the
  // params (dt_masks_refine_bypass_commit). Never read blend_data from this
  // thread
  const dt_dev_refine_bypass_t *const bypass = &piece->refine_bypass;

  // a bypassed group contributes nothing, exactly as if it were not there
  const gboolean bypassed = (head->state & DT_MASKS_STATE_OP_BYPASS) != 0;
  // the group's operator (how members fold together, in list order): maximum
  // (default), screen (soft union), minimum, product (true per-pixel product),
  // sum (min(1, a + b)), difference (the first member less the others) or
  // exclusion
  const gboolean screen = (head->state & DT_MASKS_STATE_FLEXI_SCREEN) != 0;
  const gboolean flexi_minimum = (head->state & DT_MASKS_STATE_FLEXI_MINIMUM) != 0;
  const gboolean flexi_product = (head->state & DT_MASKS_STATE_FLEXI_PRODUCT) != 0;
  const gboolean flexi_sum = (head->state & DT_MASKS_STATE_FLEXI_SUM) != 0;
  const gboolean flexi_difference = (head->state & DT_MASKS_STATE_FLEXI_DIFFERENCE) != 0;
  const gboolean flexi_exclusion = (head->state & DT_MASKS_STATE_FLEXI_EXCLUSION) != 0;

  // minimum and product seed at 1.0 (everything, then min/multiply each
  // member in); maximum/screen/sum/exclusion seed at 0.0 (nothing, then
  // max/soft-union/add/exclusion in, each of which copies its first member onto
  // 0). Difference has no seed that copies, so its first member is copied
  // explicitly below
  if(flexi_minimum || flexi_product)
    for(size_t i = 0; i < npixels; i++) buffer[i] = 1.0f;
  else
    memset(buffer, 0, npixels * sizeof(float));

  int nb_members = 0; // members whose mask actually folded into `buffer`
  int nb_folded = 0;  // the same, counting no-op parametric channels too
  for(; fpts && !bypassed; fpts = g_list_next(fpts))
  {
    dt_masks_point_group_t *const m = fpts->data;
    if(m->state & (DT_MASKS_STATE_HIDDEN | DT_MASKS_STATE_DISABLE)) continue;
    dt_masks_form_t *const sel = dt_masks_get_from_id_ext(piece->pipe->forms, m->formid);
    if(!sel) continue;

    memset(bufs, 0, npixels * sizeof(float));
    if(!dt_masks_get_mask_roi(module, piece, sel, roi, bufs)) continue;

    // the member's own refinement, on its raw mask before inversion and
    // combining, where the classic fold below applies it too
    const gboolean elem_bypassed =
      dt_masks_refine_bypass_lookup(bypass, dt_masks_refine_key_element(m->formid));
    if(m->refinement.enabled == DT_MASKS_REFINE_ELEMENT && !elem_bypassed)
      dt_develop_blend_refine_form_mask((dt_iop_module_t *)module,
                                        (dt_dev_pixelpipe_iop_t *)piece, bufs, roi,
                                        &m->refinement);

    const float op = m->opacity;
    // a raster element that cannot obtain a mask renders 0. Inverted, it
    // would select the whole frame and apply the module everywhere, so it is
    // not inverted, as classic's raster branch fills 0 without inverting
    // (blend.c). The panel badges its row
    const int inverted = (m->state & DT_MASKS_STATE_INVERSE)
                         && !dt_masks_raster_is_unresolved(module, piece, sel);
    if(flexi_minimum)
      dt_masks_combine_minimum(buffer, bufs, npixels, op, inverted);
    else if(screen)
      dt_masks_combine_screen(buffer, bufs, npixels, op, inverted);
    else if(flexi_product)
      dt_masks_combine_product(buffer, bufs, npixels, op, inverted);
    else if(flexi_sum)
      dt_masks_combine_sum(buffer, bufs, npixels, op, inverted);
    else if(flexi_difference && nb_folded > 0)
      dt_masks_combine_difference(buffer, bufs, npixels, op, inverted);
    else if(flexi_exclusion)
      dt_masks_combine_exclusion(buffer, bufs, npixels, op, inverted);
    else
      // union, and the base of a difference: max onto the zero seed is a copy
      dt_masks_combine_maximum(buffer, bufs, npixels, op, inverted);
    nb_folded++;
    // a parametric channel at its full range renders all ones and restricts
    // nothing, so a group of nothing else takes the "no active mask element"
    // fallback below (opaque, no overlay) while a new channel is set up.
    // Decide it from the form's ranges, as the panel's badge does, never from
    // the pixels: a narrowed channel that happens to cover this image is a
    // real element, and skipping it would drop the group's invert and opacity
    if(!dt_masks_parametric_is_noop(sel, inverted)) nb_members++;
  }
  dt_free_align(bufs);

  if(bypassed || nb_members == 0)
  {
    // nothing contributed (bypassed, or every member hidden, disabled or
    // gone). As the mask, this renders as "no active mask element", which in
    // dt is a fully opaque mask (the module stays 100% active), matching the
    // `mode_drawn && !form` fallback in dt_develop_blend_process. As a nested group,
    // returning 0 makes its parent skip it
    for(size_t i = 0; i < npixels; i++) buffer[i] = 1.0f;
    return 0;
  }

  // per-group refinement, applied once to the folded mask (skipped while this
  // group is bypassed for preview). A group is keyed by its marker.
  const gboolean group_bypassed =
    dt_masks_refine_bypass_lookup(bypass, dt_masks_refine_key_group(head->formid));
  if(head->refinement.enabled == DT_MASKS_REFINE_GROUP && !group_bypassed)
    dt_develop_blend_refine_form_mask((dt_iop_module_t *)module,
                                      (dt_dev_pixelpipe_iop_t *)piece, buffer, roi,
                                      &head->refinement);

  // invert-output (true group invert, see DT_MASKS_STATE_OP_INVERT): applied
  // to the folded mask, after any group refinement
  if(head->state & DT_MASKS_STATE_OP_INVERT)
    for(size_t i = 0; i < npixels; i++) buffer[i] = 1.0f - buffer[i];

  // group-level opacity (see dt_masks_point_group_t.group_opacity): a
  // multiplicative gain on the group's finished mask, on top of each member's
  // own opacity. Applied after invert-output, for the same reason element
  // opacity multiplies a shape's already-inverted mask in dt_masks_combine_maximum
  // et al: it scales the group's actual contribution
  for(size_t i = 0; i < npixels; i++) buffer[i] *= head->group_opacity;

  return 1;
}

static int _group_get_mask_roi(const dt_iop_module_t *const restrict module,
                               const dt_dev_pixelpipe_iop_t *const restrict piece,
                               dt_masks_form_t *const form,
                               const dt_iop_roi_t *const roi,
                               float *const restrict buffer)
{
  // flexi masks use the group-composition fold; legacy masks fall through to
  // the classic sequential fold below, byte-identically.
  const dt_develop_blend_params_t *const bp =
    piece ? (const dt_develop_blend_params_t *)piece->blendop_data : NULL;
  if(bp && (bp->mask_mode & DEVELOP_MASK_FLEXI))
    return _group_get_mask_roi_flexi(module, piece, form, roi, buffer);

  double start = dt_get_debug_wtime();
  int nb_ok = 0;

  const int width = roi->width;
  const int height = roi->height;
  const size_t npixels = (size_t)width * height;

  // start from an empty result: an empty group renders an empty mask, and a
  // hidden/absent base form does not leave the first composited shape reading
  // uninitialized memory
  memset(buffer, 0, npixels * sizeof(float));
  if(!form->points) return 0;

  // we need to allocate a zeroed temporary buffer for intermediate
  // creation of individual shapes
  float *const restrict bufs = dt_alloc_align_float(npixels);
  if(bufs == NULL) return 0;

  // and we get all masks
  for(GList *fpts = form->points; fpts; fpts = g_list_next(fpts))
  {
    dt_masks_point_group_t *fpt = fpts->data;
    // a hidden or disabled shape contributes nothing
    if(fpt->state & (DT_MASKS_STATE_HIDDEN | DT_MASKS_STATE_DISABLE)) continue;
    dt_masks_form_t *sel = dt_masks_get_from_id_ext(piece->pipe->forms, fpt->formid);

    if(sel)
    {
      // ensure that we start with a zeroed buffer regardless of what
      // was previously written into 'bufs'
      memset(bufs, 0, npixels*sizeof(float));
      const int ok = dt_masks_get_mask_roi(module, piece, sel, roi, bufs);
      const float op = fpt->opacity;
      const int state = fpt->state;

      if(darktable.dump_pfm_module)
      {
        char *filename = g_strdup_printf("mask-%d", fpt->formid);
        dt_dump_pfm(filename,
                    bufs,
                    width,
                    height,
                    sizeof(float),
                    module->op);
        g_free(filename);
      }

      if(ok)
      {
        // the shape's own refinement, on its raw mask before inversion and
        // combining
        if(fpt->refinement.enabled)
          dt_develop_blend_refine_form_mask((dt_iop_module_t *)module,
                                            (dt_dev_pixelpipe_iop_t *)piece, bufs, roi,
                                            &fpt->refinement);

        // first see if we need to invert this shape, unless it is a raster
        // element that renders 0 for want of a mask (see
        // _group_get_mask_roi_flexi above)
        const int inverted = (state & DT_MASKS_STATE_INVERSE)
                             && !dt_masks_raster_is_unresolved(module, piece, sel);

        // every shape applies its own operator, the bottom one included: onto
        // the empty accumulator, intersection and difference leave nothing.
        // Migration turns such shapes into zero-opacity unions
        // (_zero_empty_base_members in migrate_legacy.c)
        if(state & DT_MASKS_STATE_UNION)
        {
          dt_masks_combine_maximum(buffer, bufs, npixels, op, inverted);
        }
        else if(state & DT_MASKS_STATE_INTERSECTION)
        {
          dt_masks_combine_minimum(buffer, bufs, npixels, op, inverted);
        }
        else if(state & DT_MASKS_STATE_DIFFERENCE)
        {
          dt_masks_combine_difference(buffer, bufs, npixels, op, inverted);
        }
        else if(state & DT_MASKS_STATE_SUM)
        {
          dt_masks_combine_sum(buffer, bufs, npixels, op, inverted);
        }
        else if(state & DT_MASKS_STATE_EXCLUSION)
        {
          dt_masks_combine_exclusion(buffer, bufs, npixels, op, inverted);
        }
        else // if we are here, this mean that we just have to copy
             // the shape and null other parts
        {
          DT_OMP_FOR_SIMD(aligned(buffer, bufs : 64))
          for(size_t index = 0; index < npixels; index++)
          {
            buffer[index] = op * (inverted ? (1.0f - bufs[index]) : bufs[index]);
          }
        }

        dt_print(DT_DEBUG_MASKS | DT_DEBUG_PERF,
                 "[masks %d] combine took %0.04f sec",
                 nb_ok, dt_get_lap_time(&start));

        nb_ok++;
      }
    }

    if(darktable.dump_pfm_module)
    {
      char *filename = g_strdup_printf("mask-combined-%d", fpt->formid);
      dt_dump_pfm(filename,
                  buffer,
                  width,
                  height,
                  sizeof(float),
                  module->op);
      g_free(filename);
    }
  }
  // and we free the intermediate buffer
  dt_free_align(bufs);

  return nb_ok != 0;
}

int dt_masks_group_render_roi(dt_iop_module_t *module,
                              dt_dev_pixelpipe_iop_t *piece,
                              dt_masks_form_t *form,
                              const dt_iop_roi_t *roi,
                              float *buffer)
{
  if(!form) return 0;

  double start = dt_get_debug_wtime();
  const int ok = dt_masks_get_mask_roi(module, piece, form, roi, buffer);

  dt_print(DT_DEBUG_MASKS | DT_DEBUG_PERF,
           "[masks] render all masks took %0.04f sec",
           dt_get_lap_time(&start));
  return ok;
}

static GSList *_group_setup_mouse_actions(const dt_masks_form_t *const form)
{
  GSList *lm = NULL;
  // initialize the mask of seen shapes to the set of flags which
  // aren't actually shapes
  dt_masks_type_t seen_types = (DT_MASKS_GROUP | DT_MASKS_CLONE | DT_MASKS_NON_CLONE);
  // iterate over the shapes in the group, adding the mouse_action for
  // each distinct type of shape

  for(GList *fpts = form->points; fpts; fpts = g_list_next(fpts))
  {
    dt_masks_point_group_t *fpt = fpts->data;
    dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
    if(!sel || (sel->type & ~seen_types) == 0)
      continue;
    if(sel->functions && sel->functions->setup_mouse_actions)
    {
      GSList *new_actions = sel->functions->setup_mouse_actions(sel);
      lm = g_slist_concat(lm, new_actions);
      seen_types |= sel->type;
    }
  }
  return lm;
}

void dt_masks_group_duplicate_points(dt_develop_t *const dev,
                                     dt_masks_form_t *const base,
                                     dt_masks_form_t *const dest)
{
  for(GList *pts = base->points; pts; pts = g_list_next(pts))
  {
    dt_masks_point_group_t *pt = pts->data;
    if(dt_masks_point_is_marker(pt))
    {
      dt_masks_group_copy_marker(dev->forms, dest, pt);
      continue;
    }
    dt_masks_point_group_t *npt = calloc(1, sizeof(dt_masks_point_group_t));

    npt->formid = dt_masks_form_duplicate(dev, pt->formid);
    npt->parentid = dest->formid;
    npt->state = pt->state;
    npt->opacity = pt->opacity;
    npt->refinement = pt->refinement;
    npt->group_opacity = pt->group_opacity;
    dest->points = g_list_append(dest->points, npt);
  }
}

// a group renders a member group by recursing through the functions table,
// which has no room for a depth, so a cyclic tree would recurse until the
// stack is gone. Pipes render on threads of their own, hence the per-thread
// count
static __thread int _render_depth = 0;

int dt_masks_group_get_mask(const dt_iop_module_t *const module,
                            const dt_dev_pixelpipe_iop_t *const piece,
                            dt_masks_form_t *const form,
                            float **buffer,
                            int *width,
                            int *height,
                            int *posx,
                            int *posy)
{
  if(_render_depth > DT_MASKS_NESTING_MAX) return 0;
  _render_depth++;
  const int ok = _group_get_mask(module, piece, form, buffer, width, height, posx, posy);
  _render_depth--;
  return ok;
}

int dt_masks_group_get_mask_roi(const dt_iop_module_t *const restrict module,
                                const dt_dev_pixelpipe_iop_t *const restrict piece,
                                dt_masks_form_t *const form,
                                const dt_iop_roi_t *const roi,
                                float *const restrict buffer)
{
  if(_render_depth > DT_MASKS_NESTING_MAX) return 0;
  _render_depth++;
  const int ok = _group_get_mask_roi(module, piece, form, roi, buffer);
  _render_depth--;
  return ok;
}

// The function table for groups.  This must be public, i.e. no "static" keyword.
const dt_masks_functions_t dt_masks_functions_group = {
  .point_struct_size = sizeof(struct dt_masks_point_group_t),
  .sanitize_config = NULL,
  .setup_mouse_actions = _group_setup_mouse_actions,
  .set_form_name = NULL,
  .set_hint_message = NULL,
  .duplicate_points = dt_masks_group_duplicate_points,
  .initial_source_pos = NULL,
  .get_distance = NULL,
  .get_points = NULL,
  .get_points_border = NULL,
  .get_mask = dt_masks_group_get_mask,
  .get_mask_roi = dt_masks_group_get_mask_roi,
  .get_area = NULL,
  .get_source_area = NULL,
  .mouse_moved = _group_events_mouse_moved,
  .mouse_scrolled = _group_events_mouse_scrolled,
  .button_pressed = _group_events_button_pressed,
  .button_released = _group_events_button_released,
//TODO:  .post_expose = _group_events_post_expose
};


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
