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
*/

#include "control/control.h"
#include "develop/imageop.h"
#include "develop/openmp_maths.h"
#include "heal.h"

/* Based on the original source code of GIMP's Healing Tool, by Jean-Yves Couleaud
 *
 * http://www.gimp.org/
 *
 * */

/* NOTES
 *
 * The method used here is similar to the lighting invariant correction
 * method but slightly different: we do not divide the RGB components,
 * but subtract them I2 = I0 - I1, where I0 is the sample image to be
 * corrected, I1 is the reference pattern. Then we solve DeltaI=0
 * (Laplace) with I2 Dirichlet conditions at the borders of the
 * mask. The solver is a red/black checker Gauss-Seidel with over-relaxation.
 * It could benefit from a multi-grid evaluation of an initial solution
 * before the main iteration loop.
 *
 * I reduced the convergence criteria to 0.1% (0.001) as we are
 * dealing here with RGB integer components, more is overkill.
 *
 * Jean-Yves Couleaud cjyves@free.fr
 */


// Subtract bottom from top and store in result as a float; separate 'red' and 'black' pixels into
// two contiguous regions
static void _heal_sub(const float *const top_buffer, const float *const bottom_buffer,
                      float *const restrict red_buffer, float *const restrict black_buffer,
                      const size_t width, const size_t height)
{
  // how many red or black pixels per line?  For consistency, we need the larger of the two, so round up
  const size_t res_stride = 4 * ((width + 1) / 2);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(top_buffer, bottom_buffer, red_buffer, black_buffer, height, width, res_stride) \
  schedule(static)
#endif
  for(size_t row = 0; row < height; row++)
  {
    const int parity = row & 1;
    const size_t row_start = (row+1) * res_stride;
    float *const buf1 = parity ? red_buffer + row_start : black_buffer + row_start;
    float *const buf2 = parity ? black_buffer + row_start : red_buffer + row_start;
    // handle the pixels of the row pairwise, one red and one black at a time
    for(size_t col = 0; col < width/2; col++)
    {
      const size_t idx = 4 * (row * width + 2*col);
      for_each_channel(c)
      {
        buf1[4*col + c] = top_buffer[idx + c] - bottom_buffer[idx + c];
        buf2[4*col + c] = top_buffer[idx+4 + c] - bottom_buffer[idx+4 + c];
      }
    }
    if(width & 1)
    {
      // Handle the left-over pixel when the total width is odd.  Its color will always be the same as the
      // left-most pixel, so it goes into buf1
      const size_t res_idx = (width-1)/2;
      const size_t idx = 4 * (row * width + (width-1));
      for_each_channel(c)
      {
        buf1[4*res_idx + c] = top_buffer[idx + c] - bottom_buffer[idx + c];
        buf2[4*res_idx + c] = 0.0f;
      }
    }
  }
  // clear the top and bottom rows, used for padding
  memset(red_buffer, 0, res_stride * sizeof(float));
  memset(red_buffer + (height+1)*res_stride, 0, res_stride * sizeof(float));
  memset(black_buffer, 0, res_stride * sizeof(float));
  memset(black_buffer + (height+1)*res_stride, 0, res_stride * sizeof(float));
}

