/*
    This file is part of darktable,
    Copyright (C) 2012-2020 darktable developers.

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


#include <assert.h>
#include <math.h>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif
#include "common/gaussian.h"
#include "common/math.h"
#include "common/opencl.h"

#define BLOCKSIZE (1 << 6)

static void compute_gauss_params(const float sigma, dt_gaussian_order_t order, float *a0, float *a1,
                                 float *a2, float *a3, float *b1, float *b2, float *coefp, float *coefn)
{
  const float alpha = 1.695f / sigma;
  const float ema = expf(-alpha);
  const float ema2 = expf(-2.0f * alpha);
  *b1 = -2.0f * ema;
  *b2 = ema2;
  *a0 = 0.0f;
  *a1 = 0.0f;
  *a2 = 0.0f;
  *a3 = 0.0f;
  *coefp = 0.0f;
  *coefn = 0.0f;

  switch(order)
  {
    default:
    case DT_IOP_GAUSSIAN_ZERO:
    {
      const float k = (1.0f - ema) * (1.0f - ema) / (1.0f + (2.0f * alpha * ema) - ema2);
      *a0 = k;
      *a1 = k * (alpha - 1.0f) * ema;
      *a2 = k * (alpha + 1.0f) * ema;
      *a3 = -k * ema2;
    }
    break;

    case DT_IOP_GAUSSIAN_ONE:
    {
      *a0 = (1.0f - ema) * (1.0f - ema);
      *a1 = 0.0f;
      *a2 = -*a0;
      *a3 = 0.0f;
    }
    break;

    case DT_IOP_GAUSSIAN_TWO:
    {
      const float k = -(ema2 - 1.0f) / (2.0f * alpha * ema);
      float kn = -2.0f * (-1.0f + (3.0f * ema) - (3.0f * ema * ema) + (ema * ema * ema));
      kn /= ((3.0f * ema) + 1.0f + (3.0f * ema * ema) + (ema * ema * ema));
      *a0 = kn;
      *a1 = -kn * (1.0f + (k * alpha)) * ema;
      *a2 = kn * (1.0f - (k * alpha)) * ema;
      *a3 = -kn * ema2;
    }
  }

  *coefp = (*a0 + *a1) / (1.0f + *b1 + *b2);
  *coefn = (*a2 + *a3) / (1.0f + *b1 + *b2);
}

size_t dt_gaussian_memory_use(const int width,    // width of input image
                              const int height,   // height of input image
                              const int channels) // channels per pixel
{
  return sizeof(float) * channels * width * height;
}

#ifdef HAVE_OPENCL
size_t dt_gaussian_memory_use_cl(const int width,    // width of input image
                                 const int height,   // height of input image
                                 const int channels) // channels per pixel
{
  return sizeof(float) * channels * (width + BLOCKSIZE) * (height + BLOCKSIZE) * 2;
}
#endif /* HAVE_OPENCL */

size_t dt_gaussian_singlebuffer_size(const int width,    // width of input image
                                     const int height,   // height of input image
                                     const int channels) // channels per pixel
{
  size_t mem_use;
#ifdef HAVE_OPENCL
  mem_use = sizeof(float) * channels * (width + BLOCKSIZE) * (height + BLOCKSIZE);
#else
  mem_use = sizeof(float) * channels * width * height;
#endif
  return mem_use;
}


dt_gaussian_t *dt_gaussian_init(const int width,    // width of input image
                                const int height,   // height of input image
                                const int channels, // channels per pixel
                                const float *max,   // maximum allowed values per channel for clamping
                                const float *min,   // minimum allowed values per channel for clamping
                                const float sigma,  // gaussian sigma
                                const int order)    // order of gaussian blur
{
  dt_gaussian_t *g = (dt_gaussian_t *)malloc(sizeof(dt_gaussian_t));
  if(!g) return NULL;

  g->width = width;
  g->height = height;
  g->channels = channels;
  g->sigma = sigma;
  g->order = order;
  g->buf = NULL;
  g->max = (float *)calloc(channels, sizeof(float));
  g->min = (float *)calloc(channels, sizeof(float));

  if(!g->min || !g->max) goto error;

  for(int k = 0; k < channels; k++)
  {
    g->max[k] = max[k];
    g->min[k] = min[k];
  }

  g->buf = dt_alloc_align_float((size_t)channels * width * height);
  if(!g->buf) goto error;

  return g;

error:
  dt_free_align(g->buf);
  free(g->max);
  free(g->min);
  free(g);
  return NULL;
}


