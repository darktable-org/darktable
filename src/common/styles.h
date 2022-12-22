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

#include "common/darktable.h"
#include "develop/blend.h"
#include "develop/imageop.h"

#include <glib.h>
#include <inttypes.h>
#include <sqlite3.h>

/** The definition of styles are copied historystack to
        reproduce an development style such as sepia, cross process
        etc.
*/

typedef struct dt_style_t
{
  gchar *name;
  gchar *description;
} dt_style_t;

typedef enum dt_style_applymode_t
{
  DT_STYLE_HISTORY_APPEND = 0,
  DT_STYLE_HISTORY_OVERWRITE = 1
} dt_style_applymode_t;

typedef struct dt_style_item_t
{
  int num, selimg_num, enabled, multi_priority;
  int iop_order;
  gchar *name, *operation, *multi_name;
  int module_version, blendop_version;
  dt_iop_params_t *params;
  dt_develop_blend_params_t *blendop_params;
  int32_t params_size, blendop_params_size;
} dt_style_item_t;

/** helpers that free a style or style_item. can be used in g_list_free_full() */
void dt_style_free(gpointer data);
void dt_style_item_free(gpointer data);

/** creates a new style from specified image, items are the history stack number of items to include in style
 */
gboolean dt_styles_create_from_image(const char *name, const char *description,
                                     const int32_t imgid, GList *items, gboolean copy_iop_order);

/** creates styles from selection */
void dt_styles_create_from_list(const GList *list);

/** creates a new style from specified style, items are the style number of items to include in style */
void dt_styles_create_from_style(const char *name, const char *newname, const char *description,
                                 GList *filter, const int32_t imgid, GList *update,
                                 gboolean copy_iop_order, const gboolean update_iop_order);

/** update a style */
void dt_styles_update(const char *name, const char *newname, const char *description, GList *filter,
                      const int32_t imgid, GList *update,
                      const gboolean copy_iop_order, const gboolean update_iop_order);

/** applies the style to selection of images */
void dt_styles_apply_to_list(const char *name, const GList *list, gboolean duplicate);

/** applies list of styles to selection of images */
void dt_multiple_styles_apply_to_list(GList *styles, const GList *list, gboolean duplicate);

/** applies the item style to dev->history */
void dt_styles_apply_style_item(dt_develop_t *dev, dt_style_item_t *style_item, GList **modules_used, const gboolean append);

/** applies the style to image by imgid, takes care of overwrite and duplicate modes */
void dt_styles_apply_to_image(const char *name, const gboolean duplicate, const gboolean overwrite, const int32_t imgid);

/** applies the style to the currently edited image in the darkroom.
    does nothing if not called with a proper dev struct initialized */
void dt_styles_apply_to_dev(const char *name, const int32_t imgid);

/** delete a style by name */
void dt_styles_delete_by_name_adv(const char *name, const gboolean raise);

/** delete a style by name, raise signal */
void dt_styles_delete_by_name(const char *name);

/** get a style object by name, the object needs to be freed by the caller */
dt_style_t *dt_styles_get_by_name(const char *name);

/** check if style exists by name*/
gboolean dt_styles_exists(const char *name);

/** returns TRUE if the style has a module order defined */
gboolean dt_styles_has_module_order(const char *name);

/** get a list of styles based on filter string */
GList *dt_styles_get_list(const char *filter);

/** get a list of items for a named style
    if imgid != -1, then styles from the corresponding image are also reported if they are
    not already part of the style. If with_multi_name is TRUE the name field will contains
    the multi_name.
*/
GList *dt_styles_get_item_list(const char *name, gboolean params, int imgid, gboolean with_multi_name);

/** get list of items for a named style as a nice string */
char *dt_styles_get_item_list_as_string(const char *name);

/** get a description of a named style */
gchar *dt_styles_get_description(const char *name);

/** save style to file */
void dt_styles_save_to_file(const char *style_name, const char *filedir, gboolean overwrite);

/** load style from file */
void dt_styles_import_from_file(const char *style_path);

/** register style actions for shortcuts at start time */
void dt_init_styles_actions();

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
