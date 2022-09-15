/*
   This file is part of darktable,
   Copyright (C) 2021 darktable developers.

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

#include "common/extra_optimizations.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "bauhaus/bauhaus.h"
#include "common/bspline.h"
#include "common/darktable.h"
#include "common/dwt.h"
#include "common/gaussian.h"
#include "common/image.h"
#include "common/imagebuf.h"
#include "common/iop_profile.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop_gui.h"
#include "develop/imageop_math.h"
#include "develop/noise_generator.h"
#include "develop/openmp_maths.h"
#include "develop/tiling.h"
#include "dtgtk/button.h"
#include "dtgtk/drawingarea.h"
#include "dtgtk/expander.h"
#include "dtgtk/paint.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

// Set to one to output intermediate image steps as PFM in /tmp
#define DEBUG_DUMP_PFM 0

DT_MODULE_INTROSPECTION(2, dt_iop_diffuse_params_t)

#define MAX_NUM_SCALES 10
typedef struct dt_iop_diffuse_params_t
{
  // global parameters
  int iterations;           // $MIN: 0    $MAX: 500  $DEFAULT: 1  $DESCRIPTION: "iterations"
  float sharpness;          // $MIN: -1.  $MAX: 1.   $DEFAULT: 0. $DESCRIPTION: "sharpness"
  int radius;               // $MIN: 0    $MAX: 2048 $DEFAULT: 8  $DESCRIPTION: "radius span"
  float regularization;     // $MIN: 0.   $MAX: 4.   $DEFAULT: 0. $DESCRIPTION: "edge sensitivity"
  float variance_threshold; // $MIN: -2.  $MAX: 2.   $DEFAULT: 0. $DESCRIPTION: "edge threshold"

  float anisotropy_first;   // $MIN: -10. $MAX: 10.  $DEFAULT: 0. $DESCRIPTION: "1st order anisotropy"
  float anisotropy_second;  // $MIN: -10. $MAX: 10.  $DEFAULT: 0. $DESCRIPTION: "2nd order anisotropy"
  float anisotropy_third;   // $MIN: -10. $MAX: 10.  $DEFAULT: 0. $DESCRIPTION: "3rd order anisotropy"
  float anisotropy_fourth;  // $MIN: -10. $MAX: 10.  $DEFAULT: 0. $DESCRIPTION: "4th order anisotropy"

  float threshold;          // $MIN: 0.   $MAX: 8.   $DEFAULT: 0. $DESCRIPTION: "luminance masking threshold"

  float first;              // $MIN: -1.  $MAX: 1.   $DEFAULT: 0. $DESCRIPTION: "1st order speed"
  float second;             // $MIN: -1.  $MAX: 1.   $DEFAULT: 0. $DESCRIPTION: "2nd order speed"
  float third;              // $MIN: -1.  $MAX: 1.   $DEFAULT: 0. $DESCRIPTION: "3rd order speed"
  float fourth;             // $MIN: -1.  $MAX: 1.   $DEFAULT: 0. $DESCRIPTION: "4th order speed"

  // v2
  int radius_center;        // $MIN: 0    $MAX: 1024 $DEFAULT: 0  $DESCRIPTION: "central radius"

  // new versions add params mandatorily at the end, so we can memcpy old parameters at the beginning

} dt_iop_diffuse_params_t;


typedef struct dt_iop_diffuse_gui_data_t
{
  GtkWidget *iterations, *fourth, *third, *second, *radius, *radius_center, *sharpness, *threshold, *regularization, *first,
      *anisotropy_first, *anisotropy_second, *anisotropy_third, *anisotropy_fourth, *regularization_first, *variance_threshold;
} dt_iop_diffuse_gui_data_t;

typedef struct dt_iop_diffuse_global_data_t
{
  int kernel_filmic_bspline_vertical;
  int kernel_filmic_bspline_horizontal;
  int kernel_filmic_wavelets_detail;

  int kernel_diffuse_build_mask;
  int kernel_diffuse_inpaint_mask;
  int kernel_diffuse_pde;
} dt_iop_diffuse_global_data_t;


// only copy params struct to avoid a commit_params()
typedef struct dt_iop_diffuse_params_t dt_iop_diffuse_data_t;


typedef enum dt_isotropy_t
{
  DT_ISOTROPY_ISOTROPE = 0, // diffuse in all directions with same intensity
  DT_ISOTROPY_ISOPHOTE = 1, // diffuse more in the isophote direction (orthogonal to gradient)
  DT_ISOTROPY_GRADIENT = 2  // diffuse more in the gradient direction
} dt_isotropy_t;


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline dt_isotropy_t check_isotropy_mode(const float anisotropy)
{
  // user param is negative, positive or zero. The sign encodes the direction of diffusion, the magnitude encodes the ratio of anisotropy
  // ultimately, the anisotropy factor needs to be positive before going into the exponential
  if(anisotropy == 0.f)
    return DT_ISOTROPY_ISOTROPE;
  else if(anisotropy > 0.f)
    return DT_ISOTROPY_ISOPHOTE;
  else
    return DT_ISOTROPY_GRADIENT; // if(anisotropy > 0.f)
}


const char *name()
{
  return _("diffuse or sharpen");
}

const char *aliases()
{
  return _("diffusion|deconvolution|blur|sharpening");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self,
                                _("simulate directional diffusion of light with heat transfer model\n"
                                  "to apply an iterative edge-oriented blur,\n"
                                  "inpaint damaged parts of the image,"
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
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params,
                  const int new_version)
{
  if(old_version == 1 && new_version == 2)
  {
    typedef struct dt_iop_diffuse_params_v1_t
    {
      // global parameters
      int iterations;
      float sharpness;
      int radius;
      float regularization;
      float variance_threshold;

      float anisotropy_first;
      float anisotropy_second;
      float anisotropy_third;
      float anisotropy_fourth;

      float threshold;

      float first;
      float second;
      float third;
      float fourth;
    } dt_iop_diffuse_params_v1_t;

    dt_iop_diffuse_params_v1_t *o = (dt_iop_diffuse_params_v1_t *)old_params;
    dt_iop_diffuse_params_t *n = (dt_iop_diffuse_params_t *)new_params;
    dt_iop_diffuse_params_t *d = (dt_iop_diffuse_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    // copy common parameters
    memcpy(n, o, sizeof(dt_iop_diffuse_params_v1_t));

    // init only new parameters
    n->radius_center = 0;

    return 0;
  }
  return 1;
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_diffuse_params_t p;
  memset(&p, 0, sizeof(p));
  p.radius_center = 0;

  // deblurring presets
  p.sharpness = 0.0f;
  p.threshold = 0.0f;
  p.variance_threshold = +1.f;
  p.regularization = 3.f;

  p.anisotropy_first = +1.f;
  p.anisotropy_second = 0.f;
  p.anisotropy_third = +1.f;
  p.anisotropy_fourth = 0.f;

  p.first = -0.25f;
  p.second = +0.125f;
  p.third = -0.50f;
  p.fourth = +0.25f;

  p.radius = 8;
  p.iterations = 8;
  dt_gui_presets_add_generic(_("lens deblur: soft"), self->op, self->version(), &p, sizeof(p), 1,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  p.radius = 10;
  p.iterations = 16;
  dt_gui_presets_add_generic(_("lens deblur: medium"), self->op, self->version(), &p, sizeof(p), 1,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  p.radius = 12;
  p.iterations = 24;
  dt_gui_presets_add_generic(_("lens deblur: hard"), self->op, self->version(), &p, sizeof(p), 1,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  p.iterations = 10;
  p.radius = 512;
  p.sharpness = 0.f;
  p.variance_threshold = 0.25f;
  p.regularization = 2.5f;

  p.first = -0.20f;
  p.second = +0.10f;
  p.third = -0.20f;
  p.fourth = +0.10f;

  p.anisotropy_first = 2.f;
  p.anisotropy_second = 0.f;
  p.anisotropy_third = 2.f;
  p.anisotropy_fourth = 0.f;

  dt_gui_presets_add_generic(_("dehaze"), self->op, self->version(), &p, sizeof(p), 1,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  p.iterations = 32;
  p.sharpness = 0.f;
  p.threshold = 0.f;
  p.variance_threshold = -0.25f;
  p.regularization = 4.f;

  p.anisotropy_first = +2.f;
  p.anisotropy_second = 0.f;
  p.anisotropy_third = +2.f;
  p.anisotropy_fourth = 0.f;

  p.radius = 1;
  p.radius_center = 2;

  p.first = +0.06f;
  p.second = 0.f;
  p.third = +0.06f;
  p.fourth = 0.f;
  dt_gui_presets_add_generic(_("denoise: fine"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  p.radius = 3;
  p.radius_center = 4;

  p.first = +0.05f;
  p.second = 0.f;
  p.third = +0.05f;
  p.fourth = 0.f;
  dt_gui_presets_add_generic(_("denoise: medium"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  p.radius = 6;
  p.radius_center = 8;

  p.first = +0.04f;
  p.second = 0.f;
  p.third = +0.04f;
  p.fourth = 0.f;
  dt_gui_presets_add_generic(_("denoise: coarse"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  p.radius_center = 0;

  p.iterations = 2;
  p.radius = 32;
  p.sharpness = 0.0f;
  p.threshold = 0.0f;
  p.variance_threshold = 0.f;
  p.regularization = 4.f;

  p.anisotropy_first = +4.f;
  p.anisotropy_second = +4.f;
  p.anisotropy_third = +4.f;
  p.anisotropy_fourth = +4.f;

  p.first = +1.f;
  p.second = +1.f;
  p.third = +1.f;
  p.fourth = +1.f;
  dt_gui_presets_add_generic(_("surface blur"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  p.iterations = 1;
  p.radius = 32;
  p.sharpness = 0.0f;
  p.threshold = 0.0f;
  p.variance_threshold = 0.f;
  p.regularization = 0.f;

  p.anisotropy_first = 0.f;
  p.anisotropy_second = 0.f;
  p.anisotropy_third = 0.f;
  p.anisotropy_fourth = 0.f;

  p.first = +0.5f;
  p.second = +0.5f;
  p.third = +0.5f;
  p.fourth = +0.5f;
  dt_gui_presets_add_generic(_("bloom"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  p.iterations = 1;
  p.radius = 4;
  p.sharpness = 0.0f;
  p.threshold = 0.0f;
  p.variance_threshold = 0.f;
  p.regularization = 1.f;

  p.anisotropy_first = +1.f;
  p.anisotropy_second = +1.f;
  p.anisotropy_third = +1.f;
  p.anisotropy_fourth = +1.f;

  p.first = -0.25f;
  p.second = -0.25f;
  p.third = -0.25f;
  p.fourth = -0.25f;
  dt_gui_presets_add_generic(_("sharpen demosaicing (no AA filter)"), self->op, self->version(), &p, sizeof(p), 1,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  p.radius = 8;
  dt_gui_presets_add_generic(_("sharpen demosaicing (AA filter)"), self->op, self->version(), &p, sizeof(p), 1,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  p.iterations = 4;
  p.radius = 64;
  p.sharpness = 0.0f;
  p.threshold = 0.0f;
  p.variance_threshold = 0.f;
  p.regularization = 2.f;

  p.anisotropy_first = 0.f;
  p.anisotropy_second = 0.f;
  p.anisotropy_third = +4.f;
  p.anisotropy_fourth = +4.f;

  p.first = 0.f;
  p.second = 0.f;
  p.third = +0.5f;
  p.fourth = +0.5f;
  dt_gui_presets_add_generic(_("simulate watercolor"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  p.iterations = 50;
  p.radius = 64;
  p.sharpness = 0.0f;
  p.threshold = 0.0f;
  p.variance_threshold = 0.f;
  p.regularization = 4.f;

  p.anisotropy_first = -5.f;
  p.anisotropy_second = -5.f;
  p.anisotropy_third = -5.f;
  p.anisotropy_fourth = -5.f;

  p.first = -1.f;
  p.second = -1.f;
  p.third = -1.f;
  p.fourth = -1.f;
  dt_gui_presets_add_generic(_("simulate line drawing"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // local contrast
  p.sharpness = 0.0f;
  p.threshold = 0.0f;
  p.variance_threshold = 1.f;

  p.anisotropy_first = -2.5f;
  p.anisotropy_second = 0.f;
  p.anisotropy_third = 0.f;
  p.anisotropy_fourth = -2.5f;

  p.first = -0.50f;
  p.second = 0.f;
  p.third = 0.f;
  p.fourth = -0.50f;

  p.iterations = 10;
  p.radius = 384;
  p.radius_center = 512;
  p.regularization = 1.f;
  dt_gui_presets_add_generic(_("add local contrast"), self->op, self->version(), &p, sizeof(p), 1,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  p.iterations = 32;
  p.radius = 4;
  p.radius_center = 0;
  p.sharpness = 0.0f;
  p.threshold = 1.41f;
  p.variance_threshold = 0.f;
  p.regularization = 0.f;

  p.anisotropy_first = +0.f;
  p.anisotropy_second = +0.f;
  p.anisotropy_third = +0.f;
  p.anisotropy_fourth = +2.f;

  p.first = +0.0f;
  p.second = +0.0f;
  p.third = +0.0f;
  p.fourth = +0.5f;
  dt_gui_presets_add_generic(_("inpaint highlights"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // fast presets for slow hardware
  p.radius_center = 0;
  p.radius = 128;
  p.sharpness = 0.0f;
  p.threshold = 0.0f;
  p.variance_threshold = 0.25f;
  p.regularization = 0.25f;

  p.anisotropy_first = 0.f;
  p.anisotropy_second = 0.f;
  p.anisotropy_third = 5.f;
  p.anisotropy_fourth = 0.f;

  p.first = 0.f;
  p.second = 0.f;
  p.third = -0.50f;
  p.fourth = 0.f;

  p.iterations = 1;
  dt_gui_presets_add_generic(_("fast sharpness"), self->op, self->version(), &p, sizeof(p), 1,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  p.radius_center = 512;
  p.radius = 512;
  p.sharpness = 0.0f;
  p.threshold = 0.0f;
  p.variance_threshold = 0.05f;
  p.regularization = 0.01f;


  p.anisotropy_first = 0.f;
  p.anisotropy_second = 0.f;
  p.anisotropy_third = 5.f;
  p.anisotropy_fourth = 0.f;

  p.first = 0.f;
  p.second = 0.f;
  p.third = -0.50f;
  p.fourth = 0.f;

  p.iterations = 1;
  dt_gui_presets_add_generic(_("fast local contrast"), self->op, self->version(), &p, sizeof(p), 1,
                             DEVELOP_BLEND_CS_RGB_SCENE);
}

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  dt_iop_diffuse_data_t *data = (dt_iop_diffuse_data_t *)piece->data;

  const float scale = fmaxf(piece->iscale / roi_in->scale, 1.f);
  const float final_radius = (data->radius + data->radius_center) * 2.f / scale;
  const int diffusion_scales = num_steps_to_reach_equivalent_sigma(B_SPLINE_SIGMA, final_radius);
  const int scales = CLAMP(diffusion_scales, 1, MAX_NUM_SCALES);
  const int max_filter_radius = (1 << scales);

  // in + out + 2 * tmp + 2 * LF + s details + grey mask
  tiling->factor = 6.25f + scales;
  tiling->factor_cl = 6.25f + scales;

  tiling->maxbuf = 1.0f;
  tiling->maxbuf_cl = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = max_filter_radius;
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

static inline void init_reconstruct(float *const restrict reconstructed, const size_t width, const size_t height)
{
// init the reconstructed buffer with non-clipped and partially clipped pixels
#ifdef _OPENMP
#pragma omp parallel for simd default(none) dt_omp_firstprivate(reconstructed, width, height)                 \
    schedule(simd:static) aligned(reconstructed:64)
#endif
  for(size_t k = 0; k < height * width * 4; k++) reconstructed[k] = 0.f;
}


// Discretization parameters for the Partial Derivative Equation solver
#define H 1         // spatial step
#define KAPPA 0.25f // 0.25 if h = 1, 1 if h = 2


#ifdef _OPENMP
#pragma omp declare simd aligned(pixels:64) aligned(xy:16) uniform(pixels)
#endif
static inline void find_gradients(const dt_aligned_pixel_t pixels[9], dt_aligned_pixel_t xy[2])
{
  // Compute the gradient with centered finite differences in a 3×3 stencil
  // warning : x is vertical, y is horizontal
  for_each_channel(c,aligned(pixels:64) aligned(xy))
  {
    xy[0][c] = (pixels[7][c] - pixels[1][c]) / 2.f;
    xy[1][c] = (pixels[5][c] - pixels[3][c]) / 2.f;
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(pixels:64) aligned(xy:16) uniform(pixels)
#endif
static inline void find_laplacians(const dt_aligned_pixel_t pixels[9], dt_aligned_pixel_t xy[2])
{
  // Compute the laplacian with centered finite differences in a 3×3 stencil
  // warning : x is vertical, y is horizontal
  for_each_channel(c, aligned(xy) aligned(pixels:64))
  {
    xy[0][c] = (pixels[7][c] + pixels[1][c]) - 2.f * pixels[4][c];
    xy[1][c] = (pixels[5][c] + pixels[3][c]) - 2.f * pixels[4][c];
  }
}


#ifdef _OPENMP
#pragma omp declare simd aligned(a, c2, cos_theta_sin_theta, cos_theta2, sin_theta2:16)
#endif
static inline void rotation_matrix_isophote(const dt_aligned_pixel_t c2, const dt_aligned_pixel_t cos_theta_sin_theta,
                                            const dt_aligned_pixel_t cos_theta2,
                                            const dt_aligned_pixel_t sin_theta2, dt_aligned_pixel_t a[2][2])
{
  // Write the coefficients of a square symmetrical matrice of rotation of the gradient :
  // [[ a11, a12 ],
  //  [ a12, a22 ]]
  // taken from https://www.researchgate.net/publication/220663968
  // c dampens the gradient direction
  for_each_channel(c)
  {
    a[0][0][c] = cos_theta2[c] + c2[c] * sin_theta2[c];
    a[1][1][c] = c2[c] * cos_theta2[c] + sin_theta2[c];
    a[0][1][c] = a[1][0][c] = (c2[c] - 1.0f) * cos_theta_sin_theta[c];
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(a, c2, cos_theta_sin_theta, cos_theta2, sin_theta2:16)
#endif
static inline void rotation_matrix_gradient(const dt_aligned_pixel_t c2, const dt_aligned_pixel_t cos_theta_sin_theta,
                                            const dt_aligned_pixel_t cos_theta2,
                                            const dt_aligned_pixel_t sin_theta2, dt_aligned_pixel_t a[2][2])
{
  // Write the coefficients of a square symmetrical matrice of rotation of the gradient :
  // [[ a11, a12 ],
  //  [ a12, a22 ]]
  // based on https://www.researchgate.net/publication/220663968 and inverted
  // c dampens the isophote direction
  for_each_channel(c)
  {
    a[0][0][c] = c2[c] * cos_theta2[c] + sin_theta2[c];
    a[1][1][c] = cos_theta2[c] + c2[c] * sin_theta2[c];
    a[0][1][c] = a[1][0][c] = (1.0f - c2[c]) * cos_theta_sin_theta[c];
  }
}


#ifdef _OPENMP
#pragma omp declare simd aligned(a, kernel: 64)
#endif
static inline void build_matrix(const dt_aligned_pixel_t a[2][2], dt_aligned_pixel_t kernel[9])
{
  for_each_channel(c)
  {
    const float b11 = a[0][1][c] / 2.0f;
    const float b13 = -b11;
    const float b22 = -2.0f * (a[0][0][c] + a[1][1][c]);

    // build the kernel of rotated anisotropic laplacian
    // from https://www.researchgate.net/publication/220663968 :
    // [ [ a12 / 2,  a22,            -a12 / 2 ],
    //   [ a11,      -2 (a11 + a22), a11      ],
    //   [ -a12 / 2,   a22,          a12 / 2  ] ]
    // N.B. we have flipped the signs of the a12 terms
    // compared to the paper. There's probably a mismatch
    // of coordinate convention between the paper and the
    // original derivation of this convolution mask
    // (Witkin 1991, https://doi.org/10.1145/127719.122750).
    kernel[0][c] = b11;
    kernel[1][c] = a[1][1][c];
    kernel[2][c] = b13;
    kernel[3][c] = a[0][0][c];
    kernel[4][c] = b22;
    kernel[5][c] = a[0][0][c];
    kernel[6][c] = b13;
    kernel[7][c] = a[1][1][c];
    kernel[8][c] = b11;
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(kernel: 64)
#endif
static inline void isotrope_laplacian(dt_aligned_pixel_t kernel[9])
{
  // see in https://eng.aurelienpierre.com/2021/03/rotation-invariant-laplacian-for-2d-grids/#Second-order-isotropic-finite-differences
  // for references (Oono & Puri)
  for_each_channel(c)
  {
    kernel[0][c] = 0.25f;
    kernel[1][c] = 0.5f;
    kernel[2][c] = 0.25f;
    kernel[3][c] = 0.5f;
    kernel[4][c] = -3.f;
    kernel[5][c] = 0.5f;
    kernel[6][c] = 0.25f;
    kernel[7][c] = 0.5f;
    kernel[8][c] = 0.25f;
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(kernel, c2: 64) uniform(isotropy_type)
#endif
static inline void compute_kernel(const dt_aligned_pixel_t c2, const dt_aligned_pixel_t cos_theta_sin_theta,
                                  const dt_aligned_pixel_t cos_theta2, const dt_aligned_pixel_t sin_theta2,
                                  const dt_isotropy_t isotropy_type, dt_aligned_pixel_t kernel[9])
{
  // Build the matrix of rotation with anisotropy

  switch(isotropy_type)
  {
    case(DT_ISOTROPY_ISOTROPE):
    default:
    {
      isotrope_laplacian(kernel);
      break;
    }
    case(DT_ISOTROPY_ISOPHOTE):
    {
      dt_aligned_pixel_t a[2][2] = { { { 0.f } } };
      rotation_matrix_isophote(c2, cos_theta_sin_theta, cos_theta2, sin_theta2, a);
      build_matrix(a, kernel);
      break;
    }
    case(DT_ISOTROPY_GRADIENT):
    {
      dt_aligned_pixel_t a[2][2] = { { { 0.f } } };
      rotation_matrix_gradient(c2, cos_theta_sin_theta, cos_theta2, sin_theta2, a);
      build_matrix(a, kernel);
      break;
    }
  }
}

static inline void heat_PDE_diffusion(const float *const restrict high_freq, const float *const restrict low_freq,
                                      const uint8_t *const restrict mask, const int has_mask,
                                      float *const restrict output, const size_t width, const size_t height,
                                      const dt_aligned_pixel_t anisotropy, const dt_isotropy_t isotropy_type[4],
                                      const float regularization, const float variance_threshold,
                                      const float current_radius_square, const int mult,
                                      const dt_aligned_pixel_t ABCD, const float strength)
{
  // Simultaneous inpainting for image structure and texture using anisotropic heat transfer model
  // https://www.researchgate.net/publication/220663968
  // modified as follow :
  //  * apply it in a multi-scale wavelet setup : we basically solve it twice, on the wavelets LF and HF layers.
  //  * replace the manual texture direction/distance selection by an automatic detection similar to the structure one,
  //  * generalize the framework for isotropic diffusion and anisotropic weighted on the isophote direction
  //  * add a variance regularization to better avoid edges.
  // The sharpness setting mimics the contrast equalizer effect by simply multiplying the HF by some gain.

  float *const restrict out = DT_IS_ALIGNED(output);
  const float *const restrict LF = DT_IS_ALIGNED(low_freq);
  const float *const restrict HF = DT_IS_ALIGNED(high_freq);

  const float regularization_factor = regularization * current_radius_square / 9.f;

#ifdef _OPENMP
#pragma omp parallel for default(none)                                                                            \
    dt_omp_firstprivate(out, mask, HF, LF, height, width, ABCD, has_mask, variance_threshold, anisotropy,         \
                        regularization_factor, mult, strength, isotropy_type) schedule(static)
#endif
  for(size_t row = 0; row < height; ++row)
  {
    // interleave the order in which we process the rows so that we minimize cache misses
    const size_t i = dwt_interleave_rows(row, height, mult);
    // compute the 'above' and 'below' coordinates, clamping them to the image, once for the entire row
    const size_t i_neighbours[3]
      = { MAX((int)(i - mult * H), (int)0) * width,            // x - mult
          i * width,                                           // x
          MIN((int)(i + mult * H), (int)height - 1) * width }; // x + mult
    for(size_t j = 0; j < width; ++j)
    {
      const size_t idx = (i * width + j);
      const size_t index = idx * 4;
      const uint8_t opacity = (has_mask) ? mask[idx] : 1;

      if(opacity)
      {
        // non-local neighbours coordinates
        const size_t j_neighbours[3]
          = { MAX((int)(j - mult * H), (int)0),            // y - mult
              j,                                          // y
              MIN((int)(j + mult * H), (int)width - 1) }; // y + mult

        // fetch non-local pixels and store them locally and contiguously
        dt_aligned_pixel_t neighbour_pixel_HF[9];
        dt_aligned_pixel_t neighbour_pixel_LF[9];

        for(size_t ii = 0; ii < 3; ii++)
          for(size_t jj = 0; jj < 3; jj++)
          {
            size_t neighbor = 4 * (i_neighbours[ii] + j_neighbours[jj]);
            for_each_channel(c)
            {
              neighbour_pixel_HF[3 * ii + jj][c] = HF[neighbor + c];
              neighbour_pixel_LF[3 * ii + jj][c] = LF[neighbor + c];
            }
          }

        // c² in https://www.researchgate.net/publication/220663968
        dt_aligned_pixel_t c2[4];
        // build the local anisotropic convolution filters for gradients and laplacians
        dt_aligned_pixel_t gradient[2], laplacian[2]; // x, y for each channel
        find_gradients(neighbour_pixel_LF, gradient);
        find_gradients(neighbour_pixel_HF, laplacian);

        dt_aligned_pixel_t cos_theta_grad_sq;
        dt_aligned_pixel_t sin_theta_grad_sq;
        dt_aligned_pixel_t cos_theta_sin_theta_grad;
        for_each_channel(c)
        {
          float magnitude_grad = sqrtf(sqf(gradient[0][c]) + sqf(gradient[1][c]));
          c2[0][c] = -magnitude_grad * anisotropy[0];
          c2[2][c] = -magnitude_grad * anisotropy[2];
          // Compute cos(arg(grad)) = dx / hypot - force arg(grad) = 0 if hypot == 0
          gradient[0][c] = (magnitude_grad != 0.f) ? gradient[0][c] / magnitude_grad : 1.f; // cos(0)
          // Compute sin (arg(grad))= dy / hypot - force arg(grad) = 0 if hypot == 0
          gradient[1][c] = (magnitude_grad != 0.f) ? gradient[1][c] / magnitude_grad : 0.f; // sin(0)
          // Warning : now gradient = { cos(arg(grad)) , sin(arg(grad)) }
          cos_theta_grad_sq[c] = sqf(gradient[0][c]);
          sin_theta_grad_sq[c] = sqf(gradient[1][c]);
          cos_theta_sin_theta_grad[c] = gradient[0][c] * gradient[1][c];
        }

        dt_aligned_pixel_t cos_theta_lapl_sq;
        dt_aligned_pixel_t sin_theta_lapl_sq;
        dt_aligned_pixel_t cos_theta_sin_theta_lapl;
        for_each_channel(c)
        {
          float magnitude_lapl = sqrtf(sqf(laplacian[0][c]) + sqf(laplacian[1][c]));
          c2[1][c] = -magnitude_lapl * anisotropy[1];
          c2[3][c] = -magnitude_lapl * anisotropy[3];
          // Compute cos(arg(lapl)) = dx / hypot - force arg(lapl) = 0 if hypot == 0
          laplacian[0][c] = (magnitude_lapl != 0.f) ? laplacian[0][c] / magnitude_lapl : 1.f; // cos(0)
          // Compute sin (arg(lapl))= dy / hypot - force arg(lapl) = 0 if hypot == 0
          laplacian[1][c] = (magnitude_lapl != 0.f) ? laplacian[1][c] / magnitude_lapl : 0.f; // sin(0)
          // Warning : now laplacian = { cos(arg(lapl)) , sin(arg(lapl)) }
          cos_theta_lapl_sq[c] = sqf(laplacian[0][c]);
          sin_theta_lapl_sq[c] = sqf(laplacian[1][c]);
          cos_theta_sin_theta_lapl[c] = laplacian[0][c] * laplacian[1][c];
        }

        // elements of c2 need to be expf(mag*anistropy), but we haven't applied the expf() yet.  Do that now.
        for(size_t k = 0; k < 4; k++)
        {
          dt_fast_expf_4wide(c2[k], c2[k]);
        }

        dt_aligned_pixel_t kern_first[9], kern_second[9], kern_third[9], kern_fourth[9];
        compute_kernel(c2[0], cos_theta_sin_theta_grad, cos_theta_grad_sq, sin_theta_grad_sq, isotropy_type[0],
                       kern_first);
        compute_kernel(c2[1], cos_theta_sin_theta_lapl, cos_theta_lapl_sq, sin_theta_lapl_sq, isotropy_type[1],
                       kern_second);
        compute_kernel(c2[2], cos_theta_sin_theta_grad, cos_theta_grad_sq, sin_theta_grad_sq, isotropy_type[2],
                       kern_third);
        compute_kernel(c2[3], cos_theta_sin_theta_lapl, cos_theta_lapl_sq, sin_theta_lapl_sq, isotropy_type[3],
                       kern_fourth);

        dt_aligned_pixel_t derivatives[4] = { { 0.f } };
        dt_aligned_pixel_t variance = { 0.f };
        // convolve filters and compute the variance and the regularization term
        for(size_t k = 0; k < 9; k++)
        {
          for_each_channel(c,aligned(derivatives,neighbour_pixel_LF,kern_first,kern_second))
          {
            derivatives[0][c] += kern_first[k][c] * neighbour_pixel_LF[k][c];
            derivatives[1][c] += kern_second[k][c] * neighbour_pixel_LF[k][c];
            derivatives[2][c] += kern_third[k][c] * neighbour_pixel_HF[k][c];
            derivatives[3][c] += kern_fourth[k][c] * neighbour_pixel_HF[k][c];
            variance[c] += sqf(neighbour_pixel_HF[k][c]);
          }
        }
        // Regularize the variance taking into account the blurring scale.
        // This allows to keep the scene-referred variance roughly constant
        // regardless of the wavelet scale where we compute it.
        // Prevents large scale halos when deblurring.
        for_each_channel(c, aligned(variance))
        {
          variance[c] = variance_threshold + variance[c] * regularization_factor;
        }
        // compute the update
        dt_aligned_pixel_t acc = { 0.f };
        for(size_t k = 0; k < 4; k++)
        {
          for_each_channel(c, aligned(acc,derivatives,ABCD))
            acc[c] += derivatives[k][c] * ABCD[k];
        }
        for_each_channel(c, aligned(acc,HF,LF,variance,out))
        {
          acc[c] = (HF[index + c] * strength + acc[c] / variance[c]);
          // update the solution
          out[index + c] = fmaxf(acc[c] + LF[index + c], 0.f);
        }
      }
      else
      {
        // only copy input to output, do nothing
        for_each_channel(c, aligned(out, HF, LF : 64))
          out[index + c] = HF[index + c] + LF[index + c];
      }
    }
  }
}

static inline float compute_anisotropy_factor(const float user_param)
{
  // compute the inverse of the K param in c evaluation from
  // https://www.researchgate.net/publication/220663968
  // but in a perceptually-even way, for better GUI interaction
  return sqf(user_param);
}

#if DEBUG_DUMP_PFM
static void dump_PFM(const char *filename, const float* out, const uint32_t w, const uint32_t h)
{
  FILE *f = g_fopen(filename, "wb");
  fprintf(f, "PF\n%d %d\n-1.0\n", w, h);
  for(int j = h - 1 ; j >= 0 ; j--)
    for(int i = 0 ; i < w ; i++)
      for(int c = 0 ; c < 3 ; c++)
        fwrite(out + (j * w + i) * 4 + c, 1, sizeof(float), f);
  fclose(f);
}
#endif

static inline gint wavelets_process(const float *const restrict in, float *const restrict reconstructed,
                                    const uint8_t *const restrict mask, const size_t width,
                                    const size_t height, const dt_iop_diffuse_data_t *const data,
                                    const float final_radius, const float zoom, const int scales,
                                    const int has_mask,
                                    float *const restrict HF[MAX_NUM_SCALES],
                                    float *const restrict LF_odd,
                                    float *const restrict LF_even)
{
  gint success = TRUE;

  const dt_aligned_pixel_t anisotropy
      = { compute_anisotropy_factor(data->anisotropy_first),
          compute_anisotropy_factor(data->anisotropy_second),
          compute_anisotropy_factor(data->anisotropy_third),
          compute_anisotropy_factor(data->anisotropy_fourth) };

  const dt_isotropy_t DT_ALIGNED_PIXEL isotropy_type[4]
      = { check_isotropy_mode(data->anisotropy_first),
          check_isotropy_mode(data->anisotropy_second),
          check_isotropy_mode(data->anisotropy_third),
          check_isotropy_mode(data->anisotropy_fourth) };

  const float regularization = powf(10.f, data->regularization) - 1.f;
  const float variance_threshold = powf(10.f, data->variance_threshold);

  // À trous decimated wavelet decompose
  // there is a paper from a guy we know that explains it : https://jo.dreggn.org/home/2010_atrous.pdf
  // the wavelets decomposition here is the same as the equalizer/atrous module,
  float *restrict residual; // will store the temp buffer containing the last step of blur
  // allocate a one-row temporary buffer for the decomposition
  size_t padded_size;
  float *const DT_ALIGNED_ARRAY tempbuf = dt_alloc_perthread_float(4 * width, &padded_size); //TODO: alloc in caller
  for(int s = 0; s < scales; ++s)
  {
    /* fprintf(stdout, "Wavelet decompose : scale %i\n", s); */
    const int mult = 1 << s;

    const float *restrict buffer_in;
    float *restrict buffer_out;

    if(s == 0)
    {
      buffer_in = in;
      buffer_out = LF_odd;
    }
    else if(s % 2 != 0)
    {
      buffer_in = LF_odd;
      buffer_out = LF_even;
    }
    else
    {
      buffer_in = LF_even;
      buffer_out = LF_odd;
    }

    decompose_2D_Bspline(buffer_in, HF[s], buffer_out, width, height, mult, tempbuf, padded_size);

    residual = buffer_out;

