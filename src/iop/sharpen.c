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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#if defined(__SSE__)
#include <xmmintrin.h>
#endif
#include "bauhaus/bauhaus.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/tiling.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"
#include "common/iop_group.h"
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
  float radius, amount, threshold;
} dt_iop_sharpen_params_t;

typedef struct dt_iop_sharpen_gui_data_t
{
  GtkWidget *scale1, *scale2, *scale3;
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

int groups()
{
  return dt_iop_get_group("sharpen", IOP_GROUP_CORRECT);
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_sharpen_params_t tmp = (dt_iop_sharpen_params_t){ 2.0, 0.5, 0.5 };
  // add the preset.
  dt_gui_presets_add_generic(_("sharpen"), self->op, self->version(), &tmp, sizeof(dt_iop_sharpen_params_t),
                             1);
  // restrict to raw images
  dt_gui_presets_update_ldr(_("sharpen"), self->op, self->version(), FOR_RAW);
  // make it auto-apply for matching images:
  dt_gui_presets_update_autoapply(_("sharpen"), self->op, self->version(), 1);
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "radius"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "amount"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "threshold"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_sharpen_gui_data_t *g = (dt_iop_sharpen_gui_data_t *)self->gui_data;

  dt_accel_connect_slider_iop(self, "radius", g->scale1);
  dt_accel_connect_slider_iop(self, "amount", g->scale2);
  dt_accel_connect_slider_iop(self, "threshold", g->scale3);
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_sharpen_data_t *d = (dt_iop_sharpen_data_t *)piece->data;
  dt_iop_sharpen_global_data_t *gd = (dt_iop_sharpen_global_data_t *)self->data;
  cl_mem dev_m = NULL;
  cl_mem dev_tmp = NULL;
  cl_int err = -999;

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

  mat = malloc(wd * sizeof(float));

  // init gaussian kernel
  float *m = mat + rad;
  const float sigma2 = (1.0f / (2.5 * 2.5)) * (d->radius * roi_in->scale / piece->iscale)
                       * (d->radius * roi_in->scale / piece->iscale);
  float weight = 0.0f;
  for(int l = -rad; l <= rad; l++) weight += m[l] = expf(-(l * l) / (2.f * sigma2));
  for(int l = -rad; l <= rad; l++) m[l] /= weight;

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

  dev_tmp = dt_opencl_alloc_device(devid, width, height, 4 * sizeof(float));
  if(dev_tmp == NULL) goto error;

  dev_m = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * wd, mat);
  if(dev_m == NULL) goto error;

  /* horizontal blur */
  sizes[0] = bwidth;
  sizes[1] = ROUNDUPHT(height);
  sizes[2] = 1;
  local[0] = hblocksize;
  local[1] = 1;
  local[2] = 1;
  dt_opencl_set_kernel_arg(devid, gd->kernel_sharpen_hblur, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_sharpen_hblur, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_sharpen_hblur, 2, sizeof(cl_mem), (void *)&dev_m);
  dt_opencl_set_kernel_arg(devid, gd->kernel_sharpen_hblur, 3, sizeof(int), (void *)&rad);
  dt_opencl_set_kernel_arg(devid, gd->kernel_sharpen_hblur, 4, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_sharpen_hblur, 5, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_sharpen_hblur, 6, sizeof(int), (void *)&hblocksize);
  dt_opencl_set_kernel_arg(devid, gd->kernel_sharpen_hblur, 7, (hblocksize + 2 * rad) * sizeof(float), NULL);
  err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_sharpen_hblur, sizes, local);
  if(err != CL_SUCCESS) goto error;

  /* vertical blur */
  sizes[0] = ROUNDUPWD(width);
  sizes[1] = bheight;
  sizes[2] = 1;
  local[0] = 1;
  local[1] = vblocksize;
  local[2] = 1;
  dt_opencl_set_kernel_arg(devid, gd->kernel_sharpen_vblur, 0, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_sharpen_vblur, 1, sizeof(cl_mem), (void *)&dev_tmp);
  dt_opencl_set_kernel_arg(devid, gd->kernel_sharpen_vblur, 2, sizeof(cl_mem), (void *)&dev_m);
  dt_opencl_set_kernel_arg(devid, gd->kernel_sharpen_vblur, 3, sizeof(int), (void *)&rad);
  dt_opencl_set_kernel_arg(devid, gd->kernel_sharpen_vblur, 4, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_sharpen_vblur, 5, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_sharpen_vblur, 6, sizeof(int), (void *)&vblocksize);
  dt_opencl_set_kernel_arg(devid, gd->kernel_sharpen_vblur, 7, (vblocksize + 2 * rad) * sizeof(float), NULL);
  err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_sharpen_vblur, sizes, local);
  if(err != CL_SUCCESS) goto error;

  /* mixing tmp and in -> out */
  sizes[0] = ROUNDUPWD(width);
  sizes[1] = ROUNDUPHT(height);
  sizes[2] = 1;
  dt_opencl_set_kernel_arg(devid, gd->kernel_sharpen_mix, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_sharpen_mix, 1, sizeof(cl_mem), (void *)&dev_tmp);
  dt_opencl_set_kernel_arg(devid, gd->kernel_sharpen_mix, 2, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_sharpen_mix, 3, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_sharpen_mix, 4, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_sharpen_mix, 5, sizeof(float), (void *)&d->amount);
  dt_opencl_set_kernel_arg(devid, gd->kernel_sharpen_mix, 6, sizeof(float), (void *)&d->threshold);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_sharpen_mix, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_tmp);
  free(mat);
  return TRUE;

