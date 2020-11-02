/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "dtgtk/resetlabel.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"
#include <gtk/gtk.h>
#include <inttypes.h>

#if defined(__SSE__)
#include <xmmintrin.h>
#endif

#define MAX_RADIUS 32
#define BOX_ITERATIONS 8

#define CLIP(x) ((x < 0) ? 0.0 : (x > 1.0) ? 1.0 : x)
#define MM_CLIP_PS(X) (_mm_min_ps(_mm_max_ps((X), _mm_setzero_ps()), _mm_set1_ps(1.0)))

DT_MODULE_INTROSPECTION(1, dt_iop_soften_params_t)

typedef struct dt_iop_soften_params_t
{
  float size;       // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 50.0
  float saturation; // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 100.0
  float brightness; // $MIN: -2.0 $MAX: 2.0 $DEFAULT: 0.33
  float amount;     // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 50.0 $DESCRIPTION: "mix"
} dt_iop_soften_params_t;

typedef struct dt_iop_soften_gui_data_t
{
  GtkWidget *size, *saturation, *brightness, *amount;
} dt_iop_soften_gui_data_t;

typedef struct dt_iop_soften_data_t
{
  float size;
  float saturation;
  float brightness;
  float amount;
} dt_iop_soften_data_t;

typedef struct dt_iop_soften_global_data_t
{
  int kernel_soften_overexposed;
  int kernel_soften_hblur;
  int kernel_soften_vblur;
  int kernel_soften_mix;
} dt_iop_soften_global_data_t;