#if DEBUG_DUMP_PFM
    char name[64];
    sprintf(name, "/tmp/scale-input-%i.pfm", s);
    dump_PFM(name, buffer_in, width, height);

    sprintf(name, "/tmp/scale-blur-%i.pfm", s);
    dump_PFM(name, buffer_out, width, height);
#endif
  }
  dt_free_align(tempbuf);

  // will store the temp buffer NOT containing the last step of blur
  float *restrict temp = (residual == LF_even) ? LF_odd : LF_even;

  int count = 0;
  for(int s = scales - 1; s > -1; --s)
  {
    const int mult = 1 << s;
    const float current_radius = equivalent_sigma_at_step(B_SPLINE_SIGMA, s);
    const float real_radius = current_radius * zoom;

    const float norm = expf(-sqf(real_radius - (float)data->radius_center) / sqf(data->radius));
    const dt_aligned_pixel_t ABCD = { data->first * KAPPA * norm, data->second * KAPPA * norm,
                                      data->third * KAPPA * norm, data->fourth * KAPPA * norm };
    const float strength = data->sharpness * norm + 1.f;

    /* debug
    fprintf(stdout, "PDE solve : scale %i : mult = %i ; current rad = %.0f ; real rad = %.0f ; norm = %f ; strength = %f\n", s,
            1 << s, current_radius, real_radius, norm, strength);
    */

    const float *restrict buffer_in;
    float *restrict buffer_out;

    if(count == 0)
    {
      buffer_in = residual;
      buffer_out = temp;
    }
    else if(count % 2 != 0)
    {
      buffer_in = temp;
      buffer_out = residual;
    }
    else
    {
      buffer_in = residual;
      buffer_out = temp;
    }

    if(s == 0) buffer_out = reconstructed;

    // Compute wavelets low-frequency scales
    heat_PDE_diffusion(HF[s], buffer_in, mask, has_mask, buffer_out, width, height,
                       anisotropy, isotropy_type, regularization,
                       variance_threshold, sqf(current_radius), mult, ABCD, strength);

#if DEBUG_DUMP_PFM
    char name[64];
    sprintf(name, "/tmp/scale-up-unblur-%i.pfm", s);
    dump_PFM(name, buffer_out, width, height);
#endif

    count++;
  }

  return success;
}


