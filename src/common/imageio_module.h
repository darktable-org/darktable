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
#ifndef DT_IMAGEIO_MODULE_H
#define DT_IMAGEIO_MODULE_H

#include <gmodule.h>
#include <gtk/gtk.h>
#include <inttypes.h>

#ifdef USE_LUA
#include "lua/types.h"
#endif
/** Flag for the format modules */
#define FORMAT_FLAGS_SUPPORT_XMP   1

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
}
dt_imageio_module_data_t;

struct dt_imageio_module_format_t;
/* responsible for image encoding, such as jpg,png,etc */
typedef struct dt_imageio_module_format_t
{
  // office use only:
  char plugin_name[128];
  GModule *module;

  // gui stuff:
  GtkWidget *widget;

  // data for you to initialize
  void *gui_data;

  // gui and management:
  /* get translated module name */
  const char* (*name) ();
  /* construct widget above */
  void (*gui_init)    (struct dt_imageio_module_format_t *self);
  /* destroy resources */
  void (*gui_cleanup) (struct dt_imageio_module_format_t *self);
  /* reset options to defaults */
  void (*gui_reset)   (struct dt_imageio_module_format_t *self);

  /* construct widget above */
  void (*init)    (struct dt_imageio_module_format_t *self);
  /* construct widget above */
  void (*cleanup)    (struct dt_imageio_module_format_t *self);

  /* gets the current export parameters from gui/conf and stores in this struct for later use. */
  void* (*get_params)   (struct dt_imageio_module_format_t *self, int *size);
  void  (*free_params)  (struct dt_imageio_module_format_t *self, dt_imageio_module_data_t *data);
  /* resets the gui to the paramters as given here. return != 0 on fail. */
  int   (*set_params)   (struct dt_imageio_module_format_t *self, const void *params, const int size);

  /* returns the mime type of the exported image. */
  const char* (*mime)      (dt_imageio_module_data_t *data);
  /* this extension (plus dot) is appended to the exported filename. */
  const char* (*extension) (dt_imageio_module_data_t *data);
  /* get storage max supported image dimension, return 0 if no dimension restrictions exists. */
  int (*dimension)    (struct dt_imageio_module_format_t *self, uint32_t *width, uint32_t *height);

  // optional: functions operating in memory, not on files:
  /* reads the header and fills width/height in data struct. */
  int (*decompress_header)(const void *in, size_t length, dt_imageio_module_data_t *data);
  /* reads the whole image to the out buffer, which has to be large enough. */
  int (*decompress)(dt_imageio_module_data_t *data, uint8_t *out);
  /* compresses in to out buffer. out buffer must be large enough. returns actual data length. */
  int (*compress)(dt_imageio_module_data_t *data, const uint8_t *in, uint8_t *out);

  // writing functions:
  /* bits per pixel and color channel we want to write: 8: char x3, 16: uint16_t x3, 32: float x3. */
  int (*bpp)(dt_imageio_module_data_t *data);
  /* write to file, with exif if not NULL, and icc profile if supported. */
  int (*write_image)(dt_imageio_module_data_t *data, const char *filename, const void *in, void *exif, int exif_len, int imgid);

  // sometimes we want to tell the world about what we can do
  int (*flags)();

  // reading functions:
  /* read header from file, get width and height */
  int (*read_header)(const char *filename, dt_imageio_module_data_t *data);
  /* reads the image to the (sufficiently allocated) buffer, closes file. */
  int (*read_image)(dt_imageio_module_data_t *data, uint8_t *out);

}
dt_imageio_module_format_t;


/* responsible for image storage, such as flickr, harddisk, etc */
typedef struct dt_imageio_module_storage_t
{
  // office use only:
  char plugin_name[128];
  GModule *module;

  // gui stuff:
  GtkWidget *widget;

  // data for you to initialize
  void *gui_data;

  // gui and management:
  /* get translated module name */
  const char* (*name) ();
  /* construct widget above */
  void (*gui_init)    (struct dt_imageio_module_storage_t *self);
  /* destroy resources */
  void (*gui_cleanup) (struct dt_imageio_module_storage_t *self);
  /* reset options to defaults */
  void (*gui_reset)   (struct dt_imageio_module_storage_t *self);
  /* try and see if this format is supported? */
  int (*supported)    (struct dt_imageio_module_storage_t *self, struct dt_imageio_module_format_t *format);
  /* get storage max supported image dimension, return 0 if no dimension restrictions exists. */
  int (*dimension)    (struct dt_imageio_module_storage_t *self, uint32_t *width, uint32_t *height);
  /* get storage recommended image dimension, return 0 if no recommendation exists. */
  int (*recommended_dimension)    (struct dt_imageio_module_storage_t *self, uint32_t *width, uint32_t *height);

  /* this actually does the work */
  int (*store)(struct dt_imageio_module_data_t *self, const int imgid, dt_imageio_module_format_t *format, dt_imageio_module_data_t *fdata, const int num, const int total, const gboolean high_quality);
  /* called once at the end (after exporting all images), if implemented. */
  int (*finalize_store) (struct dt_imageio_module_storage_t *self, dt_imageio_module_data_t *data);

  void* (*get_params)   (struct dt_imageio_module_storage_t *self, int *size);
  void  (*free_params)  (struct dt_imageio_module_storage_t *self, dt_imageio_module_data_t *data);
  int   (*set_params)   (struct dt_imageio_module_storage_t *self, const void *params, const int size);
  /* a lua function called at .so load time to allow lua access to the structure */
  lua_CFunction lua_init;
}
dt_imageio_module_storage_t;


/* main struct */
typedef struct dt_imageio_t
{
  GList *plugins_format;
  GList *plugins_storage;
}
dt_imageio_t;


/* load all modules */
void dt_imageio_init   (dt_imageio_t *iio);
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

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