// Add first to second and store in result, re-interleaving the 'red' and 'black' pixels
static void _heal_add(const float *const restrict red_buffer, const float *const black_buffer,
                      const float *const restrict second_buffer, float *const restrict result_buffer,
                      const size_t width, const size_t height)
{
  // how many red or black pixels per line?  For consistency, we need the larger of the two, so round up, then
  // add one to ensure a padding pixel on the right
  const size_t res_stride = 4 * ((width + 1) / 2);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(red_buffer, black_buffer, second_buffer, height, width, res_stride) \
  dt_omp_sharedconst(result_buffer) \
  schedule(static)
#endif
  for(size_t row = 0; row < height; row++)
  {
    const int parity = row & 1;
    const size_t row_start = (row+1) * res_stride;
    const float *const restrict buf1 = parity ? red_buffer + row_start : black_buffer + row_start;
    const float *const restrict buf2 = parity ? black_buffer + row_start : red_buffer + row_start;
    // handle the pixels of the row pairwise, one red and one black at a time
    for(size_t col = 0; col < width/2; col++)
    {
      const size_t idx = 4 * (row * width + 2*col);
      for_each_channel(c)
      {
        result_buffer[idx + c] = buf1[4*col + c] + second_buffer[idx + c];
        result_buffer[idx + 4 + c] = buf2[4*col + c] + second_buffer[idx + 4 + c];
      }
    }
    if(width & 1)
    {
      // handle the left-over pixel when the total width is odd
      const size_t res_idx = (width-1)/2;
      const size_t idx = 4 * (row * width + (width-1));
      for_each_channel(c)
        result_buffer[idx + c] = buf1[4*res_idx + c] + second_buffer[idx + c];
    }
  }
}

// define a custom reduction operation to handle a 3-vector of floats
// we can't return an array from a function, so wrap the array type in a struct
typedef struct _aligned_pixel { dt_aligned_pixel_t v; } _aligned_pixel;
#ifdef _OPENMP
static inline _aligned_pixel _add_float4(_aligned_pixel acc, _aligned_pixel newval)
{
  for_each_channel(c) acc.v[c] += newval.v[c];
  return acc;
}
#pragma omp declare reduction(vsum:_aligned_pixel:omp_out=_add_float4(omp_out,omp_in)) \
  initializer(omp_priv = { { 0.0f, 0.0f, 0.0f, 0.0f } })
#endif

// Perform one iteration of Gauss-Seidel, and return the sum squared residual.
static float _heal_laplace_iteration(float *const restrict active_pixels,
                                     const float *const restrict neighbor_pixels,
                                     const size_t height, const size_t width, const unsigned *const restrict runs,
                                     const size_t num_runs, const size_t start_parity, const float w)
{
  _aligned_pixel err = { { 0.f } };

  // on each iteration, we adjust each cell of the current color by a weighted fraction of the difference between
  // it and the sum of the adjacenct cells of the opposite color.  Because we've split the cells by color, this
  // leads to a somewhat different computation of neighbors.
  // Rearranged, as provided to this function:
  //   r00 r01 r02 ...           b00 b01 b02 ...
  //   r10 r11 r12               b10 b11 b12
  //   r20 r21 r22               b20 b21 b22
  //   ...                       ...
  // Original layout, from which we need to get the neighbors:
  //     r00 b00 r01 b01 r02 b02 ...
  //     b10 r10 b11 r11 b12 r12
  //     r20 b20 r21 b21 r22 b22
  //     b30 r30 b31 r31 b32 r32
  //     ...
  // As can be seen, the 'above' and 'below' neighbors of r(i)(j) are always b(i-1)(j) and b(i+1)(j).  The
  // left and right neighbors depend on which color the row starts with: if red, they are b(i)(j-1) and b(i)(j);
  // if black, they are b(i)(j) and b(i)(j+1).  All of the above holds when colors are swapped.
#if !(defined(__apple_build_version__) && __apple_build_version__ < 11030000) //makes Xcode 11.3.1 compiler crash
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(active_pixels, neighbor_pixels, runs, num_runs, width, height, start_parity, w) \
  schedule(static) \
  reduction(vsum : err)
#endif /* _OPENMP */
#endif
    for(size_t i = 0; i < num_runs; i++)
    {
      const size_t idx = runs[2*i];
      const unsigned count = runs[2*i+1];
      const size_t index = (size_t)4 * idx;
      const size_t row = idx / width;
      float a = 4.0f; // four neighboring pixels except at the edges
      if(row == 1) a -= 1.0f;  // we added a padding row at top and bottom
      if(row == height) a -= 1.0f;
      const size_t vert_offset = 4 * width;
      const size_t lroffset = 4 * (start_parity ^ (row & 1)); // how many floats to offset the left/right neighbors
      if(count == 1)
      {
        const size_t col = idx % width;
        float aa = a;
        dt_aligned_pixel_t left = { 0.0f };
        dt_aligned_pixel_t right = { 0.0f };
        if(col > 0 || lroffset) // first pixel in original stamp?
          for_each_channel(c) left[c] = neighbor_pixels[index - 4 + lroffset + c];
        else
          aa -= 1.0f;
        if(col + 1 < width || lroffset == 0) // last pixel in original stamp?
          for_each_channel(c) right[c] = neighbor_pixels[index + lroffset + c];
        else
          aa -= 1.0f;

        dt_aligned_pixel_t diff;
        for_each_channel(c, aligned(active_pixels, neighbor_pixels))
        {
          diff[c] = w * ((aa * active_pixels[index+c])
                             - (neighbor_pixels[index - vert_offset + c] + neighbor_pixels[index + vert_offset + c]
                                + left[c] + right[c]));
          active_pixels[index + c] -= diff[c];
          err.v[c] += (diff[c] * diff[c]);
        }
        continue;
      }
      dt_aligned_pixel_t left;
      copy_pixel(left, neighbor_pixels + index - 4 + lroffset);
      for(size_t j = 0; j < count; j++)
      {
        const size_t pixidx = index + 4*j;
        dt_aligned_pixel_t diff;
        dt_aligned_pixel_t right;
        for_each_channel(c, aligned(active_pixels,neighbor_pixels))
        {
          right[c] = neighbor_pixels[pixidx + lroffset + c];
          diff[c] = w * (a * active_pixels[pixidx+c]
                         - (neighbor_pixels[pixidx - vert_offset + c] + neighbor_pixels[pixidx + vert_offset + c]
                            + left[c] + right[c]));
          active_pixels[pixidx + c] -= diff[c];
          err.v[c] += (diff[c] * diff[c]);
          left[c] = right[c];
        }
      }
    }
  return err.v[0] + err.v[1] + err.v[2];
}