static inline void build_mask(const float *const restrict input, uint8_t *const restrict mask,
                              const float threshold, const size_t width, const size_t height)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) dt_omp_firstprivate(input, mask, height, width, threshold)        \
    schedule(simd:static) aligned(mask, input : 64)
#endif
  for(size_t k = 0; k < height * width * 4; k += 4)
  {
    // TRUE if any channel is above threshold
    mask[k / 4] = (input[k] > threshold || input[k + 1] > threshold || input[k + 2] > threshold);
  }
}

static inline void inpaint_mask(float *const restrict inpainted, const float *const restrict original,
                                const uint8_t *const restrict mask,
                                const size_t width, const size_t height)
{
  // init the reconstruction with noise inside the masked areas
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(inpainted, original, mask, width, height) schedule(simd:static)
#endif
  for(size_t k = 0; k < height * width * 4; k += 4)
  {
    if(mask[k / 4])
    {
      const uint32_t i = k / width;
      const uint32_t j = k - i;
      uint32_t DT_ALIGNED_ARRAY state[4]
          = { splitmix32(j + 1), splitmix32((uint64_t)(j + 1) * (i + 3)),
              splitmix32(1337), splitmix32(666) };
      xoshiro128plus(state);
      xoshiro128plus(state);
      xoshiro128plus(state);
      xoshiro128plus(state);

      for_four_channels(c, aligned(inpainted, original, state:64))
        inpainted[k + c] = fabsf(gaussian_noise(original[k + c], original[k + c], i % 2 || j % 2, state));
    }
    else
    {
      for_four_channels(c, aligned(original, inpainted:64))
        inpainted[k + c] = original[k + c];
    }
  }
}

