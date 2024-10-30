/*
    This file is part of darktable,
    Copyright (C) 2012-2024 darktable developers.

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
#include "common/gaussian.h"
#include "common/math.h"
#include "common/opencl.h"
#include "common/imagebuf.h"

#define BLOCKSIZE (1 << 6)

static void _compute_gauss_params(const float sigma,
                                  dt_gaussian_order_t order,
                                  float *a0,
                                  float *a1,
                                  float *a2,
                                  float *a3,
                                  float *b1,
                                  float *b2,
                                  float *coefp,
                                  float *coefn)
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

static void _calc_9x9_gauss_coeffs(float *coeffs, const float sigma)
{
  float kernel[9][9];
  const float range = 4.5f * 4.5f;
  /* ignore locations with a radius > 4.5 or marked wit X
      X X 0 0 0 0 0 X X
      X 0 0 0 0 0 0 0 X
      0 0 0 0 0 0 0 0 0
      0 0 0 0 0 0 0 0 0
      0 0 0 0 0 0 0 0 0
      0 0 0 0 0 0 0 0 0
      0 0 0 0 0 0 0 0 0
      X 0 0 0 0 0 0 0 X
      X X 0 0 0 0 0 X X
  */
  const float temp = -2.0f * sigma * sigma;
  float sum = 0.0;
  for(int k = -4; k < 5; k++)
  {
    for(int j = -4; j < 5; j++)
    {
      const float rad = (float)(k*k + j*j);
      if(rad <= range)
      {
        kernel[k + 4][j + 4] = expf(rad / temp);
        sum += kernel[k + 4][j + 4];
      }
      else
        kernel[k + 4][j + 4] = 0.0f;
    }
  }

  for(int k = 0; k < 5; k++)
    for(int j = 0; j < 5; j++)
      coeffs[5*k+j] = kernel[k+4][j+4] / sum;
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
  dt_gaussian_t *g = malloc(sizeof(dt_gaussian_t));
  if(!g) return NULL;

  g->width = width;
  g->height = height;
  g->channels = channels;
  g->sigma = sigma;
  g->order = order;
  g->buf = NULL;
  g->max = calloc(channels, sizeof(float));
  g->min = calloc(channels, sizeof(float));

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

  _compute_gauss_params(g->sigma, g->order, &a0, &a1, &a2, &a3, &b1, &b2, &coefp, &coefn);

  float *temp = g->buf;

  float *Labmax = g->max;
  float *Labmin = g->min;

// vertical blur column by column
  DT_OMP_FOR()
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
  DT_OMP_FOR()
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

