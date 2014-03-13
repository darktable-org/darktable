/*
    This file is part of darktable,
    copyright (c) 2010-2013 Tobias Ellinghaus.
    copyright (c) 2011-2012 henrik andersson.

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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <cairo.h>
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "common/opencl.h"
#include "gui/accelerators.h"

DT_MODULE(3)

typedef enum dt_iop_overexposed_colorscheme_t
{
  DT_IOP_OVEREXPOSED_BLACKWHITE = 0,
  DT_IOP_OVEREXPOSED_REDBLUE = 1,
  DT_IOP_OVEREXPOSED_PURPLEGREEN = 2
}
dt_iop_overexposed_colorscheme_t;

static const float dt_iop_overexposed_colors[][2][4] =
{
  {
    { 0.0f, 0.0f, 0.0f, 1.0f },    // black
    { 1.0f, 1.0f, 1.0f, 1.0f }     // white
  },
  {
    { 1.0f, 0.0f, 0.0f, 1.0f },    // red
    { 0.0f, 0.0f, 1.0f, 1.0f }     // blue
  },
  {
    { 0.371f, 0.434f, 0.934f, 1.0f }, // purple (#5f6fef)
    { 0.512f, 0.934f, 0.371f, 1.0f }  // green  (#83ef5f)
  }
};

typedef struct dt_iop_overexposed_global_data_t
{
  int kernel_overexposed;
}
dt_iop_overexposed_global_data_t;

typedef struct dt_iop_overexposed_t
{
  int dummy;
}
dt_iop_overexposed_t;

const char* name()
{
  return _("overexposed");
}

int groups()
{
  return IOP_GROUP_BASIC;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_HIDDEN | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_NO_HISTORY_STACK;
}


int
legacy_params (dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params, const int new_version)
{
  // we do no longer have module params in here and just ignore any legacy entries
  return 0;
}


// void init_key_accels(dt_iop_module_so_t *self)
// {
//   dt_accel_register_slider_iop(self, FALSE, NC_("accel", "lower threshold"));
//   dt_accel_register_slider_iop(self, FALSE, NC_("accel", "upper threshold"));
//   dt_accel_register_slider_iop(self, FALSE, NC_("accel", "color scheme"));
// }
//
// void connect_key_accels(dt_iop_module_t *self)
// {
//   dt_iop_overexposed_gui_data_t *g =
//     (dt_iop_overexposed_gui_data_t*)self->gui_data;
//
//   dt_accel_connect_slider_iop(self, "lower threshold", GTK_WIDGET(g->lower));
//   dt_accel_connect_slider_iop(self, "upper threshold", GTK_WIDGET(g->upper));
//   dt_accel_connect_slider_iop(self, "color scheme", GTK_WIDGET(g->colorscheme));
// }

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_develop_t *dev = self->dev;

  const int ch = piece->colors;

  // FIXME: turn off the module instead?
  if(!dev->overexposed.enabled || !dev->gui_attached)
  {
    memcpy(o, i, (size_t)roi_out->width*roi_out->height*sizeof(float)*ch);
    return;
  }

  const float lower  = dev->overexposed.lower / 100.0;
  const float upper  = dev->overexposed.upper / 100.0;
  const int colorscheme = dev->overexposed.colorscheme;

  const float *upper_color  = dt_iop_overexposed_colors[colorscheme][0];
  const float *lower_color = dt_iop_overexposed_colors[colorscheme][1];

  float *in    = (float *)i;
  float *out   = (float *)o;

#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_out, in, out, upper_color, lower_color) schedule(static)
#endif
  for(size_t k=0; k<(size_t)roi_out->width*roi_out->height; k++)
  {
    float *inp = in + ch*k;
    float *outp = out + ch*k;
    if(inp[0] >= upper || inp[1] >= upper || inp[2] >= upper)
    {
      outp[0] = upper_color[0];
      outp[1] = upper_color[1];
      outp[2] = upper_color[2];
    }
    else if(inp[0] <= lower && inp[1] <= lower && inp[2] <= lower)
    {
      outp[0] = lower_color[0];
      outp[1] = lower_color[1];
      outp[2] = lower_color[2];
    }
    else
    {
      // TODO: memcpy()?
      outp[0] = inp[0];
      outp[1] = inp[1];
      outp[2] = inp[2];
    }

    outp[3] = inp[3];
  }
}

#ifdef HAVE_OPENCL
int
process_cl (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_develop_t *dev = self->dev;
  dt_iop_overexposed_global_data_t *gd = (dt_iop_overexposed_global_data_t *)self->data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;

  const int width = roi_out->width;
  const int height = roi_out->height;

  const float lower  = dev->overexposed.lower / 100.0f;
  const float upper  = dev->overexposed.upper / 100.0f;
  const int colorscheme = dev->overexposed.colorscheme;

  const float *upper_color  = dt_iop_overexposed_colors[colorscheme][0];
  const float *lower_color = dt_iop_overexposed_colors[colorscheme][1];

  if(!dev->overexposed.enabled || !dev->gui_attached)
  {
    size_t origin[] = { 0, 0, 0};
    size_t region[] = { width, height, 1};
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, region);
    if (err != CL_SUCCESS) goto error;
    return TRUE;
  }

  size_t sizes[2] = { ROUNDUPWD(width), ROUNDUPHT(height) };
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 0, sizeof(cl_mem), &dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 1, sizeof(cl_mem), &dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 2, sizeof(int), &width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 3, sizeof(int), &height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 4, sizeof(float), &lower);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 5, sizeof(float), &upper);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 6, 4*sizeof(float), lower_color);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 7, 4*sizeof(float), upper_color);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_overexposed, sizes);
  if(err != CL_SUCCESS) goto error;
  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_overexposed] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif


void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl from programs.conf
  dt_iop_overexposed_global_data_t *gd = (dt_iop_overexposed_global_data_t *)malloc(sizeof(dt_iop_overexposed_global_data_t));
  module->data = gd;
  gd->kernel_overexposed = dt_opencl_create_kernel(program, "overexposed");
}


void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_overexposed_global_data_t *gd = (dt_iop_overexposed_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_overexposed);
  free(module->data);
  module->data = NULL;
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  if(pipe->type != DT_DEV_PIXELPIPE_FULL) piece->enabled = 0;
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = NULL;
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
}

void init(dt_iop_module_t *module)
{
  module->params                  = malloc(sizeof(dt_iop_overexposed_t));
  module->default_params          = malloc(sizeof(dt_iop_overexposed_t));
  module->hide_enable_button      = 1;
  module->default_enabled         = 1;
  module->priority = 929; // module order created by iop_dependencies.py, do not edit!
  module->params_size             = sizeof(dt_iop_overexposed_t);
  module->gui_data                = NULL;
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