error:
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_tmp);
  free(mat);
  dt_print(DT_DEBUG_OPENCL, "[opencl_sharpen] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif


void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  dt_iop_sharpen_data_t *d = (dt_iop_sharpen_data_t *)piece->data;
  const int rad = MIN(MAXR, ceilf(d->radius * roi_in->scale / piece->iscale));

  tiling->factor = 3.0f; // in + out + tmp
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
  const dt_iop_sharpen_data_t *const data = (dt_iop_sharpen_data_t *)piece->data;
  const int ch = piece->colors;
  const int rad = MIN(MAXR, ceilf(data->radius * roi_in->scale / piece->iscale));
  if(rad == 0)
  {
    memcpy(ovoid, ivoid, (size_t)sizeof(float) * ch * roi_out->width * roi_out->height);
    return;
  }

  // special case handling: very small image with one or two dimensions below 2*rad+1 => no sharpening
  // avoids handling of all kinds of border cases below
  if(roi_out->width < 2 * rad + 1 || roi_out->height < 2 * rad + 1)
  {
    memcpy(ovoid, ivoid, (size_t)sizeof(float) * ch * roi_out->width * roi_out->height);
    return;
  }

  float *const tmp = dt_alloc_align(16, (size_t)sizeof(float) * roi_out->width * roi_out->height);
  if(tmp == NULL)
  {
    fprintf(stderr, "[sharpen] failed to allocate temporary buffer\n");
    return;
  }

  const int wd = 2 * rad + 1;
  const int wd4 = (wd & 3) ? (wd >> 2) + 1 : wd >> 2;

  const size_t mat_size = wd4 * 4 * sizeof(float);
  float *const mat = dt_alloc_align(16, mat_size);
  memset(mat, 0, mat_size);

  const float sigma2 = (1.0f / (2.5 * 2.5)) * (data->radius * roi_in->scale / piece->iscale)
                       * (data->radius * roi_in->scale / piece->iscale);
  float weight = 0.0f;

  // init gaussian kernel
  for(int l = -rad; l <= rad; l++) weight += mat[l + rad] = expf(-l * l / (2.f * sigma2));
  for(int l = -rad; l <= rad; l++) mat[l + rad] /= weight;

// gauss blur the image horizontally
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    const float *in = ((float *)ivoid) + (size_t)ch * (j * roi_in->width + rad);
    float *out = tmp + (size_t)j * roi_out->width + rad;
    int i;
    for(i = rad; i < roi_out->width - wd4 * 4 + rad; i++)
    {
      const float *inp = in - ch * rad;
      __attribute__((aligned(16))) float sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

      for(int k = 0; k < wd4 * 4; k += 4, inp += 4 * ch)
      {
        for(int c = 0; c < 4; c++)
        {
          sum[c] += ((mat[k + c]) * (inp[ch * c]));
        }
      }
      *out = sum[0] + sum[1] + sum[2] + sum[3];
      out++;
      in += ch;
    }
    for(; i < roi_out->width - rad; i++)
    {
      const float *inp = in - ch * rad;
      const float *m = mat;
      float sum = 0.0f;
      for(int k = -rad; k <= rad; k++, m++, inp += ch)
      {
        sum += *m * *inp;
      }
      *out = sum;
      out++;
      in += ch;
    }
  }

