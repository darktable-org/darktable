/*
    This file is part of darktable,
    Copyright (C) 2010-2021 darktable developers.

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

#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/metadata_export.h"
#include "common/action.h"
#include <gmodule.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#ifdef USE_LUA
#include "lua/call.h"
#include "lua/events.h"
#include "lua/format.h"
#include "lua/modules.h"
#include "lua/storage.h"
#include "lua/types.h"
#endif


/** Flag for the format modules */
typedef enum dt_imageio_format_flags_t
{
  FORMAT_FLAGS_SUPPORT_XMP = 1,
  FORMAT_FLAGS_NO_TMPFILE = 2,
  FORMAT_FLAGS_SUPPORT_LAYERS = 4
} dt_imageio_format_flags_t;

/**
 * defines the plugin structure for image import and export.
 *
 * io is handled by the module_format plugins, which in turn is
 * called by the module_storage plugins, which handles the type of export,
 * such as flickr upload or simple on-disk storage.
 */

/*
 * custom data for the module. append private stuff after these.
 * this will be inited once when the export button is hit, so the user can make
 * gui adjustments that won't affect the currently running export.
 */
typedef struct dt_imageio_module_data_t
{
  int max_width, max_height;
  int width, height;
  char style[128];
  gboolean style_append;
} dt_imageio_module_data_t;

struct dt_imageio_module_format_t;
struct dt_dev_pixelpipe_t;
/* responsible for image encoding, such as jpg,png,etc */
typedef struct dt_imageio_module_format_t
{
  dt_action_t actions; // !!! NEEDS to be FIRST (to be able to cast convert)

#define INCLUDE_API_FROM_MODULE_H
#include "imageio/format/imageio_format_api.h"

  // office use only:
  char plugin_name[128];
  GModule *module;

  // gui stuff:
  GtkWidget *widget;

  // data for you to initialize
  void *gui_data;

  luaA_Type parameter_lua_type;

  gboolean ready;
} dt_imageio_module_format_t;


/* responsible for image storage, such as flickr, harddisk, etc */
typedef struct dt_imageio_module_storage_t
{
  dt_action_t actions; // !!! NEEDS to be FIRST (to be able to cast convert)

#define INCLUDE_API_FROM_MODULE_H
#include "imageio/storage/imageio_storage_api.h"

  // office use only:
  char plugin_name[128];
  GModule *module;

  // gui stuff:
  GtkWidget *widget;

  // data for you to initialize
  void *gui_data;

  // saved format
  int format_index;

  luaA_Type parameter_lua_type;
} dt_imageio_module_storage_t;


/* main struct */
typedef struct dt_imageio_t
{
  GList *plugins_format;
  GList *plugins_storage;
} dt_imageio_t;

/* load all modules */
void dt_imageio_init(dt_imageio_t *iio);
/* cleanup */
void dt_imageio_cleanup(dt_imageio_t *iio);

/* get selected imageio plugin for export */
dt_imageio_module_format_t *dt_imageio_get_format();

/* get selected imageio plugin for export */
dt_imageio_module_storage_t *dt_imageio_get_storage();

/* get by name. */
dt_imageio_module_format_t *dt_imageio_get_format_by_name(const char *name);
dt_imageio_module_storage_t *dt_imageio_get_storage_by_name(const char *name);

/* get by index */
dt_imageio_module_format_t *dt_imageio_get_format_by_index(int index);
dt_imageio_module_storage_t *dt_imageio_get_storage_by_index(int index);
int dt_imageio_get_index_of_format(dt_imageio_module_format_t *format);
int dt_imageio_get_index_of_storage(dt_imageio_module_storage_t *storage);

/* add a module into the known module list */
void dt_imageio_insert_storage(dt_imageio_module_storage_t *storage);
/* remove a module from the known module list */
void dt_imageio_remove_storage(dt_imageio_module_storage_t *storage);

// This function returns value of string which stored in the
// "plugins/lighttable/export/resizing_factor" parameter of the configuration file
// and its "num" and "denum" fraction's elements to calculate the scaling factor
// and improve the readability of the displayed string itself in the "scale" field
// of the settings export.
gchar *dt_imageio_resizing_factor_get_and_parsing(double *num, double *denum);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

