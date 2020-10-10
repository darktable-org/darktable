/*
    This file is part of darktable,
    Copyright (C) 2018-2020 darktable developers.

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
#include "gui/gtk.h"
#include "develop/blend.h"

typedef struct dt_iop_color_picker_t
{
  dt_iop_module_t *module;
  dt_iop_color_picker_kind_t kind;
  /** requested colorspace for the color picker, valid options are:
   * iop_cs_NONE: module colorspace
   * iop_cs_LCh: for Lab modules
   * iop_cs_HSL: for RGB modules
   */
  dt_iop_colorspace_type_t picker_cst;
  /** used to avoid recursion when a parameter is modified in the apply() */
  GtkWidget *colorpick;
  float pick_pos[2]; // last picker positions (max 9 picker per module)
  float pick_box[4]; // last picker areas (max 9 picker per module)
} dt_iop_color_picker_t;

static gboolean _iop_record_point_area(dt_iop_color_picker_t *self)
{
  gboolean selection_changed = FALSE;

  if(self && self->module)
  {
    for(int k = 0; k < 2; k++)
    {
      if(self->pick_pos[k] != self->module->color_picker_point[k])
      {

        self->pick_pos[k] = self->module->color_picker_point[k];
        selection_changed = TRUE;
      }

    }
    for(int k = 0; k < 4; k++)
    {
      if (self->pick_box[k] != self->module->color_picker_box[k])
      {
        self->pick_box[k] = self->module->color_picker_box[k];
        selection_changed = TRUE;
      }
    }
  }

  return selection_changed;
}

static void _iop_get_point(dt_iop_color_picker_t *self, float *pos)
{
  pos[0] = pos[1] = 0.5f;

  if(!isnan(self->pick_pos[0]) && !isnan(self->pick_pos[1]))
  {
    pos[0] = self->pick_pos[0];
    pos[1] = self->pick_pos[1];
  }
}

static void _iop_get_area(dt_iop_color_picker_t *self, float *box)
{
  if(!isnan(self->pick_box[0]) && !isnan(self->pick_box[1]))
  {
    for(int k = 0; k < 4; k++) box[k] = self->pick_box[k];
  }
  else
  {
    const float size = 0.99f;

    box[0] = box[1] = 1.0f - size;
    box[2] = box[3] = size;
  }
}

static void _iop_color_picker_apply(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece)
{
  if(_iop_record_point_area(module->picker))
  {
    if(!module->blend_data || !blend_color_picker_apply(module, module->picker->colorpick, piece))
    {
      if(module->color_picker_apply)
        module->color_picker_apply(module, module->picker->colorpick, piece);
    }
  }
}

static void _iop_color_picker_reset(dt_iop_color_picker_t *picker)
{
  if(picker)
  {
    ++darktable.gui->reset;

    if(DTGTK_IS_TOGGLEBUTTON(picker->colorpick))
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(picker->colorpick), FALSE);
    else
      dt_bauhaus_widget_set_quad_active(picker->colorpick, FALSE);

    --darktable.gui->reset;
  }
}

void dt_iop_color_picker_reset(dt_iop_module_t *module, gboolean keep)
{
  if(module && module->picker)
  {
    if(!keep || (strcmp(gtk_widget_get_name(module->picker->colorpick), "keep-active") != 0))
    {
      _iop_color_picker_reset(module->picker);
      module->picker = NULL;
      module->request_color_pick = DT_REQUEST_COLORPICK_OFF;
    }
  }
}

static void _iop_init_picker(dt_iop_color_picker_t *picker, dt_iop_module_t *module,
                             dt_iop_color_picker_kind_t kind, GtkWidget *button)
{
  picker->module     = module;
  picker->kind       = kind;
  picker->picker_cst = iop_cs_NONE;
  picker->colorpick  = button;

  for(int j = 0; j<2; j++) picker->pick_pos[j] = NAN;
  for(int j = 0; j < 4; j++) picker->pick_box[j] = NAN;

  _iop_color_picker_reset(picker);
}

