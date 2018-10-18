/*
    This file is part of darktable,
    copyright (c) 2010-2011 Jose Carlos Garcia Sogo

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
#include "imageio/storage/imageio_storage_api.h"

#define DT_PIWIGO_VERSION 0                 // module version
#define DT_PIWIGO_NAME    "Piwigo"          // module name
#define DT_PIWIGO_FAILED  0
#define DT_PIWIGO_SUCCESS 1
#define DT_PIWIGO_NO      DT_PIWIGO_FILED
#define DT_PIWIGO_YES     DT_PIWIGO_SUCCESS 
#define DT_PIWIGO_DIMENSION_MAX   0         // No maximum dimension
#define DT_PIWIGO_DIMENSION_BEST  0         // No recommended dimension

// module supports darktable release 2.x
DT_MODULE(2)

int version()
{
  return DT_PIWIGO_VERSION;
}

/* get translated module name */
const char *name(const struct dt_imageio_module_storage_t *self)
{
  return DT_PIWIGO_NAME;
}

/* construct widget above */
void gui_init(struct dt_imageio_module_storage_t *self)
{
  return;
}

/* destroy resources */
void gui_cleanup(struct dt_imageio_module_storage_t *self)
{
  return;
}
/* reset options to defaults */
void gui_reset(struct dt_imageio_module_storage_t *self)
{
  return;
}

/* allow the module to initialize itself */
void init(struct dt_imageio_module_storage_t *self)
{
  return;
}

/* try and see if this format is supported? */
int supported(struct dt_imageio_module_storage_t *self, struct dt_imageio_module_format_t *format)
{
  if ( false )
  {
    return DT_PIWIGO_YES;
  }
  return DT_PIWIGO_NO;
}

/* get storage max supported image dimension, return 0 if no dimension restrictions exists. */
int dimension(struct dt_imageio_module_storage_t *self, struct dt_imageio_module_data_t *data,
              uint32_t *width, uint32_t *height)
{
  return DT_PIWIGO_DIMENSION_MAX;
}
        
/* get storage recommended image dimension, return 0 if no recommendation exists. */
int recommended_dimension(struct dt_imageio_module_storage_t *self, struct dt_imageio_module_data_t *data,
                          uint32_t *width, uint32_t *height)
{
  return DT_PIWIGO_DIMENSION_BEST;
}

/* called once at the beginning (before exporting image), if implemented
   * can change the list of exported images (including a NULL list)
 */
int initialize_store(struct dt_imageio_module_storage_t *self, struct dt_imageio_module_data_t *data,
                     struct dt_imageio_module_format_t **format, struct dt_imageio_module_data_t **fdata,
                     GList **images, const gboolean high_quality, const gboolean upscale)
{
  return DT_PIWIGO_SUCCESS;
}

/* this actually does the work */
int store(struct dt_imageio_module_storage_t *self, struct dt_imageio_module_data_t *self_data,
          const int imgid, struct dt_imageio_module_format_t *format, struct dt_imageio_module_data_t *fdata,
          const int num, const int total, const gboolean high_quality, const gboolean upscale,
          enum dt_colorspaces_color_profile_type_t icc_type, const gchar *icc_filename,
          enum dt_iop_color_intent_t icc_intent)
{
  return DT_PIWIGO_FAILED;
}

/* called once at the end (after exporting all images), if implemented. */
void finalize_store(struct dt_imageio_module_storage_t *self, struct dt_imageio_module_data_t *data)
{
  return;
}

void *legacy_params(struct dt_imageio_module_storage_t *self, const void *const old_params,
                    const size_t old_params_size, const int old_version, const int new_version,
                    size_t *new_size)
{
  return;
}

size_t params_size(struct dt_imageio_module_storage_t *self)
{
  return 0;
}

void *get_params(struct dt_imageio_module_storage_t *self)
{
  return;
}

void free_params(struct dt_imageio_module_storage_t *self, struct dt_imageio_module_data_t *data)
{
  return;
}

int set_params(struct dt_imageio_module_storage_t *self, const void *params, const int size)
{
  return DT_PIWIGO_SUCCESS;
}


void export_dispatched(struct dt_imageio_module_storage_t *self)
{
  return;
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
