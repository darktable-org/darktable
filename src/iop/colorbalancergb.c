/*
    This file is part of darktable,
    Copyright (C) 2020 darktable developers.

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
// our includes go first:
#include "bauhaus/bauhaus.h"
#include "common/exif.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/opencl.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "dtgtk/gradientslider.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "gui/color_picker_proxy.h"
#include "iop/iop_api.h"

//#include <gtk/gtk.h>
#include <stdlib.h>
#define LUT_ELEM 360 // gamut LUT number of elements: resolution of 1°
#define STEPS 72     // so we test 72×72×72 combinations of RGB in [0; 1] to build the gamut LUT

// Filmlight Yrg puts red at 330°, while usual HSL wheels put it at 360/0°
// so shift in GUI only it to not confuse people. User params are always degrees,
// pixel params are always radians.
#define ANGLE_SHIFT -30.f
#define DEG_TO_RAD(x) ((x + ANGLE_SHIFT) * M_PI / 180.f)
#define RAD_TO_DEG(x) (x * 180.f / M_PI - ANGLE_SHIFT)

DT_MODULE_INTROSPECTION(2, dt_iop_colorbalancergb_params_t)


typedef struct dt_iop_colorbalancergb_params_t
{
  /* params of v1 */
  float shadows_Y;             // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "luminance"
  float shadows_C;             // $MIN:  0.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma"
  float shadows_H;             // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
  float midtones_Y;            // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "luminance"
  float midtones_C;            // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma"
  float midtones_H;            // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
  float highlights_Y;          // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "luminance"
  float highlights_C;          // $MIN:  0.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma"
  float highlights_H;          // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
  float global_Y;              // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "luminance"
  float global_C;              // $MIN:  0.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma"
  float global_H;              // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
  float shadows_weight;        // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "shadows fall-off"
  float midtones_weight;       // $MIN: -6.0 $MAX:   6.0 $DEFAULT: 0.0 $DESCRIPTION: "white pivot"
  float highlights_weight;     // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "highlights fall-off"
  float chroma_shadows;        // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "shadows"
  float chroma_highlights;     // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "highlights"
  float chroma_global;         // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "global"
  float chroma_midtones;       // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "midtones"
  float saturation_global;     // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "global"
  float saturation_highlights; // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "highlights"
  float saturation_midtones;   // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "midtones"
  float saturation_shadows;    // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "shadows"
  float hue_angle;             // $MIN: -180. $MAX: 180. $DEFAULT: 0.0 $DESCRIPTION: "hue shift"

  /* params of v2 */
  float purity_global;     // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "global"
  float purity_highlights; // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "highlights"
  float purity_midtones;   // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "midtones"
  float purity_shadows;    // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "shadows"

  /* add future params after this so the legacy params import can use a blind memcpy */

} dt_iop_colorbalancergb_params_t;


typedef struct dt_iop_colorbalancergb_gui_data_t
{
  GtkWidget *shadows_H, *midtones_H, *highlights_H, *global_H;
  GtkWidget *shadows_C, *midtones_C, *highlights_C, *global_C;
  GtkWidget *shadows_Y, *midtones_Y, *highlights_Y, *global_Y;
  GtkWidget *shadows_weight, *midtones_weight, *highlights_weight;
  GtkWidget *chroma_highlights, *chroma_global, *chroma_shadows, *chroma_midtones;
  GtkWidget *saturation_global, *saturation_highlights, *saturation_midtones, *saturation_shadows;
  GtkWidget *purity_global, *purity_highlights, *purity_midtones, *purity_shadows;
  GtkWidget *hue_angle;
  GtkNotebook *notebook;
} dt_iop_colorbalancergb_gui_data_t;

typedef struct dt_iop_colorbalancergb_data_t
{
  float global[4];
  float shadows[4];
  float highlights[4];
  float midtones[4];
  float midtones_Y;
  float chroma_global, chroma[4];
  float saturation_global, saturation[4];
  float purity_global, purity[4];
  float hue_angle;
  float shadows_weight, midtones_weight, highlights_weight;
  float *gamut_LUT;
  float max_chroma;
  gboolean lut_inited;
  struct dt_iop_order_iccprofile_info_t *work_profile;
} dt_iop_colorbalancergb_data_t;

const char *name()
{
  return _("color balance rgb");
}

const char *aliases()
{
  return _("offset power slope|cdl|color grading|contrast|chroma_highlights|hue");
}

const char *deprecated_msg()
{
  return _("this module is experimental and the internal algo may slightly change before darktable 3.6. don't use it for serious work yet.");
}

