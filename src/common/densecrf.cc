/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

// Mean-field DenseCRF for binary mask refinement.
// Krähenbühl & Koltun, NIPS 2011 (https://arxiv.org/abs/1210.5644).

#include <exception>
#include <math.h>
#ifdef _OPENMP
#include <omp.h>
#endif

#include "common/darktable.h"
#include "common/densecrf.h"

#include "iop/Permutohedral.h"


// one mean-field iteration: re-splat the foreground probability + a
// normalisation channel via cached replay, blur, slice and normalise
template <int D>
static void _mf_filter(PermutohedralLattice<D, 2> &lattice,
                       const float *const __restrict__ probs,
                       float *const __restrict__ out,
                       const int npixels)
{
  lattice.clearValues();

  // splat is sequential — multiple pixels can map to the same lattice
  // vertex, parallel writes would race
  for(int i = 0; i < npixels; i++)
  {
    const float val[2] = { probs[i], 1.0f };
    lattice.quickSplat(val, (size_t)i);
  }

  lattice.blur();

  DT_OMP_FOR(shared(lattice))
  for(int i = 0; i < npixels; i++)
  {
    float val[2];
    lattice.slice(val, (size_t)i);
    out[i] = (val[1] > 1e-10f) ? (val[0] / val[1]) : 0.0f;
  }
}


// initial multi-threaded splat: builds lattice topology AND splats
// iteration-0 values in one pass; subsequent iterations reuse via
// quickSplat
template <int D>
static void _initial_splat(PermutohedralLattice<D, 2> &lattice,
                           const float *const __restrict__ features,
                           const float *const __restrict__ probs,
                           const int npixels)
{
  DT_OMP_FOR(shared(lattice))
  for(int i = 0; i < npixels; i++)
  {
    float pos[D];
    for(int d = 0; d < D; d++) pos[d] = features[i * D + d];
    const float val[2] = { probs[i], 1.0f };
    const int thread = dt_get_thread_num();
    lattice.splat(pos, const_cast<float *>(val), (size_t)i, thread);
  }
  // single-threaded: combines per-thread hash tables into hashTables[0]
  lattice.merge_splat_threads();
}


template <int D>
static void _slice_normalize(PermutohedralLattice<D, 2> &lattice,
                             float *const __restrict__ out,
                             const int npixels)
{
  DT_OMP_FOR(shared(lattice))
  for(int i = 0; i < npixels; i++)
  {
    float val[2];
    lattice.slice(val, (size_t)i);
    out[i] = (val[1] > 1e-10f) ? (val[0] / val[1]) : 0.0f;
  }
}


