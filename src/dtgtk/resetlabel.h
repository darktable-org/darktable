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

#include "develop/imageop.h"
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define DTGTK_TYPE_RESET_LABEL dtgtk_reset_label_get_type()
G_DECLARE_FINAL_TYPE(GtkDarktableResetLabel, dtgtk_reset_label, DTGTK, RESET_LABEL, GtkEventBox)

struct _GtkDarktableResetLabel
{
  GtkEventBox widget;
  GtkLabel *lb;
  dt_iop_module_t *module;
  int offset; // offset in params to reset
  int size;   // size of param to reset
};

/** instantiate a new darktable reset label for the given module and param. */
GtkWidget *dtgtk_reset_label_new(const gchar *label, dt_iop_module_t *module, void *param, int param_size);
/** Sets the text within the GtkResetLabel widget. It overwrites any text that was there before. */
void dtgtk_reset_label_set_text(GtkDarktableResetLabel *label, const gchar *str);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

