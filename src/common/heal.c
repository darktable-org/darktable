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


// Subtract bottom from top and store in result as a float
static void dt_heal_sub(const float *const top_buffer, const float *const bottom_buffer,
                        float *const restrict result_buffer, const int width, const int height)
{
  const size_t i_size = (size_t)width * height * 4;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(top_buffer, bottom_buffer, i_size) \
  dt_omp_sharedconst(result_buffer) \
  schedule(static)
#endif
  for(size_t i = 0; i < i_size; i++) result_buffer[i] = top_buffer[i] - bottom_buffer[i];
}

// Add first to second and store in result
static void dt_heal_add(const float *const restrict first_buffer, const float *const restrict second_buffer,
                        float *const restrict result_buffer, const int width, const int height)
{
  const size_t i_size = (size_t)width * height * 4;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(first_buffer, second_buffer, i_size) \
  dt_omp_sharedconst(result_buffer) \
  schedule(static)
#endif
  for(size_t i = 0; i < i_size; i++) result_buffer[i] = first_buffer[i] + second_buffer[i];
}

// define a custom reduction operation to handle a 3-vector of floats
// we can't return an array from a function, so wrap the array type in a struct
typedef struct _aligned_pixel { dt_aligned_pixel_t v; } _aligned_pixel;
#ifdef _OPENMP
static inline _aligned_pixel add_float4(_aligned_pixel acc, _aligned_pixel newval)
{
  for_each_channel(c) acc.v[c] += newval.v[c];
  return acc;
}
#pragma omp declare reduction(vsum:_aligned_pixel:omp_out=add_float4(omp_out,omp_in)) \
  initializer(omp_priv = { { 0.0f, 0.0f, 0.0f, 0.0f } })
#endif

// Perform one iteration of Gauss-Seidel, and return the sum squared residual.
static float dt_heal_laplace_iteration(float *const restrict pixels, const float *const restrict Adiag,
                                       const int *const restrict Aidx, const float w,
                                       const int nmask_from, const int nmask_to)
{
  _aligned_pixel err = { { 0.f } };

#if !(defined(__apple_build_version__) && __apple_build_version__ < 11030000) //makes Xcode 11.3.1 compiler crash
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(Adiag, Aidx, w, nmask_from, nmask_to) \
  dt_omp_sharedconst(pixels) \
  schedule(static) \
  reduction(vsum : err)
#endif
#endif
  for(int i = nmask_from; i < nmask_to; i++)
  {
    const size_t j0 = Aidx[i * 5 + 0];
    const size_t j1 = Aidx[i * 5 + 1];
    const size_t j2 = Aidx[i * 5 + 2];
    const size_t j3 = Aidx[i * 5 + 3];
    const size_t j4 = Aidx[i * 5 + 4];
    const float a = Adiag[i];

    dt_aligned_pixel_t diff;
    for_each_channel(k,aligned(pixels))
    {
      diff[k] = w * (a * pixels[j0 + k] - (pixels[j1 + k] + pixels[j2 + k] + pixels[j3 + k] + pixels[j4 + k]));
      pixels[j0 + k] -= diff[k];
      err.v[k] += diff[k] * diff[k];
    }
  }

  return err.v[0] + err.v[1] + err.v[2];
}

