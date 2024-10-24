/*
    This file is part of darktable,
    Copyright (C) 2010-2024 darktable developers.

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

#include "common/opencl.h"
#include "common/imagebuf.h"
#include "common/iop_profile.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "gui/accelerators.h"
#include "develop/tiling.h"
#include "iop/iop_api.h"

DT_MODULE(3)

typedef enum dt_iop_overexposed_colorscheme_t
{
  DT_IOP_OVEREXPOSED_BLACKWHITE = 0,
  DT_IOP_OVEREXPOSED_REDBLUE = 1,
  DT_IOP_OVEREXPOSED_PURPLEGREEN = 2
} dt_iop_overexposed_colorscheme_t;

static const float DT_ALIGNED_ARRAY dt_iop_overexposed_colors[][2][4]
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

int default_group()
{
  return IOP_GROUP_BASIC | IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_HIDDEN | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_NO_HISTORY_STACK;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
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

void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, piece->module, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return; // image has been copied through to output and module's trouble flag has been updated

  dt_develop_t *dev = self->dev;

  const int ch = 4;

  float *restrict img_tmp = NULL;
  if(!dt_iop_alloc_image_buffers(self, roi_in, roi_out, ch, &img_tmp, 0))
  {
    dt_iop_copy_image_roi(ovoid, ivoid, ch, roi_in, roi_out);
    dt_control_log(_("module overexposed failed in buffer allocation"));
    return;
  }

  const float lower = exp2f(fminf(dev->overexposed.lower, -4.f));   // in EV
  const float upper = dev->overexposed.upper / 100.0f;              // in %

  const int colorscheme = dev->overexposed.colorscheme;
  const float *const upper_color = dt_iop_overexposed_colors[colorscheme][0];
  const float *const lower_color = dt_iop_overexposed_colors[colorscheme][1];

  const float *const restrict in = DT_IS_ALIGNED((const float *const restrict)ivoid);
  float *const restrict out = DT_IS_ALIGNED((float *const restrict)ovoid);

  const dt_iop_order_iccprofile_info_t *const current_profile = dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_histogram_profile_info(dev);

  // display mask using histogram profile as output
  // FIXME: the histogram already does this work -- use that data instead?
  if(current_profile && work_profile)
    dt_ioppr_transform_image_colorspace_rgb(in, img_tmp, roi_out->width, roi_out->height, current_profile,
                                            work_profile, self->op);
  else
  {
    dt_print(DT_DEBUG_ALWAYS, "[overexposed process] can't create transform profile");
    dt_iop_copy_image_roi(ovoid, ivoid, ch, roi_in, roi_out);
    dt_control_log(_("module overexposed failed in color conversion"));
    goto process_finish;
  }
  // flush denormals to zero to avoid performance penalty if there are a lot of near-zero values
  const unsigned int oldMode = dt_mm_enable_flush_zero();

  if(dev->overexposed.mode == DT_CLIPPING_PREVIEW_ANYRGB)
  {
    // Any of the RGB channels is out of bounds
    DT_OMP_FOR()
    for(size_t k = 0; k < (size_t)ch * roi_out->width * roi_out->height; k += ch)
    {
      if(img_tmp[k + 0] >= upper || img_tmp[k + 1] >= upper || img_tmp[k + 2] >= upper)
      {
        copy_pixel(out + k, upper_color);
      }
      else if(img_tmp[k + 0] <= lower && img_tmp[k + 1] <= lower && img_tmp[k + 2] <= lower)
      {
        copy_pixel(out + k, lower_color);
      }
      else
      {
        copy_pixel(out + k, in + k);
      }
    }
  }

  else if(dev->overexposed.mode == DT_CLIPPING_PREVIEW_GAMUT && work_profile)
  {
    // Gamut is out of bounds
    DT_OMP_FOR()
    for(size_t k = 0; k < (size_t)ch * roi_out->width * roi_out->height; k += ch)
    {
      const float luminance = dt_ioppr_get_rgb_matrix_luminance(img_tmp + k,
                                                                work_profile->matrix_in, work_profile->lut_in,
                                                                work_profile->unbounded_coeffs_in,
                                                                work_profile->lutsize, work_profile->nonlinearlut);

      // luminance is out of bounds
      if(luminance >= upper)
      {
        copy_pixel(out + k, upper_color);
      }
      else if(luminance <= lower)
      {
        copy_pixel(out + k, lower_color);
      }
      // luminance is ok, so check for saturation
      else
      {
        dt_aligned_pixel_t saturation = { 0.f };

        for_each_channel(c,aligned(saturation, img_tmp : 64))
        {
          saturation[c] = (img_tmp[k + c] - luminance);
          saturation[c] = sqrtf(saturation[c] * saturation[c] / (luminance * luminance + img_tmp[k + c] * img_tmp[k + c]));
        }

        // we got over-saturation, relatively to luminance or absolutely over RGB
        if(saturation[0] > upper || saturation[1] > upper || saturation[2] > upper ||
          img_tmp[k + 0] >= upper || img_tmp[k + 1] >= upper || img_tmp[k + 2] >= upper)
        {
          copy_pixel(out + k, upper_color);
        }

        // saturation is fine but we got out-of-bounds RGB
        else if(img_tmp[k + 0] <= lower && img_tmp[k + 1] <= lower && img_tmp[k + 2] <= lower)
        {
          copy_pixel(out + k, lower_color);
        }

        // evererything is fine
        else
        {
          copy_pixel(out + k, in + k);
        }
      }
    }
  }

  else if(dev->overexposed.mode == DT_CLIPPING_PREVIEW_LUMINANCE && work_profile)
  {
    // Luminance channel is out of bounds
    DT_OMP_FOR()
    for(size_t k = 0; k < (size_t)ch * roi_out->width * roi_out->height; k += ch)
    {
      const float luminance = dt_ioppr_get_rgb_matrix_luminance(img_tmp + k,
                                                                work_profile->matrix_in, work_profile->lut_in,
                                                                work_profile->unbounded_coeffs_in,
                                                                work_profile->lutsize, work_profile->nonlinearlut);

      if(luminance >= upper)
      {
        copy_pixel(out + k, upper_color);
      }

      else if(luminance <= lower)
      {
        copy_pixel(out + k, lower_color);
      }
      else
      {
        copy_pixel(out + k, in + k);
      }
    }
  }

  else if(dev->overexposed.mode == DT_CLIPPING_PREVIEW_SATURATION && work_profile)
  {
    // Show saturation out of bounds where luminance is valid
    DT_OMP_FOR()
    for(size_t k = 0; k < (size_t)ch * roi_out->width * roi_out->height; k += ch)
    {
      const float luminance = dt_ioppr_get_rgb_matrix_luminance(img_tmp + k,
                                                                work_profile->matrix_in, work_profile->lut_in,
                                                                work_profile->unbounded_coeffs_in,
                                                                work_profile->lutsize, work_profile->nonlinearlut);
      if(luminance < upper && luminance > lower)
      {
        dt_aligned_pixel_t saturation = { 0.f };

        for_each_channel(c,aligned(saturation, img_tmp : 64))
        {
          saturation[c] = (img_tmp[k + c] - luminance);
          saturation[c] = sqrtf(saturation[c] * saturation[c] / (luminance * luminance + img_tmp[k + c] * img_tmp[k + c]));
        }

        // we got over-saturation, relatively to luminance or absolutely over RGB
        if(saturation[0] > upper || saturation[1] > upper || saturation[2] > upper ||
           img_tmp[k + 0] >= upper || img_tmp[k + 1] >= upper || img_tmp[k + 2] >= upper)
        {
          copy_pixel(out + k, upper_color);
        }
        else if(img_tmp[k + 0] <= lower && img_tmp[k + 1] <= lower && img_tmp[k + 2] <= lower)
        {
          copy_pixel(out + k, lower_color);
        }
        else
        {
          copy_pixel(out + k, in + k);
        }
      }

      else
      {
        copy_pixel(out + k, in + k);
      }
    }
  }

  dt_mm_restore_flush_zero(oldMode);

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
    dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);

process_finish:
  dt_free_align(img_tmp);
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_develop_t *dev = self->dev;
  dt_iop_overexposed_global_data_t *gd = self->global_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  const int devid = piece->pipe->devid;

  const int ch = piece->colors;
  cl_mem dev_tmp = NULL;

  const int width = roi_out->width;
  const int height = roi_out->height;

  const dt_iop_order_iccprofile_info_t *const current_profile = dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_histogram_profile_info(dev);

  // display mask using histogram profile as output
  dev_tmp = dt_opencl_alloc_device(devid, width, height, sizeof(float) * ch);
  if(dev_tmp == NULL)
  {
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    dt_control_log(_("module overexposed failed in buffer allocation"));
    goto error;
  }

  if(current_profile && work_profile)
    dt_ioppr_transform_image_colorspace_rgb_cl(devid, dev_in, dev_tmp, roi_out->width, roi_out->height,
                                               current_profile, work_profile, self->op);
  else
  {
    dt_print(DT_DEBUG_ALWAYS, "[overexposed process_cl] can't create transform profile");
    dt_control_log(_("module overexposed failed in color conversion"));
    goto error;
  }

  const int use_work_profile = (work_profile == NULL) ? 0 : 1;
  cl_mem dev_profile_info = NULL;
  cl_mem dev_profile_lut = NULL;
  dt_colorspaces_iccprofile_info_cl_t *profile_info_cl;
  cl_float *profile_lut_cl = NULL;

  err = dt_ioppr_build_iccprofile_params_cl(work_profile, devid, &profile_info_cl, &profile_lut_cl,
                                            &dev_profile_info, &dev_profile_lut);
  if(err != CL_SUCCESS) goto error;

  const float lower = exp2f(fminf(dev->overexposed.lower, -4.f));   // in EV
  const float upper = dev->overexposed.upper / 100.0f;              // in %
  const int colorscheme = dev->overexposed.colorscheme;

  const float *upper_color = dt_iop_overexposed_colors[colorscheme][0];
  const float *lower_color = dt_iop_overexposed_colors[colorscheme][1];
  const int mode = dev->overexposed.mode;

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_overexposed, width, height,
    CLARG(dev_in), CLARG(dev_out), CLARG(dev_tmp), CLARG(width), CLARG(height), CLARG(lower), CLARG(upper),
    CLARRAY(4, lower_color), CLARRAY(4, upper_color),
    CLARG(dev_profile_info), CLARG(dev_profile_lut), CLARG(use_work_profile), CLARG(mode));

error:
  dt_opencl_release_mem_object(dev_tmp);
  return err;
}
#endif

void tiling_callback(dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     dt_develop_tiling_t *tiling)
{
  tiling->factor = 3.0f;  // in + out + temp
  tiling->factor_cl = 3.0f;
  tiling->maxbuf = 1.0f;
  tiling->maxbuf_cl = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 0;
  tiling->xalign = 1;
  tiling->yalign = 1;
}


void init_global(dt_iop_module_so_t *self)
{
  const int program = 2; // basic.cl from programs.conf
  dt_iop_overexposed_global_data_t *gd = malloc(sizeof(dt_iop_overexposed_global_data_t));
  self->data = gd;
  gd->kernel_overexposed = dt_opencl_create_kernel(program, "overexposed");
}


void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_overexposed_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_overexposed);
  free(self->data);
  self->data = NULL;
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const gboolean fullpipe = piece->pipe->type & DT_DEV_PIXELPIPE_FULL;
  piece->enabled = self->dev->overexposed.enabled && fullpipe && self->dev->gui_attached;
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = NULL;
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
}

void init(dt_iop_module_t *self)
{
  self->params = calloc(1, sizeof(dt_iop_overexposed_t));
  self->default_params = calloc(1, sizeof(dt_iop_overexposed_t));
  self->hide_enable_button = TRUE;
  self->default_enabled = TRUE;
  self->params_size = sizeof(dt_iop_overexposed_t);
  self->gui_data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