const char *description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("affect color, brightness and contrast"),
                                      _("corrective or creative"),
                                      _("linear, Lab, scene-referred"),
                                      _("non-linear, RGB"),
                                      _("non-linear, Lab, scene-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_group()
{
  return IOP_GROUP_COLOR | IOP_GROUP_GRADING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_rgb;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params,
                  const int new_version)
{
  if(old_version == 1 && new_version == 2)
  {
    typedef struct dt_iop_colorbalancergb_params_v1_t
    {
      float shadows_Y;             // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "luminance"
      float shadows_C;             // $MIN:  0.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma"
      float shadows_H;             // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
      float midtones_Y;            // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "luminance"
      float midtones_C;            // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma"
      float midtones_H;            // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
      float highlights_Y;          // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "luminance"
      float highlights_C;          // $MIN:  0.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma"
      float highlights_H;          // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
      float global_Y;              // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "luminance"
      float global_C;              // $MIN:  0.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma"
      float global_H;              // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
      float shadows_weight;        // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "tonal weight"
      float midtones_weight;       // $MIN: -6.0 $MAX:   6.0 $DEFAULT: 0.0 $DESCRIPTION: "fulcrum"
      float highlights_weight;     // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "tonal weight"
      float chroma_shadows;        // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "shadows"
      float chroma_highlights;     // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "highlights"
      float chroma_global;         // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "global"
      float chroma_midtones;       // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "midtones"
      float saturation_global;     // $MIN: -5.0 $MAX: 5.0 $DEFAULT: 0.0 $DESCRIPTION: "saturation global"
      float saturation_highlights; // $MIN: -0.2 $MAX: 0.2 $DEFAULT: 0.0 $DESCRIPTION: "highlights"
      float saturation_midtones;   // $MIN: -0.2 $MAX: 0.2 $DEFAULT: 0.0 $DESCRIPTION: "midtones"
      float saturation_shadows;    // $MIN: -0.2 $MAX: 0.2 $DEFAULT: 0.0 $DESCRIPTION: "shadows"
      float hue_angle;             // $MIN: -180. $MAX: 180. $DEFAULT: 0.0 $DESCRIPTION: "hue shift"
    } dt_iop_colorbalancergb_params_v1_t;

    // Init params with defaults
    memcpy(new_params, self->default_params, sizeof(dt_iop_colorbalancergb_params_t));

    // Copy the common part of the params struct
    memcpy(new_params, old_params, sizeof(dt_iop_colorbalancergb_params_v1_t));

    dt_iop_colorbalancergb_params_t *n = (dt_iop_colorbalancergb_params_t *)new_params;
    n->saturation_global /= 180.f / M_PI;

    return 0;
  }
  return 1;
}


/* Custom matrix handling for speed */
static inline void repack_3x3_to_3xSSE(const float input[9], float output[3][4])
{
  // Repack a 3×3 array/matrice into a 3×1 SSE2 vector to enable SSE4/AVX/AVX2 dot products
  output[0][0] = input[0];
  output[0][1] = input[1];
  output[0][2] = input[2];
  output[0][3] = 0.0f;

  output[1][0] = input[3];
  output[1][1] = input[4];
  output[1][2] = input[5];
  output[1][3] = 0.0f;

  output[2][0] = input[6];
  output[2][1] = input[7];
  output[2][2] = input[8];
  output[2][3] = 0.0f;
}


static void mat3mul4(float *dst, const float *const m1, const float *const m2)
{
  for(int k = 0; k < 3; ++k)
  {
    for(int i = 0; i < 3; ++i)
    {
      float x = 0.0f;
      for(int j = 0; j < 3; j++) x += m1[4 * k + j] * m2[4 * j + i];
      dst[4 * k + i] = x;
    }
  }
}


void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_colorbalancergb_data_t *d = (dt_iop_colorbalancergb_data_t *)piece->data;
  const struct dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  if(work_profile == NULL) return; // no point

  float DT_ALIGNED_ARRAY RGB_to_XYZ[3][4];
  float DT_ALIGNED_ARRAY XYZ_to_RGB[3][4];

  // repack the matrices as flat AVX2-compliant matrice
  if(work_profile)
  {
    // work profile can't be fetched in commit_params since it is not yet initialised
    repack_3x3_to_3xSSE(work_profile->matrix_in, RGB_to_XYZ);
    repack_3x3_to_3xSSE(work_profile->matrix_out, XYZ_to_RGB);
  }

  // Matrices from CIE 1931 2° XYZ D50 to Filmlight grading RGB D65 through CIE 2006 LMS
  const float XYZ_to_gradRGB[3][4] = { { 0.53346004f,  0.15226970f , -0.19946283f, 0.f },
                                       {-0.67012691f,  1.91752954f,   0.39223917f, 0.f },
                                       { 0.06557547f, -0.07983082f,   0.75036927f, 0.f } };
  const float gradRGB_to_XYZ[3][4] = { { 1.67222161f, -0.11185000f,  0.50297636f, 0.f },
                                       { 0.60120746f,  0.47018395f, -0.08596569f, 0.f },
                                       {-0.08217531f,  0.05979694f,  1.27957582f, 0.f } };

  // Premultiply the pipe RGB -> XYZ and XYZ -> grading RGB matrices to spare 2 matrix products per pixel
  float DT_ALIGNED_ARRAY input_matrix[3][4];
  float DT_ALIGNED_ARRAY output_matrix[3][4];
  mat3mul4((float *)input_matrix, (float *)XYZ_to_gradRGB, (float *)RGB_to_XYZ);
  mat3mul4((float *)output_matrix, (float *)XYZ_to_RGB, (float *)gradRGB_to_XYZ);

  // Test white point of the current space in grading RGB
  const float white_pipe_RGB[4] = { 1.f, 1.f, 1.f };
  float white_grading_RGB[4] = { 0.f };
  dot_product(white_pipe_RGB, input_matrix, white_grading_RGB);
  const float sum_white = white_grading_RGB[0] + white_grading_RGB[1] + white_grading_RGB[2];
  for_four_channels(c) white_grading_RGB[c] /= sum_white;

  const float *const restrict in = __builtin_assume_aligned(((const float *const restrict)ivoid), 64);
  float *const restrict out = __builtin_assume_aligned(((float *const restrict)ovoid), 64);
  const float *const restrict gamut_LUT = __builtin_assume_aligned(((const float *const restrict)d->gamut_LUT), 64);

  const float *const restrict global = __builtin_assume_aligned((const float *const restrict)d->global, 16);
  const float *const restrict highlights = __builtin_assume_aligned((const float *const restrict)d->highlights, 16);
  const float *const restrict shadows = __builtin_assume_aligned((const float *const restrict)d->shadows, 16);
  const float *const restrict midtones = __builtin_assume_aligned((const float *const restrict)d->midtones, 16);

  const float *const restrict chroma = __builtin_assume_aligned((const float *const restrict)d->chroma, 16);
  const float *const restrict saturation = __builtin_assume_aligned((const float *const restrict)d->saturation, 16);
  const float *const restrict purity = __builtin_assume_aligned((const float *const restrict)d->purity, 16);

#ifdef _OPENMP
#pragma omp parallel for simd default(none) aligned(in, out, gamut_LUT: 64) aligned(global, highlights, shadows, midtones, chroma, saturation, purity:16)\
      dt_omp_firstprivate(in, out, roi_in, roi_out, d, input_matrix, output_matrix, gamut_LUT, white_grading_RGB, \
        global, highlights, shadows, midtones, chroma, saturation, purity) schedule(static)
#endif
  for(size_t k = 0; k < (size_t)4 * roi_in->width * roi_out->height; k += 4)
  {
    const float *const restrict pix_in = __builtin_assume_aligned(in + k, 16);
    float *const restrict pix_out = __builtin_assume_aligned(out + k, 16);

    float DT_ALIGNED_PIXEL Ych[4] = { 0.f };
    float DT_ALIGNED_PIXEL RGB[4] = { 0.f };

    for(size_t c = 0; c < 4; ++c) Ych[c] = fmaxf(pix_in[c], 0.0f);
    dot_product(Ych, input_matrix, RGB);
    gradingRGB_to_Ych(RGB, Ych, white_grading_RGB);

    // Sanitize input : no negative luminance
    float Y = fmaxf(Ych[0], 0.f);
    const int is_black = (Y == 0.f);

    // Opacities for luma masks
    const float x_offset = (Y - 0.1845f) / 0.1845f;

    const float alpha = 1.f / (1.f + expf(x_offset * d->shadows_weight));         // opacity of shadows
    const float gamma = expf(-0.1845f * x_offset * x_offset / (d->shadows_weight * d->highlights_weight));
    const float beta = 1.f / (1.f + expf(- x_offset * d->highlights_weight));     // opacity of highlights
    const float DT_ALIGNED_PIXEL opacities[4] = { alpha, gamma, beta, 0.f };
    const float alpha_comp = 1.f - alpha;
    const float beta_comp = 1.f - beta;

    // Hue shift - do it now because we need the gamut limit at output hue right after
    Ych[2] += d->hue_angle;

    // Get max allowed chroma in working RGB gamut at current output hue
    const float max_chroma_h = (is_black) ? 0.f : gamut_LUT[CLAMP((size_t)(LUT_ELEM / 2. * (Ych[2] + M_PI) / M_PI), 0, LUT_ELEM - 1)];
    float C = (is_black) ? 0.f : fminf(Ych[1], max_chroma_h);

    // Linear chroma : distance to achromatic at constant luminance in scene-referred
    // - in case we desaturate, we do so by a constant factor
    // - in case we resaturate, we normalize the correction by the max chroma allowed at current hue
    //   to prevent users from pushing saturated colors outside of gamut while the low-sat ones
    //   are still muted.
    const float chroma_boost = d->chroma_global + scalar_product(opacities, chroma);
    const float chroma_norm = (chroma_boost > 0.f) ? max_chroma_h / d->max_chroma : 1.f;
    const float chroma_factor = fmaxf(1.f + chroma_boost * chroma_norm, 0.f);
    Ych[1] = fminf(C * chroma_factor, max_chroma_h);
    Ych_to_gradingRGB(Ych, RGB, white_grading_RGB);

    /* Color balance */
    for(size_t c = 0; c < 4; ++c)
    {
      // global : offset
      RGB[c] = RGB[c] + global[c];

      //  highlights, shadows : 2 slopes with masking
      RGB[c] *= beta_comp * (alpha_comp + alpha * shadows[c]) + beta * highlights[c];
      // factorization of : (RGB[c] * (1.f - alpha) + RGB[c] * d->shadows[c] * alpha) * (1.f - beta)  + RGB[c] * d->highlights[c] * beta;

      // midtones : power with sign preservation
      const float sign = (RGB[c] < 0.f) ? -1.f : 1.f;
      RGB[c] = sign * powf(fabsf(RGB[c]) / d->midtones_weight, midtones[c]) * d->midtones_weight;
    }

    // for the Y midtones power (gamma), we need to go in Ych again because RGB doesn't preserve color
    gradingRGB_to_Ych(RGB, Ych, white_grading_RGB);
    Y = Ych[0] = powf(fmaxf(Ych[0] / d->midtones_weight, 0.f), d->midtones_Y) * d->midtones_weight;
    Ych_to_gradingRGB(Ych, RGB, white_grading_RGB);

    /* Perceptual color adjustments */

     // grading RGB to CIE 1931 XYZ 2° D65
    const float RGB_to_XYZ_D65[3][4] = { { 1.64004888f, -0.10969806f, 0.49329934f, 0.f },
                                         { 0.61055787f, 0.47749658f, -0.08730269f, 0.f },
                                         { -0.10698534f, 0.07785058f, 1.66590006f, 0.f } };

    const float XYZ_to_RGB_D65[3][4] = { { 0.54392489f, 0.14993776f, -0.15320716f, 0.f },
                                         { -0.68327274f, 1.88816348f, 0.30127843f, 0.f },
                                         { 0.06686186f, -0.07860825f, 0.57635773f, 0.f } };

    // Go to JzAzBz for perceptual saturation
    // We can't use gradingRGB_to_XYZ() since it also does chromatic adaptation to D50
    // and JzAzBz uses D65, same as grading RGB. So we use the matrices above instead
    float DT_ALIGNED_PIXEL Jab[4] = { 0.f };
    dot_product(RGB, RGB_to_XYZ_D65, Ych);
    dt_XYZ_2_JzAzBz(Ych, Jab);

    // Convert to JCh
    float JC[2] = { Jab[0], hypotf(Jab[1], Jab[2]) };               // brightness/chroma vector
    const float h = (JC[1] == 0.f) ? 0.f : atan2f(Jab[2], Jab[1]);  // hue : (a, b) angle

    // Project JC to S, the saturation eigenvector, with orthogonal vector O.
    // Note : O should be = (C * cosf(T) - J * sinf(T)) = 0 since S is the eigenvector,
    // so we add the chroma projected along the orthogonal axis to get some control value
    const float T = atan2f(JC[1], JC[0]); // angle of the eigenvector over the hue plane
    const float sin_T = sinf(T);
    const float cos_T = cosf(T);
    const float DT_ALIGNED_PIXEL M_rot_dir[2][2] = { {  cos_T,  sin_T },
                                                     { -sin_T,  cos_T } };
    const float DT_ALIGNED_PIXEL M_rot_inv[2][2] = { {  cos_T, -sin_T },
                                                     {  sin_T,  cos_T } };
    float SO[2];

    // Purity & Saturation : mix of chroma and luminance
    const float boosts[2] = { 1.f + d->purity_global + scalar_product(opacities, purity),     // move in S direction
                              d->saturation_global + scalar_product(opacities, saturation) }; // move in O direction

    SO[0] = fmaxf(JC[0] * M_rot_dir[0][0] + JC[1] * M_rot_dir[0][1] * boosts[0], 0.f);
    SO[1] = JC[0] * fminf(fmaxf(T * boosts[1], -T), DT_M_PI_F / 2.f - T);

    // Project back to JCh, that is rotate back of -T angle
    JC[0] = fmaxf(SO[0] * M_rot_inv[0][0] + SO[1] * M_rot_inv[0][1], 0.f);
    JC[1] = fmaxf(SO[0] * M_rot_inv[1][0] + SO[1] * M_rot_inv[1][1], 0.f);

    // Project back to JzAzBz
    Jab[0] = JC[0];
    Jab[1] = JC[1] * cosf(h);
    Jab[2] = JC[1] * sinf(h);

    dt_JzAzBz_2_XYZ(Jab, Ych);
    dot_product(Ych, XYZ_to_RGB_D65, RGB);
    gradingRGB_to_Ych(RGB, Ych, white_grading_RGB);

    /* Gamut mapping */
    const float out_max_chroma_h = gamut_LUT[CLAMP((size_t)(LUT_ELEM / 2. * (Ych[2] + M_PI) / M_PI), 0, LUT_ELEM - 1)];
    Ych[1] = fminf(Ych[1], out_max_chroma_h);

    Ych_to_gradingRGB(Ych, RGB, white_grading_RGB);
    dot_product(RGB, output_matrix, pix_out);
    for(size_t c = 0; c < 4; ++c) pix_out[c] = fmaxf(pix_out[c], 0.f);
  }
}


