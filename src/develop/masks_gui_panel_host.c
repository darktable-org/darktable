/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

// where the flexi masks panel's content lives. bd->relocatable_box holds the
// whole blend and mask panel, in one of three homes per
// "plugins/darkroom/blend/masks_panel_position": the module's own expander
// (the default), the masks_flexi_host utility lib, or the canvas panel of
// gui/gtk.c (dt_ui_flexi_panel_*). This file moves it between them (relocate
// and release, the host lib's reconfigure, the position section of the
// blending options) and builds no panel content: blend_gui.c does
//
// It needs one helper from blend_gui.c (dt_masks_gui_reparent_into) and
// exports its entry points back, all declared in blend_gui_internal.h.

#include "develop/blend_gui_internal.h"

#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "control/conf.h"
#include "develop/develop.h"
#include "dtgtk/button.h"
#include "dtgtk/expander.h"
#include "dtgtk/togglebutton.h"
#include "gui/gtk.h"
#include "libs/lib.h"

int dt_masks_gui_panel_position(void)
{
  return dt_conf_get_int("plugins/darkroom/blend/masks_panel_position");
}

gboolean dt_masks_gui_panel_always_on(void)
{
  return dt_masks_gui_panel_position() == MASKS_PANEL_POS_CANVAS
         && dt_conf_get_bool("plugins/darkroom/masks/always_on_panel");
}

gboolean dt_masks_gui_is_host_lib(const dt_lib_module_t *module)
{
  return darktable.develop && module == darktable.develop->proxy.masks_flexi_host.module;
}

// which edge the panel is docked against. Not part of the position choice:
// the panel opens on whichever edge was clicked, and stays there.
//
// Until the user has pinned it once the key does not exist yet, and the panel
// would fall to the left simply because that is what dt_conf_get_bool() returns
// for an unset key. Land it next to the processing modules instead -- that is
// the panel it belongs with, and it is the right-hand one unless
// "plugins/darkroom/panel_swap" has moved it over (see
// dt_ui_container_swap_left_right in views/view.c). Not written back here: the
// derived side keeps tracking the preference until the user's first pin freezes
// it (dt_ui_flexi_panel_set_collapsed in gui/gtk.c writes the key then).
gboolean dt_masks_gui_panel_side_right(void)
{
  static const char *key = "plugins/darkroom/blend/masks_panel_side_right";
  if(!dt_conf_key_exists(key))
    return !dt_conf_get_bool("plugins/darkroom/panel_swap");
  return dt_conf_get_bool(key);
}

// let the utility-mode host lib re-apply its live visibility (see
// _reconfigure in masks_flexi_host.c) -- only relevant for
// MASKS_PANEL_POS_UTILITY, a no-op otherwise (its expander just stays
// hidden)
static void _masks_flexi_host_reconfigure(void)
{
  dt_lib_module_t *host = darktable.develop->proxy.masks_flexi_host.module;
  if(host && darktable.develop->proxy.masks_flexi_host.reconfigure)
    darktable.develop->proxy.masks_flexi_host.reconfigure(host);
}

// human-readable mask type name for the canvas panel's edge-strip hint (see
// _flexi_sliver_hint in gui/gtk.c)
static const char *_mask_mode_label(const uint32_t mask_mode)
{
  switch(mask_mode)
  {
  case DEVELOP_MASK_DISABLED: return _("no mask");
  case DEVELOP_MASK_ENABLED: return _("uniformly");
  case DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK: return _("drawn mask");
  case DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL: return _("parametric mask");
  case DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK_CONDITIONAL:
    return _("drawn & parametric mask");
  case DEVELOP_MASK_ENABLED | DEVELOP_MASK_RASTER: return _("raster mask");
  case DEVELOP_MASK_ENABLED | DEVELOP_MASK_FLEXI: return _("flexi mask");
  default: return _("mask");
  }
}

// one fold state for all three positions, so that it carries over between
// them: a fold the user asked for is stored here, one the panel does itself
// (no mask to show) is not.
//
// dt_lib_gui_set_expanded(), which folds the utility lib, always saves the
// lib's own "plugins/<view>/masks_flexi_host/expanded" key, with no
// persist=FALSE as dt_ui_flexi_panel_set_collapsed() has. So this file drives
// that expander from the shared preference (_masks_utility_apply_collapsed,
// dt_iop_gui_blend_masks_panel_relocate), and the lib's key only mirrors it
static gboolean _masks_panel_collapsed_pref(void)
{
  return dt_conf_get_bool("plugins/darkroom/blend/masks_panel_collapsed");
}

// the module that took focus with its mask off: the panel stays folded for it
// whatever the preference says (dt_iop_gui_blend_masks_panel_relocate). That
// fold is the panel's own, so it is not stored, and the preference applies
// again on the next module with a mask. A fold or unfold by the user clears it
static const dt_iop_module_t *_no_mask_fold = NULL;

// the module being focused by a click on its panel's header or arrow: it
// takes focus with its panel open, mask or no mask
static const dt_iop_module_t *_open_on_focus = NULL;

// an always-on panel is folded by the user alone: a module with its mask off does
// not fold it
static gboolean _masks_panel_collapsed_for(const dt_iop_module_t *module)
{
  return _masks_panel_collapsed_pref()
         || (module && module == _no_mask_fold && !dt_masks_gui_panel_always_on());
}

void dt_masks_gui_panel_set_collapsed_pref(const gboolean collapsed)
{
  _no_mask_fold = NULL;
  dt_conf_set_bool("plugins/darkroom/blend/masks_panel_collapsed", collapsed);
}

static void _inline_collapse_toggle(dt_iop_module_t *module, const gboolean locate);

static void _panel_toggle(const gboolean locate)
{
  dt_iop_module_t *module = dt_dev_gui_module();
  if(!module || !module->blend_data) return;
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!bd->masks_support || !bd->masks_inited) return;
  // dispatches on the position itself, utility included
  _inline_collapse_toggle(module, locate);
}

void dt_iop_gui_blend_masks_panel_toggle(void)
{
  _panel_toggle(TRUE);
}

void dt_iop_gui_blend_masks_panel_show(void)
{
  if(_masks_panel_collapsed_pref()) _panel_toggle(FALSE);
}

void dt_iop_gui_blend_masks_panel_fold_unmasked(void)
{
  dt_iop_module_t *module = dt_dev_gui_module();
  if(!module || !module->blend_params
     || module->blend_params->mask_mode != DEVELOP_MASK_DISABLED)
    return;
  _no_mask_fold = module;
  dt_iop_gui_blend_masks_panel_relocate(module);
  dt_iop_gui_blend_masks_panel_sync_toolbox();
}

// why the panel has nothing to show with no module focused, for the controls
// that stand for it: the toolbar button, and the utility lib's header
static const char *_no_focused_module_tip(void)
{
  return _("unavailable: the blend mask panel shows the mask of the focused"
           " module, and no module is focused.\n"
           "click a module's header to focus it");
}

static void _utility_header_hosting(dt_lib_module_t *host, const gboolean hosting);