void dt_gaussian_blur(dt_gaussian_t *g, const float *const in, float *const out)
{

  const int width = g->width;
  const int height = g->height;
  const int ch = MIN(4, g->channels); // just to appease zealous compiler warnings about stack usage

  float a0, a1, a2, a3, b1, b2, coefp, coefn;

  compute_gauss_params(g->sigma, g->order, &a0, &a1, &a2, &a3, &b1, &b2, &coefp, &coefn);

  float *temp = g->buf;

  float *Labmax = g->max;
  float *Labmin = g->min;

// vertical blur column by column
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, width, height, ch) \
  shared(temp, Labmin, Labmax, a0, a1, a2, a3, b1, b2, coefp, coefn) \
  schedule(static)
#endif
  for(int i = 0; i < width; i++)
  {
    dt_aligned_pixel_t xp = {0.0f};
    dt_aligned_pixel_t yb = {0.0f};
    dt_aligned_pixel_t yp = {0.0f};

    // forward filter
    for(int k = 0; k < ch; k++)
    {
      xp[k] = CLAMPF(in[(size_t)i * ch + k], Labmin[k], Labmax[k]);
      yb[k] = xp[k] * coefp;
      yp[k] = yb[k];
    }

    dt_aligned_pixel_t xc = {0.0f};
    dt_aligned_pixel_t yc = {0.0f};
    dt_aligned_pixel_t xn = {0.0f};
    dt_aligned_pixel_t xa = {0.0f};
    dt_aligned_pixel_t yn = {0.0f};
    dt_aligned_pixel_t ya = {0.0f};
    for(int j = 0; j < height; j++)
    {
      size_t offset = ((size_t)j * width + i) * ch;

      for(int k = 0; k < ch; k++)
      {
        xc[k] = CLAMPF(in[offset + k], Labmin[k], Labmax[k]);
        yc[k] = (a0 * xc[k]) + (a1 * xp[k]) - (b1 * yp[k]) - (b2 * yb[k]);

        temp[offset + k] = yc[k];

        xp[k] = xc[k];
        yb[k] = yp[k];
        yp[k] = yc[k];
      }
    }

    // backward filter
    for(int k = 0; k < ch; k++)
    {
      xn[k] = CLAMPF(in[((size_t)(height - 1) * width + i) * ch + k], Labmin[k], Labmax[k]);
      xa[k] = xn[k];
      yn[k] = xn[k] * coefn;
      ya[k] = yn[k];
    }

    for(int j = height - 1; j > -1; j--)
    {
      size_t offset = ((size_t)j * width + i) * ch;

      for(int k = 0; k < ch; k++)
      {
        xc[k] = CLAMPF(in[offset + k], Labmin[k], Labmax[k]);

        yc[k] = (a2 * xn[k]) + (a3 * xa[k]) - (b1 * yn[k]) - (b2 * ya[k]);

        xa[k] = xn[k];
        xn[k] = xc[k];
        ya[k] = yn[k];
        yn[k] = yc[k];

        temp[offset + k] += yc[k];
      }
    }
  }

// horizontal blur line by line
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(out, ch, width, height) \
  shared(temp, Labmin, Labmax, a0, a1, a2, a3, b1, b2, coefp, coefn) \
  schedule(static)
