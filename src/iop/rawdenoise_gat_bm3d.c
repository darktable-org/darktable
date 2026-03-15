/*
    This file is part of darktable,
    Copyright (C) 2025-2026 darktable developers.

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

    GAT+BM3D raw denoise module.
    Applies Generalized Anscombe Transform to stabilize Poisson-Gaussian
    noise variance, then denoises with BM3D (hard thresholding step),
    then applies exact unbiased inverse GAT.

    References:
    - Makitalo & Foi, "Optimal inversion of the GAT for Poisson-Gaussian
      noise", IEEE TIP 2013
    - Dabov et al., "Image denoising by sparse 3-D transform-domain
      collaborative filtering", IEEE TIP 2007
    - Lebrun, "An Analysis and Implementation of the BM3D Image Denoising
      Method", IPOL 2012
*/
#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/imagebuf.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/openmp_maths.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE_INTROSPECTION(1, dt_iop_rawdenoisebm3d_params_t)

/* ============================================================
 *  Parameters
 * ============================================================ */

typedef struct dt_iop_rawdenoisebm3d_params_t
{
  float strength; // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "strength"
} dt_iop_rawdenoisebm3d_params_t;

typedef struct dt_iop_rawdenoisebm3d_gui_data_t
{
  GtkWidget *strength;
} dt_iop_rawdenoisebm3d_gui_data_t;

typedef struct dt_iop_rawdenoisebm3d_data_t
{
  float strength;
} dt_iop_rawdenoisebm3d_data_t;

typedef struct dt_iop_rawdenoisebm3d_global_data_t
{
} dt_iop_rawdenoisebm3d_global_data_t;

/* ============================================================
 *  IOP metadata
 * ============================================================ */

const char *name()
{
  return _("RAW denoise (BM3D)");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self,
    _("denoise raw Bayer data with Poisson-Gaussian noise model:\n"
      "1) RGGB channel separation\n"
      "2) noise parameter estimation (mean-variance regression)\n"
      "3) Generalized Anscombe Transform (variance stabilization)\n"
      "4) BM3D denoising (block-matching & 3D collaborative filtering)\n"
      "5) exact unbiased inverse GAT (Mäkitalo & Foi 2013)\n"
      "6) RGGB channel recombination"),
    _("corrective"),
    _("linear, raw, scene-referred"),
    _("linear, raw"),
    _("linear, raw, scene-referred"));
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_group()
{
  return IOP_GROUP_CORRECT | IOP_GROUP_TECHNICAL;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RAW;
}

/* ============================================================
 *  8x8 DCT-II  (no FFTW dependency)
 * ============================================================ */

#define PATCH_SIZE 8
#define PATCH_PIXELS (PATCH_SIZE * PATCH_SIZE)

/* Precomputed DCT-II basis: A[k][n] = alpha_k * cos(pi*(2n+1)*k / 16)
   alpha_0 = 1/sqrt(8), alpha_k = sqrt(2/8) = 1/2 for k>0 */
static float dct_basis[PATCH_SIZE][PATCH_SIZE];
static int dct_basis_initialized = 0;

static void init_dct_basis(void)
{
  if(dct_basis_initialized) return;
  for(int k = 0; k < PATCH_SIZE; k++)
  {
    const float alpha = (k == 0) ? 1.0f / sqrtf((float)PATCH_SIZE)
                                 : sqrtf(2.0f / (float)PATCH_SIZE);
    for(int n = 0; n < PATCH_SIZE; n++)
    {
      dct_basis[k][n] = alpha * cosf((float)M_PI * (2.0f * n + 1.0f) * k
                                     / (2.0f * PATCH_SIZE));
    }
  }
  dct_basis_initialized = 1;
}

/* Forward 2D DCT of an 8x8 patch (separable: rows then columns) */
static void dct2d_forward(const float *restrict in, float *restrict out)
{
  float tmp[PATCH_PIXELS];
  /* transform rows: tmp[k][n] = sum_j A[k][j] * in[n][j] */
  for(int row = 0; row < PATCH_SIZE; row++)
  {
    for(int k = 0; k < PATCH_SIZE; k++)
    {
      float s = 0.0f;
      for(int j = 0; j < PATCH_SIZE; j++)
        s += dct_basis[k][j] * in[row * PATCH_SIZE + j];
      tmp[row * PATCH_SIZE + k] = s;
    }
  }
  /* transform columns: out[k1][k2] = sum_n A[k1][n] * tmp[n][k2] */
  for(int col = 0; col < PATCH_SIZE; col++)
  {
    for(int k = 0; k < PATCH_SIZE; k++)
    {
      float s = 0.0f;
      for(int j = 0; j < PATCH_SIZE; j++)
        s += dct_basis[k][j] * tmp[j * PATCH_SIZE + col];
      out[k * PATCH_SIZE + col] = s;
    }
  }
}

