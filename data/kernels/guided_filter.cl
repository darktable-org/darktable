/*
    This file is part of darktable,
    copyright (c) 2019 Heiko Bauke
    Copyright (C) 2023 darktable developers.

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

#include "common.h"

kernel void guided_filter_split_rgb_image(const int width,
                                          const int height,
                                          read_only image2d_t guide,
                                          write_only image2d_t out_r,
                                          write_only image2d_t out_g,
                                          write_only image2d_t out_b,
                                          const float guide_weight)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float4 weight = { guide_weight, guide_weight, guide_weight, 0.0f };
  const float4 pixel = weight * fmin(100.0, fmax(0.0, read_imagef(guide, sampleri, (int2)(x, y))));
  write_imagef(out_r, (int2)(x, y), pixel.x);
  write_imagef(out_g, (int2)(x, y), pixel.y);
  write_imagef(out_b, (int2)(x, y), pixel.z);
}


// Kahan summation algorithm
#define Kahan_sum(m, c, add)        \
  {                                 \
    const float t1 = (add) - (c);   \
    const float t2 = (m) + t1;      \
    c = (t2 - m) - t1;              \
    m = t2;                         \
  }


kernel void guided_filter_box_mean_x(const int width,
                                     const int height,
                                     read_only image2d_t in,
                                     write_only image2d_t out,
                                     const int w)
{
  const int y = get_global_id(0);
  if(y >= height) return;

  float m = 0.f, n_box = 0.f, c = 0.f;
  if(width > 2 * w)
  {
    for(int i = 0, i_end = w + 1; i < i_end; i++)
    {
      Kahan_sum(m, c, read_imagef(in, sampleri, (int2)(i, y)).x);
      n_box += 1.f;
    }
    for(int i = 0, i_end = w; i < i_end; i++)
    {
      write_imagef(out, (int2)(i, y), m / n_box);
      Kahan_sum(m, c, read_imagef(in, sampleri, (int2)(i + w + 1, y)).x);
      n_box += 1.f;
    }
    for(int i = w, i_end = width - w - 1; i < i_end; i++)
    {
      write_imagef(out, (int2)(i, y), m / n_box);
      Kahan_sum(m, c, read_imagef(in, sampleri, (int2)(i + w + 1, y)).x);
      Kahan_sum(m, c, -read_imagef(in, sampleri, (int2)(i - w, y)).x);
    }
    for(int i = width - w - 1, i_end = width; i < i_end; i++)
    {
      write_imagef(out, (int2)(i, y), m / n_box);
      Kahan_sum(m, c, -read_imagef(in, sampleri, (int2)(i - w, y)).x);
      n_box -= 1.f;
    }
  }
  else
  {
    for(int i = 0, i_end = min(w + 1, width); i < i_end; i++)
    {
      Kahan_sum(m, c, read_imagef(in, sampleri, (int2)(i, y)).x);
      n_box += 1.f;
    }
    for(int i = 0; i < width; i++)
    {
      write_imagef(out, (int2)(i, y), m / n_box);
      if(i - w >= 0)
      {
        Kahan_sum(m, c, -read_imagef(in, sampleri, (int2)(i - w, y)).x);
        n_box -= 1.f;
      }
      if(i + w + 1 < width)
      {
        Kahan_sum(m, c, read_imagef(in, sampleri, (int2)(i + w + 1, y)).x);
        n_box += 1.f;
      }
    }
  }
}


kernel void guided_filter_box_mean_y(const int width,
                                     const int height,
                                     read_only image2d_t in,
                                     write_only image2d_t out,
                                     const int w)
{
  const int x = get_global_id(0);
  if(x >= width) return;

  float m = 0.f, n_box = 0.f, c = 0.f;
  if(height > 2 * w)
  {
    for(int i = 0, i_end = w + 1; i < i_end; i++)
    {
      Kahan_sum(m, c, read_imagef(in, sampleri, (int2)(x, i)).x);
      n_box += 1.f;
    }
    for(int i = 0, i_end = w; i < i_end; i++)
    {
      write_imagef(out, (int2)(x, i), m / n_box);
      Kahan_sum(m, c, read_imagef(in, sampleri, (int2)(x, i + w + 1)).x);
      n_box += 1.f;
    }
    for(int i = w, i_end = height - w - 1; i < i_end; i++)
    {
      write_imagef(out, (int2)(x, i), m / n_box);
      Kahan_sum(m, c, read_imagef(in, sampleri, (int2)(x, i + w + 1)).x);
      Kahan_sum(m, c, -read_imagef(in, sampleri, (int2)(x, i - w)).x);
    }
    for(int i = height - w - 1, i_end = height; i < i_end; i++)
    {
      write_imagef(out, (int2)(x, i), m / n_box);
      Kahan_sum(m, c, -read_imagef(in, sampleri, (int2)(x, i - w)).x);
      n_box -= 1.f;
    }
  }
  else
  {
    for(int i = 0, i_end = min(w + 1, height); i < i_end; i++)
    {
      Kahan_sum(m, c, read_imagef(in, sampleri, (int2)(x, i)).x);
      n_box += 1.f;
    }
    for(int i = 0; i < height; i++)
    {
      write_imagef(out, (int2)(x, i), m / n_box);
      if(i - w >= 0)
      {
        Kahan_sum(m, c, -read_imagef(in, sampleri, (int2)(x, i - w)).x);
        n_box -= 1.f;
      }
      if(i + w + 1 < height)
      {
        Kahan_sum(m, c, read_imagef(in, sampleri, (int2)(x, i + w + 1)).x);
        n_box += 1.f;
      }
    }
  }
}

kernel void guided_filter_covariances(const int width,
                                      const int height,
                                      read_only image2d_t guide,
                                      read_only image2d_t img,
                                      write_only image2d_t cov_imgg_img_r,
                                      write_only image2d_t cov_imgg_img_g,
                                      write_only image2d_t cov_imgg_img_b,
                                      const float guide_weight)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float4 weight = { guide_weight, guide_weight, guide_weight, 0.0f };
  const float img_ = read_imagef(img, sampleri, (int2)(x, y)).x;
  const float4 imgv = { img_, img_, img_, 0.0f };
  const float4 pixel = imgv * weight * fmin(100.0, fmax(0.0, read_imagef(guide, sampleri, (int2)(x, y))));
  write_imagef(cov_imgg_img_r, (int2)(x, y), pixel.x);
  write_imagef(cov_imgg_img_g, (int2)(x, y), pixel.y);
  write_imagef(cov_imgg_img_b, (int2)(x, y), pixel.z);
}


kernel void guided_filter_variances(const int width,
                                    const int height,
                                    read_only image2d_t guide,
                                    write_only image2d_t var_imgg_rr,
                                    write_only image2d_t var_imgg_rg,
                                    write_only image2d_t var_imgg_rb,
                                    write_only image2d_t var_imgg_gg,
                                    write_only image2d_t var_imgg_gb,
                                    write_only image2d_t var_imgg_bb,
                                    const float guide_weight)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float4 weight = { guide_weight, guide_weight, guide_weight, 0.0f };
  const float4 pixel = weight * fmin(100.0, fmax(0.0, read_imagef(guide, sampleri, (int2)(x, y))));
  write_imagef(var_imgg_rr, (int2)(x, y), pixel.x * pixel.x);
  write_imagef(var_imgg_rg, (int2)(x, y), pixel.x * pixel.y);
  write_imagef(var_imgg_rb, (int2)(x, y), pixel.x * pixel.z);
  write_imagef(var_imgg_gg, (int2)(x, y), pixel.y * pixel.y);
  write_imagef(var_imgg_gb, (int2)(x, y), pixel.y * pixel.z);
  write_imagef(var_imgg_bb, (int2)(x, y), pixel.z * pixel.z);
}


kernel void guided_filter_update_covariance(const int width,
                                            const int height,
                                            read_only image2d_t in,
                                            write_only image2d_t out,
                                            read_only image2d_t a,
                                            read_only image2d_t b,
                                            const float eps)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float i_val = read_imagef(in, samplerA, (int2)(x, y)).x;
  const float a_val = read_imagef(a,  samplerA, (int2)(x, y)).x;
  const float b_val = read_imagef(b,  samplerA, (int2)(x, y)).x;
  write_imagef(out, (int2)(x, y), i_val - a_val * b_val + eps);
}


kernel void guided_filter_solve(const int width,
                                const int height,
                                read_only image2d_t img_mean,
                                read_only image2d_t imgg_mean_r,
                                read_only image2d_t imgg_mean_g,
                                read_only image2d_t imgg_mean_b,
                                read_only image2d_t cov_imgg_img_r,
                                read_only image2d_t cov_imgg_img_g,
                                read_only image2d_t cov_imgg_img_b,
                                read_only image2d_t var_imgg_rr,
                                read_only image2d_t var_imgg_rg,
                                read_only image2d_t var_imgg_rb,
                                read_only image2d_t var_imgg_gg,
                                read_only image2d_t var_imgg_gb,
                                read_only image2d_t var_imgg_bb,
                                write_only image2d_t a_r,
                                write_only image2d_t a_g,
                                write_only image2d_t a_b,
                                write_only image2d_t b)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float Sigma_0_0 = read_imagef(var_imgg_rr, sampleri, (int2)(x, y)).x;
  const float Sigma_0_1 = read_imagef(var_imgg_rg, sampleri, (int2)(x, y)).x;
  const float Sigma_0_2 = read_imagef(var_imgg_rb, sampleri, (int2)(x, y)).x;
  const float Sigma_1_1 = read_imagef(var_imgg_gg, sampleri, (int2)(x, y)).x;
  const float Sigma_1_2 = read_imagef(var_imgg_gb, sampleri, (int2)(x, y)).x;
  const float Sigma_2_2 = read_imagef(var_imgg_bb, sampleri, (int2)(x, y)).x;
  const float cov_imgg_img[3] = { read_imagef(cov_imgg_img_r, sampleri, (int2)(x, y)).x,
                                  read_imagef(cov_imgg_img_g, sampleri, (int2)(x, y)).x,
                                  read_imagef(cov_imgg_img_b, sampleri, (int2)(x, y)).x };
  const float det0 = Sigma_0_0 * (Sigma_1_1 * Sigma_2_2 - Sigma_1_2 * Sigma_1_2)
                     - Sigma_0_1 * (Sigma_0_1 * Sigma_2_2 - Sigma_0_2 * Sigma_1_2)
                     + Sigma_0_2 * (Sigma_0_1 * Sigma_1_2 - Sigma_0_2 * Sigma_1_1);
  float a_r_ = 0.0f;
  float a_g_ = 0.0f;
  float a_b_ = 0.0f;
  float b_ = read_imagef(img_mean, sampleri, (int2)(x, y)).x;
  if(fabs(det0) > 4.f * FLT_EPSILON)
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
    b_ =  b_
        - a_r_ * read_imagef(imgg_mean_r, sampleri, (int2)(x, y)).x
        - a_g_ * read_imagef(imgg_mean_g, sampleri, (int2)(x, y)).x
        - a_b_ * read_imagef(imgg_mean_b, sampleri, (int2)(x, y)).x;
  }

  write_imagef(a_r, (int2)(x, y), a_r_);
  write_imagef(a_g, (int2)(x, y), a_g_);
  write_imagef(a_b, (int2)(x, y), a_b_);
  write_imagef(b,   (int2)(x, y), b_);
}


kernel void guided_filter_generate_result(const int width,
                                          const int height,
                                          read_only image2d_t guide,
                                          read_only image2d_t a_r,
                                          read_only image2d_t a_g,
                                          read_only image2d_t a_b,
                                          read_only image2d_t b,
                                          write_only image2d_t res,
                                          const float guide_weight,
                                          const float minval,
                                          const float maxval)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float4 pixel = fmin(100.0, fmax(0.0, read_imagef(guide, sampleri, (int2)(x, y))));
  const float a_r_ = pixel.x * read_imagef(a_r, sampleri, (int2)(x, y)).x;
  const float a_g_ = pixel.y * read_imagef(a_g, sampleri, (int2)(x, y)).x;
  const float a_b_ = pixel.z * read_imagef(a_b, sampleri, (int2)(x, y)).x;
  const float b_ =             read_imagef(b,   sampleri, (int2)(x, y)).x;
  const float res_ = guide_weight * (a_r_ + a_g_ + a_b_)+ b_;
  write_imagef(res, (int2)(x, y), fmin(maxval, fmax(minval, res_)));
}
