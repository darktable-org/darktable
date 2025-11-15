    /*
    This file is part of darktable,
    Copyright (C) 2009-2021 darktable developers.

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

G_BEGIN_DECLS

GtkWidget *dt_bauhaus_slider_from_params(dt_iop_module_t *self, const char *param);

GtkWidget *dt_bauhaus_combobox_from_params(dt_iop_module_t *self, const char *param);

GtkWidget *dt_bauhaus_toggle_from_params(dt_iop_module_t *self, const char *param);

// package dt_iop_module_t pointer and section name to pass to a _from_params function
// it will then create a widget action in a section, rather than top level in the module
// optionally pass a box to add the widgets to
#define DT_IOP_SECTION_FOR_PARAMS_DECL(self, section, ...) \
  (dt_iop_module_t){.actions  = DT_ACTION_TYPE_IOP_SECTION,\
                    .get_f   = self->get_f,                \
                    .module   = (GModule*)self,            \
                    .params   = self->params,              \
                    .default_params = self->default_params,\
                    .gui_data = self->gui_data,            \
                    .data     = (void*)section,            \
                    .widget   = __VA_OPT__(TRUE            \
                                ?  GTK_WIDGET(__VA_ARGS__) \
                                :) self->widget }
#define DT_IOP_SECTION_FOR_PARAMS(...)                     \
       &DT_IOP_SECTION_FOR_PARAMS_DECL(__VA_ARGS__)

GtkWidget *dt_iop_togglebutton_new(dt_iop_module_t *self, const char *section, const gchar *label, const gchar *ctrl_label,
                                   GCallback callback, gboolean local, guint accel_key, GdkModifierType mods,
                                   DTGTKCairoPaintIconFunc paint, GtkWidget *box);

GtkWidget *dt_iop_button_new(dt_iop_module_t *self, const gchar *label,
                             GCallback callback, gboolean local, guint accel_key, GdkModifierType mods,
                             DTGTKCairoPaintIconFunc paint, gint paintflags, GtkWidget *box);

/* returns up or !up depending on the masks_updown preference */
gboolean dt_mask_scroll_increases(int up);

GtkWidget *dt_bauhaus_combobox_new_interpolation(dt_iop_module_t *self);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

