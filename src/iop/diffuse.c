/*
   This file is part of darktable,
   Copyright (C) 2019-2020 darktable developers.

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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/gaussian.h"
#include "common/image.h"
#include "common/iop_profile.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop_gui.h"
#include "develop/imageop_math.h"
#include "develop/noise_generator.h"
#include "develop/openmp_maths.h"
#include "dtgtk/button.h"
#include "dtgtk/drawingarea.h"
#include "dtgtk/expander.h"
#include "dtgtk/paint.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

DT_MODULE_INTROSPECTION(1, dt_iop_diffuse_params_t)

#define MAX_NUM_SCALES 12

typedef enum dt_iop_diffuse_model_t
{
  DT_DIFFUSE_GAUSSIAN    = 0,   // $DESCRIPTION: "gaussian (natural)"
  DT_DIFFUSE_CONSTANT    = 1,   // $DESCRIPTION: "constant"
  DT_DIFFUSE_LINEAR      = 2,   // $DESCRIPTION: "linear"
  DT_DIFFUSE_QUADRATIC   = 3    // $DESCRIPTION: "quadratic"
} dt_iop_diffuse_model_t;


typedef struct dt_iop_diffuse_params_t
{
  int iterations;       // $MIN: 1   $MAX: 20   $DEFAULT: 1  $DESCRIPTION: "iterations of diffusion"
  float texture;        // $MIN: -1. $MAX: 1.   $DEFAULT: 0. $DESCRIPTION: "texture"
  float structure;      // $MIN: -1. $MAX: 1.   $DEFAULT: 0. $DESCRIPTION: "structure"
  float edges;          // $MIN: -12. $MAX: 12.   $DEFAULT: 0. $DESCRIPTION: "edge directivity"
  int radius;           // $MIN: 1   $MAX: 1024 $DEFAULT: 8  $DESCRIPTION: "radius of diffusion"
  float update;         // $MIN: 0.  $MAX: 1.   $DEFAULT: 1. $DESCRIPTION: "speed of diffusion"
  float threshold;      // $MIN: 0.  $MAX: 8.   $DEFAULT: 0. $DESCRIPTION: "luminance masking threshold"
  float regularization; // $MIN: -12. $MAX: 12.   $DEFAULT: 0. $DESCRIPTION: "edge regularization"
  dt_iop_diffuse_model_t model;
} dt_iop_diffuse_params_t;


typedef struct dt_iop_diffuse_gui_data_t
{
  GtkWidget *iterations, *texture, *structure, *fine, *coarse, *edges, *radius, *update, *model, *threshold,
      *regularization;
} dt_iop_diffuse_gui_data_t;


// only copy params struct to avoid a commit_params()
typedef struct dt_iop_diffuse_params_t dt_iop_diffuse_data_t;

const char *name()
{
  return _("diffuse or sharpen");
}

const char *aliases()
{
  return _("diffusion|deconvolution|blur|sharpening");
}

const char *description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("simulate directional diffusion of light with heat transfer model\n"
                                        "to apply an iterative edge-oriented blur, \n"
                                        "inpaint damaged parts of the image,\n"
                                        "or to remove blur with blind deconvolution."),
                                      _("corrective and creative"),
                                      _("linear, RGB, scene-referred"),
                                      _("linear, RGB"),
                                      _("linear, RGB, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_EFFECTS;
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_rgb;
}


void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_diffuse_params_t p;
  memset(&p, 0, sizeof(p));

  p.iterations = 4;
  p.texture = -0.25f;
  p.structure = -1.f;
  p.edges = 4.f;
  p.radius = 16;
  p.update = 1.f;
  p.threshold = 0.f;
  p.regularization = 4.f;
  p.model = DT_DIFFUSE_GAUSSIAN;
  dt_gui_presets_add_generic(_("sharpen"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  p.iterations = 4;
  p.texture = 0.25f;
  p.structure = -1.f;
  p.edges = 4.f;
  p.radius = 16;
  p.update = 1.f;
  p.threshold = 0.f;
  p.regularization = 4.f;
  p.model = DT_DIFFUSE_GAUSSIAN;
  dt_gui_presets_add_generic(_("sharpen and denoise"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  p.iterations = 4;
  p.texture = 0.5f;
  p.structure = 1.f;
  p.edges = 0.f;
  p.radius = 128;
  p.update = 1.f;
  p.threshold = 0.f;
  p.regularization = 0.f;
  p.model = DT_DIFFUSE_GAUSSIAN;
  dt_gui_presets_add_generic(_("diffuse"), self->op, self->version(), &p, sizeof(p), 1,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  p.iterations = 20;
  p.texture = 0.25f;
  p.structure = 1.f;
  p.edges = 4.f;
  p.radius = 1024;
  p.update = 1.f;
  p.threshold = 0.99f;
  p.regularization = -12.f;
  p.model = DT_DIFFUSE_CONSTANT;
  dt_gui_presets_add_generic(_("inpaint highlights"), self->op, self->version(), &p, sizeof(p), 1,
                             DEVELOP_BLEND_CS_RGB_SCENE);
}

// B spline filter
#define FSIZE 5


#ifdef _OPENMP
#pragma omp declare simd aligned(buf, indices, result:64)
#endif
inline static void sparse_scalar_product(const float *const buf, const size_t indices[FSIZE], float result[4])
{
  // scalar product of 2 3×5 vectors stored as RGB planes and B-spline filter,
  // e.g. RRRRR - GGGGG - BBBBB

  const float DT_ALIGNED_ARRAY filter[FSIZE] =
                        { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };

  #ifdef _OPENMP
  #pragma omp simd aligned(buf, filter:64) aligned(result:16)
  #endif
  for(int c = 0; c < 4; ++c)
  {
    float acc = 0.0f;
    for(size_t k = 0; k < FSIZE; ++k)
      acc += filter[k] * buf[indices[k] + c];
    result[c] = acc;
  }
}

//TODO: consolidate with the copy of this code in src/common/dwt.c
static inline int dwt_interleave_rows(const size_t rowid, const size_t height, const size_t scale)
{
  // to make this algorithm as cache-friendly as possible, we want to interleave the actual processing of rows
  // such that the next iteration processes the row 'scale' pixels below the current one, which will already
  // be in L2 cache (if not L1) from having been accessed on this iteration so if vscale is 16, we want to
  // process rows 0, 16, 32, ..., then 1, 17, 33, ..., 2, 18, 34, ..., etc.
  if (height <= scale)
    return rowid;
  const size_t per_pass = ((height + scale - 1) / scale);
  const size_t long_passes = height % scale;
  // adjust for the fact that we have some passes with one fewer iteration when height is not a multiple of scale
  if (long_passes == 0 || rowid < long_passes * per_pass)
    return (rowid / per_pass) + scale * (rowid % per_pass);
  const size_t rowid2 = rowid - long_passes * per_pass;
  return long_passes + (rowid2 / (per_pass-1)) + scale * (rowid2 % (per_pass-1));
}

#ifdef _OPENMP
#pragma omp declare simd aligned(in, out:64) aligned(tempbuf:16)
#endif
inline static void blur_2D_Bspline(const float *const restrict in, float *const restrict out,
                                   float *const restrict tempbuf,
                                   const size_t width, const size_t height, const int mult)
{
  // À-trous B-spline interpolation/blur shifted by mult
  #ifdef _OPENMP
  #pragma omp parallel for default(none) \
    dt_omp_firstprivate(width, height, mult)  \
    dt_omp_sharedconst(out, in, tempbuf) \
    schedule(simd:static)
  #endif
  for(size_t row = 0; row < height; row++)
  {
    // get a thread-private one-row temporary buffer
    float *const temp = tempbuf + 4 * width * dt_get_thread_num();
    // interleave the order in which we process the rows so that we minimize cache misses
    const size_t i = dwt_interleave_rows(row, height, mult);
    // Convolve B-spline filter over columns: for each pixel in the current row, compute vertical blur
    size_t DT_ALIGNED_ARRAY indices[FSIZE] = { 0 };
    // Start by computing the array indices of the pixels of interest; the offsets from the current pixel stay
    // unchanged over the entire row, so we can compute once and just offset the base address while iterating
    // over the row
    for(size_t ii = 0; ii < FSIZE; ++ii)
    {
      const size_t r = CLAMP((int)i + mult * (int)(ii - (FSIZE - 1) / 2), (int)0, (int)height - 1);
      indices[ii] = 4 * r * width;
    }
    for(size_t j = 0; j < width; j++)
    {
      // Compute the vertical blur of the current pixel and store it in the temp buffer for the row
      sparse_scalar_product(in + j * 4, indices, temp + j * 4);
    }
    // Convolve B-spline filter horizontally over current row
    for(size_t j = 0; j < width; j++)
    {
      // Compute the array indices of the pixels of interest; since the offsets will change near the ends of
      // the row, we need to recompute for each pixel
      for(size_t jj = 0; jj < FSIZE; ++jj)
      {
        const size_t col = CLAMP((int)j + mult * (int)(jj - (FSIZE - 1) / 2), (int)0, (int)width - 1);
        indices[jj] = 4 * col;
      }
      // Compute the horizonal blur of the already vertically-blurred pixel and store the result at the proper
      //  row/column location in the output buffer
      sparse_scalar_product(temp, indices, out + (i * width + j) * 4);
    }
  }
}

static inline void init_reconstruct(float *const restrict reconstructed, const size_t width, const size_t height,
                                    const size_t ch)
{
// init the reconstructed buffer with non-clipped and partially clipped pixels
// Note : it's a simple multiplied alpha blending where mask = alpha weight
#ifdef _OPENMP
#pragma omp parallel for simd default(none) dt_omp_firstprivate(reconstructed, width, height, ch)       \
    schedule(simd : static) aligned(reconstructed : 64)
#endif
  for(size_t k = 0; k < height * width * ch; k++)
  {
    reconstructed[k] = 0.f;
  }
}


static inline void wavelets_detail_level(const float *const restrict detail, const float *const restrict LF,
                                         float *const restrict HF,
                                         const size_t width, const size_t height, const size_t ch)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) dt_omp_firstprivate(width, height, HF, LF, detail)           \
    schedule(simd                                                                                                 \
             : static) aligned(HF, LF, detail : 64)
#endif
  for(size_t k = 0; k < height * width; k++)
    for(size_t c = 0; c < 4; ++c) HF[4*k + c] = detail[4*k + c] - LF[4*k + c];
}

static int get_scales(const dt_iop_roi_t *roi_in, const dt_dev_pixelpipe_iop_t *const piece)
{
  /* How many wavelets scales do we need to compute at current zoom level ?
   * 0. To get the same preview no matter the zoom scale, the relative image coverage ratio of the filter at
   * the coarsest wavelet level should always stay constant.
   * 1. The image coverage of each B spline filter of size `FSIZE` is `2^(level) * (FSIZE - 1) / 2 + 1` pixels
   * 2. The coarsest level filter at full resolution should cover `1/FSIZE` of the largest image dimension.
   * 3. The coarsest level filter at current zoom level should cover `scale/FSIZE` of the largest image dimension.
   *
   * So we compute the level that solves 1. subject to 3. Of course, integer rounding doesn't make that 1:1
   * accurate.
   */
  const float scale = roi_in->scale / piece->iscale;
  const size_t size = MAX(piece->buf_in.height * piece->iscale, piece->buf_in.width * piece->iscale);
  const int scales = floorf(log2f((2.0f * size * scale / ((FSIZE - 1) * FSIZE)) - 1.0f));
  return CLAMP(scales, 1, MAX_NUM_SCALES);
}