void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const restrict ivoid,
             void *const restrict ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const gboolean fastmode = (piece->pipe->type & DT_DEV_PIXELPIPE_FAST) == DT_DEV_PIXELPIPE_FAST;

  const dt_iop_diffuse_data_t *const data = (dt_iop_diffuse_data_t *)piece->data;

  const size_t width = roi_out->width;
  const size_t height = roi_out->height;

  // allow fast mode, just copy input to ouput
  if(fastmode)
  {
    const size_t ch = piece->colors;
    dt_iop_copy_image_roi(ovoid, ivoid, ch, roi_in, roi_out, TRUE);
    return;
  }

  float *restrict in = DT_IS_ALIGNED((float *const restrict)ivoid);
  float *const restrict out = DT_IS_ALIGNED((float *const restrict)ovoid);

  float *const restrict temp1 = dt_alloc_align_float((size_t)roi_out->width * roi_out->height * 4);
  float *const restrict temp2 = dt_alloc_align_float((size_t)roi_out->width * roi_out->height * 4);

  float *restrict temp_in = NULL;
  float *restrict temp_out = NULL;

  uint8_t *const restrict mask = dt_alloc_align(64, sizeof(uint8_t) * roi_out->width * roi_out->height);

  const float scale = fmaxf(piece->iscale / roi_in->scale, 1.f);
  const float final_radius = (data->radius + data->radius_center) * 2.f / scale;

  const int iterations = MAX(ceilf((float)data->iterations), 1);
  const int diffusion_scales = num_steps_to_reach_equivalent_sigma(B_SPLINE_SIGMA, final_radius);
  const int scales = CLAMP(diffusion_scales, 1, MAX_NUM_SCALES);

  self->cache_next_important = ((data->iterations * (data->radius + data->radius_center)) > 16);

  gboolean out_of_memory = FALSE;

  // wavelets scales buffers
  float *restrict HF[MAX_NUM_SCALES];
  for(int s = 0; s < scales; s++)
  {
    HF[s] = dt_alloc_align_float(width * height * 4);
    if(!HF[s]) out_of_memory = TRUE;
  }

  // temp buffer for blurs. We will need to cycle between them for memory efficiency
  float *const restrict LF_odd = dt_alloc_align_float(width * height * 4);
  float *const restrict LF_even = dt_alloc_align_float(width * height * 4);

  // PAUSE !
  // check that all buffers exist before processing,
  // because we use a lot of memory here.
  if(!temp1 || !temp2 || !LF_odd || !LF_even || out_of_memory)
  {
    dt_control_log(_("diffuse/sharpen failed to allocate memory, check your RAM settings"));
    goto error;
  }

  const int has_mask = (data->threshold > 0.f);

  if(has_mask)
  {
    // build a boolean mask, TRUE where image is above threshold, FALSE otherwise
    build_mask(in, mask, data->threshold, roi_out->width, roi_out->height);

    // init the inpainting area with noise
    inpaint_mask(temp1, in, mask, roi_out->width, roi_out->height);

    in = temp1;
  }

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

    if(it == (int)iterations - 1)
      temp_out = out;

    wavelets_process(temp_in, temp_out, mask,
                     roi_out->width, roi_out->height,
                     data, final_radius, scale, scales, has_mask, HF, LF_odd, LF_even);
  }

