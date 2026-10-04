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

// the host lib of the flexi masks panel in the "utility module" position
// (MASKS_PANEL_POS_UTILITY, see develop/masks_gui_panel_host.c), in
// DT_UI_CONTAINER_PANEL_LEFT_CENTER and folded by its own expander (see
// expanded_state, which ties on-canvas mask editing to it). The canvas
// position does not use it: that is a grid column of src/gui/gtk.c
// (dt_ui_flexi_panel_*).
//
// The lib stays visible to dt_lib_is_visible: view.c builds the expander
// (dt_lib_gui_get_expander) only for libs visible when the view is entered,
// so a hidden lib could not take the panel until the next view change. In
// the other positions self->expander is hidden with gtk_widget_hide()

#include "control/conf.h"
#include "control/signal.h"
#include "develop/blend.h"
#include "develop/blend_gui_internal.h"
#include "develop/develop.h"
#include "dtgtk/expander.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

typedef struct dt_lib_masks_flexi_host_t
{
  GtkBox *content_box;
  GtkBox *actions_box;
  GtkBox *toggle_box;
} dt_lib_masks_flexi_host_t;

static void _reconfigure(dt_lib_module_t *self);

const char *name(dt_lib_module_t *self)
{
  return _("blend mask");
}

const char *description(dt_lib_module_t *self)
{
  return _("blend mask panel of the focused module\n"
           "(right-click the mask on/off toggle for its options)");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_DARKROOM;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position(const dt_lib_module_t *self)
{
  // sorts before the libs sharing its container: the duplicate manager (850),
  // history (900) and snapshots (1000)
  return 2;
}

GtkWidget *gui_tool_box(dt_lib_module_t *self)
{
  dt_lib_masks_flexi_host_t *d = (dt_lib_masks_flexi_host_t *)self->data;
  return GTK_WIDGET(d->actions_box);
}

// show self->expander only in the "utility" position, by gtk_widget_show and
// hide (see the file comment). Only once self->expander exists (view_enter)
static void _reconfigure(dt_lib_module_t *self)
{
  if(!self->expander) return;

  const int pos = dt_masks_gui_panel_position();
  gtk_widget_set_visible(self->expander, pos == MASKS_PANEL_POS_UTILITY);
}

// this lib's expander is the collapse control for the masking panel in the
// "utility module" position -- the counterpart of the canvas panel's edge
// strip and of the embedded position's in-header arrow. Folding it hides the shapes
// list, so the on-canvas editing it drives goes with it (and comes back on
// expand), and a fold the user asked for is recorded as the panel's shared
// collapse preference, the same as in the other two positions. Both are
// masks_gui_panel_host.c's business, not this lib's; forward and let it decide.
void expanded_state(dt_lib_module_t *self, const gboolean expanded)
{
  if(!darktable.develop || !darktable.develop->proxy.masks_flexi_host.hosted_module)
  {
    if(expanded && self->expander)
      dt_lib_gui_set_expanded(self, FALSE);
    return;
  }
  dt_iop_gui_blend_masks_panel_host_expanded(expanded);
}

/* a raster element's badge depends on its source module, which can be
 * switched off, stop masking or be removed without touching this module's
 * forms. Each of those adds a history item, so the badges of every module are
 * refreshed on a history change: the badge belongs to the reader of the mask,
 * not to the module touched. A module with no list returns at once. Here for
 * the lib's darkroom-scoped lifecycle; it runs in every panel position */
static void _history_change_callback(gpointer instance, gpointer user_data)
{
  if(!darktable.develop) return;
  for(GList *m = darktable.develop->iop; m; m = g_list_next(m))
    dt_iop_gui_blend_refresh_mask_badges((dt_iop_module_t *)m->data);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_masks_flexi_host_t *d = g_malloc0(sizeof(dt_lib_masks_flexi_host_t));
  self->data = (void *)d;

  d->content_box = GTK_BOX(dt_gui_vbox());
  gtk_widget_set_name(GTK_WIDGET(d->content_box), "masks-host-content");
  // both live in the expander header, which view.c destroys on every view
  // leave and rebuilds on enter: own them, and take them out of the header
  // before it goes (see view_leave), or the next darkroom visit packs dead
  // widgets and the mask's on/off toggle and overlay button vanish with them
  d->actions_box = GTK_BOX(g_object_ref_sink(dt_gui_hbox()));
  d->toggle_box = GTK_BOX(g_object_ref_sink(dt_gui_hbox()));
  gtk_widget_set_valign(GTK_WIDGET(d->actions_box), GTK_ALIGN_CENTER);
  gtk_widget_set_valign(GTK_WIDGET(d->toggle_box), GTK_ALIGN_CENTER);
  // the theme sizes the module's controls these boxes take in, and only them:
  // the lib's own header buttons (its arrow above all) keep every lib's size
  dt_gui_add_class(GTK_WIDGET(d->actions_box), "dt_masks_host_controls");
  dt_gui_add_class(GTK_WIDGET(d->toggle_box), "dt_masks_host_controls");
  gtk_widget_show(GTK_WIDGET(d->actions_box));
  gtk_widget_show(GTK_WIDGET(d->toggle_box));
  self->widget = GTK_WIDGET(d->content_box);
  gtk_widget_show_all(self->widget);

  darktable.develop->proxy.masks_flexi_host.module = self;
  darktable.develop->proxy.masks_flexi_host.content_box = d->content_box;
  darktable.develop->proxy.masks_flexi_host.actions_box = d->actions_box;
  darktable.develop->proxy.masks_flexi_host.toggle_box = d->toggle_box;
  darktable.develop->proxy.masks_flexi_host.hosted_module = NULL;
  darktable.develop->proxy.masks_flexi_host.reconfigure = _reconfigure;

  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_HISTORY_CHANGE, _history_change_callback);

  // deliberately NOT calling dt_lib_set_visible(self, FALSE) here even
  // when the current position isn't utility -- see file comment.
  // self->expander doesn't exist yet at this point anyway (view.c builds
  // it after gui_init returns); the initial visual state is applied from
  // view_enter() instead.
}