inline static void wavelets_reconstruct_RGB(const float *const restrict HF, const float *const restrict LF,
                                            float *const restrict reconstructed, const size_t width,
                                            const size_t height, const size_t ch, const size_t s, const size_t scales)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none)                                                          \
    dt_omp_firstprivate(width, height, ch, HF, LF, reconstructed, s, scales) schedule(simd : static) \
    aligned(reconstructed, HF:64)
#endif
  for(size_t k = 0; k < height * width * ch; ++k)
  {
    reconstructed[k] += (s == scales - 1) ? HF[k] + LF[k] : HF[k];
  }
}

static void heat_PDE_inpanting(const float *const restrict input,
                               float *const restrict output, const uint8_t *const restrict mask,
                               const size_t width, const size_t height, const size_t ch, const int mult,
                               const float texture, const float structure, const float edges, const float regularization)
{
  // Simultaneous inpainting for image structure and texture using anisotropic heat transfer model
  // https://www.researchgate.net/publication/220663968

  // Discretization parameters for the Partial Derivative Equation solver
  const int h = 1;              // spatial step
  const float kappa = 0.25f;    // 0.25 if h = 1, 1 if h = 2

  const float A = texture * kappa;
  const float B = structure * kappa;
  const float K = edges;
  const float L = regularization;

  const int compute_texture = (A != 0.f);
  const int compute_structure = (B != 0.f);

  const int has_mask = (mask != NULL);

  #ifdef _OPENMP
  #pragma omp parallel for default(none) \
  dt_omp_firstprivate(input, output, mask, height, width, ch, K, L, A, B, h, compute_structure, compute_texture, has_mask, mult) \
  schedule(dynamic) collapse(2)
  #endif
  for(size_t i = 0; i < height; ++i)
    for(size_t j = 0; j < width; ++j)
    {
      const size_t idx = (i * width + j);
      const size_t index = idx * ch;
      uint8_t opacity = (has_mask) ? mask[idx] : 1;

      if(opacity)
      {
        // neighbours
        const size_t j_prev = CLAMP((int)(j - h), (int)0, (int)width - 1); // y
        const size_t j_next = CLAMP((int)(j + h), (int)0, (int)width - 1); // y
        const size_t i_prev = CLAMP((int)(i - h), (int)0, (int)height - 1); // x
        const size_t i_next = CLAMP((int)(i + h), (int)0, (int)height - 1); // x

        const size_t j_far_prev = CLAMP((int)(j - mult * h), (int)0, (int)width - 1); // y
        const size_t j_far_next = CLAMP((int)(j + mult * h), (int)0, (int)width - 1); // y
        const size_t i_far_prev = CLAMP((int)(i - mult * h), (int)0, (int)height - 1); // x
        const size_t i_far_next = CLAMP((int)(i + mult * h), (int)0, (int)height - 1); // x

        const size_t DT_ALIGNED_ARRAY idx_grad[9]
            = { (i_prev * width + j_prev) * ch, (i_prev * width + j) * ch, (i_prev * width + j_next) * ch,
                (i * width + j_prev) * ch,      (i * width + j) * ch,      (i* width + j_next) * ch,
                (i_next * width + j_prev) * ch, (i_next * width + j) * ch, (i_next * width + j_next) * ch };

        const size_t DT_ALIGNED_ARRAY idx_lapl[9]
            = { (i_far_prev * width + j_far_prev) * ch, (i_far_prev * width + j) * ch, (i_far_prev * width + j_far_next) * ch,
                (i * width + j_far_prev) * ch,      (i * width + j) * ch,      (i* width + j_far_next) * ch,
                (i_far_next * width + j_far_prev) * ch, (i_far_next * width + j) * ch, (i_far_next * width + j_far_next) * ch };

        float DT_ALIGNED_ARRAY kern_grad[9][4] = { { 0.f } };
        float DT_ALIGNED_ARRAY kern_lap[9][4] = { { 0.f } };
        float DT_ALIGNED_PIXEL TV_grad[4] = { 1.f };
        float DT_ALIGNED_PIXEL TV_lap[4] = { 1.f };

        const float *const restrict north = __builtin_assume_aligned(input + idx_grad[1], 16);
        const float *const restrict south = __builtin_assume_aligned(input + idx_grad[7], 16);
        const float *const restrict east  = __builtin_assume_aligned(input + idx_grad[5], 16);
        const float *const restrict west  = __builtin_assume_aligned(input + idx_grad[3], 16);

        const float *const restrict north_far = __builtin_assume_aligned(input + idx_lapl[1], 16);
        const float *const restrict south_far = __builtin_assume_aligned(input + idx_lapl[7], 16);
        const float *const restrict east_far  = __builtin_assume_aligned(input + idx_lapl[5], 16);
        const float *const restrict west_far  = __builtin_assume_aligned(input + idx_lapl[3], 16);

        // build the local anisotropic convolution filters
        #ifdef _OPENMP
        #pragma omp simd aligned(north, south, west, east, TV_grad, TV_lap, north_far, south_far, east_far, west_far : 16) \
          aligned(idx_grad, idx_lapl, kern_grad, kern_lap : 64)
        #endif
        for(size_t c = 0; c < 4; c++)
        {
          if(compute_structure)
          {
            // Compute the gradient with centered finite differences - warning : x is vertical, y is horizontal
            const float grad_x = (south[c] - north[c]) / 2.0f; // du(i, j) / dx
            const float grad_y = (east[c] - west[c]) / 2.0f;   // du(i, j) / dy

            // Find the dampening factor
            const float TV = hypotf(grad_x, grad_y);
            const float c2 = expf(-TV / K);
            TV_grad[c] = expf(-TV / L);

            // Find the direction of the gradient
            const float theta = atan2f(grad_y, grad_x);

            // Find the gradient rotation coefficients for the matrix
            float sin_theta = sinf(theta);
            float cos_theta = cosf(theta);
            const float sin_theta2 = sqf(sin_theta);
            const float cos_theta2 = sqf(cos_theta);

            // Build the convolution kernel for the structure extraction
            const float a11 = cos_theta2 + c2 * sin_theta2;
            const float a12 = (c2 - 1.0f) * cos_theta * sin_theta;
            const float a22 = c2 * cos_theta2 + sin_theta2;

            const float b11 = -a12 / 2.0f;
            const float b13 = -b11;
            const float b22 = -2.0f * (a11 + a22);

            kern_grad[0][c] = b11;
            kern_grad[1][c] = a22;
            kern_grad[2][c] = b13;
            kern_grad[3][c] = a11;
            kern_grad[4][c] = b22;
            kern_grad[5][c] = a11;
            kern_grad[6][c] = b13;
            kern_grad[7][c] = a22;
            kern_grad[8][c] = b11;
          }

          if(compute_texture)
          {
            // Compute the laplacian with centered finite differences - warning : x is vertical, y is horizontal
            const float grad_x = south_far[c] + north_far[c] - 2.f * input[index + c]; // du(i, j) / dx
            const float grad_y = east_far[c] + west_far[c] - 2.f * input[index + c];   // du(i, j) / dy

            // Find the dampening factor
            const float TV = hypotf(grad_x, grad_y);
            const float c2 = expf(-TV / K);
            TV_lap[c] = expf(-TV / L);

            // Find the direction of the gradient
            const float theta = atan2f(grad_y, grad_x);

            // Find the gradient rotation coefficients for the matrix
            float sin_theta = sinf(theta);
            float cos_theta = cosf(theta);
            const float sin_theta2 = sqf(sin_theta);
            const float cos_theta2 = sqf(cos_theta);

            // Build the convolution kernel for the texture extraction
            const float a11 = cos_theta2 + c2 * sin_theta2;
            const float a12 = (c2 - 1.0f) * cos_theta * sin_theta;
            const float a22 = c2 * cos_theta2 + sin_theta2;

            const float b11 = a12 / sqrtf(2.f);
            const float b22 = -2.f * (a11 + a22 ) - 4.f * a12 / sqrtf(2.f);

            kern_lap[0][c] = b11;
            kern_lap[1][c] = a22;
            kern_lap[2][c] = b11;
            kern_lap[3][c] = a11;
            kern_lap[4][c] = b22;
            kern_lap[5][c] = a11;
            kern_lap[6][c] = b11;
            kern_lap[7][c] = a22;
            kern_lap[8][c] = b11;
          }
        }

        float DT_ALIGNED_PIXEL grad[4] = { 0.f };
        float DT_ALIGNED_PIXEL lapl[4] = { 0.f };

        // Convolve anisotropic filters at current pixel
        #ifdef _OPENMP
        #pragma omp simd aligned(kern_grad, kern_lap, input, output, idx_grad: 64) aligned(TV_grad, TV_lap, grad, lapl : 16)
        #endif
        for(size_t c = 0; c < 4; c++)
        {
          float acc1 = 0.f;
          float acc2 = 0.f;

          for(size_t k = 0; k < 9; k++)
          {
            // Convolve first-order term (gradient)
            acc1 += kern_grad[k][c] * input[idx_grad[k] + c];

            // Convolve second-order term (laplacian)
            acc2 += kern_lap[k][c] * input[idx_lapl[k] + c];
          }

          grad[c] = acc1;
          lapl[c] = acc2;
        }

        // Use a collaborative regularization
        const float TV_lap_min = fminf(fminf(TV_lap[0], TV_lap[1]), TV_lap[2]);
        const float TV_grad_min = fminf(fminf(TV_grad[0], TV_grad[1]), TV_grad[2]);

        // Update the solution
        #ifdef _OPENMP
        #pragma omp simd aligned(input, output: 64) aligned(grad, lapl : 16)
        #endif
        for(size_t c = 0; c < 4; c++)
        {
          output[index + c] = input[index + c] + A * lapl[c] * TV_lap_min + B * grad[c] * TV_grad_min;
        }
      }
      else
      {
        #ifdef _OPENMP
        #pragma omp simd aligned(input, output: 64)
        #endif
        for(size_t c = 0; c < 4; c++) output[index + c] = input[index + c];
      }
    }
}