error:
  if(mask) dt_free_align(mask);
  if(temp1) dt_free_align(temp1);
  if(temp2) dt_free_align(temp2);
  if(LF_even) dt_free_align(LF_even);
  if(LF_odd) dt_free_align(LF_odd);
  for(int s = 0; s < scales; s++) if(HF[s]) dt_free_align(HF[s]);
}

#if HAVE_OPENCL
static inline cl_int wavelets_process_cl(const int devid, cl_mem in, cl_mem reconstructed, cl_mem mask,
                                         const size_t sizes[3], const int width, const int height,
                                         const dt_iop_diffuse_data_t *const data,
                                         dt_iop_diffuse_global_data_t *const gd,
                                         const float final_radius, const float zoom, const int scales,
                                         const int has_mask,
                                         cl_mem HF[MAX_NUM_SCALES],
                                         cl_mem LF_odd,
                                         cl_mem LF_even)
{
  cl_int err = -999;

  const dt_aligned_pixel_t anisotropy
      = { compute_anisotropy_factor(data->anisotropy_first),
          compute_anisotropy_factor(data->anisotropy_second),
          compute_anisotropy_factor(data->anisotropy_third),
          compute_anisotropy_factor(data->anisotropy_fourth) };

  /*
  fprintf(stdout, "anisotropy : %f ; %f ; %f ; %f \n",
                  anisotropy[0], anisotropy[1], anisotropy[2], anisotropy[3]);
  */

  const dt_isotropy_t DT_ALIGNED_PIXEL isotropy_type[4]
      = { check_isotropy_mode(data->anisotropy_first),
          check_isotropy_mode(data->anisotropy_second),
          check_isotropy_mode(data->anisotropy_third),
          check_isotropy_mode(data->anisotropy_fourth) };

  /*
  fprintf(stdout, "type : %d ; %d ; %d ; %d \n",
                  isotropy_type[0], isotropy_type[1], isotropy_type[2], isotropy_type[3]);
  */

  float regularization = powf(10.f, data->regularization) - 1.f;
  float variance_threshold = powf(10.f, data->variance_threshold);


  // À trous wavelet decompose
  // there is a paper from a guy we know that explains it : https://jo.dreggn.org/home/2010_atrous.pdf
  // the wavelets decomposition here is the same as the equalizer/atrous module,
  cl_mem residual;
  for(int s = 0; s < scales; ++s)
  {
    const int mult = 1 << s;

    cl_mem buffer_in;
    cl_mem buffer_out;

    if(s == 0)
    {
      buffer_in = in;
      buffer_out = LF_odd;
    }
    else if(s % 2 != 0)
    {
      buffer_in = LF_odd;
      buffer_out = LF_even;
    }
    else
    {
      buffer_in = LF_even;
      buffer_out = LF_odd;
    }

    // Compute wavelets low-frequency scales
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal, 0, sizeof(cl_mem), (void *)&buffer_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal, 1, sizeof(cl_mem), (void *)&HF[s]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal, 4, sizeof(int), (void *)&mult);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_bspline_horizontal, sizes);
    if(err != CL_SUCCESS) return err;

    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical, 0, sizeof(cl_mem), (void *)&HF[s]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical, 1, sizeof(cl_mem), (void *)&buffer_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical, 4, sizeof(int), (void *)&mult);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_bspline_vertical, sizes);
    if(err != CL_SUCCESS) return err;

    // Compute wavelets high-frequency scales and backup the maximum of texture over the RGB channels
    // Note : HF = detail - LF
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_wavelets_detail, 0, sizeof(cl_mem), (void *)&buffer_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_wavelets_detail, 1, sizeof(cl_mem), (void *)&buffer_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_wavelets_detail, 2, sizeof(cl_mem), (void *)&HF[s]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_wavelets_detail, 3, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_wavelets_detail, 4, sizeof(int), (void *)&height);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_wavelets_detail, sizes);
    if(err != CL_SUCCESS) return err;

    residual = buffer_out;
  }

  // will store the temp buffer NOT containing the last step of blur
  cl_mem temp = (residual == LF_even) ? LF_odd : LF_even;

  int count = 0;
  for(int s = scales - 1; s > -1; --s)
  {
    const int mult = 1 << s;
    const float current_radius = equivalent_sigma_at_step(B_SPLINE_SIGMA, s);
    const float real_radius = current_radius * zoom;
    const float current_radius_square = sqf(current_radius);

    const float norm = expf(-sqf(real_radius - (float)data->radius_center) / sqf(data->radius));
    const dt_aligned_pixel_t ABCD = { data->first * KAPPA * norm, data->second * KAPPA * norm,
                                      data->third * KAPPA * norm, data->fourth * KAPPA * norm };
    const float strength = data->sharpness * norm + 1.f;

    cl_mem buffer_in;
    cl_mem buffer_out;

    if(count == 0)
    {
      buffer_in = residual;
      buffer_out = temp;
    }
    else if(count % 2 != 0)
    {
      buffer_in = temp;
      buffer_out = residual;
    }
    else
    {
      buffer_in = residual;
      buffer_out = temp;
    }

    if(s == 0) buffer_out = reconstructed;

    // Compute wavelets low-frequency scales
    dt_opencl_set_kernel_arg(devid, gd->kernel_diffuse_pde, 0, sizeof(cl_mem), (void *)&HF[s]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_diffuse_pde, 1, sizeof(cl_mem), (void *)&buffer_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_diffuse_pde, 2, sizeof(cl_mem), (void *)&mask);
    dt_opencl_set_kernel_arg(devid, gd->kernel_diffuse_pde, 3, sizeof(int), (void *)&has_mask);
    dt_opencl_set_kernel_arg(devid, gd->kernel_diffuse_pde, 4, sizeof(cl_mem), (void *)&buffer_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_diffuse_pde, 5, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_diffuse_pde, 6, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_diffuse_pde, 7, 4 * sizeof(float), (void *)&anisotropy);
    dt_opencl_set_kernel_arg(devid, gd->kernel_diffuse_pde, 8, 4 * sizeof(dt_isotropy_t), (void *)&isotropy_type);
    dt_opencl_set_kernel_arg(devid, gd->kernel_diffuse_pde, 9, sizeof(float), (void *)&regularization);
    dt_opencl_set_kernel_arg(devid, gd->kernel_diffuse_pde, 10, sizeof(float), (void *)&variance_threshold);
    dt_opencl_set_kernel_arg(devid, gd->kernel_diffuse_pde, 11, sizeof(float), (void *)&current_radius_square);
    dt_opencl_set_kernel_arg(devid, gd->kernel_diffuse_pde, 12, sizeof(int), (void *)&mult);
    dt_opencl_set_kernel_arg(devid, gd->kernel_diffuse_pde, 13, 4 * sizeof(float), (void *)&ABCD);
    dt_opencl_set_kernel_arg(devid, gd->kernel_diffuse_pde, 14, sizeof(float), (void *)&strength);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_diffuse_pde, sizes);
    if(err != CL_SUCCESS) return err;

    count++;
  }

  return err;
}