// gauss blur the image vertically
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j = rad; j < roi_out->height - wd4 * 4 + rad; j++)
  {
    const float *in = tmp + (size_t)j * roi_in->width;
    float *out = ((float *)ovoid) + (size_t)ch * j * roi_out->width;

    const int step = roi_in->width;

    for(int i = 0; i < roi_out->width; i++)
    {
      const float *inp = in - step * rad;
      __attribute__((aligned(16))) float sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

      for(int k = 0; k < wd4 * 4; k += 4, inp += step * 4)
      {
        for(int c = 0; c < 4; c++)
        {
          sum[c] += ((mat[k + c]) * (inp[step * c]));
        }
      }
      *out = sum[0] + sum[1] + sum[2] + sum[3];
      out += ch;
      in++;
    }
  }
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j = roi_out->height - wd4 * 4 + rad; j < roi_out->height - rad; j++)
  {
    const float *in = tmp + (size_t)j * roi_in->width;
    float *out = ((float *)ovoid) + (size_t)ch * j * roi_out->width;
    const int step = roi_in->width;

    for(int i = 0; i < roi_out->width; i++)
    {
      const float *inp = in - step * rad;
      const float *m = mat;
      float sum = 0.0f;
      for(int k = -rad; k <= rad; k++, m++, inp += step) sum += *m * *inp;
      *out = sum;
      out += ch;
      in++;
    }
  }

  dt_free_align(mat);

  // fill unsharpened border
  for(int j = 0; j < rad; j++)
    memcpy(((float *)ovoid) + (size_t)ch * j * roi_out->width,
           ((float *)ivoid) + (size_t)ch * j * roi_in->width, (size_t)ch * sizeof(float) * roi_out->width);
  for(int j = roi_out->height - rad; j < roi_out->height; j++)
    memcpy(((float *)ovoid) + (size_t)ch * j * roi_out->width,
           ((float *)ivoid) + (size_t)ch * j * roi_in->width, (size_t)ch * sizeof(float) * roi_out->width);

  dt_free_align(tmp);

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j = rad; j < roi_out->height - rad; j++)
  {
    float *in = ((float *)ivoid) + (size_t)ch * roi_out->width * j;
    float *out = ((float *)ovoid) + (size_t)ch * roi_out->width * j;
    for(int i = 0; i < rad; i++) out[ch * i] = in[ch * i];
    for(int i = roi_out->width - rad; i < roi_out->width; i++) out[ch * i] = in[ch * i];
  }

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  // subtract blurred image, if diff > thrs, add *amount to original image
  for(int j = 0; j < roi_out->height; j++)
  {
    float *in = (float *)ivoid + (size_t)j * ch * roi_out->width;
    float *out = (float *)ovoid + (size_t)j * ch * roi_out->width;

    for(int i = 0; i < roi_out->width; i++)
    {
      out[1] = in[1];
      out[2] = in[2];
      const float diff = in[0] - out[0];
      if(fabsf(diff) > data->threshold)
      {
        const float detail = copysignf(fmaxf(fabsf(diff) - data->threshold, 0.0), diff);
        out[0] = in[0] + detail * data->amount;
      }
      else
        out[0] = in[0];
      out += ch;
      in += ch;
    }
  }

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