/* Inverse 2D DCT (A is orthogonal, so A^-1 = A^T) */
static void dct2d_inverse(const float *restrict in, float *restrict out)
{
  float tmp[PATCH_PIXELS];
  /* inverse columns: tmp[n][col] = sum_k A[k][n] * in[k][col] = sum_k A^T[n][k] * in[k][col] */
  for(int col = 0; col < PATCH_SIZE; col++)
  {
    for(int n = 0; n < PATCH_SIZE; n++)
    {
      float s = 0.0f;
      for(int k = 0; k < PATCH_SIZE; k++)
        s += dct_basis[k][n] * in[k * PATCH_SIZE + col];
      tmp[n * PATCH_SIZE + col] = s;
    }
  }
  /* inverse rows */
  for(int row = 0; row < PATCH_SIZE; row++)
  {
    for(int n = 0; n < PATCH_SIZE; n++)
    {
      float s = 0.0f;
      for(int k = 0; k < PATCH_SIZE; k++)
        s += dct_basis[k][n] * tmp[row * PATCH_SIZE + k];
      out[row * PATCH_SIZE + n] = s;
    }
  }
}

/* ============================================================
 *  Walsh-Hadamard transform (in-place, power-of-2 length)
 * ============================================================ */

static void hadamard_transform(float *data, const int len)
{
  for(int step = 1; step < len; step *= 2)
  {
    for(int i = 0; i < len; i += 2 * step)
    {
      for(int j = i; j < i + step; j++)
      {
        const float a = data[j];
        const float b = data[j + step];
        data[j] = a + b;
        data[j + step] = a - b;
      }
    }
  }
  const float norm = 1.0f / sqrtf((float)len);
  for(int i = 0; i < len; i++)
    data[i] *= norm;
}

/* ============================================================
 *  Noise parameter estimation (mean-variance linear regression)
 * ============================================================ */

#define EST_BLOCK_SIZE 32

typedef struct noise_params_t
{
  float alpha;       /* gain (slope of variance vs mean) */
  float sigma_sq;    /* read noise variance (intercept) */
} noise_params_t;

static int compare_floats(const void *a, const void *b)
{
  const float fa = *(const float *)a;
  const float fb = *(const float *)b;
  return (fa > fb) - (fa < fb);
}