void dt_iop_gui_blend_masks_panel_sync_toolbox(void)
{
  // the utility lib's unhosted header says why it is empty, which depends on
  // the focused module. Nothing else refreshes it when focus moves between
  // modules without a mask: relocating one of those returns early
  if(darktable.develop && !darktable.develop->proxy.masks_flexi_host.hosted_module
     && dt_masks_gui_panel_position() == MASKS_PANEL_POS_UTILITY)
    _utility_header_hosting(darktable.develop->proxy.masks_flexi_host.module, FALSE);

  GtkWidget *btn = darktable.develop ? darktable.develop->masks_panel_button : NULL;
  if(!btn || !GTK_IS_TOGGLE_BUTTON(btn)) return;

  const dt_iop_module_t *module = dt_dev_gui_module();
  const dt_iop_gui_blend_data_t *bd = module ? module->blend_data : NULL;
  const gboolean usable = bd && bd->masks_support && bd->masks_inited;

  // deliberately not gtk_widget_set_sensitive: an insensitive widget receives no
  // motion events, so its tooltip never shows -- and "why can I not use this?"
  // is exactly what has to be explained here. The button is left sensitive and
  // inert instead: dt_iop_gui_blend_masks_panel_toggle refuses without a masking
  // module, and _masks_panel_quickbutton_clicked re-syncs from the panel's real
  // state afterwards, so a click on it cannot leave the toggle showing a change
  // that did not happen. The unavailable look is carried by the same dimming the
  // "no mask" state uses, since both are reached with mask_active FALSE.
  gtk_widget_set_sensitive(btn, TRUE);

  // four states, on two independent channels, because the button answers two
  // separate questions and the user needs both at a glance:
  //
  //   does the module have a mask?  -> the icon itself: filled and at full
  //                                    strength when it does, outline-only and
  //                                    dimmed when it does not. The fill is the
  //                                    icon's own designed meaning (see
  //                                    dtgtk_cairo_paint_masks_panel), carried
  //                                    on CPF_SPECIAL_FLAG because the toggle
  //                                    overwrites CPF_ACTIVE with its checked
  //                                    state
  //   is the panel showing?         -> the button's box: a highlighted
  //                                    background with a border when it is,
  //                                    nothing at all when it is not
  //
  // so the icon answers "is there a mask" and the box around it answers "is the
  // panel on screen". The dimming must not be dt_dimmed: that class restores
  // full opacity on :checked, which would tie the two answers back together
  // whenever the panel is out.
  const gboolean mask_active =
    usable && module->blend_params
    && module->blend_params->mask_mode != DEVELOP_MASK_DISABLED;
  // the effective state, not the stored preference, which a collapsed module
  // overrides (dt_masks_model_panel_state): the button would show checked with
  // nothing on screen, and the next click "hide" a hidden panel
  const dt_masks_panel_state_t state =
    dt_masks_model_panel_state(dt_masks_gui_panel_position(), usable, usable,
                               module ? module->expanded : FALSE, mask_active,
                               _masks_panel_collapsed_for(module),
                               dt_masks_gui_panel_always_on());
  const gboolean showing = usable && !state.panel_collapsed;

  dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(btn), dtgtk_cairo_paint_masks_panel,
                               mask_active ? CPF_SPECIAL_FLAG : CPF_NONE, NULL);

  if(mask_active) dt_gui_remove_class(btn, "dt_masks_empty");
  else dt_gui_add_class(btn, "dt_masks_empty");

  // the shared fold preference rather than each position's own widget: it is
  // what all three positions already agree on, so this reads the same however
  // the panel is hosted.
  DT_ENTER_GUI_UPDATE();
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), showing);
  DT_LEAVE_GUI_UPDATE();

  // the panel always belongs to one module, so with no module focused there is
  // nothing for it to show: say that, rather than leaving a button that looks
  // unavailable for no stated reason
  // the right-click menu resolves the module itself, so it opens whatever the
  // button's own state is (see _masks_panel_quickbutton_right_click): both
  // branches below say so, or the blending options stay undiscoverable exactly
  // when the button looks least worth clicking
  const char *const rc = _("right-click for the blending options");
  if(!usable)
  {
    gchar *tt = g_strdup_printf(
      "%s\n%s",
      module
        ? _("unavailable: the focused module does not support masks")
        : _no_focused_module_tip(),
      rc);
    gtk_widget_set_tooltip_text(btn, tt);
    g_free(tt);
  }
  else
  {
    gchar *tt = g_strdup_printf(
      _("%s the blend mask panel of the focused module\nmask: %s\n%s"),
      showing ? _("hide") : _("show"), mask_active ? _("on") : _("off"), rc);
    gtk_widget_set_tooltip_text(btn, tt);
    g_free(tt);
  }

  gtk_widget_queue_draw(btn);
}

// set while we drive the utility lib's expander ourselves, so the
// expanded_state callback it fires back is not mistaken for the user folding
// the panel by hand. The utility position's counterpart of the persist
// argument the other two positions' collapse calls take.
static gboolean _driving_host_expander = FALSE;

// put the mask's on/off toggle back in step with the params it reports. Called
// at the end of every relocation, where the widget may have been moved between
// headers -- see the comment at that call for what drifts without it.
static void _sync_mask_enable_toggle(dt_iop_gui_blend_data_t *bd)
{
  if(!bd || !bd->mask_enable_toggle || !GTK_IS_TOGGLE_BUTTON(bd->mask_enable_toggle)) return;
  if(!bd->module || !bd->module->blend_params) return;

  const gboolean on = bd->module->blend_params->mask_mode != DEVELOP_MASK_DISABLED;
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(bd->mask_enable_toggle)) == on) return;

  // guarded: the toggle is driven by a click gesture rather than "toggled"
  // (see _blendop_mask_enable_toggled), and this is a correction, not a user
  // action -- nothing downstream should treat it as one
  DT_ENTER_GUI_UPDATE();
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->mask_enable_toggle), on);
  DT_LEAVE_GUI_UPDATE();
}

static void _masks_utility_apply_collapsed(dt_lib_module_t *host,
                                           const gboolean collapsed)
{
  _driving_host_expander = TRUE;
  dt_lib_gui_set_expanded(host, !collapsed);
  _driving_host_expander = FALSE;
}

// apply the embedded position's collapse state to `module`'s panel: fold the
// body away below the header (or bring it back), and point the header arrow
// the way the next click will take it -- DOWN when open, LEFT when folded,
// as the panel's other expanders closing a header on the right do.
static void _masks_embedded_apply_collapsed(dt_iop_module_t *module,
                                            const gboolean collapsed)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!bd || !bd->masks_panel_reveal || !bd->flexi_inline_collapse_btn) return;

  // slid like a module's expander. A change while the panel is not on screen
  // is immediate, as the revealer does for an unmapped widget
  GtkRevealer *reveal = GTK_REVEALER(bd->masks_panel_reveal);
  if(gtk_revealer_get_reveal_child(reveal) == collapsed)
  {
    gtk_revealer_set_transition_duration(reveal, dt_conf_get_int("darkroom/ui/transition_duration"));
    gtk_revealer_set_reveal_child(reveal, !collapsed);
  }
  dtgtk_button_set_paint(DTGTK_BUTTON(bd->flexi_inline_collapse_btn),
                         dtgtk_cairo_paint_solid_arrow,
                         collapsed ? CPF_DIRECTION_LEFT : CPF_DIRECTION_DOWN, NULL);
  gtk_widget_set_tooltip_text(bd->flexi_inline_collapse_btn,
                              collapsed
                                ? _("show the blend mask panel")
                                : _("hide the blend mask panel"));
}

void dt_iop_gui_blend_masks_panel_fold_embedded(dt_iop_module_t *module)
{
  if(!module || dt_masks_gui_panel_position() != MASKS_PANEL_POS_EMBEDDED) return;
  // the panel's own fold, as for a module that took focus with its mask off:
  // not stored, so the next module with a mask follows the preference again
  _no_mask_fold = module;
  _masks_embedded_apply_collapsed(module, TRUE);
  dt_iop_gui_blend_masks_panel_collapsed(TRUE);
  dt_iop_gui_blend_masks_panel_sync_toolbox();
}

// the header's layout:
//
//   toggle <space> overlay eye edit solo lock expander
//
// with no name in it: the focus row under it names the module, in the
// hosted positions (dt_masks_gui_focus_row_sync; embedded, the module's own
// header does). The controls (masks_header_cluster) are right-aligned, ahead
// of the expander. Embedded, the on/off toggle sits under the module's own
// on/off button and the expander under its presets menu. In the utility
// position the header leads the lib's body as embedded, without the two: the
// toggle moves onto the lib's own header, where its presets menu would be
// (see view_enter in libs/masks_flexi_host.c), and the lib's expander folds
// the panel.
// There is no preferences button: the blending options open on a right-click
// of the on/off toggle (see _blendop_mask_enable_toggled), the way guide
// settings hang off the guides icon. The expander is embedded-only: the
// utility position already has the lib's own expander, and out on
// the canvas the panel is opened and closed from the darkroom toolbar button,
// which is a bigger and more findable target than an icon on a floating panel.
// There the expander stays as a placeholder, `on_canvas`: laid out but not
// drawn, on the panel's outer edge (`expander_start`, docked left, moves it
// to the start). That edge is where the panel insets the header by its
// scrollbar's width, once that is known (_flexi_sync_header_scrollbar_pad in
// gui/gtk.c), and the placeholder takes the inset, so nothing visible shifts
static void _masks_header_apply_side(dt_iop_gui_blend_data_t *bd,
                                     const gboolean on_canvas,
                                     const gboolean expander_start)
{
  GtkWidget *pin = bd->flexi_inline_collapse_btn;
  GtkWidget *toggle = bd->mask_enable_toggle;
  GtkWidget *header = bd->masks_blend_header;
  if(!pin || !toggle || !header) return;
  if(!GTK_IS_WIDGET(pin) || !GTK_IS_WIDGET(toggle) || !GTK_IS_BOX(header)) return;

  // the toggle (back from the utility lib's header, if it was there) at the
  // start and the controls at the end, the expander outermost on its side.
  // Each side by its pack type, since the start children and the end children
  // are laid out from opposite edges; first in the list is outermost
  GtkWidget *controls = bd->masks_header_cluster;
  dt_masks_gui_reparent_into(toggle, header, FALSE, FALSE);
  dt_masks_gui_reparent_into(pin, header, FALSE, FALSE);
  gtk_container_child_set(GTK_CONTAINER(header), toggle, "pack-type", GTK_PACK_START, NULL);
  gtk_container_child_set(GTK_CONTAINER(header), pin,
                          "pack-type", expander_start ? GTK_PACK_START : GTK_PACK_END, NULL);
  if(controls)
  {
    gtk_container_child_set(GTK_CONTAINER(header), controls, "pack-type", GTK_PACK_END, NULL);
    gtk_box_reorder_child(GTK_BOX(header), controls, 0);
  }
  gtk_box_reorder_child(GTK_BOX(header), toggle, 0);
  gtk_box_reorder_child(GTK_BOX(header), pin, 0);
  dt_gui_remove_class(pin, expander_start ? "dt_masks_right" : "dt_masks_left");
  dt_gui_add_class(pin, expander_start ? "dt_masks_left" : "dt_masks_right");

  // the expander works in the embedded position alone, and holds its place
  // on the canvas. child-visible after visible: a widget is allocated while
  // visible, and drawn and reached by the pointer only while child-visible too
  const gboolean show_pin = dt_masks_gui_panel_position() == MASKS_PANEL_POS_EMBEDDED;
  gtk_widget_set_visible(pin, show_pin || on_canvas);
  gtk_widget_set_child_visible(pin, !on_canvas);
  // held with no width of its own: it is there for the scrollbar inset that
  // goes on its margin, and a full-size blank would make the header lopsided
  // and set the least the panel can be narrowed to
  if(on_canvas)
    dt_gui_add_class(pin, "dt_masks_placeholder");
  else
    dt_gui_remove_class(pin, "dt_masks_placeholder");
  gtk_widget_show(toggle);
}

