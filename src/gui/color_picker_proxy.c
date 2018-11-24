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
#include "libs/lib.h"
#include "control/control.h"

/* a callback for the "draw" signal on the main IOP widget (self->widget) passed to gui_init. this is
   to used to apply the picked value (requested in dt_iop_color_picker_callback) when available. */
static gboolean _iop_color_picker_draw(GtkWidget *widget, cairo_t *cr, dt_iop_color_picker_t *self);

void dt_iop_color_picker_reset(dt_iop_color_picker_t *picker, gboolean update)
{
  picker->module->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  picker->reset(picker->module);
  if (update) picker->update(picker->module);
}

int dt_iop_color_picker_get_set(dt_iop_color_picker_t *picker, GtkWidget *button)
{
  return picker->get_set(picker->module, button);
}

void dt_iop_color_picker_apply(dt_iop_color_picker_t *picker)
{
  picker->apply(picker->module);
}

void dt_iop_color_picker_update(dt_iop_color_picker_t *picker)
{
  picker->update(picker->module);
}

void init_picker (dt_iop_color_picker_t *picker,
                  dt_iop_module_t *module,
                  int (*get_set)(dt_iop_module_t *self, GtkWidget *button),
                  void (*apply)(dt_iop_module_t *self),
                  void (*reset)(dt_iop_module_t *self),
                  void (*update)(dt_iop_module_t *self))
{
  picker->module  = module;
  picker->get_set = get_set;
  picker->apply   = apply;
  picker->reset   = reset;
  picker->update  = update;

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
  const int clicked_colorpick = self->get_set(self->module, button);

  if(self->module->request_color_pick == DT_REQUEST_COLORPICK_OFF || clicked_colorpick != ALREADY_SELECTED)
  {
    self->module->request_color_pick = DT_REQUEST_COLORPICK_MODULE;
    dt_lib_colorpicker_set_area(darktable.lib, 0.99);
    dt_dev_reprocess_all(self->module->dev);
  }
  else
  {
    /* focus on the center area, to force a redraw when focus on module is called below */
    dt_iop_color_picker_reset(self, FALSE);
  }
  self->update(self->module);
  dt_control_queue_redraw();
  dt_iop_request_focus(self->module);
}

static gboolean _iop_color_picker_draw(GtkWidget *widget, cairo_t *cr, dt_iop_color_picker_t *self)
{
  if(darktable.gui->reset) return FALSE;

  /* No color picked, or picked color already applied */
  if(self->module->picked_color_max[0] < 0.0f) return FALSE;

  self->apply(self->module);
  /* Make sure next call won't re-apply in loop: draw -> picker -> set_sliders -> draw. */
  self->module->picked_color_max[0] = -INFINITY;

  return FALSE;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
