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

typedef struct dt_import_metadata_t
{
  GtkWidget *box;   // widget where to put the metadata widgets
  GtkWidget *apply_metadata;
  GtkWidget *grid;
  GtkListStore *m_model;
  GtkListStore *t_model;
} dt_import_metadata_t;

void dt_import_metadata_init(dt_import_metadata_t *metadata);
void dt_import_metadata_update(dt_import_metadata_t *metadata);
void dt_import_metadata_cleanup(dt_import_metadata_t *metadata);
void dt_import_metadata_reset(dt_import_metadata_t *metadata);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