void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorbalancergb_data_t *d = (dt_iop_colorbalancergb_data_t *)(piece->data);
  dt_iop_colorbalancergb_params_t *p = (dt_iop_colorbalancergb_params_t *)p1;

  d->chroma_global = p->chroma_global;
  d->chroma[0] = p->chroma_shadows;
  d->chroma[1] = p->chroma_midtones;
  d->chroma[2] = p->chroma_highlights;
  d->chroma[3] = 0.f;

  d->saturation_global = p->saturation_global;
  d->saturation[0] = p->saturation_shadows;
  d->saturation[1] = p->saturation_midtones;
  d->saturation[2] = p->saturation_highlights;
  d->saturation[3] = 0.f;

  d->purity_global = p->purity_global;
  d->purity[0] = p->purity_shadows;
  d->purity[1] = p->purity_midtones;
  d->purity[2] = p->purity_highlights;
  d->purity[3] = 0.f;

  d->hue_angle = M_PI * p->hue_angle / 180.f;

  // measure the grading RGB of a pure white
  const float Ych_norm[4] = { 1.f, 0.f, 0.f, 0.f };
  float RGB_norm[4] = { 0.f };
  Ych_to_gradingRGB(Ych_norm, RGB_norm, NULL);

  // global
  {
    float Ych[4] = { 1.f, p->global_C, DEG_TO_RAD(p->global_H), 0.f };
    Ych_to_gradingRGB(Ych, d->global, NULL);
    for(size_t c = 0; c < 4; c++) d->global[c] = (d->global[c] - RGB_norm[c]) + RGB_norm[c] * p->global_Y;
  }

  // shadows
  {
    float Ych[4] = { 1.f, p->shadows_C, DEG_TO_RAD(p->shadows_H), 0.f };
    Ych_to_gradingRGB(Ych, d->shadows, NULL);
    for(size_t c = 0; c < 4; c++) d->shadows[c] = 1.f + (d->shadows[c] - RGB_norm[c]) + p->shadows_Y;
    d->shadows_weight = 2.f + p->shadows_weight * 2.f;
  }

  // highlights
  {
    float Ych[4] = { 1.f, p->highlights_C, DEG_TO_RAD(p->highlights_H), 0.f };
    Ych_to_gradingRGB(Ych, d->highlights, NULL);
    for(size_t c = 0; c < 4; c++) d->highlights[c] = 1.f + (d->highlights[c] - RGB_norm[c]) + p->highlights_Y;
    d->highlights_weight = 2.f + p->highlights_weight * 2.f;
  }

  // midtones
  {
    float Ych[4] = { 1.f, p->midtones_C, DEG_TO_RAD(p->midtones_H), 0.f };
    Ych_to_gradingRGB(Ych, d->midtones, NULL);
    for(size_t c = 0; c < 4; c++) d->midtones[c] = 1.f / (1.f + (d->midtones[c] - RGB_norm[c]));
    d->midtones_Y = 1.f / (1.f + p->midtones_Y);
    d->midtones_weight = exp2f(p->midtones_weight);
  }

  // Check if the RGB working profile has changed in pipe
  struct dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  if(work_profile == NULL) return;
  if(work_profile != d->work_profile)
  {
    d->lut_inited = FALSE;
    d->work_profile = work_profile;
  }

  // find the maximum chroma allowed by the current working gamut in conjunction to hue
  // this will be used to prevent users to mess up their images by pushing chroma out of gamut
  if(!d->lut_inited && d->gamut_LUT)
  {
    float *const restrict LUT = d->gamut_LUT;

    // init the LUT between -pi and pi by increments of 1°
    for(size_t k = 0; k < LUT_ELEM; k++) LUT[k] = 0.f;

    // Premultiply the matrix to speed-up
    float DT_ALIGNED_ARRAY RGB_to_XYZ[3][4];
    repack_3x3_to_3xSSE(work_profile->matrix_in, RGB_to_XYZ);
    const float XYZ_to_gradingRGB[3][4] = { { 0.53346004f,  0.15226970f , -0.19946283f, 0.f },
                                            {-0.67012691f,  1.91752954f,   0.39223917f, 0.f },
                                            { 0.06557547f, -0.07983082f,   0.75036927f, 0.f } };
    float DT_ALIGNED_ARRAY input_matrix[3][4];
    mat3mul4((float *)input_matrix, (float *)XYZ_to_gradingRGB, (float *)RGB_to_XYZ);

    // Test white point of the current space in grading RGB
    const float white_pipe_RGB[4] = { 1.f, 1.f, 1.f };
    float white_grading_RGB[4] = { 0.f };
    dot_product(white_pipe_RGB, input_matrix, white_grading_RGB);
    const float sum_white = white_grading_RGB[0] + white_grading_RGB[1] + white_grading_RGB[2];
    for_four_channels(c) white_grading_RGB[c] /= sum_white;

    // make RGB values vary between [0; 1] in working space, convert to Ych and get the max(c(h)))
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(input_matrix, white_grading_RGB) schedule(static) dt_omp_sharedconst(LUT)
#endif
    for(size_t r = 0; r < STEPS; r++)
      for(size_t g = 0; g < STEPS; g++)
        for(size_t b = 0; b < STEPS; b++)
        {
          const float DT_ALIGNED_PIXEL rgb[4] = { (float)r / (float)(STEPS - 1),
                                                  (float)g / (float)(STEPS - 1),
                                                  (float)b / (float)(STEPS - 1),
                                                  0.f };

          float DT_ALIGNED_PIXEL RGB[4] = { 0.f };
          float DT_ALIGNED_PIXEL Ych[4] = { 0.f };
          dot_product(rgb, input_matrix, RGB);
          gradingRGB_to_Ych(RGB, Ych, white_grading_RGB);
          const size_t index = CLAMP((size_t)(LUT_ELEM / 2. * (Ych[2] + M_PI) / M_PI), 0, LUT_ELEM - 1);
          if(LUT[index] < Ych[1]) LUT[index] = Ych[1];
        }

    d->lut_inited = TRUE;

    d->max_chroma = 0.f;
    for(size_t k = 0; k < LUT_ELEM; k++)
      if(d->max_chroma < LUT[k]) d->max_chroma = LUT[k];
  }
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_colorbalancergb_data_t));
  dt_iop_colorbalancergb_data_t *d = (dt_iop_colorbalancergb_data_t *)(piece->data);
  d->gamut_LUT = NULL;
  d->gamut_LUT = dt_alloc_sse_ps(LUT_ELEM);
  d->lut_inited = FALSE;
  d->work_profile = NULL;
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorbalancergb_data_t *d = (dt_iop_colorbalancergb_data_t *)(piece->data);
  if(d->gamut_LUT) dt_free_align(d->gamut_LUT);
  free(piece->data);
  piece->data = NULL;
}