const char *name()
{
  return _("soften");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_group()
{
  return IOP_GROUP_EFFECT | IOP_GROUP_EFFECTS;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_rgb;
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_soften_data_t *const d = (const dt_iop_soften_data_t *const)piece->data;

  const int ch = piece->colors;

  const float brightness = 1.0 / exp2f(-d->brightness);
  const float saturation = d->saturation / 100.0;

  const float *const in = (const float *const)ivoid;
  float *const out = (float *const)ovoid;

/* create overexpose image and then blur */
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(brightness, ch, in, out, roi_out, saturation) \
  schedule(static)
#endif
  for(size_t k = 0; k < (size_t)ch * roi_out->width * roi_out->height; k += ch)
  {
    float h, s, l;
    rgb2hsl(&in[k], &h, &s, &l);
    s *= saturation;
    l *= brightness;
    hsl2rgb(&out[k], h, CLIP(s), CLIP(l));
  }

  const float w = piece->iwidth * piece->iscale;
  const float h = piece->iheight * piece->iscale;
  int mrad = sqrt(w * w + h * h) * 0.01;
  int rad = mrad * (fmin(100.0, d->size + 1) / 100.0);
  const int radius = MIN(mrad, ceilf(rad * roi_in->scale / piece->iscale));

  const int size = roi_out->width > roi_out->height ? roi_out->width : roi_out->height;

  const size_t scanline_size = (size_t)4 * size;
  float *const scanline_buf = dt_alloc_align(64, scanline_size * dt_get_num_threads() * sizeof(float));

  for(int iteration = 0; iteration < BOX_ITERATIONS; iteration++)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(ch, radius, out, roi_out, scanline_buf, scanline_size) \
    schedule(static)
#endif
    /* horizontal blur out into out */
    for(int y = 0; y < roi_out->height; y++)
    {
      float *scanline = scanline_buf + scanline_size * dt_get_thread_num();
      __attribute__((aligned(64))) float L[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

      size_t index = (size_t)y * roi_out->width;
      int hits = 0;
      for(int x = -radius; x < roi_out->width; x++)
      {
        int op = x - radius - 1;
        int np = x + radius;
        if(op >= 0)
        {
          for(int c = 0; c < 4; c++)
          {
            L[c] -= out[((index + op) * ch) + c];
          }
          hits--;
        }
        if(np < roi_out->width)
        {
          for(int c = 0; c < 4; c++)
          {
            L[c] += out[((index + np) * ch) + c];
          }
          hits++;
        }
        if(x >= 0)
        {
          for(int c = 0; c < 4; c++)
          {
            scanline[4 * x + c] = L[c] / hits;
          }
        }
      }

      for(int x = 0; x < roi_out->width; x++)
      {
        for(int c = 0; c < 4; c++)
        {
          out[(index + x) * ch + c] = scanline[4 * x + c];
        }
      }
    }

    /* vertical pass on blurlightness */
    const int opoffs = -(radius + 1) * roi_out->width;
    const int npoffs = (radius)*roi_out->width;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(ch, npoffs, opoffs, radius, out, roi_out, \
                        scanline_buf, scanline_size) \
    schedule(static)
#endif
    for(int x = 0; x < roi_out->width; x++)
    {
      float *scanline = scanline_buf + scanline_size * dt_get_thread_num();
      __attribute__((aligned(64))) float L[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

      int hits = 0;
      size_t index = (size_t)x - radius * roi_out->width;
      for(int y = -radius; y < roi_out->height; y++)
      {
        int op = y - radius - 1;
        int np = y + radius;

        if(op >= 0)
        {
          for(int c = 0; c < 4; c++)
          {
            L[c] -= out[((index + opoffs) * ch) + c];
          }
          hits--;
        }
        if(np < roi_out->height)
        {
          for(int c = 0; c < 4; c++)
          {
            L[c] += out[((index + npoffs) * ch) + c];
          }
          hits++;
        }
        if(y >= 0)
        {
          for(int c = 0; c < 4; c++)
          {
            scanline[4 * y + c] = L[c] / hits;
          }
        }
        index += roi_out->width;
      }

      for(int y = 0; y < roi_out->height; y++)
      {
        for(int c = 0; c < 4; c++)
        {
          out[((size_t)y * roi_out->width + x) * ch + c] = scanline[ch * y + c];
        }
      }
    }
  }

  dt_free_align(scanline_buf);

  const float amount = (d->amount / 100.0);
  const float amount_1 = (1 - (d->amount) / 100.0);

#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) \
  dt_omp_firstprivate(amount, amount_1, ch, in, out, roi_out) \
  schedule(static) \
  collapse(2)
#endif
  for(size_t k = 0; k < (size_t)ch * roi_out->width * roi_out->height; k += ch)
  {
    for(int c = 0; c < 4; c++)
    {
      out[k + c] = ((in[k + c] * amount_1) + (CLIP(out[k + c]) * amount));
    }
  }
}

#if defined(__SSE__)
void process_sse2(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                  void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_soften_data_t *data = (dt_iop_soften_data_t *)piece->data;
  float *in = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;

  const float brightness = 1.0 / exp2f(-data->brightness);
  const float saturation = data->saturation / 100.0;
/* create overexpose image and then blur */
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, brightness, roi_out, saturation) \
  shared(in, out) \
  schedule(static)
#endif
  for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
  {
    size_t index = ch * k;
    float h, s, l;
    rgb2hsl(&in[index], &h, &s, &l);
    s *= saturation;
    l *= brightness;
    hsl2rgb(&out[index], h, CLIP(s), CLIP(l));
  }

  const float w = piece->iwidth * piece->iscale;
  const float h = piece->iheight * piece->iscale;
  int mrad = sqrt(w * w + h * h) * 0.01;
  int rad = mrad * (fmin(100.0, data->size + 1) / 100.0);
  const int radius = MIN(mrad, ceilf(rad * roi_in->scale / piece->iscale));

  const int size = roi_out->width > roi_out->height ? roi_out->width : roi_out->height;

  __m128 *const scanline_buf = dt_alloc_align(64, size * dt_get_num_threads() * sizeof(__m128));

  for(int iteration = 0; iteration < BOX_ITERATIONS; iteration++)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(ch, radius, roi_out, scanline_buf, size) \
    shared(out) \
    schedule(static)
#endif
    /* horizontal blur out into out */
    for(int y = 0; y < roi_out->height; y++)
    {
      __m128 *scanline = scanline_buf + size * dt_get_thread_num();
      size_t index = (size_t)y * roi_out->width;
      __m128 L = _mm_setzero_ps();
      int hits = 0;
      for(int x = -radius; x < roi_out->width; x++)
      {
        int op = x - radius - 1;
        int np = x + radius;
        if(op >= 0)
        {
          L = _mm_sub_ps(L, _mm_load_ps(&out[(index + op) * ch]));
          hits--;
        }
        if(np < roi_out->width)
        {
          L = _mm_add_ps(L, _mm_load_ps(&out[(index + np) * ch]));
          hits++;
        }
        if(x >= 0) scanline[x] = _mm_div_ps(L, _mm_set_ps1(hits));
      }

      for(int x = 0; x < roi_out->width; x++) _mm_store_ps(&out[(index + x) * ch], scanline[x]);
    }

    /* vertical pass on blurlightness */
    const int opoffs = -(radius + 1) * roi_out->width;
    const int npoffs = (radius)*roi_out->width;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(ch, npoffs, opoffs, radius, roi_out, scanline_buf, size) \
    shared(out) \
    schedule(static)
#endif
    for(int x = 0; x < roi_out->width; x++)
    {
      __m128 *scanline = scanline_buf + size * dt_get_thread_num();
      __m128 L = _mm_setzero_ps();
      int hits = 0;
      size_t index = (size_t)x - radius * roi_out->width;
      for(int y = -radius; y < roi_out->height; y++)
      {
        int op = y - radius - 1;
        int np = y + radius;

        if(op >= 0)
        {
          L = _mm_sub_ps(L, _mm_load_ps(&out[(index + opoffs) * ch]));
          hits--;
        }
        if(np < roi_out->height)
        {
          L = _mm_add_ps(L, _mm_load_ps(&out[(index + npoffs) * ch]));
          hits++;
        }
        if(y >= 0) scanline[y] = _mm_div_ps(L, _mm_set_ps1(hits));
        index += roi_out->width;
      }

      for(int y = 0; y < roi_out->height; y++)
        _mm_store_ps(&out[((size_t)y * roi_out->width + x) * ch], scanline[y]);
    }
  }

  dt_free_align(scanline_buf);

  const __m128 amount = _mm_set1_ps(data->amount / 100.0);
  const __m128 amount_1 = _mm_set1_ps(1 - (data->amount) / 100.0);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, amount, amount_1, roi_out) \
  shared(in, out, data) \
  schedule(static)