#endif
  for(int j = 0; j < height; j++)
  {
    dt_aligned_pixel_t xp = {0.0f};
    dt_aligned_pixel_t yb = {0.0f};
    dt_aligned_pixel_t yp = {0.0f};

    // forward filter
    for(int k = 0; k < ch; k++)
    {
      xp[k] = CLAMPF(temp[(size_t)j * width * ch + k], Labmin[k], Labmax[k]);
      yb[k] = xp[k] * coefp;
      yp[k] = yb[k];
    }

    dt_aligned_pixel_t xc = {0.0f};
    dt_aligned_pixel_t yc = {0.0f};
    dt_aligned_pixel_t xn = {0.0f};
    dt_aligned_pixel_t xa = {0.0f};
    dt_aligned_pixel_t yn = {0.0f};
    dt_aligned_pixel_t ya = {0.0f};

    for(int i = 0; i < width; i++)
    {
      size_t offset = ((size_t)j * width + i) * ch;

      for(int k = 0; k < ch; k++)
      {
        xc[k] = CLAMPF(temp[offset + k], Labmin[k], Labmax[k]);
        yc[k] = (a0 * xc[k]) + (a1 * xp[k]) - (b1 * yp[k]) - (b2 * yb[k]);

        out[offset + k] = yc[k];

        xp[k] = xc[k];
        yb[k] = yp[k];
        yp[k] = yc[k];
      }
    }

    // backward filter
    for(int k = 0; k < ch; k++)
    {
      xn[k] = CLAMPF(temp[((size_t)(j + 1) * width - 1) * ch + k], Labmin[k], Labmax[k]);
      xa[k] = xn[k];
      yn[k] = xn[k] * coefn;
      ya[k] = yn[k];
    }

    for(int i = width - 1; i > -1; i--)
    {
      size_t offset = ((size_t)j * width + i) * ch;

      for(int k = 0; k < ch; k++)
      {
        xc[k] = CLAMPF(temp[offset + k], Labmin[k], Labmax[k]);

        yc[k] = (a2 * xn[k]) + (a3 * xa[k]) - (b1 * yn[k]) - (b2 * ya[k]);

        xa[k] = xn[k];
        xn[k] = xc[k];
        ya[k] = yn[k];
        yn[k] = yc[k];

        out[offset + k] += yc[k];
      }
    }
  }
}



#if defined(__SSE__)
static void dt_gaussian_blur_4c_sse(dt_gaussian_t *g, const float *const in, float *const out)
{

  const int width = g->width;
  const int height = g->height;
  const int ch = 4;

  assert(g->channels == 4);

  float a0, a1, a2, a3, b1, b2, coefp, coefn;

  compute_gauss_params(g->sigma, g->order, &a0, &a1, &a2, &a3, &b1, &b2, &coefp, &coefn);

  const __m128 Labmax = _mm_set_ps(g->max[3], g->max[2], g->max[1], g->max[0]);
  const __m128 Labmin = _mm_set_ps(g->min[3], g->min[2], g->min[1], g->min[0]);

  float *temp = g->buf;


// vertical blur column by column
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, Labmin, Labmax, width, height, ch) \
  shared(temp, a0, a1, a2, a3, b1, b2, coefp, coefn) \
  schedule(static)
#endif
  for(int i = 0; i < width; i++)
  {
    __m128 xp = _mm_setzero_ps();
    __m128 yb = _mm_setzero_ps();
    __m128 yp = _mm_setzero_ps();
    __m128 xc = _mm_setzero_ps();
    __m128 yc = _mm_setzero_ps();
    __m128 xn = _mm_setzero_ps();
    __m128 xa = _mm_setzero_ps();
    __m128 yn = _mm_setzero_ps();
    __m128 ya = _mm_setzero_ps();

    // forward filter
    xp = MMCLAMPPS(_mm_load_ps(in + i * ch), Labmin, Labmax);
    yb = _mm_mul_ps(_mm_set_ps1(coefp), xp);
    yp = yb;


    for(int j = 0; j < height; j++)
    {
      size_t offset = ((size_t)j * width + i) * ch;

      xc = MMCLAMPPS(_mm_load_ps(in + offset), Labmin, Labmax);


      yc = _mm_add_ps(
          _mm_mul_ps(xc, _mm_set_ps1(a0)),
          _mm_sub_ps(_mm_mul_ps(xp, _mm_set_ps1(a1)),
                     _mm_add_ps(_mm_mul_ps(yp, _mm_set_ps1(b1)), _mm_mul_ps(yb, _mm_set_ps1(b2)))));

      _mm_store_ps(temp + offset, yc);

      xp = xc;
      yb = yp;
      yp = yc;
    }

    // backward filter
    xn = MMCLAMPPS(_mm_load_ps(in + ((size_t)(height - 1) * width + i) * ch), Labmin, Labmax);
    xa = xn;
    yn = _mm_mul_ps(_mm_set_ps1(coefn), xn);
    ya = yn;

    for(int j = height - 1; j > -1; j--)
    {
      size_t offset = ((size_t)j * width + i) * ch;

      xc = MMCLAMPPS(_mm_load_ps(in + offset), Labmin, Labmax);

      yc = _mm_add_ps(
          _mm_mul_ps(xn, _mm_set_ps1(a2)),
          _mm_sub_ps(_mm_mul_ps(xa, _mm_set_ps1(a3)),
                     _mm_add_ps(_mm_mul_ps(yn, _mm_set_ps1(b1)), _mm_mul_ps(ya, _mm_set_ps1(b2)))));


      xa = xn;
      xn = xc;
      ya = yn;
      yn = yc;

      _mm_store_ps(temp + offset, _mm_add_ps(_mm_load_ps(temp + offset), yc));
    }
  }