void dt_iop_gui_blend_masks_panel_side_changed(void)
{
  dt_iop_module_t *module =
    darktable.develop ? darktable.develop->proxy.masks_flexi_host.hosted_module : NULL;
  if(!module || !module->blend_data
     || dt_masks_gui_panel_position() != MASKS_PANEL_POS_CANVAS) return;
  _masks_header_apply_side(module->blend_data, TRUE,
                           !dt_ui_flexi_panel_is_right(darktable.gui->ui));
}

static gboolean _scroll_widget_into_view_idle(gpointer user_data)
{
  // held by a reference (see _scroll_into_view_later): a widget destroyed
  // meanwhile is unrealized, and then there is nothing to scroll to
  GtkWidget *widget = GTK_WIDGET(user_data);
  if(!gtk_widget_get_realized(widget)) return G_SOURCE_REMOVE;

  GtkWidget *sw = gtk_widget_get_ancestor(widget, GTK_TYPE_SCROLLED_WINDOW);
  if(!sw) return G_SOURCE_REMOVE;

  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw));
  if(!adj) return G_SOURCE_REMOVE;

  GtkWidget *child = gtk_bin_get_child(GTK_BIN(sw));
  if(GTK_IS_VIEWPORT(child))
    child = gtk_bin_get_child(GTK_BIN(child));
  if(!child) return G_SOURCE_REMOVE;

  gint wx = 0, wy = 0;
  if(!gtk_widget_translate_coordinates(widget, child, 0, 0, &wx, &wy))
    return G_SOURCE_REMOVE;

  gint total_height = gtk_widget_get_allocated_height(widget);
  GtkWidget *extra = g_object_get_data(G_OBJECT(widget), "scroll-extra-child");
  if(extra && GTK_IS_WIDGET(extra) && gtk_widget_get_visible(extra))
    total_height += gtk_widget_get_allocated_height(extra);

  const gdouble cur_val = gtk_adjustment_get_value(adj);
  const gdouble page_size = gtk_adjustment_get_page_size(adj);
  const gdouble lower = gtk_adjustment_get_lower(adj);
  const gdouble upper = gtk_adjustment_get_upper(adj);

  gdouble target_val = cur_val;

  if(wy < cur_val)
    target_val = wy;
  else if(wy + total_height > cur_val + page_size)
  {
    if(total_height <= page_size)
      target_val = wy + total_height - page_size;
    else
      target_val = wy;
  }

  target_val = CLAMP(target_val, lower, MAX(lower, upper - page_size));
  if(target_val != cur_val)
    gtk_adjustment_set_value(adj, target_val);

  return G_SOURCE_REMOVE;
}

// keep a widget where it is on screen while the layout around it moves. The
// panel of the module that had focus folds, and one above the widget takes the
// view with it, so the click would land the user somewhere else. The view is
// put back after each layout of the transition, and let go of when it is over
typedef struct _anchor_t
{
  GtkWidget *widget;
  GdkFrameClock *clock;
  gulong handler;
  guint timeout;
  double offset; // from the top of the viewport
} _anchor_t;

static _anchor_t *_anchor = NULL;

static gboolean _anchor_viewport(GtkWidget *widget, GtkAdjustment **adj, GtkWidget **content)
{
  GtkWidget *sw = gtk_widget_get_ancestor(widget, GTK_TYPE_SCROLLED_WINDOW);
  if(!sw) return FALSE;
  *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw));
  GtkWidget *child = gtk_bin_get_child(GTK_BIN(sw));
  if(child && GTK_IS_VIEWPORT(child)) child = gtk_bin_get_child(GTK_BIN(child));
  *content = child;
  return *adj && child;
}

static void _anchor_stop(void)
{
  _anchor_t *a = _anchor;
  if(!a) return;
  _anchor = NULL;
  if(a->handler) g_signal_handler_disconnect(a->clock, a->handler);
  if(a->timeout) g_source_remove(a->timeout);
  g_object_unref(a->clock);
  g_object_unref(a->widget);
  g_free(a);
}

static void _anchor_layout(GdkFrameClock *clock, gpointer user_data)
{
  _anchor_t *a = user_data;
  GtkAdjustment *adj;
  GtkWidget *content;
  if(!gtk_widget_get_realized(a->widget) || !_anchor_viewport(a->widget, &adj, &content))
    return;

  gint wx = 0, wy = 0;
  if(!gtk_widget_translate_coordinates(a->widget, content, 0, 0, &wx, &wy)) return;

  const double lower = gtk_adjustment_get_lower(adj);
  const double upper = gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj);
  const double want = CLAMP(wy - a->offset, lower, MAX(lower, upper));
  if(fabs(want - gtk_adjustment_get_value(adj)) >= 1.0)
    gtk_adjustment_set_value(adj, want);
}

static gboolean _anchor_timeout(gpointer user_data)
{
  _anchor_t *a = user_data;
  a->timeout = 0;
  _anchor_stop();
  return G_SOURCE_REMOVE;
}

static void _anchor_start(GtkWidget *widget)
{
  _anchor_stop();
  GtkAdjustment *adj;
  GtkWidget *content;
  GdkFrameClock *clock = widget ? gtk_widget_get_frame_clock(widget) : NULL;
  // not on screen: nothing to hold in place
  if(!clock || !gtk_widget_get_mapped(widget) || !_anchor_viewport(widget, &adj, &content))
    return;

  gint wx = 0, wy = 0;
  if(!gtk_widget_translate_coordinates(widget, content, 0, 0, &wx, &wy)) return;

  _anchor_t *a = g_new0(_anchor_t, 1);
  a->widget = g_object_ref(widget);
  a->clock = g_object_ref(clock);
  a->offset = wy - gtk_adjustment_get_value(adj);
  // after the toolkit's own layout of the frame, so the widget is where the
  // frame will draw it
  a->handler = g_signal_connect_after(clock, "layout", G_CALLBACK(_anchor_layout), a);
  a->timeout = g_timeout_add(dt_conf_get_int("darkroom/ui/transition_duration") + 80,
                             _anchor_timeout, a);
  _anchor = a;
}

static void _scroll_into_view_later(GtkWidget *widget)
{
  g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, _scroll_widget_into_view_idle,
                  g_object_ref(widget), g_object_unref);
}

// once the panel has finished sliding open, or its size is not yet the one to
// scroll to
static void _scroll_into_view_after_transition(GtkWidget *widget)
{
  g_timeout_add_full(G_PRIORITY_DEFAULT_IDLE,
                     dt_conf_get_int("darkroom/ui/transition_duration") + 30,
                     _scroll_widget_into_view_idle,
                     g_object_ref(widget), g_object_unref);
}