void dt_gaussian_blur_4c(dt_gaussian_t *g, const float *const in, float *const out)
{
  assert(g->channels == 4);
  const size_t width = g->width;
  const size_t height = g->height;

  float a0, a1, a2, a3, b1, b2, coefp, coefn;

  _compute_gauss_params(g->sigma, g->order, &a0, &a1, &a2, &a3, &b1, &b2, &coefp, &coefn);

  float *const temp = g->buf;

  dt_aligned_pixel_t Labmin, Labmax;
  copy_pixel(Labmin, g->min);
  copy_pixel(Labmax, g->max);

// vertical blur column by column
  DT_OMP_FOR()
  for(size_t i = 0; i < width; i++)
  {
    // forward filter
    dt_aligned_pixel_t xp;
    dt_aligned_pixel_t yb;
    dt_aligned_pixel_t yp;
    for_four_channels(k)
    {
      xp[k] = CLAMPF(in[4*i + k], Labmin[k], Labmax[k]);
      yb[k] = xp[k] * coefp;
      yp[k] = yb[k];
    }

    dt_aligned_pixel_t xc;
    dt_aligned_pixel_t xn;
    dt_aligned_pixel_t xa;
    for(size_t j = 0; j < height; j++)
    {
      size_t offset = 4 * (j * width + i);

      dt_aligned_pixel_t yc;
      for_four_channels(k)
      {
        xc[k] = CLAMPF(in[offset + k], Labmin[k], Labmax[k]);
        yc[k] = (a0 * xc[k]) + (a1 * xp[k]) - (b1 * yp[k]) - (b2 * yb[k]);

        xp[k] = xc[k];
        yb[k] = yp[k];
        yp[k] = yc[k];
      }
      copy_pixel(temp + offset, yc);
    }

    // backward filter
    dt_aligned_pixel_t yn;
    dt_aligned_pixel_t ya;
    for_four_channels(k)
    {
      xn[k] = CLAMPF(in[4*((height - 1) * width + i) + k], Labmin[k], Labmax[k]);
      xa[k] = xn[k];
      yn[k] = xn[k] * coefn;
      ya[k] = yn[k];
    }

    for(size_t j = height; j > 0; j--)
    {
      size_t offset = 4 * ((j-1) * width + i);

      dt_aligned_pixel_t yc;
      for_four_channels(k)
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
  DT_OMP_FOR()
  for(size_t j = 0; j < height; j++)
  {
    // forward filter
    dt_aligned_pixel_t xp;
    dt_aligned_pixel_t yb;
    dt_aligned_pixel_t yp;
    dt_aligned_pixel_t xc;
    for_four_channels(k)
    {
      xp[k] = CLAMPF(temp[4*(j * width) + k], Labmin[k], Labmax[k]);
      yb[k] = xp[k] * coefp;
      yp[k] = yb[k];
    }

    for(size_t i = 0; i < width; i++)
    {
      size_t offset = 4 * (j * width + i);
      dt_aligned_pixel_t yc;

      for_four_channels(k)
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
    dt_aligned_pixel_t xn;
    dt_aligned_pixel_t xa;
    dt_aligned_pixel_t ya;
    dt_aligned_pixel_t yn;
    for_four_channels(k)
    {
      xn[k] = CLAMPF(temp[4*((j + 1) * width - 1) + k], Labmin[k], Labmax[k]);
      xa[k] = xn[k];
      yn[k] = xn[k] * coefn;
      ya[k] = yn[k];
    }

    for(int i = width - 1; i > -1; i--)
    {
      size_t offset = 4 * (j * width + i);

      dt_aligned_pixel_t yc;
      for_four_channels(k)
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

void dt_gaussian_free(dt_gaussian_t *g)
{
  if(!g) return;
  dt_free_align(g->buf);
  free(g->min);
  free(g->max);
  free(g);
}

DT_OMP_DECLARE_SIMD(aligned(in, out:64))
static void _fast_9x9_kernel_1(float *in,
                               float *out,
                               const int width,
                               const int height,
                               const float sigma,
                               const float min,
                               const float max)
{
  float kern[25];
  _calc_9x9_gauss_coeffs(kern, sigma);
  const int w1 = width;
  const int w2 = 2 * width;
  const int w3 = 3 * width;
  const int w4 = 4 * width;

  #define h0 0
  #define h1 1
  #define h2 2
  #define h3 3
  #define h4 4

  DT_OMP_FOR(collapse(2))
  for(int row = 0; row < height; row++)
  {
    for(int col = 0; col < width; col++)
    {
      const size_t i = (size_t)row * width + col;

      float val = 0.0f;
      if(col >= 4 && row >= 4 && col < width - 4 && row < height - 4)
      {
        val =
            kern[10+4] * (in[i - w4 -h2]  + in[i - w4 +h2]  + in[i - w2 -h4]  + in[i - w2 +h4] + in[i + w2 -h4] + in[i + w2 +h4] + in[i + w4 -h2] + in[i + w4 +h2]) +
            kern[5 +4] * (in[i - w4 -h1]  + in[i - w4 +h1]  + in[i - w1 -h4]  + in[i - w1 +h4] + in[i + w1 -h4] + in[i + w1 +h4] + in[i + w4 -h1] + in[i + w4 +h1]) +
            kern[4]    * (in[i - w4 +h0]  + in[i      -h4]  + in[i      +h4]  + in[i + w4 +h0]) +
            kern[15+3] * (in[i - w3 -h3]  + in[i - w3 +h3]  + in[i + w3 -h3]  + in[i + w3 +h3]) +
            kern[10+3] * (in[i - w3 -h2]  + in[i - w3 +h2]  + in[i - w2 -h3]  + in[i - w2 +h3] + in[i + w2 -h3] + in[i + w2 +h3] + in[i + w3 -h2] + in[i + w3 +h2]) +
            kern[ 5+3] * (in[i - w3 -h1]  + in[i - w3 +h1]  + in[i - w1 -h3]  + in[i - w1 +h3] + in[i + w1 -h3] + in[i + w1 +h3] + in[i + w3 -h1] + in[i + w3 +h1]) +
            kern[   3] * (in[i - w3 +h0]  + in[i      -h3]  + in[i      +h3]  + in[i + w3 +h0]) +
            kern[10+2] * (in[i - w2 -h2]  + in[i - w2 +h2]  + in[i + w2 -h2]  + in[i + w2 +h2]) +
            kern[ 5+2] * (in[i - w2 -h1]  + in[i - w2 +h1]  + in[i - w1 -h2]  + in[i - w1 +h2] + in[i + w1 -h2] + in[i + w1 +h2] + in[i + w2 -h1] + in[i + w2 +h1]) +
            kern[   2] * (in[i - w2 +h0]  + in[i      -h2]  + in[i      +h2]  + in[i + w2 +h0]) +
            kern[ 5+1] * (in[i - w1 -h1]  + in[i - w1 +h1]  + in[i + w1 -h1]  + in[i + w1 +h1]) +
            kern[   1] * (in[i - w1 +h0]  + in[i      -h1]  + in[i      +h1]  + in[i + w1 +h0]) +
            kern[   0] * (in[i      +h0]);
      }
      else
      {
        for(int ir = -4; ir <= 4; ir++)
        {
          const int irow = row+ir;
          if(irow >= 0 && irow < height)
          {
            for(int ic = -4; ic <= 4; ic++)
            {
              const int icol = col+ic;
              if(icol >=0 && icol < width)
                val += kern[5 * ABS(ir) + ABS(ic)] * in[(size_t)(irow * width + icol)];
            }
          }
        }
      }

      out[i] = CLAMPF(val, min, max);
    }
  }
  #undef h0
  #undef h1
  #undef h2
  #undef h3
  #undef h4
}


DT_OMP_DECLARE_SIMD(aligned(in, out:64))
static void _fast_9x9_kernel_2(float *in,
                               float *out,
                               const int width,
                               const int height,
                               const float sigma,
                               const float min,
                               const float max)
{
  float kern[25];
  _calc_9x9_gauss_coeffs(kern, sigma);

  const int w1 = 2 * width;
  const int w2 = 4 * width;
  const int w3 = 6 * width;
  const int w4 = 8 * width;

  #define h0 c
  #define h1 2+c
  #define h2 4+c
  #define h3 6+c
  #define h4 8+c

  DT_OMP_FOR(collapse(2))
  for(int row = 0; row < height; row++)
  {
    for(int col = 0; col < width; col++)
    {
      const size_t i = (size_t)2 * (row * width + col);
      dt_aligned_pixel_t val = { 0.0f, 0.0f, 0.0f, 0.0f };

      if(col >= 4 && row >= 4 && col < width - 4 && row < height - 4)
      {
        for(int c = 0; c < 2; c++)
          val[c] =
            kern[10+4] * (in[i - w4 -h2]  + in[i - w4 +h2]  + in[i - w2 -h4]  + in[i - w2 +h4] + in[i + w2 -h4] + in[i + w2 +h4] + in[i + w4 -h2] + in[i + w4 +h2]) +
            kern[5 +4] * (in[i - w4 -h1]  + in[i - w4 +h1]  + in[i - w1 -h4]  + in[i - w1 +h4] + in[i + w1 -h4] + in[i + w1 +h4] + in[i + w4 -h1] + in[i + w4 +h1]) +
            kern[4]    * (in[i - w4 +h0]  + in[i      -h4]  + in[i      +h4]  + in[i + w4 +h0]) +
            kern[15+3] * (in[i - w3 -h3]  + in[i - w3 +h3]  + in[i + w3 -h3]  + in[i + w3 +h3]) +
            kern[10+3] * (in[i - w3 -h2]  + in[i - w3 +h2]  + in[i - w2 -h3]  + in[i - w2 +h3] + in[i + w2 -h3] + in[i + w2 +h3] + in[i + w3 -h2] + in[i + w3 +h2]) +
            kern[ 5+3] * (in[i - w3 -h1]  + in[i - w3 +h1]  + in[i - w1 -h3]  + in[i - w1 +h3] + in[i + w1 -h3] + in[i + w1 +h3] + in[i + w3 -h1] + in[i + w3 +h1]) +
            kern[   3] * (in[i - w3 +h0]  + in[i      -h3]  + in[i      +h3]  + in[i + w3 +h0]) +
            kern[10+2] * (in[i - w2 -h2]  + in[i - w2 +h2]  + in[i + w2 -h2]  + in[i + w2 +h2]) +
            kern[ 5+2] * (in[i - w2 -h1]  + in[i - w2 +h1]  + in[i - w1 -h2]  + in[i - w1 +h2] + in[i + w1 -h2] + in[i + w1 +h2] + in[i + w2 -h1] + in[i + w2 +h1]) +
            kern[   2] * (in[i - w2 +h0]  + in[i      -h2]  + in[i      +h2]  + in[i + w2 +h0]) +
            kern[ 5+1] * (in[i - w1 -h1]  + in[i - w1 +h1]  + in[i + w1 -h1]  + in[i + w1 +h1]) +
            kern[   1] * (in[i - w1 +h0]  + in[i      -h1]  + in[i      +h1]  + in[i + w1 +h0]) +
            kern[   0] * (in[i      +h0]);
      }
      else
      {
        for(int ir = -4; ir <= 4; ir++)
        {
          const int irow = row+ir;
          if(irow >= 0 && irow < height)
          {
            for(int ic = -4; ic <= 4; ic++)
            {
              const int icol = col+ic;
              if(icol >=0 && icol < width)
              {
                const float coeff = kern[5 * ABS(ir) + ABS(ic)];
                for(int c = 0; c < 2; c++)
                  val[c] += coeff * in[(size_t)2 * (irow * width + icol) + c];
              }
            }
          }
        }
      }

      for(int c = 0; c < 2; c++)
        out[i+c] = CLAMPF(val[c], min, max);
    }
  }
  #undef h0
  #undef h1
  #undef h2
  #undef h3
  #undef h4
}

DT_OMP_DECLARE_SIMD(aligned(in, out:64))
static void _fast_9x9_kernel_4(float *in,
                               float *out,
                               const int width,
                               const int height,
                               const float sigma,
                               const float min,
                               const float max)
{
  float kern[25];
  _calc_9x9_gauss_coeffs(kern, sigma);

  const int w1 = 4 * width;
  const int w2 = 8 * width;
  const int w3 = 12 * width;
  const int w4 = 16 * width;

  #define h0 c
  #define h1 4+c
  #define h2 8+c
  #define h3 12+c
  #define h4 16+c

  DT_OMP_FOR(collapse(2))
  for(int row = 0; row < height; row++)
  {
    for(int col = 0; col < width; col++)
    {
      const size_t i = (size_t)4 * (row * width + col);
      dt_aligned_pixel_t val = { 0.0f, 0.0f, 0.0f, 0.0f };

      if(col >= 4 && row >= 4 && col < width - 4 && row < height - 4)
      {
        for(int c = 0; c < 4; c++)
          val[c] =
            kern[10+4] * (in[i - w4 -h2]  + in[i - w4 +h2]  + in[i - w2 -h4]  + in[i - w2 +h4] + in[i + w2 -h4] + in[i + w2 +h4] + in[i + w4 -h2] + in[i + w4 +h2]) +
            kern[5 +4] * (in[i - w4 -h1]  + in[i - w4 +h1]  + in[i - w1 -h4]  + in[i - w1 +h4] + in[i + w1 -h4] + in[i + w1 +h4] + in[i + w4 -h1] + in[i + w4 +h1]) +
            kern[4]    * (in[i - w4 +h0]  + in[i      -h4]  + in[i      +h4]  + in[i + w4 +h0]) +
            kern[15+3] * (in[i - w3 -h3]  + in[i - w3 +h3]  + in[i + w3 -h3]  + in[i + w3 +h3]) +
            kern[10+3] * (in[i - w3 -h2]  + in[i - w3 +h2]  + in[i - w2 -h3]  + in[i - w2 +h3] + in[i + w2 -h3] + in[i + w2 +h3] + in[i + w3 -h2] + in[i + w3 +h2]) +
            kern[ 5+3] * (in[i - w3 -h1]  + in[i - w3 +h1]  + in[i - w1 -h3]  + in[i - w1 +h3] + in[i + w1 -h3] + in[i + w1 +h3] + in[i + w3 -h1] + in[i + w3 +h1]) +
            kern[   3] * (in[i - w3 +h0]  + in[i      -h3]  + in[i      +h3]  + in[i + w3 +h0]) +
            kern[10+2] * (in[i - w2 -h2]  + in[i - w2 +h2]  + in[i + w2 -h2]  + in[i + w2 +h2]) +
            kern[ 5+2] * (in[i - w2 -h1]  + in[i - w2 +h1]  + in[i - w1 -h2]  + in[i - w1 +h2] + in[i + w1 -h2] + in[i + w1 +h2] + in[i + w2 -h1] + in[i + w2 +h1]) +
            kern[   2] * (in[i - w2 +h0]  + in[i      -h2]  + in[i      +h2]  + in[i + w2 +h0]) +
            kern[ 5+1] * (in[i - w1 -h1]  + in[i - w1 +h1]  + in[i + w1 -h1]  + in[i + w1 +h1]) +
            kern[   1] * (in[i - w1 +h0]  + in[i      -h1]  + in[i      +h1]  + in[i + w1 +h0]) +
            kern[   0] * (in[i      +h0]);
      }
      else
      {
        for(int ir = -4; ir <= 4; ir++)
        {
          const int irow = row+ir;
          if(irow >= 0 && irow < height)
          {
            for(int ic = -4; ic <= 4; ic++)
            {
              const int icol = col+ic;
              if(icol >=0 && icol < width)
              {
                const float coeff = kern[5 * ABS(ir) + ABS(ic)];
                for(int c = 0; c < 4; c++)
                  val[c] += coeff * in[4*(irow * width + icol)+c];
              }
            }
          }
        }
      }

      for(int c = 0; c < 4; c++)
        out[i+c] = CLAMPF(val[c], min, max);
    }
  }
  #undef h0
  #undef h1
  #undef h2
  #undef h3
  #undef h4
}

void dt_gaussian_fast_blur(float *in,
                           float *out,
                           const int width,
                           const int height,
                           const float sigma,
                           const float min,
                           const float max,
                           const int ch)
{
  float *tmpout = out;
  const gboolean inplace = (in == out);

  const size_t bsize = (size_t)ch * width * height;
  if(inplace) tmpout = dt_alloc_align_float(bsize);

  if(ch == 1)       _fast_9x9_kernel_1(in, tmpout, width, height, sigma, min, max);
  else if(ch == 2)  _fast_9x9_kernel_2(in, tmpout, width, height, sigma, min, max);
  else if(ch == 4)  _fast_9x9_kernel_4(in, tmpout, width, height, sigma, min, max);

  if(inplace)
  {
    dt_iop_image_copy(out, tmpout, bsize);
    dt_free_align(tmpout);
  }
}

#ifdef HAVE_OPENCL
dt_gaussian_cl_global_t *dt_gaussian_init_cl_global()
{
  dt_gaussian_cl_global_t *g = malloc(sizeof(dt_gaussian_cl_global_t));

  const int program = 6; // gaussian.cl, from programs.conf
  g->kernel_gaussian_column_1c = dt_opencl_create_kernel(program, "gaussian_column_1c");
  g->kernel_gaussian_transpose_1c = dt_opencl_create_kernel(program, "gaussian_transpose_1c");
  g->kernel_gaussian_column_2c = dt_opencl_create_kernel(program, "gaussian_column_2c");
  g->kernel_gaussian_transpose_2c = dt_opencl_create_kernel(program, "gaussian_transpose_2c");
  g->kernel_gaussian_column_4c = dt_opencl_create_kernel(program, "gaussian_column_4c");
  g->kernel_gaussian_transpose_4c = dt_opencl_create_kernel(program, "gaussian_transpose_4c");
  g->kernel_gaussian_9x9 = dt_opencl_create_kernel(program, "gaussian_kernel_9x9");
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
  assert(channels == 1 || channels == 2 || channels == 4);

  if(!(channels == 1 || channels == 2 || channels == 4)) return NULL;

  dt_gaussian_cl_t *g = malloc(sizeof(dt_gaussian_cl_t));
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
  g->max = calloc(channels, sizeof(float));
  g->min = calloc(channels, sizeof(float));

  if(!g->min || !g->max) goto error;

  for(int k = 0; k < channels; k++)
  {
    g->max[k] = max[k];
    g->min[k] = min[k];
  }

  int kernel_gaussian_transpose = (channels == 1) ? g->global->kernel_gaussian_transpose_1c
                               : ((channels == 2) ? g->global->kernel_gaussian_transpose_2c
                                                  : g->global->kernel_gaussian_transpose_4c);
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
  cl_int err = DT_OPENCL_DEFAULT_ERROR;
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
  else if(channels == 2)
  {
    kernel_gaussian_column = g->global->kernel_gaussian_column_2c;
    kernel_gaussian_transpose = g->global->kernel_gaussian_transpose_2c;
  }
  else if(channels == 4)
  {
    kernel_gaussian_column = g->global->kernel_gaussian_column_4c;
    kernel_gaussian_transpose = g->global->kernel_gaussian_transpose_4c;
  }
  else
    return DT_OPENCL_PROCESS_CL;

  size_t origin[] = { 0, 0, 0 };
  size_t region[] = { width, height, 1 };
  size_t local[] = { blocksize, blocksize, 1 };
  size_t sizes[3];

  // compute gaussian parameters
  float a0, a1, a2, a3, b1, b2, coefp, coefn;
  _compute_gauss_params(g->sigma, g->order, &a0, &a1, &a2, &a3, &b1, &b2, &coefp, &coefn);

  // copy dev_in to intermediate buffer dev_temp1
  err = dt_opencl_enqueue_copy_image_to_buffer(devid, dev_in, dev_temp1, origin, region, 0);
  if(err != CL_SUCCESS) return err;

  // first blur step: column by column with dev_temp1 -> dev_temp2
  err = dt_opencl_enqueue_kernel_1d_args(devid, kernel_gaussian_column, width,
          CLARG(dev_temp1), CLARG(dev_temp2),
          CLARG(width), CLARG(height), CLARG(a0), CLARG(a1), CLARG(a2), CLARG(a3), CLARG(b1), CLARG(b2),
          CLARG(coefp), CLARG(coefn), CLFLARRAY(channels, Labmax), CLFLARRAY(channels, Labmin));
  if(err != CL_SUCCESS)
    return err;

  // intermediate step: transpose dev_temp2 -> dev_temp1
  sizes[0] = bwidth;
  sizes[1] = bheight;
  sizes[2] = 1;
  dt_opencl_set_kernel_args(devid, kernel_gaussian_transpose, 0, CLARG(dev_temp2), CLARG(dev_temp1),
    CLARG(width), CLARG(height), CLARG(blocksize), CLLOCAL(bpp * blocksize * (blocksize + 1)));
  err = dt_opencl_enqueue_kernel_2d_with_local(devid, kernel_gaussian_transpose, sizes, local);
  if(err != CL_SUCCESS)
    return err;

  // second blur step: column by column of transposed image with dev_temp1 -> dev_temp2 (!! height <-> width
  // !!)
  err = dt_opencl_enqueue_kernel_1d_args(devid, kernel_gaussian_column, height,
          CLARG(dev_temp1), CLARG(dev_temp2),
          CLARG(height), CLARG(width), CLARG(a0), CLARG(a1), CLARG(a2), CLARG(a3), CLARG(b1), CLARG(b2),
          CLARG(coefp), CLARG(coefn), CLFLARRAY(channels, Labmax), CLFLARRAY(channels, Labmin));
  if(err != CL_SUCCESS)
     return err;

  // transpose back dev_temp2 -> dev_temp1
  sizes[0] = bheight;
  sizes[1] = bwidth;
  sizes[2] = 1;
  dt_opencl_set_kernel_args(devid, kernel_gaussian_transpose, 0, CLARG(dev_temp2), CLARG(dev_temp1),
    CLARG(height), CLARG(width), CLARG(blocksize), CLLOCAL(bpp * blocksize * (blocksize + 1)));
  err = dt_opencl_enqueue_kernel_2d_with_local(devid, kernel_gaussian_transpose, sizes, local);
  if(err != CL_SUCCESS)
    return err;

  // finally produce output in dev_out
  return dt_opencl_enqueue_copy_buffer_to_image(devid, dev_temp1, dev_out, 0, origin, region);
}

cl_int dt_gaussian_blur_cl_buffer(dt_gaussian_cl_t *g, cl_mem dev_in, cl_mem dev_out)
{
  cl_int err = DT_OPENCL_DEFAULT_ERROR;
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

  dt_aligned_pixel_t Labmax = { 0.0f, 0.0f, 0.0f, 0.0f };
  dt_aligned_pixel_t Labmin = { 0.0f, 0.0f, 0.0f, 0.0f };

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
  else if(channels == 2)
  {
    kernel_gaussian_column = g->global->kernel_gaussian_column_2c;
    kernel_gaussian_transpose = g->global->kernel_gaussian_transpose_2c;
  }
  else if(channels == 4)
  {
    kernel_gaussian_column = g->global->kernel_gaussian_column_4c;
    kernel_gaussian_transpose = g->global->kernel_gaussian_transpose_4c;
  }
  else
    return  DT_OPENCL_PROCESS_CL;

  size_t local[] = { blocksize, blocksize, 1 };
  size_t sizes[3];

  // compute gaussian parameters
  float a0, a1, a2, a3, b1, b2, coefp, coefn;
  _compute_gauss_params(g->sigma, g->order, &a0, &a1, &a2, &a3, &b1, &b2, &coefp, &coefn);

  // first blur step: column by column with dev_in -> dev_temp2
  err = dt_opencl_enqueue_kernel_1d_args(devid, kernel_gaussian_column, width,
          CLARG(dev_in), CLARG(dev_temp2),
          CLARG(width), CLARG(height), CLARG(a0), CLARG(a1), CLARG(a2), CLARG(a3), CLARG(b1), CLARG(b2),
          CLARG(coefp), CLARG(coefn), CLFLARRAY(channels, Labmax), CLFLARRAY(channels, Labmin));
  if(err != CL_SUCCESS)
    return err;

  // intermediate step: transpose dev_temp2 -> dev_temp1
  sizes[0] = bwidth;
  sizes[1] = bheight;
  sizes[2] = 1;
  dt_opencl_set_kernel_args(devid, kernel_gaussian_transpose, 0, CLARG(dev_temp2), CLARG(dev_temp1),
    CLARG(width), CLARG(height), CLARG(blocksize), CLLOCAL(bpp * blocksize * (blocksize + 1)));
  err = dt_opencl_enqueue_kernel_2d_with_local(devid, kernel_gaussian_transpose, sizes, local);
  if(err != CL_SUCCESS)
    return err;

  // second blur step: column by column of transposed image with dev_temp1 -> dev_temp2 (!! height <-> width !!)
  err = dt_opencl_enqueue_kernel_1d_args(devid, kernel_gaussian_column, height,
          CLARG(dev_temp1), CLARG(dev_temp2),
          CLARG(height), CLARG(width), CLARG(a0), CLARG(a1), CLARG(a2), CLARG(a3), CLARG(b1), CLARG(b2),
          CLARG(coefp), CLARG(coefn), CLFLARRAY(channels, Labmax), CLFLARRAY(channels, Labmin));
  if(err != CL_SUCCESS)
    return err;

  // transpose back dev_temp2 -> dev_out
  // we can do so without out-of-data problems as the writing is limited to width&height of
  // destination buffer
  sizes[0] = bheight;
  sizes[1] = bwidth;
  sizes[2] = 1;
  dt_opencl_set_kernel_args(devid, kernel_gaussian_transpose, 0, CLARG(dev_temp2), CLARG(dev_out),
    CLARG(height), CLARG(width), CLARG(blocksize), CLLOCAL(bpp * blocksize * (blocksize + 1)));
  return dt_opencl_enqueue_kernel_2d_with_local(devid, kernel_gaussian_transpose, sizes, local);
}

cl_int dt_gaussian_fast_blur_cl_buffer(const int devid,
                                       cl_mem dev_in,
                                       cl_mem dev_out,
                                       const int width,
                                       const int height,
                                       const float sigma,
                                       const int ch,
                                       const float min,
                                       const float max)
{
  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
  dt_gaussian_cl_global_t *global = darktable.opencl->gaussian;

  const gboolean inplace = (dev_in == dev_out);
  const size_t bsize = (size_t)ch * width * height * sizeof(float);
  cl_mem tmp_out = dev_out;
  cl_mem kern_cl = NULL;

  if(inplace)
  {
    tmp_out = dt_opencl_alloc_device_buffer(devid, bsize);
    if(tmp_out == NULL) goto error;
  }

  float kern[25];
  _calc_9x9_gauss_coeffs(kern, sigma);
  kern_cl = dt_opencl_copy_host_to_device_constant(devid, 25 * sizeof(float), kern);
  if(kern_cl == NULL) goto error;

  err = dt_opencl_enqueue_kernel_2d_args(devid, global->kernel_gaussian_9x9, width, height,
    CLARG(dev_in), CLARG(tmp_out), CLARG(width), CLARG(height), CLARG(ch), CLARG(kern_cl),
    CLARG(min), CLARG(max));
  if(err != CL_SUCCESS) goto error;

  if(inplace)
    err = dt_opencl_enqueue_copy_buffer_to_buffer(devid, tmp_out, dev_out, 0, 0, bsize);

error:
  dt_opencl_release_mem_object(kern_cl);
  if(inplace) dt_opencl_release_mem_object(tmp_out);

  return err;
}

void dt_gaussian_free_cl_global(dt_gaussian_cl_global_t *g)
{
  if(!g) return;
  // destroy kernels
  dt_opencl_free_kernel(g->kernel_gaussian_column_1c);
  dt_opencl_free_kernel(g->kernel_gaussian_transpose_1c);
  dt_opencl_free_kernel(g->kernel_gaussian_column_2c);
  dt_opencl_free_kernel(g->kernel_gaussian_transpose_2c);
  dt_opencl_free_kernel(g->kernel_gaussian_column_4c);
  dt_opencl_free_kernel(g->kernel_gaussian_transpose_4c);
  dt_opencl_free_kernel(g->kernel_gaussian_9x9);
  free(g);
}

#endif
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

