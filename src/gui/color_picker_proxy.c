/*
    This file is part of darktable,
    copyright (c) 2018 Pascal Obry.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "gui/color_picker_proxy.h"
#include "bauhaus/bauhaus.h"
#include "libs/lib.h"
#include "control/control.h"

typedef enum _internal__status
{
  PICKER_STATUS_NONE = 0,
  PICKER_STATUS_SELECTED
} dt_internal_status_t;

typedef enum dt_iop_color_picker_requested_by_t
{
  DT_COLOR_PICKER_REQ_MODULE = 0,
  DT_COLOR_PICKER_REQ_BLEND
} dt_iop_color_picker_requested_by_t;

static void _iop_record_point(dt_iop_color_picker_t *self)
{
  const int pick_index = CLAMP(self->current_picker, 1, 9) - 1;
  self->pick_pos[pick_index][0] = self->module->color_picker_point[0];
  self->pick_pos[pick_index][1] = self->module->color_picker_point[1];
  for(int k = 0; k < 4; k++) self->pick_box[pick_index][k] = self->module->color_picker_box[k];
}

static void _iop_get_point(dt_iop_color_picker_t *self, float *pos)
{
  const int pick_index = CLAMP(self->current_picker, 1, 9) - 1;

  pos[0] = pos[1] = 0.5f;

  if(!isnan(self->pick_pos[pick_index][0]) && !isnan(self->pick_pos[pick_index][1]))
  {
    pos[0] = self->pick_pos[pick_index][0];
    pos[1] = self->pick_pos[pick_index][1];
  }
}

static void _iop_get_area(dt_iop_color_picker_t *self, float *box)
{
  const int pick_index = CLAMP(self->current_picker, 1, 9) - 1;

  if(!isnan(self->pick_box[pick_index][0]) && !isnan(self->pick_box[pick_index][1]))
  {
    for(int k = 0; k < 4; k++) box[k] = self->pick_box[pick_index][k];
  }
  else
  {
    const float size = 0.99f;

    box[0] = box[1] = 1.0f - size;
    box[2] = box[3] = size;
  }
}

static int _internal_iop_color_picker_get_set(dt_iop_color_picker_t *picker, GtkWidget *button)
{
  const int current_picker = picker->current_picker;

  picker->current_picker = PICKER_STATUS_SELECTED;

  if(current_picker == picker->current_picker)
    return DT_COLOR_PICKER_ALREADY_SELECTED;
  else
    return picker->current_picker;
}

static void _internal_iop_color_picker_update(dt_iop_color_picker_t *picker)
{
  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;

  if(DTGTK_IS_TOGGLEBUTTON(picker->colorpick))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(picker->colorpick), picker->current_picker == PICKER_STATUS_SELECTED);
  else
    dt_bauhaus_widget_set_quad_active(picker->colorpick, picker->current_picker == PICKER_STATUS_SELECTED);

  darktable.gui->reset = reset;
}

void dt_iop_color_picker_apply_module(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece)
{
  if(module->request_color_pick == DT_REQUEST_COLORPICK_MODULE && module->picker && module->picker->apply)
  {
    if(module->picker->skip_apply)
      module->picker->skip_apply = FALSE;
    else
      module->picker->apply(module, piece);
    _iop_record_point(module->picker);
  }
  else if(module->request_color_pick == DT_REQUEST_COLORPICK_BLEND && module->blend_picker && module->blend_picker->apply)
  {
    if(module->blend_picker->skip_apply)
      module->blend_picker->skip_apply = FALSE;
    else
      module->blend_picker->apply(module, piece);
    _iop_record_point(module->blend_picker);
  }
}

static void _iop_color_picker_reset(dt_iop_color_picker_t *picker, gboolean update)
{
  if((picker->requested_by == DT_COLOR_PICKER_REQ_MODULE && picker->module->request_color_pick == DT_REQUEST_COLORPICK_MODULE) ||
      (picker->requested_by == DT_COLOR_PICKER_REQ_BLEND && picker->module->request_color_pick == DT_REQUEST_COLORPICK_BLEND))
    picker->module->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  picker->current_picker = PICKER_STATUS_NONE;
  if(update) dt_iop_color_picker_update(picker);
}

void dt_iop_color_picker_reset(dt_iop_module_t *module, gboolean update)
{
  if(module->picker) _iop_color_picker_reset(module->picker, update);
  if(module->blend_picker) _iop_color_picker_reset(module->blend_picker, update);
}

int dt_iop_color_picker_get_set(dt_iop_color_picker_t *picker, GtkWidget *button)
{
  if(picker->get_set)
    return picker->get_set(picker->module, button);
  else
    return _internal_iop_color_picker_get_set(picker, button);
}

void dt_iop_color_picker_apply(dt_iop_color_picker_t *picker, dt_dev_pixelpipe_iop_t *piece)
{
  if(picker->skip_apply)
    picker->skip_apply = FALSE;
  else
    picker->apply(picker->module, piece);
}

void dt_iop_color_picker_update(dt_iop_color_picker_t *picker)
{
  if(picker->update)
    picker->update(picker->module);
  else
    _internal_iop_color_picker_update(picker);
}

static void _iop_init_picker(dt_iop_color_picker_t *picker, dt_iop_module_t *module,
                             dt_iop_color_picker_kind_t kind, const int requested_by,
                             int (*get_set)(dt_iop_module_t *self, GtkWidget *button),
                             void (*apply)(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece),
                             void (*update)(dt_iop_module_t *self))
{
  picker->module  = module;
  picker->get_set = get_set;
  picker->apply   = apply;
  picker->update  = update;
  picker->kind    = kind;
  picker->requested_by = requested_by;
  picker->picker_cst = iop_cs_NONE;
  picker->skip_apply = FALSE;
  if(picker->requested_by == DT_COLOR_PICKER_REQ_MODULE)
    module->picker  = picker;
  else
    module->blend_picker  = picker;

  for(int i = 0; i<9; i++)
  {
    for(int j = 0; j<2; j++)
      picker->pick_pos[i][j] = NAN;
    for(int j = 0; j < 4; j++) picker->pick_box[i][j] = NAN;
  }

  _iop_color_picker_reset(picker, TRUE);
}

void dt_iop_init_single_picker(dt_iop_color_picker_t *picker, dt_iop_module_t *module, GtkWidget *colorpick,
                               dt_iop_color_picker_kind_t kind,
                               void (*apply)(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece))
{
  picker->colorpick = colorpick;
  dt_iop_init_picker(picker, module, kind, NULL, apply, NULL);
}

void dt_iop_init_picker(dt_iop_color_picker_t *picker, dt_iop_module_t *module, dt_iop_color_picker_kind_t kind,
                        int (*get_set)(dt_iop_module_t *self, GtkWidget *button),
                        void (*apply)(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece),
                        void (*update)(dt_iop_module_t *self))
{
  _iop_init_picker(picker,
                    module,
                    kind,
                    DT_COLOR_PICKER_REQ_MODULE,
                    get_set,
                    apply,
                    update);
}

void dt_iop_init_blend_picker(dt_iop_color_picker_t *picker, dt_iop_module_t *module,
                              dt_iop_color_picker_kind_t kind,
                              int (*get_set)(dt_iop_module_t *self, GtkWidget *button),
                              void (*apply)(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece),
                              void (*update)(dt_iop_module_t *self))
{
  _iop_init_picker(picker,
                    module,
                    kind,
                    DT_COLOR_PICKER_REQ_BLEND,
                    get_set,
                    apply,
                    update);
}

static gboolean _iop_color_picker_callback(GtkWidget *button, GdkEventButton *e, dt_iop_color_picker_t *self)
{
  if(self->module->dt->gui->reset) return FALSE;

  // set module active if not yet the case
  if(self->module->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->module->off), TRUE);

  // get the current color picker (a module can have multiple one)
  // should returns -1 if the same picker was already selected.
  const int clicked_colorpick = dt_iop_color_picker_get_set(self, button);

  if(self->module->request_color_pick == DT_REQUEST_COLORPICK_OFF || clicked_colorpick != DT_COLOR_PICKER_ALREADY_SELECTED)
  {
    if(self->requested_by == DT_COLOR_PICKER_REQ_MODULE)
    {
      if(self->module->request_color_pick == DT_REQUEST_COLORPICK_BLEND && self->module->blend_picker)
        _iop_color_picker_reset(self->module->blend_picker, TRUE);

      self->module->request_color_pick = DT_REQUEST_COLORPICK_MODULE;
    }
    else
    {
      if(self->module->request_color_pick == DT_REQUEST_COLORPICK_MODULE && self->module->picker)
        _iop_color_picker_reset(self->module->picker, TRUE);

      self->module->request_color_pick = DT_REQUEST_COLORPICK_BLEND;
    }

    if(clicked_colorpick != DT_COLOR_PICKER_ALREADY_SELECTED)
      self->current_picker = clicked_colorpick;

    dt_iop_color_picker_kind_t kind = self->kind;
    if(kind == DT_COLOR_PICKER_POINT_AREA)
    {
      const uint32_t state = (e != NULL) ? e->state: 0;
      GdkModifierType modifiers = gtk_accelerator_get_default_mod_mask();
      if((state & modifiers) == GDK_CONTROL_MASK)
        kind = DT_COLOR_PICKER_AREA;
      else
        kind = DT_COLOR_PICKER_POINT;
    }
    if(kind == DT_COLOR_PICKER_AREA)
    {
      float box[4];
      _iop_get_area(self, box);
      dt_lib_colorpicker_set_box_area(darktable.lib, box);
    }
    else
    {
      float pos[2];
      _iop_get_point(self, pos);
      dt_lib_colorpicker_set_point(darktable.lib, pos[0], pos[1]);
    }

    self->module->dev->preview_status = DT_DEV_PIXELPIPE_DIRTY;
  }
  else
  {
    /* focus on the center area, to force a redraw when focus on module is called below */
    _iop_color_picker_reset(self, FALSE);
  }
  dt_iop_color_picker_update(self);
  dt_control_queue_redraw();
  dt_iop_request_focus(self->module);

  return TRUE;
}

void dt_iop_color_picker_callback(GtkWidget *button, dt_iop_color_picker_t *self)
{
  _iop_color_picker_callback(button, NULL, self);
}

gboolean dt_iop_color_picker_callback_button_press(GtkWidget *button, GdkEventButton *e, dt_iop_color_picker_t *self)
{
  return _iop_color_picker_callback(button, e, self);
}

void dt_iop_color_picker_set_cst(dt_iop_color_picker_t *picker, const dt_iop_colorspace_type_t picker_cst)
{
  picker->picker_cst = picker_cst;
}

dt_iop_colorspace_type_t dt_iop_color_picker_get_active_cst(dt_iop_module_t *module)
{
  dt_iop_colorspace_type_t picker_cst = iop_cs_NONE;

  if(module->request_color_pick == DT_REQUEST_COLORPICK_BLEND && module->blend_picker)
    picker_cst = module->blend_picker->picker_cst;
  else if(module->request_color_pick == DT_REQUEST_COLORPICK_MODULE && module->picker)
    picker_cst = module->picker->picker_cst;

  return picker_cst;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