int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const gboolean fastmode = (piece->pipe->type & DT_DEV_PIXELPIPE_FAST) == DT_DEV_PIXELPIPE_FAST;

  const dt_iop_diffuse_data_t *const data = (dt_iop_diffuse_data_t *)piece->data;
  dt_iop_diffuse_global_data_t *const gd = (dt_iop_diffuse_global_data_t *)self->global_data;

  int out_of_memory = FALSE;

  cl_int err = -999;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  // allow fast mode, just copy input to ouput
  if(fastmode)
  {
    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { width, height, 1 };
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, region);
    return err == CL_SUCCESS;
  }

  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };

  cl_mem in = dev_in;

  cl_mem temp1 = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);
  cl_mem temp2 = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);

  cl_mem temp_in = NULL;
  cl_mem temp_out = NULL;

  cl_mem mask = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(uint8_t));

  const float scale = fmaxf(piece->iscale / roi_in->scale, 1.f);
  const float final_radius = (data->radius + data->radius_center) * 2.f / scale;

  const int iterations = MAX(ceilf((float)data->iterations), 1);
  const int diffusion_scales = num_steps_to_reach_equivalent_sigma(B_SPLINE_SIGMA, final_radius);
  const int scales = CLAMP(diffusion_scales, 1, MAX_NUM_SCALES);

  self->cache_next_important = ((data->iterations * (data->radius + data->radius_center)) > 16);

  // wavelets scales buffers
  cl_mem HF[MAX_NUM_SCALES];
  for(int s = 0; s < scales; s++)
  {
    HF[s] = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);
    if(!HF[s]) out_of_memory = TRUE;
  }

  // temp buffer for blurs. We will need to cycle between them for memory efficiency
  cl_mem LF_even = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);
  cl_mem LF_odd = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);

  // PAUSE !
  // check that all buffers exist before processing,
  // because we use a lot of memory here.
  if(!temp1 || !temp2 || !LF_odd || !LF_even || out_of_memory)
  {
    dt_control_log(_("diffuse/sharpen failed to allocate memory, check your RAM settings"));
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto error;
  }

  const int has_mask = (data->threshold > 0.f);

  if(has_mask)
  {
    // build a boolean mask, TRUE where image is above threshold, FALSE otherwise
    dt_opencl_set_kernel_arg(devid, gd->kernel_diffuse_build_mask, 0, sizeof(cl_mem), (void *)&in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_diffuse_build_mask, 1, sizeof(cl_mem), (void *)&mask);
    dt_opencl_set_kernel_arg(devid, gd->kernel_diffuse_build_mask, 2, sizeof(float), (void *)&data->threshold);
    dt_opencl_set_kernel_arg(devid, gd->kernel_diffuse_build_mask, 3, sizeof(int), (void *)&roi_out->width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_diffuse_build_mask, 4, sizeof(int), (void *)&roi_out->height);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_diffuse_build_mask, sizes);
    if(err != CL_SUCCESS) goto error;

    // init the inpainting area with noise
    dt_opencl_set_kernel_arg(devid, gd->kernel_diffuse_inpaint_mask, 0, sizeof(cl_mem), (void *)&temp1);
    dt_opencl_set_kernel_arg(devid, gd->kernel_diffuse_inpaint_mask, 1, sizeof(cl_mem), (void *)&in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_diffuse_inpaint_mask, 2, sizeof(cl_mem), (void *)&mask);
    dt_opencl_set_kernel_arg(devid, gd->kernel_diffuse_inpaint_mask, 3, sizeof(int), (void *)&roi_out->width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_diffuse_inpaint_mask, 4, sizeof(int), (void *)&roi_out->height);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_diffuse_inpaint_mask, sizes);
    if(err != CL_SUCCESS) goto error;

    in = temp1;
  }

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

    if(it == (int)iterations - 1) temp_out = dev_out;
    err = wavelets_process_cl(devid, temp_in, temp_out, mask, sizes, width, height, data, gd, final_radius, scale, scales, has_mask, HF, LF_odd, LF_even);
    if(err != CL_SUCCESS) goto error;
  }

  // cleanup and exit on success
  dt_opencl_release_mem_object(mask);
  dt_opencl_release_mem_object(temp1);
  dt_opencl_release_mem_object(temp2);
  dt_opencl_release_mem_object(LF_even);
  dt_opencl_release_mem_object(LF_odd);
  for(int s = 0; s < scales; s++) dt_opencl_release_mem_object(HF[s]);
  return TRUE;