#endif
  for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
  {
    int index = ch * k;
    _mm_store_ps(&out[index], _mm_add_ps(_mm_mul_ps(_mm_load_ps(&in[index]), amount_1),
                                         _mm_mul_ps(MM_CLIP_PS(_mm_load_ps(&out[index])), amount)));
  }
}
#endif

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_soften_data_t *d = (dt_iop_soften_data_t *)piece->data;
  dt_iop_soften_global_data_t *gd = (dt_iop_soften_global_data_t *)self->global_data;

  cl_int err = -999;
  cl_mem dev_tmp = NULL;
  cl_mem dev_m = NULL;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const float brightness = 1.0f / exp2f(-d->brightness);
  const float saturation = d->saturation / 100.0f;
  const float amount = d->amount / 100.0f;

  const float w = piece->iwidth * piece->iscale;
  const float h = piece->iheight * piece->iscale;
  int mrad = sqrt(w * w + h * h) * 0.01f;

  int rad = mrad * (fmin(100.0f, d->size + 1) / 100.0f);
  const int radius = MIN(mrad, ceilf(rad * roi_in->scale / piece->iscale));

  /* sigma-radius correlation to match opencl vs. non-opencl. identified by numerical experiments but
   * unproven. ask me if you need details. ulrich */
  const float sigma = sqrt((radius * (radius + 1) * BOX_ITERATIONS + 2) / 3.0f);
  const int wdh = ceilf(3.0f * sigma);
  const int wd = 2 * wdh + 1;
  const size_t mat_size = sizeof(float) * wd;
  float *mat = malloc(mat_size);
  float *m = mat + wdh;
  float weight = 0.0f;

  // init gaussian kernel
  for(int l = -wdh; l <= wdh; l++) weight += m[l] = expf(-(l * l) / (2.f * sigma * sigma));
  for(int l = -wdh; l <= wdh; l++) m[l] /= weight;

  // for(int l=-wdh; l<=wdh; l++) printf("%.6f ", (double)m[l]);
  // printf("\n");

  int hblocksize;
  dt_opencl_local_buffer_t hlocopt
    = (dt_opencl_local_buffer_t){ .xoffset = 2 * wdh, .xfactor = 1, .yoffset = 0, .yfactor = 1,
                                  .cellsize = 4 * sizeof(float), .overhead = 0,
                                  .sizex = 1 << 16, .sizey = 1 };

  if(dt_opencl_local_buffer_opt(devid, gd->kernel_soften_hblur, &hlocopt))
    hblocksize = hlocopt.sizex;
  else
    hblocksize = 1;

  int vblocksize;
  dt_opencl_local_buffer_t vlocopt
    = (dt_opencl_local_buffer_t){ .xoffset = 1, .xfactor = 1, .yoffset = 2 * wdh, .yfactor = 1,
                                  .cellsize = 4 * sizeof(float), .overhead = 0,
                                  .sizex = 1, .sizey = 1 << 16 };

  if(dt_opencl_local_buffer_opt(devid, gd->kernel_soften_vblur, &vlocopt))
    vblocksize = vlocopt.sizey;
  else
    vblocksize = 1;


  const size_t bwidth = ROUNDUP(width, hblocksize);
  const size_t bheight = ROUNDUP(height, vblocksize);

  size_t sizes[3];
  size_t local[3];

  dev_tmp = dt_opencl_alloc_device(devid, width, height, 4 * sizeof(float));
  if(dev_tmp == NULL) goto error;

  dev_m = dt_opencl_copy_host_to_device_constant(devid, mat_size, mat);
  if(dev_m == NULL) goto error;

  /* overexpose image */
  sizes[0] = ROUNDUPWD(width);
  sizes[1] = ROUNDUPHT(height);
  sizes[2] = 1;
  dt_opencl_set_kernel_arg(devid, gd->kernel_soften_overexposed, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_soften_overexposed, 1, sizeof(cl_mem), (void *)&dev_tmp);
  dt_opencl_set_kernel_arg(devid, gd->kernel_soften_overexposed, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_soften_overexposed, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_soften_overexposed, 4, sizeof(float), (void *)&saturation);
  dt_opencl_set_kernel_arg(devid, gd->kernel_soften_overexposed, 5, sizeof(float), (void *)&brightness);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_soften_overexposed, sizes);
  if(err != CL_SUCCESS) goto error;

  if(rad != 0)
  {
    /* horizontal blur */
    sizes[0] = bwidth;
    sizes[1] = ROUNDUPHT(height);
    sizes[2] = 1;
    local[0] = hblocksize;
    local[1] = 1;
    local[2] = 1;
    dt_opencl_set_kernel_arg(devid, gd->kernel_soften_hblur, 0, sizeof(cl_mem), (void *)&dev_tmp);
    dt_opencl_set_kernel_arg(devid, gd->kernel_soften_hblur, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_soften_hblur, 2, sizeof(cl_mem), (void *)&dev_m);
    dt_opencl_set_kernel_arg(devid, gd->kernel_soften_hblur, 3, sizeof(int), (void *)&wdh);
    dt_opencl_set_kernel_arg(devid, gd->kernel_soften_hblur, 4, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_soften_hblur, 5, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_soften_hblur, 6, sizeof(int), (void *)&hblocksize);
    dt_opencl_set_kernel_arg(devid, gd->kernel_soften_hblur, 7, (hblocksize + 2 * wdh) * 4 * sizeof(float),
                             NULL);
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_soften_hblur, sizes, local);
    if(err != CL_SUCCESS) goto error;


    /* vertical blur */
    sizes[0] = ROUNDUPWD(width);
    sizes[1] = bheight;
    sizes[2] = 1;
    local[0] = 1;
    local[1] = vblocksize;
    local[2] = 1;
    dt_opencl_set_kernel_arg(devid, gd->kernel_soften_vblur, 0, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_soften_vblur, 1, sizeof(cl_mem), (void *)&dev_tmp);
    dt_opencl_set_kernel_arg(devid, gd->kernel_soften_vblur, 2, sizeof(cl_mem), (void *)&dev_m);
    dt_opencl_set_kernel_arg(devid, gd->kernel_soften_vblur, 3, sizeof(int), (void *)&wdh);
    dt_opencl_set_kernel_arg(devid, gd->kernel_soften_vblur, 4, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_soften_vblur, 5, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_soften_vblur, 6, sizeof(int), (void *)&vblocksize);
    dt_opencl_set_kernel_arg(devid, gd->kernel_soften_vblur, 7, (vblocksize + 2 * wdh) * 4 * sizeof(float),
                             NULL);
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_soften_vblur, sizes, local);
    if(err != CL_SUCCESS) goto error;
  }

  /* mixing tmp and in -> out */
  sizes[0] = ROUNDUPWD(width);
  sizes[1] = ROUNDUPHT(height);
  sizes[2] = 1;
  dt_opencl_set_kernel_arg(devid, gd->kernel_soften_mix, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_soften_mix, 1, sizeof(cl_mem), (void *)&dev_tmp);
  dt_opencl_set_kernel_arg(devid, gd->kernel_soften_mix, 2, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_soften_mix, 3, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_soften_mix, 4, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_soften_mix, 5, sizeof(float), (void *)&amount);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_soften_mix, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_tmp);
  free(mat);
  return TRUE;

