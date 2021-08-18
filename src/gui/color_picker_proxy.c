/*
    This file is part of darktable,
    Copyright (C) 2018-2021 darktable developers.

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
#include "libs/colorpicker.h"
#include "bauhaus/bauhaus.h"
#include "libs/lib.h"
#include "control/control.h"
#include "gui/gtk.h"
#include "develop/blend.h"

/*
  The color_picker_proxy code links the UI colorpicker buttons in
  iops (and the colorpicker lib) with the rest of the implementation
  (selecting/drawing colorpicker area in center view, reading color
  value from preview pipe, and displaying results in the colorpicker
  lib).

  From the iop (or lib) POV, all that is necessary is to instantiate
  color picker(s) via dt_color_picker_new() or
  dt_color_picker_new_with_cst() then receive their results via the
  color_picker_apply() callback.

  This code will initialize new pickers with a default area, then
  remember the last area of the picker and use that when the picker is
  reactivated.

  The actual work of "picking" happens in pixelpipe_hb.c. The drawing
  & mouse-sensitivity of the picker overlay in the center view happens
  in darkroom.c. The display of current sample values occurs via
  libs/colorpicker.c, which uses this code to activate its own picker.

  The sample position is potentially stored in two places:

  1. For each sampler widget, in dt_iop_color_picker_t.
  2. For the active iop, the primary, and the live samples in
     dt_colorpicker_sample_t.
*/

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
  // positions are associated with the current picker widget: will set
  // the picker request for the primary picker when this picker is
  // activated, and will remember the most recent picker position
  float pick_pos[2];
  dt_boundingbox_t pick_box;
} dt_iop_color_picker_t;

