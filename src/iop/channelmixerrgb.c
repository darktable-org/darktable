/*
  This file is part of darktable,
  Copyright (C) 2010-2020 darktable developers.

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
#include "develop/imageop_gui.h"
#include "dtgtk/drawingarea.h"
#include "common/chromatic_adaptation.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/opencl.h"
#include "common/illuminants.h"
#include "common/iop_profile.h"
#include "develop/imageop_math.h"
#include "gui/accelerators.h"
#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

#include <assert.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

DT_MODULE_INTROSPECTION(2, dt_iop_channelmixer_rgb_params_t)

/** Note :
 * we use finite-math-only and fast-math because divisions by zero are manually avoided in the code
 * fp-contract=fast enables hardware-accelerated Fused Multiply-Add
 * the rest is loop reorganization and vectorization optimization
 **/
#if defined(__GNUC__)
#pragma GCC optimize ("unroll-loops", "tree-loop-if-convert", \
                      "tree-loop-distribution", "no-strict-aliasing", \
                      "loop-interchange", "loop-nest-optimize", "tree-loop-im", \
                      "unswitch-loops", "tree-loop-ivcanon", "ira-loop-pressure", \
                      "split-ivs-in-unroller", "variable-expansion-in-unroller", \
                      "split-loops", "ivopts", "predictive-commoning",\
                      "tree-loop-linear", "loop-block", "loop-strip-mine", \
                      "finite-math-only", "fp-contract=fast", "fast-math", \
                      "tree-vectorize", "no-math-errno")
#endif


#define CHANNEL_SIZE 4
#define NORM_MIN 1e-6f

typedef struct dt_iop_channelmixer_rgb_params_t
{
  float red[CHANNEL_SIZE];         // $MIN: -2.0 $MAX: 2.0
  float green[CHANNEL_SIZE];       // $MIN: -2.0 $MAX: 2.0
  float blue[CHANNEL_SIZE];        // $MIN: -2.0 $MAX: 2.0
  float saturation[CHANNEL_SIZE];  // $MIN: -1.0 $MAX: 1.0
  float lightness[CHANNEL_SIZE];   // $MIN: -1.0 $MAX: 1.0
  float grey[CHANNEL_SIZE];        // $MIN: 0.0 $MAX: 1.0
  gboolean normalize_R, normalize_G, normalize_B, normalize_sat, normalize_light, normalize_grey; // $DESCRIPTION: "normalize channels"
  dt_illuminant_t illuminant;      // $DEFAULT: DT_ILLUMINANT_D
  dt_illuminant_fluo_t illum_fluo; // $DEFAULT: DT_ILLUMINANT_FLUO_F3 $DESCRIPTION: "F source"
  dt_illuminant_led_t illum_led;   // $DEFAULT: DT_ILLUMINANT_LED_B5 $DESCRIPTION: "LED source"
  dt_adaptation_t adaptation;      // $DEFAULT: DT_ADAPTATION_LINEAR_BRADFORD
  float x, y;                      // $DEFAULT: 0.333
  float temperature;               // $MIN: 1667. $MAX: 25000. $DEFAULT: 5003.
  float gamut;                     // $MIN: 0.0 $MAX: 4.0 $DEFAULT: 1.0 $DESCRIPTION: "gamut compression"
  gboolean clip;                   // $DEFAULT: TRUE $DESCRIPTION: "clip negative RGB from gamut"
} dt_iop_channelmixer_rgb_params_t;


typedef struct dt_iop_channelmixer_rgb_gui_data_t
{
  GtkNotebook *notebook;
  GtkWidget *illuminant, *temperature, *adaptation, *gamut, *clip;
  GtkWidget *illum_fluo, *illum_led, *illum_x, *illum_y, *approx_cct, *illum_color;
  GtkWidget *scale_red_R, *scale_red_G, *scale_red_B;
  GtkWidget *scale_green_R, *scale_green_G, *scale_green_B;
  GtkWidget *scale_blue_R, *scale_blue_G, *scale_blue_B;
  GtkWidget *scale_saturation_R, *scale_saturation_G, *scale_saturation_B;
  GtkWidget *scale_lightness_R, *scale_lightness_G, *scale_lightness_B;
  GtkWidget *scale_grey_R, *scale_grey_G, *scale_grey_B;
  GtkWidget *normalize_R, *normalize_G, *normalize_B, *normalize_sat, *normalize_light, *normalize_grey;
  GtkWidget *color_picker;
  GtkWidget *warning_label;
  float xy[2];
  float XYZ[4];
  dt_pthread_mutex_t lock;
} dt_iop_channelmixer_rgb_gui_data_t;

typedef struct dt_iop_channelmixer_rbg_data_t
{
  float DT_ALIGNED_ARRAY MIX[3][4];
  float DT_ALIGNED_PIXEL saturation[CHANNEL_SIZE];
  float DT_ALIGNED_PIXEL lightness[CHANNEL_SIZE];
  float DT_ALIGNED_PIXEL grey[CHANNEL_SIZE];
  float DT_ALIGNED_PIXEL illuminant[4]; // XYZ coordinates of illuminant
  float p, gamut;
  int apply_grey;
  int clip;
  dt_adaptation_t adaptation;
  dt_illuminant_t illuminant_type;
} dt_iop_channelmixer_rbg_data_t;


const char *name()
{
  return _("color calibration");
}

const char *aliases()
{
  return _("channel mixer|white balance|monochrome");
}

