/*
    This file is part of darktable,
    Copyright (C) 2018-2026 darktable developers.

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
#include "common/color_picker.h"
#include "bauhaus/bauhaus.h"
#include "libs/lib.h"
#include "control/control.h"
#include "gui/gtk.h"
#include "gui/accelerators.h"
#include "develop/blend.h"

/*
  The color_picker_proxy code is an interface which links the UI
  colorpicker buttons in iops (and the colorpicker lib) with the rest
  of the implementation (selecting/drawing colorpicker area in center
  view, reading color value from preview pipe, and displaying results
  in the colorpicker lib).

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

  There is "global" state in darktable.lib->proxy.colorpicker
  including the current picker_proxy and the primary_sample. There
  will be at most one editable sample, with one proxy, at one time in
  the center view.
*/


// FIXME: should this be here or perhaps lib.c?
gboolean dt_iop_color_picker_is_visible(const dt_develop_t *dev)
{
  dt_iop_color_picker_t *proxy = darktable.lib->proxy.colorpicker.picker_proxy;

  const gboolean module_picker = dev->gui_module
    && dev->gui_module->enabled
    && dev->gui_module->request_color_pick != DT_REQUEST_COLORPICK_OFF
    && proxy && proxy->module == dev->gui_module;

  const gboolean primary_picker = proxy && !proxy->module;

  return module_picker || primary_picker;
}

static gboolean _record_point_area(dt_iop_color_picker_t *self)
{
  const dt_colorpicker_sample_t *const sample =
    darktable.lib->proxy.colorpicker.primary_sample;
  gboolean changed = self->changed;
  if(self && sample)
  {
    if(sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
      for(int k = 0; k < 2; k++)
      {
        if(self->pick_pos[k] != sample->point[k])
        {
          self->pick_pos[k] = sample->point[k];
          changed = TRUE;
        }
      }
    else if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
      for(int k = 0; k < 8; k++)
      {
        if(self->pick_box[k] != sample->box[k])
        {
          self->pick_box[k] = sample->box[k];
          changed = TRUE;
        }
      }
  }
  self->changed = FALSE;
  return changed;
}

static void _color_picker_reset(dt_iop_color_picker_t *picker)
{
  if(picker)
  {
    DT_ENTER_GUI_UPDATE();

    if(DTGTK_IS_TOGGLEBUTTON(picker->colorpick))
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(picker->colorpick), FALSE);
    else
      dt_bauhaus_widget_set_quad_active(picker->colorpick, FALSE);

    DT_LEAVE_GUI_UPDATE();
  }
}

void dt_iop_color_picker_reset(dt_iop_module_t *module,
                               const gboolean keep)
{
  dt_iop_color_picker_t *picker = darktable.lib->proxy.colorpicker.picker_proxy;
  if(picker && picker->module == module)
  {
    if(!keep || (strcmp(gtk_widget_get_name(picker->colorpick), "keep-active") != 0))
    {
      _color_picker_reset(picker);
      darktable.lib->proxy.colorpicker.picker_proxy = NULL;
      if(module)
        module->request_color_pick = DT_REQUEST_COLORPICK_OFF;
    }
  }
}

static void _init_picker(dt_iop_color_picker_t *picker,
                         dt_iop_module_t *module,
                         const dt_iop_color_picker_flags_t flags,
                         GtkWidget *button)
{
  DT_IOP_SECTION_FOR_PARAMS_UNWIND(module);

  // module is NULL if primary colorpicker
  picker->module      = module;
  picker->flags       = flags;
  picker->picker_cst  = module ? module->default_colorspace(module, NULL, NULL)
                               : IOP_CS_NONE;
  picker->colorpick   = button;
  picker->changed     = FALSE;
  picker->fixed_cst   = FALSE;
  picker->initialized = FALSE;

  _color_picker_reset(picker);
}

