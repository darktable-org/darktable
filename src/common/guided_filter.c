/*
    This file is part of darktable,
    copyright (c) 2018 Heiko Bauke.

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


    Implementation of the guided image filter as described in

    "Guided Image Filtering" by Kaiming He, Jian Sun, and Xiaoou Tang in
    K. Daniilidis, P. Maragos, N. Paragios (Eds.): ECCV 2010, Part I,
    LNCS 6311, pp. 1-14, 2010. Springer-Verlag Berlin Heidelberg 2010

    "Guided Image Filtering" by Kaiming He, Jian Sun, and Xiaoou Tang in
    IEEE Transactions on Pattern Analysis and Machine Intelligence, vol. 35,
    no. 6, June 2013, 1397-1409

*/

#include "common/guided_filter.h"
#include "common/darktable.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>

typedef struct tile
{
  int left, right, lower, upper;
} tile;

typedef float rgb_lab_pixel[3];

typedef struct rgb_lab_image
{
  rgb_lab_pixel *data;
  int width, height, stride;
} rgb_lab_image;

static inline float *get_rgb_lab_pixel(rgb_lab_image img, size_t i)
{
  return ((float *)img.data) + i * img.stride;
}

typedef float gray_pixel;

typedef struct gray_image
{
  gray_pixel *data;
  int width, height;
} gray_image;

// allocate space for 1-component image of size width x height
static inline gray_image new_gray_image(int width, int height)
{
  return (gray_image){ dt_alloc_align(64, sizeof(gray_pixel) * width * height), width, height };
}

// free space for 1-component image
static inline void free_gray_image(gray_image *img_p)
{
  dt_free_align(img_p->data);
  img_p->data = NULL;
}

// minimum of two integers
static inline int min_i(int a, int b)
{
  return a < b ? a : b;
}

// maximum of two integers
static inline int max_i(int a, int b)
{
  return a > b ? a : b;
}

// calculate the one-dimensional moving average over a window of size 2*w+1
// input array x has stride 1, output array y has stride stride_y
static inline void box_mean_1d(int N, const float *x, float *y, size_t stride_y, int w)
{
  float m = 0.f, n_box = 0.f;
  if(N > 2 * w)
  {
    for(int i = 0, i_end = w + 1; i < i_end; i++)
    {
      m += x[i];
      n_box++;
    }
    for(int i = 0, i_end = w; i < i_end; i++)
    {
      y[i * stride_y] = m / n_box;
      m += x[i + w + 1];
      n_box++;
    }
    for(int i = w, i_end = N - w - 1; i < i_end; i++)
    {
      y[i * stride_y] = m / n_box;
      m += x[i + w + 1] - x[i - w];
    }
    for(int i = N - w -1, i_end = N; i < i_end; i++)
    {
      y[i * stride_y] = m / n_box;
      m -= x[i - w];
      n_box--;
    }
  }
  else
  {
    for(int i = 0, i_end = min_i(w + 1, N); i < i_end; i++)
    {
      m += x[i];
      n_box++;
    }
    for(int i = 0; i < N; i++)
    {
      y[i * stride_y] = m / n_box;
      if(i - w >= 0)
      {
        m -= x[i - w];
        n_box--;
      }
      if(i + w + 1 < N)
      {
        m += x[i + w + 1];
        n_box++;
      }
    }
  }
}

// calculate the two-dimensional moving average over a box of size (2*w+1) x (2*w+1)
// does the calculation in-place if input and ouput images are identical
// this function is always called from a OpenMP thread, thus no parallelization
static void box_mean(gray_image img1, gray_image img2, int w)
{
  gray_image img2_bak;
  if(img1.data == img2.data)
  {
    img2_bak = new_gray_image(max_i(img2.width, img2.height), 1);
    for(int i1 = 0; i1 < img2.height; i1++)
    {
      memcpy(img2_bak.data, img2.data + (size_t)i1 * img2.width, sizeof(gray_pixel) * img2.width);
      box_mean_1d(img2.width, img2_bak.data, img2.data + (size_t)i1 * img2.width, 1, w);
    }
  }
  else
  {
    for(int i1 = 0; i1 < img1.height; i1++)
      box_mean_1d(img1.width, img1.data + (size_t)i1 * img1.width, img2.data + (size_t)i1 * img2.width, 1, w);
    img2_bak = new_gray_image(1, img2.height);
  }
  for(int i0 = 0; i0 < img1.width; i0++)
  {
    for(int i1 = 0; i1 < img1.height; i1++) img2_bak.data[i1] = img2.data[i0 + (size_t)i1 * img2.width];
    box_mean_1d(img1.height, img2_bak.data, img2.data + i0, img1.width, w);
  }
  free_gray_image(&img2_bak);
}