// convert alternating pixels of one row of the opacity mask into a set of runs of opaque pixels of the
// form (start_index, count), and return the updated number of runs
static size_t _collect_color_runs(const float *const restrict mask, const size_t start_index,
                                  size_t start, const size_t width,
                                  unsigned *const restrict runs, size_t count, size_t *nmask)
{
  size_t masked = 0;
  // handle the first and last pixels of the row specially so that we don't need checks on every pixel in the
  // adaptation loop (which will be run hundreds of times for any stamp large enough that execution time is
  // non-negligible)
  if(start == 0 && mask[start])
  {
    runs[2*count] = start_index;
    runs[2*count+1] = 1;
    count++;
    masked++;
    start += 2; // we've processed the first pixel
  }
  gboolean in_run = FALSE;
  unsigned run_start = 0;
  size_t col;
  for(col = start; col < width; col += 2)
  {
    if(mask[col])
    {
      masked++;
      if(!in_run)
      {
        run_start = col;
        in_run = TRUE;
      }
    }
    else if(in_run)
    {
      runs[2*count] = start_index + run_start / 2;
      runs[2*count + 1] = (col - run_start) / 2;
      count++;
      in_run = FALSE;
    }
  }
  if(in_run)  // finish off a run that doesn't have a zero pixel to its right
  {
    runs[2*count] = start_index + run_start / 2;
    const unsigned runlen = (col - run_start) / 2;
    runs[2*count + 1] = runlen;
    if(runlen > 1 && col > width)
    {
      // split off the final pixel into its own run
      runs[2*count + 1]--;
      runs[2*count + 2] = runs[2*count] + runs[2*count+1];
      runs[2*count + 3] = 1;
      count++;
    }
    count++;
  }
  *nmask += masked;
  return count;
}

