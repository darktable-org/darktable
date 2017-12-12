/*
    This file is part of darktable,
    copyright (c) 2013 jeremy Rosen
    copyright (c) 2016 Roman Lebedev.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

struct dt_imageio_module_format_t;
struct dt_imageio_module_data_t;

#include "common/colorspaces.h" // because forward declaring enums doesn't work in C++ :(

/* early definition of modules to do type checking */

// !!! MUST BE KEPT IN SYNC WITH dt_imageio_module_format_t defined in src/common/imageio_module.h !!!

#pragma GCC visibility push(default)

// gui and management:
/** version */
int version();
/* get translated module name */
const char *name();
/* construct widget above */
void gui_init(struct dt_imageio_module_format_t *self);
/* destroy resources */
void gui_cleanup(struct dt_imageio_module_format_t *self);
/* reset options to defaults */
void gui_reset(struct dt_imageio_module_format_t *self);

/* construct widget above */
void init(struct dt_imageio_module_format_t *self);
/* construct widget above */
void cleanup(struct dt_imageio_module_format_t *self);

/* gets the current export parameters from gui/conf and stores in this struct for later use. */
void *legacy_params(struct dt_imageio_module_format_t *self, const void *const old_params,
                    const size_t old_params_size, const int old_version, const int new_version,
                    size_t *new_size);
size_t params_size(struct dt_imageio_module_format_t *self);
void *get_params(struct dt_imageio_module_format_t *self);
void free_params(struct dt_imageio_module_format_t *self, struct dt_imageio_module_data_t *data);
/* resets the gui to the parameters as given here. return != 0 on fail. */
int set_params(struct dt_imageio_module_format_t *self, const void *params, const int size);

/* returns the mime type of the exported image. */
const char *mime(struct dt_imageio_module_data_t *data);
/* this extension (plus dot) is appended to the exported filename. */
const char *extension(struct dt_imageio_module_data_t *data);
/* get storage max supported image dimension, return 0 if no dimension restrictions exists. */
int dimension(struct dt_imageio_module_format_t *self, struct dt_imageio_module_data_t *data, uint32_t *width,
              uint32_t *height);

// writing functions:
/* bits per pixel and color channel we want to write: 8: char x3, 16: uint16_t x3, 32: float x3. */
int bpp(struct dt_imageio_module_data_t *data);
/* write to file, with exif if not NULL, and icc profile if supported. */
int write_image(struct dt_imageio_module_data_t *data, const char *filename, const void *in,
                dt_colorspaces_color_profile_type_t over_type, const char *over_filename,
                void *exif, int exif_len, int imgid, int num, int total);
/* flag that describes the available precision/levels of output format. mainly used for dithering. */
int levels(struct dt_imageio_module_data_t *data);

// sometimes we want to tell the world about what we can do
int flags(struct dt_imageio_module_data_t *data);

int read_image(struct dt_imageio_module_data_t *data, uint8_t *out);

#pragma GCC visibility pop

#ifdef __cplusplus
}
#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
