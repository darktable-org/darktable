/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
  FORMAT_FLAGS_NO_TMPFILE = 2
} dt_imageio_format_flags_t;

/**
 * defines the plugin structure for image import and export.
 *
 * io is handled by the module_format plugins, which in turn is
 * called by the module_storage plugins, which handles the type of export,
 * such as flickr upload or simple on-disk storage.
 */

/*
 * custom data for the module. append private stuff after width and height.
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
/* responsible for image encoding, such as jpg,png,etc */
typedef struct dt_imageio_module_format_t
{
  // !!! MUST BE KEPT IN SYNC WITH src/imageio/format/imageio_format_api.h !!!

  // office use only:
  char plugin_name[128];
  GModule *module;

  // gui stuff:
  GtkWidget *widget;

  // data for you to initialize
  void *gui_data;

  // gui and management:
  /** version */
  int (*version)();
  /* get translated module name */
  const char *(*name)();
  /* construct widget above */
  void (*gui_init)(struct dt_imageio_module_format_t *self);
  /* destroy resources */
  void (*gui_cleanup)(struct dt_imageio_module_format_t *self);
  /* reset options to defaults */
  void (*gui_reset)(struct dt_imageio_module_format_t *self);

  /* construct widget above */
  void (*init)(struct dt_imageio_module_format_t *self);
  /* construct widget above */
  void (*cleanup)(struct dt_imageio_module_format_t *self);

  /* gets the current export parameters from gui/conf and stores in this struct for later use. */
  void *(*legacy_params)(struct dt_imageio_module_format_t *self, const void *const old_params,
                         const size_t old_params_size, const int old_version, const int new_version,
                         size_t *new_size);
  size_t (*params_size)(struct dt_imageio_module_format_t *self);
  void *(*get_params)(struct dt_imageio_module_format_t *self);
  void (*free_params)(struct dt_imageio_module_format_t *self, dt_imageio_module_data_t *data);
  /* resets the gui to the parameters as given here. return != 0 on fail. */
  int (*set_params)(struct dt_imageio_module_format_t *self, const void *params, const int size);

  /* returns the mime type of the exported image. */
  const char *(*mime)(dt_imageio_module_data_t *data);
  /* this extension (plus dot) is appended to the exported filename. */
  const char *(*extension)(dt_imageio_module_data_t *data);
  /* get storage max supported image dimension, return 0 if no dimension restrictions exists. */
  int (*dimension)(struct dt_imageio_module_format_t *self, dt_imageio_module_data_t *data, uint32_t *width, uint32_t *height);

  // writing functions:
  /* bits per pixel and color channel we want to write: 8: char x3, 16: uint16_t x3, 32: float x3. */
  int (*bpp)(dt_imageio_module_data_t *data);
  /* write to file, with exif if not NULL, and icc profile if supported. */
  int (*write_image)(dt_imageio_module_data_t *data, const char *filename, const void *in,
                     dt_colorspaces_color_profile_type_t over_type, const char *over_filename,
                     void *exif, int exif_len, int imgid, int num, int total);
  /* flag that describes the available precision/levels of output format. mainly used for dithering. */
  int (*levels)(dt_imageio_module_data_t *data);

  // sometimes we want to tell the world about what we can do
  int (*flags)(dt_imageio_module_data_t *data);

  int (*read_image)(dt_imageio_module_data_t *data, uint8_t *out);
  luaA_Type parameter_lua_type;

} dt_imageio_module_format_t;


/* responsible for image storage, such as flickr, harddisk, etc */
typedef struct dt_imageio_module_storage_t
{
  // !!! MUST BE KEPT IN SYNC WITH src/imageio/storage/imageio_storage_api.h !!!

  // office use only:
  char plugin_name[128];
  GModule *module;

  // gui stuff:
  GtkWidget *widget;

  // data for you to initialize
  void *gui_data;

  // gui and management:
  /** version */
  int (*version)();
  /* get translated module name */
  const char *(*name)(const struct dt_imageio_module_storage_t *self);
  /* construct widget above */
  void (*gui_init)(struct dt_imageio_module_storage_t *self);
  /* destroy resources */
  void (*gui_cleanup)(struct dt_imageio_module_storage_t *self);
  /* reset options to defaults */
  void (*gui_reset)(struct dt_imageio_module_storage_t *self);
  /* allow the module to initialize itself */
  void (*init)(struct dt_imageio_module_storage_t *self);
  /* try and see if this format is supported? */
  int (*supported)(struct dt_imageio_module_storage_t *self, struct dt_imageio_module_format_t *format);
  /* get storage max supported image dimension, return 0 if no dimension restrictions exists. */
  int (*dimension)(struct dt_imageio_module_storage_t *self, dt_imageio_module_data_t *data, uint32_t *width, uint32_t *height);
  /* get storage recommended image dimension, return 0 if no recommendation exists. */
  int (*recommended_dimension)(struct dt_imageio_module_storage_t *self, dt_imageio_module_data_t *data, uint32_t *width, uint32_t *height);

  /* called once at the beginning (before exporting image), if implemented
     * can change the list of exported images (including a NULL list)
   */
  int (*initialize_store)(struct dt_imageio_module_storage_t *self, dt_imageio_module_data_t *data,
                          dt_imageio_module_format_t **format, dt_imageio_module_data_t **fdata,
                          GList **images, const gboolean high_quality, const gboolean upscale);
  /* this actually does the work */
  int (*store)(struct dt_imageio_module_storage_t *self, struct dt_imageio_module_data_t *self_data,
               const int imgid, dt_imageio_module_format_t *format, dt_imageio_module_data_t *fdata,
               const int num, const int total, const gboolean high_quality, const gboolean upscale,
               dt_colorspaces_color_profile_type_t icc_type, const gchar *icc_filename,
               dt_iop_color_intent_t icc_intent);
  /* called once at the end (after exporting all images), if implemented. */
  void (*finalize_store)(struct dt_imageio_module_storage_t *self, dt_imageio_module_data_t *data);

  void *(*legacy_params)(struct dt_imageio_module_storage_t *self, const void *const old_params,
                         const size_t old_params_size, const int old_version, const int new_version,
                         size_t *new_size);
  size_t (*params_size)(struct dt_imageio_module_storage_t *self);
  void *(*get_params)(struct dt_imageio_module_storage_t *self);
  void (*free_params)(struct dt_imageio_module_storage_t *self, dt_imageio_module_data_t *data);
  int (*set_params)(struct dt_imageio_module_storage_t *self, const void *params, const int size);

  void (*export_dispatched)(struct dt_imageio_module_storage_t *self);

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

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