// horizontal blur line by line
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(out, Labmin, Labmax, width, height, ch) \
  shared(temp, a0, a1, a2, a3, b1, b2, coefp, coefn) \
  schedule(static)
#endif
  for(size_t j = 0; j < height; j++)
  {
    __m128 xp = _mm_setzero_ps();
    __m128 yb = _mm_setzero_ps();
    __m128 yp = _mm_setzero_ps();
    __m128 xc = _mm_setzero_ps();
    __m128 yc = _mm_setzero_ps();
    __m128 xn = _mm_setzero_ps();
    __m128 xa = _mm_setzero_ps();
    __m128 yn = _mm_setzero_ps();
    __m128 ya = _mm_setzero_ps();

    // forward filter
    xp = MMCLAMPPS(_mm_load_ps(temp + j * width * ch), Labmin, Labmax);
    yb = _mm_mul_ps(_mm_set_ps1(coefp), xp);
    yp = yb;


    for(int i = 0; i < width; i++)
    {
      size_t offset = ((size_t)j * width + i) * ch;

      xc = MMCLAMPPS(_mm_load_ps(temp + offset), Labmin, Labmax);

      yc = _mm_add_ps(
          _mm_mul_ps(xc, _mm_set_ps1(a0)),
          _mm_sub_ps(_mm_mul_ps(xp, _mm_set_ps1(a1)),
                     _mm_add_ps(_mm_mul_ps(yp, _mm_set_ps1(b1)), _mm_mul_ps(yb, _mm_set_ps1(b2)))));

      _mm_store_ps(out + offset, yc);

      xp = xc;
      yb = yp;
      yp = yc;
    }

    // backward filter
    xn = MMCLAMPPS(_mm_load_ps(temp + ((size_t)(j + 1) * width - 1) * ch), Labmin, Labmax);
    xa = xn;
    yn = _mm_mul_ps(_mm_set_ps1(coefn), xn);
    ya = yn;


    for(int i = width - 1; i > -1; i--)
    {
      size_t offset = ((size_t)j * width + i) * ch;

      xc = MMCLAMPPS(_mm_load_ps(temp + offset), Labmin, Labmax);

      yc = _mm_add_ps(
          _mm_mul_ps(xn, _mm_set_ps1(a2)),
          _mm_sub_ps(_mm_mul_ps(xa, _mm_set_ps1(a3)),
                     _mm_add_ps(_mm_mul_ps(yn, _mm_set_ps1(b1)), _mm_mul_ps(ya, _mm_set_ps1(b2)))));


      xa = xn;
      xn = xc;
      ya = yn;
      yn = yc;

      _mm_store_ps(out + offset, _mm_add_ps(_mm_load_ps(out + offset), yc));
    }
  }
}
#endif

void dt_gaussian_blur_4c(dt_gaussian_t *g, const float *const in, float *const out)
{
  if(darktable.codepath.OPENMP_SIMD) return dt_gaussian_blur(g, in, out);
#if defined(__SSE__)
  else if(darktable.codepath.SSE2)
    return dt_gaussian_blur_4c_sse(g, in, out);
#endif
  else
    dt_unreachable_codepath();
}

void dt_gaussian_free(dt_gaussian_t *g)
{
  if(!g) return;
  dt_free_align(g->buf);
  free(g->min);
  free(g->max);
  free(g);
}


#ifdef HAVE_OPENCL
dt_gaussian_cl_global_t *dt_gaussian_init_cl_global()
{
  dt_gaussian_cl_global_t *g = (dt_gaussian_cl_global_t *)malloc(sizeof(dt_gaussian_cl_global_t));

  const int program = 6; // gaussian.cl, from programs.conf
  g->kernel_gaussian_column_1c = dt_opencl_create_kernel(program, "gaussian_column_1c");
  g->kernel_gaussian_transpose_1c = dt_opencl_create_kernel(program, "gaussian_transpose_1c");
  g->kernel_gaussian_column_4c = dt_opencl_create_kernel(program, "gaussian_column_4c");
  g->kernel_gaussian_transpose_4c = dt_opencl_create_kernel(program, "gaussian_transpose_4c");
  return g;
}