error:
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_tmp);
  free(mat);
  dt_print(DT_DEBUG_OPENCL, "[opencl_soften] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  dt_iop_soften_data_t *d = (dt_iop_soften_data_t *)piece->data;

  const float w = piece->iwidth * piece->iscale;
  const float h = piece->iheight * piece->iscale;
  int mrad = sqrt(w * w + h * h) * 0.01f;

  int rad = mrad * (fmin(100.0f, d->size + 1) / 100.0f);
  const int radius = MIN(mrad, ceilf(rad * roi_in->scale / piece->iscale));

  /* sigma-radius correlation to match opencl vs. non-opencl. identified by numerical experiments but
   * unproven. ask me if you need details. ulrich */
  const float sigma = sqrt((radius * (radius + 1) * BOX_ITERATIONS + 2) / 3.0f);
  const int wdh = ceilf(3.0f * sigma);

  tiling->factor = 3.0f; // in + out + tmp
  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = wdh;
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 9; // soften.cl, from programs.conf
  dt_iop_soften_global_data_t *gd
      = (dt_iop_soften_global_data_t *)malloc(sizeof(dt_iop_soften_global_data_t));
  module->data = gd;
  gd->kernel_soften_overexposed = dt_opencl_create_kernel(program, "soften_overexposed");
  gd->kernel_soften_hblur = dt_opencl_create_kernel(program, "soften_hblur");
  gd->kernel_soften_vblur = dt_opencl_create_kernel(program, "soften_vblur");
  gd->kernel_soften_mix = dt_opencl_create_kernel(program, "soften_mix");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_soften_global_data_t *gd = (dt_iop_soften_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_soften_overexposed);
  dt_opencl_free_kernel(gd->kernel_soften_hblur);
  dt_opencl_free_kernel(gd->kernel_soften_vblur);
  dt_opencl_free_kernel(gd->kernel_soften_mix);
  free(module->data);
  module->data = NULL;
}


