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

#include <glib.h>
#include <stdint.h>

struct dt_imageio_module_storage_t;
struct dt_imageio_module_format_t;
struct dt_imageio_module_data_t;
enum dt_colorspaces_color_profile_type_t;
enum dt_iop_color_intent_t;

/* early definition of modules to do type checking */

// !!! MUST BE KEPT IN SYNC WITH dt_imageio_module_storage_t defined in src/common/imageio_module.h !!!

#pragma GCC visibility push(default)

int version();
/* get translated module name */
const char *name(const struct dt_imageio_module_storage_t *self);
/* construct widget above */
void gui_init(struct dt_imageio_module_storage_t *self);
/* destroy resources */
void gui_cleanup(struct dt_imageio_module_storage_t *self);
/* reset options to defaults */
void gui_reset(struct dt_imageio_module_storage_t *self);
/* allow the module to initialize itself */
void init(struct dt_imageio_module_storage_t *self);
/* try and see if this format is supported? */
int supported(struct dt_imageio_module_storage_t *self, struct dt_imageio_module_format_t *format);
/* get storage max supported image dimension, return 0 if no dimension restrictions exists. */
int dimension(struct dt_imageio_module_storage_t *self, struct dt_imageio_module_data_t *data,
              uint32_t *width, uint32_t *height);
/* get storage recommended image dimension, return 0 if no recommendation exists. */
int recommended_dimension(struct dt_imageio_module_storage_t *self, struct dt_imageio_module_data_t *data,
                          uint32_t *width, uint32_t *height);

/* called once at the beginning (before exporting image), if implemented
   * can change the list of exported images (including a NULL list)
 */
int initialize_store(struct dt_imageio_module_storage_t *self, struct dt_imageio_module_data_t *data,
                     struct dt_imageio_module_format_t **format, struct dt_imageio_module_data_t **fdata,
                     GList **images, const gboolean high_quality, const gboolean upscale);
/* this actually does the work */
int store(struct dt_imageio_module_storage_t *self, struct dt_imageio_module_data_t *self_data,
          const int imgid, struct dt_imageio_module_format_t *format, struct dt_imageio_module_data_t *fdata,
          const int num, const int total, const gboolean high_quality, const gboolean upscale,
          enum dt_colorspaces_color_profile_type_t icc_type, const gchar *icc_filename,
          enum dt_iop_color_intent_t icc_intent);
/* called once at the end (after exporting all images), if implemented. */
void finalize_store(struct dt_imageio_module_storage_t *self, struct dt_imageio_module_data_t *data);

void *legacy_params(struct dt_imageio_module_storage_t *self, const void *const old_params,
                    const size_t old_params_size, const int old_version, const int new_version,
                    size_t *new_size);
size_t params_size(struct dt_imageio_module_storage_t *self);
void *get_params(struct dt_imageio_module_storage_t *self);
void free_params(struct dt_imageio_module_storage_t *self, struct dt_imageio_module_data_t *data);
int set_params(struct dt_imageio_module_storage_t *self, const void *params, const int size);

void export_dispatched(struct dt_imageio_module_storage_t *self);

#pragma GCC visibility pop

#ifdef __cplusplus
}
#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