void pipe_RGB_to_Ych(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const float RGB[4], float Ych[4])
{
  const struct dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  if(work_profile == NULL) return; // no point

  float XYZ[4] = { 0.f };
  float LMS[4] = { 0.f };

  dt_ioppr_rgb_matrix_to_xyz(RGB, XYZ, work_profile->matrix_in, work_profile->lut_in,
                             work_profile->unbounded_coeffs_in, work_profile->lutsize,
                             work_profile->nonlinearlut);
  XYZ_to_gradingRGB(XYZ, LMS);
  gradingRGB_to_Ych(LMS, Ych, NULL);

  if(Ych[2] < 0.f)
    Ych[2] = 2.f * M_PI + Ych[2];
}


void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorbalancergb_gui_data_t *g = (dt_iop_colorbalancergb_gui_data_t *)self->gui_data;
  dt_iop_colorbalancergb_params_t *p = (dt_iop_colorbalancergb_params_t *)self->params;

  float Ych[4] = { 0.f };
  pipe_RGB_to_Ych(self, piece, (const float *)self->picked_color, Ych);
  float hue = RAD_TO_DEG(Ych[2]) + 180.f;   // take the opponent color
  hue = (hue > 360.f) ? hue - 360.f : hue;  // normalize in [0 ; 360]°

  ++darktable.gui->reset;
  if(picker == g->global_H)
  {
    p->global_H = hue;
    p->global_C = Ych[1] * Ych[0];
    dt_bauhaus_slider_set_soft(g->global_H, p->global_H);
    dt_bauhaus_slider_set_soft(g->global_C, p->global_C);
  }
  else if(picker == g->shadows_H)
  {
    p->shadows_H = hue;
    p->shadows_C = Ych[1] * Ych[0];
    dt_bauhaus_slider_set_soft(g->shadows_H, p->shadows_H);
    dt_bauhaus_slider_set_soft(g->shadows_C, p->shadows_C);
  }
  else if(picker == g->midtones_H)
  {
    p->midtones_H = hue;
    p->midtones_C = Ych[1] * Ych[0];
    dt_bauhaus_slider_set_soft(g->midtones_H, p->midtones_H);
    dt_bauhaus_slider_set_soft(g->midtones_C, p->midtones_C);
  }
  else if(picker == g->highlights_H)
  {
    p->highlights_H = hue;
    p->highlights_C = Ych[1] * Ych[0];
    dt_bauhaus_slider_set_soft(g->highlights_H, p->highlights_H);
    dt_bauhaus_slider_set_soft(g->highlights_C, p->highlights_C);
  }
  else
    fprintf(stderr, "[colorbalancergb] unknown color picker\n");
  --darktable.gui->reset;

  gui_changed(self, picker, NULL);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void paint_chroma_slider(GtkWidget *w, const float hue)
{
  const float x_min = DT_BAUHAUS_WIDGET(w)->data.slider.soft_min;
  const float x_max = DT_BAUHAUS_WIDGET(w)->data.slider.soft_max;
  const float x_range = x_max - x_min;

  // Varies x in range around current y param
  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    const float x = x_min + stop * x_range;
    const float h = DEG_TO_RAD(hue);

    float RGB[4] = { 0.f };
    float Ych[4] = { 0.75f, x, h, 0.f };
    float LMS[4] = { 0.f };
    Ych_to_gradingRGB(Ych, LMS, NULL);
    gradingRGB_to_XYZ(LMS, Ych);
    dt_XYZ_to_Rec709_D65(Ych, RGB);
    const float max_RGB = fmaxf(fmaxf(RGB[0], RGB[1]), RGB[2]);
    for(size_t c = 0; c < 3; c++) RGB[c] = powf(RGB[c] / max_RGB, 1.f / 2.2f);
    dt_bauhaus_slider_set_stop(w, stop, RGB[0], RGB[1], RGB[2]);
  }

  gtk_widget_queue_draw(w);
}


