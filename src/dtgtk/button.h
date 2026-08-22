/*
    This file is part of darktable,
    Copyright (C) 2010-2020 darktable developers.

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

#pragma once

#include "paint.h"
#include <gtk/gtk.h>

/* forward declarations, see gui/accelerators.h for the real definitions */
struct dt_action_t;
typedef struct dt_action_def_t dt_action_def_t;

G_BEGIN_DECLS

#define DTKGTK_TYPE_BUTTON dtgtk_button_get_type()
G_DECLARE_FINAL_TYPE(GtkDarktableButton, dtgtk_button, DTGTK, BUTTON, GtkButton)

typedef enum _darktable_button_flags
{
  DARKTABLE_BUTTON_SHOW_LABEL = 1
} _darktable_button_flags_t;

struct _GtkDarktableButton
{
  GtkButton widget;
  DTGTKCairoPaintIconFunc icon;
  gint icon_flags;
  void *icon_data;
  GdkRGBA bg, fg;
  GtkWidget *canvas;
};

/** instantiate a new darktable button control passing paint function as content;
 * for the tooltip/action/click wiring variant see dtgtk_button_new_full(). */
GtkWidget *dtgtk_button_new(DTGTKCairoPaintIconFunc paint, gint paintflags, void *paintdata);

/** instantiate a new darktable button and wire the common extras: a tooltip,
 * an action definition and a primary-click callback.  paint/paintflags/
 * paintdata are mandatory; the wiring is optional and passed as a config
 * struct so callers can name the fields they use (designated initializers,
 * everything left out stays off).  This is the convenience constructor for
 * the plain button idiom (dtgtk_button_new + tooltip + dt_action_define +
 * "clicked"); when a press/release gesture or a secondary click is needed,
 * call dt_gui_connect_click()/dt_gui_connect_click_secondary() on the
 * result instead of passing a clicked callback. */
typedef struct dtgtk_button_config_t
{
  /** tooltip text, NULL for none */
  const gchar *tooltip;
  /** tooltip markup (plain text wins if both are set), NULL for none */
  const gchar *tooltip_markup;
  /** action owner for dt_action_define(), NULL for none; pass DT_ACTION(x)
   * or &module->actions -- iop modules dispatch to dt_action_define_iop() */
  struct dt_action_t *action;
  /** action section (submenu), usually NULL */
  const gchar *action_section;
  /** action label, NULL defines an anonymous action */
  const gchar *action_label;
  /** action definition, e.g. &dt_action_def_button */
  const dt_action_def_t *action_def;
  /** "clicked" callback, NULL for none; connected by
   * dtgtk_button_new_full(), and by dtgtk_togglebutton_new_full() for the
   * toggles that use a plain click */
  GCallback clicked_cb;
  /** user data passed to clicked_cb */
  gpointer clicked_data;
  /** "toggled" callback, NULL for none; connected by
   * dtgtk_togglebutton_new_full() */
  GCallback toggled_cb;
  /** user data passed to toggled_cb */
  gpointer toggled_data;
  /** initial active state for dtgtk_togglebutton_new_full() */
  gboolean active;
} dtgtk_button_config_t;

GtkWidget *dtgtk_button_new_full(DTGTKCairoPaintIconFunc paint,
                                 gint paintflags,
                                 void *paintdata,
                                 const dtgtk_button_config_t *config);
/** set the paint function for a button */
void dtgtk_button_set_paint(GtkDarktableButton *button, DTGTKCairoPaintIconFunc paint, gint paintflags, void *paintdata);
/** clear the stale hover/pressed state GTK3 leaves on buttons after a grab
 * (menu, popover, modal dialog) that shadowed them has ended; connect on any
 * button that pops up menus or opens dialogs */
void dtgtk_button_connect_stale_hover_cleanup(GtkWidget *widget);
/** set the active state of the button icon */
void dtgtk_button_set_active(GtkDarktableButton *button, gboolean active);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

