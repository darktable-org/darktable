/*
    This file is part of darktable,
    Copyright (C) 2025 darktable developers.

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
#include "bauhaus/bauhaus.h"
#include "common/interpolation.h"
#include "common/opencl.h"
#include "common/imagebuf.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/tiling.h"
#include "iop/iop_api.h"

DT_MODULE_INTROSPECTION(1, dt_iop_pipescale_params_t)

typedef struct dt_iop_pipescale_params_t
{
  int dummy;
} dt_iop_pipescale_params_t;

typedef dt_iop_pipescale_params_t dt_iop_pipescale_data_t;

typedef struct dt_iop_pipescale_gui_data_t
{
  int dummy;
} dt_iop_pipescale_gui_data_t;

const char *name()
{
  return _("pipe scale");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("crop and scale sensor data to current region of interest"),
                                      _("mandatory"),
                                      _("linear, RGB, scene-referred"),
                                      _("linear, RGB"),
                                      _("linear, RGB, scene-referred"));
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_TILING_FULL_ROI
    | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_NO_HISTORY_STACK;
}

int default_group()
{
  return IOP_GROUP_BASIC;
}


dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

void modify_roi_out(dt_iop_module_t *self,
                    dt_dev_pixelpipe_iop_t *piece,
                    dt_iop_roi_t *roi_out,
                    const dt_iop_roi_t *const roi_in)
{
  *roi_out = *roi_in;
  roi_out->x = 0;
  roi_out->y = 0;
}

void modify_roi_in(dt_iop_module_t *self,
                   dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *const roi_out,
                   dt_iop_roi_t *roi_in)
{
  *roi_in = *roi_out;
  roi_in->scale = 1.0f;
  roi_in->x = 0;
  roi_in->y = 0;
  roi_in->width = piece->buf_in.width;
  roi_in->height = piece->buf_in.height;
}

void tiling_callback(dt_iop_module_t *self,
                     dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in,
                     const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  const float ioratio
      = (float)(roi_out->width * roi_out->height) / (float)(roi_in->width * roi_in->height);

  tiling->factor = 1.0f + ioratio;
  tiling->factor += ioratio != 1.0f ? 0.5f : 0.0f; // approximate extra requirements for interpolation
  tiling->factor_cl = tiling->factor;
  tiling->maxbuf = 1.0f;
  tiling->maxbuf_cl = tiling->maxbuf;
  tiling->overhead = 0;

  tiling->overlap = 4;
  tiling->xalign = 1;
  tiling->yalign = 1;
}

void distort_mask(dt_iop_module_t *self,
                  dt_dev_pixelpipe_iop_t *piece,
                  const float *const in,
                  float *const out,
                  const dt_iop_roi_t *const roi_in,
                  const dt_iop_roi_t *const roi_out)
{
  if(roi_out->scale != roi_in->scale)
  {
    const dt_interpolation_t *itor = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);
    dt_interpolation_resample_1c(itor, out, roi_out, in, roi_in);
  }
  else
    dt_iop_copy_image_roi(out, in, 1, roi_in, roi_out);
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi,
               const dt_iop_roi_t *const roo)
{
  const int devid = piece->pipe->devid;
  if(roo->width == roi->width && roo->height == roi->height && roi->scale == roo->scale)
  {
    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { roo->width, roo->height, 1 };
    return dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, region);
  }
  else
    return dt_iop_clip_and_zoom_cl(devid, dev_out, dev_in, roo, roi);
}
#endif

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi,
             const dt_iop_roi_t *const roo)
{
  if(roo->width == roi->width && roo->height == roi->height && roi->scale == roo->scale)
    dt_iop_copy_image_roi((float *)ovoid, (float *)ivoid, 4, roi, roo);
  else
    dt_iop_clip_and_zoom((float *)ovoid, (float *)ivoid, roo, roi);
}

void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_pipescale_data_t));
}

void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void init(dt_iop_module_t *self)
{
  self->params = calloc(1, sizeof(dt_iop_pipescale_params_t));
  self->default_params = calloc(1, sizeof(dt_iop_pipescale_params_t));
  self->default_enabled = TRUE;
  self->hide_enable_button = TRUE;
  self->params_size = sizeof(dt_iop_pipescale_params_t);
}

void gui_init(dt_iop_module_t *self)
{
  IOP_GUI_ALLOC(pipescale);
  self->widget = dt_ui_label_new("");
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