// `locate`: scroll the embedded panel into view, for a toggle from outside
// the module (the toolbar button, the shortcut), where finding the panel is
// the point. From the module's own arrow or header it is already under the
// pointer, and the view must stay where it is
static void _inline_collapse_toggle(dt_iop_module_t *module, const gboolean locate)
{
  const int pos = dt_masks_gui_panel_position();

  if(pos == MASKS_PANEL_POS_CANVAS)
  {
    const gboolean collapsed = dt_ui_flexi_panel_is_collapsed(darktable.gui->ui);
    if(module && collapsed)
    {
      // deliberately does NOT switch the mask on: showing the panel is a view
      // action and must not write to the image. The panel's controls are live
      // with the mask off, and the first one the user touches switches it on
      // (see _blendop_mask_enable in blend_gui.c)
      if(dt_masks_model_pin_should_expand_iop(module->expanded, collapsed))
      {
        const gboolean collapse_others = dt_conf_get_bool("darkroom/ui/single_module");
        dt_iop_gui_set_expanded(module, TRUE, collapse_others);
      }
    }

    dt_ui_flexi_panel_set_collapsed(darktable.gui->ui, !collapsed, TRUE, TRUE);
    return;
  }

  if(pos == MASKS_PANEL_POS_UTILITY)
  {
    dt_lib_module_t *host = darktable.develop->proxy.masks_flexi_host.module;
    if(host)
    {
      const gboolean exp =
        host->expander && dtgtk_expander_get_expanded(DTGTK_EXPANDER(host->expander));
      // as in the canvas position above: showing the panel never switches the
      // mask on
      dt_lib_gui_set_expanded(host, !exp);
      if(!exp && host->expander)
        _scroll_into_view_later(host->expander);
    }
    return;
  }

  // embedded, and the module is only showing its header: the click is the
  // module reaching for its panel, so focus it, with the panel open
  if(module && module != darktable.develop->gui_module)
  {
    dt_masks_gui_panel_set_collapsed_pref(FALSE);
    _open_on_focus = module;
    dt_iop_request_focus(module);
    _open_on_focus = NULL;
    return;
  }

  // embedded: the header this arrow sits in stays put, so it toggles both ways.
  // What it toggles is what is actually on screen, not the stored state: if the
  // two ever disagree, deriving the click from the stored one "folds" an
  // already-folded panel and the user has to click twice to open it.
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  const gboolean collapsed =
    bd && bd->masks_panel_reveal && gtk_revealer_get_reveal_child(GTK_REVEALER(bd->masks_panel_reveal));
  dt_masks_gui_panel_set_collapsed_pref(collapsed);
  _masks_embedded_apply_collapsed(module, collapsed);
  dt_iop_gui_blend_masks_panel_collapsed(collapsed);
  dt_iop_gui_blend_masks_panel_sync_toolbox();
  if(locate && !collapsed && bd && bd->masks_blend_header)
  {
    g_object_set_data(G_OBJECT(bd->masks_blend_header), "scroll-extra-child",
                      bd->masks_panel_body);
    _scroll_into_view_after_transition(bd->masks_blend_header);
  }
}

void dt_masks_gui_flexi_inline_collapse_clicked(GtkWidget *w, gpointer user_data)
{
  _inline_collapse_toggle((dt_iop_module_t *)user_data, FALSE);
}

// the utility lib's expander was toggled by the user, which is what its own
// collapse control means there -- the counterpart of what the canvas position's
// edge strip and the embedded arrow do for their positions, so the panel folds
// and unfolds the same way wherever it lives.
void dt_iop_gui_blend_masks_panel_host_expanded(const gboolean expanded)
{
  if(dt_masks_gui_panel_position() != MASKS_PANEL_POS_UTILITY) return;

  if(!_driving_host_expander) dt_masks_gui_panel_set_collapsed_pref(!expanded);
  dt_iop_gui_blend_masks_panel_collapsed(!expanded);
  dt_iop_gui_blend_masks_panel_sync_toolbox();
}

// the masking panel just folded away, or came back -- see the header comment
// on this function in blend.h for the contract; this is called from all three
// positions' own collapse mechanisms.
//
// The point: "edit on canvas" and the shape-add tools are driven from this
// panel, so leaving them armed once it is gone strands a live editing overlay
// on canvas with no visible control over it. Turning them off is not enough
// on its own, though -- collapsing the panel to get a clear look at the image
// and then re-opening it should not silently cost the user their editing
// mode, so stash it and put it back.
void dt_iop_gui_blend_masks_panel_collapsed(const gboolean collapsed)
{
  if(!darktable.develop) return;
  // whatever is hosted in a flexi panel; embedded, the focused module (only
  // the focused module's panel is on screen there -- see dt_masks_gui_flexi_release)
  dt_iop_module_t *module = darktable.develop->proxy.masks_flexi_host.hosted_module;
  if(!module) module = darktable.develop->gui_module;
  if(!module) return;

  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!bd || !bd->masks_support || !bd->masks_inited) return;
  // the canvas of a module without shapes is not the panel's
  if(!bd->shapes_support) return;

  if(collapsed)
  {
    // an armed shape-add tool is just as unusable with the panel gone; it has
    // no state worth restoring, unlike the edit mode below
    for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_shapes[n]), FALSE);

    // unconditionally, so a collapse with editing already off also clears any
    // stale stash rather than leaving an older one to be restored later
    bd->masks_shown_stash = bd->masks_shown;
    if(bd->masks_shown == DT_MASKS_EDIT_OFF) return;
    // this untoggles bd->masks_edit itself, and drops any solo-edit
    dt_masks_set_edit_mode(module, DT_MASKS_EDIT_OFF);
  }
  else
  {
    const dt_masks_edit_mode_t stash = bd->masks_shown_stash;
    bd->masks_shown_stash = DT_MASKS_EDIT_OFF;
    // nothing was interrupted, or the user turned editing back on by hand
    // while the panel was away (via a shortcut) -- don't second-guess either
    if(stash == DT_MASKS_EDIT_OFF || bd->masks_shown != DT_MASKS_EDIT_OFF) return;

    // don't restore an overlay onto a mask that lost its shapes in the
    // meantime (module reset, history jump, undo): same guard the "edit on
    // canvas" toggle itself applies before entering edit mode
    dt_masks_form_t *grp =
      dt_masks_get_from_id(darktable.develop, module->blend_params->mask_id);
    if(!grp || !(grp->type & DT_MASKS_GROUP) || !grp->points) return;

    dt_masks_set_edit_mode(module, stash);
  }
}

static void _masks_flexi_release_full(dt_iop_module_t *module, const gboolean handoff);

// does any of this module's panel content currently sit in a host? Asked of
// the widget tree, so it stays true even when hosted_module has lost track.
static gboolean _widgets_are_hosted(dt_iop_gui_blend_data_t *bd)
{
  if(!bd) return FALSE;
  GtkWidget *hosts[3] = { dt_ui_flexi_panel_header(darktable.gui->ui),
                          dt_ui_flexi_panel_content(darktable.gui->ui),
                          GTK_WIDGET(darktable.develop->proxy.masks_flexi_host.content_box) };
  // the three widgets a host can end up owning: header and body separately
  // (LEFT/RIGHT), or the whole relocatable box at once (UTILITY)
  GtkWidget *owned[3] = { bd->masks_blend_header, GTK_WIDGET(bd->masks_panel_body),
                          GTK_WIDGET(bd->relocatable_box) };

  for(int h = 0; h < 3; h++)
    for(int w = 0; w < 3; w++)
      if(hosts[h] && owned[w] && gtk_widget_get_parent(owned[w]) == hosts[h]) return TRUE;
  return FALSE;
}

// hand the panel back whatever the module's focus or position, unlike
// relocate, which re-hosts a focused module. dt_iop_gui_cleanup_module calls
// it before destroying the module's widgets: hosted, masks_blend_header and
// masks_panel_body are the host's children, not the expander's, and would
// stay behind with handlers bound to the freed module. Too late in
// dt_iop_gui_cleanup_blending, after the destroy
void dt_iop_gui_blend_masks_panel_release(dt_iop_module_t *module)
{
  // the module is about to be freed, and a later one could reuse its address
  if(module && module == _no_mask_fold) _no_mask_fold = NULL;
  if(!module || !module->blend_data) return;
  // only when there is something to hand back. This runs for every module on
  // teardown, most of which never hosted anything, and release touches header
  // widgets that a module without an inited masks GUI does not have.
  if(darktable.develop->proxy.masks_flexi_host.hosted_module != module
     && !_widgets_are_hosted(module->blend_data))
    return;
  _masks_flexi_release_full(module, FALSE);
}

gboolean dt_masks_model_canvas_panel_hidden(const gboolean folded,
                                            const gboolean user_action,
                                            const gboolean always_on,
                                            const gboolean in_view,
                                            const gboolean side_visible,
                                            const gboolean keep_shown)
{
  if(folded) return TRUE;
  if(user_action || !always_on || keep_shown) return FALSE;
  return !in_view || !side_visible;
}

gboolean dt_masks_model_tab_keeps_panel(const gboolean always_on,
                                        const gboolean in_view,
                                        const gboolean shown_before_tab)
{
  return always_on && in_view && shown_before_tab;
}