void dt_gaussian_free_cl(dt_gaussian_cl_t *g)
{
  if(!g) return;
  // be sure we're done with the memory:
  dt_opencl_finish(g->devid);

  free(g->min);
  free(g->max);
  // free device mem
  dt_opencl_release_mem_object(g->dev_temp1);
  dt_opencl_release_mem_object(g->dev_temp2);
  free(g);
}

dt_gaussian_cl_t *dt_gaussian_init_cl(const int devid,
                                      const int width,    // width of input image
                                      const int height,   // height of input image
                                      const int channels, // channels per pixel
                                      const float *max,   // maximum allowed values per channel for clamping
                                      const float *min,   // minimum allowed values per channel for clamping
                                      const float sigma,  // gaussian sigma
                                      const int order)    // order of gaussian blur
{
  assert(channels == 1 || channels == 4);

  if(!(channels == 1 || channels == 4)) return NULL;

  dt_gaussian_cl_t *g = (dt_gaussian_cl_t *)malloc(sizeof(dt_gaussian_cl_t));
  if(!g) return NULL;

  g->global = darktable.opencl->gaussian;
  g->devid = devid;
  g->width = width;
  g->height = height;
  g->channels = channels;
  g->sigma = sigma;
  g->order = order;
  g->dev_temp1 = NULL;
  g->dev_temp2 = NULL;
  g->max = (float *)calloc(channels, sizeof(float));
  g->min = (float *)calloc(channels, sizeof(float));

  if(!g->min || !g->max) goto error;

  for(int k = 0; k < channels; k++)
  {
    g->max[k] = max[k];
    g->min[k] = min[k];
  }

  int kernel_gaussian_transpose = (channels == 1) ? g->global->kernel_gaussian_transpose_1c
                                                  : g->global->kernel_gaussian_transpose_4c;
  int blocksize;

  dt_opencl_local_buffer_t locopt
    = (dt_opencl_local_buffer_t){ .xoffset = 1, .xfactor = 1, .yoffset = 0, .yfactor = 1,
                                  .cellsize = channels * sizeof(float), .overhead = 0,
                                  .sizex = BLOCKSIZE, .sizey = BLOCKSIZE };

  if(dt_opencl_local_buffer_opt(devid, kernel_gaussian_transpose, &locopt))
    blocksize = MIN(locopt.sizex, locopt.sizey);
  else
    blocksize = 1;

  // width and height of intermediate buffers. Need to be multiples of blocksize
  const size_t bwidth = ROUNDUP(width, blocksize);
  const size_t bheight = ROUNDUP(height, blocksize);

  g->blocksize = blocksize;
  g->bwidth = bwidth;
  g->bheight = bheight;

  // get intermediate vector buffers with read-write access
  g->dev_temp1 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * channels * bwidth * bheight);
  if(!g->dev_temp1) goto error;
  g->dev_temp2 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * channels * bwidth * bheight);
  if(!g->dev_temp2) goto error;

  return g;

error:
  free(g->min);
  free(g->max);
  dt_opencl_release_mem_object(g->dev_temp1);
  dt_opencl_release_mem_object(g->dev_temp2);
  g->dev_temp1 = g->dev_temp2 = NULL;
  free(g);
  return NULL;
}