static gboolean _color_picker_callback_button_press(GtkWidget *button,
                                                    const gboolean ctrl,
                                                    const gboolean right,
                                                    dt_iop_color_picker_t *self)
{
  // module is NULL if primary colorpicker
  dt_iop_module_t *module = self->module;

  DT_GUARD_GUI_UPDATE(FALSE);

  dt_iop_color_picker_t *prior_picker = darktable.lib->proxy.colorpicker.picker_proxy;
  if(prior_picker && prior_picker != self)
  {
    _color_picker_reset(prior_picker);
    if(prior_picker->module)
      prior_picker->module->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  }

  if(module && module->off)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), TRUE);

  const gboolean to_area_mode = ctrl || right;
  dt_iop_color_picker_flags_t flags = self->flags;

  // setup if a new picker or switching between point/area mode
  if(prior_picker != self)
  {
    darktable.lib->proxy.colorpicker.picker_proxy = self;

    if(module)
      module->request_color_pick = DT_REQUEST_COLORPICK_MODULE;

    // set point or area mode without stomping on any other flags
    dt_iop_color_picker_flags_t kind = self->flags & DT_COLOR_PICKER_POINT_AREA;
    if(kind == DT_COLOR_PICKER_POINT_AREA)
      kind = to_area_mode ? DT_COLOR_PICKER_AREA : DT_COLOR_PICKER_POINT;
    // pull picker's last recorded positions
    if(kind & DT_COLOR_PICKER_AREA)
    {
      if(!self->initialized)
        dt_lib_colorpicker_reset_box_area(self->pick_box);
      dt_lib_colorpicker_set_box_area(darktable.lib, self->pick_box);
    }
    else if(kind & DT_COLOR_PICKER_POINT)
    {
      if(!self->initialized)
        dt_lib_colorpicker_reset_point(self->pick_pos);
      dt_lib_colorpicker_set_point(darktable.lib, self->pick_pos);
    }
    else
      dt_unreachable_codepath();

    self->initialized = TRUE;

    dt_lib_colorpicker_setup(darktable.lib,
                             flags & DT_COLOR_PICKER_DENOISE,
                             flags & DT_COLOR_PICKER_IO);

    // important to have set up state before toggling button and
    // triggering more callbacks
    DT_ENTER_GUI_UPDATE();
    if(DTGTK_IS_TOGGLEBUTTON(self->colorpick))
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->colorpick), TRUE);
    else
      dt_bauhaus_widget_set_quad_active(self->colorpick, TRUE);
    DT_LEAVE_GUI_UPDATE();

    if(module)
    {
      module->dev->preview_pipe->status = DT_DEV_PIXELPIPE_DIRTY;
      dt_iop_request_focus(module);
    }
    else
    {
      dt_dev_invalidate_all(darktable.develop);
    }
    // force applying the next incoming sample
    self->changed = TRUE;
  }
  else
  {
    darktable.lib->proxy.colorpicker.picker_proxy = NULL;
    _color_picker_reset(self);
    if(module)
    {
      module->request_color_pick = DT_REQUEST_COLORPICK_OFF;
      // will turn off live sample button
      darktable.lib->proxy.colorpicker.update_panel
        (darktable.lib->proxy.colorpicker.module);
    }
    else if(darktable.lib->proxy.colorpicker.restrict_histogram)
    {
      dt_dev_invalidate_all(darktable.develop);
    }
  }

  dt_control_queue_redraw_center();

  return TRUE;
}

static void _color_picker_callback(GtkWidget *button,
                                   dt_iop_color_picker_t *self)
{
  _color_picker_callback_button_press(button,
                                      dt_modifier_is(dt_key_modifier_state(), GDK_CONTROL_MASK),
                                      FALSE,
                                      self);
}

static void _color_picker_clicked(GtkGestureSingle *gesture,
                                   gint n_press,
                                   gdouble x,
                                   gdouble y,
                                   dt_iop_color_picker_t *self)
{
  GtkWidget *button = dt_gui_get_widget(gesture);
  // pass the clicked button through to the callback: a secondary click
  // switches the picker to area mode (as before the gtk4-prep migration)
  _color_picker_callback_button_press(button,
                                      dt_modifier_is(dt_key_modifier_state(), GDK_CONTROL_MASK),
                                      gtk_gesture_single_get_current_button(gesture)
                                        == GDK_BUTTON_SECONDARY,
                                      self);
}