void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_colorbalancergb_gui_data_t *g = (dt_iop_colorbalancergb_gui_data_t *)self->gui_data;
  dt_iop_colorbalancergb_params_t *p = (dt_iop_colorbalancergb_params_t *)self->params;

   ++darktable.gui->reset;

  if(!w || w == g->global_H)
    paint_chroma_slider(g->global_C, p->global_H);

  if(!w || w == g->shadows_H)
    paint_chroma_slider(g->shadows_C, p->shadows_H);

  if(!w || w == g->midtones_H)
    paint_chroma_slider(g->midtones_C, p->midtones_H);

  if(!w || w == g->highlights_H)
    paint_chroma_slider(g->highlights_C, p->highlights_H);

  --darktable.gui->reset;

}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_colorbalancergb_gui_data_t *g = (dt_iop_colorbalancergb_gui_data_t *)self->gui_data;
  dt_iop_colorbalancergb_params_t *p = (dt_iop_colorbalancergb_params_t *)self->params;

  dt_bauhaus_slider_set_soft(g->hue_angle, p->hue_angle);

  dt_bauhaus_slider_set_soft(g->chroma_global, p->chroma_global);
  dt_bauhaus_slider_set_soft(g->chroma_highlights, p->chroma_highlights);
  dt_bauhaus_slider_set_soft(g->chroma_midtones, p->chroma_midtones);
  dt_bauhaus_slider_set_soft(g->chroma_shadows, p->chroma_shadows);

  dt_bauhaus_slider_set_soft(g->saturation_global, p->saturation_global);
  dt_bauhaus_slider_set_soft(g->saturation_highlights, p->saturation_highlights);
  dt_bauhaus_slider_set_soft(g->saturation_midtones, p->saturation_midtones);
  dt_bauhaus_slider_set_soft(g->saturation_shadows, p->saturation_shadows);


  dt_bauhaus_slider_set_soft(g->purity_global, p->purity_global);
  dt_bauhaus_slider_set_soft(g->purity_highlights, p->purity_highlights);
  dt_bauhaus_slider_set_soft(g->purity_midtones, p->purity_midtones);
  dt_bauhaus_slider_set_soft(g->purity_shadows, p->purity_shadows);

  dt_bauhaus_slider_set_soft(g->global_C, p->global_C);
  dt_bauhaus_slider_set_soft(g->global_H, p->global_H);
  dt_bauhaus_slider_set_soft(g->global_Y, p->global_Y);

  dt_bauhaus_slider_set_soft(g->shadows_C, p->shadows_C);
  dt_bauhaus_slider_set_soft(g->shadows_H, p->shadows_H);
  dt_bauhaus_slider_set_soft(g->shadows_Y, p->shadows_Y);
  dt_bauhaus_slider_set_soft(g->shadows_weight, p->shadows_weight);

  dt_bauhaus_slider_set_soft(g->midtones_C, p->midtones_C);
  dt_bauhaus_slider_set_soft(g->midtones_H, p->midtones_H);
  dt_bauhaus_slider_set_soft(g->midtones_Y, p->midtones_Y);
  dt_bauhaus_slider_set_soft(g->midtones_weight, p->midtones_weight);

  dt_bauhaus_slider_set_soft(g->highlights_C, p->highlights_C);
  dt_bauhaus_slider_set_soft(g->highlights_H, p->highlights_H);
  dt_bauhaus_slider_set_soft(g->highlights_Y, p->highlights_Y);
  dt_bauhaus_slider_set_soft(g->highlights_weight, p->highlights_weight);

  gui_changed(self, NULL, NULL);
  dt_iop_color_picker_reset(self, TRUE);
}