const char *description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("perform color space corrections\n"
                                        "such as white balance, channels mixing\n"
                                        "and conversions to monochrome emulating film"),
                                      _("corrective or creative"),
                                      _("linear, RGB, scene-referred"),
                                      _("linear, RGB or XYZ"),
                                      _("linear, RGB, scene-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_group()
{
  return IOP_GROUP_COLOR;
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
    // V1 and V2 use the same param structure but the normalize_grey param had no effect since commit_params
    // forced normalization no matter what. So we re-import the params and force the param to TRUE to keep edits.
    memcpy(new_params, old_params, sizeof(dt_iop_channelmixer_rgb_params_t));
    dt_iop_channelmixer_rgb_params_t *n = (dt_iop_channelmixer_rgb_params_t *)new_params;
    n->normalize_grey = TRUE;
    return 0;
  }
  return 1;
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_channelmixer_rgb_params_t p;
  memset(&p, 0, sizeof(p));

  // bypass adaptation
  p.illuminant = DT_ILLUMINANT_PIPE;
  p.adaptation = DT_ADAPTATION_XYZ;

  // set everything to no-op
  p.gamut = 0.f;
  p.clip = FALSE;
  p.illum_fluo = DT_ILLUMINANT_FLUO_F3;
  p.illum_led = DT_ILLUMINANT_LED_B5;
  p.temperature = 5003.f;
  illuminant_to_xy(DT_ILLUMINANT_PIPE, NULL, NULL, &p.x, &p.y, p.temperature, DT_ILLUMINANT_FLUO_LAST, DT_ILLUMINANT_LED_LAST);

  p.red[0] = 1.f;
  p.red[1] = 0.f;
  p.red[2] = 0.f;
  p.green[0] = 0.f;
  p.green[1] = 1.f;
  p.green[2] = 0.f;
  p.blue[0] = 0.f;
  p.blue[1] = 0.f;
  p.blue[2] = 1.f;

  p.saturation[0] = 0.f;
  p.saturation[1] = 0.f;
  p.saturation[2] = 0.f;
  p.lightness[0] = 0.f;
  p.lightness[1] = 0.f;
  p.lightness[2] = 0.f;
  p.grey[0] = 0.f;
  p.grey[1] = 0.f;
  p.grey[2] = 0.f;

  p.normalize_R = FALSE;
  p.normalize_G = FALSE;
  p.normalize_B = FALSE;
  p.normalize_sat = FALSE;
  p.normalize_light = FALSE;
  p.normalize_grey = TRUE;

  // Create B&W presets
  p.grey[0] = 0.f;
  p.grey[1] = 1.f;
  p.grey[2] = 0.f;

  dt_gui_presets_add_generic(_("B&W : luminance-based"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // film emulations

  /* These emulations are built using spectral sensitivies provided by film manufacturers for tungsten light,
  * corrected in spectral domain for D50 illuminant, and integrated in spectral space against CIE 2° 1931 XYZ
  * color matching functions in the Python lib Colour, with the following code :
  *
    import colour
    import numpy as np

    XYZ = np.zeros((3))

    for l in range(360, 830):
        XYZ += film_CMF[l] * colour.colorimetry.STANDARD_OBSERVERS_CMFS['CIE 1931 2 Degree Standard Observer'][l] / colour.ILLUMINANTS_SDS['A'][l] * colour.ILLUMINANTS_SDS['D50'][l]

    XYZ / np.sum(XYZ)
  *
  * The film CMF is visually approximated from the graph. It is still more accurate than bullshit factors
  * in legacy channel mixer that don't even say in which RGB space they are supposed to be applied.
  */

  // Ilford HP5 +
  // https://www.ilfordphoto.com/amfile/file/download/file/1903/product/695/
  p.grey[0] = 0.25304098f;
  p.grey[1] = 0.25958747f;
  p.grey[2] = 0.48737156f;

  dt_gui_presets_add_generic(_("B&W : Ilford HP5+"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // Ilford Delta 100
  // https://www.ilfordphoto.com/amfile/file/download/file/3/product/681/
  p.grey[0] = 0.24552374f;
  p.grey[1] = 0.25366007f;
  p.grey[2] = 0.50081619f;

  dt_gui_presets_add_generic(_("B&W : Ilford Delta 100"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // Ilford Delta 400 and 3200 - they have the same curve
  // https://www.ilfordphoto.com/amfile/file/download/file/1915/product/685/
  // https://www.ilfordphoto.com/amfile/file/download/file/1913/product/683/
  p.grey[0] = 0.24376712f;
  p.grey[1] = 0.23613559f;
  p.grey[2] = 0.52009729f;

  dt_gui_presets_add_generic(_("B&W : Ilford Delta 400 - 3200"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // Ilford FP 2
  // https://www.ilfordphoto.com/amfile/file/download/file/1919/product/690/
  p.grey[0] = 0.24149085f;
  p.grey[1] = 0.22149272f;
  p.grey[2] = 0.53701643f;

  dt_gui_presets_add_generic(_("B&W : Ilford FP2"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // Fuji Acros 100
  // https://dacnard.wordpress.com/2013/02/15/the-real-shades-of-gray-bw-film-is-a-matter-of-heart-pt-1/
  p.grey[0] = 0.333f;
  p.grey[1] = 0.313f;
  p.grey[2] = 0.353f;

  dt_gui_presets_add_generic(_("B&W : Fuji Acros 100"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // Kodak ?
  // can't find spectral sensivity curves and the illuminant under wich they are produced,
  // so ¯\_(ツ)_/¯

  // basic channel-mixer
  p.adaptation = DT_ADAPTATION_RGB; // bypass adaptation
  p.grey[0] = 0.f;
  p.grey[1] = 0.f;
  p.grey[2] = 0.f;
  p.normalize_R = TRUE;
  p.normalize_G = TRUE;
  p.normalize_B = TRUE;
  p.normalize_grey = FALSE;
  dt_gui_presets_add_generic(_("basic channel mixer"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // swap G-B
  p.red[0] = 1.f;
  p.red[1] = 0.f;
  p.red[2] = 0.f;
  p.green[0] = 0.f;
  p.green[1] = 0.f;
  p.green[2] = 1.f;
  p.blue[0] = 0.f;
  p.blue[1] = 1.f;
  p.blue[2] = 0.f;
  dt_gui_presets_add_generic(_("swap G and B"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // swap G-R
  p.red[0] = 0.f;
  p.red[1] = 1.f;
  p.red[2] = 0.f;
  p.green[0] = 1.f;
  p.green[1] = 0.f;
  p.green[2] = 0.f;
  p.blue[0] = 0.f;
  p.blue[1] = 0.f;
  p.blue[2] = 1.f;
  dt_gui_presets_add_generic(_("swap G and R"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // swap R-B
  p.red[0] = 0.f;
  p.red[1] = 0.f;
  p.red[2] = 1.f;
  p.green[0] = 0.f;
  p.green[1] = 1.f;
  p.green[2] = 0.f;
  p.blue[0] = 1.f;
  p.blue[1] = 0.f;
  p.blue[2] = 0.f;
  dt_gui_presets_add_generic(_("swap R and B"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);
}


static int get_white_balance_coeff(struct dt_iop_module_t *self, float custom_wb[4])
{
  // Init output with a no-op
  for(size_t k = 0; k < 4; k++) custom_wb[k] = 1.f;

  if(!dt_image_is_matrix_correction_supported(&self->dev->image_storage)) return 1;

  // First, get the D65-ish coeffs from the input matrix
  // keep this in synch with calculate_bogus_daylight_wb from temperature.c !
  // predicts the bogus D65 that temperature.c will compute for the camera input matrix
  double bwb[4];

  if(dt_colorspaces_conversion_matrices_rgb(self->dev->image_storage.camera_makermodel, NULL, NULL, self->dev->image_storage.d65_color_matrix, bwb))
  {
    // normalize green:
    bwb[0] /= bwb[1];
    bwb[2] /= bwb[1];
    bwb[3] /= bwb[1];
    bwb[1] = 1.0;
  }
  else
  {
    return 1;
  }

  // Second, if the temperature module is not using these, for example because they are wrong
  // and user made a correct preset, find the WB adaptation ratio
  if(self->dev->proxy.wb_coeffs[0] != 0.f)
  {
    for(size_t k = 0; k < 4; k++) custom_wb[k] = bwb[k] / self->dev->proxy.wb_coeffs[k];
  }

  return 0;
}


#ifdef _OPENMP
#pragma omp declare simd uniform(v_2) aligned(v_1, v_2:16)
#endif
static inline float scalar_product(const float v_1[4], const float v_2[4])
{
  // specialized 3×1 dot products 2 4×1 RGB-alpha pixels.
  // v_2 needs to be uniform along loop increments, e.g. independent from current pixel values
  // we force an order of computation similar to SSE4 _mm_dp_ps() hoping the compiler will get the clue
  float DT_ALIGNED_PIXEL premul[4] = { 0.f };
  for(size_t c = 0; c < 3; c++) premul[c] = v_1[c] * v_2[c];
  return premul[0] + premul[1] + premul[2];
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float sqf(const float x)
{
  return x * x;
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float clamp_simd(const float x)
{
  return fminf(fmaxf(x, 0.0f), 1.0f);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(vector:16)
#endif
static inline float euclidean_norm(const float vector[4])
{
  return sqrtf(fmaxf(sqf(vector[0]) + sqf(vector[1]) + sqf(vector[2]), 1e-6f));
}


#ifdef _OPENMP
#pragma omp declare simd aligned(vector:16)
#endif
static inline void downscale_vector(float vector[4], const float scaling)
{
  // check zero or NaN
  const int valid = (scaling > NORM_MIN) && !isnan(scaling);
  for(size_t c = 0; c < 3; c++) vector[c] = (valid) ? vector[c] / (scaling + NORM_MIN) : vector[c] / NORM_MIN;
}


#ifdef _OPENMP
#pragma omp declare simd aligned(vector:16)
#endif
static inline void upscale_vector(float vector[4], const float scaling)
{
  const int valid = (scaling > NORM_MIN) && !isnan(scaling);
  for(size_t c = 0; c < 3; c++) vector[c] = (valid) ? vector[c] * (scaling + NORM_MIN) : vector[c] * NORM_MIN;
}


#ifdef _OPENMP
#pragma omp declare simd aligned(input, output:16) uniform(compression, clip)
#endif
static inline void gamut_mapping(const float input[4], const float compression, const int clip, float output[4])
{
  // Get the sum XYZ
  float sum = 0.f;
  for(size_t c = 0; c < 3; c++) sum += input[c];
  sum = fmaxf(sum, NORM_MIN);

  // Convert to xyY
  float Y = fmaxf(input[1], 0.f);
  float xyY[4] DT_ALIGNED_PIXEL = { input[0] / sum, input[1] / sum , Y, 0.0f };

  // Convert to uvY
  float uvY[4] DT_ALIGNED_PIXEL;
  dt_xyY_to_uvY(xyY, uvY);

  // Get the chromaticity difference with white point uv
  const float D50[2] DT_ALIGNED_PIXEL = { 0.20915914598542354f, 0.488075320769787f };
  const float delta[2] DT_ALIGNED_PIXEL = { D50[0] - uvY[0], D50[1] - uvY[1] };
  const float Delta = Y * (sqf(delta[0]) + sqf(delta[1]));

  // Compress chromaticity (move toward white point)
  const float correction = (compression == 0.0f) ? 0.f : powf(Delta, compression);
  for(size_t c = 0; c < 2; c++)
  {
    // Ensure the correction does not bring our uyY vector the other side of D50
    // that would switch to the opposite color, so we clip at D50
    const float tmp = DT_FMA(correction, delta[c], uvY[c]); // correction * delta[c] + uvY[c]
    uvY[c] = (uvY[c] > D50[c]) ? fmaxf(tmp, D50[c])
                               : fminf(tmp, D50[c]);
  }

  // Convert back to xyY
  dt_uvY_to_xyY(uvY, xyY);

  // Clip upon request
  if(clip) for(size_t c = 0; c < 2; c++) xyY[c] = fmaxf(xyY[c], 0.0f);

  // Check sanity of y
  // since we later divide by y, it can't be zero
  xyY[1] = fmaxf(xyY[1], NORM_MIN);

  // Check sanity of x and y :
  // since Z = Y (1 - x - y) / y, if x + y >= 1, Z will be negative
  const float scale = xyY[0] + xyY[1];
  const int sanitize = (scale >= 1.f);
  for(size_t c = 0; c < 2; c++) xyY[c] = (sanitize) ? xyY[c] / scale : xyY[c];

  // Convert back to XYZ
  output[0] = xyY[2] * xyY[0] / xyY[1];
  output[1] = xyY[2];
  output[2] = xyY[2] * (1.f - xyY[0] - xyY[1]) / xyY[1];
}


#ifdef _OPENMP
#pragma omp declare simd aligned(input, output, saturation, lightness:16) uniform(saturation, lightness)
#endif
static inline void luma_chroma(const float input[4], const float saturation[4], const float lightness[4],
                               float output[4])
{
  // Compute euclidean norm and flat lightness adjustment
  const float avg = (input[0] + input[1] + input[2]) / 3.0f;
  const float mix = scalar_product(input, lightness);
  float norm = euclidean_norm(input);

  // Ratios
  for(size_t c = 0; c < 3; c++) output[c] = input[c] / norm;

  // Compute ratios and a flat colorfulness adjustment for the whole pixel
  float coeff_ratio = 0.f;
  for(size_t c = 0; c < 3; c++)
    coeff_ratio += sqf(1.0f - output[c]) * saturation[c];
  coeff_ratio /= 3.f;

  // Adjust the RGB ratios with the pixel correction
  for(size_t c = 0; c < 3; c++)
  {
    // if the ratio was already invalid (negative), we accept the result to be invalid too
    // otherwise bright saturated blues end up solid black
    const float min_ratio = (output[c] < 0.0f) ? output[c] : 0.0f;
    const float output_inverse = 1.0f - output[c];
    output[c] = fmaxf(DT_FMA(output_inverse, coeff_ratio, output[c]), min_ratio); // output_inverse  * coeff_ratio + output
  }

  // Apply colorfulness adjustment channel-wise and repack with lightness to get LMS back
  norm *= fmaxf(1.f + mix / avg, 0.f);
  for(size_t c = 0; c < 3; c++) output[c] *= norm;
}


static inline void loop_switch(const float *const restrict in, float *const restrict out,
                               const size_t width, const size_t height, const size_t ch,
                               const float XYZ_to_RGB[3][4], const float RGB_to_XYZ[3][4], const float MIX[3][4],
                               const float illuminant[4], const float saturation[4], const float lightness[4], const float grey[4],
                               const float p, const float gamut, const int clip, const int apply_grey, const dt_adaptation_t kind)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(width, height, ch, in, out, XYZ_to_RGB, RGB_to_XYZ, MIX, illuminant, saturation, lightness, grey, p, gamut, clip, apply_grey, kind) \
  aligned(in, out, XYZ_to_RGB, RGB_to_XYZ, MIX:64) aligned(illuminant, saturation, lightness, grey:16)\
  schedule(simd:static)
#endif
  for(size_t k = 0; k < height * width * ch; k += ch)
  {
    // intermediate temp buffers
    float DT_ALIGNED_PIXEL temp_one[4];
    float DT_ALIGNED_PIXEL temp_two[4];

    for(size_t c = 0; c < 3; c++)
      temp_two[c] = (clip) ? fmaxf(in[k + c], 0.0f) : in[k + c];
    float Y = 1.f;

    switch(kind)
    {
      case DT_ADAPTATION_FULL_BRADFORD:
      {
        // Convert from RGB to XYZ
        dot_product(temp_two, RGB_to_XYZ, temp_one);

        // Normalize by Y
        Y = temp_one[1];
        downscale_vector(temp_one, Y);

        // Convert from XYZ to LMS
        convert_XYZ_to_bradford_LMS(temp_one, temp_two);

        // Do white balance in LMS
        bradford_adapt_D50(temp_two, illuminant, p, TRUE, temp_one);

        // Compute the 3D mix in LMS - this is a rotation + homothety of the vector base
        dot_product(temp_one, MIX, temp_two);

        break;
      }
      case DT_ADAPTATION_LINEAR_BRADFORD:
      {
        // Convert from RGB to XYZ
        dot_product(temp_two, RGB_to_XYZ, temp_one);

         // Normalize by Y
        Y = temp_one[1];
        downscale_vector(temp_one, Y);

        // Convert from XYZ to LMS
        convert_XYZ_to_bradford_LMS(temp_one, temp_two);

        // Do white balance in LMS
        bradford_adapt_D50(temp_two, illuminant, p, FALSE, temp_one);

        // Compute the 3D mix in LMS - this is a rotation + homothety of the vector base
        dot_product(temp_one, MIX, temp_two);

        break;
      }
      case DT_ADAPTATION_CAT16:
      {
        // Convert from RGB to XYZ
        dot_product(temp_two, RGB_to_XYZ, temp_one);

         // Normalize by Y
        Y = temp_one[1];
        downscale_vector(temp_one, Y);

        // Convert from XYZ to LMS
        convert_XYZ_to_CAT16_LMS(temp_one, temp_two);

        // Do white balance in LMS
        CAT16_adapt_D50(temp_two, illuminant, 1.0f, TRUE, temp_one); // force full-adaptation

        // Compute the 3D mix in LMS - this is a rotation + homothety of the vector base
        dot_product(temp_one, MIX, temp_two);

        break;
      }
      case DT_ADAPTATION_XYZ:
      {
        // Convert from RGB to XYZ
        dot_product(temp_two, RGB_to_XYZ, temp_one);

         // Normalize by Y
        Y = temp_one[1];
        downscale_vector(temp_one, Y);

        // Do white balance in XYZ
        XYZ_adapt_D50(temp_one, illuminant, temp_two);

        // Compute the 3D mix in XYZ - this is a rotation + homothety of the vector base
        dot_product(temp_two, MIX, temp_one);
        dt_simd_memcpy(temp_one, temp_two, 4);

        break;
      }
      case DT_ADAPTATION_RGB:
      case DT_ADAPTATION_LAST:
      {
        // No white balance.
        // Compute the 3D mix in RGB - this is a rotation + homothety of the vector base
        dot_product(temp_two, MIX, temp_one);

        // Convert from RGB to XYZ
        dot_product(temp_one, RGB_to_XYZ, temp_two);

        // Normalize by Y
        Y = temp_one[1];
        downscale_vector(temp_two, Y);
        break;
      }
    }

    // Gamut mapping happens in XYZ space no matter what
    convert_any_LMS_to_XYZ(temp_two, temp_one, kind);
      upscale_vector(temp_one, Y);
        gamut_mapping(temp_one, gamut, clip, temp_two);
      downscale_vector(temp_two, Y);
    convert_any_XYZ_to_LMS(temp_two, temp_one, kind);

    // Clip in LMS
    if(clip) for(size_t c = 0; c < 3; c++) temp_one[c] = fmaxf(temp_one[c], 0.0f);

    // Apply lightness / saturation adjustment
    luma_chroma(temp_one, saturation, lightness, temp_two);

    // Clip in LMS
    if(clip) for(size_t c = 0; c < 3; c++) temp_two[c] = fmaxf(temp_two[c], 0.0f);

    // Convert back LMS to XYZ to RGB
    convert_any_LMS_to_XYZ(temp_two, temp_one, kind);

    // Clip in XYZ
    if(clip) for(size_t c = 0; c < 3; c++) temp_one[c] = fmaxf(temp_one[c], 0.0f);

    upscale_vector(temp_one, Y);

    // Save
    if(apply_grey)
    {
      // Turn RGB into monochrome
      const float grey_mix = fmaxf(scalar_product(temp_one, grey), 0.0f);

      out[k] = out[k + 1] = out[k + 2] = grey_mix;
      out[k + 3] = in[k + 3]; // alpha mask
    }
    else
    {
      // Convert back to RGB
      dot_product(temp_one, XYZ_to_RGB, temp_two);

      if(clip)
        for(size_t c = 0; c < 3; c++) out[k + c] = fmaxf(temp_two[c], 0.0f);
      else
        for(size_t c = 0; c < 3; c++) out[k + c] = temp_two[c];

      out[k + 3] = in[k + 3]; // alpha mask
    }
  }
}

// util to shift pixel index without headache
#define SHF(ii, jj, c) ((i + ii) * width + j + jj) * ch + c
#define OFF 4

static inline void auto_detect_WB(const float *const restrict in, dt_illuminant_t illuminant,
                                  const size_t width, const size_t height, const size_t ch,
                                  const float RGB_to_XYZ[3][4], float xyz[4])
{
  /**
   * Detect the chromaticity of the illuminant based on the grey edges hypothesis.
   * So we compute a laplacian filter and get the weighted average of its chromaticities
   *
   * Inspired by :
   *  A Fast White Balance Algorithm Based on Pixel Greyness, Ba Thai·Guang Deng·Robert Ross
   *  https://www.researchgate.net/profile/Ba_Son_Thai/publication/308692177_A_Fast_White_Balance_Algorithm_Based_on_Pixel_Greyness/
   *
   *  Edge-Based Color Constancy, Joost van de Weijer, Theo Gevers, Arjan Gijsenij
   *  https://hal.inria.fr/inria-00548686/document
   *
  */

   float *const restrict temp = dt_alloc_sse_ps(width * height * ch);

   // Convert RGB to xy
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(width, height, ch, in, temp, RGB_to_XYZ) \
  aligned(in, temp, RGB_to_XYZ:64) collapse(2)\
  schedule(simd:static)
#endif
  for(size_t i = 0; i < height; i++)
    for(size_t j = 0; j < width; j++)
    {
      const size_t index = (i * width + j) * ch;
      float DT_ALIGNED_PIXEL RGB[4];
      float DT_ALIGNED_PIXEL XYZ[4];

      // Clip negatives
      for(size_t c = 0; c < 3; c++) RGB[c] = fmaxf(in[index + c], 0.0f);

      // Convert to XYZ
      dot_product(RGB, RGB_to_XYZ, XYZ);

      // Convert to xyY
      const float sum = fmaxf(XYZ[0] + XYZ[1] + XYZ[2], 1e-6f);
      XYZ[0] /= sum;   // x
      XYZ[2] = XYZ[1]; // Y
      XYZ[1] /= sum;   // y

      // Shift the chromaticity plane so the D50 point (target) becomes the origin
      const float D50[2] = { 0.34567f, 0.35850f };
      const float norm = hypotf(D50[0], D50[1]);

      temp[index    ] = (XYZ[0] - D50[0]) / norm;
      temp[index + 1] = (XYZ[1] - D50[1]) / norm;
      temp[index + 2] =  XYZ[2];
    }

  float elements = 0.f;
  float xyY[4] = { 0.f };

  if(illuminant == DT_ILLUMINANT_DETECT_SURFACES)
  {
#ifdef _OPENMP
#pragma omp parallel for simd default(none) reduction(+:xyY, elements) \
  dt_omp_firstprivate(width, height, ch, temp) \
  aligned(temp:64) \
  schedule(simd:static)
#endif
    for(size_t i = 2 * OFF; i < height - 4 * OFF; i += OFF)
      for(size_t j = 2 * OFF; j < width - 4 * OFF; j += OFF)
      {
        float DT_ALIGNED_PIXEL central_average[2];

        #pragma unroll
        for(size_t c = 0; c < 2; c++)
        {
          // B-spline local average / blur
          central_average[c] = (      temp[SHF(-OFF, -OFF, c)] + 2.f * temp[SHF(-OFF, 0, c)] +       temp[SHF(-OFF, +OFF, c)] +
                                2.f * temp[SHF(   0, -OFF, c)] + 4.f * temp[SHF(   0, 0, c)] + 2.f * temp[SHF(   0, +OFF, c)] +
                                      temp[SHF(+OFF, -OFF, c)] + 2.f * temp[SHF(+OFF, 0, c)] +       temp[SHF(+OFF, +OFF, c)]) / 16.0f;
          central_average[c] = fmaxf(central_average[c], 0.0f);
        }

        float var[4] = { 0.f };

        // compute patch-wise variance
        // If variance = 0, we are on a flat surface and want to discard that patch.
        #pragma unroll
        for(size_t c = 0; c < 2; c++)
        {
          var[c] = (
                      sqf(temp[SHF(-OFF, -OFF, c)] - central_average[c]) +
                      sqf(temp[SHF(-OFF,    0, c)] - central_average[c]) +
                      sqf(temp[SHF(-OFF, +OFF, c)] - central_average[c]) +
                      sqf(temp[SHF(0,    -OFF, c)] - central_average[c]) +
                      sqf(temp[SHF(0,       0, c)] - central_average[c]) +
                      sqf(temp[SHF(0,    +OFF, c)] - central_average[c]) +
                      sqf(temp[SHF(+OFF, -OFF, c)] - central_average[c]) +
                      sqf(temp[SHF(+OFF,    0, c)] - central_average[c]) +
                      sqf(temp[SHF(+OFF, +OFF, c)] - central_average[c])
                    ) / 9.0f;
        }

        // Compute the patch-wise chroma covariance.
        // If covariance = 0, chroma channels are not correlated and we either have noise or chromatic aberrations.
        // Both ways, we want to discard that patch from the chroma average.
        var[2] = (
                    (temp[SHF(-OFF, -OFF, 0)] - central_average[0]) * (temp[SHF(-OFF, -OFF, 1)] - central_average[1]) +
                    (temp[SHF(-OFF,    0, 0)] - central_average[0]) * (temp[SHF(-OFF,    0, 1)] - central_average[1]) +
                    (temp[SHF(-OFF, +OFF, 0)] - central_average[0]) * (temp[SHF(-OFF, +OFF, 1)] - central_average[1]) +
                    (temp[SHF(   0, -OFF, 0)] - central_average[0]) * (temp[SHF(   0, -OFF, 1)] - central_average[1]) +
                    (temp[SHF(   0,    0, 0)] - central_average[0]) * (temp[SHF(   0,    0, 1)] - central_average[1]) +
                    (temp[SHF(   0, +OFF, 0)] - central_average[0]) * (temp[SHF(   0, +OFF, 1)] - central_average[1]) +
                    (temp[SHF(+OFF, -OFF, 0)] - central_average[0]) * (temp[SHF(+OFF, -OFF, 1)] - central_average[1]) +
                    (temp[SHF(+OFF,    0, 0)] - central_average[0]) * (temp[SHF(+OFF,    0, 1)] - central_average[1]) +
                    (temp[SHF(+OFF, +OFF, 0)] - central_average[0]) * (temp[SHF(+OFF, +OFF, 1)] - central_average[1])
                  ) / 9.0f;

        // Compute the Minkowski p-norm for regularization
        const float p = 8.f;
        const float p_norm = powf(powf(fabsf(central_average[0]), p) + powf(fabsf(central_average[1]), p), 1.f / p) + 1e-6f;
        const float weight = var[0] * var[1] * var[2];

        #pragma unroll
        for(size_t c = 0; c < 2; c++) xyY[c] += central_average[c] * weight / p_norm;
        elements += weight / p_norm;
      }
  }
  else if(illuminant == DT_ILLUMINANT_DETECT_EDGES)
  {
    #ifdef _OPENMP
#pragma omp parallel for simd default(none) reduction(+:xyY, elements) \
  dt_omp_firstprivate(width, height, ch, temp) \
  aligned(temp:64) \
  schedule(simd:static)
#endif
    for(size_t i = 2 * OFF; i < height - 4 * OFF; i += OFF)
      for(size_t j = 2 * OFF; j < width - 4 * OFF; j += OFF)
      {
        float DT_ALIGNED_PIXEL dd[2];
        float DT_ALIGNED_PIXEL central_average[2];

        #pragma unroll
        for(size_t c = 0; c < 2; c++)
        {
          // B-spline local average / blur
          central_average[c] = (      temp[SHF(-OFF, -OFF, c)] + 2.f * temp[SHF(-OFF, 0, c)] +       temp[SHF(-OFF, +OFF, c)] +
                                2.f * temp[SHF(   0, -OFF, c)] + 4.f * temp[SHF(   0, 0, c)] + 2.f * temp[SHF(   0, +OFF, c)] +
                                      temp[SHF(+OFF, -OFF, c)] + 2.f * temp[SHF(+OFF, 0, c)] +       temp[SHF(+OFF, +OFF, c)]) / 16.0f;

          // image - blur = laplacian = edges
          dd[c] = temp[SHF(0, 0, c)] - central_average[c];
        }

        // Compute the Minkowski p-norm for regularization
        const float p = 8.f;
        const float p_norm = powf(powf(fabsf(dd[0]), p) + powf(fabsf(dd[1]), p), 1.f / p) + 1e-6f;

        #pragma unroll
        for(size_t c = 0; c < 2; c++) xyY[c] -= dd[c] / p_norm;
        elements += 1.f;
      }
  }

  const float D50[2] = { 0.34567f, 0.35850 };
  const float norm_D50 = hypotf(D50[0], D50[1]);

  for(size_t c = 0; c < 2; c++)
    xyz[c] = norm_D50 * (xyY[c] / elements) + D50[c];

  dt_free_align(temp);
}

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


static void declare_cat_on_pipe(struct dt_iop_module_t *self, gboolean preset)
{
  // Advertise to the pipeline that we are doing chromatic adaptation here
  // preset = TRUE allows to capture the CAT a priori at init time
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  dt_iop_order_entry_t *this
      = dt_ioppr_get_iop_order_entry(self->dev->iop_order_list, "channelmixerrgb", self->multi_priority);

  if(this == NULL) return; // there is no point then

  if((self->enabled && !(p->adaptation == DT_ADAPTATION_RGB || p->illuminant == DT_ILLUMINANT_PIPE)) || preset)
  {
    // We do CAT here so we need to register this instance as CAT-handler.
    if(self->dev->proxy.chroma_adaptation == NULL)
    {
      // We are the first to try to register, let's go !
      self->dev->proxy.chroma_adaptation = this;
    }
    else
    {
      // Another instance already registered.
      // If we are lower in the pipe than it, register in its place.
      if(this->o.iop_order < self->dev->proxy.chroma_adaptation->o.iop_order)
        self->dev->proxy.chroma_adaptation = this;
    }
  }
  else
  {
    if(self->dev->proxy.chroma_adaptation != NULL)
    {
      // We do NOT do CAT here.
      // Deregister this instance as CAT-handler if it previously registered
      if(self->dev->proxy.chroma_adaptation == this)
        self->dev->proxy.chroma_adaptation = NULL;
    }
  }
}

static inline gboolean is_module_cat_on_pipe(struct dt_iop_module_t *self)
{
  // Check on the pipeline that we are doing chromatic adaptation here
  dt_iop_order_entry_t *this
      = dt_ioppr_get_iop_order_entry(self->dev->iop_order_list, "channelmixerrgb", self->multi_priority);

  if(this == NULL) return FALSE; // there is no point then

  return (self->dev->proxy.chroma_adaptation == this);
}


static void update_illuminants(struct dt_iop_module_t *self);
static void update_approx_cct(struct dt_iop_module_t *self);
static void update_illuminant_color(struct dt_iop_module_t *self);


static void check_if_close_to_daylight(const float x, const float y, float *temperature,
                                       dt_illuminant_t *illuminant, dt_adaptation_t *adaptation)
{
  /* Check if a chromaticity x, y is close to daylight within 2.5 % error margin.
   * If so, we enable the daylight GUI for better ergonomics
   * Otherwise, we default to direct x, y control for better accuracy
   *
   * Note : The use of CCT is discouraged if dE > 5 % in CIE 1960 Yuv space
   *        reference : https://onlinelibrary.wiley.com/doi/abs/10.1002/9780470175637.ch3
   */

  // Get the correlated color temperature (CCT)
  float t = xy_to_CCT(x, y);

  // xy_to_CCT is valid only in 3000 - 25000 K. We need another model below
  if(t < 3000.f && t > 1667.f)
    t = CCT_reverse_lookup(x, y);

  if(temperature)
    *temperature = t;

  // Convert to CIE 1960 Yuv space
  float xy_ref[2] = { x, y };
  float uv_ref[2];
  xy_to_uv(xy_ref, uv_ref);

  float xy_test[2] = { 0.f };
  float uv_test[2];

  // Compute the test chromaticity from the daylight model
  illuminant_to_xy(DT_ILLUMINANT_D, NULL, NULL, &xy_test[0], &xy_test[1], t, DT_ILLUMINANT_FLUO_LAST, DT_ILLUMINANT_LED_LAST);
  xy_to_uv(xy_test, uv_test);

  // Compute the error between the reference illuminant and the test illuminant derivated from the CCT with daylight model
  const float delta_daylight = hypotf((uv_test[0] - uv_ref[0]), (uv_test[1] - uv_ref[1]));

  // Compute the test chromaticity from the blackbody model
  illuminant_to_xy(DT_ILLUMINANT_BB, NULL, NULL, &xy_test[0], &xy_test[1], t, DT_ILLUMINANT_FLUO_LAST, DT_ILLUMINANT_LED_LAST);
  xy_to_uv(xy_test, uv_test);

  // Compute the error between the reference illuminant and the test illuminant derivated from the CCT with black body model
  const float delta_bb = hypotf((uv_test[0] - uv_ref[0]), (uv_test[1] - uv_ref[1]));

  // Check the error between original and test chromaticity
  if(delta_bb < 0.005f || delta_daylight < 0.005f)
  {
    // Bradford is more accurate for daylight
    if(adaptation) *adaptation = DT_ADAPTATION_LINEAR_BRADFORD;

    if(illuminant)
    {
      if(delta_bb < delta_daylight)
        *illuminant = DT_ILLUMINANT_BB;
      else
        *illuminant = DT_ILLUMINANT_D;
    }
  }
  else
  {
    // error is too big to use a CCT-based model, we fall back to a custom/freestyle chroma selection for the illuminant
    if(illuminant) *illuminant = DT_ILLUMINANT_CUSTOM;

    // CAT16 is less accurate but more robust for non-daylight (produces fewer out-of-gamut colors)
    if(adaptation) *adaptation = DT_ADAPTATION_CAT16;
  }
}


void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
             const void *const restrict ivoid, void *const restrict ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_channelmixer_rbg_data_t *data = (dt_iop_channelmixer_rbg_data_t *)piece->data;
  const struct dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  dt_iop_channelmixer_rgb_gui_data_t *g = (dt_iop_channelmixer_rgb_gui_data_t *)self->gui_data;

  float DT_ALIGNED_ARRAY RGB_to_XYZ[3][4];
  float DT_ALIGNED_ARRAY XYZ_to_RGB[3][4];

  // repack the matrices as flat AVX2-compliant matrice
  if(work_profile)
  {
    // work profile can't be fetched in commit_params since it is not yet initialised
    repack_3x3_to_3xSSE(work_profile->matrix_in, RGB_to_XYZ);
    repack_3x3_to_3xSSE(work_profile->matrix_out, XYZ_to_RGB);
  }

  assert(piece->colors == 4);
  const size_t ch = 4;

  const float *const restrict in = (const float *const restrict)ivoid;
  float *const restrict out = (float *const restrict)ovoid;

  // auto-detect WB upon request
  if(self->dev->gui_attached && g)
  {
    if(data->illuminant_type == DT_ILLUMINANT_DETECT_EDGES || data->illuminant_type == DT_ILLUMINANT_DETECT_SURFACES)
    {
      if(piece->pipe->type == DT_DEV_PIXELPIPE_FULL)
      {
        // detection on full image only
        dt_pthread_mutex_lock(&g->lock);
        auto_detect_WB(in, data->illuminant_type, roi_in->width, roi_in->height, ch, RGB_to_XYZ, g->XYZ);
        dt_pthread_mutex_unlock(&g->lock);
      }

      // passthrough pixels
      dt_simd_memcpy(in, out, roi_in->width * roi_in->height * ch);

      dt_control_log(_("auto-detection of white balance completed"));
      return;
    }
  }

  if(data->illuminant_type == DT_ILLUMINANT_CAMERA)
  {
    // The camera illuminant is a behaviour rather than a preset of values:
    // it uses whatever is in the RAW EXIF. But it depends on what temperature.c is doing
    // and needs to be updated accordingly, to give a consistent result.
    // We initialise the CAT defaults using the temperature coeffs at startup, but if temperature
    // is changed later, we get no notification of the change here, so we can't update the defaults.
    // So we need to re-run the detection at runtime…
    float x, y;
    float custom_wb[4];
    get_white_balance_coeff(self, custom_wb);

    if(find_temperature_from_raw_coeffs(&(self->dev->image_storage), custom_wb, &(x), &(y)))
    {
      // Convert illuminant from xyY to XYZ
      float XYZ[3];
      illuminant_xy_to_XYZ(x, y, XYZ);

      // Convert illuminant from XYZ to Bradford modified LMS
      convert_any_XYZ_to_LMS(XYZ, data->illuminant, data->adaptation);
      data->illuminant[3] = 0.f;
    }
    else
    {
      // just use whatever was defined in commit_params hoping the defaults work…
    }
  }

  // force loop unswitching in a controlled way
  switch(data->adaptation)
  {
    case DT_ADAPTATION_FULL_BRADFORD:
    {
      loop_switch(in, out, roi_out->width, roi_out->height, ch,
                  XYZ_to_RGB, RGB_to_XYZ, data->MIX,
                  data->illuminant, data->saturation, data->lightness, data->grey,
                  data->p, data->gamut, data->clip, data->apply_grey, DT_ADAPTATION_FULL_BRADFORD);
      break;
    }
    case DT_ADAPTATION_LINEAR_BRADFORD:
    {
      loop_switch(in, out, roi_out->width, roi_out->height, ch,
                  XYZ_to_RGB, RGB_to_XYZ, data->MIX,
                  data->illuminant, data->saturation, data->lightness, data->grey,
                  data->p, data->gamut, data->clip, data->apply_grey, DT_ADAPTATION_LINEAR_BRADFORD);
      break;
    }
    case DT_ADAPTATION_CAT16:
    {
      loop_switch(in, out, roi_out->width, roi_out->height, ch,
                  XYZ_to_RGB, RGB_to_XYZ, data->MIX,
                  data->illuminant, data->saturation, data->lightness, data->grey,
                  data->p, data->gamut, data->clip, data->apply_grey, DT_ADAPTATION_CAT16);
      break;
    }
    case DT_ADAPTATION_XYZ:
    {
      loop_switch(in, out, roi_out->width, roi_out->height, ch,
                  XYZ_to_RGB, RGB_to_XYZ, data->MIX,
                  data->illuminant, data->saturation, data->lightness, data->grey,
                  data->p, data->gamut, data->clip, data->apply_grey, DT_ADAPTATION_XYZ);
      break;
    }
    case DT_ADAPTATION_RGB:
    {
      loop_switch(in, out, roi_out->width, roi_out->height, ch,
                  XYZ_to_RGB, RGB_to_XYZ, data->MIX,
                  data->illuminant, data->saturation, data->lightness, data->grey,
                  data->p, data->gamut, data->clip, data->apply_grey, DT_ADAPTATION_RGB);
      break;
    }
    case DT_ADAPTATION_LAST:
    {
      break;
    }
  }
  declare_cat_on_pipe(self, FALSE);
}

static void _develop_ui_pipe_finished_callback(gpointer instance, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_channelmixer_rgb_gui_data_t *g = (dt_iop_channelmixer_rgb_gui_data_t *)self->gui_data;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;

  if(g == NULL) return;
  if(p->illuminant != DT_ILLUMINANT_DETECT_EDGES && p->illuminant != DT_ILLUMINANT_DETECT_SURFACES)
  {
    gui_changed(self, NULL, NULL);
    return;
  }

  dt_pthread_mutex_lock(&g->lock);
  p->x = g->XYZ[0];
  p->y = g->XYZ[1];
  dt_pthread_mutex_unlock(&g->lock);

  check_if_close_to_daylight(p->x, p->y, &p->temperature, &p->illuminant, &p->adaptation);

  ++darktable.gui->reset;

  dt_bauhaus_slider_set(g->temperature, p->temperature);
  dt_bauhaus_combobox_set(g->illuminant, p->illuminant);
  dt_bauhaus_combobox_set(g->adaptation, p->adaptation);

  const float xyY[3] = { p->x, p->y, 1.f };
  float Lch[3];
  dt_xyY_to_Lch(xyY, Lch);
  dt_bauhaus_slider_set(g->illum_x, Lch[2] / M_PI * 180.f);
  dt_bauhaus_slider_set_soft(g->illum_y, Lch[1]);

  update_illuminants(self);
  update_approx_cct(self);
  update_illuminant_color(self);

  --darktable.gui->reset;

  gui_changed(self, NULL, NULL);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)p1;
  dt_iop_channelmixer_rbg_data_t *d = (dt_iop_channelmixer_rbg_data_t *)piece->data;

  float norm_R = 1.0f;
  if(p->normalize_R) norm_R = p->red[0] + p->red[1] + p->red[2];

  float norm_G = 1.0f;
  if(p->normalize_G) norm_G = p->green[0] + p->green[1] + p->green[2];

  float norm_B = 1.0f;
  if(p->normalize_B) norm_B = p->blue[0] + p->blue[1] + p->blue[2];

  float norm_sat = 0.0f;
  if(p->normalize_sat) norm_sat = (p->saturation[0] + p->saturation[1] + p->saturation[2]) / 3.f;

  float norm_light = 0.0f;
  if(p->normalize_light) norm_light = (p->lightness[0] + p->lightness[1] + p->lightness[2]) / 3.f;

  float norm_grey = p->grey[0] + p->grey[1] + p->grey[2];
  d->apply_grey = (p->grey[0] != 0.f) || (p->grey[1] != 0.f) || (p->grey[2] != 0.f);
  if(!p->normalize_grey || norm_grey == 0.f) norm_grey = 1.f;

  for(int i = 0; i < 3; i++)
  {
    d->MIX[0][i] = p->red[i] / norm_R;
    d->MIX[1][i] = p->green[i] / norm_B;
    d->MIX[2][i] = p->blue[i] / norm_G;
    d->saturation[i] = -p->saturation[i] - norm_sat;
    d->lightness[i] = p->lightness[i] - norm_light;
    d->grey[i] = p->grey[i] / norm_grey; // = NaN if (norm_grey == 0.f) but we don't care since (d->apply_grey == FALSE)
  }

  // just in case compiler feels clever and uses SSE 4×1 dot product
  d->saturation[CHANNEL_SIZE - 1] = 0.0f;
  d->lightness[CHANNEL_SIZE - 1] = 0.0f;
  d->grey[CHANNEL_SIZE - 1] = 0.0f;

  d->adaptation = p->adaptation;
  d->clip = p->clip;
  d->gamut = (p->gamut == 0.f) ? p->gamut : 1.f / p->gamut;

  d->illuminant_type = p->illuminant;

  // Convert illuminant from xyY to XYZ
  float XYZ[3];
  illuminant_xy_to_XYZ(p->x, p->y, XYZ);

  // Convert illuminant from XYZ to Bradford modified LMS
  convert_any_XYZ_to_LMS(XYZ, d->illuminant, d->adaptation);
  d->illuminant[3] = 0.f;

  //fprintf(stdout, "illuminant: %i\n", p->illuminant);
  //fprintf(stdout, "x: %f, y: %f\n", x, y);
  //fprintf(stdout, "X: %f - Y: %f - Z: %f\n", XYZ[0], XYZ[1], XYZ[2]);
  //fprintf(stdout, "L: %f - M: %f - S: %f\n", d->illuminant[0], d->illuminant[1], d->illuminant[2]);

  // blue compensation for Bradford transform = (test illuminant blue / reference illuminant blue)^0.0834
  // reference illuminant is hard-set D50 for darktable's pipeline
  // test illuminant is user params
  d->p = powf(0.818155f / d->illuminant[2], 0.0834f);

  declare_cat_on_pipe(self, FALSE);
}


static void update_illuminants(dt_iop_module_t *self)
{
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  dt_iop_channelmixer_rgb_gui_data_t *g = (dt_iop_channelmixer_rgb_gui_data_t *)self->gui_data;

  if(p->adaptation == DT_ADAPTATION_RGB || p->adaptation == DT_ADAPTATION_LAST)
  {
    // user disabled CAT at all, hide everything and exit
    gtk_widget_set_visible(g->illuminant, FALSE);
    gtk_widget_set_visible(g->illum_color, FALSE);
    gtk_widget_set_visible(g->approx_cct, FALSE);
    gtk_widget_set_visible(g->color_picker, FALSE);
    gtk_widget_set_visible(g->temperature, FALSE);
    gtk_widget_set_visible(g->illum_fluo, FALSE);
    gtk_widget_set_visible(g->illum_led, FALSE);
    gtk_widget_set_visible(g->illum_x, FALSE);
    gtk_widget_set_visible(g->illum_y, FALSE);
    return;
  }
  else
  {
    // set everything visible again and carry on
    gtk_widget_set_visible(g->illuminant, TRUE);
    gtk_widget_set_visible(g->illum_color, TRUE);
    gtk_widget_set_visible(g->approx_cct, TRUE);
    gtk_widget_set_visible(g->color_picker, TRUE);
    gtk_widget_set_visible(g->temperature, TRUE);
    gtk_widget_set_visible(g->illum_fluo, TRUE);
    gtk_widget_set_visible(g->illum_led, TRUE);
    gtk_widget_set_visible(g->illum_x, TRUE);
  }

  // Display only the relevant sliders
  switch(p->illuminant)
  {
    case DT_ILLUMINANT_PIPE:
    case DT_ILLUMINANT_A:
    case DT_ILLUMINANT_E:
    {
      gtk_widget_set_visible(g->adaptation, TRUE);
      gtk_widget_set_visible(g->temperature, FALSE);
      gtk_widget_set_visible(g->illum_fluo, FALSE);
      gtk_widget_set_visible(g->illum_led, FALSE);
      gtk_widget_set_visible(g->illum_x, FALSE);
      gtk_widget_set_visible(g->illum_y, FALSE);
      break;
    }
    case DT_ILLUMINANT_D:
    case DT_ILLUMINANT_BB:
    {
      gtk_widget_set_visible(g->adaptation, TRUE);
      gtk_widget_set_visible(g->temperature, TRUE);
      gtk_widget_set_visible(g->illum_fluo, FALSE);
      gtk_widget_set_visible(g->illum_led, FALSE);
      gtk_widget_set_visible(g->illum_x, FALSE);
      gtk_widget_set_visible(g->illum_y, FALSE);
      break;
    }
    case DT_ILLUMINANT_F:
    {
      gtk_widget_set_visible(g->adaptation, TRUE);
      gtk_widget_set_visible(g->temperature, FALSE);
      gtk_widget_set_visible(g->illum_fluo, TRUE);
      gtk_widget_set_visible(g->illum_led, FALSE);
      gtk_widget_set_visible(g->illum_x, FALSE);
      gtk_widget_set_visible(g->illum_y, FALSE);
      break;
    }
    case DT_ILLUMINANT_LED:
    {
      gtk_widget_set_visible(g->adaptation, TRUE);
      gtk_widget_set_visible(g->temperature, FALSE);
      gtk_widget_set_visible(g->illum_fluo, FALSE);
      gtk_widget_set_visible(g->illum_led, TRUE);
      gtk_widget_set_visible(g->illum_x, FALSE);
      gtk_widget_set_visible(g->illum_y, FALSE);
      break;
    }
    case DT_ILLUMINANT_CUSTOM:
    {
      gtk_widget_set_visible(g->adaptation, TRUE);
      gtk_widget_set_visible(g->temperature, FALSE);
      gtk_widget_set_visible(g->illum_fluo, FALSE);
      gtk_widget_set_visible(g->illum_led, FALSE);
      gtk_widget_set_visible(g->illum_x, TRUE);
      gtk_widget_set_visible(g->illum_y, TRUE);
      break;
    }
    case DT_ILLUMINANT_CAMERA:
    {
      gtk_widget_set_visible(g->adaptation, TRUE);
      gtk_widget_set_visible(g->temperature, FALSE);
      gtk_widget_set_visible(g->illum_fluo, FALSE);
      gtk_widget_set_visible(g->illum_led, FALSE);
      gtk_widget_set_visible(g->illum_x, FALSE);
      gtk_widget_set_visible(g->illum_y, FALSE);
      break;
    }
    case DT_ILLUMINANT_DETECT_EDGES:
    case DT_ILLUMINANT_DETECT_SURFACES:
    {
      gtk_widget_set_visible(g->adaptation, FALSE);
      gtk_widget_set_visible(g->temperature, FALSE);
      gtk_widget_set_visible(g->illum_fluo, FALSE);
      gtk_widget_set_visible(g->illum_led, FALSE);
      gtk_widget_set_visible(g->illum_x, FALSE);
      gtk_widget_set_visible(g->illum_y, FALSE);
      break;
    }
    case DT_ILLUMINANT_LAST:
    {
      break;
    }
  }
}

/**
 * DOCUMENTATION
 *
 * The illuminant is stored in params as a set of x and y coordinates, describing its chrominance in xyY color space.
 * xyY is a normalized XYZ space, derivated from the retina cone sensors. By definition, for an illuminant, Y = 1,
 * so we only really care about (x, y).
 *
 * Using (x, y) is a robust and interoperable way to describe an illuminant, since it is all the actual pixel code needs
 * to perform the chromatic adaptation. This (x, y) can be computed in many different ways or taken from databases,
 * and possibly from other software, so storing only the result let us room to improve the computation in the future,
 * without loosing compatibility with older versions.
 *
 * However, it's not a great GUI since x and y are not perceptually scaled. So the `g->illum_x` and `g->illum_y`
 * actually display respectively hue and chroma, in LCh color space, which is designed for illuminants
 * and preceptually spaced. This gives UI controls which effect feels more even to the user.
 *
 * But that makes things a bit tricky, API-wise, since a set of (x, y) depends on a set of (hue, chroma),
 * so they always need to be handled together, but also because the back-and-forth computations
 * Lch <-> xyY need to be done anytime we read or write from/to params from/to GUI.
 *
 * Also, the R, G, B sliders have a background color gradient that shows the actual R, G, B sensors
 * used by the selected chromatic adaptation. Each chromatic adaptation method uses a different RGB space,
 * called LMS in the litterature (but it's only a special-purpose RGB space for all we care here),
 * which primaries are projected to sRGB colors, to be displayed in the GUI, so users may get a feeling
 * of what colors they will get.
 **/

static void update_xy_color(dt_iop_module_t *self)
{
  // update the fill background color of x, y sliders
  dt_iop_channelmixer_rgb_gui_data_t *g = (dt_iop_channelmixer_rgb_gui_data_t *)self->gui_data;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;

  // Get the current values bound of the slider, taking into account the possible soft rescaling
  const float x_min = DT_BAUHAUS_WIDGET(g->illum_x)->data.slider.soft_min;
  const float x_max = DT_BAUHAUS_WIDGET(g->illum_x)->data.slider.soft_max;
  const float y_min = DT_BAUHAUS_WIDGET(g->illum_y)->data.slider.soft_min;
  const float y_max = DT_BAUHAUS_WIDGET(g->illum_y)->data.slider.soft_max;
  const float x_range = x_max - x_min;
  const float y_range = y_max - y_min;

  // Varies x in range around current y param
  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    const float x = x_min + stop * x_range;
    float RGB[4];

    const float Lch[3] = { 100.f, 50.f, x / 180.f * M_PI };
    float xyY[3] = { 0 };
    dt_Lch_to_xyY(Lch, xyY);
    illuminant_xy_to_RGB(xyY[0], xyY[1], RGB);
    dt_bauhaus_slider_set_stop(g->illum_x, stop, RGB[0], RGB[1], RGB[2]);
  }

  // Varies y in range around current x params
  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    const float y = (y_min + stop * y_range) / 2.0f;
    float RGB[4] = { 0 };

    // Find current hue
    float xyY[3] = { p->x, p->y, 1.f };
    float Lch[3] = { 0 };
    dt_xyY_to_Lch(xyY, Lch);

    // Replace chroma by current step
    Lch[0] = 75.f;
    Lch[1] = y;

    // Go back to xyY
    dt_Lch_to_xyY(Lch, xyY);
    illuminant_xy_to_RGB(xyY[0], xyY[1], RGB);
    dt_bauhaus_slider_set_stop(g->illum_y, stop, RGB[0], RGB[1], RGB[2]);
  }

  gtk_widget_queue_draw(self->widget);
}

static void _convert_GUI_colors(dt_iop_channelmixer_rgb_params_t *p,
                                const struct dt_iop_order_iccprofile_info_t *const work_profile, const float LMS[4], float RGB[4])
{
  if(p->adaptation != DT_ADAPTATION_RGB)
  {
    convert_any_LMS_to_RGB(LMS, RGB, p->adaptation);
    // RGB vector is normalized with max(RGB)
  }
  else
  {
    float XYZ[4];
    if(work_profile)
    {
      dt_ioppr_rgb_matrix_to_xyz(LMS, XYZ, work_profile->matrix_in, work_profile->lut_in,
                                  work_profile->unbounded_coeffs_in, work_profile->lutsize,
                                  work_profile->nonlinearlut);
      dt_XYZ_to_Rec709_D65(XYZ, RGB);

      // normalize with hue-preserving method (sort-of) to prevent gamut-clipping in sRGB
      const float max_RGB = fmaxf(fmaxf(RGB[0], RGB[1]), RGB[2]);
      for(size_t c = 0; c < 3; c++) RGB[c] = fmaxf(RGB[c] / max_RGB, 0.f);
    }
    else
    {
      // work profile not available yet - default to grey
      for(size_t c = 0; c < 3; c++) RGB[c] = 0.5f;
    }
  }
}


static void update_R_colors(dt_iop_module_t *self)
{
  // update the fill background color of x, y sliders
  dt_iop_channelmixer_rgb_gui_data_t *g = (dt_iop_channelmixer_rgb_gui_data_t *)self->gui_data;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  const struct dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_current_profile_info(self, self->dev->pipe);

  // scale params if needed
  float RGB[3] = { p->red[0], p->red[1], p->red[2] };

  if(p->normalize_R)
  {
    const float sum = RGB[0] + RGB[1] + RGB[2];
    if(sum != 0.f) for(int c = 0; c < 3; c++) RGB[c] /= sum;
  }

  // Get the current values bound of the slider, taking into account the possible soft rescaling
  const float RR_min = DT_BAUHAUS_WIDGET(g->scale_red_R)->data.slider.soft_min;
  const float RR_max = DT_BAUHAUS_WIDGET(g->scale_red_R)->data.slider.soft_max;
  const float RR_range = RR_max - RR_min;

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    const float RR = RR_min + stop * RR_range;
    const float stop_R = RR + RGB[1] + RGB[2];
    const float LMS[4] = { 0.5f * stop_R, 0.5f, 0.5f };
    float RGB_t[4] = { 0.5f };
    _convert_GUI_colors(p, work_profile, LMS, RGB_t);
    dt_bauhaus_slider_set_stop(g->scale_red_R, stop, RGB_t[0], RGB_t[1], RGB_t[2]);
  }

  const float RG_min = DT_BAUHAUS_WIDGET(g->scale_red_G)->data.slider.soft_min;
  const float RG_max = DT_BAUHAUS_WIDGET(g->scale_red_G)->data.slider.soft_max;
  const float RG_range = RG_max - RG_min;

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    const float RG = RG_min + stop * RG_range;
    const float stop_R = RGB[0] + RG + RGB[2];
    const float LMS[4] = { 0.5f * stop_R, 0.5f, 0.5f };
    float RGB_t[4] = { 0.5f };
    _convert_GUI_colors(p, work_profile, LMS, RGB_t);
    dt_bauhaus_slider_set_stop(g->scale_red_G, stop, RGB_t[0], RGB_t[1], RGB_t[2]);
  }

  const float RB_min = DT_BAUHAUS_WIDGET(g->scale_red_B)->data.slider.soft_min;
  const float RB_max = DT_BAUHAUS_WIDGET(g->scale_red_B)->data.slider.soft_max;
  const float RB_range = RB_max - RB_min;

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    const float RB = RB_min + stop * RB_range;
    const float stop_R = RGB[0] + RGB[1] + RB;
    const float LMS[4] = { 0.5f * stop_R, 0.5f, 0.5f };
    float RGB_t[4] = { 0.5f };
    _convert_GUI_colors(p, work_profile, LMS, RGB_t);
    dt_bauhaus_slider_set_stop(g->scale_red_B, stop, RGB_t[0], RGB_t[1], RGB_t[2]);
  }

  gtk_widget_queue_draw(self->widget);
}


static void update_B_colors(dt_iop_module_t *self)
{
  // update the fill background color of x, y sliders
  dt_iop_channelmixer_rgb_gui_data_t *g = (dt_iop_channelmixer_rgb_gui_data_t *)self->gui_data;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  const struct dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_current_profile_info(self, self->dev->pipe);

  // scale params if needed
  float RGB[3] = { p->blue[0], p->blue[1], p->blue[2] };

  if(p->normalize_B)
  {
    const float sum = RGB[0] + RGB[1] + RGB[2];
    if(sum != 0.f) for(int c = 0; c < 3; c++) RGB[c] /= sum;
  }

  // Get the current values bound of the slider, taking into account the possible soft rescaling
  const float BR_min = DT_BAUHAUS_WIDGET(g->scale_blue_R)->data.slider.soft_min;
  const float BR_max = DT_BAUHAUS_WIDGET(g->scale_blue_R)->data.slider.soft_max;
  const float BR_range = BR_max - BR_min;

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    const float BR = BR_min + stop * BR_range;
    const float stop_B = BR + RGB[1] + RGB[2];
    const float LMS[4] = { 0.5f, 0.5f, 0.5f * stop_B };
    float RGB_t[4] = { 0.5f };
    _convert_GUI_colors(p, work_profile, LMS, RGB_t);
    dt_bauhaus_slider_set_stop(g->scale_blue_R, stop, RGB_t[0], RGB_t[1], RGB_t[2]);
  }

  const float BG_min = DT_BAUHAUS_WIDGET(g->scale_blue_G)->data.slider.soft_min;
  const float BG_max = DT_BAUHAUS_WIDGET(g->scale_blue_G)->data.slider.soft_max;
  const float BG_range = BG_max - BG_min;

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    const float BG = BG_min + stop * BG_range;
    const float stop_B = RGB[0] + BG + RGB[2];
    const float LMS[4] = { 0.5f , 0.5f, 0.5f * stop_B };
    float RGB_t[4] = { 0.5f };
    _convert_GUI_colors(p, work_profile, LMS, RGB_t);
    dt_bauhaus_slider_set_stop(g->scale_blue_G, stop, RGB_t[0], RGB_t[1], RGB_t[2]);
  }

  const float BB_min = DT_BAUHAUS_WIDGET(g->scale_blue_B)->data.slider.soft_min;
  const float BB_max = DT_BAUHAUS_WIDGET(g->scale_blue_B)->data.slider.soft_max;
  const float BB_range = BB_max - BB_min;

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    const float BB = BB_min + stop * BB_range;
    const float stop_B = RGB[0] + RGB[1] + BB;
    const float LMS[4] = { 0.5f, 0.5f, 0.5f * stop_B , 0.f};
    float RGB_t[4] = { 0.5f };
    _convert_GUI_colors(p, work_profile, LMS, RGB_t);
    dt_bauhaus_slider_set_stop(g->scale_blue_B, stop, RGB_t[0], RGB_t[1], RGB_t[2]);
  }

  gtk_widget_queue_draw(self->widget);
}

static void update_G_colors(dt_iop_module_t *self)
{
  // update the fill background color of x, y sliders
  dt_iop_channelmixer_rgb_gui_data_t *g = (dt_iop_channelmixer_rgb_gui_data_t *)self->gui_data;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  const struct dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_current_profile_info(self, self->dev->pipe);

  // scale params if needed
  float RGB[3] = { p->green[0], p->green[1], p->green[2] };

  if(p->normalize_G)
  {
    float sum = RGB[0] + RGB[1] + RGB[2];
    if(sum != 0.f) for(int c = 0; c < 3; c++) RGB[c] /= sum;
  }

  // Get the current values bound of the slider, taking into account the possible soft rescaling
  const float GR_min = DT_BAUHAUS_WIDGET(g->scale_green_R)->data.slider.soft_min;
  const float GR_max = DT_BAUHAUS_WIDGET(g->scale_green_R)->data.slider.soft_max;
  const float GR_range = GR_max - GR_min;

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    const float GR = GR_min + stop * GR_range;
    const float stop_G = GR + RGB[1] + RGB[2];
    const float LMS[4] = { 0.5f , 0.5f * stop_G, 0.5f };
    float RGB_t[4] = { 0.5f };
    _convert_GUI_colors(p, work_profile, LMS, RGB_t);
    dt_bauhaus_slider_set_stop(g->scale_green_R, stop, RGB_t[0], RGB_t[1], RGB_t[2]);
  }

  const float GG_min = DT_BAUHAUS_WIDGET(g->scale_green_G)->data.slider.soft_min;
  const float GG_max = DT_BAUHAUS_WIDGET(g->scale_green_G)->data.slider.soft_max;
  const float GG_range = GG_max - GG_min;

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    const float GG = GG_min + stop * GG_range;
    const float stop_G = RGB[0] + GG + RGB[2];
    const float LMS[4] = { 0.5f, 0.5f * stop_G, 0.5f };
    float RGB_t[4] = { 0.5f };
    _convert_GUI_colors(p, work_profile, LMS, RGB_t);
    dt_bauhaus_slider_set_stop(g->scale_green_G, stop, RGB_t[0], RGB_t[1], RGB_t[2]);
  }

  const float GB_min = DT_BAUHAUS_WIDGET(g->scale_green_B)->data.slider.soft_min;
  const float GB_max = DT_BAUHAUS_WIDGET(g->scale_green_B)->data.slider.soft_max;
  const float GB_range = GB_max - GB_min;

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    const float GB = GB_min + stop * GB_range;
    const float stop_G = RGB[0] + RGB[1] + GB;
    const float LMS[4] = { 0.5f, 0.5f * stop_G , 0.5f};
    float RGB_t[4] = { 0.5f };
    _convert_GUI_colors(p, work_profile, LMS, RGB_t);
    dt_bauhaus_slider_set_stop(g->scale_green_B, stop, RGB_t[0], RGB_t[1], RGB_t[2]);
  }

  gtk_widget_queue_draw(self->widget);
}


static void update_illuminant_color(dt_iop_module_t *self)
{
  dt_iop_channelmixer_rgb_gui_data_t *g = (dt_iop_channelmixer_rgb_gui_data_t *)self->gui_data;
  gtk_widget_queue_draw(g->illum_color);
  update_xy_color(self);
}

static gboolean illuminant_color_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;

  // Init
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  // Margins
  const double INNER_PADDING = 4.0;
  const float margin = 2. * DT_PIXEL_APPLY_DPI(darktable.bauhaus->line_space);
  width -= 2* INNER_PADDING;
  height -= 2 * margin;

  // Paint illuminant color - we need to recompute it in full in case camera RAW is choosen
  float x = p->x;
  float y = p->y;
  float RGB[4] = { 0 };
  float custom_wb[4];
  get_white_balance_coeff(self, custom_wb);
  illuminant_to_xy(p->illuminant, &(self->dev->image_storage), custom_wb,
                   &x, &y, p->temperature, p->illum_fluo, p->illum_led);
  illuminant_xy_to_RGB(x, y, RGB);
  cairo_set_source_rgb(cr, RGB[0], RGB[1], RGB[2]);
  cairo_rectangle(cr, INNER_PADDING, margin, width, height);
  cairo_fill(cr);

  // Clean
  cairo_stroke(cr);
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

static void update_approx_cct(dt_iop_module_t *self)
{
  dt_iop_channelmixer_rgb_gui_data_t *g = (dt_iop_channelmixer_rgb_gui_data_t *)self->gui_data;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;

  float x = p->x;
  float y = p->y;
  float custom_wb[4];
  get_white_balance_coeff(self, custom_wb);
  illuminant_to_xy(p->illuminant, &(self->dev->image_storage), custom_wb, &x, &y, p->temperature, p->illum_fluo, p->illum_led);

  dt_illuminant_t test_illuminant;
  float t = 5000.f;
  check_if_close_to_daylight(x, y, &t, &test_illuminant, NULL);

  gchar *str;
  if(t > 1667.f && t < 25000.f)
  {
    if(test_illuminant == DT_ILLUMINANT_D)
    {
      str = g_strdup_printf(_("CCT: %.0f K (daylight)"), t);
      gtk_widget_set_tooltip_text(GTK_WIDGET(g->approx_cct),
                                  _("approximated correlated color temperature.\n"
                                    "this illuminant can be accurately modeled by a daylight spectrum,\n"
                                    "so its temperature is relevant and meaningful with a D illuminant."));
    }
    else if(test_illuminant == DT_ILLUMINANT_BB)
    {
      str = g_strdup_printf(_("CCT: %.0f K (black body)"), t);
      gtk_widget_set_tooltip_text(GTK_WIDGET(g->approx_cct),
                                  _("approximated correlated color temperature.\n"
                                    "this illuminant can be accurately modeled by a black body spectrum,\n"
                                    "so its temperature is relevant and meaningful with a Planckian illuminant."));
    }
    else
    {
      str = g_strdup_printf(_("CCT: %.0f K (invalid)"), t);
      gtk_widget_set_tooltip_text(GTK_WIDGET(g->approx_cct),
                                  _("approximated correlated color temperature.\n"
                                    "this illuminant cannot be accurately modeled by a daylight or black body spectrum,\n"
                                    "so its temperature is not relevant and meaningful and you need to use a custom illuminant."));
    }
  }
  else
  {
    str = g_strdup_printf(_("CCT: undefined"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(g->approx_cct),
                                _("the approximated correlated color temperature\n"
                                  "cannot be computed at all so you need to use a custom illuminant."));
  }
  gtk_label_set_text(GTK_LABEL(g->approx_cct), str);
}


static void illum_xy_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  dt_iop_channelmixer_rgb_gui_data_t *g = (dt_iop_channelmixer_rgb_gui_data_t *)self->gui_data;

  float Lch[3] = { 0 };
  Lch[0] = 100.f;
  Lch[2] = dt_bauhaus_slider_get(g->illum_x) / 180. * M_PI;
  Lch[1] = dt_bauhaus_slider_get(g->illum_y);

  float xyY[3] = { 0 };
  dt_Lch_to_xyY(Lch, xyY);
  p->x = xyY[0];
  p->y = xyY[1];

  float t = xy_to_CCT(p->x, p->y);
  // xy_to_CCT is valid only above 3000 K
  if(t < 3000.f) t = CCT_reverse_lookup(p->x, p->y);
  p->temperature = t;

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->temperature, p->temperature);
  update_approx_cct(self);
  update_illuminant_color(self);
  --darktable.gui->reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_channelmixer_rbg_data_t));
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  self->dev->proxy.chroma_adaptation = NULL;
  free(piece->data);
  piece->data = NULL;
}

void gui_reset(dt_iop_module_t *self)
{
  dt_iop_color_picker_reset(self, TRUE);
  gui_changed(self, NULL, NULL);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_channelmixer_rgb_gui_data_t *g = (dt_iop_channelmixer_rgb_gui_data_t *)self->gui_data;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)module->params;

  dt_iop_color_picker_reset(self, TRUE);

  dt_bauhaus_combobox_set(g->illuminant, p->illuminant);
  dt_bauhaus_combobox_set(g->illum_fluo, p->illum_fluo);
  dt_bauhaus_combobox_set(g->illum_led, p->illum_led);
  dt_bauhaus_slider_set(g->temperature, p->temperature);
  dt_bauhaus_slider_set_soft(g->gamut, p->gamut);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->clip), p->clip);

  float xyY[3] = { p->x, p->y, 1.f };
  float Lch[3] = { 0 };
  dt_xyY_to_Lch(xyY, Lch);

  dt_bauhaus_slider_set(g->illum_x, Lch[2] / M_PI * 180.f);
  dt_bauhaus_slider_set_soft(g->illum_y, Lch[1]);

  dt_bauhaus_combobox_set(g->adaptation, p->adaptation);

  dt_bauhaus_slider_set_soft(g->scale_red_R, p->red[0]);
  dt_bauhaus_slider_set_soft(g->scale_red_G, p->red[1]);
  dt_bauhaus_slider_set_soft(g->scale_red_B, p->red[2]);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->normalize_R), p->normalize_R);

  dt_bauhaus_slider_set_soft(g->scale_green_R, p->green[0]);
  dt_bauhaus_slider_set_soft(g->scale_green_G, p->green[1]);
  dt_bauhaus_slider_set_soft(g->scale_green_B, p->green[2]);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->normalize_G), p->normalize_G);

  dt_bauhaus_slider_set_soft(g->scale_blue_R, p->blue[0]);
  dt_bauhaus_slider_set_soft(g->scale_blue_G, p->blue[1]);
  dt_bauhaus_slider_set_soft(g->scale_blue_B, p->blue[2]);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->normalize_B), p->normalize_B);

  dt_bauhaus_slider_set_soft(g->scale_saturation_R, p->saturation[0]);
  dt_bauhaus_slider_set_soft(g->scale_saturation_G, p->saturation[1]);
  dt_bauhaus_slider_set_soft(g->scale_saturation_B, p->saturation[2]);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->normalize_sat), p->normalize_sat);

  dt_bauhaus_slider_set_soft(g->scale_lightness_R, p->lightness[0]);
  dt_bauhaus_slider_set_soft(g->scale_lightness_G, p->lightness[1]);
  dt_bauhaus_slider_set_soft(g->scale_lightness_B, p->lightness[2]);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->normalize_light), p->normalize_light);

  dt_bauhaus_slider_set_soft(g->scale_grey_R, p->grey[0]);
  dt_bauhaus_slider_set_soft(g->scale_grey_G, p->grey[1]);
  dt_bauhaus_slider_set_soft(g->scale_grey_B, p->grey[2]);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->normalize_grey), p->normalize_grey);

  gui_changed(self, NULL, NULL);

}