/*
 * Claim the event sequence in CAPTURE phase so the toggle button's own
 * internal GtkGestureMultiPress (GTK_PHASE_BUBBLE, which emits "clicked"
 * and toggles the button on release) never processes the event; the
 * picker callback fully controls the button state instead.  Same pattern
 * as dt_iop_togglebutton_new.
 *
 * GTK4 migration: the pattern is the same, just rename
 * GtkGestureMultiPress to GtkGestureClick. */
static void _gesture_begin_claim(GtkGesture *gesture,
                                  GdkEventSequence *sequence,
                                  gpointer user_data)
{
  gtk_gesture_set_sequence_state(gesture, sequence, GTK_EVENT_SEQUENCE_CLAIMED);
}

/*
 * Shared activation entry for standalone picker toggle buttons (created
 * with a NULL container, e.g. AgX "auto tune levels").  Real clicks go
 * through the CAPTURE-phase gesture above, shortcuts through the action
 * definition below (dt_action_def_color_picker); both end up here,
 * mirroring how bauhaus pickers route clicks and shortcuts through
 * dt_bauhaus_widget_press_quad().
 */
static float _color_picker_widget_toggle(GtkWidget *target,
                                         const dt_action_effect_t effect,
                                         const float move_size)
{
  if(!DT_PERFORM_ACTION(move_size) || !target)
    return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(target));

  dt_iop_color_picker_t *self = g_object_get_data(G_OBJECT(target), DT_COLOR_PICKER_INSTANCE_KEY);
  if(!self) return 0.f;

  if(!DT_ACTION_TOGGLE_NEEDED(effect, move_size, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(target))))
    return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(target));

  _color_picker_callback_button_press(target,
                                      effect == DT_ACTION_EFFECT_TOGGLE_CTRL
                                        || effect == DT_ACTION_EFFECT_ON_CTRL,
                                      effect == DT_ACTION_EFFECT_TOGGLE_RIGHT
                                        || effect == DT_ACTION_EFFECT_ON_RIGHT,
                                      self);

  const gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(target));
  if(!gtk_widget_is_visible(target))
    dt_action_widget_toast(NULL, target, active ? _("on") : _("off"));

  return active;
}

/*
 * Action definition for standalone picker toggle buttons.
 *
 * The generic toggle definition (dt_action_def_toggle in accelerators.c)
 * activates widgets by synthesizing GObject "button-press-event" signals.
 * Those only reach the widget class handler (which dispatches
 * BUBBLE-phase controllers) and carry no device, so they can never
 * trigger the CAPTURE-phase gesture that picker buttons need in order to
 * suppress the button's own internal gesture -- which is why picker
 * shortcuts stopped working after the gtk4-prep migration.  Like bauhaus
 * widgets, picker buttons therefore use their own definition whose
 * process calls the same shared entry as real clicks.
 */
static const dt_action_element_def_t _color_picker_elements[]
  = { { NULL, dt_action_effect_toggle } };

static const dt_shortcut_fallback_t _color_picker_fallbacks[]
  = { { .mods = GDK_CONTROL_MASK    , .effect = DT_ACTION_EFFECT_TOGGLE_CTRL  },
      { .button = DT_SHORTCUT_RIGHT , .effect = DT_ACTION_EFFECT_TOGGLE_RIGHT },
      { .press = DT_SHORTCUT_LONG   , .effect = DT_ACTION_EFFECT_TOGGLE_RIGHT },
      { } };

static float _color_picker_process(gpointer target,
                                   const dt_action_element_t element,
                                   const dt_action_effect_t effect,
                                   const float move_size)
{
  return _color_picker_widget_toggle(target, effect, move_size);
}

const dt_action_def_t dt_action_def_color_picker
  = { N_("toggle"),
      _color_picker_process,
      _color_picker_elements,
      _color_picker_fallbacks };

void dt_iop_color_picker_set_cst(dt_iop_module_t *module,
                                 const dt_iop_colorspace_type_t picker_cst)
{
  dt_iop_color_picker_t *const picker = darktable.lib->proxy.colorpicker.picker_proxy;
  // this is a bit hacky, because the code was built for when a module
  // "owned" an active pcicker
  if(picker && picker->module == module && picker->picker_cst != picker_cst && !picker->fixed_cst)
  {
    picker->picker_cst = picker_cst;
    // force applying next picker data
    picker->changed = TRUE;
  }
}

