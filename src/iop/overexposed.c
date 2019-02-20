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
#if defined(__SSE__)
#include <xmmintrin.h>
#endif
#include <cairo.h>

#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "gui/accelerators.h"
#include "iop/iop_api.h"

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

int default_group()
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

static void _get_histogram_profile_type(dt_colorspaces_color_profile_type_t *out_type, gchar **out_filename)
{
  // if in gamut check use soft proof
  if(darktable.color_profiles->histogram_type == DT_COLORSPACE_SOFTPROOF)
  {
    *out_type = darktable.color_profiles->softproof_type;
    *out_filename = darktable.color_profiles->softproof_filename;
  }
  else if(darktable.color_profiles->histogram_type == DT_COLORSPACE_EXPORT)
  {
    // use introspection to get the params values
    dt_iop_module_so_t *colorout_so = NULL;
    dt_iop_module_t *colorout = NULL;
    GList *modules = g_list_last(darktable.iop);
    while(modules)
    {
      dt_iop_module_so_t *module_so = (dt_iop_module_so_t *)(modules->data);
      if(!strcmp(module_so->op, "colorout"))
      {
        colorout_so = module_so;
        break;
      }
      modules = g_list_previous(modules);
    }
    if(colorout_so && colorout_so->get_p)
    {
      modules = g_list_last(darktable.develop->iop);
      while(modules)
      {
        dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
        if(!strcmp(module->op, "colorout"))
        {
          colorout = module;
          break;
        }
        modules = g_list_previous(modules);
      }
    }
    if(colorout)
    {
      dt_colorspaces_color_profile_type_t *_type = colorout_so->get_p(colorout->params, "type");
      char *_filename = colorout_so->get_p(colorout->params, "filename");
      if(_type) *out_type = *_type;
      if(_filename) *out_filename = _filename;
    }
  }
  else
  {
    *out_type = darktable.color_profiles->histogram_type;
    *out_filename = darktable.color_profiles->histogram_filename;
  }
}

static void _transform_image_colorspace(const float *const img_in, float *const img_out, const dt_iop_roi_t *const roi_in)
{
  cmsHPROFILE display_profile = NULL;
  cmsHPROFILE histogram_profile = NULL;
  cmsHTRANSFORM xform_rgb2rgb = NULL;

  dt_colorspaces_color_profile_type_t histogram_type = DT_COLORSPACE_SRGB;
  gchar *histogram_filename = NULL;
  
  if(darktable.color_profiles->display_type == DT_COLORSPACE_DISPLAY || histogram_type == DT_COLORSPACE_DISPLAY)
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);

  _get_histogram_profile_type(&histogram_type, &histogram_filename);
  
  const dt_colorspaces_color_profile_t *d_profile = dt_colorspaces_get_profile(darktable.color_profiles->display_type,
                                                       darktable.color_profiles->display_filename,
                                                       DT_PROFILE_DIRECTION_OUT | DT_PROFILE_DIRECTION_DISPLAY);
  if(d_profile) display_profile = d_profile->profile;

  if((histogram_type != darktable.color_profiles->display_type) || 
      (histogram_type == DT_COLORSPACE_FILE &&
      strcmp(histogram_filename, darktable.color_profiles->display_filename)))
  {
    const dt_colorspaces_color_profile_t *d_histogram = dt_colorspaces_get_profile(histogram_type,
                                                         histogram_filename,
                                                         DT_PROFILE_DIRECTION_OUT | DT_PROFILE_DIRECTION_DISPLAY);
    if(d_histogram) histogram_profile = d_histogram->profile;
  }

  // display rgb --> histogram rgb
  if(display_profile && histogram_profile)
  {
    xform_rgb2rgb = cmsCreateTransform(display_profile, TYPE_RGBA_FLT, histogram_profile, TYPE_RGBA_FLT, INTENT_PERCEPTUAL, 0);
    if(xform_rgb2rgb == NULL)
      fprintf(stderr, "[overexposed _transform_image_colorspace] can't create colorspace transform\n");
  }

  if(darktable.color_profiles->display_type == DT_COLORSPACE_DISPLAY || histogram_type == DT_COLORSPACE_DISPLAY)
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);

  if(xform_rgb2rgb)
    cmsDoTransform(xform_rgb2rgb, img_in, img_out, roi_in->width * roi_in->height);
  
  if(xform_rgb2rgb) cmsDeleteTransform(xform_rgb2rgb);
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_develop_t *dev = self->dev;

  const int ch = piece->colors;

  float *const img_tmp = dt_alloc_align(64, ch * roi_out->width * roi_out->height * sizeof(float));
  if(img_tmp == NULL)
  {
    fprintf(stderr, "[overexposed process] can't alloc temp image\n");
    goto cleanup;
  }
  
  const float lower = MAX(dev->overexposed.lower / 100.0f, 1e-6f);
  const float upper = dev->overexposed.upper / 100.0f;

  const int colorscheme = dev->overexposed.colorscheme;
  const float *const upper_color = dt_iop_overexposed_colors[colorscheme][0];
  const float *const lower_color = dt_iop_overexposed_colors[colorscheme][1];

  const float *const in = (const float *const)ivoid;
  float *const out = (float *const)ovoid;

  // display mask using histogram profile as output
  _transform_image_colorspace(in, img_tmp, roi_out);

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(size_t k = 0; k < (size_t)ch * roi_out->width * roi_out->height; k += ch)
  {
    if(img_tmp[k + 0] >= upper || img_tmp[k + 1] >= upper || img_tmp[k + 2] >= upper)
    {
      for(int c = 0; c < 3; c++)
      {
        out[k + c] = upper_color[c];
      }
    }
    else if(img_tmp[k + 0] <= lower && img_tmp[k + 1] <= lower && img_tmp[k + 2] <= lower)
    {
      for(int c = 0; c < 3; c++)
      {
        out[k + c] = lower_color[c];
      }
    }
    else
    {
      for(int c = 0; c < 3; c++)
      {
        const size_t p = (size_t)k + c;
        out[p] = in[p];
      }
    }
  }

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);