#if defined(__SSE__)
void process_sse2(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                  void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_sharpen_data_t *data = (dt_iop_sharpen_data_t *)piece->data;
  const int ch = piece->colors;
  const int rad = MIN(MAXR, ceilf(data->radius * roi_in->scale / piece->iscale));
  if(rad == 0)
  {
    memcpy(ovoid, ivoid, (size_t)sizeof(float) * ch * roi_out->width * roi_out->height);
    return;
  }

  // special case handling: very small image with one or two dimensions below 2*rad+1 => no sharpening
  // avoids handling of all kinds of border cases below
  if(roi_out->width < 2 * rad + 1 || roi_out->height < 2 * rad + 1)
  {
    memcpy(ovoid, ivoid, (size_t)sizeof(float) * ch * roi_out->width * roi_out->height);
    return;
  }

  float *const tmp = dt_alloc_align(16, (size_t)sizeof(float) * roi_out->width * roi_out->height);
  if(tmp == NULL)
  {
    fprintf(stderr, "[sharpen] failed to allocate temporary buffer\n");
    return;
  }

  const int wd = 2 * rad + 1;
  const int wd4 = (wd & 3) ? (wd >> 2) + 1 : wd >> 2;

  const size_t mat_size = wd4 * 4 * sizeof(float);
  float *const mat = dt_alloc_align(16, mat_size);
  memset(mat, 0, mat_size);

  const float sigma2 = (1.0f / (2.5 * 2.5)) * (data->radius * roi_in->scale / piece->iscale)
                       * (data->radius * roi_in->scale / piece->iscale);
  float weight = 0.0f;

  // init gaussian kernel
  for(int l = -rad; l <= rad; l++) weight += mat[l + rad] = expf(-l * l / (2.f * sigma2));
  for(int l = -rad; l <= rad; l++) mat[l + rad] /= weight;

// gauss blur the image horizontally
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    const float *in = ((float *)ivoid) + (size_t)ch * (j * roi_in->width + rad);
    float *out = tmp + (size_t)j * roi_out->width + rad;
    int i;
    for(i = rad; i < roi_out->width - wd4 * 4 + rad; i++)
    {
      const float *inp = in - ch * rad;
      __attribute__((aligned(16))) float sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
      __m128 msum = _mm_setzero_ps();

      for(int k = 0; k < wd4 * 4; k += 4, inp += 4 * ch)
      {
        msum = _mm_add_ps(
            msum, _mm_mul_ps(_mm_load_ps(mat + k), _mm_set_ps(inp[3 * ch], inp[2 * ch], inp[ch], inp[0])));
      }
      _mm_store_ps(sum, msum);
      *out = sum[0] + sum[1] + sum[2] + sum[3];
      out++;
      in += ch;
    }
    for(; i < roi_out->width - rad; i++)
    {
      const float *inp = in - ch * rad;
      const float *m = mat;
      float sum = 0.0f;
      for(int k = -rad; k <= rad; k++, m++, inp += ch)
      {
        sum += *m * *inp;
      }
      *out = sum;
      out++;
      in += ch;
    }
  }
  _mm_sfence();

// gauss blur the image vertically
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j = rad; j < roi_out->height - wd4 * 4 + rad; j++)
  {
    const float *in = tmp + (size_t)j * roi_in->width;
    float *out = ((float *)ovoid) + (size_t)ch * j * roi_out->width;

    const int step = roi_in->width;

    __attribute__((aligned(16))) float sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    for(int i = 0; i < roi_out->width; i++)
    {
      const float *inp = in - step * rad;
      __m128 msum = _mm_setzero_ps();

      for(int k = 0; k < wd4 * 4; k += 4, inp += step * 4)
      {
        msum = _mm_add_ps(msum, _mm_mul_ps(_mm_load_ps(mat + k),
                                           _mm_set_ps(inp[3 * step], inp[2 * step], inp[step], inp[0])));
      }
      _mm_store_ps(sum, msum);
      *out = sum[0] + sum[1] + sum[2] + sum[3];
      out += ch;
      in++;
    }
  }
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j = roi_out->height - wd4 * 4 + rad; j < roi_out->height - rad; j++)
  {
    const float *in = tmp + (size_t)j * roi_in->width;
    float *out = ((float *)ovoid) + (size_t)ch * j * roi_out->width;
    const int step = roi_in->width;

    for(int i = 0; i < roi_out->width; i++)
    {
      const float *inp = in - step * rad;
      const float *m = mat;
      float sum = 0.0f;
      for(int k = -rad; k <= rad; k++, m++, inp += step) sum += *m * *inp;
      *out = sum;
      out += ch;
      in++;
    }
  }

  dt_free_align(mat);

  _mm_sfence();

  // fill unsharpened border
  for(int j = 0; j < rad; j++)
    memcpy(((float *)ovoid) + (size_t)ch * j * roi_out->width,
           ((float *)ivoid) + (size_t)ch * j * roi_in->width, (size_t)ch * sizeof(float) * roi_out->width);
  for(int j = roi_out->height - rad; j < roi_out->height; j++)
    memcpy(((float *)ovoid) + (size_t)ch * j * roi_out->width,
           ((float *)ivoid) + (size_t)ch * j * roi_in->width, (size_t)ch * sizeof(float) * roi_out->width);

  dt_free_align(tmp);

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j = rad; j < roi_out->height - rad; j++)
  {
    float *in = ((float *)ivoid) + (size_t)ch * roi_out->width * j;
    float *out = ((float *)ovoid) + (size_t)ch * roi_out->width * j;
    for(int i = 0; i < rad; i++) out[ch * i] = in[ch * i];
    for(int i = roi_out->width - rad; i < roi_out->width; i++) out[ch * i] = in[ch * i];
  }

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(data) schedule(static)
#endif
  // subtract blurred image, if diff > thrs, add *amount to original image
  for(int j = 0; j < roi_out->height; j++)
  {
    float *in = (float *)ivoid + (size_t)j * ch * roi_out->width;
    float *out = (float *)ovoid + (size_t)j * ch * roi_out->width;

    for(int i = 0; i < roi_out->width; i++)
    {
      out[1] = in[1];
      out[2] = in[2];
      const float diff = in[0] - out[0];
      if(fabsf(diff) > data->threshold)
      {
        const float detail = copysignf(fmaxf(fabsf(diff) - data->threshold, 0.0), diff);
        out[0] = in[0] + detail * data->amount;
      }
      else
        out[0] = in[0];
      out += ch;
      in += ch;
    }
  }

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}
#endif