dt_masks_panel_state_t dt_masks_model_panel_state(const int pos,
                                                  const gboolean is_focused,
                                                  const gboolean has_masking,
                                                  const gboolean is_expanded,
                                                  const gboolean mask_active,
                                                  const gboolean panel_pref_collapsed,
                                                  const gboolean always_on)
{
  dt_masks_panel_state_t s;
  s.want_hosted = (pos == MASKS_PANEL_POS_CANVAS
                   || pos == MASKS_PANEL_POS_UTILITY) && is_focused && has_masking;

  if(pos == MASKS_PANEL_POS_CANVAS && always_on)
  {
    // on screen whatever is focused, collapsed or without a mask: only the
    // user folds it. What it shows is the focused module's, when there is one
    s.panel_collapsed = panel_pref_collapsed;
    s.corner_icon_visible = panel_pref_collapsed;
    s.corner_icon_active = s.want_hosted && mask_active;
  }
  else if(pos == MASKS_PANEL_POS_CANVAS)
  {
    if(!is_focused || !has_masking)
    {
      s.panel_collapsed = TRUE;
      s.corner_icon_visible = FALSE;
      s.corner_icon_active = FALSE;
    }
    else if(!is_expanded)
    {
      // module is collapsed: hide the panel, whose controls would be stranded
      // on screen, but keep the edge strips (corner_icon_visible), through
      // which a click shows it
      s.panel_collapsed = TRUE;
      s.corner_icon_visible = TRUE;
      s.corner_icon_active = mask_active;
    }
    else
    {
      // module is expanded: follow the user's preference, whether or not the
      // mask is switched on. An off mask is edited from the same live panel as
      // an on one (the first control touched switches it on), so folding the
      // panel away here would take the controls with it -- and it would also
      // move the panel in response to the image changing, which is not
      // something the user asked for
      s.panel_collapsed = panel_pref_collapsed;
      s.corner_icon_visible = panel_pref_collapsed;
      s.corner_icon_active = mask_active;
    }
  }
  else if(pos == MASKS_PANEL_POS_UTILITY && is_focused && has_masking)
  {
    s.panel_collapsed = panel_pref_collapsed;
    s.corner_icon_visible = FALSE;
    s.corner_icon_active = mask_active;
  }
  else if(pos == MASKS_PANEL_POS_EMBEDDED && is_focused && has_masking)
  {
    s.panel_collapsed = !is_expanded || panel_pref_collapsed;
    s.corner_icon_visible = FALSE;
    s.corner_icon_active = mask_active;
  }
  else
  {
    s.panel_collapsed = TRUE;
    s.corner_icon_visible = FALSE;
    s.corner_icon_active = FALSE;
  }
  return s;
}

gboolean dt_masks_model_pin_should_expand_iop(const gboolean is_expanded,
                                              const gboolean is_collapsed)
{
  return !is_expanded && is_collapsed;
}

// put the panel's header and body back into bd->relocatable_box, in that
// order: the header into its own home there (masks_header_home), the focus
// row after it, the body after that
static void _masks_header_body_home(dt_iop_gui_blend_data_t *bd)
{
  GtkWidget *const parts[3] = { bd->masks_blend_header, GTK_WIDGET(bd->masks_panel_body),
                                bd->masks_focus_row };
  GtkWidget *const homes[3] = { GTK_WIDGET(bd->masks_header_home),
                                GTK_WIDGET(bd->relocatable_box),
                                GTK_WIDGET(bd->masks_header_home) };
  // the focus row is the header's second child, the body the box's second
  const int at[3] = { 0, 1, 1 };
  for(int i = 0; i < 3; i++)
    if(parts[i] && GTK_IS_WIDGET(parts[i]) && homes[i]
       && gtk_widget_get_parent(parts[i]) != homes[i])
    {
      dt_masks_gui_reparent_into(parts[i], homes[i], FALSE, FALSE);
      gtk_box_reorder_child(GTK_BOX(homes[i]), parts[i], at[i]);

      // on the canvas, the header's outermost child is inset by the panel's
      // scrollbar width (_flexi_sync_header_scrollbar_pad in gui/gtk.c), which
      // nothing clears when the header leaves: it would stay pushed in. The
      // focus row is a row of that header too
      if(parts[i] == bd->masks_blend_header || parts[i] == bd->masks_focus_row)
      {
        GList *kids = gtk_container_get_children(GTK_CONTAINER(parts[i]));
        for(GList *k = kids; k; k = g_list_next(k))
        {
          gtk_widget_set_margin_start(GTK_WIDGET(k->data), 0);
          gtk_widget_set_margin_end(GTK_WIDGET(k->data), 0);
        }
        g_list_free(kids);
      }
    }
}

// the utility lib, with or without a module's mask in it: with none, the
// whole lib is insensitive, a stand-in takes the toggle's place, and the
// header's tooltip says why. The header's buttons have no tooltip of their
// own then, so the header's shows over them. Hosting, the reset button
// follows the mask's lock (dt_iop_gui_blend_mask_header_sync)
static void _utility_header_hosting(dt_lib_module_t *host, const gboolean hosting)
{
  GtkWidget *idle = darktable.develop->proxy.masks_flexi_host.idle_toggle;
  if(idle) gtk_widget_set_visible(idle, !hosting);
  if(!host || !host->expander) return;
  gtk_widget_set_sensitive(host->expander, hosting);
  // this position hosts every focused module with masks
  // (dt_masks_model_panel_state), so a focused one left unhosted has none
  const char *idle_tip = dt_dev_gui_module()
    ? _("unavailable: the focused module does not support masks")
    : _no_focused_module_tip();
  gtk_widget_set_tooltip_text(DTGTK_EXPANDER(host->expander)->header,
                              !hosting ? idle_tip
                              : host->description ? host->description(host) : NULL);
  if(host->arrow)
    gtk_widget_set_tooltip_text(host->arrow, hosting ? _("show module") : NULL);
  if(host->reset_button)
    gtk_widget_set_tooltip_text(host->reset_button, hosting ? _("reset mask") : NULL);
}

void dt_masks_gui_utility_header_unhosted(dt_lib_module_t *host)
{
  _utility_header_hosting(host, FALSE);
}

// `handoff`: whether the module giving the panel up is doing so because
// another one is taking focus, in which case the panel is passed straight on
// to whatever dev->gui_module now names. FALSE on teardown, where there is no
// successor to hand it to (see dt_iop_gui_blend_masks_panel_release).
// no module's mask is in the panel (nothing is focused, or the focused module
// has no masking): an always-on panel stays, and says so, any other hides with
// its edge strips
static void _masks_panel_show_nothing(void)
{
  if(dt_masks_gui_panel_always_on())
  {
    dt_iop_gui_blend_masks_panel_idle_sync();
    return;
  }
  dt_ui_flexi_panel_set_idle(darktable.gui->ui, NULL);
  dt_ui_flexi_panel_set_icon(darktable.gui->ui, FALSE, NULL);
  dt_ui_flexi_panel_set_collapsed(darktable.gui->ui, TRUE, FALSE, FALSE);
}