void gui_reset(dt_iop_module_t *self)
{
  //dt_iop_colorbalancergb_gui_data_t *g = (dt_iop_colorbalancergb_gui_data_t *)self->gui_data;
  dt_iop_color_picker_reset(self, TRUE);
}


void gui_init(dt_iop_module_t *self)
{
  dt_iop_colorbalancergb_gui_data_t *g = IOP_GUI_ALLOC(colorbalancergb);

  // start building top level widget
  g->notebook = GTK_NOTEBOOK(gtk_notebook_new());

  // Page master
  self->widget = dt_ui_notebook_page(g->notebook, _("master"), _("global grading"));

  g->hue_angle = dt_bauhaus_slider_from_params(self, "hue_angle");
  dt_bauhaus_slider_set_digits(g->hue_angle, 4);
  dt_bauhaus_slider_set_step(g->hue_angle, 1.);
  dt_bauhaus_slider_set_format(g->hue_angle, "%.2f °");
  gtk_widget_set_tooltip_text(g->hue_angle, _("rotate all hues by an angle, at the same luminance"));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("linear chroma grading")), FALSE, FALSE, 0);

  g->chroma_global = dt_bauhaus_slider_from_params(self, "chroma_global");
  dt_bauhaus_slider_set_soft_range(g->chroma_global, -0.5, 0.5);
  dt_bauhaus_slider_set_digits(g->chroma_global, 4);
  dt_bauhaus_slider_set_factor(g->chroma_global, 100.0f);
  dt_bauhaus_slider_set_format(g->chroma_global, "%.2f %%");
  gtk_widget_set_tooltip_text(g->chroma_global, _("increase colorfulness at same luminance globally"));

  g->chroma_shadows = dt_bauhaus_slider_from_params(self, "chroma_shadows");
  dt_bauhaus_slider_set_digits(g->chroma_shadows, 4);
  dt_bauhaus_slider_set_factor(g->chroma_shadows, 100.0f);
  dt_bauhaus_slider_set_format(g->chroma_shadows, "%.2f %%");
  gtk_widget_set_tooltip_text(g->chroma_shadows, _("increase colorfulness at same luminance mostly in shadows"));

  g->chroma_midtones = dt_bauhaus_slider_from_params(self, "chroma_midtones");
  dt_bauhaus_slider_set_digits(g->chroma_midtones, 4);
  dt_bauhaus_slider_set_factor(g->chroma_midtones, 100.0f);
  dt_bauhaus_slider_set_format(g->chroma_midtones, "%.2f %%");
  gtk_widget_set_tooltip_text(g->chroma_midtones, _("increase colorfulness at same luminance mostly in midtones"));

  g->chroma_highlights = dt_bauhaus_slider_from_params(self, "chroma_highlights");
  dt_bauhaus_slider_set_digits(g->chroma_highlights, 4);
  dt_bauhaus_slider_set_factor(g->chroma_highlights, 100.0f);
  dt_bauhaus_slider_set_format(g->chroma_highlights, "%.2f %%");
  gtk_widget_set_tooltip_text(g->chroma_highlights, _("increase colorfulness at same luminance mostly in highlights"));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("perceptual saturation grading")), FALSE, FALSE, 0);

  g->saturation_global = dt_bauhaus_slider_from_params(self, "saturation_global");
  dt_bauhaus_slider_set_soft_range(g->saturation_global, -0.5, 0.5);
  dt_bauhaus_slider_set_digits(g->saturation_global, 4);
  dt_bauhaus_slider_set_factor(g->saturation_global, 100.0f);
  dt_bauhaus_slider_set_format(g->saturation_global, "%.2f %%");
  gtk_widget_set_tooltip_text(g->saturation_global, _("add or remove saturation by an absolute amount"));

  g->saturation_shadows = dt_bauhaus_slider_from_params(self, "saturation_shadows");
  dt_bauhaus_slider_set_digits(g->saturation_shadows, 4);
  dt_bauhaus_slider_set_factor(g->saturation_shadows, 100.0f);
  dt_bauhaus_slider_set_format(g->saturation_shadows, "%.2f %%");
  gtk_widget_set_tooltip_text(g->saturation_shadows, _("increase or decrease saturation proportionnaly to the original pixel saturation"));

  g->saturation_midtones= dt_bauhaus_slider_from_params(self, "saturation_midtones");
  dt_bauhaus_slider_set_digits(g->saturation_midtones, 4);
  dt_bauhaus_slider_set_factor(g->saturation_midtones, 100.0f);
  dt_bauhaus_slider_set_format(g->saturation_midtones, "%.2f %%");
  gtk_widget_set_tooltip_text(g->saturation_midtones, _("increase or decrease saturation proportionnaly to the original pixel saturation"));

  g->saturation_highlights = dt_bauhaus_slider_from_params(self, "saturation_highlights");
  dt_bauhaus_slider_set_digits(g->saturation_highlights, 4);
  dt_bauhaus_slider_set_factor(g->saturation_highlights, 100.0f);
  dt_bauhaus_slider_set_format(g->saturation_highlights, "%.2f %%");
  gtk_widget_set_tooltip_text(g->saturation_highlights, _("increase or decrease saturation proportionnaly to the original pixel saturation"));


  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("perceptual purity grading")), FALSE, FALSE, 0);

  g->purity_global = dt_bauhaus_slider_from_params(self, "purity_global");
  dt_bauhaus_slider_set_soft_range(g->purity_global, -0.5, 0.5);
  dt_bauhaus_slider_set_digits(g->purity_global, 4);
  dt_bauhaus_slider_set_factor(g->purity_global, 100.0f);
  dt_bauhaus_slider_set_format(g->purity_global, "%.2f %%");
  gtk_widget_set_tooltip_text(g->purity_global, _("add or remove purity by an absolute amount"));

  g->purity_shadows = dt_bauhaus_slider_from_params(self, "purity_shadows");
  dt_bauhaus_slider_set_digits(g->purity_shadows, 4);
  dt_bauhaus_slider_set_factor(g->purity_shadows, 100.0f);
  dt_bauhaus_slider_set_format(g->purity_shadows, "%.2f %%");
  gtk_widget_set_tooltip_text(g->purity_shadows, _("increase or decrease purity proportionnaly to the original pixel purity"));

  g->purity_midtones= dt_bauhaus_slider_from_params(self, "purity_midtones");
  dt_bauhaus_slider_set_digits(g->purity_midtones, 4);
  dt_bauhaus_slider_set_factor(g->purity_midtones, 100.0f);
  dt_bauhaus_slider_set_format(g->purity_midtones, "%.2f %%");
  gtk_widget_set_tooltip_text(g->purity_midtones, _("increase or decrease purity proportionnaly to the original pixel purity"));

  g->purity_highlights = dt_bauhaus_slider_from_params(self, "purity_highlights");
  dt_bauhaus_slider_set_digits(g->purity_highlights, 4);
  dt_bauhaus_slider_set_factor(g->purity_highlights, 100.0f);
  dt_bauhaus_slider_set_format(g->purity_highlights, "%.2f %%");
  gtk_widget_set_tooltip_text(g->purity_highlights, _("increase or decrease purity proportionnaly to the original pixel purity"));


  // Page 4-ways
  self->widget = dt_ui_notebook_page(g->notebook, _("4 ways"), _("selective color grading"));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("global")), FALSE, FALSE, 0);

  g->global_Y = dt_bauhaus_slider_from_params(self, "global_Y");
  dt_bauhaus_slider_set_soft_range(g->global_Y, -0.05, 0.05);
  dt_bauhaus_slider_set_factor(g->global_Y, 100.0f);
  dt_bauhaus_slider_set_digits(g->global_Y, 4);
  dt_bauhaus_slider_set_format(g->global_Y, "%.2f %%");
  gtk_widget_set_tooltip_text(g->global_Y, _("global luminance offset"));

  g->global_H = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, "global_H"));
  dt_bauhaus_slider_set_feedback(g->global_H, 0);
  dt_bauhaus_slider_set_step(g->global_H, 10.);
  dt_bauhaus_slider_set_digits(g->global_H, 4);
  dt_bauhaus_slider_set_format(g->global_H, "%.2f °");
  gtk_widget_set_tooltip_text(g->global_H, _("hue of the global color offset"));

  g->global_C = dt_bauhaus_slider_from_params(self, "global_C");
  dt_bauhaus_slider_set_soft_range(g->global_C, 0., 0.005);
  dt_bauhaus_slider_set_digits(g->global_C, 4);
  dt_bauhaus_slider_set_factor(g->global_C, 100.0f);
  dt_bauhaus_slider_set_format(g->global_C, "%.2f %%");
  gtk_widget_set_tooltip_text(g->global_C, _("chroma of the global color offset"));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("shadows")), FALSE, FALSE, 0);

  g->shadows_Y = dt_bauhaus_slider_from_params(self, "shadows_Y");
  dt_bauhaus_slider_set_soft_range(g->shadows_Y, -0.5, 0.5);
  dt_bauhaus_slider_set_factor(g->shadows_Y, 100.0f);
  dt_bauhaus_slider_set_digits(g->shadows_Y, 4);
  dt_bauhaus_slider_set_format(g->shadows_Y, "%.2f %%");
  gtk_widget_set_tooltip_text(g->shadows_Y, _("luminance gain in shadows"));

  g->shadows_H = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, "shadows_H"));
  dt_bauhaus_slider_set_feedback(g->shadows_H, 0);
  dt_bauhaus_slider_set_step(g->shadows_H, 10.);
  dt_bauhaus_slider_set_digits(g->shadows_H, 4);
  dt_bauhaus_slider_set_format(g->shadows_H, "%.2f °");
  gtk_widget_set_tooltip_text(g->shadows_H, _("hue of the color gain in shadows"));

  g->shadows_C = dt_bauhaus_slider_from_params(self, "shadows_C");
  dt_bauhaus_slider_set_soft_range(g->shadows_C, 0., 0.1);
  dt_bauhaus_slider_set_step(g->shadows_C, 0.01);
  dt_bauhaus_slider_set_digits(g->shadows_C, 4);
  dt_bauhaus_slider_set_factor(g->shadows_C, 100.0f);
  dt_bauhaus_slider_set_format(g->shadows_C, "%.2f %%");
  gtk_widget_set_tooltip_text(g->shadows_C, _("chroma of the color gain in shadows"));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("midtones")), FALSE, FALSE, 0);

  g->midtones_Y = dt_bauhaus_slider_from_params(self, "midtones_Y");
  dt_bauhaus_slider_set_soft_range(g->midtones_Y, -0.25, 0.25);
  dt_bauhaus_slider_set_factor(g->midtones_Y, 100.0f);
  dt_bauhaus_slider_set_digits(g->midtones_Y, 4);
  dt_bauhaus_slider_set_format(g->midtones_Y, "%.2f %%");
  gtk_widget_set_tooltip_text(g->midtones_Y, _("luminance exponent in midtones"));

  g->midtones_H = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, "midtones_H"));
  dt_bauhaus_slider_set_feedback(g->midtones_H, 0);
  dt_bauhaus_slider_set_step(g->midtones_H, 10.);
  dt_bauhaus_slider_set_digits(g->midtones_H, 4);
  dt_bauhaus_slider_set_format(g->midtones_H, "%.2f °");
  gtk_widget_set_tooltip_text(g->midtones_H, _("hue of the color exponent in midtones"));

  g->midtones_C = dt_bauhaus_slider_from_params(self, "midtones_C");
  dt_bauhaus_slider_set_soft_range(g->midtones_C, 0., 0.02);
  dt_bauhaus_slider_set_step(g->midtones_C, 0.005);
  dt_bauhaus_slider_set_digits(g->midtones_C, 4);
  dt_bauhaus_slider_set_factor(g->midtones_C, 100.0f);
  dt_bauhaus_slider_set_format(g->midtones_C, "%.2f %%");
  gtk_widget_set_tooltip_text(g->midtones_C, _("chroma of the color exponent in midtones"));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("highlights")), FALSE, FALSE, 0);

  g->highlights_Y = dt_bauhaus_slider_from_params(self, "highlights_Y");
  dt_bauhaus_slider_set_soft_range(g->highlights_Y, -0.5, 0.5);
  dt_bauhaus_slider_set_factor(g->highlights_Y, 100.0f);
  dt_bauhaus_slider_set_digits(g->highlights_Y, 4);
  dt_bauhaus_slider_set_format(g->highlights_Y, "%.2f %%");
  gtk_widget_set_tooltip_text(g->highlights_Y, _("luminance gain in highlights"));

  g->highlights_H = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, "highlights_H"));
  dt_bauhaus_slider_set_feedback(g->highlights_H, 0);
  dt_bauhaus_slider_set_step(g->highlights_H, 10.);
  dt_bauhaus_slider_set_digits(g->highlights_H, 4);
  dt_bauhaus_slider_set_format(g->highlights_H, "%.2f °");
  gtk_widget_set_tooltip_text(g->highlights_H, _("hue of the color gain in highlights"));

  g->highlights_C = dt_bauhaus_slider_from_params(self, "highlights_C");
  dt_bauhaus_slider_set_soft_range(g->highlights_C, 0., 0.05);
  dt_bauhaus_slider_set_step(g->shadows_C, 0.01);
  dt_bauhaus_slider_set_digits(g->highlights_C, 4);
  dt_bauhaus_slider_set_factor(g->highlights_C, 100.0f);
  dt_bauhaus_slider_set_format(g->highlights_C, "%.2f %%");
  gtk_widget_set_tooltip_text(g->highlights_C, _("chroma of the color gain in highlights"));

  // Page masks
  self->widget = dt_ui_notebook_page(g->notebook, _("masks"), _("isolate luminances"));

  g->shadows_weight = dt_bauhaus_slider_from_params(self, "shadows_weight");
  dt_bauhaus_slider_set_digits(g->shadows_weight, 4);
  dt_bauhaus_slider_set_step(g->shadows_weight, 0.1);
  dt_bauhaus_slider_set_format(g->shadows_weight, "%.2f %%");
  dt_bauhaus_slider_set_factor(g->shadows_weight, 100.0f);
  gtk_widget_set_tooltip_text(g->shadows_weight, _("weight of the shadows over the whole tonal range"));

  g->midtones_weight = dt_bauhaus_slider_from_params(self, "midtones_weight");
  dt_bauhaus_slider_set_soft_range(g->midtones_weight, -2., +2.);
  dt_bauhaus_slider_set_step(g->midtones_weight, 0.1);
  dt_bauhaus_slider_set_digits(g->midtones_weight, 4);
  dt_bauhaus_slider_set_format(g->midtones_weight, "%.2f EV");
  gtk_widget_set_tooltip_text(g->midtones_weight, _("peak white luminance value used to normalize the power function"));

  g->highlights_weight = dt_bauhaus_slider_from_params(self, "highlights_weight");
  dt_bauhaus_slider_set_step(g->highlights_weight, 0.1);
  dt_bauhaus_slider_set_digits(g->highlights_weight, 4);
  dt_bauhaus_slider_set_format(g->highlights_weight, "%.2f %%");
  dt_bauhaus_slider_set_factor(g->highlights_weight, 100.0f);
  gtk_widget_set_tooltip_text(g->highlights_weight, _("weights of highlights over the whole tonal range"));


  // paint backgrounds
  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    const float h = DEG_TO_RAD(stop * (360.f));
    float RGB[4] = { 0.f };
    float Ych[4] = { 0.75f, 0.2f, h, 0.f };
    float LMS[4] = { 0.f };
    Ych_to_gradingRGB(Ych, LMS, NULL);
    gradingRGB_to_XYZ(LMS, Ych);
    dt_XYZ_to_Rec709_D65(Ych, RGB);
    const float max_RGB = fmaxf(fmaxf(RGB[0], RGB[1]), RGB[2]);
    for(size_t c = 0; c < 3; c++) RGB[c] = powf(RGB[c] / max_RGB, 1.f / 2.2f);
    dt_bauhaus_slider_set_stop(g->global_H, stop, RGB[0], RGB[1], RGB[2]);
    dt_bauhaus_slider_set_stop(g->shadows_H, stop, RGB[0], RGB[1], RGB[2]);
    dt_bauhaus_slider_set_stop(g->highlights_H, stop, RGB[0], RGB[1], RGB[2]);
    dt_bauhaus_slider_set_stop(g->midtones_H, stop, RGB[0], RGB[1], RGB[2]);

    const float Y = 0.f + stop;
    dt_bauhaus_slider_set_stop(g->global_Y, stop, Y, Y, Y);
    dt_bauhaus_slider_set_stop(g->shadows_Y, stop, Y, Y, Y);
    dt_bauhaus_slider_set_stop(g->highlights_Y, stop, Y, Y, Y);
    dt_bauhaus_slider_set_stop(g->midtones_Y, stop, Y, Y, Y);
  }

  // main widget is the notebook
  self->widget = GTK_WIDGET(g->notebook);
}


void gui_cleanup(struct dt_iop_module_t *self)
{
  IOP_GUI_FREE;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