static void radius_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_sharpen_params_t *p = (dt_iop_sharpen_params_t *)self->params;
  p->radius = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void amount_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_sharpen_params_t *p = (dt_iop_sharpen_params_t *)self->params;
  p->amount = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void threshold_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_sharpen_params_t *p = (dt_iop_sharpen_params_t *)self->params;
  p->threshold = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
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
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_sharpen_gui_data_t *g = (dt_iop_sharpen_gui_data_t *)self->gui_data;
  dt_iop_sharpen_params_t *p = (dt_iop_sharpen_params_t *)module->params;
  dt_bauhaus_slider_set_soft(g->scale1, p->radius);
  dt_bauhaus_slider_set(g->scale2, p->amount);
  dt_bauhaus_slider_set(g->scale3, p->threshold);
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_sharpen_data_t));
  module->params = calloc(1, sizeof(dt_iop_sharpen_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_sharpen_params_t));
  module->default_enabled = 0;
  module->priority = 742; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_sharpen_params_t);
  module->gui_data = NULL;
  dt_iop_sharpen_params_t tmp = (dt_iop_sharpen_params_t){ 2.0, 0.5, 0.5 };
  memcpy(module->params, &tmp, sizeof(dt_iop_sharpen_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_sharpen_params_t));
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

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
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
  self->gui_data = malloc(sizeof(dt_iop_sharpen_gui_data_t));
  dt_iop_sharpen_gui_data_t *g = (dt_iop_sharpen_gui_data_t *)self->gui_data;
  dt_iop_sharpen_params_t *p = (dt_iop_sharpen_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));

  g->scale1 = dt_bauhaus_slider_new_with_range(self, 0.0, 8.0000, 0.100, p->radius, 3);
  gtk_widget_set_tooltip_text(g->scale1, _("spatial extent of the unblurring"));
  dt_bauhaus_widget_set_label(g->scale1, NULL, _("radius"));
  dt_bauhaus_slider_enable_soft_boundaries(g->scale1, 0.0, 99.0);
  g->scale2 = dt_bauhaus_slider_new_with_range(self, 0.0, 2.0000, 0.010, p->amount, 3);
  gtk_widget_set_tooltip_text(g->scale2, _("strength of the sharpen"));
  dt_bauhaus_widget_set_label(g->scale2, NULL, _("amount"));
  g->scale3 = dt_bauhaus_slider_new_with_range(self, 0.0, 100.00, 0.100, p->threshold, 3);
  gtk_widget_set_tooltip_text(g->scale3, _("threshold to activate sharpen"));
  dt_bauhaus_widget_set_label(g->scale3, NULL, _("threshold"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale1, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale2, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale3, TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(g->scale1), "value-changed", G_CALLBACK(radius_callback), self);
  g_signal_connect(G_OBJECT(g->scale2), "value-changed", G_CALLBACK(amount_callback), self);
  g_signal_connect(G_OBJECT(g->scale3), "value-changed", G_CALLBACK(threshold_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

#undef MAXR

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