cleanup:
  if(img_tmp) dt_free_align(img_tmp);
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_develop_t *dev = self->dev;
  dt_iop_overexposed_global_data_t *gd = (dt_iop_overexposed_global_data_t *)self->data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;

  const int ch = piece->colors;
  cl_mem dev_tmp = NULL;

  const int width = roi_out->width;
  const int height = roi_out->height;

  float *const img_tmp = dt_alloc_align(64, ch * width * height * sizeof(float));
  if(img_tmp == NULL)
  {
    fprintf(stderr, "[overexposed process_cl] can't alloc temp image\n");
    goto error;
  }
  
  // display mask using histogram profile as output
  dev_tmp = dt_opencl_alloc_device(devid, width, height, ch * sizeof(float));
  if(dev_tmp == NULL)
  {
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    fprintf(stderr, "[overexposed process_cl] error allocating memory for color transformation\n");
    goto error;
  }
  
  err = dt_opencl_copy_device_to_host(devid, img_tmp, dev_in, width, height, ch * sizeof(float));
  if(err != CL_SUCCESS)
  {
    fprintf(stderr, "[overexposed process_cl] error allocating memory for color transformation\n");
    goto error;
  }

  _transform_image_colorspace(img_tmp, img_tmp, roi_out);

  err = dt_opencl_write_host_to_device(devid, img_tmp, dev_tmp, width, height, ch * sizeof(float));
  if(err != CL_SUCCESS)
  {
    fprintf(stderr, "[dt_ioppr_transform_image_colorspace_cl] error allocating memory for color transformation 3\n");
    goto error;
  }

  const float lower = MAX(dev->overexposed.lower / 100.0f, 1e-6f);
  const float upper = dev->overexposed.upper / 100.0f;
  const int colorscheme = dev->overexposed.colorscheme;

  const float *upper_color = dt_iop_overexposed_colors[colorscheme][0];
  const float *lower_color = dt_iop_overexposed_colors[colorscheme][1];

  size_t sizes[2] = { ROUNDUPWD(width), ROUNDUPHT(height) };
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 0, sizeof(cl_mem), &dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 1, sizeof(cl_mem), &dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 2, sizeof(cl_mem), &dev_tmp);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 3, sizeof(int), &width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 4, sizeof(int), &height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 5, sizeof(float), &lower);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 6, sizeof(float), &upper);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 7, 4 * sizeof(float), lower_color);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 8, 4 * sizeof(float), upper_color);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_overexposed, sizes);
  if(err != CL_SUCCESS) goto error;
  if(img_tmp) dt_free_align(img_tmp);
  if(dev_tmp) dt_opencl_release_mem_object(dev_tmp);
  return TRUE;

error:
  if(img_tmp) dt_free_align(img_tmp);
  if(dev_tmp) dt_opencl_release_mem_object(dev_tmp);
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
  module->params = calloc(1, sizeof(dt_iop_overexposed_t));
  module->default_params = calloc(1, sizeof(dt_iop_overexposed_t));
  module->hide_enable_button = 1;
  module->default_enabled = 1;
  module->priority = 928; // module order created by iop_dependencies.py, do not edit!
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
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