void init(dt_iop_module_t *module)
{
  dt_iop_default_init(module);

  dt_iop_channelmixer_rgb_params_t *d = (dt_iop_channelmixer_rgb_params_t *)module->default_params;
  d->red[0] = d->green[1] = d->blue[2] = 1.0;
}

void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_channelmixer_rgb_params_t *d = (dt_iop_channelmixer_rgb_params_t *)module->default_params;

  d->x = module->get_f("x")->Float.Default;
  d->y = module->get_f("y")->Float.Default;
  d->temperature = module->get_f("temperature")->Float.Default;
  d->illuminant = module->get_f("illuminant")->Enum.Default;
  d->adaptation = module->get_f("adaptation")->Enum.Default;

  gchar *workflow = dt_conf_get_string("plugins/darkroom/chromatic-adaptation");
  const gboolean is_modern = strcmp(workflow, "modern") == 0;
  g_free(workflow);

  // note that if there is already an instance of this module with an
  // adaptation set we default to RGB (none) in this instance.
  // try to register the CAT here
  declare_cat_on_pipe(module, is_modern);
  // check if we could register
  gboolean CAT_already_applied = !is_module_cat_on_pipe(module);
  module->default_enabled = FALSE;


  const dt_image_t *img = &module->dev->image_storage;

  float custom_wb[4];
  if(!CAT_already_applied
     && is_modern
     && !get_white_balance_coeff(module, custom_wb))
  {
    // if workflow = modern and we find WB coeffs, take care of white balance here
    if(find_temperature_from_raw_coeffs(img, custom_wb, &(d->x), &(d->y)))
      d->illuminant = DT_ILLUMINANT_CAMERA;

    check_if_close_to_daylight(d->x, d->y, &(d->temperature), &(d->illuminant), &(d->adaptation));
  }
  else
  {
    // otherwise, simple channel mixer
    d->illuminant = DT_ILLUMINANT_PIPE;
    d->adaptation = DT_ADAPTATION_RGB;
  }

  dt_iop_channelmixer_rgb_gui_data_t *g = (dt_iop_channelmixer_rgb_gui_data_t *)module->gui_data;
  if(g)
  {
    const float xyY[3] = { d->x, d->y, 1.f };
    float Lch[3] = { 0 };
    dt_xyY_to_Lch(xyY, Lch);

    dt_bauhaus_slider_set_default(g->illum_x, Lch[2] / M_PI * 180.f);
    dt_bauhaus_slider_set_default(g->illum_y, Lch[1]);
    dt_bauhaus_slider_set_default(g->temperature, d->temperature);
    dt_bauhaus_combobox_set_default(g->illuminant, d->illuminant);
    dt_bauhaus_combobox_set_default(g->adaptation, d->adaptation);

    if(dt_image_is_matrix_correction_supported(img))
    {
      if(dt_bauhaus_combobox_length(g->illuminant) < DT_ILLUMINANT_CAMERA + 1)
        dt_bauhaus_combobox_add(g->illuminant, _("as shot in camera"));
    }
    else
      dt_bauhaus_combobox_remove_at(g->illuminant, DT_ILLUMINANT_CAMERA);

    gui_changed(module, NULL, NULL);
  }
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  dt_iop_channelmixer_rgb_gui_data_t *g = (dt_iop_channelmixer_rgb_gui_data_t *)self->gui_data;

  if(w == g->illuminant)
  {
    if(p->illuminant == DT_ILLUMINANT_CAMERA)
    {
      // Get camera WB and update illuminant
      float custom_wb[4];
      get_white_balance_coeff(self, custom_wb);
      const int found = find_temperature_from_raw_coeffs(&(self->dev->image_storage), custom_wb, &(p->x), &(p->y));
      check_if_close_to_daylight(p->x, p->y, &(p->temperature), NULL, NULL);

      if(found)
        dt_control_log(_("white balance successfuly extracted from raw image"));
    }
    else if(p->illuminant == DT_ILLUMINANT_DETECT_EDGES
            || p->illuminant == DT_ILLUMINANT_DETECT_SURFACES)
    {
      // We need to recompute only the full preview
      dt_control_log(_("auto-detection of white balance started…"));
    }

    // Put current illuminant x y directly in user params x and y in case user wants to
    // take over manually in custom mode
    float custom_wb[4] = { 1.f };
    get_white_balance_coeff(self, custom_wb);
    gboolean changed = illuminant_to_xy(p->illuminant, &(self->dev->image_storage), custom_wb, &(p->x), &(p->y),
                                        p->temperature, p->illum_fluo, p->illum_led);
    if(changed)
    {
      if(p->illuminant != DT_ILLUMINANT_D && p->illuminant != DT_ILLUMINANT_BB)
      {
        // Put current illuminant closest CCT directly in user params temperature in case user wants to
        // switch to a temperature-based mode
        check_if_close_to_daylight(p->x, p->y, &(p->temperature), NULL, NULL);
      }

      float xyY[3] = { p->x, p->y, 1.f };
      float Lch[3];
      dt_xyY_to_Lch(xyY, Lch);

      ++darktable.gui->reset;
      dt_bauhaus_slider_set(g->illum_x, Lch[2] / M_PI * 180.f);
      dt_bauhaus_slider_set_soft(g->illum_y, Lch[1]);
      dt_bauhaus_slider_set(g->temperature, p->temperature);
      --darktable.gui->reset;
    }

  }

  if(w == g->temperature)
  {
    // Commit temperature to illuminant x, y
    illuminant_to_xy(p->illuminant, NULL, NULL, &(p->x), &(p->y), p->temperature, p->illum_fluo, p->illum_led);
  }

  ++darktable.gui->reset;

  if(!w || w == g->illuminant || w == g->illum_fluo || w == g->illum_led || g->temperature)
  {
    update_illuminants(self);
    update_approx_cct(self);
    update_illuminant_color(self);
  }
  if(!w || w == g->scale_red_R   || w == g->scale_red_G   || w == g->scale_red_B   || w == g->normalize_R)
    update_R_colors(self);
  if(!w || w == g->scale_green_R || w == g->scale_green_G || w == g->scale_green_B || w == g->normalize_G)
    update_G_colors(self);
  if(!w || w == g->scale_blue_R  || w == g->scale_blue_G  || w == g->scale_blue_B  || w == g->normalize_B)
    update_B_colors(self);

  // if grey channel is used and norm = 0 and normalization = ON, we are going to have a division by zero
  // in commit_param, we avoid dividing by zero automatically, but user needs a notification
  if((p->grey[0] != 0.f) || (p->grey[1] != 0.f) || (p->grey[2] != 0.f))
    if((p->grey[0] + p->grey[1] + p->grey[2] == 0.f) && p->normalize_grey)
      dt_control_log(_("color calibration: the sum of the grey channel parameters is zero, normalization will be disabled."));

  if(w == g->adaptation)
  {
    update_illuminants(self);
    update_R_colors(self);
    update_G_colors(self);
    update_B_colors(self);
  }

  if(self->enabled && !(p->illuminant == DT_ILLUMINANT_PIPE || p->adaptation == DT_ADAPTATION_RGB))
  {
    // this module instance is doing chromatic adaptation
    dt_iop_order_entry_t *CAT_instance = self->dev->proxy.chroma_adaptation;
    dt_iop_order_entry_t *current_instance
        = dt_ioppr_get_iop_order_entry(self->dev->iop_order_list, "channelmixerrgb", self->multi_priority);

    if(CAT_instance && CAT_instance->o.iop_order != current_instance->o.iop_order)
    {
      // our second biggest problem : another channelmixerrgb instance is doing CAT earlier in the pipe
      dt_iop_set_module_in_trouble(self, TRUE);
      char *wmes = dt_iop_warning_message(_("double CAT applied"));
      gtk_label_set_text(GTK_LABEL(g->warning_label), wmes);
      g_free(wmes);
      gtk_widget_set_tooltip_text(GTK_WIDGET(g->warning_label),
                                  _("you have 2 instances or more of color calibration,\n"
                                    "all performing chromatic adaptation.\n"
                                    "this can lead to inconsistencies, unless you\n"
                                    "use them with masks or know what you are doing."));
      gtk_widget_set_visible(GTK_WIDGET(g->warning_label), TRUE);
    }
    else if(!self->dev->proxy.wb_is_D65)
    {
      // our first and biggest problem : white balance module is being clever with WB coeffs
      dt_iop_set_module_in_trouble(self, TRUE);
      char *wmes = dt_iop_warning_message(_("white balance module error"));
      gtk_label_set_text(GTK_LABEL(g->warning_label), wmes);
      g_free(wmes);
      gtk_widget_set_tooltip_text(GTK_WIDGET(g->warning_label),
                                  _("the white balance module is not using the camera\n"
                                    "reference illuminant, which will cause issues here\n"
                                    "with chromatic adaptation. either set it to reference\n"
                                    "or disable chromatic adaptation here."));
      gtk_widget_set_visible(GTK_WIDGET(g->warning_label), TRUE);
    }
    else
    {
      dt_iop_set_module_in_trouble(self, FALSE);
      gtk_label_set_text(GTK_LABEL(g->warning_label), "");
      gtk_widget_set_tooltip_text(GTK_WIDGET(g->warning_label), "");
      gtk_widget_set_visible(GTK_WIDGET(g->warning_label), FALSE);
    }
  }
  else
  {
    dt_iop_set_module_in_trouble(self, FALSE);
    gtk_label_set_text(GTK_LABEL(g->warning_label), "");
    gtk_widget_set_tooltip_text(GTK_WIDGET(g->warning_label), "");
    gtk_widget_set_visible(GTK_WIDGET(g->warning_label), FALSE);
  }

  --darktable.gui->reset;

}


