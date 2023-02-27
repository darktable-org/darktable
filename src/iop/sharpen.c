/*
    This file is part of darktable,
    Copyright (C) 2009-2020 darktable developers.

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
#include "bauhaus/bauhaus.h"
#include "common/imagebuf.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>
#include <inttypes.h>

DT_MODULE_INTROSPECTION(1, dt_iop_sharpen_params_t)

#define MAXR 12

typedef struct dt_iop_sharpen_params_t
{
  float radius;    // $MIN: 0.0 $MAX: 99.0 $DEFAULT: 2.0
  float amount;    // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 0.5
  float threshold; // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 0.5
} dt_iop_sharpen_params_t;

typedef struct dt_iop_sharpen_gui_data_t
{
  GtkWidget *radius, *amount, *threshold;
} dt_iop_sharpen_gui_data_t;

typedef struct dt_iop_sharpen_data_t
{
  float radius, amount, threshold;
} dt_iop_sharpen_data_t;

typedef struct dt_iop_sharpen_global_data_t
{
  int kernel_sharpen_hblur;
  int kernel_sharpen_vblur;
  int kernel_sharpen_mix;
} dt_iop_sharpen_global_data_t;


const char *name()
{
  return C_("modulename", "sharpen");
}

int default_group()
{
  return IOP_GROUP_CORRECT | IOP_GROUP_EFFECTS;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("sharpen the details in the image using a standard UnSharp Mask (USM)"),
                                      _("corrective"),
                                      _("linear or non-linear, Lab, display or scene-referred"),
                                      _("frequential, Lab"),
                                      _("quasi-linear, Lab, display or scene-referred"));
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_sharpen_params_t tmp = (dt_iop_sharpen_params_t){ 2.0, 0.5, 0.5 };
  // add the preset.
  dt_gui_presets_add_generic(_("sharpen"), self->op,
                             self->version(), &tmp, sizeof(dt_iop_sharpen_params_t),
                             1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  // restrict to raw images
  dt_gui_presets_update_ldr(_("sharpen"), self->op,
                            self->version(), FOR_RAW);
}

static float *const init_gaussian_kernel(const int rad, const size_t mat_size, const float sigma2)
{
  float *const mat = dt_calloc_align_float(mat_size);
  if(mat)
  {
    float weight = 0.0f;
    for(int l = -rad; l <= rad; l++)
      weight += mat[l + rad] = expf(-l * l / (2.f * sigma2));
    for(int l = -rad; l <= rad; l++)
      mat[l + rad] /= weight;
  }
  return mat;
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_sharpen_data_t *d = (dt_iop_sharpen_data_t *)piece->data;
  dt_iop_sharpen_global_data_t *gd = (dt_iop_sharpen_global_data_t *)self->global_data;
  cl_mem dev_m = NULL;
  cl_mem dev_tmp = NULL;
  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const int rad = MIN(MAXR, ceilf(d->radius * roi_in->scale / piece->iscale));
  const int wd = 2 * rad + 1;
  float *mat = NULL;

  if(rad == 0)
  {
    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { width, height, 1 };
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, region);
    if(err != CL_SUCCESS) goto error;
    return TRUE;
  }

  // special case handling: very small image with one or two dimensions below 2*rad+1 => no sharpening,
  // normally not needed for OpenCL but implemented here for identity with CPU code path
  if(width < 2 * rad + 1 || height < 2 * rad + 1)
  {
    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { width, height, 1 };
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, region);
    if(err != CL_SUCCESS) goto error;
    return TRUE;
  }

  const float sigma2 = (1.0f / (2.5 * 2.5)) * (d->radius * roi_in->scale / piece->iscale)
                       * (d->radius * roi_in->scale / piece->iscale);
  mat = init_gaussian_kernel(rad, wd, sigma2);
  if(!mat)
    goto error;

  int hblocksize;
  dt_opencl_local_buffer_t hlocopt
    = (dt_opencl_local_buffer_t){ .xoffset = 2 * rad, .xfactor = 1, .yoffset = 0, .yfactor = 1,
                                  .cellsize = sizeof(float), .overhead = 0,
                                  .sizex = 1 << 16, .sizey = 1 };

  if(dt_opencl_local_buffer_opt(devid, gd->kernel_sharpen_hblur, &hlocopt))
    hblocksize = hlocopt.sizex;
  else
    hblocksize = 1;

  int vblocksize;
  dt_opencl_local_buffer_t vlocopt
    = (dt_opencl_local_buffer_t){ .xoffset = 1, .xfactor = 1, .yoffset = 2 * rad, .yfactor = 1,
                                  .cellsize = sizeof(float), .overhead = 0,
                                  .sizex = 1, .sizey = 1 << 16 };

  if(dt_opencl_local_buffer_opt(devid, gd->kernel_sharpen_vblur, &vlocopt))
    vblocksize = vlocopt.sizey;
  else
    vblocksize = 1;


  const size_t bwidth = ROUNDUP(width, hblocksize);
  const size_t bheight = ROUNDUP(height, vblocksize);

  size_t sizes[3];
  size_t local[3];

  dev_tmp = dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
  if(dev_tmp == NULL) goto error;

  dev_m = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * wd, mat);
  if(dev_m == NULL) goto error;

  /* horizontal blur */
  sizes[0] = bwidth;
  sizes[1] = ROUNDUPDHT(height, devid);
  sizes[2] = 1;
  local[0] = hblocksize;
  local[1] = 1;
  local[2] = 1;
  dt_opencl_set_kernel_args(devid, gd->kernel_sharpen_hblur, 0, CLARG(dev_in), CLARG(dev_out), CLARG(dev_m),
    CLARG(rad), CLARG(width), CLARG(height), CLARG(hblocksize), CLLOCAL((hblocksize + 2 * rad) * sizeof(float)));
  err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_sharpen_hblur, sizes, local);
  if(err != CL_SUCCESS) goto error;

  /* vertical blur */
  sizes[0] = ROUNDUPDWD(width, devid);
  sizes[1] = bheight;
  sizes[2] = 1;
  local[0] = 1;
  local[1] = vblocksize;
  local[2] = 1;
  dt_opencl_set_kernel_args(devid, gd->kernel_sharpen_vblur, 0, CLARG(dev_out), CLARG(dev_tmp), CLARG(dev_m),
    CLARG(rad), CLARG(width), CLARG(height), CLARG(vblocksize), CLLOCAL((vblocksize + 2 * rad) * sizeof(float)));
  err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_sharpen_vblur, sizes, local);
  if(err != CL_SUCCESS) goto error;

  /* mixing tmp and in -> out */
  sizes[0] = ROUNDUPDWD(width, devid);
  sizes[1] = ROUNDUPDHT(height, devid);
  sizes[2] = 1;
  dt_opencl_set_kernel_args(devid, gd->kernel_sharpen_mix, 0, CLARG(dev_in), CLARG(dev_tmp), CLARG(dev_out),
    CLARG(width), CLARG(height), CLARG(d->amount), CLARG(d->threshold));
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_sharpen_mix, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_tmp);
  dt_free_align(mat);
  return TRUE;