static void _masks_flexi_release_full(dt_iop_module_t *module, const gboolean handoff)
{
  if(!module || !module->blend_data) return;
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!bd->relocatable_box || !GTK_IS_WIDGET(bd->relocatable_box)) return;

  const gboolean is_focused = darktable.develop && darktable.develop->gui_module == module;
  const gboolean embedded =
    dt_masks_gui_panel_position() == MASKS_PANEL_POS_EMBEDDED;
  // embedded, an expanded module that is not focused keeps its header, so that
  // it does not look as if it had lost its mask; the rest waits for focus
  const gboolean peek = embedded && !is_focused && module->expanded && bd->masks_support;
  const gboolean show = (is_focused && module->expanded) || peek;

  const gboolean was_hosted =
    darktable.develop->proxy.masks_flexi_host.hosted_module == module;
  if(was_hosted) darktable.develop->proxy.masks_flexi_host.hosted_module = NULL;

  // the header's controls come home with _masks_header_apply_side below
  _masks_header_body_home(bd);
  if(bd->masks_blend_header && GTK_IS_WIDGET(bd->masks_blend_header))
    gtk_widget_set_visible(bd->masks_blend_header, TRUE);
  dt_masks_gui_focus_row_sync(bd, FALSE);

  if(bd->iopw && GTK_IS_WIDGET(bd->iopw))
  {
    dt_masks_gui_reparent_into(GTK_WIDGET(bd->relocatable_box), bd->iopw, FALSE, FALSE);
    gtk_widget_set_visible(GTK_WIDGET(bd->relocatable_box), show);
  }
  // back in the module's own expander. When that is where the panel actually
  // lives (embedded), the in-header arrow keeps working, now folding the panel
  // body away in place; when the box only landed here because this module lost
  // focus, its real home is a host and the arrow has nothing to act on (both
  // decided by _masks_header_apply_side)
  _masks_header_apply_side(bd, FALSE, FALSE);
  _masks_embedded_apply_collapsed(module, peek || (embedded && (!module->expanded || _masks_panel_collapsed_pref())));
  // back in the module's own content -- restore the embedded inset (see
  // darktable.css's "#blending-tabs.dt_masks_embedded")
  dt_gui_add_class(bd->masks_blend_header, "dt_masks_embedded");

  if(was_hosted)
  {
    dt_lib_module_t *util_host = darktable.develop->proxy.masks_flexi_host.module;
    if(util_host && util_host->expander)
    {
      _masks_utility_apply_collapsed(util_host, TRUE);
      dt_masks_gui_utility_header_unhosted(util_host);
    }
    if(util_host && util_host->preset_label && GTK_IS_LABEL(util_host->preset_label))
      gtk_label_set_text(GTK_LABEL(util_host->preset_label), "");

    _masks_flexi_host_reconfigure();
    // dev->gui_module is already updated to the new focus target (or NULL)
    // by the time this runs -- see dt_iop_request_focus in imageop.c, which
    // sets it before calling lose_focus on the outgoing module
    dt_iop_module_t *next = darktable.develop->gui_module;
    dt_iop_gui_blend_data_t *next_bd = next ? next->blend_data : NULL;
    // no handing over on teardown: gui_module is then a module about to be
    // freed, and showing the panel for it would undo darkroom's leave(), with
    // an empty panel left over the lighttable
    const gboolean next_wants_host =
      handoff && next && next_bd && next_bd->masks_support;
    if(next_wants_host)
    {
      const int pos = dt_masks_gui_panel_position();
      if(pos == MASKS_PANEL_POS_CANVAS)
      {
        const gboolean next_mask_active =
          next->blend_params && next->blend_params->mask_mode != DEVELOP_MASK_DISABLED;
        // the fold relocate is about to give `next`, so the panel does not
        // open for it in between
        const gboolean always_on = dt_masks_gui_panel_always_on();
        const dt_masks_panel_state_t state =
          dt_masks_model_panel_state(pos, TRUE, TRUE, next->expanded, next_mask_active,
                                     _masks_panel_collapsed_pref()
                                       || (!always_on && !next_mask_active),
                                     always_on);
        dt_ui_flexi_panel_set_icon(darktable.gui->ui, state.corner_icon_active,
                                   _mask_mode_label(next->blend_params ? next->blend_params->mask_mode : 0));
        dt_ui_flexi_panel_set_collapsed(darktable.gui->ui,
                                        state.panel_collapsed,
                                        TRUE, FALSE);
      }
      else
      {
        dt_ui_flexi_panel_set_icon(darktable.gui->ui, FALSE, NULL);
        dt_ui_flexi_panel_set_collapsed(darktable.gui->ui, TRUE, FALSE, FALSE);
      }
    }
    else
      _masks_panel_show_nothing();
  }
}

void dt_masks_gui_flexi_release(dt_iop_module_t *module)
{
  _masks_flexi_release_full(module, TRUE);
}

void dt_iop_gui_blend_masks_panel_peek(dt_iop_module_t *module)
{
  if(!module || !module->blend_data || !darktable.develop) return;
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!bd->relocatable_box || !bd->masks_support) return;
  // the focused module's panel is relocate's
  if(darktable.develop->gui_module == module) return;

  _masks_flexi_release_full(module, FALSE);
  _sync_mask_enable_toggle(bd);
}

// guarantee the host shows exactly one module's panel.
//
// hosted_module is what normally does this: relocate releases it before
// installing its own widgets. But it is a single pointer maintained by hand
// across focus changes, position changes and module teardown, and nothing ever
// removes a header from a host except the release that pairs with it. So any
// path that loses track of a module whose header is still parented in a host
// -- a module destroyed while hosted, a focus change that never reached us --
// strands that header there permanently, and the user gets two stacked panel
// headers with a single body under them.
//
// This asks the widget tree instead of trusting the pointer: any *live* module
// still parented in a host, other than the one taking over, is released
// properly. It logs when it fires, because it firing means one of those paths
// is still wrong and the log names the module that got left behind.
static void _release_stray_hosted(dt_iop_module_t *keep)
{
  for(GList *m = darktable.develop->iop; m; m = g_list_next(m))
  {
    dt_iop_module_t *other = m->data;
    if(other == keep || !other->blend_data) continue;
    if(!_widgets_are_hosted(other->blend_data)) continue;

    dt_print(DT_DEBUG_MASKS,
             "[masks] flexi panel: '%s' was still hosted when '%s' took it over"
             " -- releasing it (its header would have been stranded)",
             other->op, keep->op);
    dt_masks_gui_flexi_release(other);
  }
}

