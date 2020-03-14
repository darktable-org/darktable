/*
    This file is part of darktable,
    Copyright (C) 2017-2020 darktable developers.

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
#include "common/opencl.h"
#include <assert.h>
#include <float.h>
#include <stdlib.h>
#include <string.h>

typedef struct tile
{
  int left, right, lower, upper;
} tile;

typedef struct color_image
{
  float *data;
  int width, height, stride;
} color_image;

static inline float *get_color_pixel(color_image img, size_t i)
{
  return img.data + i * img.stride;
}

typedef struct gray_image
{
  float *data;
  int width, height;
} gray_image;

// allocate space for 1-component image of size width x height
static inline gray_image new_gray_image(int width, int height)
{
  return (gray_image){ dt_alloc_align(64, sizeof(float) * width * height), width, height };
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

// Kahan summation algorithm
static inline float Kahan_sum(const float m, float *c, const float add)
{
   const float t1 = add - (*c);
   const float t2 = m + t1;
   *c = (t2 - m) - t1;
   return t2;
}

// calculate the one-dimensional moving average over a window of size 2*w+1
// input array x has stride 1, output array y has stride stride_y
static inline void box_mean_1d(int N, const float *x, float *y, size_t stride_y, int w)
{
  float m = 0.f, n_box = 0.f, c = 0.f;
  if(N > 2 * w)
  {
    for(int i = 0, i_end = w + 1; i < i_end; i++)
    {
      m = Kahan_sum(m, &c, x[i]);
      n_box++;
    }
    for(int i = 0, i_end = w; i < i_end; i++)
    {
      y[i * stride_y] = m / n_box;
      m = Kahan_sum(m, &c, x[i + w + 1]);
      n_box++;
    }
    for(int i = w, i_end = N - w - 1; i < i_end; i++)
    {
      y[i * stride_y] = m / n_box;
      m = Kahan_sum(m, &c, x[i + w + 1]),
      m = Kahan_sum(m, &c, -x[i - w]);
    }
    for(int i = N - w - 1, i_end = N; i < i_end; i++)
    {
      y[i * stride_y] = m / n_box;
      m = Kahan_sum(m, &c, -x[i - w]);
      n_box--;
    }
  }
  else
  {
    for(int i = 0, i_end = min_i(w + 1, N); i < i_end; i++)
    {
      m = Kahan_sum(m, &c, x[i]);
      n_box++;
    }
    for(int i = 0; i < N; i++)
    {
      y[i * stride_y] = m / n_box;
      if(i - w >= 0)
      {
        m = Kahan_sum(m, &c, -x[i - w]);
        n_box--;
      }
      if(i + w + 1 < N)
      {
        m = Kahan_sum(m, &c, x[i + w + 1]);
        n_box++;
      }
    }
  }
}

// calculate the two-dimensional moving average over a box of size (2*w+1) x (2*w+1)
// does the calculation in-place if input and output images are identical
// this function is always called from a OpenMP thread, thus no parallelization
static void box_mean(gray_image img1, gray_image img2, int w)
{
  gray_image img2_bak;
  if(img1.data == img2.data)
  {
    img2_bak = new_gray_image(max_i(img2.width, img2.height), 1);
    for(int i1 = 0; i1 < img2.height; i1++)
    {
      memcpy(img2_bak.data, img2.data + (size_t)i1 * img2.width, sizeof(float) * img2.width);
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
static void guided_filter_tiling(color_image imgg, gray_image img, gray_image img_out, tile target, const int w,
                                 const float eps, const float guide_weight, const float min, const float max)
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
      float *pixel = get_color_pixel(imgg, i_imgg + (size_t)j_imgg * imgg.width);
      size_t k = i + (size_t)j * width;
      imgg_mean_r.data[k] = pixel[0] * guide_weight;
      imgg_mean_g.data[k] = pixel[1] * guide_weight;
      imgg_mean_b.data[k] = pixel[2] * guide_weight;
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
      float *pixel_ = get_color_pixel(imgg, i_imgg + (size_t)j_imgg * imgg.width);
      float pixel[3] = { pixel_[0] * guide_weight, pixel_[1] * guide_weight, pixel_[2] * guide_weight };
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
    var_imgg_rr.data[i] -= imgg_mean_r.data[i] * imgg_mean_r.data[i];
    var_imgg_rr.data[i] += eps;
    var_imgg_rg.data[i] -= imgg_mean_r.data[i] * imgg_mean_g.data[i];
    var_imgg_rb.data[i] -= imgg_mean_r.data[i] * imgg_mean_b.data[i];
    var_imgg_gg.data[i] -= imgg_mean_g.data[i] * imgg_mean_g.data[i];
    var_imgg_gg.data[i] += eps;
    var_imgg_gb.data[i] -= imgg_mean_g.data[i] * imgg_mean_b.data[i];
    var_imgg_bb.data[i] -= imgg_mean_b.data[i] * imgg_mean_b.data[i];
    var_imgg_bb.data[i] += eps;
  }
  // we will recycle memory of the arrays imgg_mean_? and img_mean for the new coefficient arrays a_? and b to
  // reduce memory foot print
  gray_image a_r = imgg_mean_r;
  gray_image a_g = imgg_mean_g;
  gray_image a_b = imgg_mean_b;
  gray_image b = img_mean;
  for(int i1 = 0; i1 < height; i1++)
  {
    size_t i = (size_t)i1 * width;
    for(int i0 = 0; i0 < width; i0++)
    {
      // solve linear system of equations of size 3x3 via Cramer's rule
      // symmetric coefficient matrix
      const float Sigma_0_0 = var_imgg_rr.data[i];
      const float Sigma_0_1 = var_imgg_rg.data[i];
      const float Sigma_0_2 = var_imgg_rb.data[i];
      const float Sigma_1_1 = var_imgg_gg.data[i];
      const float Sigma_1_2 = var_imgg_gb.data[i];
      const float Sigma_2_2 = var_imgg_bb.data[i];
      const float cov_imgg_img[3] = { cov_imgg_img_r.data[i], cov_imgg_img_g.data[i], cov_imgg_img_b.data[i] };
      const float det0 = Sigma_0_0 * (Sigma_1_1 * Sigma_2_2 - Sigma_1_2 * Sigma_1_2)
                         - Sigma_0_1 * (Sigma_0_1 * Sigma_2_2 - Sigma_0_2 * Sigma_1_2)
                         + Sigma_0_2 * (Sigma_0_1 * Sigma_1_2 - Sigma_0_2 * Sigma_1_1);
      float a_r_, a_g_, a_b_;
      if(fabsf(det0) > 4.f * FLT_EPSILON)
      {
        const float det1 = cov_imgg_img[0] * (Sigma_1_1 * Sigma_2_2 - Sigma_1_2 * Sigma_1_2)
                           - Sigma_0_1 * (cov_imgg_img[1] * Sigma_2_2 - cov_imgg_img[2] * Sigma_1_2)
                           + Sigma_0_2 * (cov_imgg_img[1] * Sigma_1_2 - cov_imgg_img[2] * Sigma_1_1);
        const float det2 = Sigma_0_0 * (cov_imgg_img[1] * Sigma_2_2 - cov_imgg_img[2] * Sigma_1_2)
                           - cov_imgg_img[0] * (Sigma_0_1 * Sigma_2_2 - Sigma_0_2 * Sigma_1_2)
                           + Sigma_0_2 * (Sigma_0_1 * cov_imgg_img[2] - Sigma_0_2 * cov_imgg_img[1]);
        const float det3 = Sigma_0_0 * (Sigma_1_1 * cov_imgg_img[2] - Sigma_1_2 * cov_imgg_img[1])
                           - Sigma_0_1 * (Sigma_0_1 * cov_imgg_img[2] - Sigma_0_2 * cov_imgg_img[1])
                           + cov_imgg_img[0] * (Sigma_0_1 * Sigma_1_2 - Sigma_0_2 * Sigma_1_1);
        a_r_ = det1 / det0;
        a_g_ = det2 / det0;
        a_b_ = det3 / det0;
      }
      else
      {
        // linear system is singular
        a_r_ = 0.f;
        a_g_ = 0.f;
        a_b_ = 0.f;
      }
      b.data[i] -= a_r_ * imgg_mean_r.data[i];
      b.data[i] -= a_g_ * imgg_mean_g.data[i];
      b.data[i] -= a_b_ * imgg_mean_b.data[i];
      // now data of imgg_mean_? is no longer needed, we can safely overwrite aliasing arrays
      a_r.data[i] = a_r_;
      a_g.data[i] = a_g_;
      a_b.data[i] = a_b_;
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
    // index of the left most source pixel in the current row of the
    // smaller auxiliary gray-scale images a_r, a_g, a_b, and b
    // excluding boundary data from neighboring tiles
    size_t k = (target.left - source.left) + (size_t)(j_imgg - source.lower) * width;
    for(int i_imgg = target.left; i_imgg < target.right; i_imgg++, k++, l++)
    {
      float *pixel = get_color_pixel(imgg, l);
      float res = a_r.data[k] * pixel[0] + a_g.data[k] * pixel[1] + a_b.data[k] * pixel[2];
      res *= guide_weight;
      res += b.data[k];
      if(res < min) res = min;
      if(res > max) res = max;
      img_out.data[i_imgg + (size_t)j_imgg * imgg.width] = res;
    }
  }
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


void guided_filter(const float *const guide, const float *const in, float *const out, const int width,
                   const int height, const int ch,
                   const int w,              // window size
                   const float sqrt_eps,     // regularization parameter
                   const float guide_weight, // to balance the amplitudes in the guiding image and the input image
                   const float min, const float max)
{
  assert(ch >= 3);
  assert(w >= 1);

  color_image img_guide = (color_image){ (float *)guide, width, height, ch };
  gray_image img_in = (gray_image){ (float *)in, width, height };
  gray_image img_out = (gray_image){ out, width, height };
  const int tile_width = max_i(3 * w, 512);
  const float eps = sqrt_eps * sqrt_eps; // this is the regularization parameter of the original papers

#ifdef _OPENMP
#pragma omp parallel for collapse(2)
#endif
  for(int j = 0; j < height; j += tile_width)
  {
    for(int i = 0; i < width; i += tile_width)
    {
      tile target = { i, min_i(i + tile_width, width), j, min_i(j + tile_width, height) };
      guided_filter_tiling(img_guide, img_in, img_out, target, w, eps, guide_weight, min, max);
    }
  }
}

#ifdef HAVE_OPENCL

dt_guided_filter_cl_global_t *dt_guided_filter_init_cl_global()
{
  dt_guided_filter_cl_global_t *g = malloc(sizeof(*g));
  const int program = 26; // guided_filter.cl, from programs.conf
  g->kernel_guided_filter_split_rgb = dt_opencl_create_kernel(program, "guided_filter_split_rgb_image");
  g->kernel_guided_filter_box_mean_x = dt_opencl_create_kernel(program, "guided_filter_box_mean_x");
  g->kernel_guided_filter_box_mean_y = dt_opencl_create_kernel(program, "guided_filter_box_mean_y");
  g->kernel_guided_filter_guided_filter_covariances
      = dt_opencl_create_kernel(program, "guided_filter_covariances");
  g->kernel_guided_filter_guided_filter_variances = dt_opencl_create_kernel(program, "guided_filter_variances");
  g->kernel_guided_filter_update_covariance = dt_opencl_create_kernel(program, "guided_filter_update_covariance");
  g->kernel_guided_filter_solve = dt_opencl_create_kernel(program, "guided_filter_solve");
  g->kernel_guided_filter_generate_result = dt_opencl_create_kernel(program, "guided_filter_generate_result");
  return g;
}


void dt_guided_filter_free_cl_global(dt_guided_filter_cl_global_t *g)
{
  if(!g) return;
  // destroy kernels
  dt_opencl_free_kernel(g->kernel_guided_filter_split_rgb);
  dt_opencl_free_kernel(g->kernel_guided_filter_box_mean_x);
  dt_opencl_free_kernel(g->kernel_guided_filter_box_mean_y);
  dt_opencl_free_kernel(g->kernel_guided_filter_guided_filter_covariances);
  dt_opencl_free_kernel(g->kernel_guided_filter_guided_filter_variances);
  dt_opencl_free_kernel(g->kernel_guided_filter_update_covariance);
  dt_opencl_free_kernel(g->kernel_guided_filter_solve);
  dt_opencl_free_kernel(g->kernel_guided_filter_generate_result);
  free(g);
}


static int cl_split_rgb(const int devid, const int width, const int height, cl_mem guide, cl_mem imgg_r,
                        cl_mem imgg_g, cl_mem imgg_b, const float guide_weight)
{
  const int kernel = darktable.opencl->guided_filter->kernel_guided_filter_split_rgb;
  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(width), &width);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(height), &height);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(guide), &guide);
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(imgg_r), &imgg_r);
  dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(imgg_g), &imgg_g);
  dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(imgg_b), &imgg_b);
  dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(guide_weight), &guide_weight);
  const size_t sizes[] = { ROUNDUPWD(width), ROUNDUPWD(height) };
  return dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
}


static int cl_box_mean(const int devid, const int width, const int height, const int w, cl_mem in, cl_mem out,
                       cl_mem temp)
{
  const int kernel_x = darktable.opencl->guided_filter->kernel_guided_filter_box_mean_x;
  dt_opencl_set_kernel_arg(devid, kernel_x, 0, sizeof(width), &width);
  dt_opencl_set_kernel_arg(devid, kernel_x, 1, sizeof(height), &height);
  dt_opencl_set_kernel_arg(devid, kernel_x, 2, sizeof(in), &in);
  dt_opencl_set_kernel_arg(devid, kernel_x, 3, sizeof(temp), &temp);
  dt_opencl_set_kernel_arg(devid, kernel_x, 4, sizeof(w), &w);
  const size_t sizes_x[] = { 1, ROUNDUPWD(height) };
  const int err = dt_opencl_enqueue_kernel_2d(devid, kernel_x, sizes_x);
  if(err != CL_SUCCESS) return err;

  const int kernel_y = darktable.opencl->guided_filter->kernel_guided_filter_box_mean_y;
  dt_opencl_set_kernel_arg(devid, kernel_y, 0, sizeof(width), &width);
  dt_opencl_set_kernel_arg(devid, kernel_y, 1, sizeof(height), &height);
  dt_opencl_set_kernel_arg(devid, kernel_y, 2, sizeof(temp), &temp);
  dt_opencl_set_kernel_arg(devid, kernel_y, 3, sizeof(out), &out);
  dt_opencl_set_kernel_arg(devid, kernel_y, 4, sizeof(w), &w);
  const size_t sizes_y[] = { ROUNDUPWD(width), 1 };
  return dt_opencl_enqueue_kernel_2d(devid, kernel_y, sizes_y);
}


static int cl_covariances(const int devid, const int width, const int height, cl_mem guide, cl_mem in,
                          cl_mem cov_imgg_img_r, cl_mem cov_imgg_img_g, cl_mem cov_imgg_img_b,
                          const float guide_weight)
{
  const int kernel = darktable.opencl->guided_filter->kernel_guided_filter_guided_filter_covariances;
  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(width), &width);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(height), &height);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(guide), &guide);
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(in), &in);
  dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cov_imgg_img_r), &cov_imgg_img_r);
  dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cov_imgg_img_g), &cov_imgg_img_g);
  dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(cov_imgg_img_b), &cov_imgg_img_b);
  dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(guide_weight), &guide_weight);
  const size_t sizes[] = { ROUNDUPWD(width), ROUNDUPWD(height) };
  return dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
}


static int cl_variances(const int devid, const int width, const int height, cl_mem guide, cl_mem var_imgg_rr,
                        cl_mem var_imgg_rg, cl_mem var_imgg_rb, cl_mem var_imgg_gg, cl_mem var_imgg_gb,
                        cl_mem var_imgg_bb, const float guide_weight)
{
  const int kernel = darktable.opencl->guided_filter->kernel_guided_filter_guided_filter_variances;
  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(width), &width);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(height), &height);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(guide), &guide);
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(var_imgg_rr), &var_imgg_rr);
  dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(var_imgg_rg), &var_imgg_rg);
  dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(var_imgg_rb), &var_imgg_rb);
  dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(var_imgg_gg), &var_imgg_gg);
  dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(var_imgg_gb), &var_imgg_gb);
  dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(var_imgg_bb), &var_imgg_bb);
  dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(guide_weight), &guide_weight);
  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPWD(height) };
  return dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
}


static int cl_update_covariance(const int devid, const int width, const int height, cl_mem in, cl_mem out,
                                cl_mem a, cl_mem b, float eps)
{
  const int kernel = darktable.opencl->guided_filter->kernel_guided_filter_update_covariance;
  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(width), &width);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(height), &height);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(in), &in);
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(out), &out);
  dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(a), &a);
  dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(b), &b);
  dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(eps), &eps);
  const size_t sizes[] = { ROUNDUPWD(width), ROUNDUPWD(height) };
  return dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
}


static int cl_solve(const int devid, const int width, const int height, cl_mem img_mean, cl_mem imgg_mean_r,
                    cl_mem imgg_mean_g, cl_mem imgg_mean_b, cl_mem cov_imgg_img_r, cl_mem cov_imgg_img_g,
                    cl_mem cov_imgg_img_b, cl_mem var_imgg_rr, cl_mem var_imgg_rg, cl_mem var_imgg_rb,
                    cl_mem var_imgg_gg, cl_mem var_imgg_gb, cl_mem var_imgg_bb, cl_mem a_r, cl_mem a_g, cl_mem a_b,
                    cl_mem b)
{
  const int kernel = darktable.opencl->guided_filter->kernel_guided_filter_solve;
  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(width), &width);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(height), &height);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(img_mean), &img_mean);
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(imgg_mean_r), &imgg_mean_r);
  dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(imgg_mean_g), &imgg_mean_g);
  dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(imgg_mean_b), &imgg_mean_b);
  dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(cov_imgg_img_r), &cov_imgg_img_r);
  dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(cov_imgg_img_g), &cov_imgg_img_g);
  dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(cov_imgg_img_b), &cov_imgg_img_b);
  dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(var_imgg_rr), &var_imgg_rr);
  dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(var_imgg_rg), &var_imgg_rg);
  dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(var_imgg_rb), &var_imgg_rb);
  dt_opencl_set_kernel_arg(devid, kernel, 12, sizeof(var_imgg_gg), &var_imgg_gg);
  dt_opencl_set_kernel_arg(devid, kernel, 13, sizeof(var_imgg_gb), &var_imgg_gb);
  dt_opencl_set_kernel_arg(devid, kernel, 14, sizeof(var_imgg_bb), &var_imgg_bb);
  dt_opencl_set_kernel_arg(devid, kernel, 15, sizeof(a_r), &a_r);
  dt_opencl_set_kernel_arg(devid, kernel, 16, sizeof(a_g), &a_g);
  dt_opencl_set_kernel_arg(devid, kernel, 17, sizeof(a_b), &a_b);
  dt_opencl_set_kernel_arg(devid, kernel, 18, sizeof(b), &b);
  const size_t sizes[] = { ROUNDUPWD(width), ROUNDUPWD(height) };
  return dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
}


static int cl_generate_result(const int devid, const int width, const int height, cl_mem guide, cl_mem a_r,
                              cl_mem a_g, cl_mem a_b, cl_mem b, cl_mem out, const float guide_weight,
                              const float min, const float max)
{
  const int kernel = darktable.opencl->guided_filter->kernel_guided_filter_generate_result;
  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(width), &width);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(height), &height);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(guide), &guide);
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(a_r), &a_r);
  dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(a_g), &a_g);
  dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(a_b), &a_b);
  dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(b), &b);
  dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(out), &out);
  dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(guide_weight), &guide_weight);
  dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(min), &min);
  dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(max), &max);
  const size_t sizes[] = { ROUNDUPWD(width), ROUNDUPWD(height) };
  return dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
}


static int guided_filter_cl_impl(int devid, cl_mem guide, cl_mem in, cl_mem out, const int width, const int height,
                                 const int ch,
                                 const int w,              // window size
                                 const float sqrt_eps,     // regularization parameter
                                 const float guide_weight, // to balance the amplitudes in the guiding image and
                                                           // the input// image
                                 const float min, const float max)
{
  const float eps = sqrt_eps * sqrt_eps; // this is the regularization parameter of the original papers

  void *temp1 = dt_opencl_alloc_device(devid, width, height, (int)sizeof(float));
  void *temp2 = dt_opencl_alloc_device(devid, width, height, (int)sizeof(float));
  void *imgg_mean_r = dt_opencl_alloc_device(devid, width, height, (int)sizeof(float));
  void *imgg_mean_g = dt_opencl_alloc_device(devid, width, height, (int)sizeof(float));
  void *imgg_mean_b = dt_opencl_alloc_device(devid, width, height, (int)sizeof(float));
  void *img_mean = dt_opencl_alloc_device(devid, width, height, (int)sizeof(float));
  void *cov_imgg_img_r = dt_opencl_alloc_device(devid, width, height, (int)sizeof(float));
  void *cov_imgg_img_g = dt_opencl_alloc_device(devid, width, height, (int)sizeof(float));
  void *cov_imgg_img_b = dt_opencl_alloc_device(devid, width, height, (int)sizeof(float));
  void *var_imgg_rr = dt_opencl_alloc_device(devid, width, height, (int)sizeof(float));
  void *var_imgg_gg = dt_opencl_alloc_device(devid, width, height, (int)sizeof(float));
  void *var_imgg_bb = dt_opencl_alloc_device(devid, width, height, (int)sizeof(float));
  void *var_imgg_rg = dt_opencl_alloc_device(devid, width, height, (int)sizeof(float));
  void *var_imgg_rb = dt_opencl_alloc_device(devid, width, height, (int)sizeof(float));
  void *var_imgg_gb = dt_opencl_alloc_device(devid, width, height, (int)sizeof(float));
  void *a_r = dt_opencl_alloc_device(devid, width, height, (int)sizeof(float));
  void *a_g = dt_opencl_alloc_device(devid, width, height, (int)sizeof(float));
  void *a_b = dt_opencl_alloc_device(devid, width, height, (int)sizeof(float));
  void *b = temp2;

  int err = CL_SUCCESS;
  if(temp1 == NULL || temp2 == NULL ||                                                        //
     imgg_mean_r == NULL || imgg_mean_g == NULL || imgg_mean_b == NULL || img_mean == NULL || //
     cov_imgg_img_r == NULL || cov_imgg_img_g == NULL || cov_imgg_img_b == NULL ||            //
     var_imgg_rr == NULL || var_imgg_gg == NULL || var_imgg_bb == NULL ||                     //
     var_imgg_rg == NULL || var_imgg_rb == NULL || var_imgg_gb == NULL ||                     //
     a_r == NULL || a_g == NULL || a_b == NULL)
  {
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto error;
  }

  err = cl_split_rgb(devid, width, height, guide, imgg_mean_r, imgg_mean_g, imgg_mean_b, guide_weight);
  if(err != CL_SUCCESS) goto error;

  err = cl_box_mean(devid, width, height, w, in, img_mean, temp1);
  if(err != CL_SUCCESS) goto error;
  err = cl_box_mean(devid, width, height, w, imgg_mean_r, imgg_mean_r, temp1);
  if(err != CL_SUCCESS) goto error;
  err = cl_box_mean(devid, width, height, w, imgg_mean_g, imgg_mean_g, temp1);
  if(err != CL_SUCCESS) goto error;
  err = cl_box_mean(devid, width, height, w, imgg_mean_b, imgg_mean_b, temp1);
  if(err != CL_SUCCESS) goto error;

  err = cl_covariances(devid, width, height, guide, in, cov_imgg_img_r, cov_imgg_img_g, cov_imgg_img_b,
                       guide_weight);
  if(err != CL_SUCCESS) goto error;

  err = cl_variances(devid, width, height, guide, var_imgg_rr, var_imgg_rg, var_imgg_rb, var_imgg_gg, var_imgg_gb,
                     var_imgg_bb, guide_weight);
  if(err != CL_SUCCESS) goto error;

  err = cl_box_mean(devid, width, height, w, cov_imgg_img_r, temp2, temp1);
  if(err != CL_SUCCESS) goto error;
  err = cl_update_covariance(devid, width, height, temp2, cov_imgg_img_r, imgg_mean_r, img_mean, 0.f);
  if(err != CL_SUCCESS) goto error;
  err = cl_box_mean(devid, width, height, w, cov_imgg_img_g, temp2, temp1);
  if(err != CL_SUCCESS) goto error;
  err = cl_update_covariance(devid, width, height, temp2, cov_imgg_img_g, imgg_mean_g, img_mean, 0.f);
  if(err != CL_SUCCESS) goto error;
  err = cl_box_mean(devid, width, height, w, cov_imgg_img_b, temp2, temp1);
  if(err != CL_SUCCESS) goto error;
  err = cl_update_covariance(devid, width, height, temp2, cov_imgg_img_b, imgg_mean_b, img_mean, 0.f);
  if(err != CL_SUCCESS) goto error;
  err = cl_box_mean(devid, width, height, w, var_imgg_rr, temp2, temp1);
  if(err != CL_SUCCESS) goto error;
  err = cl_update_covariance(devid, width, height, temp2, var_imgg_rr, imgg_mean_r, imgg_mean_r, eps);
  if(err != CL_SUCCESS) goto error;
  err = cl_box_mean(devid, width, height, w, var_imgg_rg, temp2, temp1);
  if(err != CL_SUCCESS) goto error;
  err = cl_update_covariance(devid, width, height, temp2, var_imgg_rg, imgg_mean_r, imgg_mean_g, 0.f);
  if(err != CL_SUCCESS) goto error;
  err = cl_box_mean(devid, width, height, w, var_imgg_rb, temp2, temp1);
  if(err != CL_SUCCESS) goto error;
  err = cl_update_covariance(devid, width, height, temp2, var_imgg_rb, imgg_mean_r, imgg_mean_b, 0.f);
  if(err != CL_SUCCESS) goto error;
  err = cl_box_mean(devid, width, height, w, var_imgg_gg, temp2, temp1);
  if(err != CL_SUCCESS) goto error;
  err = cl_update_covariance(devid, width, height, temp2, var_imgg_gg, imgg_mean_g, imgg_mean_g, eps);
  if(err != CL_SUCCESS) goto error;
  err = cl_box_mean(devid, width, height, w, var_imgg_gb, temp2, temp1);
  if(err != CL_SUCCESS) goto error;
  err = cl_update_covariance(devid, width, height, temp2, var_imgg_gb, imgg_mean_g, imgg_mean_b, 0.f);
  if(err != CL_SUCCESS) goto error;
  err = cl_box_mean(devid, width, height, w, var_imgg_bb, temp2, temp1);
  if(err != CL_SUCCESS) goto error;
  err = cl_update_covariance(devid, width, height, temp2, var_imgg_bb, imgg_mean_b, imgg_mean_b, eps);
  if(err != CL_SUCCESS) goto error;

  err = cl_solve(devid, width, height, img_mean, imgg_mean_r, imgg_mean_g, imgg_mean_b, cov_imgg_img_r,
                 cov_imgg_img_g, cov_imgg_img_b, var_imgg_rr, var_imgg_rg, var_imgg_rb, var_imgg_gg, var_imgg_gb,
                 var_imgg_bb, a_r, a_g, a_b, b);
  if(err != CL_SUCCESS) goto error;

  err = cl_box_mean(devid, width, height, w, a_r, a_r, temp1);
  if(err != CL_SUCCESS) goto error;
  err = cl_box_mean(devid, width, height, w, a_g, a_g, temp1);
  if(err != CL_SUCCESS) goto error;
  err = cl_box_mean(devid, width, height, w, a_b, a_b, temp1);
  if(err != CL_SUCCESS) goto error;
  err = cl_box_mean(devid, width, height, w, b, b, temp1);
  if(err != CL_SUCCESS) goto error;

  err = cl_generate_result(devid, width, height, guide, a_r, a_g, a_b, b, out, guide_weight, min, max);

error:
  if(err != CL_SUCCESS) dt_print(DT_DEBUG_OPENCL, "[guided filter] unknown error: %d\n", err);

  dt_opencl_release_mem_object(a_r);
  dt_opencl_release_mem_object(a_g);
  dt_opencl_release_mem_object(a_b);
  dt_opencl_release_mem_object(var_imgg_rr);
  dt_opencl_release_mem_object(var_imgg_rg);
  dt_opencl_release_mem_object(var_imgg_rb);
  dt_opencl_release_mem_object(var_imgg_gg);
  dt_opencl_release_mem_object(var_imgg_gb);
  dt_opencl_release_mem_object(var_imgg_bb);
  dt_opencl_release_mem_object(cov_imgg_img_r);
  dt_opencl_release_mem_object(cov_imgg_img_g);
  dt_opencl_release_mem_object(cov_imgg_img_b);
  dt_opencl_release_mem_object(img_mean);
  dt_opencl_release_mem_object(imgg_mean_r);
  dt_opencl_release_mem_object(imgg_mean_g);
  dt_opencl_release_mem_object(imgg_mean_b);
  dt_opencl_release_mem_object(temp1);
  dt_opencl_release_mem_object(temp2);

  return err;
}


static void guided_filter_cl_fallback(int devid, cl_mem guide, cl_mem in, cl_mem out, const int width,
                                      const int height, const int ch,
                                      const int w,              // window size
                                      const float sqrt_eps,     // regularization parameter
                                      const float guide_weight, // to balance the amplitudes in the guiding image
                                                                // and the input// image
                                      const float min, const float max)
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
  guided_filter(guide_host, in_host, out_host, width, height, ch, w, sqrt_eps, guide_weight, min, max);
  err = dt_opencl_write_host_to_device(devid, out_host, out, width, height, sizeof(float));
  if(err != CL_SUCCESS) goto error;
error:
  dt_free_align(guide_host);
  dt_free_align(in_host);
  dt_free_align(out_host);
}


void guided_filter_cl(int devid, cl_mem guide, cl_mem in, cl_mem out, const int width, const int height,
                      const int ch,
                      const int w,              // window size
                      const float sqrt_eps,     // regularization parameter
                      const float guide_weight, // to balance the amplitudes in the guiding image and the input
                                                // image
                      const float min, const float max)
{
  assert(ch >= 3);
  assert(w >= 1);

  const cl_ulong max_global_mem = dt_opencl_get_max_global_mem(devid);
  const size_t reserved_memory = (size_t)(dt_conf_get_float("opencl_memory_headroom") * 1024 * 1024);
  // estimate required memory for OpenCL code path with a safety factor of 5/4
  const size_t required_memory
      = darktable.opencl->dev[devid].memory_in_use + (size_t)width * height * sizeof(float) * 18 * 5 / 4;
  int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
  if(max_global_mem - reserved_memory > required_memory)
    err = guided_filter_cl_impl(devid, guide, in, out, width, height, ch, w, sqrt_eps, guide_weight, min, max);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL, "[guided filter] fall back to cpu implementation due to insufficient gpu memory\n");
    guided_filter_cl_fallback(devid, guide, in, out, width, height, ch, w, sqrt_eps, guide_weight, min, max);
  }
}

#endif