static noise_params_t estimate_noise_params(const float *channel,
                                            const int width,
                                            const int height)
{
  const int bw = width / EST_BLOCK_SIZE;
  const int bh = height / EST_BLOCK_SIZE;
  const int nblocks = bw * bh;

  noise_params_t result = { 1e-4f, 1e-6f };
  if(nblocks < 10) return result;

  float *means = dt_alloc_align_float((size_t)nblocks);
  float *vars = dt_alloc_align_float((size_t)nblocks);
  if(!means || !vars)
  {
    dt_free_align(means);
    dt_free_align(vars);
    return result;
  }

  /* compute block means and variances */
  for(int by = 0; by < bh; by++)
  {
    for(int bx = 0; bx < bw; bx++)
    {
      const int idx = by * bw + bx;
      float sum = 0.0f, sum2 = 0.0f;
      const int n = EST_BLOCK_SIZE * EST_BLOCK_SIZE;
      for(int dy = 0; dy < EST_BLOCK_SIZE; dy++)
      {
        const int row = by * EST_BLOCK_SIZE + dy;
        for(int dx = 0; dx < EST_BLOCK_SIZE; dx++)
        {
          const int col = bx * EST_BLOCK_SIZE + dx;
          const float v = channel[row * width + col];
          sum += v;
          sum2 += v * v;
        }
      }
      means[idx] = sum / n;
      vars[idx] = sum2 / n - (sum / n) * (sum / n);
    }
  }

  /* MAD-based robust linear regression: Var = alpha * Mean + sigma_sq
     Use iterative reweighted least squares with MAD outlier rejection */

  /* first pass: ordinary least squares */
  float sx = 0, sy = 0, sxx = 0, sxy = 0;
  for(int i = 0; i < nblocks; i++)
  {
    sx += means[i];
    sy += vars[i];
    sxx += means[i] * means[i];
    sxy += means[i] * vars[i];
  }
  float det = nblocks * sxx - sx * sx;
  if(fabsf(det) < 1e-12f) det = 1e-12f;
  float alpha = (nblocks * sxy - sx * sy) / det;
  float sigma_sq = (sy * sxx - sx * sxy) / det;

  /* second pass: reject outliers using MAD of residuals */
  float *residuals = dt_alloc_align_float((size_t)nblocks);
  if(residuals)
  {
    for(int i = 0; i < nblocks; i++)
      residuals[i] = fabsf(vars[i] - alpha * means[i] - sigma_sq);
    qsort(residuals, nblocks, sizeof(float), compare_floats);
    const float mad = residuals[nblocks / 2] * 1.4826f; /* MAD to sigma */
    const float threshold = 3.0f * mad;

    /* refit without outliers */
    sx = sy = sxx = sxy = 0;
    int count = 0;
    for(int i = 0; i < nblocks; i++)
    {
      const float r = fabsf(vars[i] - alpha * means[i] - sigma_sq);
      if(r < threshold)
      {
        sx += means[i];
        sy += vars[i];
        sxx += means[i] * means[i];
        sxy += means[i] * vars[i];
        count++;
      }
    }
    if(count > 5)
    {
      det = count * sxx - sx * sx;
      if(fabsf(det) > 1e-12f)
      {
        alpha = (count * sxy - sx * sy) / det;
        sigma_sq = (sy * sxx - sx * sxy) / det;
      }
    }
    dt_free_align(residuals);
  }

  /* clamp to physically meaningful values */
  result.alpha = fmaxf(alpha, 1e-6f);
  result.sigma_sq = fmaxf(sigma_sq, 0.0f);

  dt_free_align(means);
  dt_free_align(vars);
  return result;
}

/* ============================================================
 *  Generalized Anscombe Transform (GAT) and inverse
 * ============================================================ */

static inline float gat_forward(const float x, const float alpha,
                                const float sigma_sq)
{
  const float arg = alpha * fmaxf(x, 0.0f) + 0.375f * alpha * alpha + sigma_sq;
  return (2.0f / alpha) * sqrtf(fmaxf(arg, 0.0f));
}

/* Exact unbiased inverse GAT — closed-form approximation from
   Makitalo & Foi, IEEE TIP 2013 */
static inline float gat_inverse(const float D, const float alpha,
                                const float sigma_sq)
{
  if(D <= 0.0f) return 0.0f;

  /* D = (2/alpha)*sqrt(alpha*x + 3/8*alpha^2 + sigma^2)
       = 2*sqrt(x/alpha + 3/8 + sigma^2/alpha^2)
       = 2*sqrt(y + 3/8)
     where y = x/alpha + sigma^2/alpha^2.
     So D is the standard Anscombe transform of y.
     Apply the Makitalo-Foi exact unbiased inverse to recover y,
     then x = alpha*y - sigma^2/alpha. */

  const float sqrt_3_2 = 1.2247448713916f; /* sqrt(3/2) */
  const float D_inv = 1.0f / fmaxf(D, 1e-8f);
  const float D2 = D * D;
  const float D_inv2 = D_inv * D_inv;
  const float D_inv3 = D_inv2 * D_inv;

  /* standard unbiased inverse Anscombe: recovers y */
  const float y = 0.25f * D2
                + 0.25f * sqrt_3_2 * D_inv
                - 11.0f / 8.0f * D_inv2
                + 5.0f / 8.0f * sqrt_3_2 * D_inv3
                - 1.0f / 8.0f;

  /* recover x from y = x/alpha + sigma^2/alpha^2 */
  const float x_out = alpha * y - sigma_sq / alpha;

  return fmaxf(x_out, 0.0f);
}

/* ============================================================
 *  BM3D Step 1: Block matching + Hard thresholding
 * ============================================================ */

#define BM3D_STEP         3    /* step between reference patches */
#define BM3D_SEARCH_RAD   16   /* search window radius */
#define BM3D_MAX_MATCHED  16   /* max patches in a group */
#define BM3D_TAU_MATCH    400.0f  /* distance threshold (SSD, not normalized) */
#define BM3D_LAMBDA_3D    2.7f /* hard threshold factor */

