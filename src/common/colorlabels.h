/*
    This file is part of darktable,
    Copyright (C) 2010-2024 darktable developers.

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

#include "common/darktable.h"
#include <gtk/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/** array of names and constant to ease label manipulation */
typedef enum dt_colorlables_enum
{
  DT_COLORLABELS_RED,
  DT_COLORLABELS_YELLOW,
  DT_COLORLABELS_GREEN,
  DT_COLORLABELS_BLUE,
  DT_COLORLABELS_PURPLE,
  DT_COLORLABELS_LAST,
} dt_colorlabels_enum;

/** array with all names as strings, terminated by a NULL entry */
extern const char *dt_colorlabels_name[];

/** get the assigned colorlabels of imgid*/
int dt_colorlabels_get_labels(const dt_imgid_t imgid);
/** remove labels associated to imgid */
void dt_colorlabels_remove_all_labels(const dt_imgid_t imgid);
/** assign a color label to imgid - no undo no image group*/
void dt_colorlabels_set_label(const dt_imgid_t imgid,
                              const int color);
/** assign a color label to image imgid or all selected for !dt_is_valid_imgid(imgid)*/
void dt_colorlabels_set_labels(const GList *img,
                               const int color,
                               const gboolean clear_on,
                               const gboolean undo_on);
/** assign a color label to the list of image*/
void dt_colorlabels_toggle_label_on_list(const GList *list,
                                         const int color,
                                         const gboolean undo_on);
/** remove a color label from imgid */
void dt_colorlabels_remove_label(const dt_imgid_t imgid,
                                 const int color);
/** get the name of the color for a given number (could be replaced by an array) */
const char *dt_colorlabels_to_string(int label);
/** check if an image has a color label */
gboolean dt_colorlabels_check_label(const dt_imgid_t imgid,
                                    const int color);

extern const struct dt_action_def_t dt_action_def_color_label;

#ifdef __cplusplus
} // extern "C"
#endif /* __cplusplus */

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
