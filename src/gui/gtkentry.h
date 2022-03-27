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

#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct completion_spec
{
  gchar *varname;
  gchar *description;
} dt_gtkentry_completion_spec;

typedef enum
{
  COMPL_VARNAME = 0,
  COMPL_DESCRIPTION
} dtGtkEntryCompletionSpecCol;

void dt_gtkentry_setup_completion(GtkEntry *entry, const dt_gtkentry_completion_spec *compl_list);

const dt_gtkentry_completion_spec *dt_gtkentry_get_default_path_compl_list();

gchar *dt_gtkentry_build_completion_tooltip_text(const gchar *header,
                                                 const dt_gtkentry_completion_spec *compl_list);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