// apply guided filter to single-component image img using the 3-components
// image imgg as a guide
static void guided_filter_tiling(rgb_lab_image imgg, gray_image img, gray_image img_out, tile target, int w,
                                 float eps, float min, float max)
{
  const tile source = { max_i(target.left - 2 * w, 0), min_i(target.right + 2 * w, imgg.width),
                        max_i(target.lower - 2 * w, 0), min_i(target.upper + 2 * w, imgg.height) };
  const int width = source.right - source.left;
  const int height = source.upper - source.lower;
  size_t size = (size_t)width * (size_t)height;
  gray_image imgg_mean_r = new_gray_image(width, height);
  gray_image imgg_mean_g = new_gray_image(width, height);
  gray_image imgg_mean_b = new_gray_image(width, height);
  gray_image img_mean = new_gray_image(width, height);
  for(int j_imgg = source.lower; j_imgg < source.upper; j_imgg++)
  {
    int j = j_imgg - source.lower;
    for(int i_imgg = source.left; i_imgg < source.right; i_imgg++)
    {
      int i = i_imgg - source.left;
      float *pixel = get_rgb_lab_pixel(imgg, i_imgg + (size_t)j_imgg * imgg.width);
      size_t k = i + (size_t)j * width;
      imgg_mean_r.data[k] = pixel[0];
      imgg_mean_g.data[k] = pixel[1];
      imgg_mean_b.data[k] = pixel[2];
      img_mean.data[k] = img.data[i_imgg + (size_t)j_imgg * img.width];
    }
  }
  box_mean(imgg_mean_r, imgg_mean_r, w);
  box_mean(imgg_mean_g, imgg_mean_g, w);
  box_mean(imgg_mean_b, imgg_mean_b, w);
  box_mean(img_mean, img_mean, w);
  gray_image cov_imgg_img_r = new_gray_image(width, height);
  gray_image cov_imgg_img_g = new_gray_image(width, height);
  gray_image cov_imgg_img_b = new_gray_image(width, height);
  gray_image var_imgg_rr = new_gray_image(width, height);
  gray_image var_imgg_gg = new_gray_image(width, height);
  gray_image var_imgg_bb = new_gray_image(width, height);
  gray_image var_imgg_rg = new_gray_image(width, height);
  gray_image var_imgg_rb = new_gray_image(width, height);
  gray_image var_imgg_gb = new_gray_image(width, height);
  for(int j_imgg = source.lower; j_imgg < source.upper; j_imgg++)
  {
    int j = j_imgg - source.lower;
    for(int i_imgg = source.left; i_imgg < source.right; i_imgg++)
    {
      int i = i_imgg - source.left;
      float *pixel = get_rgb_lab_pixel(imgg, i_imgg + (size_t)j_imgg * imgg.width);
      size_t k = i + (size_t)j * width;
      cov_imgg_img_r.data[k] = pixel[0] * img.data[i_imgg + (size_t)j_imgg * imgg.width];
      cov_imgg_img_g.data[k] = pixel[1] * img.data[i_imgg + (size_t)j_imgg * imgg.width];
      cov_imgg_img_b.data[k] = pixel[2] * img.data[i_imgg + (size_t)j_imgg * imgg.width];
      var_imgg_rr.data[k] = pixel[0] * pixel[0];
      var_imgg_rg.data[k] = pixel[0] * pixel[1];
      var_imgg_rb.data[k] = pixel[0] * pixel[2];
      var_imgg_gg.data[k] = pixel[1] * pixel[1];
      var_imgg_gb.data[k] = pixel[1] * pixel[2];
      var_imgg_bb.data[k] = pixel[2] * pixel[2];
    }
  }
  box_mean(cov_imgg_img_r, cov_imgg_img_r, w);
  box_mean(cov_imgg_img_g, cov_imgg_img_g, w);
  box_mean(cov_imgg_img_b, cov_imgg_img_b, w);
  box_mean(var_imgg_rr, var_imgg_rr, w);
  box_mean(var_imgg_rg, var_imgg_rg, w);
  box_mean(var_imgg_rb, var_imgg_rb, w);
  box_mean(var_imgg_gg, var_imgg_gg, w);
  box_mean(var_imgg_gb, var_imgg_gb, w);
  box_mean(var_imgg_bb, var_imgg_bb, w);
  for(size_t i = 0; i < size; i++)
  {
    cov_imgg_img_r.data[i] -= imgg_mean_r.data[i] * img_mean.data[i];
    cov_imgg_img_g.data[i] -= imgg_mean_g.data[i] * img_mean.data[i];
    cov_imgg_img_b.data[i] -= imgg_mean_b.data[i] * img_mean.data[i];
    var_imgg_rr.data[i] -= imgg_mean_r.data[i] * imgg_mean_r.data[i] - eps;
    var_imgg_rg.data[i] -= imgg_mean_r.data[i] * imgg_mean_g.data[i];
    var_imgg_rb.data[i] -= imgg_mean_r.data[i] * imgg_mean_b.data[i];
    var_imgg_gg.data[i] -= imgg_mean_g.data[i] * imgg_mean_g.data[i] - eps;
    var_imgg_gb.data[i] -= imgg_mean_g.data[i] * imgg_mean_b.data[i];
    var_imgg_bb.data[i] -= imgg_mean_b.data[i] * imgg_mean_b.data[i] - eps;
  }
  gray_image a_r = new_gray_image(width, height);
  gray_image a_g = new_gray_image(width, height);
  gray_image a_b = new_gray_image(width, height);
  gray_image b = img_mean;
  for(int i1 = 0; i1 < height; i1++)
  {
    size_t i = (size_t)i1 * width;
    for(int i0 = 0; i0 < width; i0++)
    {
      // solve linear system of equations of size 3x3 via Cramer's rule
      // symmetric coefficient matrix
      float Sigma_0_0 = var_imgg_rr.data[i];
      float Sigma_0_1 = var_imgg_rg.data[i];
      float Sigma_0_2 = var_imgg_rb.data[i];
      float Sigma_1_1 = var_imgg_gg.data[i];
      float Sigma_1_2 = var_imgg_gb.data[i];
      float Sigma_2_2 = var_imgg_bb.data[i];
      rgb_lab_pixel cov_imgg_img;
      cov_imgg_img[0] = cov_imgg_img_r.data[i];
      cov_imgg_img[1] = cov_imgg_img_g.data[i];
      cov_imgg_img[2] = cov_imgg_img_b.data[i];
      float det0 = Sigma_0_0 * (Sigma_1_1 * Sigma_2_2 - Sigma_1_2 * Sigma_1_2)
                   - Sigma_0_1 * (Sigma_0_1 * Sigma_2_2 - Sigma_0_2 * Sigma_1_2)
                   + Sigma_0_2 * (Sigma_0_1 * Sigma_1_2 - Sigma_0_2 * Sigma_1_1);
      if(fabsf(det0) > 4.f * FLT_EPSILON)
      {
        float det1 = cov_imgg_img[0] * (Sigma_1_1 * Sigma_2_2 - Sigma_1_2 * Sigma_1_2)
                     - Sigma_0_1 * (cov_imgg_img[1] * Sigma_2_2 - cov_imgg_img[2] * Sigma_1_2)
                     + Sigma_0_2 * (cov_imgg_img[1] * Sigma_1_2 - cov_imgg_img[2] * Sigma_1_1);
        float det2 = Sigma_0_0 * (cov_imgg_img[1] * Sigma_2_2 - cov_imgg_img[2] * Sigma_1_2)
                     - cov_imgg_img[0] * (Sigma_0_1 * Sigma_2_2 - Sigma_0_2 * Sigma_1_2)
                     + Sigma_0_2 * (Sigma_0_1 * cov_imgg_img[2] - Sigma_0_2 * cov_imgg_img[1]);
        float det3 = Sigma_0_0 * (Sigma_1_1 * cov_imgg_img[2] - Sigma_1_2 * cov_imgg_img[1])
                     - Sigma_0_1 * (Sigma_0_1 * cov_imgg_img[2] - Sigma_0_2 * cov_imgg_img[1])
                     + cov_imgg_img[0] * (Sigma_0_1 * Sigma_1_2 - Sigma_0_2 * Sigma_1_1);
        a_r.data[i] = det1 / det0;
        a_g.data[i] = det2 / det0;
        a_b.data[i] = det3 / det0;
      }
      else
      {
        // linear system is singular
        a_r.data[i] = 0.f;
        a_g.data[i] = 0.f;
        a_b.data[i] = 0.f;
      }
      b.data[i] -= a_r.data[i] * imgg_mean_r.data[i];
      b.data[i] -= a_g.data[i] * imgg_mean_g.data[i];
      b.data[i] -= a_b.data[i] * imgg_mean_b.data[i];
      ++i;
    }
  }
  box_mean(a_r, a_r, w);
  box_mean(a_g, a_g, w);
  box_mean(a_b, a_b, w);
  box_mean(b, b, w);
  for(int j_imgg = target.lower; j_imgg < target.upper; j_imgg++)
  {
    // index of the left most target pixel in the current row
    size_t l = target.left + (size_t)j_imgg * imgg.width;
    // index of the left most source pixel in the curent row of the
    // smaller auxiliary gray-scale images a_r, a_g, a_b, and b
    // excluding boundary data from neighboring tiles
    size_t k = (target.left - source.left) + (size_t)(j_imgg - source.lower) * width;
    for(int i_imgg = target.left; i_imgg < target.right; i_imgg++, k++, l++)
    {
      float *pixel = get_rgb_lab_pixel(imgg, l);
      float res = a_r.data[k] * pixel[0] + a_g.data[k] * pixel[1] + a_b.data[k] * pixel[2] + b.data[k];
      if(res < min) res = min;
      if(res > max) res = max;
      img_out.data[i_imgg + (size_t)j_imgg * imgg.width] = res;
    }
  }
  free_gray_image(&a_r);
  free_gray_image(&a_g);
  free_gray_image(&a_b);
  free_gray_image(&var_imgg_rr);
  free_gray_image(&var_imgg_rg);
  free_gray_image(&var_imgg_rb);
  free_gray_image(&var_imgg_gg);
  free_gray_image(&var_imgg_gb);
  free_gray_image(&var_imgg_bb);
  free_gray_image(&cov_imgg_img_r);
  free_gray_image(&cov_imgg_img_g);
  free_gray_image(&cov_imgg_img_b);
  free_gray_image(&img_mean);
  free_gray_image(&imgg_mean_r);
  free_gray_image(&imgg_mean_g);
  free_gray_image(&imgg_mean_b);
}