// convert one row of the opacity mask into a set of runs of opaque pixels of the form (start_index, count)
static void collect_runs(const int start, const float *const restrict mask, const size_t width, const size_t height,
                         const size_t subwidth, unsigned *const restrict runs, size_t *count, size_t *nmask)
{
  for(size_t row = 0; row < height; row++)
  {
    const int parity = start ^ (row & 1);
    const size_t index = (row + 1) * subwidth;
    const size_t mask_index = row * width;
    *count = _collect_color_runs(mask + mask_index, index, parity, width, runs, *count, nmask);
  }
}

// Solve the laplace equation for pixels and store the result in-place.
static void _heal_laplace_loop(float *const restrict red_pixels, float *const restrict black_pixels,
                               const size_t width, const size_t height,
                               const float *const restrict mask, const int max_iter)
{
  // we start by converting the opacity mask into runs of nonzero positions, handling the 'red' and 'black'
  // checkerboarded pixels separately
  // the worst case is when consecutive red pixels alternate between being in the mask and out (same for black),
  // in which case we will need exactly as many values in the run-length encoding as pixels; any other
  // arrangement will yield fewer runs.  For any mask a user would have the patience to draw, there will only be
  // a handful of runs in each row of pixels.
  // Note that using `unsigned` instead of size_t, the stamp is limited to ~8 gigapixels (the main image can be larger)
  const size_t subwidth = (width+1)/2;  // round up to be able to handle odd widths
  unsigned *const restrict red_runs = dt_alloc_align(64, sizeof(unsigned) * subwidth * (height + 2));
  unsigned *const restrict black_runs = dt_alloc_align(64, sizeof(unsigned) * subwidth * (height + 2));
  if(!red_runs || !black_runs)
  {
    dt_print(DT_DEBUG_ALWAYS, "_heal_laplace_loop: error allocating memory for healing\n");
    goto cleanup;
  }

  size_t num_red = 0;
  size_t num_black = 0;
  size_t nmask_red = 0;
  size_t nmask_black = 0;

#ifdef _OPENMP
#pragma omp parallel sections
#endif
  {
    collect_runs(1, mask, width, height, subwidth, red_runs, &num_red, &nmask_red);
    #ifdef _OPENMP
    #pragma omp section
    #endif
    collect_runs(0, mask, width, height, subwidth, black_runs, &num_black, &nmask_black);
  }
  const size_t nmask = nmask_red + nmask_black;

  /* Empirically optimal over-relaxation factor. (Benchmarked on
   * round brushes, at least. I don't know whether aspect ratio
   * affects it.)
   */
  const float w = ((2.0f - 1.0f / (0.1575f * sqrtf(nmask) + 0.8f)) * .25f);

  const float epsilon = (0.1 / 255);
  const float err_exit = epsilon * epsilon * w * w;

  /* Gauss-Seidel with successive over-relaxation */
  for(int iter = 0; iter < max_iter; iter++)
  {
    // process red/black cells separately
    float err = _heal_laplace_iteration(black_pixels, red_pixels, height, subwidth, black_runs, num_black, 1, w);
    err += _heal_laplace_iteration(red_pixels, black_pixels, height, subwidth, red_runs, num_red, 0, w);

    if(err < err_exit) break;
  }

cleanup:
  if(red_runs) dt_free_align(red_runs);
  if(black_runs) dt_free_align(black_runs);
}


/* Original Algorithm Design:
 *
 * T. Georgiev, "Photoshop Healing Brush: a Tool for Seamless Cloning
 * http://www.tgeorgiev.net/Photoshop_Healing.pdf
 */