error:
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_tmp);
  dt_free_align(mat);
  dt_print(DT_DEBUG_OPENCL, "[opencl_sharpen] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}
#endif


void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  dt_iop_sharpen_data_t *d = (dt_iop_sharpen_data_t *)piece->data;
  const int rad = MIN(MAXR, ceilf(d->radius * roi_in->scale / piece->iscale));

  tiling->factor = 2.1f; // in + out + tmprow
  tiling->factor_cl = 3.0f; // in + out + tmp
  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = rad;
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return;
  const dt_iop_sharpen_data_t *const data = (dt_iop_sharpen_data_t *)piece->data;
  const int rad = MIN(MAXR, ceilf(data->radius * roi_in->scale / piece->iscale));
  // Special case handling: very small image with one or two dimensions below 2*rad+1 treat as no sharpening and just
  // pass through.  This avoids handling of all kinds of border cases below.
  if(rad == 0 ||
     (roi_out->width < 2 * rad + 1 || roi_out->height < 2 * rad + 1))
  {
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, 4);
    return;
  }

  float *restrict tmp;	// one row per thread
  size_t padded_size;
  if(!dt_iop_alloc_image_buffers(self, roi_in, roi_out,
                                  1 | DT_IMGSZ_WIDTH | DT_IMGSZ_PERTHREAD, &tmp, &padded_size,
                                  0))
  {
    dt_iop_copy_image_roi(ovoid, ivoid, 4, roi_in, roi_out, TRUE);
    return;
  }

  const int wd = 2 * rad + 1;
  const int wd4 = (wd & 3) ? (wd >> 2) + 1 : wd >> 2;

  const size_t mat_size = (size_t)4 * wd4;
  const float sigma2 = (1.0f / (2.5 * 2.5)) * (data->radius * roi_in->scale / piece->iscale)
                       * (data->radius * roi_in->scale / piece->iscale);
  float *const mat = init_gaussian_kernel(rad, mat_size, sigma2);
  if(!mat)
  {
    dt_print(DT_DEBUG_ALWAYS,"[sharpen] out of memory\n");
    dt_iop_copy_image_roi(ovoid, ivoid, 4, roi_in, roi_out, TRUE);
    return;
  }
  const float *const restrict in = (float*)ivoid;
  const size_t width = roi_out->width;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, ovoid, mat, rad, width, roi_in, roi_out, tmp, padded_size, data) \
  schedule(static)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    // We skip the top and bottom 'rad' rows because the kernel would extend beyond the edge of the image, resulting
    // in an incomplete summation.
    if(j < rad || j >= roi_out->height - rad)
    {
      // fill in the top/bottom border with unchanged luma values from the input image.
      const float *const restrict row_in = in + (size_t)4 * j * width;
      float *const restrict row_out = ((float*)ovoid) + (size_t)4 * j * width;
      memcpy(row_out, row_in, 4 * sizeof(float) * width);
      continue;
    }
    // Get a thread-local temporary buffer for processing the current row of the image.
    float *const restrict temp_buf = dt_get_perthread(tmp, padded_size);
    // vertically blur the pixels of the current row into the temp buffer
    const size_t start_row = j-rad;
    const size_t end_row = j+rad;
    const size_t end_bulk = width & ~(size_t)3;
    // do the bulk of the row four at a time
    for(size_t i = 0; i < end_bulk; i += 4)
    {
      dt_aligned_pixel_t sum = { 0.0f };
      for(size_t k = start_row; k <= end_row; k++)
      {
        const size_t k_adj = k - start_row;
        for_four_channels(c,aligned(in))
          sum[c] += mat[k_adj] * in[4*(k*width+i+c)];
      }
      float *const vblurred = temp_buf + i;
      for_four_channels(c,aligned(vblurred))
        vblurred[c] = sum[c];
    }
    // do the leftover 0-3 pixels of the row
    for(size_t i = end_bulk; i < width; i++)
    {
      float sum = 0.0f;
      for(size_t k = start_row; k <= end_row; k++)
      {
        const size_t k_adj = k - start_row;
        sum += mat[k_adj] * in[4*(k*width+i)];
      }
      temp_buf[i] = sum;
    }

    // now horizontally blur the already vertically-blurred pixels from the temp buffer to the final output buffer
    // we can skip the left-most and right-most pixels for the same reason as we skipped the top and bottom borders.
    float *const restrict row_out = ((float*)ovoid) + (size_t)4 * j * width;
    for(int i = 0; i < rad; i++)
      copy_pixel(row_out + 4*i, in + 4*(j*width+i));  //copy unsharpened border pixel
    const float threshold = data->threshold;
    const float amount = data->amount;
    for(int i = rad; i < roi_out->width - rad; i++)
    {
      float sum = 0.0f;
      for(int k = i-rad; k <= i+rad; k++)
      {
        const int k_adj = k - (i-rad);
        sum += mat[k_adj] * temp_buf[k];
      }
      // subtract the blurred pixel's luma from the original input pixel's luma
      const size_t index = 4 * (j * width + i);
      const float diff = in[index] - sum;
      const float absdiff = fabs(diff);
      const float detail = (absdiff > threshold) ? copysignf(MAX(absdiff - threshold, 0.0f), diff) : 0.0f;
      row_out[4*i] = in[index] + detail * amount;
      row_out[4*i + 1] = in[index + 1];
      row_out[4*i + 2] = in[index + 2];
    }
    for(int i = roi_out->width - rad; i < roi_out->width; i++)
      copy_pixel(row_out + 4*i, in + 4*(j*width+i));  //copy unsharpened border pixel
  }

  dt_free_align(mat);
  dt_free_align(tmp);

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
    dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_sharpen_params_t *p = (dt_iop_sharpen_params_t *)p1;
  dt_iop_sharpen_data_t *d = (dt_iop_sharpen_data_t *)piece->data;

  // actually need to increase the mask to fit 2.5 sigma inside
  d->radius = 2.5f * p->radius;
  d->amount = p->amount;
  d->threshold = p->threshold;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_sharpen_data_t));
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 7; // sharpen.cl, from programs.conf
  dt_iop_sharpen_global_data_t *gd
      = (dt_iop_sharpen_global_data_t *)malloc(sizeof(dt_iop_sharpen_global_data_t));
  module->data = gd;
  gd->kernel_sharpen_hblur = dt_opencl_create_kernel(program, "sharpen_hblur");
  gd->kernel_sharpen_vblur = dt_opencl_create_kernel(program, "sharpen_vblur");
  gd->kernel_sharpen_mix = dt_opencl_create_kernel(program, "sharpen_mix");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_sharpen_global_data_t *gd = (dt_iop_sharpen_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_sharpen_hblur);
  dt_opencl_free_kernel(gd->kernel_sharpen_vblur);
  dt_opencl_free_kernel(gd->kernel_sharpen_mix);
  free(module->data);
  module->data = NULL;
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_sharpen_gui_data_t *g = IOP_GUI_ALLOC(sharpen);

  g->radius = dt_bauhaus_slider_from_params(self, N_("radius"));
  dt_bauhaus_slider_set_soft_max(g->radius, 8.0);
  dt_bauhaus_slider_set_digits(g->radius, 3);
  gtk_widget_set_tooltip_text(g->radius, _("spatial extent of the unblurring"));

  g->amount = dt_bauhaus_slider_from_params(self, N_("amount"));
  dt_bauhaus_slider_set_digits(g->amount, 3);
  gtk_widget_set_tooltip_text(g->amount, _("strength of the sharpen"));

  g->threshold = dt_bauhaus_slider_from_params(self, N_("threshold"));
  dt_bauhaus_slider_set_digits(g->threshold, 3);
  gtk_widget_set_tooltip_text(g->threshold, _("threshold to activate sharpen"));
}

#undef MAXR

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