void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  gui_changed(self, NULL, NULL);
}


void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece)
{
  if(darktable.gui->reset) return;

  dt_iop_channelmixer_rgb_gui_data_t *g = (dt_iop_channelmixer_rgb_gui_data_t *)self->gui_data;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;

  // capture gui color picked event.
  if(self->picked_color_max[0] < self->picked_color_min[0]) return;
  const float *RGB = self->picked_color;

  // Get work profile
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);
  if(work_profile == NULL) return;

  // repack the matrices as flat AVX2-compliant matrice
  float DT_ALIGNED_ARRAY RGB_to_XYZ[3][4];
  repack_3x3_to_3xSSE(work_profile->matrix_in, RGB_to_XYZ);

  // Convert to XYZ
  float XYZ[4] = { 0 };
  dot_product(RGB, RGB_to_XYZ, XYZ);

  // Convert to xyY
  const float sum = fmaxf(XYZ[0] + XYZ[1] + XYZ[2], 1e-6f);
  XYZ[0] /= sum;   // x
  XYZ[2] = XYZ[1]; // Y
  XYZ[1] /= sum;   // y

  p->x = XYZ[0];
  p->y = XYZ[1];

  ++darktable.gui->reset;

  check_if_close_to_daylight(p->x, p->y, &p->temperature, &p->illuminant, &p->adaptation);

  dt_bauhaus_slider_set(g->temperature, p->temperature);
  dt_bauhaus_combobox_set(g->illuminant, p->illuminant);
  dt_bauhaus_combobox_set(g->adaptation, p->adaptation);

  const float xyY[3] = { p->x, p->y, 1.f };
  float Lch[3] = { 0 };
  dt_xyY_to_Lch(xyY, Lch);
  dt_bauhaus_slider_set(g->illum_x, Lch[2] / M_PI * 180.f);
  dt_bauhaus_slider_set_soft(g->illum_y, Lch[1]);

  update_illuminants(self);
  update_approx_cct(self);
  update_illuminant_color(self);

  --darktable.gui->reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_channelmixer_rgb_gui_data_t *g = IOP_GUI_ALLOC(channelmixer_rgb);

  g->XYZ[0] = NAN;
  dt_pthread_mutex_init(&g->lock, NULL);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED,
                            G_CALLBACK(_develop_ui_pipe_finished_callback), self);

  // Init GTK notebook
  g->notebook = GTK_NOTEBOOK(gtk_notebook_new());

  // Page CAT
  self->widget = dt_ui_notebook_page(g->notebook, _("CAT"), _("chromatic adaptation transform"));

  g->warning_label = dt_ui_label_new("");
  gtk_label_set_line_wrap(GTK_LABEL(g->warning_label), TRUE);
  gtk_box_pack_start(GTK_BOX(self->widget), g->warning_label, FALSE, FALSE, 4);

  g->adaptation = dt_bauhaus_combobox_from_params(self, N_("adaptation"));
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->adaptation),
                              _("choose the method to adapt the illuminant\n"
                                "and the colorspace in which the module works: \n"
                                "• Linear Bradford (1985) is more accurate for illuminants close to daylight\n"
                                "but produces out-of-gamut colors for difficult illuminants.\n"
                                "• CAT16 (2016) is more robust to avoid imaginary colours\n"
                                "while working with large gamut or saturated cyan and purple.\n"
                                "• Non-linear Bradford (1985) is the original Bradford,\n"
                                "it can produce better results than the linear version, but is unreliable.\n"
                                "• XYZ is a simple scaling in XYZ space. It is not recommended in general.\n"
                                "• none disables any adaptation and uses pipeline working RGB."));

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  g->approx_cct = dt_ui_label_new("CCT:");
  gtk_box_pack_start(GTK_BOX(hbox), g->approx_cct, FALSE, FALSE, 0);

  g->illum_color = GTK_WIDGET(gtk_drawing_area_new());
  gtk_widget_set_size_request(g->illum_color, 2 * DT_PIXEL_APPLY_DPI(darktable.bauhaus->quad_width), -1);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->illum_color),
                              _("this is the color of the scene illuminant before chromatic adaptation\n"
                                "this color will be turned into pure white by the adaptation."));

  g_signal_connect(G_OBJECT(g->illum_color), "draw", G_CALLBACK(illuminant_color_draw), self);
  gtk_box_pack_start(GTK_BOX(hbox), g->illum_color, TRUE, TRUE, 0);

  g->color_picker = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, hbox);
  gtk_widget_set_tooltip_text(g->color_picker, _("set white balance to detected from area"));

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), FALSE, FALSE, 0);

  g->illuminant = dt_bauhaus_combobox_from_params(self, N_("illuminant"));

  g->illum_fluo = dt_bauhaus_combobox_from_params(self, "illum_fluo");

  g->illum_led = dt_bauhaus_combobox_from_params(self, "illum_led");

  g->temperature = dt_bauhaus_slider_from_params(self, N_("temperature"));
  dt_bauhaus_slider_set_step(g->temperature, 50.);
  dt_bauhaus_slider_set_digits(g->temperature, 0);
  dt_bauhaus_slider_set_format(g->temperature, "%.0f K");

  const float max_temp = dt_bauhaus_slider_get_hard_max(g->temperature);
  const float min_temp = dt_bauhaus_slider_get_hard_min(g->temperature);

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    const float t = min_temp + stop * (max_temp - min_temp);
    float RGB[4] = { 0 };
    illuminant_CCT_to_RGB(t, RGB);
    dt_bauhaus_slider_set_stop(g->temperature, stop, RGB[0], RGB[1], RGB[2]);
  }

  g->illum_x = dt_bauhaus_slider_new_with_range_and_feedback(self, 0., 360., 0.5, 0, 1, 0);
  dt_bauhaus_widget_set_label(g->illum_x, NULL, _("hue"));
  dt_bauhaus_slider_set_format(g->illum_x, "%.1f °");
  g_signal_connect(G_OBJECT(g->illum_x), "value-changed", G_CALLBACK(illum_xy_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->illum_x), FALSE, FALSE, 0);

  g->illum_y = dt_bauhaus_slider_new_with_range(self, 0., 100., 0.5, 0, 1);
  dt_bauhaus_widget_set_label(g->illum_y, NULL, _("chroma"));
  dt_bauhaus_slider_set_format(g->illum_y, "%.1f %%");
  dt_bauhaus_slider_set_hard_max(g->illum_y, 300.f);
  g_signal_connect(G_OBJECT(g->illum_y), "value-changed", G_CALLBACK(illum_xy_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->illum_y), FALSE, FALSE, 0);

  g->gamut = dt_bauhaus_slider_from_params(self, "gamut");
  dt_bauhaus_slider_set_hard_max(g->gamut, 12.f);

  g->clip = dt_bauhaus_toggle_from_params(self, "clip");

  GtkWidget *first, *second, *third;