dt_iop_colorspace_type_t dt_iop_color_picker_get_active_cst(dt_iop_module_t *module)
{
  dt_iop_color_picker_t *picker = darktable.lib->proxy.colorpicker.picker_proxy;
  if(picker && picker->module == module)
    return picker->picker_cst;
  else
    return IOP_CS_NONE;
}

static void _iop_color_picker_pickerdata_ready_callback(gpointer instance,
                                                        dt_iop_module_t *module,
                                                        dt_dev_pixelpipe_t *pipe,
                                                        gpointer user_data)
{
  // an iop colorpicker receives new data from the pixelpipe
  dt_iop_color_picker_t *picker = darktable.lib->proxy.colorpicker.picker_proxy;
  if(!picker) return;

  // Invalidate the cache to ensure it will be fully recomputed.
  // modules between colorin & colorout may need the work_profile
  // to work properly. This will force colorin to be run and it
  // will set the work_profile if needed.
  // FIXME: is this overdoing it? see #14812
  pipe->changed |= DT_DEV_PIPE_REMOVE;
  pipe->cache_obsolete_order = module->iop_order;

  // iops only need new picker data if the pointer has moved
  if(_record_point_area(picker))
  {
    if(!module->blend_data || !blend_color_picker_apply(module, picker->colorpick, pipe))
    {
      if(module->color_picker_apply)
      {
        dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_PICKER,
                      "color picker apply",
                      pipe, module, DT_DEVICE_NONE, NULL, NULL,
                      "%s%s.%s%s. point=%.3f - %.3f. area=%.3f - %.3f / %.3f - %.3f",
                      picker->flags & DT_COLOR_PICKER_POINT ? " point" : "",
                      picker->flags & DT_COLOR_PICKER_AREA  ? " area" : "",
                      picker->flags & DT_COLOR_PICKER_DENOISE ? " denoise" : "",
                      picker->flags & DT_COLOR_PICKER_IO ? " output" : "",
                      picker->pick_pos[0], picker->pick_pos[1],
                      picker->pick_box[0], picker->pick_box[1],
                      picker->pick_box[2], picker->pick_box[3]);

        module->color_picker_apply(module, picker->colorpick, pipe);
      }
    }
  }
}

static void _color_picker_proxy_preview_pipe_callback(gpointer instance,
                                                      gpointer user_data)
{
  dt_iop_color_picker_t *picker = darktable.lib->proxy.colorpicker.picker_proxy;
  if(picker)
  {
    // lib picker is active? record new picker area, but we don't care
    // about changed value as regardless we want to handle the new
    // sample
    if(!picker->module)
      _record_point_area(picker);
  }

  dt_lib_module_t *module = darktable.lib->proxy.colorpicker.module;
  if(module)
  {
    dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_PICKER | DT_DEBUG_VERBOSE,
                  "picker update callback",
                  NULL, NULL, DT_DEVICE_NONE, NULL, NULL);

    // pixelpipe may have run because sample area changed or an iop,
    // regardless we want to the colorpicker lib, which also can
    // provide swatch color for a point sample overlay
    darktable.lib->proxy.colorpicker.update_panel(module);
    darktable.lib->proxy.colorpicker.update_samples(module);
    // FIXME: It appears that DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED --
    // which redraws the center view -- isn't called until all the
    // DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED signal handlers are
    // called. Hence the UI will always update once the picker data
    // updates. But I'm not clear how this is guaranteed to be so.
  }
}

void dt_iop_color_picker_init(void)
{
  // we have incoming iop picker data
  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_CONTROL_PICKERDATA_READY, _iop_color_picker_pickerdata_ready_callback, NULL);
  // we have new primary picker data as preview pipe has run to conclusion
  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED, _color_picker_proxy_preview_pipe_callback, NULL);
}

void dt_iop_color_picker_cleanup(void)
{
  DT_CONTROL_SIGNAL_DISCONNECT(_iop_color_picker_pickerdata_ready_callback, NULL);
  DT_CONTROL_SIGNAL_DISCONNECT(_color_picker_proxy_preview_pipe_callback, NULL);
}