cl_int dt_gaussian_blur_cl(dt_gaussian_cl_t *g, cl_mem dev_in, cl_mem dev_out)
{
  cl_int err = -999;
  const int devid = g->devid;

  const int width = g->width;
  const int height = g->height;
  const int channels = g->channels;
  const size_t bpp = sizeof(float) * channels;
  cl_mem dev_temp1 = g->dev_temp1;
  cl_mem dev_temp2 = g->dev_temp2;

  const int blocksize = g->blocksize;
  const int bwidth = g->bwidth;
  const int bheight = g->bheight;

  dt_aligned_pixel_t Labmax = { 0.0f };
  dt_aligned_pixel_t Labmin = { 0.0f };

  for(int k = 0; k < MIN(channels, 4); k++)
  {
    Labmax[k] = g->max[k];
    Labmin[k] = g->min[k];
  }

  int kernel_gaussian_column = -1;
  int kernel_gaussian_transpose = -1;

  if(channels == 1)
  {
    kernel_gaussian_column = g->global->kernel_gaussian_column_1c;
    kernel_gaussian_transpose = g->global->kernel_gaussian_transpose_1c;
  }
  else if(channels == 4)
  {
    kernel_gaussian_column = g->global->kernel_gaussian_column_4c;
    kernel_gaussian_transpose = g->global->kernel_gaussian_transpose_4c;
  }
  else
    return err;

  size_t origin[] = { 0, 0, 0 };
  size_t region[] = { width, height, 1 };
  size_t local[] = { blocksize, blocksize, 1 };
  size_t sizes[3];

  // compute gaussian parameters
  float a0, a1, a2, a3, b1, b2, coefp, coefn;
  compute_gauss_params(g->sigma, g->order, &a0, &a1, &a2, &a3, &b1, &b2, &coefp, &coefn);

  // copy dev_in to intermediate buffer dev_temp1
  err = dt_opencl_enqueue_copy_image_to_buffer(devid, dev_in, dev_temp1, origin, region, 0);
  if(err != CL_SUCCESS) return err;

  // first blur step: column by column with dev_temp1 -> dev_temp2
  sizes[0] = ROUNDUPDWD(width, devid);
  sizes[1] = 1;
  sizes[2] = 1;
  dt_opencl_set_kernel_args(devid, kernel_gaussian_column, 0, CLARG(dev_temp1), CLARG(dev_temp2),
    CLARG(width), CLARG(height), CLARG(a0), CLARG(a1), CLARG(a2), CLARG(a3), CLARG(b1), CLARG(b2),
    CLARG(coefp), CLARG(coefn), CLARG(Labmax), CLARG(Labmin));
  err = dt_opencl_enqueue_kernel_2d(devid, kernel_gaussian_column, sizes);
  if(err != CL_SUCCESS) return err;

  // intermediate step: transpose dev_temp2 -> dev_temp1
  sizes[0] = bwidth;
  sizes[1] = bheight;
  sizes[2] = 1;
  dt_opencl_set_kernel_args(devid, kernel_gaussian_transpose, 0, CLARG(dev_temp2), CLARG(dev_temp1),
    CLARG(width), CLARG(height), CLARG(blocksize), CLLOCAL(bpp * blocksize * (blocksize + 1)));
  err = dt_opencl_enqueue_kernel_2d_with_local(devid, kernel_gaussian_transpose, sizes, local);
  if(err != CL_SUCCESS) return err;


  // second blur step: column by column of transposed image with dev_temp1 -> dev_temp2 (!! height <-> width
  // !!)
  sizes[0] = ROUNDUPDHT(height, devid);
  sizes[1] = 1;
  sizes[2] = 1;
  dt_opencl_set_kernel_args(devid, kernel_gaussian_column, 0, CLARG(dev_temp1), CLARG(dev_temp2),
    CLARG(height), CLARG(width), CLARG(a0), CLARG(a1), CLARG(a2), CLARG(a3), CLARG(b1), CLARG(b2),
    CLARG(coefp), CLARG(coefn), CLARG(Labmax), CLARG(Labmin));
  err = dt_opencl_enqueue_kernel_2d(devid, kernel_gaussian_column, sizes);
  if(err != CL_SUCCESS) return err;


  // transpose back dev_temp2 -> dev_temp1
  sizes[0] = bheight;
  sizes[1] = bwidth;
  sizes[2] = 1;
  dt_opencl_set_kernel_args(devid, kernel_gaussian_transpose, 0, CLARG(dev_temp2), CLARG(dev_temp1),
    CLARG(height), CLARG(width), CLARG(blocksize), CLLOCAL(bpp * blocksize * (blocksize + 1)));
  err = dt_opencl_enqueue_kernel_2d_with_local(devid, kernel_gaussian_transpose, sizes, local);
  if(err != CL_SUCCESS) return err;

  // finally produce output in dev_out
  err = dt_opencl_enqueue_copy_buffer_to_image(devid, dev_temp1, dev_out, 0, origin, region);
  if(err != CL_SUCCESS) return err;

  return CL_SUCCESS;
}


void dt_gaussian_free_cl_global(dt_gaussian_cl_global_t *g)
{
  if(!g) return;
  // destroy kernels
  dt_opencl_free_kernel(g->kernel_gaussian_column_1c);
  dt_opencl_free_kernel(g->kernel_gaussian_transpose_1c);
  dt_opencl_free_kernel(g->kernel_gaussian_column_4c);
  dt_opencl_free_kernel(g->kernel_gaussian_transpose_4c);
  free(g);
}

#endif
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

