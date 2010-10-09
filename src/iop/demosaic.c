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
#include "develop/imageop.h"
#include <memory.h>
#include <stdlib.h>

DT_MODULE(1)

typedef struct dt_iop_demosaic_params_t
{
  // TODO: hot pixels removal/denoise/green eq/whatever
  int32_t flags;
}
dt_iop_demosaic_params_t;

typedef struct dt_iop_demosaic_gui_data_t
{
}
dt_iop_demosaic_gui_data_t;

typedef struct dt_iop_demosaic_data_t
{
  // demosaic pattern
  uint32_t filters;
}
dt_iop_demosaic_data_t;

const char *
name()
{
  return _("demosaic");
}

int 
groups ()
{
  return IOP_GROUP_BASIC;
}

void
process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  // global scale is roi scale and pipe input prescale
  // const float global_scale = roi_in->scale / piece->iscale;
  // const uint16_t *in = (const uint16_t *)i;
  // float *out = (float *)o;

  // TODO: decide on scale, whether to demosaic image or use half_size downsample
  // TODO: if demosaic, decide if we need scaling or whether 1:1 is good
  // TODO: convert float4 to float3 buf :( (can this be done in opencl actually?
  memcpy(o, i, sizeof(float)*3*roi_in->width*roi_in->height);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_demosaic_params_t));
  module->default_params = malloc(sizeof(dt_iop_demosaic_params_t));
  if(dt_image_is_ldr(module->dev->image)) module->default_enabled = 0;
  else                                    module->default_enabled = 1;
  module->priority = 0;
  module->hide_enable_button = 1;
  module->params_size = sizeof(dt_iop_demosaic_params_t);
  module->gui_data = NULL;
  dt_iop_demosaic_params_t tmp = (dt_iop_demosaic_params_t){0};
  memcpy(module->params, &tmp, sizeof(dt_iop_demosaic_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_demosaic_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
  free(module->data);
  module->data = NULL;
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)params;
  dt_iop_demosaic_data_t *d = (dt_iop_demosaic_data_t *)piece->data;
  d->filters = self->dev->image->filters;
}

void init_pipe     (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_demosaic_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
}

void gui_update   (struct dt_iop_module_t *self)
{
  // nothing
}

void gui_init     (struct dt_iop_module_t *self)
{
  self->widget = gtk_label_new(_("this module doesn't have any options"));
}

void gui_cleanup  (struct dt_iop_module_t *self)
{
  // nothing
}