static float diffusion_scale_factor(const float current_radius, const float final_radius, const float zoom, const dt_iop_diffuse_model_t model)
{
  if(model == DT_DIFFUSE_GAUSSIAN)
  {
    return expf(-(current_radius * current_radius) / (final_radius * final_radius * zoom * zoom));
  }
  else if(model == DT_DIFFUSE_CONSTANT)
  {
    return (current_radius <= final_radius) ? 1.f : 0.f;
  }
  else if(model == DT_DIFFUSE_LINEAR)
  {
    return fmaxf(1.f - current_radius / final_radius, 0.f);
  }
  else if(model == DT_DIFFUSE_QUADRATIC)
  {
    return fmaxf(sqrtf(1.f - current_radius / final_radius), 0.f);
  }
  else
  {
    return 1.f;
  }
}


static inline gint reconstruct_highlights(const float *const restrict in, float *const restrict reconstructed,
                                          const uint8_t *const restrict mask,
                                          const size_t ch,
                                          const dt_iop_diffuse_data_t *const data, dt_dev_pixelpipe_iop_t *piece,
                                          const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  gint success = TRUE;

  // wavelets scales - either the max kernel size at current resolution or 4 times the blur radius
  const float zoom = roi_in->scale / piece->iscale;
  const int current_zoom_scales = get_scales(roi_in, piece);
  int diffusion_scales;
  if(data->model == DT_DIFFUSE_GAUSSIAN)
    diffusion_scales = ceilf(log2f(data->radius * zoom * 4));
  else
    diffusion_scales = ceilf(log2f(data->radius * zoom));

  const int scales = MIN(diffusion_scales, current_zoom_scales);

  float structure = data->structure;
  float texture = data->texture;
  float edges = expf(-data->edges);
  float regularization = expf(-data->regularization);

  // wavelets scales buffers
  float *const restrict LF_even = dt_alloc_sse_ps(roi_out->width * roi_out->height * ch); // low-frequencies RGB
  float *const restrict LF_odd = dt_alloc_sse_ps(roi_out->width * roi_out->height * ch);  // low-frequencies RGB
  float *const restrict HF_RGB = dt_alloc_sse_ps(roi_out->width * roi_out->height * ch);  // high-frequencies RGB

  // alloc a permanent reusable buffer for intermediate computations - avoid multiple alloc/free
  float *const restrict temp1 = dt_alloc_sse_ps(roi_out->width * roi_out->height * ch);
  float *const restrict temp2 = dt_alloc_sse_ps(roi_out->width * roi_out->height * ch);

  if(!LF_even || !LF_odd || !HF_RGB || !temp1 || !temp2)
  {
    dt_control_log(_("filmic highlights reconstruction failed to allocate memory, check your RAM settings"));
    success = FALSE;
    goto error;
  }

  // Init reconstructed with valid parts of image
  init_reconstruct(reconstructed, roi_out->width, roi_out->height, ch);

  // À trous wavelet decompose
  // there is a paper from a guy we know that explains it : https://jo.dreggn.org/home/2010_atrous.pdf
  // the wavelets decomposition here is the same as the equalizer/atrous module,
  // but simplified because we don't need the edge-aware term, so we can seperate the convolution kernel
  // with a vertical and horizontal blur, wich is 10 multiply-add instead of 25 by pixel.
  for(int s = 0; s < scales; ++s)
  {
    const float *restrict detail;       // buffer containing this scale's input
    float *restrict LF;                 // output buffer for the current scale
    float *restrict HF_RGB_temp;        // temp buffer for HF_RBG terms before blurring

    // swap buffers so we only need 2 LF buffers : the LF at scale (s-1) and the one at current scale (s)
    if(s == 0)
    {
      detail = in;
      LF = LF_odd;
      HF_RGB_temp = LF_even;
    }
    else if(s % 2 != 0)
    {
      detail = LF_odd;
      LF = LF_even;
      HF_RGB_temp = LF_odd;
    }
    else
    {
      detail = LF_even;
      LF = LF_odd;
      HF_RGB_temp = LF_even;
    }

    const int mult = 1 << s; // fancy-pants C notation for 2^s with integer type, don't be afraid

    // Compute wavelets low-frequency scales
    blur_2D_Bspline(detail, LF, temp1, roi_out->width, roi_out->height, mult);

    // Compute wavelets high-frequency scales and save the mininum of texture over the RGB channels
    // Note : HF_RGB = detail - LF, HF_grey = max(HF_RGB)
    wavelets_detail_level(detail, LF, HF_RGB_temp, roi_out->width, roi_out->height, ch);

    // diffuse particles
    float *LF_in = NULL;
    float *LF_out = NULL;
    float *HF_in = NULL;
    float *HF_out = NULL;

    const float factor = data->update * diffusion_scale_factor((float)mult, data->radius, zoom, data->model);

    if(s == scales - 1)
    {
      // if it's the last scale, then LF is the residual so we blur it too as if it was the next HF
      const int next_mult = 1 << (s + 1);
      const float factor_lf = data->update * diffusion_scale_factor((float)(next_mult), data->radius, zoom, data->model);

      for(size_t it = 0; it < data->iterations; it++)
      {
        if(it == 0)
        {
          LF_in = LF;
          LF_out = temp2;
        }
        else if(it % 2 != 0)
        {
          LF_in = temp2;
          LF_out = LF;
        }
        else
        {
          LF_in = LF;
          LF_out = temp2;
        }

        heat_PDE_inpanting(LF_in, LF_out, mask, roi_out->width, roi_out->height, ch, next_mult,
                           factor_lf * texture,
                           structure, edges, regularization);
      }

      LF = LF_out;
    }

    for(size_t it = 0; it < data->iterations; it++)
    {
      if(it == 0)
      {
        HF_in = HF_RGB_temp;
        HF_out = temp1;
      }
      else if(it % 2 != 0)
      {
        HF_in = temp1;
        HF_out = HF_RGB_temp;
      }
      else
      {
        HF_in = HF_RGB_temp;
        HF_out = temp1;
      }

      heat_PDE_inpanting(HF_in, HF_out, mask, roi_out->width, roi_out->height, ch, mult,
                          factor * texture,
                          structure, edges, regularization);
    }

    HF_RGB_temp = HF_out;

    // Collapse wavelets
    wavelets_reconstruct_RGB(HF_RGB_temp, LF, reconstructed, roi_out->width, roi_out->height, ch, s, scales);
  }

error:
  if(temp1) dt_free_align(temp1);
  if(temp2) dt_free_align(temp2);
  if(LF_even) dt_free_align(LF_even);
  if(LF_odd) dt_free_align(LF_odd);
  if(HF_RGB) dt_free_align(HF_RGB);
  return success;
}