/* round up to next power of 2 (max 16) */
static int next_pow2(int n)
{
  if(n <= 1) return 1;
  if(n <= 2) return 2;
  if(n <= 4) return 4;
  if(n <= 8) return 8;
  return 16;
}

/* Compute SSD between two patches in the image */
static float patch_ssd(const float *img, const int width, const int height,
                       const int r1, const int c1, const int r2, const int c2)
{
  float ssd = 0.0f;
  for(int dy = 0; dy < PATCH_SIZE; dy++)
  {
    const float *p1 = img + (r1 + dy) * width + c1;
    const float *p2 = img + (r2 + dy) * width + c2;
    for(int dx = 0; dx < PATCH_SIZE; dx++)
    {
      const float d = p1[dx] - p2[dx];
      ssd += d * d;
    }
  }
  return ssd;
}

/* Extract an 8x8 patch from image */
static void extract_patch(const float *img, const int width,
                          const int row, const int col, float *patch)
{
  for(int dy = 0; dy < PATCH_SIZE; dy++)
  {
    const float *src = img + (row + dy) * width + col;
    float *dst = patch + dy * PATCH_SIZE;
    memcpy(dst, src, PATCH_SIZE * sizeof(float));
  }
}

typedef struct match_t
{
  float dist;
  int row, col;
} match_t;

static int compare_matches(const void *a, const void *b)
{
  const float da = ((const match_t *)a)->dist;
  const float db = ((const match_t *)b)->dist;
  return (da > db) - (da < db);
}

/* BM3D step 1 on a single-channel image (half-resolution Bayer channel).
   Input and output are in GAT domain (noise sigma ≈ 1). */
