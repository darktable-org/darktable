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
#include <xmmintrin.h>
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
} dt_iop_overexposed_colorscheme_t;

static const float dt_iop_overexposed_colors[][2][4]
    = { {
          { 0.0f, 0.0f, 0.0f, 1.0f }, // black
          { 1.0f, 1.0f, 1.0f, 1.0f }  // white
        },
        {
          { 1.0f, 0.0f, 0.0f, 1.0f }, // red
          { 0.0f, 0.0f, 1.0f, 1.0f }  // blue
        },
        {
          { 0.371f, 0.434f, 0.934f, 1.0f }, // purple (#5f6fef)
          { 0.512f, 0.934f, 0.371f, 1.0f }  // green  (#83ef5f)
        } };

typedef struct dt_iop_overexposed_global_data_t
{
  int kernel_overexposed;
} dt_iop_overexposed_global_data_t;

typedef struct dt_iop_overexposed_t
{
  int dummy;
} dt_iop_overexposed_t;

const char *name()
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


int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
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

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_develop_t *dev = self->dev;

  const int ch = piece->colors;

  const __m128 upper = _mm_set_ps(FLT_MAX, dev->overexposed.upper / 100.0f, dev->overexposed.upper / 100.0f,
                                  dev->overexposed.upper / 100.0f);
  const __m128 lower = _mm_set_ps(FLT_MAX, dev->overexposed.lower / 100.0f, dev->overexposed.lower / 100.0f,
                                  dev->overexposed.lower / 100.0f);

  const int colorscheme = dev->overexposed.colorscheme;
  const __m128 upper_color = _mm_load_ps(dt_iop_overexposed_colors[colorscheme][0]);
  const __m128 lower_color = _mm_load_ps(dt_iop_overexposed_colors[colorscheme][1]);

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(ovoid) schedule(static)
#endif
  for(int k = 0; k < roi_out->height; k++)
  {
    const float *in = ((float *)ivoid) + (size_t)ch * k * roi_out->width;
    float *out = ((float *)ovoid) + (size_t)ch * k * roi_out->width;

    for(int j = 0; j < roi_out->width; j++, in += 4, out += 4)
    {
      const __m128 pixel = _mm_load_ps(in);

      __m128 isoe = _mm_cmpge_ps(pixel, upper);
      isoe = _mm_or_ps(_mm_unpacklo_ps(isoe, isoe), _mm_unpackhi_ps(isoe, isoe));
      isoe = _mm_or_ps(_mm_unpacklo_ps(isoe, isoe), _mm_unpackhi_ps(isoe, isoe));

      __m128 isue = _mm_cmple_ps(pixel, lower);
      isue = _mm_and_ps(_mm_unpacklo_ps(isue, isue), _mm_unpackhi_ps(isue, isue));
      isue = _mm_and_ps(_mm_unpacklo_ps(isue, isue), _mm_unpackhi_ps(isue, isue));

      __m128 result = _mm_or_ps(_mm_andnot_ps(isoe, pixel), _mm_and_ps(isoe, upper_color));

      result = _mm_or_ps(_mm_andnot_ps(isue, result), _mm_and_ps(isue, lower_color));

      _mm_stream_ps(out, result);
    }
  }
  _mm_sfence();

  if(piece->pipe->mask_display) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_develop_t *dev = self->dev;
  dt_iop_overexposed_global_data_t *gd = (dt_iop_overexposed_global_data_t *)self->data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;

  const int width = roi_out->width;
  const int height = roi_out->height;

  const float lower = dev->overexposed.lower / 100.0f;
  const float upper = dev->overexposed.upper / 100.0f;
  const int colorscheme = dev->overexposed.colorscheme;

  const float *upper_color = dt_iop_overexposed_colors[colorscheme][0];
  const float *lower_color = dt_iop_overexposed_colors[colorscheme][1];

  size_t sizes[2] = { ROUNDUPWD(width), ROUNDUPHT(height) };
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 0, sizeof(cl_mem), &dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 1, sizeof(cl_mem), &dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 2, sizeof(int), &width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 3, sizeof(int), &height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 4, sizeof(float), &lower);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 5, sizeof(float), &upper);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 6, 4 * sizeof(float), lower_color);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 7, 4 * sizeof(float), upper_color);
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
  dt_iop_overexposed_global_data_t *gd
      = (dt_iop_overexposed_global_data_t *)malloc(sizeof(dt_iop_overexposed_global_data_t));
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

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  if(pipe->type != DT_DEV_PIXELPIPE_FULL || !self->dev->overexposed.enabled || !self->dev->gui_attached)
    piece->enabled = 0;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = NULL;
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_overexposed_t));
  module->default_params = malloc(sizeof(dt_iop_overexposed_t));
  module->hide_enable_button = 1;
  module->default_enabled = 1;
  module->priority = 933; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_overexposed_t);
  module->gui_data = NULL;
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
