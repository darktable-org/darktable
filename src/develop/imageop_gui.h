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

/* Say which parameters a notebook page governs.

   A tab is read and reset through the parameter widgets on its page, which
   leaves a page holding none with nothing to say and nothing to do: the
   equalizer modules keep their tabs empty and point one shared graph at the
   channel the current tab selects, and the colour equalizer parks its sliders
   in a stack beside the notebook. Naming the parameters instead settles both,
   and needs no code in the module beyond the declaration: the page reads as
   changed when they differ from the module's defaults, and resetting the tab
   puts them back.

   Ranges are offsets into the module's parameter struct, so use offsetof().
   Several are allowed because a tab often owns rows of more than one array;
   they are copied, so a caller may build them on the stack. */
typedef struct dt_iop_param_range_t
{
  size_t offset;
  size_t size;
} dt_iop_param_range_t;

void dt_iop_page_bind_params(GtkWidget *page,
                             dt_iop_module_t *self,
                             const dt_iop_param_range_t *ranges,
                             const int n_ranges);

// TRUE when a bound page's parameters are away from the module's defaults
gboolean dt_iop_page_params_changed(GtkWidget *page);

/* Put a bound page's parameters back to the module's defaults, refresh the gui
   and commit history. FALSE when the page is not bound, so a caller can fall
   through to whatever else it does. */
gboolean dt_iop_page_params_reset(GtkWidget *page);

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