static void bm3d_step1(const float *restrict input,
                       float *restrict output,
                       const int width, const int height,
                       const float sigma)
{
  const float tau = BM3D_TAU_MATCH * sigma * sigma;
  const float lambda = BM3D_LAMBDA_3D * sigma;

  /* numerator and denominator for aggregation */
  float *numer = dt_alloc_align_float((size_t)width * height);
  float *denom = dt_alloc_align_float((size_t)width * height);
  if(!numer || !denom)
  {
    dt_free_align(numer);
    dt_free_align(denom);
    /* fallback: copy input to output */
    memcpy(output, input, sizeof(float) * width * height);
    return;
  }
  memset(numer, 0, sizeof(float) * width * height);
  memset(denom, 0, sizeof(float) * width * height);

  /* iterate over reference patches */
  const int rmax = height - PATCH_SIZE;
  const int cmax = width - PATCH_SIZE;

  for(int ref_r = 0; ref_r <= rmax; ref_r += BM3D_STEP)
  {
    for(int ref_c = 0; ref_c <= cmax; ref_c += BM3D_STEP)
    {
      /* --- block matching --- */
      match_t matches[BM3D_MAX_MATCHED];
      int nmatches = 0;

      /* always include the reference patch itself */
      matches[0].dist = 0.0f;
      matches[0].row = ref_r;
      matches[0].col = ref_c;
      nmatches = 1;

      const int sr0 = MAX(0, ref_r - BM3D_SEARCH_RAD);
      const int sr1 = MIN(rmax, ref_r + BM3D_SEARCH_RAD);
      const int sc0 = MAX(0, ref_c - BM3D_SEARCH_RAD);
      const int sc1 = MIN(cmax, ref_c + BM3D_SEARCH_RAD);

      float worst_dist = tau;

      for(int sr = sr0; sr <= sr1; sr += BM3D_STEP)
      {
        for(int sc = sc0; sc <= sc1; sc += BM3D_STEP)
        {
          if(sr == ref_r && sc == ref_c) continue;

          const float d = patch_ssd(input, width, height, ref_r, ref_c, sr, sc);
          if(d < tau)
          {
            if(nmatches < BM3D_MAX_MATCHED)
            {
              matches[nmatches].dist = d;
              matches[nmatches].row = sr;
              matches[nmatches].col = sc;
              nmatches++;
            }
            else if(d < worst_dist)
            {
              /* replace worst match */
              int worst_idx = 0;
              for(int i = 1; i < nmatches; i++)
                if(matches[i].dist > matches[worst_idx].dist)
                  worst_idx = i;
              matches[worst_idx].dist = d;
              matches[worst_idx].row = sr;
              matches[worst_idx].col = sc;
              /* update worst_dist */
              worst_dist = 0;
              for(int i = 0; i < nmatches; i++)
                if(matches[i].dist > worst_dist)
                  worst_dist = matches[i].dist;
            }
          }
        }
      }

      qsort(matches, nmatches, sizeof(match_t), compare_matches);

      /* pad group to next power of 2 */
      const int group_size = next_pow2(nmatches);

      /* --- 3D collaborative filtering --- */
      /* group_dct[patch_idx][pixel] */
      float group_dct[BM3D_MAX_MATCHED][PATCH_PIXELS];
      float patch_buf[PATCH_PIXELS];

      /* extract patches and apply 2D DCT */
      for(int i = 0; i < nmatches; i++)
      {
        extract_patch(input, width, matches[i].row, matches[i].col, patch_buf);
        dct2d_forward(patch_buf, group_dct[i]);
      }
      /* pad with copies of reference */
      for(int i = nmatches; i < group_size; i++)
        memcpy(group_dct[i], group_dct[0], sizeof(float) * PATCH_PIXELS);

      /* apply 1D Hadamard along group dimension + hard threshold */
      int n_nonzero = 0;
      float had_buf[BM3D_MAX_MATCHED];

      for(int p = 0; p < PATCH_PIXELS; p++)
      {
        /* gather coefficients along group dimension */
        for(int i = 0; i < group_size; i++)
          had_buf[i] = group_dct[i][p];

        /* 1D Hadamard transform */
        hadamard_transform(had_buf, group_size);

        /* hard thresholding */
        for(int i = 0; i < group_size; i++)
        {
          if(fabsf(had_buf[i]) < lambda)
            had_buf[i] = 0.0f;
          else
            n_nonzero++;
        }

        /* inverse Hadamard (same as forward for orthogonal Hadamard) */
        hadamard_transform(had_buf, group_size);

        /* scatter back */
        for(int i = 0; i < group_size; i++)
          group_dct[i][p] = had_buf[i];
      }

      /* weight based on number of surviving coefficients */
      const float weight = 1.0f / fmaxf(1.0f, (float)n_nonzero);

      /* inverse 2D DCT and aggregate */
      for(int i = 0; i < nmatches; i++)
      {
        float denoised[PATCH_PIXELS];
        dct2d_inverse(group_dct[i], denoised);

        const int mr = matches[i].row;
        const int mc = matches[i].col;
        for(int dy = 0; dy < PATCH_SIZE; dy++)
        {
          for(int dx = 0; dx < PATCH_SIZE; dx++)
          {
            const size_t pos = (size_t)(mr + dy) * width + (mc + dx);
            numer[pos] += weight * denoised[dy * PATCH_SIZE + dx];
            denom[pos] += weight;
          }
        }
      }
    } /* ref_c */
  } /* ref_r */

  /* normalize: output = numerator / denominator */
  DT_OMP_FOR()
  for(int i = 0; i < width * height; i++)
  {
    output[i] = (denom[i] > 0.0f) ? numer[i] / denom[i] : input[i];
  }

  dt_free_align(numer);
  dt_free_align(denom);
}

/* ============================================================
 *  Main processing: extract Bayer channels, GAT, BM3D, inverse GAT
 * ============================================================ */