static inline void build_mask(const float *const restrict input, uint8_t *const restrict mask, const float threshold,
                              const size_t width, const size_t height, const size_t ch)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(input, mask, height, width, ch, threshold) \
schedule(dynamic) aligned(mask, input:64)
#endif
  for(size_t k = 0; k < height * width * ch; k += ch)
  {
    mask[k / ch] = (input[k] > threshold || input[k + 1] > threshold || input[k + 2] > threshold);
  }
}


void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const restrict ivoid,
             void *const restrict ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_diffuse_data_t *const data = (dt_iop_diffuse_data_t *)piece->data;

  if(piece->colors != 4)
  {
    dt_control_log(_("filmic works only on RGB input"));
    return;
  }

  const size_t ch = 4;
  float *restrict in = (float *const restrict)ivoid;
  float *const restrict out = (float *const restrict)ovoid;
  float *restrict temp = NULL;
  uint8_t *restrict mask = NULL;

  if(data->threshold > 0.f)
  {
    const float scale = piece->iscale / roi_in->scale;
    const float blur = 32.f / scale;

    // build a boolean mask, TRUE where image is above threshold
    mask = dt_alloc_align(64,roi_out->width * roi_out->height * sizeof(uint8_t));
    build_mask(in, mask, data->threshold, roi_out->width, roi_out->height, ch);

    // init the inpainting area with blur
    temp = dt_alloc_sse_ps(roi_out->width * roi_out->height * ch);

    float RGBmax[4], RGBmin[4];
    for(int k = 0; k < 4; k++)
    {
      RGBmax[k] = INFINITY;
      RGBmin[k] = 0.f;
    }

    dt_gaussian_t *g = dt_gaussian_init(roi_out->width, roi_out->height, ch, RGBmax, RGBmin, blur, 0);
    if(!g) return;
    dt_gaussian_blur_4c(g, in, temp);
    dt_gaussian_free(g);

    // add noise and restore valid parts where mask = FALSE
    const float noise = 0.1 / scale;

    #ifdef _OPENMP
    #pragma omp parallel for default(none) \
    dt_omp_firstprivate(in, temp, mask, roi_out, ch, noise) \
    schedule(dynamic)
    #endif
    for(size_t k = 0; k < roi_out->height * roi_out->width * ch; k += ch)
    {
      if(mask[k / ch])
      {
        const uint32_t i = k / roi_out->width;
        const uint32_t j = k - i;
        uint32_t DT_ALIGNED_ARRAY state[4] = { splitmix32(j + 1), splitmix32((j + 1) * (i + 3)), splitmix32(1337), splitmix32(666) };
        xoshiro128plus(state);
        xoshiro128plus(state);
        xoshiro128plus(state);
        xoshiro128plus(state);

        for(size_t c = 0; c < 4; c++) temp[k + c] = gaussian_noise(temp[k + c], noise, i % 2 || j % 2, state);
      }
      else
      {
        for(size_t c = 0; c < 4; c++) temp[k + c] = in[k + c];
      }
    }

    in = temp;
  }

  reconstruct_highlights(in, out, mask, ch, data, piece, roi_in, roi_out);

  if(mask) dt_free_align(mask);
  if(temp) dt_free_align(temp);
}