// (re)decide where this module's masking panel content should live, per
// the current "plugins/darkroom/blend/masks_panel_position" preference:
// embedded (default) keeps it inline; utility uses the masks_flexi_host lib
// (DT_UI_CONTAINER_PANEL_LEFT_CENTER); the canvas position uses the panel
// of gui/gtk.c (dt_ui_flexi_panel_*). Hosting only depends
// on the module being focused and masking-capable -- NOT on the current mask
// mode, so the panel's controls stay reachable with the mask off, and whether
// it shows follows the shared fold preference (see dt_masks_model_panel_state),
// except for a module that takes focus with its mask off (_no_mask_fold)
void dt_iop_gui_blend_masks_panel_relocate(dt_iop_module_t *module)
{
  if(!module || !module->blend_data) return;
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!bd->relocatable_box) return;

  // the header's buttons follow the module's power too (the eye): here
  // because every power change of the focused module relocates
  dt_iop_gui_blend_mask_header_sync(bd);

  const int pos = dt_masks_gui_panel_position();
  const uint32_t mask_mode = module->blend_params->mask_mode;
  const gboolean is_focused = darktable.develop->gui_module == module;
  const gboolean has_masking = bd->masks_support;
  const gboolean mask_active = mask_mode != DEVELOP_MASK_DISABLED;
  dt_iop_module_t *prev = darktable.develop->proxy.masks_flexi_host.hosted_module;
  const gboolean focus_changed = (prev != module);

  // a module without a mask does not show the panel the user had open for the
  // previous one. Decided when the module takes focus and not after: the panel
  // must not fold away from under the user switching the mask off
  if(is_focused && focus_changed)
    _no_mask_fold = (mask_active || module == _open_on_focus) ? NULL : module;
  else if(mask_active && _no_mask_fold == module)
    _no_mask_fold = NULL;

  const dt_masks_panel_state_t state =
    dt_masks_model_panel_state(pos, is_focused, has_masking, module->expanded,
                               mask_active, _masks_panel_collapsed_for(module),
                               dt_masks_gui_panel_always_on());

  GtkWidget *target = NULL;
  if(state.want_hosted && pos == MASKS_PANEL_POS_UTILITY)
  {
    dt_lib_module_t *host = darktable.develop->proxy.masks_flexi_host.module;
    GtkBox *content_box = darktable.develop->proxy.masks_flexi_host.content_box;
    if(host && content_box) target = GTK_WIDGET(content_box);
  }
  else if(state.want_hosted) // canvas
  {
    target = dt_ui_flexi_panel_content(darktable.gui->ui);
    dt_ui_flexi_panel_set_side(darktable.gui->ui, dt_masks_gui_panel_side_right());
  }

  if(!target)
  {
    if(pos == MASKS_PANEL_POS_EMBEDDED && is_focused && has_masking)
    {
      // the panel of the module that had focus folds (it is already folding,
      // from its lose_focus), which can move this header: hold it. Nothing has
      // been laid out since, so the header is still where the user clicked
      if(focus_changed) _anchor_start(bd->masks_blend_header);
      if(prev && prev != module) dt_masks_gui_flexi_release(prev);
      darktable.develop->proxy.masks_flexi_host.hosted_module = module;

      // the header's controls come home with _masks_header_apply_side below
      _masks_header_body_home(bd);
      if(bd->masks_blend_header && GTK_IS_WIDGET(bd->masks_blend_header))
        gtk_widget_set_visible(bd->masks_blend_header, TRUE);
      // the module's own header names it
      dt_masks_gui_focus_row_sync(bd, FALSE);

      if(bd->iopw && GTK_IS_WIDGET(bd->iopw))
      {
        dt_masks_gui_reparent_into(GTK_WIDGET(bd->relocatable_box), bd->iopw, FALSE, FALSE);
        gtk_widget_set_visible(GTK_WIDGET(bd->relocatable_box), module->expanded);
      }

      _masks_header_apply_side(bd, FALSE, FALSE);
      if(focus_changed)
        _masks_embedded_apply_collapsed(module, _masks_panel_collapsed_for(module));
      dt_gui_add_class(bd->masks_blend_header, "dt_masks_embedded");
      dt_masks_gui_build_list(module);
      dt_iop_gui_blend_masks_panel_sync_toolbox();
      return;
    }

    // an expanded-but-unfocused module must not show its full blend/mask
    // panel inline, whatever the position preference (see
    // dt_masks_gui_flexi_release, which gates visibility on real focus)
    dt_masks_gui_flexi_release(module);
    dt_iop_gui_blend_masks_panel_sync_toolbox();
    return;
  }

  if(prev && prev != module) dt_masks_gui_flexi_release(prev);
  _release_stray_hosted(module);

  darktable.develop->proxy.masks_flexi_host.hosted_module = module;

  if(pos == MASKS_PANEL_POS_CANVAS)
  {
    dt_ui_flexi_panel_set_active(darktable.gui->ui, mask_active);
    dt_ui_flexi_panel_set_idle(darktable.gui->ui, NULL);
    GtkWidget *hdr_target = dt_ui_flexi_panel_header(darktable.gui->ui);
    GtkWidget *cnt_target = dt_ui_flexi_panel_content(darktable.gui->ui);
    if(hdr_target && bd->masks_blend_header)
    {
      dt_masks_gui_reparent_into(bd->masks_blend_header, hdr_target, FALSE, FALSE);
      gtk_widget_show(bd->masks_blend_header);
    }
    // the row follows the header onto the panel's sticky one, under it
    if(hdr_target && bd->masks_focus_row)
      dt_masks_gui_reparent_into(bd->masks_focus_row, hdr_target, FALSE, FALSE);
    if(cnt_target && bd->masks_panel_body)
    {
      dt_masks_gui_reparent_into(GTK_WIDGET(bd->masks_panel_body), cnt_target, FALSE, FALSE);
      gtk_widget_show(GTK_WIDGET(bd->masks_panel_body));
    }
  }
  else
  {
    _masks_header_body_home(bd);
    dt_masks_gui_reparent_into(GTK_WIDGET(bd->relocatable_box), target, FALSE, FALSE);
    gtk_widget_show(GTK_WIDGET(bd->relocatable_box));
  }

  // hosted: the host itself folds (the canvas panel to its edge strips, the
  // utility lib to its header), so the body is never folded here: undo any
  // embedded fold the box carries over
  if(bd->masks_panel_reveal)
  {
    gtk_revealer_set_transition_duration(GTK_REVEALER(bd->masks_panel_reveal), 0);
    gtk_revealer_set_reveal_child(GTK_REVEALER(bd->masks_panel_reveal), TRUE);
  }
  _masks_flexi_host_reconfigure();

  dt_masks_gui_focus_row_sync(bd, TRUE);

  if(pos == MASKS_PANEL_POS_UTILITY)
  {
    // the panel's header leads the lib's body as it leads the module's
    // embedded, with the embedded look, but without its expander (hidden by
    // _masks_header_apply_side) and its toggle, which goes last on the lib's
    // header
    dt_lib_module_t *host = darktable.develop->proxy.masks_flexi_host.module;
    _utility_header_hosting(host, TRUE);
    _masks_header_apply_side(bd, FALSE, FALSE);
    GtkWidget *toggle_box = GTK_WIDGET(darktable.develop->proxy.masks_flexi_host.toggle_box);
    if(toggle_box) dt_masks_gui_reparent_into(bd->mask_enable_toggle, toggle_box, FALSE, FALSE);
    gtk_widget_show(bd->mask_enable_toggle);
    gtk_widget_set_visible(bd->masks_blend_header, TRUE);
    dt_gui_add_class(bd->masks_blend_header, "dt_masks_embedded");
    // the lib's own header is the header here, so the focus row goes right
    // under it, above the panel header's controls (the canvas has them as its
    // header, and the row below). _masks_header_body_home puts it back
    gtk_box_reorder_child(GTK_BOX(bd->masks_header_home), bd->masks_focus_row, 0);
    // again, now that the lib hosts the mask: its reset button
    dt_iop_gui_blend_mask_header_sync(bd);

    if(host && host->preset_label)
      gtk_widget_set_visible(host->preset_label, FALSE);

    // the shared state, like the other two positions: deriving expansion from
    // mask_mode would make a relocate (a focus change, a mode change)
    // re-expand a lib the user had just folded -- and, since
    // dt_lib_gui_set_expanded persists, overwrite the folded state.
    if(host && focus_changed)
      _masks_utility_apply_collapsed(host, state.panel_collapsed);
  }
  else
  {
    // the toggle comes home with _masks_header_apply_side below
    gtk_widget_set_visible(bd->masks_blend_header, TRUE);
    // on the canvas now -- drop the embedded inset, the panel already
    // provides its own (see darktable.css's "#blending-tabs.dt_masks_embedded")
    dt_gui_remove_class(bd->masks_blend_header, "dt_masks_embedded");
  }

  if(pos == MASKS_PANEL_POS_CANVAS)
  {
    dt_ui_flexi_panel_set_icon(darktable.gui->ui, state.corner_icon_active,
                               _mask_mode_label(mask_mode));
    // the shared state again -- applying it, not deciding it, so persist=FALSE.
    // Also when the panel disagrees with the state without a focus change:
    // expanding the focused module turns state.panel_collapsed from TRUE (a
    // collapsed module never shows the panel) to the user's preference
    const gboolean panel_disagrees =
      state.panel_collapsed != dt_ui_flexi_panel_is_collapsed(darktable.gui->ui);
    if(focus_changed || panel_disagrees)
      dt_ui_flexi_panel_set_collapsed(darktable.gui->ui, state.panel_collapsed,
                                      TRUE, FALSE);

    // no in-header collapse arrow out on the canvas: a small button on a
    // floating panel is a poor target for the one thing you always want to be
    // able to do to it, and the darkroom toolbar's mask-panel button (which
    // shows whether the panel is out) does the same job at a fixed, learnable
    // position. It stays for the embedded position, where the header is the only
    // control there is; out here it only holds its place
    _masks_header_apply_side(bd, TRUE, !dt_ui_flexi_panel_is_right(darktable.gui->ui));
  }
  else
  {
    dt_ui_flexi_panel_set_icon(darktable.gui->ui, FALSE, NULL);
    dt_ui_flexi_panel_set_collapsed(darktable.gui->ui, TRUE, FALSE, FALSE);

    if(pos == MASKS_PANEL_POS_EMBEDDED)
      _masks_header_apply_side(bd, FALSE, FALSE);
  }

  // the on/off toggle is carried between headers by hand above, and a
  // reparented widget keeps whatever state it was last given, which is not
  // necessarily the mask's: hosted in the utility panel it is packed onto the
  // lib's expander header before the module's own gui update has had anything
  // to say about it. Re-assert it from the params, which are the truth.
  //
  // An out-of-date toggle shows the wrong state: the accent is styled on
  // :checked, so it simply loses it. (_blendop_mask_enable_toggled branches on
  // mask_mode rather than on the button for the same reason.)
  _sync_mask_enable_toggle(bd);
  dt_masks_gui_build_list(module);

  // focus moved, or the hosted module's masking changed: the toolbox button
  // reports both ("is there a panel to show" and "is it showing")
  dt_iop_gui_blend_masks_panel_sync_toolbox();
}

// the always-on panel with no module to show: nothing is focused, or the focused
// module has no masking. It stays on screen, as folded as the user left it,
// and says why it is empty
void dt_iop_gui_blend_masks_panel_idle_sync(void)
{
  if(!darktable.develop || darktable.develop->proxy.masks_flexi_host.hosted_module
     || !dt_masks_gui_panel_always_on())
    return;

  struct dt_ui_t *ui = darktable.gui->ui;
  dt_ui_flexi_panel_set_active(ui, FALSE);
  dt_ui_flexi_panel_set_icon(ui, FALSE, NULL);
  dt_ui_flexi_panel_set_idle(ui, dt_dev_gui_module()
                                   ? _("the focused module does not support masks")
                                   : _("no module is focused:\n"
                                       "click a module's header to focus it"));
  dt_ui_flexi_panel_set_collapsed(ui, _masks_panel_collapsed_pref(), TRUE, FALSE);
}

// ---- position preference ---------------------------------------------------

gboolean dt_masks_gui_pref_handle_applies(const int position)
{
  return position == MASKS_PANEL_POS_CANVAS;
}

gboolean dt_masks_gui_pref_always_on_applies(const int position)
{
  return position == MASKS_PANEL_POS_CANVAS;
}

gboolean dt_masks_gui_pref_focus_row_applies(const int position)
{
  return position != MASKS_PANEL_POS_EMBEDDED;
}

// the popover's checks that depend on the position: each is kept on the box
// under its key, and shown while its predicate holds
static const struct
{
  const char *key;
  gboolean (*applies)(int position);
} _position_checks[] = {
  { "dt-focus-row-check", dt_masks_gui_pref_focus_row_applies },
  { "dt-always-on-check", dt_masks_gui_pref_always_on_applies },
  { "dt-handle-check", dt_masks_gui_pref_handle_applies },
};