static void gat_bm3d_denoise(const float *const restrict in,
                             float *const restrict out,
                             const dt_iop_roi_t *const roi,
                             const float strength,
                             const uint32_t filters)
{
  const int width = roi->width;
  const int height = roi->height;

  /* copy input to output first (border pixels etc.) */
  memcpy(out, in, sizeof(float) * width * height);

  if(strength <= 0.0f) return;

  /* process each Bayer channel (R, G1, G2, B) separately */
  for(int c = 0; c < 4; c++)
  {
    const int color = FC(c & 1, c >> 1, filters);
    (void)color; /* used only for noise estimation grouping, not needed here */

    /* half-resolution dimensions for this channel */
    const int row_offset = c & 1;
    const int col_offset = (c >> 1) & 1;
    const int halfwidth = (width - col_offset + 1) / 2;
    const int halfheight = (height - row_offset + 1) / 2;

    /* need enough pixels for BM3D patches */
    if(halfwidth < PATCH_SIZE * 2 || halfheight < PATCH_SIZE * 2) continue;

    const size_t chsize = (size_t)halfwidth * halfheight;
    float *channel = dt_alloc_align_float(chsize);
    float *denoised = dt_alloc_align_float(chsize);
    if(!channel || !denoised)
    {
      dt_free_align(channel);
      dt_free_align(denoised);
      continue;
    }

    /* extract channel from Bayer mosaic */
    for(int row = row_offset; row < height; row += 2)
    {
      const int hr = (row - row_offset) / 2;
      for(int col = col_offset; col < width; col += 2)
      {
        const int hc = (col - col_offset) / 2;
        channel[hr * halfwidth + hc] = in[row * width + col];
      }
    }

    /* estimate noise parameters from this channel */
    noise_params_t np = estimate_noise_params(channel, halfwidth, halfheight);

    /* apply GAT (forward) */
    for(size_t i = 0; i < chsize; i++)
      channel[i] = gat_forward(channel[i], np.alpha, np.sigma_sq);

    /* BM3D denoise (sigma = 1.0 in GAT domain, scaled by strength) */
    bm3d_step1(channel, denoised, halfwidth, halfheight, 1.0f * strength);

    /* apply inverse GAT */
    for(size_t i = 0; i < chsize; i++)
      denoised[i] = gat_inverse(denoised[i], np.alpha, np.sigma_sq);

    /* write back to Bayer mosaic */
    for(int row = row_offset; row < height; row += 2)
    {
      const int hr = (row - row_offset) / 2;
      for(int col = col_offset; col < width; col += 2)
      {
        const int hc = (col - col_offset) / 2;
        out[row * width + col] = denoised[hr * halfwidth + hc];
      }
    }

    dt_free_align(channel);
    dt_free_align(denoised);
  }
}

/* ============================================================
 *  IOP process
 * ============================================================ */

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  const dt_iop_rawdenoisebm3d_data_t *const d = piece->data;

  if(!(d->strength > 0.0f))
  {
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_in->width, roi_in->height, piece->colors);
    return;
  }

  const uint32_t filters = piece->filters;
  if(filters == 9u)
  {
    /* X-Trans not supported — pass through */
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_in->width, roi_in->height, piece->colors);
    return;
  }

  init_dct_basis();
  gat_bm3d_denoise(ivoid, ovoid, roi_in, d->strength, filters);
}

/* ============================================================
 *  IOP lifecycle
 * ============================================================ */

void init(dt_iop_module_t *self)
{
  dt_iop_default_init(self);
}

void reload_defaults(dt_iop_module_t *self)
{
  self->hide_enable_button = !dt_image_is_raw(&self->dev->image_storage);
  if(self->widget)
    gtk_stack_set_visible_child_name(GTK_STACK(self->widget),
                                    self->hide_enable_button ? "non_raw" : "raw");
  self->default_enabled = FALSE;
}

void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_rawdenoisebm3d_data_t));
}

void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *params,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_rawdenoisebm3d_params_t *p = (dt_iop_rawdenoisebm3d_params_t *)params;
  dt_iop_rawdenoisebm3d_data_t *d = piece->data;
  d->strength = p->strength;

  if(!(dt_image_is_raw(&pipe->image)))
    piece->enabled = FALSE;
}

/* ============================================================
 *  GUI
 * ============================================================ */

void gui_init(dt_iop_module_t *self)
{
  dt_iop_rawdenoisebm3d_gui_data_t *g = IOP_GUI_ALLOC(rawdenoisebm3d);

  /* slider */
  g->strength = dt_bauhaus_slider_from_params(self, "strength");
  dt_bauhaus_slider_set_soft_range(g->strength, 0.0, 2.0);
  dt_bauhaus_slider_set_digits(g->strength, 2);
  gtk_widget_set_tooltip_text(g->strength,
    _("denoising strength\n"
      "0.0 = disabled\n"
      "0.5 = subtle denoising\n"
      "1.0 = standard (matched to estimated noise)\n"
      "1.5-2.0 = aggressive denoising"));

  GtkWidget *box_raw = self->widget;

  /* raw / non-raw switching (same pattern as rawdenoise.c) */
  self->widget = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(self->widget), FALSE);
  GtkWidget *label_non_raw = dt_ui_label_new(
    _("BM3D raw denoising\nonly works for raw images."));
  gtk_stack_add_named(GTK_STACK(self->widget), label_non_raw, "non_raw");
  gtk_stack_add_named(GTK_STACK(self->widget), box_raw, "raw");
}

void gui_cleanup(dt_iop_module_t *self)
{
  /* nothing to clean up beyond default */
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