void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_soften_params_t *p = (dt_iop_soften_params_t *)p1;
  dt_iop_soften_data_t *d = (dt_iop_soften_data_t *)piece->data;

  d->size = p->size;
  d->saturation = p->saturation;
  d->brightness = p->brightness;
  d->amount = p->amount;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_soften_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_soften_gui_data_t *g = (dt_iop_soften_gui_data_t *)self->gui_data;
  dt_iop_soften_params_t *p = (dt_iop_soften_params_t *)self->params;
  dt_bauhaus_slider_set(g->size, p->size);
  dt_bauhaus_slider_set(g->saturation, p->saturation);
  dt_bauhaus_slider_set(g->brightness, p->brightness);
  dt_bauhaus_slider_set(g->amount, p->amount);
}


void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_soften_gui_data_t *g = IOP_GUI_ALLOC(soften);

  g->size = dt_bauhaus_slider_from_params(self, N_("size"));
  dt_bauhaus_slider_set_format(g->size, "%.0f%%");
  gtk_widget_set_tooltip_text(g->size, _("the size of blur"));

  g->saturation = dt_bauhaus_slider_from_params(self, N_("saturation"));
  dt_bauhaus_slider_set_format(g->saturation, "%.0f%%");
  gtk_widget_set_tooltip_text(g->saturation, _("the saturation of blur"));

  g->brightness = dt_bauhaus_slider_from_params(self, N_("brightness"));
  dt_bauhaus_slider_set_format(g->brightness, _("%.2f EV"));
  gtk_widget_set_tooltip_text(g->brightness, _("the brightness of blur"));

  g->amount = dt_bauhaus_slider_from_params(self, "amount");
  dt_bauhaus_slider_set_format(g->amount, "%.0f%%");
  gtk_widget_set_tooltip_text(g->amount, _("the mix of effect"));
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