static void _color_picker_destroy(dt_iop_color_picker_t *picker)
{
  // When the widget is destroyed (e.g. during shutdown), clear the proxy pointer
  // before freeing the struct to prevent use-after-free in dt_iop_color_picker_reset.
  if(darktable.lib && darktable.lib->proxy.colorpicker.picker_proxy == picker)
    darktable.lib->proxy.colorpicker.picker_proxy = NULL;
  if(picker->colorpick)
  {
    g_object_set_data(G_OBJECT(picker->colorpick), DT_COLOR_PICKER_INSTANCE_KEY, NULL);
  }
  g_free(picker);
}

static GtkWidget *_color_picker_new(dt_iop_module_t *module,
                                    const dt_iop_color_picker_flags_t flags,
                                    GtkWidget *w,
                                    const gboolean init_cst,
                                    const dt_iop_colorspace_type_t cst)
{
  dt_iop_color_picker_t *color_picker = g_malloc(sizeof(dt_iop_color_picker_t));

  if(w == NULL || GTK_IS_BOX(w))
  {
    GtkWidget *button = dtgtk_togglebutton_new(dtgtk_cairo_paint_colorpicker, 0, NULL);
    dt_gui_add_class(button, "dt_transparent_background");
    _init_picker(color_picker, module, flags, button);
    if(init_cst)
    {
      color_picker->picker_cst = cst;
      color_picker->fixed_cst = TRUE;
    }
    // The button is a GtkToggleButton, which owns its own
    // GtkGestureMultiPress (bubble phase) that emits "clicked" and toggles
    // the button on release.  Use a CAPTURE-phase gesture that claims the
    // event sequence (same pattern as dt_iop_togglebutton_new) so that
    // internal gesture never runs and the picker callback fully controls
    // the button state on real clicks.
    //
    // Shortcuts (dt_action_def_color_picker, see above) call
    // _color_picker_widget_toggle() directly instead of synthesizing
    // GObject "button-press-event" signals, which cannot reach this
    // CAPTURE-phase gesture (the widget class handler only dispatches
    // BUBBLE-phase controllers and synthetic events carry no device).
    GtkGesture *gesture = gtk_gesture_multi_press_new(button);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(gesture),
                                               GTK_PHASE_CAPTURE);
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), 0);
    dt_gui_add_controller(button, gesture);
    g_signal_connect_data(gesture, "pressed",
                          G_CALLBACK(_color_picker_clicked),
                          color_picker, (GClosureNotify)_color_picker_destroy, 0);
    g_signal_connect(gesture, "begin", G_CALLBACK(_gesture_begin_claim), NULL);
    g_object_set_data(G_OBJECT(button), DT_COLOR_PICKER_INSTANCE_KEY, color_picker);
    if(w) gtk_box_pack_start(GTK_BOX(w), button, FALSE, FALSE, 0);

    return button;
  }
  else
  {
    dt_bauhaus_widget_set_quad_paint(w, dtgtk_cairo_paint_colorpicker, 0, NULL);
    dt_bauhaus_widget_set_quad_toggle(w, TRUE);
    dt_bauhaus_widget_set_quad_tooltip(w, _("pick color from image"));
    _init_picker(color_picker, module, flags, w);
    if(init_cst)
    {
      color_picker->picker_cst = cst;
      color_picker->fixed_cst = TRUE;
    }
    g_signal_connect_data(G_OBJECT(w), "quad-pressed",
                          G_CALLBACK(_color_picker_callback),
                          color_picker, (GClosureNotify)_color_picker_destroy, 0);

    return w;
  }
}

GtkWidget *dt_color_picker_new(dt_iop_module_t *module,
                               const dt_iop_color_picker_flags_t flags,
                               GtkWidget *w)
{
  return _color_picker_new(module, flags, w, FALSE, IOP_CS_NONE);
}

GtkWidget *dt_color_picker_new_with_cst(dt_iop_module_t *module,
                                        const dt_iop_color_picker_flags_t flags,
                                        GtkWidget *w,
                                        const dt_iop_colorspace_type_t cst)
{
  return _color_picker_new(module, flags, w, TRUE, cst);
}

gboolean dt_iop_color_picker_is_active(GtkWidget *w)
{
  const dt_iop_color_picker_t *p = darktable.lib->proxy.colorpicker.picker_proxy;
  return p && p->colorpick == w;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