// Solve the laplace equation for pixels and store the result in-place.
static void dt_heal_laplace_loop(float *pixels, const int width, const int height,
                                 const float *const mask)
{
  int nmask = 0;
  int nmask2 = 0;

  float *Adiag = dt_alloc_align_float((size_t)width * height);
  int *Aidx = dt_alloc_align(64, sizeof(int) * 5 * width * height);

  if((Adiag == NULL) || (Aidx == NULL))
  {
    fprintf(stderr, "dt_heal_laplace_loop: error allocating memory for healing\n");
    goto cleanup;
  }

  /* All off-diagonal elements of A are either -1 or 0. We could store it as a
   * general-purpose sparse matrix, but that adds some unnecessary overhead to
   * the inner loop. Instead, assume exactly 4 off-diagonal elements in each
   * row, all of which have value -1. Any row that in fact wants less than 4
   * coefs can put them in a dummy column to be multiplied by an empty pixel.
   */
  const int zero = 4 * width * height;
  memset(pixels + zero, 0, sizeof(float) * 4);

  /* Construct the system of equations.
   * Arrange Aidx in checkerboard order, so that a single linear pass over that
   * array results updating all of the red cells and then all of the black cells.
   */
  for(int parity = 0; parity < 2; parity++)
  {
    if(parity == 1) nmask2 = nmask;

    for(int i = 0; i < height; i++)
    {
      for(int j = (i & 1) ^ parity; j < width; j += 2)
      {
        if(mask[j + i * width])
        {
#define A_POS(di, dj) (((i + di) * width + (j  + dj)) * 4)

          /* Omit Dirichlet conditions for any neighbors off the
           * edge of the canvas.
           */
          Adiag[nmask] = 4 - (i == 0) - (j == 0) - (i == height - 1) - (j == width - 1);
          Aidx[5 * nmask] = A_POS(0,0);
          Aidx[5 * nmask + 1] = (j == width-1) ? zero : A_POS(0,1);
          Aidx[5 * nmask + 2] = (i == height-1) ? zero : A_POS(1,0);
          Aidx[5 * nmask + 3] = (j == 0) ? zero : A_POS(0,-1);
          Aidx[5 * nmask + 4] = (i == 0) ? zero : A_POS(-1,0);
          nmask++;
        }
      }
    }
  }

#undef A_POS

  /* Empirically optimal over-relaxation factor. (Benchmarked on
   * round brushes, at least. I don't know whether aspect ratio
   * affects it.)
   */
  const float w = ((2.0f - 1.0f / (0.1575f * sqrtf(nmask) + 0.8f)) * .25f);

  const int max_iter = 1000;
  const float epsilon = (0.1 / 255);
  const float err_exit = epsilon * epsilon * w * w;

  /* Gauss-Seidel with successive over-relaxation */
  for(int iter = 0; iter < max_iter; iter++)
  {
    // process red/black cells separate
    float err = dt_heal_laplace_iteration(pixels, Adiag, Aidx, w, 0, nmask2);
    err += dt_heal_laplace_iteration(pixels, Adiag, Aidx, w, nmask2, nmask);

    if(err < err_exit) break;
  }

cleanup:
  if(Adiag) dt_free_align(Adiag);
  if(Aidx) dt_free_align(Aidx);
}


/* Original Algorithm Design:
 *
 * T. Georgiev, "Photoshop Healing Brush: a Tool for Seamless Cloning
 * http://www.tgeorgiev.net/Photoshop_Healing.pdf
 */
void dt_heal(const float *const src_buffer, float *dest_buffer, const float *const mask_buffer, const int width,
             const int height, const int ch)
{
  if(ch != 4)
  {
    fprintf(stderr,"dt_heal: full-color image required\n");
    return;
  }
  float *const restrict diff_buffer = dt_alloc_align_float((size_t)ch * width * (height + 1));

  if(diff_buffer == NULL)
  {
    fprintf(stderr, "dt_heal: error allocating memory for healing\n");
    goto cleanup;
  }

  /* subtract pattern from image and store the result in diff */
  dt_heal_sub(dest_buffer, src_buffer, diff_buffer, width, height);

  dt_heal_laplace_loop(diff_buffer, width, height, mask_buffer);

  /* add solution to original image and store in dest */
  dt_heal_add(diff_buffer, src_buffer, dest_buffer, width, height);

cleanup:
  if(diff_buffer) dt_free_align(diff_buffer);
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
                  const int width, const int height)
{
  cl_int err = CL_SUCCESS;

  const int ch = 4;

  float *src_buffer = NULL;
  float *dest_buffer = NULL;

  src_buffer = dt_alloc_align_float((size_t)ch * width * height);
  if(src_buffer == NULL)
  {
    fprintf(stderr, "dt_heal_cl: error allocating memory for healing\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  dest_buffer = dt_alloc_align_float((size_t)ch * width * height);
  if(dest_buffer == NULL)
  {
    fprintf(stderr, "dt_heal_cl: error allocating memory for healing\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
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
  dt_heal(src_buffer, dest_buffer, mask_buffer, width, height, ch);

  err = dt_opencl_write_buffer_to_device(p->devid, dest_buffer, dev_dest, 0, sizeof(float) * width * height * ch,
                                         TRUE);
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