static gboolean _iop_color_picker_callback_button_press(GtkWidget *button, GdkEventButton *e, dt_iop_color_picker_t *self)
{
  dt_iop_module_t *module = self->module ? self->module : dt_iop_get_colorout_module();

  if(!module || darktable.gui->reset) return FALSE;

  // set module active if not yet the case
  if(module->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), TRUE);

  const GdkModifierType state
      = e != NULL ? e->state & gtk_accelerator_get_default_mod_mask() : dt_key_modifier_state();
  const gboolean ctrl_key_pressed = (state == GDK_CONTROL_MASK);
  dt_iop_color_picker_kind_t kind = self->kind;

  _iop_color_picker_reset(module->picker);

  if (module->picker != self || (ctrl_key_pressed && kind == DT_COLOR_PICKER_POINT_AREA))
  {
    module->picker = self;

    ++darktable.gui->reset;

    if(DTGTK_IS_TOGGLEBUTTON(self->colorpick))
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->colorpick), TRUE);
    else
      dt_bauhaus_widget_set_quad_active(self->colorpick, TRUE);

    --darktable.gui->reset;

    module->request_color_pick = DT_REQUEST_COLORPICK_MODULE;

    if(kind == DT_COLOR_PICKER_POINT_AREA)
    {
      kind = ctrl_key_pressed ? DT_COLOR_PICKER_AREA : DT_COLOR_PICKER_POINT;
    }
    if(kind == DT_COLOR_PICKER_AREA)
    {
      float box[4];
      _iop_get_area(self, box);
      dt_lib_colorpicker_set_box_area(darktable.lib, box);
      self->pick_pos[0] = NAN; // trigger difference on first apply
    }
    else
    {
      float pos[2];
      _iop_get_point(self, pos);
      dt_lib_colorpicker_set_point(darktable.lib, pos[0], pos[1]);
      self->pick_box[0] = NAN; // trigger difference on first apply
    }

    module->dev->preview_status = DT_DEV_PIXELPIPE_DIRTY;
    dt_iop_request_focus(module);
  }
  else
  {
    module->picker = NULL;
    module->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  }

  dt_control_queue_redraw();

  return TRUE;
}

static void _iop_color_picker_callback(GtkWidget *button, dt_iop_color_picker_t *self)
{
  _iop_color_picker_callback_button_press(button, NULL, self);
}

void dt_iop_color_picker_set_cst(dt_iop_module_t *module, const dt_iop_colorspace_type_t picker_cst)
{
  if(module->picker)
    module->picker->picker_cst = picker_cst;
}

dt_iop_colorspace_type_t dt_iop_color_picker_get_active_cst(dt_iop_module_t *module)
{
  if(module->picker)
    return module->picker->picker_cst;
  else
    return iop_cs_NONE;
}

static void _iop_color_picker_signal_callback(gpointer instance, dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                              gpointer user_data)
{
  _iop_color_picker_apply(module, piece);
}

void dt_iop_color_picker_init(void)
{
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_CONTROL_PICKERDATA_READY,
                            G_CALLBACK(_iop_color_picker_signal_callback), NULL);
}

void dt_iop_color_picker_cleanup(void)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_iop_color_picker_signal_callback), NULL);
}

GtkWidget *dt_color_picker_new(dt_iop_module_t *module, dt_iop_color_picker_kind_t kind, GtkWidget *w)
{
  dt_iop_color_picker_t *color_picker = (dt_iop_color_picker_t *)g_malloc(sizeof(dt_iop_color_picker_t));

  if(w == NULL || GTK_IS_BOX(w))
  {
    GtkWidget *button = dtgtk_togglebutton_new(dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_BG_TRANSPARENT, NULL);
    _iop_init_picker(color_picker, module, kind, button);
    g_signal_connect_data(G_OBJECT(button), "button-press-event",
                          G_CALLBACK(_iop_color_picker_callback_button_press), color_picker, (GClosureNotify)g_free, 0);
    if (w) gtk_box_pack_start(GTK_BOX(w), button, FALSE, FALSE, 0);

    return button;
  }
  else
  {
    dt_bauhaus_widget_set_quad_paint(w, dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT, NULL);
    dt_bauhaus_widget_set_quad_toggle(w, TRUE);
    _iop_init_picker(color_picker, module, kind, w);
    g_signal_connect_data(G_OBJECT(w), "quad-pressed",
                          G_CALLBACK(_iop_color_picker_callback), color_picker, (GClosureNotify)g_free, 0);

    return w;
  }
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