#define NOTEBOOK_PAGE(var, short, label, tooltip, section, swap)              \
  self->widget = dt_ui_notebook_page(g->notebook, _(label), _(tooltip));      \
                                                                              \
  first = dt_bauhaus_slider_from_params(self, swap ? #var "[2]" : #var "[0]");\
  dt_bauhaus_slider_set_step(first, 0.005);                                   \
  dt_bauhaus_slider_set_digits(first, 3);                                     \
  dt_bauhaus_slider_set_hard_min(first, -2.f);                                \
  dt_bauhaus_slider_set_hard_max(first, 2.f);                                 \
  dt_bauhaus_widget_set_label(first, section, _("input red"));                \
                                                                              \
  second = dt_bauhaus_slider_from_params(self, #var "[1]");                   \
  dt_bauhaus_slider_set_step(second, 0.005);                                  \
  dt_bauhaus_slider_set_digits(second, 3);                                    \
  dt_bauhaus_slider_set_hard_min(second, -2.f);                               \
  dt_bauhaus_slider_set_hard_max(second, 2.f);                                \
  dt_bauhaus_widget_set_label(second, section, _("input green"));             \
                                                                              \
  third = dt_bauhaus_slider_from_params(self, swap ? #var "[0]" : #var "[2]");\
  dt_bauhaus_slider_set_step(third, 0.005);                                   \
  dt_bauhaus_slider_set_digits(third, 3);                                     \
  dt_bauhaus_slider_set_hard_min(third, -2.f);                                \
  dt_bauhaus_slider_set_hard_max(third, 2.f);                                 \
  dt_bauhaus_widget_set_label(third, section, _("input blue"));               \
                                                                              \
  g->scale_##var##_R = swap ? third : first;                                  \
  g->scale_##var##_G = second;                                                \
  g->scale_##var##_B = swap ? first : third;                                  \
                                                                              \
  g->normalize_##short = dt_bauhaus_toggle_from_params(self, "normalize_" #short);

  NOTEBOOK_PAGE(red, R, N_("R"), N_("red"), N_("red"), FALSE)
  NOTEBOOK_PAGE(green, G, N_("G"), N_("green"), N_("green"), FALSE)
  NOTEBOOK_PAGE(blue, B, N_("B"), N_("blue"), N_("blue"), FALSE)
  NOTEBOOK_PAGE(saturation, sat, N_("colorfulness"), N_("colorfulness"), N_("colorfulness"), TRUE)
  NOTEBOOK_PAGE(lightness, light, N_("brightness"), N_("brightness"), N_("brightness"), FALSE)
  NOTEBOOK_PAGE(grey, grey, N_("grey"), N_("grey"), N_("grey"), FALSE)

  self->widget = GTK_WIDGET(g->notebook);
  const int active_page = dt_conf_get_int("plugins/darkroom/channelmixerrgb/gui_page");
  gtk_widget_show(gtk_notebook_get_nth_page(g->notebook, active_page));
  gtk_notebook_set_current_page(g->notebook, active_page);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals,
                                     G_CALLBACK(_develop_ui_pipe_finished_callback), self);

  dt_iop_channelmixer_rgb_gui_data_t *g = (dt_iop_channelmixer_rgb_gui_data_t *)self->gui_data;
  dt_conf_set_int("plugins/darkroom/channelmixerrgb/gui_page", gtk_notebook_get_current_page (g->notebook));
  dt_pthread_mutex_destroy(&g->lock);

  IOP_GUI_FREE;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