extern "C" void dt_dense_crf_binary(float *probabilities,
                                    const unsigned char *rgb,
                                    const int width,
                                    const int height,
                                    const float sigma_spatial,
                                    const float sigma_rgb,
                                    const float w_spatial,
                                    const float w_bilateral,
                                    const int n_iterations)
{
  if(!probabilities || !rgb
     || width <= 0 || height <= 0
     || n_iterations <= 0)
    return;

  const int n = width * height;
  const float inv_sigma_s = 1.0f / sigma_spatial;
  const float inv_sigma_c = 1.0f / sigma_rgb;

  float *__restrict__ spatial_features
    = (float *)dt_alloc_aligned((size_t)n * 2 * sizeof(float));
  float *__restrict__ bilateral_features
    = (float *)dt_alloc_aligned((size_t)n * 5 * sizeof(float));
  float *const __restrict__ unary
    = (float *)dt_alloc_aligned((size_t)n * sizeof(float));
  float *const __restrict__ msg_spatial
    = (float *)dt_alloc_aligned((size_t)n * sizeof(float));
  float *const __restrict__ msg_bilateral
    = (float *)dt_alloc_aligned((size_t)n * sizeof(float));

  if(!spatial_features || !bilateral_features
     || !unary || !msg_spatial || !msg_bilateral)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[densecrf] failed to allocate scratch buffers for %dx%d",
             width, height);
    dt_free_align(spatial_features);
    dt_free_align(bilateral_features);
    dt_free_align(unary);
    dt_free_align(msg_spatial);
    dt_free_align(msg_bilateral);
    return;
  }

  // features pre-scaled by 1/sigma so the lattice's implicit Gaussian
  // has unit standard deviation in feature space
  DT_OMP_FOR(collapse(2))
  for(int y = 0; y < height; y++)
  {
    for(int x = 0; x < width; x++)
    {
      const int i = y * width + x;
      const float xs = (float)x * inv_sigma_s;
      const float ys = (float)y * inv_sigma_s;
      spatial_features[i * 2 + 0] = xs;
      spatial_features[i * 2 + 1] = ys;
      bilateral_features[i * 5 + 0] = xs;
      bilateral_features[i * 5 + 1] = ys;
      bilateral_features[i * 5 + 2] = (float)rgb[i * 3 + 0] * inv_sigma_c;
      bilateral_features[i * 5 + 3] = (float)rgb[i * 3 + 1] * inv_sigma_c;
      bilateral_features[i * 5 + 4] = (float)rgb[i * 3 + 2] * inv_sigma_c;
    }
  }

  DT_OMP_FOR()
  for(int i = 0; i < n; i++)
  {
    const float p = fminf(fmaxf(probabilities[i], 1e-6f), 1.0f - 1e-6f);
    unary[i] = logf(p / (1.0f - p));
  }

  const size_t n_threads = (size_t)dt_get_num_threads();
  // grid_points hint for hash-table sizing — over-estimate is harmless,
  // under-estimate triggers an internal re-grow
  const size_t spatial_grid = (size_t)((float)height * inv_sigma_s
                                       * (float)width * inv_sigma_s);
  const size_t bilateral_grid = (size_t)((float)height * inv_sigma_s
                                         * (float)width * inv_sigma_s
                                         * inv_sigma_c * inv_sigma_c * inv_sigma_c
                                         * 256.0f * 256.0f * 256.0f);

  // Permutohedral lattices use `new` internally and can throw std::bad_alloc
  // on OOM; catch here so the exception never crosses the C boundary
  try
  {
    PermutohedralLattice<2, 2> spatial(n, n_threads, spatial_grid);
    PermutohedralLattice<5, 2> bilateral(n, n_threads, bilateral_grid);

    _initial_splat(spatial, spatial_features, probabilities, n);
    _initial_splat(bilateral, bilateral_features, probabilities, n);

    dt_free_align(spatial_features);   spatial_features = NULL;
    dt_free_align(bilateral_features); bilateral_features = NULL;

    for(int iter = 0; iter < n_iterations; iter++)
    {
      if(iter == 0)
      {
        // iter 0: lattices have initial splat values, just blur+slice
        spatial.blur();
        bilateral.blur();
        _slice_normalize(spatial, msg_spatial, n);
        _slice_normalize(bilateral, msg_bilateral, n);
      }
      else
      {
        _mf_filter(spatial, probabilities, msg_spatial, n);
        _mf_filter(bilateral, probabilities, msg_bilateral, n);
      }

      // binary Potts mean-field update:
      //   log Q(1)/Q(0) = unary_logit + Σ_k w_k · (2·filtered(p) − 1)
      DT_OMP_FOR()
      for(int i = 0; i < n; i++)
      {
        const float msg
          = w_spatial * (2.0f * msg_spatial[i] - 1.0f)
          + w_bilateral * (2.0f * msg_bilateral[i] - 1.0f);
        const float energy = unary[i] + msg;
        probabilities[i] = 1.0f / (1.0f + expf(-energy));
      }
    }
  }
  catch(const std::exception &e)
  {
    dt_print(DT_DEBUG_ALWAYS, "[densecrf] aborted: %s", e.what());
  }
  catch(...)
  {
    dt_print(DT_DEBUG_ALWAYS, "[densecrf] aborted: unknown exception");
  }

  dt_free_align(spatial_features);
  dt_free_align(bilateral_features);
  dt_free_align(unary);
  dt_free_align(msg_spatial);
  dt_free_align(msg_bilateral);
}
