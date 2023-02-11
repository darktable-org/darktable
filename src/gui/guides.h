/*
    This file is part of darktable,
    Copyright (C) 2012-2020 darktable developers.

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

#include "develop/imageop.h"
#include "views/view.h"

typedef void (*dt_guides_draw_callback)(cairo_t *cr, const float x, const float y,
                                        const float w, const float h,
                                        const float zoom_scale, void *user_data);

typedef GtkWidget *(*dt_guides_widget_callback)(dt_iop_module_t *self, void *user_data);

typedef struct dt_guides_t
{
  char name[64];
  dt_guides_draw_callback draw;
  dt_guides_widget_callback widget;
  void *user_data;
  GDestroyNotify free;
  gboolean support_flip;
} dt_guides_t;

GList *dt_guides_init();
void dt_guides_cleanup(GList *guides);

void dt_guides_add_guide(const char *name, dt_guides_draw_callback draw, dt_guides_widget_callback widget, void *user_data, GDestroyNotify free);

// create the popover to setup the guides
GtkWidget *dt_guides_popover(dt_view_t *self, GtkWidget *button);
void dt_guides_update_popover_values();

// draw the guide on screen
void dt_guides_draw(cairo_t *cr, const float left, const float top, const float width, const float height,
                    const float zoom_scale);

// routines for the module toolbar button
void dt_guides_update_button_state();
void dt_guides_button_toggled(gboolean active);

// show the menuitem for modules
void dt_guides_add_module_menuitem(void *menu, struct dt_iop_module_t *module);

// show the line in UI modules
void dt_guides_init_module_widget(GtkWidget *box, struct dt_iop_module_t *module);
void dt_guides_update_module_widget(struct dt_iop_module_t *module);

void dt_guides_set_overlay_colors();

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
