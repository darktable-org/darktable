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
typedef struct dt_iop_diffuse_params_t
{
  // global parameters
  int iterations;       // $MIN: 1   $MAX: 12   $DEFAULT: 1  $DESCRIPTION: "iterations"
  float sharpness;         // $MIN: -1.  $MAX: 1.   $DEFAULT: 0. $DESCRIPTION: "sharpness"
  int radius;           // $MIN: 1   $MAX: 1024   $DEFAULT: 8  $DESCRIPTION: "radius"
  float regularization; // $MIN: 0. $MAX: 6.   $DEFAULT: 0. $DESCRIPTION: "edge and noise avoiding"
  float noise_threshold;// $MIN: -1. $MAX: 1.   $DEFAULT: 0. $DESCRIPTION: "edge/surface threshold"
  float anisotropy;          // $MIN: -8. $MAX: 8.   $DEFAULT: 0. $DESCRIPTION: "anisotropy"

  // masking
  float threshold; // $MIN: 0.  $MAX: 8.   $DEFAULT: 0. $DESCRIPTION: "luminance masking threshold"

  // first order derivative, anisotropic, aka first order integral of wavelets details scales
  float first;       // $MIN: -1. $MAX: 1.   $DEFAULT: 0. $DESCRIPTION: "1st order (gradient)"

  // second order deritavite, isotropic, aka wavelets details scales
  float second; // $MIN: -1. $MAX: 1.   $DEFAULT: 0. $DESCRIPTION: "2nd order (laplacian)"

  // third order derivative, anisotropic, aka first order derivative of wavelets details scales
  float third;       // $MIN: -1. $MAX: 1.   $DEFAULT: 0. $DESCRIPTION: "3rd order (gradient of laplacian)"

  // fourth order derivative, anisotropic, aka second order derivative of wavelets details scales
  float fourth;       // $MIN: -1. $MAX: 1.   $DEFAULT: 0. $DESCRIPTION: "4th order (laplacian of laplacian)"
} dt_iop_diffuse_params_t;