static void _masks_panel_position_activate(GtkWidget *combo, dt_iop_module_t *module)
{
  DT_GUARD_GUI_UPDATE();

  const int pos = GPOINTER_TO_INT(dt_bauhaus_combobox_get_data(combo));
  dt_conf_set_int("plugins/darkroom/blend/masks_panel_position", pos);

  // the options that depend on the position follow it: the popover stays open
  // while it changes
  for(size_t i = 0; i < G_N_ELEMENTS(_position_checks); i++)
  {
    GtkWidget *check =
      g_object_get_data(G_OBJECT(gtk_widget_get_parent(combo)), _position_checks[i].key);
    if(check) gtk_widget_set_visible(check, _position_checks[i].applies(pos));
  }

  // update the utility-mode host lib's own visibility for the new position
  _masks_flexi_host_reconfigure();

  // always on is the canvas position's alone: the panel is a column there, and
  // an overlay when it comes back from another position
  dt_ui_flexi_panel_update_layout(darktable.gui->ui);

  // leaving the canvas position: hide the canvas panel, not just empty it.
  // dt_iop_gui_blend_masks_panel_relocate()'s release keeps its visibility
  if(pos != MASKS_PANEL_POS_CANVAS)
    dt_ui_flexi_panel_set_collapsed(darktable.gui->ui, TRUE, FALSE, FALSE);

  // repositioning is a deliberate user action -- make sure the result is
  // actually visible, in every position: unfold the panel and store that,
  // before the relocate below applies it. Overriding a fold the user made
  // earlier is the point: explicitly picking a position should show what was
  // picked.
  dt_masks_gui_panel_set_collapsed_pref(FALSE);

  // decide where this (focused) module's content should live now
  if(module)
  {
    dt_iop_gui_blend_masks_panel_relocate(module);

    switch(pos)
    {
    case MASKS_PANEL_POS_CANVAS:
      // relocate folds the panel away when there is no mask; force it open
      dt_ui_flexi_panel_set_collapsed(darktable.gui->ui, FALSE, TRUE, TRUE);
      break;
    case MASKS_PANEL_POS_UTILITY:
    {
      dt_lib_module_t *host = darktable.develop->proxy.masks_flexi_host.module;
      if(host)
        _masks_utility_apply_collapsed(host, FALSE);
      break;
    }
    case MASKS_PANEL_POS_EMBEDDED:
    default:
      // ...and the same override of the no-mask fold as the two hosted cases
      _masks_embedded_apply_collapsed(module, FALSE);
      // scrolls the already-expanded, focused module's own panel into
      // view -- dtgtk_expander_set_expanded(..., TRUE) re-triggers the
      // scroll-to-view animation even when already expanded (see its
      // "Quick Access Panel" comment in dtgtk/expander.c)
      if(module->expander)
        dtgtk_expander_set_expanded(DTGTK_EXPANDER(module->expander), TRUE);
      break;
    }
    // the other expanded modules show their header in the embedded position
    // only
    for(GList *m = darktable.develop->iop; m; m = g_list_next(m))
      dt_iop_gui_blend_masks_panel_peek((dt_iop_module_t *)m->data);
    // the relocate above folded a module without a mask again, and the two
    // forced unfolds that follow it do not store anything to clear that
    _no_mask_fold = NULL;
  }
  // with no module's mask to host (nothing is focused), an always-on panel
  // still shows, and says why it is empty, as _masks_always_on_toggled does
  if(!darktable.develop->proxy.masks_flexi_host.hosted_module) _masks_panel_show_nothing();
  dt_iop_gui_blend_masks_panel_sync_toolbox();
}

static void _masks_focus_row_toggled(GtkToggleButton *mi, dt_iop_module_t *module)
{
  dt_conf_set_bool("plugins/darkroom/masks/show_focus_row",
                   gtk_toggle_button_get_active(mi));
  // shows or hides the row, with the rest of the panel's placement
  if(module) dt_iop_gui_blend_masks_panel_relocate(module);
}

static void _masks_always_on_toggled(GtkToggleButton *mi, dt_iop_module_t *module)
{
  dt_conf_set_bool("plugins/darkroom/masks/always_on_panel", gtk_toggle_button_get_active(mi));
  // the panel changes from an overlay to a column or back, and what it shows
  // follows the fold alone (or the focused module's mask) from here
  dt_ui_flexi_panel_update_layout(darktable.gui->ui);
  if(module) dt_iop_gui_blend_masks_panel_relocate(module);
  // with no module's mask in the panel, it was up for nothing in particular, or
  // it has just become so
  if(!darktable.develop->proxy.masks_flexi_host.hosted_module) _masks_panel_show_nothing();
  dt_iop_gui_blend_masks_panel_sync_toolbox();
}

static void _masks_show_panel_handle_toggled(GtkToggleButton *mi, dt_iop_module_t *module)
{
  dt_conf_set_bool("plugins/darkroom/masks/show_panel_handle",
                   gtk_toggle_button_get_active(mi));
  // the handle is built once, at view init, so it has to be told
  dt_ui_flexi_panel_update_handle(darktable.gui->ui);
}

// a check in `box` for a boolean preference, shown or not as the position says.
// Its handler is connected by the caller, after the state is set
static GtkWidget *_masks_position_check(GtkWidget *box,
                                        const char *key,
                                        const char *label,
                                        const char *tip,
                                        const char *conf,
                                        const gboolean visible)
{
  GtkWidget *check = gtk_check_button_new_with_label(label);
  gtk_widget_set_tooltip_text(check, tip);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), dt_conf_get_bool(conf));
  gtk_widget_set_no_show_all(check, TRUE);
  gtk_widget_set_visible(check, visible);
  dt_gui_box_add(box, check);
  g_object_set_data(G_OBJECT(box), key, check);
  return check;
}

// appends the panel position combo box to `box`, with the options that only
// apply to some positions under it
void dt_masks_gui_add_panel_position_box(GtkWidget *box, dt_iop_module_t *module)
{
  static const struct
  {
    int pos;
    const char *label;
  } items[] = {
    { MASKS_PANEL_POS_EMBEDDED, N_("embedded within each module") },
    { MASKS_PANEL_POS_UTILITY, N_("utility module, left panel") },
    // one entry, not one per side: it opens on the edge the user last opened
    // it on
    { MASKS_PANEL_POS_CANVAS, N_("separate panel, beside the canvas") },
  };

  const int cur_pos = dt_masks_gui_panel_position();
  GtkWidget *combo = dt_masks_gui_pref_combo(
    box, _("panel position"),
    _("where the blend mask panel (groups, elements,\n"
      "refinements) is shown. a change applies at once"));

  // state first, handler after: setting the active entry while building would
  // otherwise read as the user choosing a position and relocate the panel
  DT_ENTER_GUI_UPDATE();
  for(size_t i = 0; i < G_N_ELEMENTS(items); i++)
    dt_bauhaus_combobox_add_full(combo, _(items[i].label), DT_BAUHAUS_COMBOBOX_ALIGN_LEFT,
                                 GINT_TO_POINTER(items[i].pos), NULL, TRUE);
  dt_bauhaus_combobox_set_from_value(combo, cur_pos);
  DT_LEAVE_GUI_UPDATE();

  // the options that depend on the position. They come and go with it
  // (_masks_panel_position_activate), and are no_show_all so the popover's
  // show_all leaves that to the position
  GtkWidget *always_on_check = _masks_position_check(
    box, "dt-always-on-check", _("always-on panel"),
    _("when enabled, the mask panel stays open for every module,\n"
      "with a mask or without, until you fold it away yourself,\n"
      "and takes its room from the canvas instead of floating over it.\n"
      "hiding the left or right panel it is docked next to hides it too"),
    "plugins/darkroom/masks/always_on_panel", dt_masks_gui_pref_always_on_applies(cur_pos));
  g_signal_connect(G_OBJECT(always_on_check), "toggled",
                   G_CALLBACK(_masks_always_on_toggled), module);

  GtkWidget *handle_check = _masks_position_check(
    box, "dt-handle-check", _("show the resize handle"),
    _("when enabled (default), the edge of the mask panel floating\n"
      "over the canvas carries a visible handle with an arrow showing\n"
      "which way the panel folds away.\n"
      "when disabled, the handle is invisible, like the main panels',\n"
      "and still resizes the panel by dragging and hides it on a click."),
    "plugins/darkroom/masks/show_panel_handle", dt_masks_gui_pref_handle_applies(cur_pos));
  g_signal_connect(G_OBJECT(handle_check), "toggled",
                   G_CALLBACK(_masks_show_panel_handle_toggled), module);

  // for the positions that have a row to show
  GtkWidget *row_check = _masks_position_check(
    box, "dt-focus-row-check", _("show focused module under header"),
    _("when enabled (default), a row under the panel's header\n"
      "names the module whose mask the panel shows, and its\n"
      "instance name, if it has one"),
    "plugins/darkroom/masks/show_focus_row", dt_masks_gui_pref_focus_row_applies(cur_pos));
  g_signal_connect(G_OBJECT(row_check), "toggled", G_CALLBACK(_masks_focus_row_toggled), module);

  g_signal_connect(G_OBJECT(combo), "value-changed",
                   G_CALLBACK(_masks_panel_position_activate), module);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
