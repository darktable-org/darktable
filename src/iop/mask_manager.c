/*
    This file is part of darktable,
    Copyright (C) 2018-2020 darktable developers.

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

/*
 * This is a dummy module intended only to be used in history so hist->module is not NULL
 * when the entry correspond to the mask manager
 *
 * It is always disabled and do not show in module list, only in history
 *
 * We start at version 2 so previous version of dt can add records in history with NULL params
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common/imagebuf.h"
#include "develop/develop.h"

DT_MODULE_INTROSPECTION(2, dt_iop_mask_manager_params_t)

typedef struct dt_iop_mask_manager_params_t
{
  int dummy;
} dt_iop_mask_manager_params_t;

typedef struct dt_iop_mask_manager_params_t dt_iop_mask_manager_data_t;

const char *name()
{
  return _("Mask manager");
}

int groups()
{
  return IOP_GROUP_BASIC | IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_HIDDEN | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_UNSAFE_COPY;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 2)
  {
    dt_iop_mask_manager_params_t *n = (dt_iop_mask_manager_params_t *)new_params;
    dt_iop_mask_manager_params_t *d = (dt_iop_mask_manager_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters
    return 0;
  }
  return 1;
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const i, void *const o,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const int ch = piece->colors;
  dt_iop_image_copy_by_size(o, i, roi_out->width, roi_out->height, ch);
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  size_t origin[] = { 0, 0, 0 };
  size_t region[] = { width, height, 1 };
  err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, region);
  if(err != CL_SUCCESS) goto error;

  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_mask_manage] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}
#endif

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  memcpy(piece->data, params, sizeof(dt_iop_mask_manager_params_t));
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_mask_manager_data_t));
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