typedef struct dt_iop_diffuse_gui_data_t
{
  GtkWidget *iterations, *fourth, *third, *second, *radius, *sharpness, *threshold, *regularization, *first,
      *anisotropy, *regularization_first, *noise_threshold;
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
  return dt_iop_set_description(self,
                                _("simulate directional diffusion of light with heat transfer model\n"
                                  "to apply an iterative edge-oriented blur, \n"
                                  "inpaint damaged parts of the image,\n"
                                  "or to remove blur with blind deconvolution."),
                                _("corrective and creative"), _("linear, RGB, scene-referred"), _("linear, RGB"),
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

  p.iterations = 8;
  p.radius = 8;
  p.sharpness = 0.01f;
  p.threshold = 0.0f;
  p.noise_threshold = -0.1f;
  p.regularization = 4.5f;
  p.anisotropy = 3.5f;

  p.first = +0.15f;
  p.second = -0.15f;
  p.third = 0.30f;
  p.fourth = -0.30f;
  dt_gui_presets_add_generic(_("remove medium lens blur"), self->op, self->version(), &p, sizeof(p), 1,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  p.iterations = 4;
  p.regularization = 4.f;
  dt_gui_presets_add_generic(_("remove soft lens blur"), self->op, self->version(), &p, sizeof(p), 1,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  p.iterations = 12;
  p.regularization = 5.f;
  dt_gui_presets_add_generic(_("remove heavy lens blur"), self->op, self->version(), &p, sizeof(p), 1,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  p.iterations = 4;
  p.radius = 4;
  p.sharpness = 0.0f;
  p.threshold = 0.2f;
  p.noise_threshold = -0.1f;
  p.regularization = 0.f;
  p.anisotropy = 3.f;

  p.first = +0.5f;
  p.second = -0.5f;
  p.third = +0.5f;
  p.fourth = +0.5f;
  dt_gui_presets_add_generic(_("denoise"), self->op, self->version(), &p, sizeof(p), 1,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  p.iterations = 2;
  p.radius = 16;
  p.sharpness = 0.0f;
  p.threshold = 0.0f;
  p.noise_threshold = 0.f;
  p.regularization = 0.f;
  p.anisotropy = 3.f;

  p.first = +0.25f;
  p.second = +0.25f;
  p.third = +0.25f;
  p.fourth = +0.25f;
  dt_gui_presets_add_generic(_("diffuse"), self->op, self->version(), &p, sizeof(p), 1,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  p.iterations = 1;
  p.radius = 6;
  p.sharpness = 0.5f;
  p.threshold = 0.0f;
  p.noise_threshold = 6.f;
  p.regularization = 0.f;
  p.anisotropy = 0.f;

  p.first = +0.1f;
  p.second = +0.1f;
  p.third = +0.1f;
  p.fourth = +0.1f;
  dt_gui_presets_add_generic(_("increase perceptual acutance"), self->op, self->version(), &p, sizeof(p), 1,
                             DEVELOP_BLEND_CS_RGB_SCENE);
}

// B spline filter
#define FSIZE 5

// The B spline best approximate a Gaussian of standard deviation :
// see https://eng.aurelienpierre.com/2021/03/rotation-invariant-laplacian-for-2d-grids/
#define B_SPLINE_SIGMA 1.0553651328015339f

static inline float normalize_laplacian(const float sigma)
{
  // Normalize the wavelet scale to approximate a laplacian
  // see https://eng.aurelienpierre.com/2021/03/rotation-invariant-laplacian-for-2d-grids/#Scaling-coefficient
  return 2.f * M_PI_F / (sqrtf(M_PI_F) * sqf(sigma));
}

static inline float equivalent_sigma_at_step(const float sigma, const unsigned int s)
{
  // If we stack several gaussian blurs of standard deviation sigma on top of each other,
  // this is the equivalent standard deviation we get at the end (after s steps)
  // First step is s = 0
  // see
  // https://eng.aurelienpierre.com/2021/03/rotation-invariant-laplacian-for-2d-grids/#Multi-scale-iterative-scheme
  if(s == 0) return sigma;
  else return sqrtf(sqf(equivalent_sigma_at_step(sigma, s - 1)) + sqf(exp2f((float)s) * sigma));
}

static inline unsigned int num_steps_to_reach_equivalent_sigma(const float sigma_filter, const float sigma_final)
{
  // The inverse of the above : compute the number of scales needed to reach the desired equivalent sigma_final
  // after sequential blurs of constant sigma_filter
  unsigned int s = 0;
  float radius = sigma_filter;
  while(radius < sigma_final)
  {
    fprintf(stdout, "computed radius : %f\n", radius);
    ++s;
    radius = sqrtf(sqf(radius) + sqf((float)(1 << s) * sigma_filter));
  }
  fprintf(stdout, "computed radius : %f\n", radius);
  return s + 1;
}

inline static void blur_2D_Bspline(const float *const restrict in, float *const restrict HF,
                                   float *const restrict LF, const int mult, const size_t width, const size_t height)
{
  // see https://arxiv.org/pdf/1711.09791.pdf
#ifdef _OPENMP
#pragma omp parallel for default(none) dt_omp_firstprivate(width, height, in, LF, HF, mult) schedule(simd               \
                                                                                               : static)          \
    collapse(2)
#endif
  for(size_t i = 0; i < height; i++)
  {
    for(size_t j = 0; j < width; j++)
    {
      const size_t index = (i * width + j) * 4;
      float DT_ALIGNED_PIXEL acc[4] = { 0.f };

#ifdef _OPENMP
#pragma omp simd aligned(in : 64) aligned(acc:16)
#endif
      for(size_t ii = 0; ii < FSIZE; ++ii)
        for(size_t jj = 0; jj < FSIZE; ++jj)
        {
          const size_t row = CLAMP((int)i + mult * (int)(ii - (FSIZE - 1) / 2), (int)0, (int)height - 1);
          const size_t col = CLAMP((int)j + mult * (int)(jj - (FSIZE - 1) / 2), (int)0, (int)width - 1);
          const size_t k_index = (row * width + col) * 4;

          const float DT_ALIGNED_ARRAY filter[FSIZE]
              = { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };

          const float filters = filter[ii] * filter[jj];

          for(size_t c = 0; c < 4; c++) acc[c] += filters * in[k_index + c];
        }

      for_four_channels(c, aligned(in, HF, LF : 64) aligned(acc : 16))
      {
        LF[index + c] = acc[c];
        HF[index + c] = in[index + c] - acc[c];
      }
    }
  }
}

static inline void init_reconstruct(float *const restrict reconstructed, const size_t width, const size_t height,
                                    const size_t ch)
{
// init the reconstructed buffer with non-clipped and partially clipped pixels
// Note : it's a simple multiplied alpha blending where mask = alpha weight
#ifdef _OPENMP
#pragma omp parallel for simd default(none) dt_omp_firstprivate(reconstructed, width, height, ch)                 \
    schedule(simd                                                                                                 \
             : static) aligned(reconstructed : 64)
#endif
  for(size_t k = 0; k < height * width * ch; k++)
  {
    reconstructed[k] = 0.f;
  }
}

// Discretization parameters for the Partial Derivative Equation solver
#define H 1         // spatial step
#define KAPPA 0.25f // 0.25 if h = 1, 1 if h = 2

static inline void heat_PDE_inpanting(const float *const restrict high_freq, const float *const restrict low_freq,
                                      const uint8_t *const restrict mask, float *const restrict output,
                                      const size_t width, const size_t height, const size_t ch,
                                      const float fourth, const float third, const float second,
                                      const float first_layer, const float anisotropy,
                                      const float regularization, const float noise_threshold, const float final_radius, const float factor,
                                      const int current_step, const int scales, const float zoom)
{
  // Simultaneous inpainting for image structure and fourth using anisotropic heat transfer model
  // https://www.researchgate.net/publication/220663968
  // modified for a multi-scale wavelet setup


  const int compute_fourth = TRUE;
  //(fourth != 0.f);

  const int has_mask = (mask != NULL);
  const int last_step = (scales - 1) == current_step;

  float *const restrict out = DT_IS_ALIGNED(output);
  const float *const restrict LF = DT_IS_ALIGNED(low_freq);
  const float *const restrict HF = DT_IS_ALIGNED(high_freq);

  const int current_radius = equivalent_sigma_at_step(B_SPLINE_SIGMA, current_step);
  const float real_radius = equivalent_sigma_at_step(B_SPLINE_SIGMA, current_step) * zoom;

  const float norm_lapl = expf(-sqf(real_radius) / sqf(final_radius));
  const float DT_ALIGNED_ARRAY ABCD[4] = { fourth * KAPPA * norm_lapl, third * KAPPA * norm_lapl,
                                           second * norm_lapl, first_layer * KAPPA * norm_lapl };

  const float strength = factor * norm_lapl + 1.f;

  fprintf(stdout, "current radius : %i, real radius : %f, final radius : %f, norm : %f, sharpness : %f\n", current_radius,
          real_radius, final_radius, norm_lapl, strength);

#ifdef _OPENMP
#pragma omp parallel for default(none) dt_omp_firstprivate(                                                       \
    out, mask, HF, LF, height, width, ch, ABCD, has_mask, current_radius, compute_fourth, noise_threshold, \
    anisotropy, regularization, real_radius, last_step, strength) schedule(dynamic) collapse(2)
#endif
  for(size_t i = 0; i < height; ++i)
    for(size_t j = 0; j < width; ++j)
    {
      const size_t idx = (i * width + j);
      const size_t index = idx * 4;
      uint8_t opacity = (has_mask) ? mask[idx] : 1;

      if(opacity)
      {
        // build arrays of pointers to the pixels we will need here,
        // to be super clear with compiler about cache management
        const float *restrict neighbour_pixel_HF[9];
        const float *restrict neighbour_pixel_LF[9];

        // non-local neighbours
        const size_t j_neighbours[3] = { CLAMP((int)(j - current_radius * H), (int)0, (int)width - 1),   // y - mult
                                         j,                                                              // y
                                         CLAMP((int)(j + current_radius * H), (int)0, (int)width - 1) }; // y + mult

        const size_t i_neighbours[3] = { CLAMP((int)(i - current_radius * H), (int)0, (int)height - 1),   // x - mult
                                         i,                                                               // x
                                         CLAMP((int)(i + current_radius * H), (int)0, (int)height - 1) }; // x + mult

        for(size_t ii = 0; ii < 3; ii++)
          for(size_t jj = 0; jj < 3; jj++)
          {
            neighbour_pixel_HF[3 * ii + jj] = __builtin_assume_aligned(HF + (i_neighbours[ii] * width + j_neighbours[jj]) * 4, 16);
            neighbour_pixel_LF[3 * ii + jj] = __builtin_assume_aligned(LF + (i_neighbours[ii] * width + j_neighbours[jj]) * 4, 16);
          }

        // assert(grad_pixel[4] == lapl_pixel[4] == center)
        const float *const restrict center_HF = neighbour_pixel_HF[4];
        const float *const restrict center_LF = neighbour_pixel_LF[4];

        const float *const restrict north = neighbour_pixel_LF[1];
        const float *const restrict south = neighbour_pixel_LF[7];
        const float *const restrict east = neighbour_pixel_LF[5];
        const float *const restrict west = neighbour_pixel_LF[3];

        const float *const restrict north_far = neighbour_pixel_LF[1];
        const float *const restrict south_far = neighbour_pixel_LF[7];
        const float *const restrict east_far = neighbour_pixel_LF[5];
        const float *const restrict west_far = neighbour_pixel_LF[3];

        float DT_ALIGNED_ARRAY kern_grad[9][4];
        float DT_ALIGNED_ARRAY kern_lap[9][4];
        float DT_ALIGNED_ARRAY kern_first[9][4];

// build the local anisotropic convolution filters
#ifdef _OPENMP
#pragma omp simd aligned(north, south, west, east, north_far, south_far, east_far, west_far, center_LF : 16)      \
    aligned(kern_grad, kern_lap, kern_first : 64)
#endif
        for(size_t c = 0; c < 4; c++)
        {
          // Compute the gradient with centered finite differences - warning : x is vertical, y is horizontal
          {
            const float grad_x_LF = (south[c] - north[c]) / 2.0f; // du(i, j) / dx
            const float grad_y_LF = (east[c] - west[c]) / 2.0f;   // du(i, j) / dy

            // Find the dampening factor
            const float tv_LF = hypotf(grad_x_LF, grad_y_LF);

            // Find the direction of the gradient
            const float theta = atan2f(grad_y_LF, grad_x_LF);

            // Find the gradient rotation coefficients for the matrix
            const float sin_theta = sinf(theta);
            const float cos_theta = cosf(theta);
            const float sin_theta2 = sqf(sin_theta);
            const float cos_theta2 = sqf(cos_theta);

            const float c2 = expf(-tv_LF / anisotropy);
            const float a11 = cos_theta2 + c2 * sin_theta2;
            const float a12 = (c2 - 1.0f) * cos_theta * sin_theta;
            const float a22 = c2 * cos_theta2 + sin_theta2;

            // Build the convolution kernel for the third extraction
            // gradient of laplacian
            {
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

            // Build the convolution kernel for the first
            // integral of laplacian (== divergence of the gradient)
            {
              const float b11 = a12 / 2.0f;
              const float b22 = a11 + a22;

              kern_first[0][c] = b11 / b22;
              kern_first[1][c] = a22 / b22;
              kern_first[2][c] = b11 / b22;
              kern_first[3][c] = a11 / b22;
              kern_first[4][c] = b22 / b22;
              kern_first[5][c] = a11 / b22;
              kern_first[6][c] = b11 / b22;
              kern_first[7][c] = a22 / b22;
              kern_first[8][c] = b11 / b22;
            }
          }

          // Compute the non-local laplacian
          if(compute_fourth)
          {
            // Compute the laplacian with centered finite differences - warning : x is vertical, y is horizontal
            const float grad_x_LF
                = (south_far[c] + north_far[c] - 2.f * center_LF[c]); // du(i, j) / dx
            const float grad_y_LF
                = (east_far[c] + west_far[c] - 2.f * center_LF[c]); // du(i, j) / dy

            // Find the dampening factor
            const float tv_LF = hypotf(grad_x_LF, grad_y_LF);
            const float c2 = expf(-tv_LF / anisotropy);

            // Find the direction of the gradient
            const float theta = atan2f(grad_y_LF, grad_x_LF);

            // Find the gradient rotation coefficients for the matrix
            const float sin_theta = sinf(theta);
            const float cos_theta = cosf(theta);
            const float sin_theta2 = sqf(sin_theta);
            const float cos_theta2 = sqf(cos_theta);

            // Build the convolution kernel for the fourth extraction
            const float a11 = cos_theta2 + c2 * sin_theta2;
            const float a12 = (c2 - 1.0f) * cos_theta * sin_theta;
            const float a22 = c2 * cos_theta2 + sin_theta2;

            const float b11 = a12 / sqrtf(2.f);
            const float b22 = -2.f * (a11 + a22) - 4.f * a12 / sqrtf(2.f);

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

        // Convolve anisotropic filters at current pixel for directional derivatives
        float DT_ALIGNED_ARRAY derivatives[4][4];
        float DT_ALIGNED_PIXEL TV[4] = { 0.f };

#ifdef _OPENMP
#pragma omp simd aligned(kern_grad, kern_first, kern_lap, derivatives, neighbour_pixel_HF : 64)          \
    aligned(center_HF, TV : 16)
#endif
        for(size_t c = 0; c < 4; c++)
        {
          float acc1 = 0.f;
          float acc2 = 0.f;
          float acc3 = 0.f;
          float acc4 = 0.f;

          for(size_t k = 0; k < 9; k++)
          {
            // Convolve the first term
            acc1 += kern_first[k][c] * neighbour_pixel_HF[k][c];

            // Convolve first-order term (gradient)
            acc2 += kern_grad[k][c] * neighbour_pixel_HF[k][c];

            // Convolve second-order term (laplacian)
            acc3 += kern_lap[k][c] * neighbour_pixel_HF[k][c];

            // Compute "Total Variation" (it's not a real TV)
            acc4 += neighbour_pixel_HF[k][c] * neighbour_pixel_HF[k][c];
          }

          TV[c] = noise_threshold + acc4 / 9.f * regularization;

          derivatives[c][0] = acc3;
          derivatives[c][1] = acc2;
          derivatives[c][2] = -center_HF[c];
          derivatives[c][3] = -acc1;
        }

// Update the solution
#ifdef _OPENMP
#pragma omp simd aligned(out, derivatives, LF, HF : 64) aligned(TV, ABCD : 16)
#endif
        for(size_t c = 0; c < 4; c++)
        {
          float acc = 0.f;
          for(size_t k = 0; k < 4; k++) acc += derivatives[c][k] * ABCD[k];
          acc = (HF[index + c] + acc / TV[c]) * strength;

          out[index + c] += (last_step) ? acc + LF[index + c] : acc;
        }
      }
      else
      {
#ifdef _OPENMP
#pragma omp simd aligned(out, HF, LF : 64)
#endif
        for(size_t c = 0; c < 4; c++)
          out[index + c] += (last_step) ? HF[index + c] + LF[index + c] : HF[index + c];
      }
    }
}

static inline gint reconstruct_highlights(const float *const restrict in, float *const restrict reconstructed,
                                          const uint8_t *const restrict mask, const size_t ch,
                                          const dt_iop_diffuse_data_t *const data, dt_dev_pixelpipe_iop_t *piece,
                                          const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  gint success = TRUE;

  const float zoom = fmaxf(piece->iscale / roi_in->scale, 1.0f);
  const float final_radius = data->radius * 3.0f / zoom;
  const int diffusion_scales = num_steps_to_reach_equivalent_sigma(B_SPLINE_SIGMA, final_radius);
  const int scales = CLAMP(diffusion_scales, 1, MAX_NUM_SCALES);
  fprintf(stdout, "scales : %i\n", scales);

  float third = data->third;
  float fourth = data->fourth;
  float second = data->second;
  float first = data->first;
  float anisotropy = expf(-data->anisotropy);
  float regularization = powf(10.f, data->regularization) - 1.f;
  float noise_threshold = powf(10.f, data->noise_threshold);

  // wavelets scales buffers
  float *const restrict LF_even
      = dt_alloc_align_float(roi_out->width * roi_out->height * ch); // low-frequencies RGB
  float *const restrict LF_odd
      = dt_alloc_align_float(roi_out->width * roi_out->height * ch);                      // low-frequencies RGB
  float *const restrict HF = dt_alloc_align_float(roi_out->width * roi_out->height * ch); // high-frequencies RGB

  if(!LF_even || !LF_odd || !HF)
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
    const float *restrict detail; // buffer containing this scale's input
    float *restrict LF;           // output buffer for the current scale

    // swap buffers so we only need 2 LF buffers : the LF at scale (s-1) and the one at current scale (s)
    if(s == 0)
    {
      detail = in;
      LF = LF_odd;
    }
    else if(s % 2 != 0)
    {
      detail = LF_odd;
      LF = LF_even;
    }
    else
    {
      detail = LF_even;
      LF = LF_odd;
    }

    // Compute wavelets low-frequency scales
    blur_2D_Bspline(detail, HF, LF, 1 << s, roi_out->width, roi_out->height);
    fprintf(stdout, "mult : %i\n", 1 << s);

    heat_PDE_inpanting(HF, LF, mask, reconstructed, roi_out->width, roi_out->height, ch, fourth, third,
                       second, first, anisotropy, regularization, noise_threshold, data->radius,
                       data->sharpness, s, scales, zoom);
  }

error:
  if(HF) dt_free_align(HF);
  if(LF_even) dt_free_align(LF_even);
  if(LF_odd) dt_free_align(LF_odd);
  return success;
}


static inline void build_mask(const float *const restrict input, uint8_t *const restrict mask,
                              const float threshold, const size_t width, const size_t height, const size_t ch)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) dt_omp_firstprivate(input, mask, height, width, ch, threshold)        \
    schedule(dynamic) aligned(mask, input : 64)
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
  float *restrict in = DT_IS_ALIGNED((float *const restrict)ivoid);
  float *const restrict out = DT_IS_ALIGNED((float *const restrict)ovoid);
  float *const restrict temp1 = dt_alloc_align_float(roi_out->width * roi_out->height * ch);
  float *const restrict temp2 = dt_alloc_align_float(roi_out->width * roi_out->height * ch);
  uint8_t *restrict mask = NULL;
  const float scale = fmaxf(piece->iscale / roi_in->scale, 1.f);

  if(data->threshold > 0.f)
  {
    const float blur = (float)data->radius / scale;

    // build a boolean mask, TRUE where image is above threshold
    mask = dt_alloc_align(64, roi_out->width * roi_out->height * sizeof(uint8_t));
    build_mask(in, mask, data->threshold, roi_out->width, roi_out->height, ch);

    // init the inpainting area with blur
    float RGBmax[4], RGBmin[4];
    for(int k = 0; k < 4; k++)
    {
      RGBmax[k] = INFINITY;
      RGBmin[k] = 0.f;
    }

    dt_gaussian_t *g = dt_gaussian_init(roi_out->width, roi_out->height, ch, RGBmax, RGBmin, blur, 0);
    if(!g) return;
    dt_gaussian_blur_4c(g, in, temp1);
    dt_gaussian_free(g);

    // add noise and restore valid parts where mask = FALSE
    const float noise = 0.2;

#ifdef _OPENMP
#pragma omp parallel for default(none) dt_omp_firstprivate(in, temp1, mask, roi_out, ch, noise) schedule(dynamic)
#endif
    for(size_t k = 0; k < roi_out->height * roi_out->width * ch; k += ch)
    {
      if(mask[k / ch])
      {
        const uint32_t i = k / roi_out->width;
        const uint32_t j = k - i;
        uint32_t DT_ALIGNED_ARRAY state[4]
            = { splitmix32(j + 1), splitmix32((j + 1) * (i + 3)), splitmix32(1337), splitmix32(666) };
        xoshiro128plus(state);
        xoshiro128plus(state);
        xoshiro128plus(state);
        xoshiro128plus(state);

        for(size_t c = 0; c < 4; c++) temp1[k + c] = gaussian_noise(temp1[k + c], noise, i % 2 || j % 2, state);
      }
      else
      {
        for(size_t c = 0; c < 4; c++) temp1[k + c] = in[k + c];
      }
    }

    in = temp1;
  }

  float *restrict temp_in = NULL;
  float *restrict temp_out = NULL;
  const int iterations = CLAMP(ceilf((float)data->iterations / scale), 1, MAX_NUM_SCALES);

  for(int it = 0; it < iterations; it++)
  {
    if(it == 0)
    {
      temp_in = in;
      temp_out = temp2;
    }
    else if(it % 2 == 0)
    {
      temp_in = temp1;
      temp_out = temp2;
    }
    else
    {
      temp_in = temp2;
      temp_out = temp1;
    }

    if(it == (int)iterations - 1) temp_out = out;
    reconstruct_highlights(temp_in, temp_out, mask, ch, data, piece, roi_in, roi_out);
  }

  if(mask) dt_free_align(mask);
  if(temp1) dt_free_align(temp1);
  if(temp2) dt_free_align(temp2);
}


void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_diffuse_gui_data_t *g = (dt_iop_diffuse_gui_data_t *)self->gui_data;
  dt_iop_diffuse_params_t *p = (dt_iop_diffuse_params_t *)self->params;
  dt_bauhaus_slider_set_soft(g->iterations, p->iterations);
  dt_bauhaus_slider_set_soft(g->fourth, p->fourth);
  dt_bauhaus_slider_set_soft(g->third, p->third);
  dt_bauhaus_slider_set_soft(g->second, p->second);
  dt_bauhaus_slider_set_soft(g->first, p->first);

  dt_bauhaus_slider_set_soft(g->noise_threshold, p->noise_threshold);
  dt_bauhaus_slider_set_soft(g->regularization, p->regularization);
  dt_bauhaus_slider_set_soft(g->anisotropy, p->anisotropy);
  dt_bauhaus_slider_set_soft(g->radius, p->radius);
  dt_bauhaus_slider_set_soft(g->sharpness, p->sharpness);
  dt_bauhaus_slider_set_soft(g->threshold, p->threshold);
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_diffuse_gui_data_t *g = IOP_GUI_ALLOC(diffuse);
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("diffusion properties")), FALSE, FALSE, 0);

  g->iterations = dt_bauhaus_slider_from_params(self, "iterations");
  gtk_widget_set_tooltip_text(g->iterations,
                              _("more iterations make the effect stronger but the module slower.\n"
                                "this is analogous to giving more time to the diffusion reaction.\n"
                                "if you plan on sharpening or inpainting, more iterations help reconstruction."));

  g->radius = dt_bauhaus_slider_from_params(self, "radius");
  dt_bauhaus_slider_set_format(g->radius, "%.0f px");
  gtk_widget_set_tooltip_text(
      g->radius, _("scale of the diffusion.\n"
                   "high values diffuse farther, at the expense of computation time.\n"
                   "low values diffuse closer.\n"
                   "if you plan on denoising, the radius should be around the width of your lens blur."));

  g->anisotropy = dt_bauhaus_slider_from_params(self, "anisotropy");
  dt_bauhaus_slider_set_digits(g->anisotropy, 4);
  gtk_widget_set_tooltip_text(g->anisotropy,
                              _("anisotropy of the diffusion.\n"
                                "high values force the diffusion to be 1D and perpendicular to edges.\n"
                                "low values allow the diffusion to be 2D and uniform, like a classic blur."));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("edges management")), FALSE,
                     FALSE, 0);

  g->sharpness = dt_bauhaus_slider_from_params(self, "sharpness");
  dt_bauhaus_slider_set_factor(g->sharpness, 100.0f);
  dt_bauhaus_slider_set_format(g->sharpness, "%.2f %%");
  gtk_widget_set_tooltip_text(g->sharpness,
                              _("weight of each iterations sharpness.\n"
                                "100 % is suitable for diffusion, inpainting and blurring.\n"
                                "lower if noise, halos or any artifact appear as you add more iterations."));


  g->regularization = dt_bauhaus_slider_from_params(self, "regularization");
  g->noise_threshold = dt_bauhaus_slider_from_params(self, "noise_threshold");


  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("diffusion typology")), FALSE,
                     FALSE, 0);

  g->first = dt_bauhaus_slider_from_params(self, "first");
  dt_bauhaus_slider_set_factor(g->first, 100.0f);
  dt_bauhaus_slider_set_digits(g->first, 4);
  dt_bauhaus_slider_set_format(g->first, "%.2f %%");
  gtk_widget_set_tooltip_text(g->first, _("smoothing or sharpening of smooth details (gradients).\n"
                                         "positive values diffuse and blur.\n"
                                         "negative values sharpen.\n"
                                         "zero does nothing."));

  g->second = dt_bauhaus_slider_from_params(self, "second");
  dt_bauhaus_slider_set_digits(g->second, 4);
  dt_bauhaus_slider_set_factor(g->second, 100.0f);
  dt_bauhaus_slider_set_format(g->second, "%.2f %%");

  g->third = dt_bauhaus_slider_from_params(self, "third");
  dt_bauhaus_slider_set_digits(g->third, 4);
  dt_bauhaus_slider_set_factor(g->third, 100.0f);
  dt_bauhaus_slider_set_format(g->third, "%.2f %%");
  gtk_widget_set_tooltip_text(g->third, _("smoothing or sharpening of sharp details.\n"
                                              "positive values diffuse and blur.\n"
                                              "negative values sharpen.\n"
                                              "zero does nothing."));

  g->fourth = dt_bauhaus_slider_from_params(self, "fourth");
  dt_bauhaus_slider_set_digits(g->fourth, 4);
  dt_bauhaus_slider_set_factor(g->fourth, 100.0f);
  dt_bauhaus_slider_set_format(g->fourth, "%.2f %%");
  gtk_widget_set_tooltip_text(g->fourth, _("smoothing or sharpening of sharp details (gradients).\n"
                                            "positive values diffuse and blur.\n"
                                            "negative values sharpen.\n"
                                            "zero does nothing."));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("diffusion spatiality")), FALSE, FALSE, 0);

  g->threshold = dt_bauhaus_slider_from_params(self, "threshold");
  gtk_widget_set_tooltip_text(g->threshold,
                              _("luminance threshold for the mask.\n"
                                "0. disables the luminance masking and applies the module on the whole image.\n"
                                "any higher value will exclude pixels whith luminance lower than the threshold.\n"
                                "this can be used to inpaint highlights."));
}
