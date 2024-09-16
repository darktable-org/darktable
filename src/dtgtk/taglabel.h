/*
    This file is part of darktable,
    Copyright (C) 2024 darktable developers.

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

#include "common/tags.h"
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define DTGTK_TYPE_TAG_LABEL dtgtk_tag_label_get_type()
G_DECLARE_FINAL_TYPE(GtkDarktableTagLabel, dtgtk_tag_label, DTGTK, TAG_LABEL, GtkFlowBoxChild)

struct _GtkDarktableTagLabel
{
  GtkFlowBoxChild flow_box_child;
  gint tagid;
};

GtkWidget *dtgtk_tag_label_new(const dt_tag_t *tag_obj);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