error:
  if(temp1) dt_opencl_release_mem_object(temp1);
  if(temp2) dt_opencl_release_mem_object(temp2);
  if(mask) dt_opencl_release_mem_object(mask);
  if(LF_even) dt_opencl_release_mem_object(LF_even);
  if(LF_odd) dt_opencl_release_mem_object(LF_odd);
  for(int s = 0; s < scales; s++) if(HF[s]) dt_opencl_release_mem_object(HF[s]);

  dt_print(DT_DEBUG_OPENCL, "[opencl_diffuse] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 33; // extended.cl in programs.conf
  dt_iop_diffuse_global_data_t *gd = (dt_iop_diffuse_global_data_t *)malloc(sizeof(dt_iop_diffuse_global_data_t));

  module->data = gd;
  gd->kernel_diffuse_build_mask = dt_opencl_create_kernel(program, "build_mask");
  gd->kernel_diffuse_inpaint_mask = dt_opencl_create_kernel(program, "inpaint_mask");
  gd->kernel_diffuse_pde = dt_opencl_create_kernel(program, "diffuse_pde");

  const int wavelets = 35; // bspline.cl, from programs.conf
  gd->kernel_filmic_bspline_horizontal = dt_opencl_create_kernel(wavelets, "blur_2D_Bspline_horizontal");
  gd->kernel_filmic_bspline_vertical = dt_opencl_create_kernel(wavelets, "blur_2D_Bspline_vertical");
  gd->kernel_filmic_wavelets_detail = dt_opencl_create_kernel(wavelets, "wavelets_detail_level");
}


void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_diffuse_global_data_t *gd = (dt_iop_diffuse_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_diffuse_build_mask);
  dt_opencl_free_kernel(gd->kernel_diffuse_inpaint_mask);
  dt_opencl_free_kernel(gd->kernel_diffuse_pde);

  dt_opencl_free_kernel(gd->kernel_filmic_bspline_vertical);
  dt_opencl_free_kernel(gd->kernel_filmic_bspline_horizontal);
  dt_opencl_free_kernel(gd->kernel_filmic_wavelets_detail);
  free(module->data);
  module->data = NULL;
}
#endif