static gboolean _iop_record_point_area(dt_iop_color_picker_t *self)
{
  gboolean selection_changed = FALSE;

  const dt_colorpicker_sample_t *const sample = darktable.lib->proxy.colorpicker.primary_sample;
  if(self && self->module && sample)
  {
    if(sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
      for(int k = 0; k < 2; k++)
      {
        if(self->pick_pos[k] != sample->point[k])
        {
          self->pick_pos[k] = sample->point[k];
          selection_changed = TRUE;
        }
      }
    else if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
      for(int k = 0; k < 4; k++)
      {
        if (self->pick_box[k] != sample->box[k])
        {
          self->pick_box[k] = sample->box[k];
          selection_changed = TRUE;
        }
      }
  }

  return selection_changed;
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
  picker->picker_cst = module ? module->default_colorspace(module, NULL, NULL) : iop_cs_NONE;
  picker->colorpick  = button;

  // default values
  const float middle = 0.5f;
  const float area = 0.99f;
  picker->pick_pos[0] = picker->pick_pos[1] = middle;
  picker->pick_box[0] = picker->pick_box[1] = 1.0f - area;
  picker->pick_box[2] = picker->pick_box[3] = area;

  _iop_color_picker_reset(picker);
}

static gboolean _iop_color_picker_callback_button_press(GtkWidget *button, GdkEventButton *e, dt_iop_color_picker_t *self)
{
  // FIXME: this is key -- module for lib picker is intialized to NULL, this use colorout -- is this still important?
  dt_iop_module_t *module = self->module ? self->module : dt_iop_get_colorout_module();
  dt_develop_t *dev = module->dev;

  if(!module || darktable.gui->reset) return FALSE;

  // If switching from another module, turn off the picker in that
  // module. If we are holding onto the colorpick libs picker, we do
  // want to maintain its (de-focused) picker for readouts and
  // potentially a scope restricted to picker selection
  // FIXME: do only need to test final clause?
  if(darktable.lib->proxy.colorpicker.picker_source &&
     module != darktable.lib->proxy.colorpicker.picker_source)
  {
    _iop_color_picker_reset(darktable.lib->proxy.colorpicker.picker_source->picker);
    darktable.lib->proxy.colorpicker.primary_sample->size = DT_LIB_COLORPICKER_SIZE_NONE;
    darktable.lib->proxy.colorpicker.picker_source = NULL;
    // FIXME: this is same as dt_iop_color_picker_reset
    dev->gui_module->picker = NULL;
    dev->gui_module->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  }

  // set module active if not yet the case
  if(module->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), TRUE);

  const GdkModifierType state = e != NULL ? e->state : dt_key_modifier_state();
  const gboolean ctrl_key_pressed = dt_modifier_is(state, GDK_CONTROL_MASK) || (e != NULL && e->button == 3);
  dt_iop_color_picker_kind_t kind = self->kind;

  // turn off any existing picker in the module
  _iop_color_picker_reset(module->picker);

  if (module->picker != self || (kind == DT_COLOR_PICKER_POINT_AREA &&
      (ctrl_key_pressed ^ (darktable.lib->proxy.colorpicker.primary_sample->size == DT_LIB_COLORPICKER_SIZE_BOX))))
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
    // pull picker's last recorded positions, initializing if
    // necessary set primary picker to this picker's position
    // FIXME: this should remember the last area for the primary picker -- but doesn't
    // FIXME: make equivalent dt_lib_colorpicker_get_loc()?
    // FIXME: these call _update_size() which calls _update_picker_output() which enables picker button for the lib picker -- does this cause a loop?
    if(kind == DT_COLOR_PICKER_AREA)
      dt_lib_colorpicker_set_box_area(darktable.lib, self->pick_box);
    else if(kind == DT_COLOR_PICKER_POINT)
      dt_lib_colorpicker_set_point(darktable.lib, self->pick_pos);
    else
      dt_unreachable_codepath();

    darktable.lib->proxy.colorpicker.picker_source = self->module;
    module->dev->preview_status = DT_DEV_PIXELPIPE_DIRTY;
    dt_iop_request_focus(module);
  }
  else
  {
    module->picker = NULL;
    module->request_color_pick = DT_REQUEST_COLORPICK_OFF;
    darktable.lib->proxy.colorpicker.primary_sample->size = DT_LIB_COLORPICKER_SIZE_NONE;
    darktable.lib->proxy.colorpicker.picker_source = NULL;
    if(darktable.lib->proxy.colorpicker.restrict_histogram)
      module->dev->preview_status = DT_DEV_PIXELPIPE_DIRTY;
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
  if(module->picker && module->picker->picker_cst != picker_cst)
  {
    module->picker->picker_cst = picker_cst;
    module->picker->pick_pos[0] = NAN; // trigger difference on next apply
  }
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
  // this callback happens when an iop (not lib) colorpicker receives
  // new data from the pixelpipe
  dt_develop_t *dev = module->dev;

  // Invalidate the cache to ensure it will be fully recomputed.
  // modules between colorin & colorout may need the work_profile
  // to work properly. This will force colorin to be run and it
  // will set the work_profile if needed.

  if(!dev) return;

  dev->preview_pipe->changed |= DT_DEV_PIPE_REMOVE;
  dev->preview_pipe->cache_obsolete = 1;

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

static GtkWidget *_color_picker_new(dt_iop_module_t *module, dt_iop_color_picker_kind_t kind, GtkWidget *w,
                                    const gboolean init_cst, const dt_iop_colorspace_type_t cst)
{
  dt_iop_color_picker_t *color_picker = (dt_iop_color_picker_t *)g_malloc(sizeof(dt_iop_color_picker_t));

  if(w == NULL || GTK_IS_BOX(w))
  {
    GtkWidget *button = dtgtk_togglebutton_new(dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_BG_TRANSPARENT, NULL);
    _iop_init_picker(color_picker, module, kind, button);
    if(init_cst)
      color_picker->picker_cst = cst;
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
    if(init_cst)
      color_picker->picker_cst = cst;
    g_signal_connect_data(G_OBJECT(w), "quad-pressed",
                          G_CALLBACK(_iop_color_picker_callback), color_picker, (GClosureNotify)g_free, 0);

    return w;
  }
}

GtkWidget *dt_color_picker_new(dt_iop_module_t *module, dt_iop_color_picker_kind_t kind, GtkWidget *w)
{
  return _color_picker_new(module, kind, w, FALSE, iop_cs_NONE);
}

GtkWidget *dt_color_picker_new_with_cst(dt_iop_module_t *module, dt_iop_color_picker_kind_t kind, GtkWidget *w,
                                        const dt_iop_colorspace_type_t cst)
{
  return _color_picker_new(module, kind, w, TRUE, cst);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
