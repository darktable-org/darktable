/*
    This file is part of darktable,
    Copyright (C) 2021 darktable developers.

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

#include <gui/gtk.h>

// cache structure
typedef struct dt_act_on_cache_t
{
  GList *images;
  int images_nb;
  gboolean ok;
  int image_over;
  gboolean inside_table;
  GSList *active_imgs;
  gboolean image_over_inside_sel;
  gboolean ordered;
} dt_act_on_cache_t;

// get images to act on for globals change (via libs or accels)
// The list needs to be freed by the caller
GList *dt_act_on_get_images(const gboolean only_visible, const gboolean force, const gboolean ordered);
gchar *dt_act_on_get_query(const gboolean only_visible);

// get the main image to act on during global changes (libs, accels)
dt_imgid_t dt_act_on_get_main_image();

// get only the number of images to act on
int dt_act_on_get_images_nb(const gboolean only_visible, const gboolean force);

// reset the cache
void dt_act_on_reset_cache(const gboolean only_visible);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