void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_diffuse_gui_data_t *g = (dt_iop_diffuse_gui_data_t *)self->gui_data;
  dt_iop_diffuse_params_t *p = (dt_iop_diffuse_params_t *)self->params;
  dt_bauhaus_slider_set_soft(g->iterations, p->iterations);
  dt_bauhaus_slider_set_soft(g->texture, p->texture);
  dt_bauhaus_slider_set_soft(g->structure, p->structure);
  dt_bauhaus_slider_set_soft(g->edges, p->edges);
  dt_bauhaus_slider_set_soft(g->regularization, p->regularization);
  dt_bauhaus_slider_set_soft(g->radius, p->radius);
  dt_bauhaus_slider_set_soft(g->update, p->update);
  dt_bauhaus_slider_set_soft(g->threshold, p->threshold);
  dt_bauhaus_combobox_set(g->model, p->model);
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_diffuse_gui_data_t *g = IOP_GUI_ALLOC(diffuse);
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("diffusion intensity")), FALSE, FALSE, 0);

  g->iterations = dt_bauhaus_slider_from_params(self, "iterations");
  gtk_widget_set_tooltip_text(g->iterations, _("more iterations make the effect stronger but the module slower.\n"
                                               "this is analogous to giving more time to the diffusion reaction.\n"
                                               "if you plan on sharpening or inpainting, more iterations help reconstruction."));

  g->update = dt_bauhaus_slider_from_params(self, "update");
  dt_bauhaus_slider_set_factor(g->update, 100.0f);
  dt_bauhaus_slider_set_format(g->update, "%.2f %%");
  gtk_widget_set_tooltip_text(g->update, _("weight of each iterations update.\n"
                                           "100 % is suitable for diffusion, inpainting and blurring.\n"
                                           "lower if noise, halos or any artifact appear as you add more iterations."));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("diffusion quality")), FALSE, FALSE, 0);

  g->structure = dt_bauhaus_slider_from_params(self, "structure");
  dt_bauhaus_slider_set_factor(g->structure, 100.0f);
  dt_bauhaus_slider_set_format(g->structure, "%.2f %%");
  gtk_widget_set_tooltip_text(g->structure, _("smoothing or sharpening of smooth details (gradients).\n"
                                              "positive values diffuse and blur.\n"
                                              "negative values sharpen.\n"
                                              "zero does nothing."));

  g->texture = dt_bauhaus_slider_from_params(self, "texture");
  dt_bauhaus_slider_set_factor(g->texture, 100.0f);
  dt_bauhaus_slider_set_format(g->texture, "%.2f %%");
  gtk_widget_set_tooltip_text(g->texture, _("smoothing or sharpening of sharp details (gradients).\n"
                                            "positive values diffuse and blur.\n"
                                            "negative values sharpen.\n"
                                            "zero does nothing."));

  g->edges = dt_bauhaus_slider_from_params(self, "edges");
  gtk_widget_set_tooltip_text(g->edges, _("anisotropy of the diffusion.\n"
                                          "high values force the diffusion to be 1D and perpendicular to edges.\n"
                                          "low values allow the diffusion to be 2D and uniform, like a classic blur."));

  g->regularization = dt_bauhaus_slider_from_params(self, "regularization");
  gtk_widget_set_tooltip_text(g->regularization, _("normalization of the diffusion.\n"
                                                   "high values dampen high-magnitude gradients to avoid overshooting at sharp edges.\n"
                                                   "low values relay the dampening and allow more and more overshooting."));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("diffusion spatiality")), FALSE, FALSE, 0);

  g->model = dt_bauhaus_combobox_from_params(self, "model");
  gtk_widget_set_tooltip_text(g->threshold, _("defines how the diffusion blends as radius increases.\n"
                                              "gaussian mimics natural diffusion, with large radii barely affected.\n"
                                              "constant is a regular wavelets blending and affect each radius the same.\n"
                                              "linear or quadratic define different rates of spatial diffusion."));

  g->radius = dt_bauhaus_slider_from_params(self, "radius");
  dt_bauhaus_slider_set_format(g->radius, "%.0f px");
  gtk_widget_set_tooltip_text(g->radius, _("scale of the diffusion.\n"
                                           "high values diffuse farther, at the expense of computation time.\n"
                                           "low values diffuse closer.\n"
                                           "if you plan on denoising, the radius should be around the width of your lens blur."));

  g->threshold = dt_bauhaus_slider_from_params(self, "threshold");
  gtk_widget_set_tooltip_text(g->threshold, _("luminance threshold for the mask.\n"
                                              "0. disables the luminance masking and applies the module on the whole image.\n"
                                              "any higher value will exclude pixels whith luminance lower than the threshold.\n"
                                              "this can be used to inpaint highlights."));
}