void guided_filter(const float *const guide, const float *const in, float *const out,
                   const int width, const int height, const int ch,
                   int w, // window size
                   float sqrt_eps, // regularization parameter
                   float min, float max)
{
  assert(ch >= 3);
  assert(w >= 1);

  rgb_lab_image img_guide = (rgb_lab_image){ (rgb_lab_pixel *)guide, width, height, ch };
  gray_image img_in = (gray_image){ (float *)in, width, height };
  gray_image img_out = (gray_image){ out, width, height };
  const int tile_width = max_i(3 * w, 512);
  const float eps = sqrt_eps * sqrt_eps;  // this is the regularization parameter of the original papers

#ifdef _OPENMP
#pragma omp parallel for collapse(2)
#endif
  for(int j = 0; j < height; j += tile_width)
  {
    for(int i = 0; i < width; i += tile_width)
    {
      tile target = { i, min_i(i + tile_width, width), j, min_i(j + tile_width, height) };
      guided_filter_tiling(img_guide, img_in, img_out, target, w, eps, min, max);
    }
  }
}

#ifdef HAVE_OPENCL
void guided_filter_cl(int devid, cl_mem guide, cl_mem in, cl_mem out,
                     int width, int height, int ch, int w,
                     float sqrt_eps, float min, float max)
{
  // fall-back implementation: copy data from device memory to host memory and perform filter
  // by CPU until there is a proper OpenCL implementation
  float *guide_host = dt_alloc_align(64, sizeof(*guide_host) * width * height * ch);
  float *in_host = dt_alloc_align(64, sizeof(*in_host) * width * height);
  float *out_host = dt_alloc_align(64, sizeof(*out_host) * width * height);
  int err;
  err = dt_opencl_read_host_from_device(devid, guide_host, guide, width, height, ch * sizeof(float));
  if(err != CL_SUCCESS) goto error;
  err = dt_opencl_read_host_from_device(devid, in_host, in, width, height, sizeof(float));
  if(err != CL_SUCCESS) goto error;
  guided_filter(guide_host, in_host, out_host, width, height, ch, w, sqrt_eps, 0.0f, 1.0f);
  err = dt_opencl_write_host_to_device(devid, out_host, out, width, height, sizeof(float));
  if(err != CL_SUCCESS) goto error;
error:
  dt_free_align(guide_host);
  dt_free_align(in_host);
  dt_free_align(out_host);
}
#endif