void view_enter(dt_lib_module_t *self,
                struct dt_view_t *old_view,
                struct dt_view_t *new_view)
{
  // self->expander now exists (view.c just built it) -- apply the initial
  // visual state for the current masks_panel_position
  _reconfigure(self);

  dt_lib_masks_flexi_host_t *d = (dt_lib_masks_flexi_host_t *)self->data;
  if(self->expander && d && d->toggle_box && !gtk_widget_get_parent(GTK_WIDGET(d->toggle_box)))
  {
    GtkWidget *header = DTGTK_EXPANDER(self->expander)->header;
    gtk_box_pack_end(GTK_BOX(header), GTK_WIDGET(d->toggle_box), FALSE, FALSE, 0);
    // visual reading order from left to right: overlay | toggle
    // For GTK_PACK_END, the earlier child in the list is placed further to the right.
    // child 2 = toggle (rightmost), child 3 = overlay
    gtk_box_reorder_child(GTK_BOX(header), GTK_WIDGET(d->toggle_box), 2);
    if(d->actions_box)
      gtk_box_reorder_child(GTK_BOX(header), GTK_WIDGET(d->actions_box), 3);
    gtk_widget_show(GTK_WIDGET(d->toggle_box));
  }

  if(self->expander)
  {
    dt_gui_add_class(DTGTK_EXPANDER(self->expander)->header, "dt_masks_host_header");
    if(self->arrow) gtk_widget_set_valign(self->arrow, GTK_ALIGN_CENTER);
  }

  if(self->reset_button)
    gtk_widget_set_visible(self->reset_button, FALSE);

  // the lib has no presets or preferences, so lib.c's header hamburger has
  // nothing to open; the masking options open on a right-click of the on/off
  // toggle in this header (_blendop_mask_enable_toggled). no_show_all: the
  // header gets a gtk_widget_show_all on every view enter
  if(self->presets_button)
  {
    gtk_widget_set_no_show_all(self->presets_button, TRUE);
    gtk_widget_hide(self->presets_button);
  }

  dt_iop_module_t *module = darktable.develop ? darktable.develop->gui_module : NULL;
  if(module)
  {
    dt_iop_gui_blend_masks_panel_relocate(module);
  }
  else
  {
    if(self->expander) dt_lib_gui_set_expanded(self, FALSE);
    dt_masks_gui_utility_header_unhosted(self);
  }
}

void view_leave(dt_lib_module_t *self,
                struct dt_view_t *old_view,
                struct dt_view_t *new_view)
{
  dt_lib_masks_flexi_host_t *d = (dt_lib_masks_flexi_host_t *)self->data;
  if(!d) return;
  GtkWidget *boxes[] = { GTK_WIDGET(d->toggle_box), GTK_WIDGET(d->actions_box) };
  for(int i = 0; i < G_N_ELEMENTS(boxes); i++)
  {
    GtkWidget *parent = boxes[i] ? gtk_widget_get_parent(boxes[i]) : NULL;
    if(parent) gtk_container_remove(GTK_CONTAINER(parent), boxes[i]);
  }
  // they point into the same header; the next use finds them in the new one
  darktable.develop->proxy.masks_flexi_host.header_label = NULL;
  darktable.develop->proxy.masks_flexi_host.label_evb = NULL;
}

void gui_cleanup(dt_lib_module_t *self)
{
  DT_CONTROL_SIGNAL_DISCONNECT(_history_change_callback, self);

  darktable.develop->proxy.masks_flexi_host.module = NULL;
  darktable.develop->proxy.masks_flexi_host.content_box = NULL;
  darktable.develop->proxy.masks_flexi_host.actions_box = NULL;
  darktable.develop->proxy.masks_flexi_host.toggle_box = NULL;
  darktable.develop->proxy.masks_flexi_host.header_label = NULL;
  darktable.develop->proxy.masks_flexi_host.label_evb = NULL;
  darktable.develop->proxy.masks_flexi_host.hosted_module = NULL;
  darktable.develop->proxy.masks_flexi_host.reconfigure = NULL;

  dt_lib_masks_flexi_host_t *d = (dt_lib_masks_flexi_host_t *)self->data;
  if(d)
  {
    g_clear_object(&d->actions_box);
    g_clear_object(&d->toggle_box);
  }
  g_free(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
