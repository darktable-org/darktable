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

/* a callback for the "draw" signal on the main IOP widget (self->widget) passed to gui_init. this is
   to used to apply the picked value (requested in dt_iop_color_picker_callback) when available. */
static gboolean _iop_color_picker_draw(GtkWidget *widget, cairo_t *cr, dt_iop_color_picker_t *self);

static void _iop_record_point(dt_iop_color_picker_t *self)
{
  const int pick_index = CLAMP(self->current_picker, 1, 9) - 1;
  self->pick_pos[pick_index][0] = self->module->color_picker_point[0];
  self->pick_pos[pick_index][1] = self->module->color_picker_point[1];
}

static void _iop_get_point(dt_iop_color_picker_t *self, float *pos)
{
  const int pick_index = CLAMP(self->current_picker, 1, 9) - 1;

  pos[0] = pos[1] = 0.5f;

  if(self->pick_pos[pick_index][0] != NAN && self->pick_pos[pick_index][1] != NAN)
  {
    pos[0] = self->pick_pos[pick_index][0];
    pos[1] = self->pick_pos[pick_index][1];
  }
}

static int _internal_iop_color_picker_get_set(dt_iop_color_picker_t *picker, GtkWidget *button)
{
  const int current_picker = picker->current_picker;

  picker->current_picker = PICKER_STATUS_SELECTED;

  if (current_picker == picker->current_picker)
    return ALREADY_SELECTED;
  else
    return picker->current_picker;
}

static void _internal_iop_color_picker_update(dt_iop_color_picker_t *picker)
{
  const int old_reset = darktable.gui->reset;
  darktable.gui->reset = 1;

  if(DTGTK_IS_TOGGLEBUTTON(picker->colorpick))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(picker->colorpick), picker->current_picker == PICKER_STATUS_SELECTED);
  else
    dt_bauhaus_widget_set_quad_active(picker->colorpick, picker->current_picker == PICKER_STATUS_SELECTED);

  darktable.gui->reset = old_reset;
}

void dt_iop_color_picker_reset(dt_iop_color_picker_t *picker, gboolean update)
{
  picker->module->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  picker->current_picker = PICKER_STATUS_NONE;
  if (update) dt_iop_color_picker_update(picker);
}

int dt_iop_color_picker_get_set(dt_iop_color_picker_t *picker, GtkWidget *button)
{
  if(picker->get_set)
    return picker->get_set(picker->module, button);
  else
    return _internal_iop_color_picker_get_set(picker, button);
}

void dt_iop_color_picker_apply(dt_iop_color_picker_t *picker)
{
  picker->apply(picker->module);
}

void dt_iop_color_picker_update(dt_iop_color_picker_t *picker)
{
  if(picker->update)
    picker->update(picker->module);
  else
    _internal_iop_color_picker_update(picker);
}

void init_single_picker (dt_iop_color_picker_t *picker,
                         dt_iop_module_t *module,
                         GtkWidget *colorpick,
                         dt_iop_color_picker_kind_t kind,
                         void (*apply)(dt_iop_module_t *self))
{
  picker->colorpick = colorpick;
  init_picker(picker, module, kind, NULL, apply, NULL);
}

void init_picker (dt_iop_color_picker_t *picker,
                  dt_iop_module_t *module,
                  dt_iop_color_picker_kind_t kind,
                  int (*get_set)(dt_iop_module_t *self, GtkWidget *button),
                  void (*apply)(dt_iop_module_t *self),
                  void (*update)(dt_iop_module_t *self))
{
  picker->module  = module;
  picker->get_set = get_set;
  picker->apply   = apply;
  picker->update  = update;
  picker->kind    = kind;

  for(int i = 0; i<9; i++)
    for(int j = 0; j<2; j++)
      picker->pick_pos[i][j] = NAN;

  dt_iop_color_picker_reset(picker, TRUE);

  /* The widget receives a draw signal right before being applied in
     the pipeline. This is when we want to take into account the  picked color. */
  g_signal_connect(G_OBJECT(picker->module->widget), "draw", G_CALLBACK(_iop_color_picker_draw), picker);
}

void dt_iop_color_picker_callback(GtkWidget *button, dt_iop_color_picker_t *self)
{
  if(self->module->dt->gui->reset) return;

  // set module active if not yet the case
  if(self->module->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->module->off), 1);

  // get the current color picker (a module can have multiple one)
  // should returns -1 if the same picker was already selected.
  const int clicked_colorpick = dt_iop_color_picker_get_set(self, button);

  if(self->module->request_color_pick == DT_REQUEST_COLORPICK_OFF || clicked_colorpick != ALREADY_SELECTED)
  {
    self->module->request_color_pick = DT_REQUEST_COLORPICK_MODULE;

    if(clicked_colorpick != ALREADY_SELECTED)
      self->current_picker = clicked_colorpick;

    if(self->kind == DT_COLOR_PICKER_AREA)
      dt_lib_colorpicker_set_area(darktable.lib, 0.99);
    else
    {
      float pos[2];
      _iop_get_point(self, pos);
      dt_lib_colorpicker_set_point(darktable.lib, pos[0], pos[1]);
    }

    dt_dev_reprocess_all(self->module->dev);
  }
  else
  {
    /* focus on the center area, to force a redraw when focus on module is called below */
    dt_iop_color_picker_reset(self, FALSE);
  }
  dt_iop_color_picker_update(self);
  dt_control_queue_redraw();
  dt_iop_request_focus(self->module);
}

static gboolean _iop_color_picker_draw(GtkWidget *widget, cairo_t *cr, dt_iop_color_picker_t *self)
{
  if(darktable.gui->reset) return FALSE;

  /* The color picker is off */
  if(self->module->request_color_pick == DT_REQUEST_COLORPICK_OFF) return FALSE;

  /* No color picked, or picked color already applied */
  if(self->module->picked_color_max[0] < 0.0f) return FALSE;

  self->apply(self->module);

  _iop_record_point(self);

  /* Make sure next call won't re-apply in loop: draw -> picker -> set_sliders -> draw. */
  self->module->picked_color_max[0] = -INFINITY;

  return FALSE;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