void dt_heal(const float *const src_buffer, float *dest_buffer, const float *const mask_buffer, const int width,
             const int height, const int ch, const int max_iter)
{
  if(ch != 4)
  {
    dt_print(DT_DEBUG_ALWAYS,"dt_heal: full-color image required\n");
    return;
  }
  const size_t subwidth = 4 * ((width+1)/2);  // round up to be able to handle odd widths
  float *const restrict red_buffer = dt_alloc_align_float(subwidth * (height + 2));
  float *const restrict black_buffer = dt_alloc_align_float(subwidth * (height + 2));
  if(red_buffer == NULL || black_buffer == NULL)
  {
    dt_print(DT_DEBUG_ALWAYS, "dt_heal: error allocating memory for healing\n");
    goto cleanup;
  }

  /* subtract pattern from image and store the result split by 'red' and 'black' positions  */
  _heal_sub(dest_buffer, src_buffer, red_buffer, black_buffer, width, height);

  _heal_laplace_loop(red_buffer, black_buffer, width, height, mask_buffer, max_iter);

  /* add solution to original image and store in dest */
  _heal_add(red_buffer, black_buffer, src_buffer, dest_buffer, width, height);

cleanup:
  if(red_buffer) dt_free_align(red_buffer);
  if(black_buffer) dt_free_align(black_buffer);
}

#ifdef HAVE_OPENCL

dt_heal_cl_global_t *dt_heal_init_cl_global()
{
  dt_heal_cl_global_t *g = (dt_heal_cl_global_t *)malloc(sizeof(dt_heal_cl_global_t));

  return g;
}

void dt_heal_free_cl_global(dt_heal_cl_global_t *g)
{
  if(!g) return;

  free(g);
}

heal_params_cl_t *dt_heal_init_cl(const int devid)
{

  heal_params_cl_t *p = (heal_params_cl_t *)malloc(sizeof(heal_params_cl_t));
  if(!p) return NULL;

  p->global = darktable.opencl->heal;
  p->devid = devid;

  return p;
}

void dt_heal_free_cl(heal_params_cl_t *p)
{
  if(!p) return;

  // be sure we're done with the memory:
  dt_opencl_finish(p->devid);

  free(p);
}

cl_int dt_heal_cl(heal_params_cl_t *p, cl_mem dev_src, cl_mem dev_dest, const float *const mask_buffer,
                  const int width, const int height, const int max_iter)
{
  cl_int err = CL_SUCCESS;

  const int ch = 4;

  float *src_buffer = NULL;
  float *dest_buffer = NULL;

  src_buffer = dt_alloc_align_float((size_t)ch * width * height);
  if(src_buffer == NULL)
  {
    dt_print(DT_DEBUG_ALWAYS, "dt_heal_cl: error allocating memory for healing\n");
    err = DT_OPENCL_SYSMEM_ALLOCATION;
    goto cleanup;
  }

  dest_buffer = dt_alloc_align_float((size_t)ch * width * height);
  if(dest_buffer == NULL)
  {
    dt_print(DT_DEBUG_ALWAYS, "dt_heal_cl: error allocating memory for healing\n");
    err = DT_OPENCL_SYSMEM_ALLOCATION;
    goto cleanup;
  }

  err = dt_opencl_read_buffer_from_device(p->devid, (void *)src_buffer, dev_src, 0,
                                          (size_t)width * height * ch * sizeof(float), CL_TRUE);
  if(err != CL_SUCCESS)
  {
    goto cleanup;
  }

  err = dt_opencl_read_buffer_from_device(p->devid, (void *)dest_buffer, dev_dest, 0,
                                          (size_t)width * height * ch * sizeof(float), CL_TRUE);
  if(err != CL_SUCCESS)
  {
    goto cleanup;
  }

  // I couldn't make it run fast on opencl (the reduction takes forever), so just call the cpu version
  dt_heal(src_buffer, dest_buffer, mask_buffer, width, height, ch, max_iter);

  err = dt_opencl_write_buffer_to_device(p->devid, dest_buffer, dev_dest, 0, sizeof(float) * width * height * ch, CL_TRUE);
  if(err != CL_SUCCESS)
  {
    goto cleanup;
  }

cleanup:
  if(src_buffer) dt_free_align(src_buffer);
  if(dest_buffer) dt_free_align(dest_buffer);

  return err;
}

#endif
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