void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_diffuse_gui_data_t *g = IOP_GUI_ALLOC(diffuse);
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("properties")), FALSE, FALSE, 0);

  g->iterations = dt_bauhaus_slider_from_params(self, "iterations");
  dt_bauhaus_slider_set_soft_range(g->iterations, 1., 128);
  gtk_widget_set_tooltip_text(g->iterations,
                              _("more iterations make the effect stronger but the module slower.\n"
                                "this is analogous to giving more time to the diffusion reaction.\n"
                                "if you plan on sharpening or inpainting, \n"
                                "more iterations help reconstruction."));

  g->radius_center = dt_bauhaus_slider_from_params(self, "radius_center");
  dt_bauhaus_slider_set_soft_range(g->radius_center, 0., 512.);
  dt_bauhaus_slider_set_format(g->radius_center, " px");
  gtk_widget_set_tooltip_text(
      g->radius_center, _("main scale of the diffusion.\n"
                          "zero makes diffusion act on the finest details more heavily.\n"
                          "non-zero defines the size of the details to diffuse heavily.\n"
                          "for deblurring and denoising, set to zero.\n"
                          "increase to act on local contrast instead."));

  g->radius = dt_bauhaus_slider_from_params(self, "radius");
  dt_bauhaus_slider_set_soft_range(g->radius, 1., 512.);
  dt_bauhaus_slider_set_format(g->radius, " px");
  gtk_widget_set_tooltip_text(
      g->radius, _("width of the diffusion around the central radius.\n"
                   "high values diffuse on a large band of radii.\n"
                   "low values diffuse closer to the central radius.\n"
                   "if you plan on deblurring, \n"
                   "the radius should be around the width of your lens blur."));

  GtkWidget *label_speed = dt_ui_section_label_new(_("speed (sharpen ↔ diffuse)"));
  gtk_box_pack_start(GTK_BOX(self->widget), label_speed, FALSE, FALSE, 0);

  g->first = dt_bauhaus_slider_from_params(self, "first");
  dt_bauhaus_slider_set_digits(g->first, 4);
  dt_bauhaus_slider_set_format(g->first, "%");
  gtk_widget_set_tooltip_text(g->first, _("diffusion speed of low-frequency wavelet layers\n"
                  "in the direction of 1st order anisotropy (set below).\n\n"
                  "negative values sharpen, \n"
                  "positive values diffuse and blur, \n"
                  "zero does nothing."));

  g->second = dt_bauhaus_slider_from_params(self, "second");
  dt_bauhaus_slider_set_digits(g->second, 4);
  dt_bauhaus_slider_set_format(g->second, "%");
  gtk_widget_set_tooltip_text(g->second, _("diffusion speed of low-frequency wavelet layers\n"
                  "in the direction of 2nd order anisotropy (set below).\n\n"
                  "negative values sharpen, \n"
                  "positive values diffuse and blur, \n"
                  "zero does nothing."));

  g->third = dt_bauhaus_slider_from_params(self, "third");
  dt_bauhaus_slider_set_digits(g->third, 4);
  dt_bauhaus_slider_set_format(g->third, "%");
  gtk_widget_set_tooltip_text(g->third, _("diffusion speed of high-frequency wavelet layers\n"
                  "in the direction of 3rd order anisotropy (set below).\n\n"
                  "negative values sharpen, \n"
                  "positive values diffuse and blur, \n"
                  "zero does nothing."));

  g->fourth = dt_bauhaus_slider_from_params(self, "fourth");
  dt_bauhaus_slider_set_digits(g->fourth, 4);
  dt_bauhaus_slider_set_format(g->fourth, "%");
  gtk_widget_set_tooltip_text(g->fourth, _("diffusion speed of high-frequency wavelet layers\n"
                  "in the direction of 4th order anisotropy (set below).\n\n"
                  "negative values sharpen, \n"
                  "positive values diffuse and blur, \n"
                  "zero does nothing."));

  GtkWidget *label_direction = dt_ui_section_label_new(_("direction"));
  gtk_box_pack_start(GTK_BOX(self->widget), label_direction, FALSE, FALSE, 0);

  g->anisotropy_first = dt_bauhaus_slider_from_params(self, "anisotropy_first");
  dt_bauhaus_slider_set_digits(g->anisotropy_first, 4);
  dt_bauhaus_slider_set_format(g->anisotropy_first, "%");
  gtk_widget_set_tooltip_text(g->anisotropy_first, _("direction of 1st order speed (set above).\n\n"
                  "negative values follow gradients more closely, \n"
                  "positive values rather avoid edges (isophotes), \n"
                  "zero affects both equally (isotropic)."));

  g->anisotropy_second = dt_bauhaus_slider_from_params(self, "anisotropy_second");
  dt_bauhaus_slider_set_digits(g->anisotropy_second, 4);
  dt_bauhaus_slider_set_format(g->anisotropy_second, "%");
  gtk_widget_set_tooltip_text(g->anisotropy_second,_("direction of 2nd order speed (set above).\n\n"
                  "negative values follow gradients more closely, \n"
                  "positive values rather avoid edges (isophotes), \n"
                  "zero affects both equally (isotropic)."));

  g->anisotropy_third = dt_bauhaus_slider_from_params(self, "anisotropy_third");
  dt_bauhaus_slider_set_digits(g->anisotropy_third, 4);
  dt_bauhaus_slider_set_format(g->anisotropy_third, "%");
  gtk_widget_set_tooltip_text(g->anisotropy_third,_("direction of 3rd order speed (set above).\n\n"
                  "negative values follow gradients more closely, \n"
                  "positive values rather avoid edges (isophotes), \n"
                  "zero affects both equally (isotropic)."));

  g->anisotropy_fourth = dt_bauhaus_slider_from_params(self, "anisotropy_fourth");
  dt_bauhaus_slider_set_digits(g->anisotropy_fourth, 4);
  dt_bauhaus_slider_set_format(g->anisotropy_fourth, "%");
  gtk_widget_set_tooltip_text(g->anisotropy_fourth,_("direction of 4th order speed (set above).\n\n"
                  "negative values follow gradients more closely, \n"
                  "positive values rather avoid edges (isophotes), \n"
                  "zero affects both equally (isotropic)."));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("edge management")), FALSE, FALSE, 0);

  g->sharpness = dt_bauhaus_slider_from_params(self, "sharpness");
  dt_bauhaus_slider_set_format(g->sharpness, "%");
  gtk_widget_set_tooltip_text(g->sharpness,
                              _("increase or decrease the sharpness of the highest frequencies.\n"
                              "can be used to keep details after blooming,\n"
                              "for standalone sharpening set speed to negative values."));

  g->regularization = dt_bauhaus_slider_from_params(self, "regularization");
  gtk_widget_set_tooltip_text(g->regularization,
                              _("define the sensitivity of the variance penalty for edges.\n"
                                "increase to exclude more edges from diffusion,\n"
                                "if fringes or halos appear."));

  g->variance_threshold = dt_bauhaus_slider_from_params(self, "variance_threshold");
  gtk_widget_set_tooltip_text(g->variance_threshold,
                              _("define the variance threshold between edge amplification and penalty.\n"
                                "decrease if you want pixels on smooth surfaces get a boost,\n"
                                "increase if you see noise appear on smooth surfaces or\n"
                                "if dark areas seem oversharpened compared to bright areas."));


  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("diffusion spatiality")), FALSE, FALSE, 0);

  g->threshold = dt_bauhaus_slider_from_params(self, "threshold");
  dt_bauhaus_slider_set_format(g->threshold, "%");
  gtk_widget_set_tooltip_text(g->threshold,
                              _("luminance threshold for the mask.\n"
                                "0. disables the luminance masking and applies the module on the whole image.\n"
                                "any higher value excludes pixels with luminance lower than the threshold.\n"
                                "this can be used to inpaint highlights."));
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
