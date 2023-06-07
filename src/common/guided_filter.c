/*
    This file is part of darktable,
    Copyright (C) 2017-2023 darktable developers.

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

#include "common/box_filters.h"
#include "common/guided_filter.h"
#include "common/math.h"
#include "common/opencl.h"
#include <assert.h>
#include <float.h>
#include <stdlib.h>
#include <string.h>

// processing is split into tiles of this size (or three times the filter
// width, if greater) to keep memory use under control.
#define GF_TILE_SIZE 512

// the filter does internal tiling to keep memory requirements reasonable, so this structure
// defines the position of the tile being processed
typedef struct tile
{
  int left, right, lower, upper;
} tile;

typedef struct color_image
{
  float *data;
  int width, height, stride;
} color_image;

// allocate space for n-component image of size width x height
static inline color_image new_color_image(int width, int height, int ch)
{
  return (color_image){ dt_alloc_align_float((size_t)width * height * ch), width, height, ch };
}

// free space for n-component image
static inline void free_color_image(color_image *img_p)
{
  dt_free_align(img_p->data);
  img_p->data = NULL;
}

// get a pointer to pixel number 'i' within the image
static inline float *get_color_pixel(color_image img, size_t i)
{
  return img.data + i * img.stride;
}


// apply guided filter to single-component image img using the 3-components image imgg as a guide
// the filtering applies a monochrome box filter to a total of 13 image channels:
//    1 monochrome input image
//    3 color guide image
//    3 covariance (R, G, B)
//    6 variance (R-R, R-G, R-B, G-G, G-B, B-B)
// for computational efficiency, we'll pack them into a four-channel image and a 9-channel image
// image instead of running 13 separate box filters: guide+input, R/G/B/R-R/R-G/R-B/G-G/G-B/B-B.
static void guided_filter_tiling(color_image imgg, gray_image img, gray_image img_out, tile target, const int w,
                                 const float eps, const float guide_weight, const float min, const float max)
{
  const tile source = { max_i(target.left - 2 * w, 0), min_i(target.right + 2 * w, imgg.width),
                        max_i(target.lower - 2 * w, 0), min_i(target.upper + 2 * w, imgg.height) };
  const int width = source.right - source.left;
  const int height = source.upper - source.lower;
  size_t size = (size_t)width * (size_t)height;
// since we're packing multiple monochrome planes into a color image, define symbolic constants so that
// we can keep track of which values we're actually using
#define INP_MEAN 0
#define GUIDE_MEAN_R 1
#define GUIDE_MEAN_G 2
#define GUIDE_MEAN_B 3
#define COV_R 0
#define COV_G 1
#define COV_B 2
#define VAR_RR 3
#define VAR_RG 4
#define VAR_RB 5
#define VAR_GG 6
#define VAR_BB 8
#define VAR_GB 7
  color_image mean = new_color_image(width, height, 4);
  color_image variance = new_color_image(width, height, 9);
  const size_t img_dimen = mean.width;
  size_t img_bak_sz;
  float *img_bak = dt_alloc_perthread_float(9*img_dimen, &img_bak_sz);
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(img, imgg, mean, variance, img_bak) \
  dt_omp_firstprivate(img_bak_sz, img_dimen, w, guide_weight) dt_omp_sharedconst(source)
#endif
  for(int j_imgg = source.lower; j_imgg < source.upper; j_imgg++)
  {
    int j = j_imgg - source.lower;
    float *const restrict meanpx = mean.data + 4 * j * mean.width;
    float *const restrict varpx = variance.data + 9 * j * variance.width;
    for(int i_imgg = source.left; i_imgg < source.right; i_imgg++)
    {
      size_t i = i_imgg - source.left;
      const float *pixel_ = get_color_pixel(imgg, i_imgg + (size_t)j_imgg * imgg.width);
      dt_aligned_pixel_t pixel =
        { pixel_[0] * guide_weight, pixel_[1] * guide_weight, pixel_[2] * guide_weight, pixel_[3] * guide_weight };
      const float input = img.data[i_imgg + (size_t)j_imgg * img.width];
      meanpx[4*i+INP_MEAN] = input;
      meanpx[4*i+GUIDE_MEAN_R] = pixel[0];
      meanpx[4*i+GUIDE_MEAN_G] = pixel[1];
      meanpx[4*i+GUIDE_MEAN_B] = pixel[2];
      varpx[9*i+COV_R] = pixel[0] * input;
      varpx[9*i+COV_G] = pixel[1] * input;
      varpx[9*i+COV_B] = pixel[2] * input;
      varpx[9*i+VAR_RR] = pixel[0] * pixel[0];
      varpx[9*i+VAR_RG] = pixel[0] * pixel[1];
      varpx[9*i+VAR_RB] = pixel[0] * pixel[2];
      varpx[9*i+VAR_GG] = pixel[1] * pixel[1];
      varpx[9*i+VAR_GB] = pixel[1] * pixel[2];
      varpx[9*i+VAR_BB] = pixel[2] * pixel[2];
    }
    // apply horizontal pass of box mean filter while the cache is still hot
    float *const restrict scratch = dt_get_perthread(img_bak, img_bak_sz);
    dt_box_mean_horizontal(meanpx, mean.width, 4|BOXFILTER_KAHAN_SUM, w, scratch);
    dt_box_mean_horizontal(varpx, variance.width, 9|BOXFILTER_KAHAN_SUM, w, scratch);
  }
  dt_free_align(img_bak);
  dt_box_mean_vertical(mean.data, mean.height, mean.width, 4|BOXFILTER_KAHAN_SUM, w);
  dt_box_mean_vertical(variance.data, variance.height, variance.width, 9|BOXFILTER_KAHAN_SUM, w);
  // we will recycle memory of 'mean' for the new coefficient arrays a_? and b to reduce memory foot print
  color_image a_b = mean;
  #define A_RED 0
  #define A_GREEN 1
  #define A_BLUE 2
  #define B 3
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) \
  dt_omp_firstprivate(size, eps) shared(mean, variance, a_b)
#endif
  for(size_t i = 0; i < size; i++)
  {
    const float *meanpx = get_color_pixel(mean, i);
    const float inp_mean = meanpx[INP_MEAN];
    const float guide_r = meanpx[GUIDE_MEAN_R];
    const float guide_g = meanpx[GUIDE_MEAN_G];
    const float guide_b = meanpx[GUIDE_MEAN_B];
    float *const varpx = get_color_pixel(variance, i);
    // solve linear system of equations of size 3x3 via Cramer's rule
    // symmetric coefficient matrix
    const float Sigma_0_0 = varpx[VAR_RR] - (guide_r * guide_r) + eps;
    const float Sigma_0_1 = varpx[VAR_RG] - (guide_r * guide_g);
    const float Sigma_0_2 = varpx[VAR_RB] - (guide_r * guide_b);
    const float Sigma_1_1 = varpx[VAR_GG] - (guide_g * guide_g) + eps;;
    const float Sigma_1_2 = varpx[VAR_GB] - (guide_g * guide_b);
    const float Sigma_2_2 = varpx[VAR_BB] - (guide_b * guide_b) + eps;
    const float det0 = Sigma_0_0 * (Sigma_1_1 * Sigma_2_2 - Sigma_1_2 * Sigma_1_2)
      - Sigma_0_1 * (Sigma_0_1 * Sigma_2_2 - Sigma_0_2 * Sigma_1_2)
      + Sigma_0_2 * (Sigma_0_1 * Sigma_1_2 - Sigma_0_2 * Sigma_1_1);
    float a_r_, a_g_, a_b_, b_;
    if(fabsf(det0) > 4.f * FLT_EPSILON)
    {
      const float cov_r = varpx[COV_R] - guide_r * inp_mean;
      const float cov_g = varpx[COV_G] - guide_g * inp_mean;
      const float cov_b = varpx[COV_B] - guide_b * inp_mean;
      const float det1 = cov_r * (Sigma_1_1 * Sigma_2_2 - Sigma_1_2 * Sigma_1_2)
        - Sigma_0_1 * (cov_g * Sigma_2_2 - cov_b * Sigma_1_2)
        + Sigma_0_2 * (cov_g * Sigma_1_2 - cov_b * Sigma_1_1);
      const float det2 = Sigma_0_0 * (cov_g * Sigma_2_2 - cov_b * Sigma_1_2)
        - cov_r * (Sigma_0_1 * Sigma_2_2 - Sigma_0_2 * Sigma_1_2)
        + Sigma_0_2 * (Sigma_0_1 * cov_b - Sigma_0_2 * cov_g);
      const float det3 = Sigma_0_0 * (Sigma_1_1 * cov_b - Sigma_1_2 * cov_g)
        - Sigma_0_1 * (Sigma_0_1 * cov_b - Sigma_0_2 * cov_g)
        + cov_r * (Sigma_0_1 * Sigma_1_2 - Sigma_0_2 * Sigma_1_1);
      a_r_ = det1 / det0;
      a_g_ = det2 / det0;
      a_b_ = det3 / det0;
      b_ = inp_mean - a_r_ * guide_r - a_g_ * guide_g - a_b_ * guide_b;
    }
    else
    {
      // linear system is singular
      a_r_ = 0.f;
      a_g_ = 0.f;
      a_b_ = 0.f;
      b_ = get_color_pixel(mean, i)[INP_MEAN];
    }
    // now data of imgg_mean_? is no longer needed, we can safely overwrite aliasing arrays
    a_b.data[4*i+A_RED] = a_r_;
    a_b.data[4*i+A_GREEN] = a_g_;
    a_b.data[4*i+A_BLUE] = a_b_;
    a_b.data[4*i+B] = b_;
  }
  free_color_image(&variance);

  dt_box_mean(a_b.data, a_b.height, a_b.width, a_b.stride|BOXFILTER_KAHAN_SUM, w, 1);

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) \
  shared(target, imgg, a_b, img_out) dt_omp_sharedconst(source) dt_omp_firstprivate(min, max, width, guide_weight)
#endif
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
      const float *pixel = get_color_pixel(imgg, l);
      const float *px_ab = get_color_pixel(a_b, k);
      float res = guide_weight * (px_ab[A_RED] * pixel[0] + px_ab[A_GREEN] * pixel[1] + px_ab[A_BLUE] * pixel[2]);
      res += px_ab[B];
      img_out.data[i_imgg + (size_t)j_imgg * imgg.width] = CLAMP(res, min, max);
    }
  }
  free_color_image(&mean);
}

static int compute_tile_height(const int height, const int w)
{
  int tile_h = max_i(3 * w, GF_TILE_SIZE);
#if 0 // enabling the below doesn't make any measureable speed difference, but does cause a handful of pixels
      // to round off differently (as does changing GF_TILE_SIZE)
  if((height % tile_h) > 0 && (height % tile_h) < GF_TILE_SIZE/3)
  {
    // if there's just a sliver left over for the last row of tiles, see whether slicing off a few pixels
    // gives us a mostly-full tile
    if(height % (tile_h - 8) >= GF_TILE_SIZE/3)
      tile_h -= 8;
    else  if(height % (tile_h - w/4) >= GF_TILE_SIZE/3)
      tile_h -= (w/4);
    else  if(height % (tile_h - w/2) >= GF_TILE_SIZE/3)
      tile_h -= (w/2);
    // try adding a few pixels
    else if(height % (tile_h + 8) >= GF_TILE_SIZE/3)
      tile_h += 8;
    else if(height % (tile_h + 16) >= GF_TILE_SIZE/3)
      tile_h += 16;
  }
#endif
  return tile_h;
}

static int compute_tile_width(const int width, const int w)
{
  int tile_w = max_i(3 * w, GF_TILE_SIZE);
#if 0 // enabling the below doesn't make any measureable speed difference, but does cause a handful of pixels
      // to round off differently (as does changing GF_TILE_SIZE)
  if((width % tile_w) > 0 && (width % tile_w) < GF_TILE_SIZE/2)
  {
    // if there's just a sliver left over for the last column of tiles, see whether slicing off a few pixels
    // gives us a mostly-full tile
    if(width % (tile_w - 8) >= GF_TILE_SIZE/3)
      tile_w -= 8;
    else  if(width % (tile_w - w/4) >= GF_TILE_SIZE/3)
      tile_w -= (w/4);
    else  if(width % (tile_w - w/2) >= GF_TILE_SIZE/3)
      tile_w -= (w/2);
    // try adding a few pixels
    else if(width % (tile_w + 8) >= GF_TILE_SIZE/3)
      tile_w += 8;
    else if(width % (tile_w + 16) >= GF_TILE_SIZE/3)
      tile_w += 16;
  }
#endif
  return tile_w;
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
  const int tile_width = compute_tile_width(width,w);
  const int tile_height = compute_tile_height(height,w);
  const float eps = sqrt_eps * sqrt_eps; // this is the regularization parameter of the original papers

  for(int j = 0; j < height; j += tile_height)
  {
    for(int i = 0; i < width; i += tile_width)
    {
      tile target = { i, min_i(i + tile_width, width), j, min_i(j + tile_height, height) };
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
  return dt_opencl_enqueue_kernel_2d_args(devid, kernel, width, height,
    CLARG(width), CLARG(height), CLARG(guide), CLARG(imgg_r), CLARG(imgg_g), CLARG(imgg_b), CLARG(guide_weight));
}


static int cl_box_mean(const int devid, const int width, const int height, const int w, cl_mem in, cl_mem out,
                       cl_mem temp)
{
  const int kernel_x = darktable.opencl->guided_filter->kernel_guided_filter_box_mean_x;
  dt_opencl_set_kernel_args(devid, kernel_x, 0, CLARG(width), CLARG(height), CLARG(in), CLARG(temp), CLARG(w));
  const size_t sizes_x[] = { 1, ROUNDUPDHT(height, devid) };
  const int err = dt_opencl_enqueue_kernel_2d(devid, kernel_x, sizes_x);
  if(err != CL_SUCCESS) return err;

  const int kernel_y = darktable.opencl->guided_filter->kernel_guided_filter_box_mean_y;
  dt_opencl_set_kernel_args(devid, kernel_y, 0, CLARG(width), CLARG(height), CLARG(temp), CLARG(out), CLARG(w));
  const size_t sizes_y[] = { ROUNDUPDWD(width, devid), 1 };
  return dt_opencl_enqueue_kernel_2d(devid, kernel_y, sizes_y);
}


static int cl_covariances(const int devid, const int width, const int height, cl_mem guide, cl_mem in,
                          cl_mem cov_imgg_img_r, cl_mem cov_imgg_img_g, cl_mem cov_imgg_img_b,
                          const float guide_weight)
{
  const int kernel = darktable.opencl->guided_filter->kernel_guided_filter_guided_filter_covariances;
  return dt_opencl_enqueue_kernel_2d_args(devid, kernel, width, height,
    CLARG(width), CLARG(height), CLARG(guide), CLARG(in), CLARG(cov_imgg_img_r), CLARG(cov_imgg_img_g),
    CLARG(cov_imgg_img_b), CLARG(guide_weight));
}


static int cl_variances(const int devid, const int width, const int height, cl_mem guide, cl_mem var_imgg_rr,
                        cl_mem var_imgg_rg, cl_mem var_imgg_rb, cl_mem var_imgg_gg, cl_mem var_imgg_gb,
                        cl_mem var_imgg_bb, const float guide_weight)
{
  const int kernel = darktable.opencl->guided_filter->kernel_guided_filter_guided_filter_variances;
  return dt_opencl_enqueue_kernel_2d_args(devid, kernel, width, height,
    CLARG(width), CLARG(height), CLARG(guide), CLARG(var_imgg_rr), CLARG(var_imgg_rg), CLARG(var_imgg_rb),
    CLARG(var_imgg_gg), CLARG(var_imgg_gb), CLARG(var_imgg_bb), CLARG(guide_weight));
}


static int cl_update_covariance(const int devid, const int width, const int height, cl_mem in, cl_mem out,
                                cl_mem a, cl_mem b, float eps)
{
  const int kernel = darktable.opencl->guided_filter->kernel_guided_filter_update_covariance;
  return dt_opencl_enqueue_kernel_2d_args(devid, kernel, width, height,
    CLARG(width), CLARG(height), CLARG(in), CLARG(out), CLARG(a), CLARG(b), CLARG(eps));
}


static int cl_solve(const int devid, const int width, const int height, cl_mem img_mean, cl_mem imgg_mean_r,
                    cl_mem imgg_mean_g, cl_mem imgg_mean_b, cl_mem cov_imgg_img_r, cl_mem cov_imgg_img_g,
                    cl_mem cov_imgg_img_b, cl_mem var_imgg_rr, cl_mem var_imgg_rg, cl_mem var_imgg_rb,
                    cl_mem var_imgg_gg, cl_mem var_imgg_gb, cl_mem var_imgg_bb, cl_mem a_r, cl_mem a_g, cl_mem a_b,
                    cl_mem b)
{
  const int kernel = darktable.opencl->guided_filter->kernel_guided_filter_solve;
  return dt_opencl_enqueue_kernel_2d_args(devid, kernel, width, height,
    CLARG(width), CLARG(height), CLARG(img_mean), CLARG(imgg_mean_r), CLARG(imgg_mean_g), CLARG(imgg_mean_b),
    CLARG(cov_imgg_img_r), CLARG(cov_imgg_img_g), CLARG(cov_imgg_img_b), CLARG(var_imgg_rr), CLARG(var_imgg_rg),
    CLARG(var_imgg_rb), CLARG(var_imgg_gg), CLARG(var_imgg_gb), CLARG(var_imgg_bb), CLARG(a_r), CLARG(a_g),
    CLARG(a_b), CLARG(b));
}


static int cl_generate_result(const int devid, const int width, const int height, cl_mem guide, cl_mem a_r,
                              cl_mem a_g, cl_mem a_b, cl_mem b, cl_mem out, const float guide_weight,
                              const float min, const float max)
{
  const int kernel = darktable.opencl->guided_filter->kernel_guided_filter_generate_result;
  return dt_opencl_enqueue_kernel_2d_args(devid, kernel, width, height,
    CLARG(width), CLARG(height), CLARG(guide), CLARG(a_r), CLARG(a_g), CLARG(a_b), CLARG(b), CLARG(out),
    CLARG(guide_weight), CLARG(min), CLARG(max));
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

  // estimate required memory for OpenCL code path with a safety factor of 1.25
  const gboolean fits = dt_opencl_image_fits_device(devid, width, height, sizeof(float), 18.0f * 1.25f, 0);

  int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
  if(fits)
    err = guided_filter_cl_impl(devid, guide, in, out, width, height, ch, w, sqrt_eps, guide_weight, min, max);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL, "[guided filter] fall back to cpu implementation due to insufficient gpu memory\n");
    guided_filter_cl_fallback(devid, guide, in, out, width, height, ch, w, sqrt_eps, guide_weight, min, max);
  }
}

#endif
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

