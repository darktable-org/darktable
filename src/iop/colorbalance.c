/*
http://www.youtube.com/watch?v=JVoUgR6bhBc
 */

/*
    This file is part of darktable,
    Copyright (C) 2013-2022 darktable developers.

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

// these are not in a state to be useful. but they look nice. too bad i couldn't map the enhanced mode with
// negative values to the wheels :(
//#define SHOW_COLOR_WHEELS

DT_MODULE_INTROSPECTION(3, dt_iop_colorbalance_params_t)

/*

  Meaning of the values:
   0 --> 100%
  -1 -->   0%
   1 --> 200%
 */

typedef enum dt_iop_colorbalance_mode_t
{
  LIFT_GAMMA_GAIN = 0,    // $DESCRIPTION: "lift, gamma, gain (ProPhoto RGB)"
  SLOPE_OFFSET_POWER = 1, // $DESCRIPTION: "slope, offset, power (ProPhoto RGB)"
  LEGACY = 2              // $DESCRIPTION: "lift, gamma, gain (sRGB)"
} dt_iop_colorbalance_mode_t;

typedef enum _colorbalance_channel_t
{
  CHANNEL_FACTOR = 0,
  CHANNEL_RED,
  CHANNEL_GREEN,
  CHANNEL_BLUE,
  CHANNEL_SIZE
} _colorbalance_channel_t;

typedef enum _colorbalance_levels_t
{
  LIFT = 0,
  GAMMA,
  GAIN,
  LEVELS
} _colorbalance_levels_t;

typedef enum _controls_t
{
  HSL,
  RGBL,
  BOTH
} _controls_t;

typedef enum _colorbalance_patch_t
{
  INVALID,
  USER_SELECTED,
  AUTO_SELECTED
} _colorbalance_patch_t;

typedef struct dt_iop_colorbalance_params_t
{
  dt_iop_colorbalance_mode_t mode; // $DEFAULT: SLOPE_OFFSET_POWER
  float lift[CHANNEL_SIZE], gamma[CHANNEL_SIZE], gain[CHANNEL_SIZE]; // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.0
  float saturation;     // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "input saturation"
  float contrast;       // $MIN: 0.01 $MAX: 1.99 $DEFAULT: 1.0
  float grey;           // $MIN: 0.1 $MAX: 100.0 $DEFAULT: 18.0 $DESCRIPTION: "contrast fulcrum"
  float saturation_out; // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "output saturation"
} dt_iop_colorbalance_params_t;

typedef struct dt_iop_colorbalance_gui_data_t
{
  GtkWidget *master_box;
  GtkWidget *main_label;
  GtkWidget *main_box;
  GtkWidget *blocks[3];
  GtkWidget *optimizer_box;
  GtkWidget *mode;
  GtkWidget *controls;
  GtkWidget *hue_lift, *hue_gamma, *hue_gain;
  GtkWidget *sat_lift, *sat_gamma, *sat_gain;
  GtkWidget *lift_r, *lift_g, *lift_b, *lift_factor;
  GtkWidget *gamma_r, *gamma_g, *gamma_b, *gamma_factor;
  GtkWidget *gain_r, *gain_g, *gain_b, *gain_factor;
  GtkWidget *saturation, *contrast, *grey, *saturation_out;
  GtkWidget *auto_luma;
  GtkWidget *auto_color;
  float color_patches_lift[3];
  float color_patches_gamma[3];
  float color_patches_gain[3];
  _colorbalance_patch_t color_patches_flags[LEVELS];
  float luma_patches[LEVELS];
  _colorbalance_patch_t luma_patches_flags[LEVELS];
} dt_iop_colorbalance_gui_data_t;

typedef struct dt_iop_colorbalance_data_t
{
  dt_iop_colorbalance_mode_t mode;
  float lift[CHANNEL_SIZE], gamma[CHANNEL_SIZE], gain[CHANNEL_SIZE];
  float saturation, contrast, grey, saturation_out;
} dt_iop_colorbalance_data_t;

typedef struct dt_iop_colorbalance_global_data_t
{
  int kernel_colorbalance;
  int kernel_colorbalance_cdl;
  int kernel_colorbalance_lgg;
} dt_iop_colorbalance_global_data_t;


const char *name()
{
  return _("color balance");
}

const char *aliases()
{
  return _("lift gamma gain|cdl|color grading|contrast|saturation|hue");
}

const char **description(struct dt_iop_module_t *self)
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
  return IOP_CS_LAB;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params,
                  const int new_version)
{
  if(old_version == 1 && new_version == 3)
  {
    typedef struct dt_iop_colorbalance_params_v1_t
    {
      float lift[CHANNEL_SIZE], gamma[CHANNEL_SIZE], gain[CHANNEL_SIZE];
    } dt_iop_colorbalance_params_v1_t;

    dt_iop_colorbalance_params_v1_t *o = (dt_iop_colorbalance_params_v1_t *)old_params;
    dt_iop_colorbalance_params_t *n = (dt_iop_colorbalance_params_t *)new_params;
    dt_iop_colorbalance_params_t *d = (dt_iop_colorbalance_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    for(int i = 0; i < CHANNEL_SIZE; i++)
    {
      n->lift[i] = o->lift[i];
      n->gamma[i] = o->gamma[i];
      n->gain[i] = o->gain[i];
    }
    n->mode = LEGACY;
    return 0;
  }

  if(old_version == 2 && new_version == 3)
  {
    typedef struct dt_iop_colorbalance_params_v2_t
    {
      dt_iop_colorbalance_mode_t mode;
      float lift[CHANNEL_SIZE], gamma[CHANNEL_SIZE], gain[CHANNEL_SIZE];
      float saturation, contrast, grey;
    } dt_iop_colorbalance_params_v2_t;

    dt_iop_colorbalance_params_v2_t *o = (dt_iop_colorbalance_params_v2_t *)old_params;
    dt_iop_colorbalance_params_t *n = (dt_iop_colorbalance_params_t *)new_params;
    dt_iop_colorbalance_params_t *d = (dt_iop_colorbalance_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    for(int i = 0; i < CHANNEL_SIZE; i++)
    {
      n->lift[i] = o->lift[i];
      n->gamma[i] = o->gamma[i];
      n->gain[i] = o->gain[i];
    }
    n->mode = o->mode;
    n->contrast = o->contrast;
    n->saturation = o->saturation;
    n->contrast = o->contrast;
    n->grey = o->grey;
    return 0;
  }
  return 1;
}

// taken from denoiseprofile.c
static void add_preset(dt_iop_module_so_t *self, const char *name,
                       const char *pi, const int version, const char *bpi, const int blendop_version)
{
  int len, blen;
  uint8_t *p  = dt_exif_xmp_decode(pi, strlen(pi), &len);
  uint8_t *bp = dt_exif_xmp_decode(bpi, strlen(bpi), &blen);
  if(blendop_version != dt_develop_blend_version())
  {
    // update to current blendop params format
    void *bp_new = malloc(sizeof(dt_develop_blend_params_t));

    if(dt_develop_blend_legacy_params_from_so(self, bp, blendop_version, bp_new, dt_develop_blend_version(),
      blen) == 0)
    {
      free(bp);
      bp = bp_new;
      blen = sizeof(dt_develop_blend_params_t);
    }
    else
    {
      free(bp);
      free(bp_new);
      bp = NULL;
    }
  }

  if(p && bp)
    dt_gui_presets_add_with_blendop(name, self->op, version, p, len, bp, 1);
  free(bp);
  free(p);
}

void init_presets(dt_iop_module_so_t *self)
{
  // these blobs were exported as dtstyle and copied from there:
  add_preset(self, _("split-toning teal-orange (2nd instance)"),
             "gz02eJxjZGBg8HhYZX99cYN9kkCDfdCOOnsGhgZ7ruvN9m8CK+yXFNTaz5w50z5PqBku9u9/PVjNv//9jqfP+NgDAHs0HIc=", 3,
             "gz05eJxjZWBgYGUAgRNODFDAzszAxMBQ5cwI4Tow4AUNdkBsD8E3gGwue9x8uB6q8s+c8bEF8Z9Y9Nnt2f3bbluCN03tg/EBIBckVg==", 8);
  add_preset(self, _("split-toning teal-orange (1st instance)"),
             "gz02eJxjZACBBvugHXX2E3fU219f3GAP4n/TqLFvfd1oL8HZaH/2jI/9prn1cLHUtDSwGgaGCY7//tfbAwBRixpm", 3,
             "gz04eJxjZWBgYGUAgRNODFDApgwiq5wZIVyHD4E7bBnwggZ7CIYBRiBbBA8fXT1l/P5DX21i+pnA/Pfv8uw6OzzIMq9I5rgtSH//4wii1AMASbIlcw==", 8);

  add_preset(self, _("generic film"),
             "gz02eJxjZACBBntN5gb7op/19u5AGsSX3dFgr+jYaL+vttb+0NcM+1Pnq+3XyFTZr/rYBJZPS0sD0hMcQDQA29kXSQ==", 3,
             "gz11eJxjYGBgkGAAgRNODGiAEV0AJ2iwh+CRxQcA5qIZBA==", 8);

  add_preset(self, _("similar to Kodak Portra"),
             "gz02eJxjZACBBnsQfh3YYK8VU28P43s8rLKP6W+yP/Q1w36deyMYLymoBcsZGxcDaQGHs2d87AGnphWu", 3,
             "gz11eJxjYGBgkGAAgRNODGiAEV0AJ2iwh+CRxQcA5qIZBA==", 8);

  add_preset(self, _("similar to Kodak Ektar"),
             "gz02eJxjZACBBvvrixvsrXIb7IN21NnD+CA2iOa6nmxvZFxsX15ebp+e1gaWNwbyGRgEHNLS0uwBE7wWhw==", 3,
             "gz11eJxjYGBgkGAAgRNODGiAEV0AJ2iwh+CRxQcA5qIZBA==", 8);

  add_preset(self, _("similar to Kodachrome"),
             "gz02eJxjZACBBvvrixvsrXIb7IN21NnD+CA2iG59HWhvZFxsX15ebp+e1gaWT0tLA9ICDrNmRtoDACjOF7c=", 3,
             "gz11eJxjYGBgkGAAgRNODGiAEV0AJ2iwh+CRxQcA5qIZBA==", 8);
}

static inline float CDL(float x, float slope, float offset, float power)
{
  float out;
  out = slope * x + offset;
  out = (out <= 0.0f) ? 0.0f : powf(out, power);
  return out;
}

// see http://www.brucelindbloom.com/Eqn_RGB_XYZ_Matrix.html for the transformation matrices
void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_colorbalance_data_t *d = (dt_iop_colorbalance_data_t *)piece->data;
  const int ch = piece->colors;

  // these are RGB values!
  const dt_aligned_pixel_t gain = { d->gain[CHANNEL_RED] * d->gain[CHANNEL_FACTOR],
                                    d->gain[CHANNEL_GREEN] * d->gain[CHANNEL_FACTOR],
                                    d->gain[CHANNEL_BLUE] * d->gain[CHANNEL_FACTOR] };
  const float contrast = (d->contrast != 0.0f) ? 1.0f / d->contrast : 1000000.0f,
              grey = d->grey / 100.0f;

  // For neutral parameters, skip the computations doing x^1 or (x-a)*1 + a to save time
  const int run_contrast = (d->contrast == 1.0f) ? 0 : 1;
  const int run_saturation = (d->saturation == 1.0f) ? 0: 1;
  const int run_saturation_out = (d->saturation_out == 1.0f) ? 0: 1;

  switch(d->mode)
  {
    case LEGACY:
    {
      // these are RGB values!
      const dt_aligned_pixel_t lift = { 2.0 - (d->lift[CHANNEL_RED] * d->lift[CHANNEL_FACTOR]),
                                        2.0 - (d->lift[CHANNEL_GREEN] * d->lift[CHANNEL_FACTOR]),
                                        2.0 - (d->lift[CHANNEL_BLUE] * d->lift[CHANNEL_FACTOR]) },
                              gamma = { d->gamma[CHANNEL_RED] * d->gamma[CHANNEL_FACTOR],
                                        d->gamma[CHANNEL_GREEN] * d->gamma[CHANNEL_FACTOR],
                                        d->gamma[CHANNEL_BLUE] * d->gamma[CHANNEL_FACTOR] },
                          gamma_inv = { (gamma[0] != 0.0) ? 1.0 / gamma[0] : 1000000.0,
                                        (gamma[1] != 0.0) ? 1.0 / gamma[1] : 1000000.0,
                                        (gamma[2] != 0.0) ? 1.0 / gamma[2] : 1000000.0 };

#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) \
      dt_omp_firstprivate(ch, gain, gamma_inv, lift, ivoid, ovoid, roi_in, \
                          roi_out) \
      shared(d) \
      schedule(static)
#endif
      for(size_t k = 0; k < (size_t)ch * roi_in->width * roi_out->height; k += ch)
      {
        float *in = ((float *)ivoid) + k;
        float *out = ((float *)ovoid) + k;

        // transform the pixel to sRGB:
        // Lab -> XYZ
        dt_aligned_pixel_t XYZ = { 0.0f };
        dt_Lab_to_XYZ(in, XYZ);

        // XYZ -> sRGB
        dt_aligned_pixel_t rgb = { 0.0f };
        dt_XYZ_to_sRGB(XYZ, rgb);

        // do the calculation in RGB space
        for(int c = 0; c < 3; c++)
        {
          // lift gamma gain
          rgb[c] = ((( rgb[c]  - 1.0f) * lift[c]) + 1.0f) * gain[c];
          rgb[c] = (rgb[c] < 0.0f) ? 0.0f : powf(rgb[c], gamma_inv[c]);
        }

        // transform the result back to Lab
        // sRGB -> XYZ
        dt_sRGB_to_XYZ(rgb, XYZ);

        // XYZ -> Lab
        dt_XYZ_to_Lab(XYZ, out);
      }
      break;
    }
    case LIFT_GAMMA_GAIN:
    {
      // these are RGB values!
      const dt_aligned_pixel_t lift = { 2.0 - (d->lift[CHANNEL_RED] * d->lift[CHANNEL_FACTOR]),
                                        2.0 - (d->lift[CHANNEL_GREEN] * d->lift[CHANNEL_FACTOR]),
                                        2.0 - (d->lift[CHANNEL_BLUE] * d->lift[CHANNEL_FACTOR]) },
                              gamma = { d->gamma[CHANNEL_RED] * d->gamma[CHANNEL_FACTOR],
                                        d->gamma[CHANNEL_GREEN] * d->gamma[CHANNEL_FACTOR],
                                        d->gamma[CHANNEL_BLUE] * d->gamma[CHANNEL_FACTOR] },
                          gamma_inv = { (gamma[0] != 0.0) ? 1.0 / gamma[0] : 1000000.0,
                                        (gamma[1] != 0.0) ? 1.0 / gamma[1] : 1000000.0,
                                        (gamma[2] != 0.0) ? 1.0 / gamma[2] : 1000000.0 };

#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) \
      dt_omp_firstprivate(ch, contrast, gain, gamma_inv, grey, ivoid, lift, \
                          ovoid, roi_in, roi_out, run_contrast, \
                          run_saturation, run_saturation_out) \
      shared(d) \
      schedule(static)
#endif
      for(size_t k = 0; k < (size_t)ch * roi_in->width * roi_out->height; k += ch)
      {
        float *in = ((float *)ivoid) + k;
        float *out = ((float *)ovoid) + k;

        // transform the pixel to sRGB:
        // Lab -> XYZ
        dt_aligned_pixel_t XYZ = { 0.0f };
        dt_Lab_to_XYZ(in, XYZ);

        // XYZ -> sRGB
        dt_aligned_pixel_t rgb = { 0.0f };
        dt_XYZ_to_prophotorgb(XYZ, rgb);

        float luma = XYZ[1]; // the Y channel is the relative luminance

        // do the calculation in RGB space
        for(int c = 0; c < 3; c++)
        {
          // main saturation input
          if(run_saturation) rgb[c] = luma + d->saturation * (rgb[c] - luma);

          // RGB gamma correction
          rgb[c] = (rgb[c] <= 0.0f) ? 0.0f : powf(rgb[c], 1.0f/2.2f);

          // lift gamma gain
          rgb[c] = ((( rgb[c]  - 1.0f) * lift[c]) + 1.0f) * gain[c];
          rgb[c] = (rgb[c] <= 0.0f) ? 0.0f : powf(rgb[c], gamma_inv[c] * 2.2f);
        }

        // main saturation output
        if(run_saturation_out)
        {
          dt_prophotorgb_to_XYZ(rgb, XYZ);
          luma = XYZ[1];
          for(int c = 0; c < 3; c++) rgb[c] = luma + d->saturation_out * (rgb[c] - luma);
        }

        // fulcrum contrat
        if(run_contrast) for(int c = 0; c < 3; c++) rgb[c] = (rgb[c] <= 0.0f) ? 0.0f : powf(rgb[c] / grey, contrast) * grey;

        // transform the result back to Lab
        // sRGB -> XYZ
        dt_prophotorgb_to_XYZ(rgb, XYZ);

        // XYZ -> Lab
        dt_XYZ_to_Lab(XYZ, out);
      }
      break;
   }
    case SLOPE_OFFSET_POWER:
    {
      // these are RGB values!

      const dt_aligned_pixel_t lift = { ( d->lift[CHANNEL_RED] + d->lift[CHANNEL_FACTOR] - 2.0f),
                                        ( d->lift[CHANNEL_GREEN] + d->lift[CHANNEL_FACTOR] - 2.0f),
                                        ( d->lift[CHANNEL_BLUE] + d->lift[CHANNEL_FACTOR] - 2.0f)},
                              gamma = { (2.0f - d->gamma[CHANNEL_RED]) * (2.0f - d->gamma[CHANNEL_FACTOR]),
                                        (2.0f - d->gamma[CHANNEL_GREEN]) * (2.0f - d->gamma[CHANNEL_FACTOR]),
                                        (2.0f - d->gamma[CHANNEL_BLUE]) * (2.0f - d->gamma[CHANNEL_FACTOR])};

#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) \
      dt_omp_firstprivate(ch, contrast, gain, gamma, grey, ivoid, lift, ovoid, \
                          roi_in, roi_out, run_contrast, run_saturation, \
                          run_saturation_out) \
      shared(d) \
      schedule(static)
#endif
      for(size_t k = 0; k < (size_t)ch * roi_in->width * roi_out->height; k += ch)
      {
        float *in = ((float *)ivoid) + k;
        float *out = ((float *)ovoid) + k;

        // transform the pixel to RGB:
        // Lab -> XYZ
        dt_aligned_pixel_t XYZ;
        dt_Lab_to_XYZ(in, XYZ);

        // XYZ -> RGB
        dt_aligned_pixel_t rgb;
        dt_XYZ_to_prophotorgb(XYZ, rgb);

        float luma = XYZ[1]; // the Y channel is the RGB luminance

        // do the calculation in RGB space
        for(int c = 0; c < 3; c++)
        {
          // main saturation input
          if(run_saturation) rgb[c] = luma + d->saturation * (rgb[c] - luma);

          // channel CDL
          rgb[c] = CDL(rgb[c], gain[c], lift[c], gamma[c]);
        }

        // main saturation output
        if(run_saturation_out)
        {
          dt_prophotorgb_to_XYZ(rgb, XYZ);
          luma = XYZ[1];
          for(int c = 0; c < 3; c++) rgb[c] = luma + d->saturation_out * (rgb[c] - luma);
        }

        // fulcrum contrat
        if(run_contrast) for(int c = 0; c < 3; c++) rgb[c] = (rgb[c] <= 0.0f) ? 0.0f : powf(rgb[c] / grey, contrast) * grey;

        // transform the result back to Lab
        // sRGB -> XYZ
        dt_prophotorgb_to_XYZ(rgb , XYZ);

        // XYZ -> Lab
        dt_XYZ_to_Lab(XYZ, out);
      }
      break;
    }
  }
  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

#if defined(__SSE__)
void process_sse2(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_colorbalance_data_t *d = (dt_iop_colorbalance_data_t *)piece->data;
  const int ch = piece->colors;
  const __m128 gain = _mm_setr_ps(d->gain[CHANNEL_RED] * d->gain[CHANNEL_FACTOR],
                                  d->gain[CHANNEL_GREEN] * d->gain[CHANNEL_FACTOR],
                                  d->gain[CHANNEL_BLUE] * d->gain[CHANNEL_FACTOR],
                                  0.0f);

  float contrast_inv = (d->contrast != 0.0f) ? 1.0f / d->contrast : 1000000.0f;
  const __m128 contrast = _mm_setr_ps(contrast_inv, contrast_inv, contrast_inv, 0.0f);
  float grey_corr = d->grey / 100.0f;
  const __m128 grey = _mm_setr_ps(grey_corr, grey_corr, grey_corr, 0.0f);
  const __m128 saturation = _mm_setr_ps(d->saturation, d->saturation, d->saturation, 0.0f);
  const __m128 saturation_out = _mm_setr_ps(d->saturation_out, d->saturation_out, d->saturation_out, 0.0f);
  const __m128 zero = _mm_setzero_ps();
  const __m128 one = _mm_set1_ps(1.0);

  // For neutral parameters, skip the computations doing x^1 or (x-a)*1 + a to save time
  const int run_contrast = (d->contrast == 1.0f) ? 0 : 1;
  const int run_saturation = (d->saturation == 1.0f) ? 0: 1;
  const int run_saturation_out = (d->saturation_out == 1.0f) ? 0: 1;

  switch(d->mode)
  {
    case LEGACY:
    {
      // these are RGB values!
      const __m128 lift = _mm_setr_ps(2.0 - (d->lift[CHANNEL_RED] * d->lift[CHANNEL_FACTOR]),
                                      2.0 - (d->lift[CHANNEL_GREEN] * d->lift[CHANNEL_FACTOR]),
                                      2.0 - (d->lift[CHANNEL_BLUE] * d->lift[CHANNEL_FACTOR]),
                                      0.0f);

      const __m128 gamma = _mm_setr_ps(d->gamma[CHANNEL_RED] * d->gamma[CHANNEL_FACTOR],
                                   d->gamma[CHANNEL_GREEN] * d->gamma[CHANNEL_FACTOR],
                                   d->gamma[CHANNEL_BLUE] * d->gamma[CHANNEL_FACTOR],
                                   0.0f);

      const __m128 gamma_inv = _mm_setr_ps((gamma[0] != 0.0) ? 1.0 / gamma[0] : 1000000.0,
                                       (gamma[1] != 0.0) ? 1.0 / gamma[1] : 1000000.0,
                                       (gamma[2] != 0.0) ? 1.0 / gamma[2] : 1000000.0,
                                       0.0f);

#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) \
      dt_omp_firstprivate(ch, gain, gamma_inv, ivoid, lift, one, ovoid, roi_in, roi_out, zero) \
      schedule(static)
#endif
      for(size_t k = 0; k < (size_t)ch * roi_in->width * roi_out->height; k += ch)
      {
        float *in = ((float *)ivoid) + k;
        float *out = ((float *)ovoid) + k;

        // transform the pixel to sRGB:
        // Lab -> XYZ
        __m128 XYZ = dt_Lab_to_XYZ_sse2(_mm_load_ps(in));
        // XYZ -> sRGB
        __m128 rgb = dt_XYZ_to_sRGB_sse2(XYZ);

        // do the calculation in RGB space
        // regular lift gamma gain
        rgb = ((rgb - one) * lift + one) * gain;
        rgb = _mm_max_ps(rgb, zero);
        rgb = _mm_pow_ps(rgb, gamma_inv);

        // transform the result back to Lab
        // sRGB -> XYZ
        XYZ = dt_sRGB_to_XYZ_sse2(rgb);
        // XYZ -> Lab
        _mm_stream_ps(out, dt_XYZ_to_Lab_sse2(XYZ));
      }
      break;
    }

    case LIFT_GAMMA_GAIN:
    {
      // these are RGB values!
      const __m128 lift = _mm_setr_ps(2.0f - (d->lift[CHANNEL_RED] * d->lift[CHANNEL_FACTOR]),
                                      2.0f - (d->lift[CHANNEL_GREEN] * d->lift[CHANNEL_FACTOR]),
                                      2.0f - (d->lift[CHANNEL_BLUE] * d->lift[CHANNEL_FACTOR]),
                                      0.0f);
      const __m128 gamma = _mm_setr_ps(d->gamma[CHANNEL_RED] * d->gamma[CHANNEL_FACTOR],
                                       d->gamma[CHANNEL_GREEN] * d->gamma[CHANNEL_FACTOR],
                                       d->gamma[CHANNEL_BLUE] * d->gamma[CHANNEL_FACTOR],
                                       0.0f);
      const __m128 gamma_inv = _mm_setr_ps((gamma[0] != 0.0) ? 1.0 / gamma[0] : 1000000.0,
                                           (gamma[1] != 0.0) ? 1.0 / gamma[1] : 1000000.0,
                                           (gamma[2] != 0.0) ? 1.0 / gamma[2] : 1000000.0,
                                           0.0f);

      const __m128 gamma_RGB = _mm_set1_ps(2.2f);
      const __m128 gamma_inv_RGB = _mm_set1_ps(1.0f/2.2f);

#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) \
      dt_omp_firstprivate(ch, contrast, gain, gamma_inv, gamma_inv_RGB, \
                          gamma_RGB, grey, ivoid, lift, one, ovoid, roi_in, \
                          roi_out, run_contrast, run_saturation, \
                          run_saturation_out, saturation, saturation_out, \
                          zero) \
      schedule(static)
#endif
      for(size_t k = 0; k < (size_t)ch * roi_in->width * roi_out->height; k += ch)
      {
        float *in = ((float *)ivoid) + k;
        float *out = ((float *)ovoid) + k;

        // transform the pixel to sRGB:
        // Lab -> XYZ
        __m128 XYZ = dt_Lab_to_XYZ_sse2(_mm_load_ps(in));
        // XYZ -> sRGB
        __m128 rgb = dt_XYZ_to_prophotoRGB_sse2(XYZ);

        __m128 luma;

        // adjust main saturation input
        if(run_saturation)
        {
          luma = _mm_set1_ps(XYZ[1]); // the Y channel is the relative luminance
          rgb = luma + saturation * (rgb - luma);
        }

        // RGB gamma adjustment
        rgb = _mm_pow_ps(_mm_max_ps(rgb, zero), gamma_inv_RGB);

        // regular lift gamma gain
        rgb = ((rgb - one) * lift + one) * gain;
        rgb = _mm_max_ps(rgb, zero);
        rgb = _mm_pow_ps(rgb, gamma_inv * gamma_RGB);

        // adjust main saturation output
        if(run_saturation_out)
        {
          XYZ = dt_prophotoRGB_to_XYZ_sse2(rgb);
          luma = _mm_set1_ps(XYZ[1]); // the Y channel is the relative luminance
          rgb = luma + saturation_out * (rgb - luma);
        }

        // fulcrum contrast
        if(run_contrast)
        {
          rgb = _mm_max_ps(rgb, zero);
          rgb = _mm_pow_ps(rgb / grey, contrast) * grey;
        }

        // transform the result back to Lab
        // sRGB -> XYZ
        XYZ = dt_prophotoRGB_to_XYZ_sse2(rgb);
        // XYZ -> Lab
        _mm_stream_ps(out, dt_XYZ_to_Lab_sse2(XYZ));
      }

      break;
    }

    case SLOPE_OFFSET_POWER:
    {
      // these are RGB values!
      const __m128 lift = _mm_setr_ps((d->lift[CHANNEL_RED] + d->lift[CHANNEL_FACTOR] - 2.f),
                                      (d->lift[CHANNEL_GREEN] + d->lift[CHANNEL_FACTOR] - 2.f),
                                      (d->lift[CHANNEL_BLUE] + d->lift[CHANNEL_FACTOR] - 2.f),
                                      0.0f);
      const __m128 gamma = _mm_setr_ps((2.0f - d->gamma[CHANNEL_RED]) * (2.0f - d->gamma[CHANNEL_FACTOR]),
                                      (2.0f - d->gamma[CHANNEL_GREEN]) * (2.0f - d->gamma[CHANNEL_FACTOR]),
                                      (2.0f - d->gamma[CHANNEL_BLUE]) * (2.0f - d->gamma[CHANNEL_FACTOR]),
                                      0.0f);

#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) \
      dt_omp_firstprivate(ch, contrast, gain, gamma, grey, ivoid, lift, ovoid, \
                          roi_in, roi_out, run_contrast, run_saturation, \
                          run_saturation_out, saturation, saturation_out, \
                          zero) \
      schedule(static)
#endif
      for(size_t k = 0; k < (size_t)ch * roi_in->width * roi_out->height; k += ch)
      {
        float *in = ((float *)ivoid) + k;
        float *out = ((float *)ovoid) + k;

        // transform the pixel to sRGB:
        // Lab -> XYZ
        __m128 XYZ = dt_Lab_to_XYZ_sse2(_mm_load_ps(in));
        // XYZ -> sRGB
        __m128 rgb = dt_XYZ_to_prophotoRGB_sse2(XYZ);

        __m128 luma;

        // adjust main saturation
        if(run_saturation)
        {
          luma = _mm_set1_ps(XYZ[1]); // the Y channel is the relative luminance
          rgb = luma + saturation * (rgb - luma);
        }

        // slope offset
        rgb = rgb * gain + lift;

        //power
        rgb = _mm_max_ps(rgb, zero);
        rgb = _mm_pow_ps(rgb, gamma);

        // adjust main saturation output
        if(run_saturation_out)
        {
          XYZ = dt_prophotoRGB_to_XYZ_sse2(rgb);
          luma = _mm_set1_ps(XYZ[1]); // the Y channel is the relative luminance
          rgb = luma + saturation_out * (rgb - luma);
        }

        // fulcrum contrast
        if(run_contrast)
        {
          rgb = _mm_max_ps(rgb, zero);
          rgb = _mm_pow_ps(rgb / grey, contrast) * grey;
        }

        // transform the result back to Lab
        // sRGB -> XYZ
        XYZ = dt_prophotoRGB_to_XYZ_sse2(rgb);
        // XYZ -> Lab
        _mm_stream_ps(out, dt_XYZ_to_Lab_sse2(XYZ));
      }
      break;
    }
  }
  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}
#endif

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_colorbalance_data_t *d = (dt_iop_colorbalance_data_t *)piece->data;
  dt_iop_colorbalance_global_data_t *gd = (dt_iop_colorbalance_global_data_t *)self->global_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };

  switch(d->mode)
  {
    case LEGACY:
    {
      const float lift[4] = { 2.0f - (d->lift[CHANNEL_RED] * d->lift[CHANNEL_FACTOR]),
                              2.0f - (d->lift[CHANNEL_GREEN] * d->lift[CHANNEL_FACTOR]),
                              2.0f - (d->lift[CHANNEL_BLUE] * d->lift[CHANNEL_FACTOR]), 0.0f },
                  gamma[4] = { d->gamma[CHANNEL_RED] * d->gamma[CHANNEL_FACTOR],
                               d->gamma[CHANNEL_GREEN] * d->gamma[CHANNEL_FACTOR],
                               d->gamma[CHANNEL_BLUE] * d->gamma[CHANNEL_FACTOR], 0.0f },
                  gamma_inv[4] = { (gamma[0] != 0.0f) ? 1.0f / gamma[0] : 1000000.0f,
                                   (gamma[1] != 0.0f) ? 1.0f / gamma[1] : 1000000.0f,
                                   (gamma[2] != 0.0f) ? 1.0f / gamma[2] : 1000000.0f, 0.0f },
                  gain[4] = { d->gain[CHANNEL_RED] * d->gain[CHANNEL_FACTOR],
                              d->gain[CHANNEL_GREEN] * d->gain[CHANNEL_FACTOR],
                              d->gain[CHANNEL_BLUE] * d->gain[CHANNEL_FACTOR], 0.0f },
                  contrast = (d->contrast != 0.0f) ? 1.0f / d->contrast : 1000000.0f,
                  grey = d->grey / 100.0f,
                  saturation = d->saturation;

      dt_opencl_set_kernel_args(devid, gd->kernel_colorbalance, 0, CLARG(dev_in), CLARG(dev_out), CLARG(width),
        CLARG(height), CLARG(lift), CLARG(gain), CLARG(gamma_inv), CLARG(saturation), CLARG(contrast),
        CLARG(grey));
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_colorbalance, sizes);
      if(err != CL_SUCCESS) goto error;
      return TRUE;

      break;
    }
    case LIFT_GAMMA_GAIN:
    {
      const float lift[4] = { 2.0f - (d->lift[CHANNEL_RED] * d->lift[CHANNEL_FACTOR]),
                              2.0f - (d->lift[CHANNEL_GREEN] * d->lift[CHANNEL_FACTOR]),
                              2.0f - (d->lift[CHANNEL_BLUE] * d->lift[CHANNEL_FACTOR]), 0.0f },
                  gamma[4] = { d->gamma[CHANNEL_RED] * d->gamma[CHANNEL_FACTOR],
                               d->gamma[CHANNEL_GREEN] * d->gamma[CHANNEL_FACTOR],
                               d->gamma[CHANNEL_BLUE] * d->gamma[CHANNEL_FACTOR], 0.0f },
                  gamma_inv[4] = { (gamma[0] != 0.0f) ? 1.0f / gamma[0] : 1000000.0f,
                                   (gamma[1] != 0.0f) ? 1.0f / gamma[1] : 1000000.0f,
                                   (gamma[2] != 0.0f) ? 1.0f / gamma[2] : 1000000.0f, 0.0f },
                  gain[4] = { d->gain[CHANNEL_RED] * d->gain[CHANNEL_FACTOR],
                              d->gain[CHANNEL_GREEN] * d->gain[CHANNEL_FACTOR],
                              d->gain[CHANNEL_BLUE] * d->gain[CHANNEL_FACTOR], 0.0f },
                  contrast = (d->contrast != 0.0f) ? 1.0f / d->contrast : 1000000.0f,
                  grey = d->grey / 100.0f,
                  saturation = d->saturation,
                  saturation_out = d->saturation_out;

      dt_opencl_set_kernel_args(devid, gd->kernel_colorbalance_lgg, 0, CLARG(dev_in), CLARG(dev_out),
        CLARG(width), CLARG(height), CLARG(lift), CLARG(gain), CLARG(gamma_inv), CLARG(saturation), CLARG(contrast),
        CLARG(grey), CLARG(saturation_out));
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_colorbalance_lgg, sizes);
      if(err != CL_SUCCESS) goto error;
      return TRUE;

      break;
    }
    case SLOPE_OFFSET_POWER:
    {
      const float lift[4] = { ( d->lift[CHANNEL_RED] + d->lift[CHANNEL_FACTOR] - 2.0f ),
                              ( d->lift[CHANNEL_GREEN] + d->lift[CHANNEL_FACTOR] - 2.0f ),
                              ( d->lift[CHANNEL_BLUE] + d->lift[CHANNEL_FACTOR] - 2.0f ),
                              0.0f },
                  gamma[4] = { (2.0f - d->gamma[CHANNEL_RED]) * (2.0f - d->gamma[CHANNEL_FACTOR]),
                               (2.0f - d->gamma[CHANNEL_GREEN]) * (2.0f - d->gamma[CHANNEL_FACTOR]),
                               (2.0f - d->gamma[CHANNEL_BLUE]) * (2.0f - d->gamma[CHANNEL_FACTOR]),
                               0.0f },
                  gain[4] = { d->gain[CHANNEL_RED] * d->gain[CHANNEL_FACTOR],
                              d->gain[CHANNEL_GREEN] * d->gain[CHANNEL_FACTOR],
                              d->gain[CHANNEL_BLUE] * d->gain[CHANNEL_FACTOR],
                              0.0f },
                  contrast = (d->contrast != 0.0f) ? 1.0f / d->contrast : 1000000.0f,
                  grey = d->grey / 100.0f,
                  saturation = d->saturation,
                  saturation_out = d->saturation_out;

      dt_opencl_set_kernel_args(devid, gd->kernel_colorbalance_cdl, 0, CLARG(dev_in), CLARG(dev_out),
        CLARG(width), CLARG(height), CLARG(lift), CLARG(gain), CLARG(gamma), CLARG(saturation), CLARG(contrast),
        CLARG(grey), CLARG(saturation_out));
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_colorbalance_cdl, sizes);
      if(err != CL_SUCCESS) goto error;
      return TRUE;

      break;
    }
  }

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_colorbalance] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}
#endif

static inline void update_saturation_slider_color(GtkWidget *slider, float hue)
{
  dt_aligned_pixel_t rgb;
  if(hue != -1)
  {
    hsl2rgb(rgb, hue, 1.0, 0.5);
    dt_bauhaus_slider_set_stop(slider, 1.0, rgb[0], rgb[1], rgb[2]);
    hsl2rgb(rgb, hue, 0.0, 0.5);
    dt_bauhaus_slider_set_stop(slider, 0.0, rgb[0], rgb[1], rgb[2]);
    gtk_widget_queue_draw(GTK_WIDGET(slider));
  }
}

static inline void set_RGB_sliders(GtkWidget *R, GtkWidget *G, GtkWidget *B, float hsl[3], float *p, int mode)
{

  dt_aligned_pixel_t rgb = { 0.0f };
  hsl2rgb(rgb, hsl[0], hsl[1], hsl[2]);

  if(hsl[0] != -1)
  {
    p[CHANNEL_RED] = rgb[0] * 2.0f;
    p[CHANNEL_GREEN] = rgb[1] * 2.0f;
    p[CHANNEL_BLUE] = rgb[2] * 2.0f;

    ++darktable.gui->reset;
    dt_bauhaus_slider_set(R, p[CHANNEL_RED]);
    dt_bauhaus_slider_set(G, p[CHANNEL_GREEN]);
    dt_bauhaus_slider_set(B, p[CHANNEL_BLUE]);
    --darktable.gui->reset;
  }
}

static inline void set_HSL_sliders(GtkWidget *hue, GtkWidget *sat, float RGB[4])
{
  /** HSL sliders are set from the RGB values at any time.
  * Only the RGB values are saved and used in the computations.
  * The HSL sliders are merely an interface.
  */
  dt_aligned_pixel_t RGB_norm = { (RGB[CHANNEL_RED] / 2.0f), (RGB[CHANNEL_GREEN] / 2.0f), (RGB[CHANNEL_BLUE] / 2.0f) };

  float h, s, l;
  rgb2hsl(RGB_norm, &h, &s, &l);

  if(h != -1.0f)
  {
    dt_bauhaus_slider_set(hue, h * 360.0f);
    dt_bauhaus_slider_set(sat, s * 100.0f);
    update_saturation_slider_color(GTK_WIDGET(sat), h);
    gtk_widget_queue_draw(GTK_WIDGET(sat));
  }
  else
  {
    dt_bauhaus_slider_set(hue, -1.0f);
    dt_bauhaus_slider_set(sat, 0.0f);
    gtk_widget_queue_draw(GTK_WIDGET(sat));
  }
}

static inline void _check_tuner_picker_labels(dt_iop_module_t *self)
{
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

  if(g->luma_patches_flags[GAIN] == USER_SELECTED && g->luma_patches_flags[GAMMA] == USER_SELECTED
     && g->luma_patches_flags[LIFT] == USER_SELECTED)
    dt_bauhaus_widget_set_label(g->auto_luma, NULL, N_("optimize luma from patches"));
  else
    dt_bauhaus_widget_set_label(g->auto_luma, NULL, N_("optimize luma"));

  if(g->color_patches_flags[GAIN] == USER_SELECTED && g->color_patches_flags[GAMMA] == USER_SELECTED
     && g->color_patches_flags[LIFT] == USER_SELECTED)
    dt_bauhaus_widget_set_label(g->auto_color, NULL, N_("neutralize colors from patches"));
  else
    dt_bauhaus_widget_set_label(g->auto_color, NULL, N_("neutralize colors"));
}


static void apply_autogrey(dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

  dt_aligned_pixel_t XYZ = { 0.0f };
  dt_aligned_pixel_t rgb = { 0.0f };
  dt_Lab_to_XYZ((const float *)self->picked_color, XYZ);
  dt_XYZ_to_prophotorgb((const float *)XYZ, rgb);

  const dt_aligned_pixel_t lift
      = { (p->lift[CHANNEL_RED] + p->lift[CHANNEL_FACTOR] - 2.0f),
          (p->lift[CHANNEL_GREEN] + p->lift[CHANNEL_FACTOR] - 2.0f),
          (p->lift[CHANNEL_BLUE] + p->lift[CHANNEL_FACTOR] - 2.0f) },
      gamma
      = { p->gamma[CHANNEL_RED] * p->gamma[CHANNEL_FACTOR],
          p->gamma[CHANNEL_GREEN] * p->gamma[CHANNEL_FACTOR],
          p->gamma[CHANNEL_BLUE] * p->gamma[CHANNEL_FACTOR] },
      gain = { p->gain[CHANNEL_RED] * p->gain[CHANNEL_FACTOR], p->gain[CHANNEL_GREEN] * p->gain[CHANNEL_FACTOR],
               p->gain[CHANNEL_BLUE] * p->gain[CHANNEL_FACTOR] };

  for(int c = 0; c < 3; c++)
  {
    rgb[c] = CDL(rgb[c], gain[c], lift[c], 2.0f - gamma[c]);
    rgb[c] = CLAMP(rgb[c], 0.0f, 1.0f);
  }

  dt_prophotorgb_to_XYZ((const float *)rgb, XYZ);

  p->grey = XYZ[1] * 100.0f;

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->grey, p->grey);
  --darktable.gui->reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void apply_lift_neutralize(dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

  dt_aligned_pixel_t XYZ = { 0.0f };
  dt_Lab_to_XYZ((const float *)self->picked_color, XYZ);
  dt_aligned_pixel_t RGB = { 0.0f };
  dt_XYZ_to_prophotorgb((const float *)XYZ, RGB);

// Save the patch color for the optimization
  for(int c = 0; c < 3; ++c) g->color_patches_lift[c] = RGB[c];
  g->color_patches_flags[LIFT] = USER_SELECTED;

  // Compute the RGB values after the CDL factors
  for(int c = 0; c < 3; ++c)
    RGB[c] = CDL(RGB[c], p->gain[CHANNEL_FACTOR], p->lift[CHANNEL_FACTOR] - 1.0f, 2.0f - p->gamma[CHANNEL_FACTOR]);

  // Compute the luminance of the average grey
  dt_XYZ_to_prophotorgb((const float *)XYZ, RGB);

  // Get the parameter
  for(int c = 0; c < 3; ++c) RGB[c] = powf(XYZ[1], 1.0f/(2.0f - p->gamma[c+1])) - RGB[c] * p->gain[c+1];

  p->lift[CHANNEL_RED] = RGB[0] + 1.0f;
  p->lift[CHANNEL_GREEN] = RGB[1] + 1.0f;
  p->lift[CHANNEL_BLUE] = RGB[2] + 1.0f;

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->lift_r, p->lift[CHANNEL_RED]);
  dt_bauhaus_slider_set(g->lift_g, p->lift[CHANNEL_GREEN]);
  dt_bauhaus_slider_set(g->lift_b, p->lift[CHANNEL_BLUE]);
  set_HSL_sliders(g->hue_lift, g->sat_lift, p->lift);
  --darktable.gui->reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void apply_gamma_neutralize(dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

  dt_aligned_pixel_t XYZ = { 0.0f };
  dt_Lab_to_XYZ((const float *)self->picked_color, XYZ);
  dt_aligned_pixel_t RGB = { 0.0f };
  dt_XYZ_to_prophotorgb((const float *)XYZ, RGB);

// Save the patch color for the optimization
  for(int c = 0; c < 3; ++c) g->color_patches_gamma[c] = RGB[c];
  g->color_patches_flags[GAMMA] = USER_SELECTED;

  // Compute the RGB values after the CDL factors
  for(int c = 0; c < 3; ++c)
    RGB[c] = CDL(RGB[c], p->gain[CHANNEL_FACTOR], p->lift[CHANNEL_FACTOR] - 1.0f, 2.0f - p->gamma[CHANNEL_FACTOR]);

  // Compute the luminance of the average grey
  dt_XYZ_to_prophotorgb((const float *)XYZ, RGB);

  // Get the parameter
  for(int c = 0; c < 3; ++c) RGB[c] = logf(XYZ[1])/ logf(RGB[c] * p->gain[c + 1] + p->lift[c + 1] - 1.0f);

  p->gamma[CHANNEL_RED] = CLAMP(2.0 - RGB[0], 0.0001f, 2.0f);
  p->gamma[CHANNEL_GREEN] = CLAMP(2.0 - RGB[1], 0.0001f, 2.0f);
  p->gamma[CHANNEL_BLUE] = CLAMP(2.0 - RGB[2], 0.0001f, 2.0f);

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->gamma_r, p->gamma[CHANNEL_RED]);
  dt_bauhaus_slider_set(g->gamma_g, p->gamma[CHANNEL_GREEN]);
  dt_bauhaus_slider_set(g->gamma_b, p->gamma[CHANNEL_BLUE]);
  set_HSL_sliders(g->hue_gamma, g->sat_gamma, p->gamma);
  --darktable.gui->reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void apply_gain_neutralize(dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

  dt_aligned_pixel_t XYZ = { 0.0f };
  dt_Lab_to_XYZ((const float *)self->picked_color, XYZ);
  dt_aligned_pixel_t RGB = { 0.0f };
  dt_XYZ_to_prophotorgb((const float *)XYZ, RGB);

// Save the patch color for the optimization
  for(int c = 0; c < 3; c++) g->color_patches_gain[c] = RGB[c];
  g->color_patches_flags[GAIN] = USER_SELECTED;

  // Compute the RGB values after the CDL factors
  for(int c = 0; c < 3; ++c)
    RGB[c] = CDL(RGB[c], p->gain[CHANNEL_FACTOR], p->lift[CHANNEL_FACTOR] - 1.0f, 2.0f - p->gamma[CHANNEL_FACTOR]);

  // Compute the luminance of the average grey
  dt_XYZ_to_prophotorgb((const float *)XYZ, RGB);

  // Get the parameter
  for(int c = 0; c < 3; ++c) RGB[c] = (powf(XYZ[1], 1.0f/(2.0f - p->gamma[c+1])) - p->lift[c+1] + 1.0f) / MAX(RGB[c], 0.000001f);

  p->gain[CHANNEL_RED] = RGB[0];
  p->gain[CHANNEL_GREEN] = RGB[1];
  p->gain[CHANNEL_BLUE] = RGB[2];

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->gain_r, p->gain[CHANNEL_RED]);
  dt_bauhaus_slider_set(g->gain_g, p->gain[CHANNEL_GREEN]);
  dt_bauhaus_slider_set(g->gain_b, p->gain[CHANNEL_BLUE]);
  set_HSL_sliders(g->hue_gain, g->sat_gain, p->gain);
  --darktable.gui->reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void apply_lift_auto(dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

  dt_aligned_pixel_t XYZ = { 0.0f };
  dt_Lab_to_XYZ((const float *)self->picked_color_min, XYZ);

  g->luma_patches[LIFT] = XYZ[1];
  g->luma_patches_flags[LIFT] = USER_SELECTED;

  dt_aligned_pixel_t RGB = { 0.0f };
  dt_XYZ_to_prophotorgb((const float *)XYZ, RGB);

  p->lift[CHANNEL_FACTOR] = -p->gain[CHANNEL_FACTOR] * XYZ[1] + 1.0f;

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->lift_factor, p->lift[CHANNEL_FACTOR]);
  --darktable.gui->reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void apply_gamma_auto(dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

  dt_aligned_pixel_t XYZ = { 0.0f };
  dt_Lab_to_XYZ((const float *)self->picked_color, XYZ);

  g->luma_patches[GAMMA] = XYZ[1];
  g->luma_patches_flags[GAMMA] = USER_SELECTED;

  dt_aligned_pixel_t RGB = { 0.0f };
  dt_XYZ_to_prophotorgb((const float *)XYZ, RGB);

  p->gamma[CHANNEL_FACTOR]
      = 2.0f - logf(0.1842f) / logf(MAX(p->gain[CHANNEL_FACTOR] * XYZ[1] + p->lift[CHANNEL_FACTOR] - 1.0f, 0.000001f));

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->gamma_factor, p->gamma[CHANNEL_FACTOR]);
  --darktable.gui->reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void apply_gain_auto(dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

  dt_aligned_pixel_t XYZ = { 0.0f };
  dt_Lab_to_XYZ((const float *)self->picked_color_max, XYZ);

  g->luma_patches[GAIN] = XYZ[1];
  g->luma_patches_flags[GAIN] = USER_SELECTED;

  dt_aligned_pixel_t RGB = { 0.0f };
  dt_XYZ_to_prophotorgb((const float *)XYZ, RGB);

  p->gain[CHANNEL_FACTOR] = p->lift[CHANNEL_FACTOR] / (XYZ[1]);

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->gain_factor, p->gain[CHANNEL_FACTOR]);
  --darktable.gui->reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void apply_autocolor(dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

  if(g->color_patches_flags[GAIN] == INVALID || g->color_patches_flags[GAMMA] == INVALID
     || g->color_patches_flags[LIFT] == INVALID)
  {
    /*
     * Some color patches were not picked by the user. Take a
     * picture-wide patch for these.
     */
    dt_aligned_pixel_t XYZ = { 0.0f };
    dt_Lab_to_XYZ((const float *)self->picked_color, XYZ);
    dt_aligned_pixel_t RGB = { 0.0f };
    dt_XYZ_to_prophotorgb((const float *)XYZ, RGB);

    // Save the patch color for the optimization
    if(g->color_patches_flags[LIFT] == INVALID)
    {
      for(int c = 0; c < 3; c++) g->color_patches_lift[c] = RGB[c];
      g->color_patches_flags[LIFT] = AUTO_SELECTED;
    }
    if(g->color_patches_flags[GAMMA] == INVALID)
    {
      for(int c = 0; c < 3; c++) g->color_patches_gamma[c] = RGB[c];
      g->color_patches_flags[GAMMA] = AUTO_SELECTED;
    }
    if(g->color_patches_flags[GAIN] == INVALID)
    {
      for(int c = 0; c < 3; c++) g->color_patches_gain[c] = RGB[c];
      g->color_patches_flags[GAIN] = AUTO_SELECTED;
    }
  }

  dt_iop_color_picker_reset(self, TRUE);

  // Build the CDL-corrected samples (after the factors)
  dt_aligned_pixel_t samples_lift = { 0.f };
  dt_aligned_pixel_t samples_gamma = { 0.f };
  dt_aligned_pixel_t samples_gain = { 0.f };

  for(int c = 0; c < 3; ++c)
  {
    samples_lift[c] = CDL(g->color_patches_lift[c], p->gain[CHANNEL_FACTOR], p->lift[CHANNEL_FACTOR] - 1.0f, 2.0f - p->gamma[CHANNEL_FACTOR]);
    samples_gamma[c] = CDL(g->color_patches_gamma[c], p->gain[CHANNEL_FACTOR], p->lift[CHANNEL_FACTOR] - 1.0f, 2.0f - p->gamma[CHANNEL_FACTOR]);
    samples_gain[c] = CDL(g->color_patches_gain[c], p->gain[CHANNEL_FACTOR], p->lift[CHANNEL_FACTOR] - 1.0f, 2.0f - p->gamma[CHANNEL_FACTOR]);
  }

  // Get the average patches luma value (= neutral grey equivalents) after the CDL factors
  dt_aligned_pixel_t greys = { 0.0 };
  dt_aligned_pixel_t XYZ = { 0.0 };
  dt_prophotorgb_to_XYZ((const float *)samples_lift, (float *)XYZ);
  greys[0] = XYZ[1];
  dt_prophotorgb_to_XYZ((const float *)samples_gamma, (float *)XYZ);
  greys[1] = XYZ[1];
  dt_prophotorgb_to_XYZ((const float *)samples_gain, (float *)XYZ);
  greys[2] = XYZ[1];

  // Get the current params
  dt_aligned_pixel_t RGB_lift = { p->lift[CHANNEL_RED] - 1.0f, p->lift[CHANNEL_GREEN] - 1.0f, p->lift[CHANNEL_BLUE] - 1.0f };
  dt_aligned_pixel_t RGB_gamma = { p->gamma[CHANNEL_RED], p->gamma[CHANNEL_GREEN], p->gamma[CHANNEL_BLUE] };
  dt_aligned_pixel_t RGB_gain = { p->gain[CHANNEL_RED], p->gain[CHANNEL_GREEN], p->gain[CHANNEL_BLUE] };

  /** Optimization loop :
  * We try to find the CDL curves that neutralize the 3 input color patches, while not affecting the overall lightness.
  * But this is a non-linear overconstrained problem with tainted inputs, so the best we can do is a numerical optimization.
  * To do so, we compute each parameter of each RGB curve from the input color and the 2 other parameters.
  * Then, we loop over the previous optimization until the difference between 2 updates is insignificant.
  * This would need a proper stopping criterion based on convergence analysis, but it would be overkill here since
  * it should converge usually in 20 iterations, and maximum in 100.
  * Also, the convergence has not been proven formally.
  * For better color accuracy, we compute on luminance corrected RGB values (after the main factors corrections).
  * To avoid divergence, we constrain the parameters between +- 0.25 around the neutral value.
  * Experimentally, nothing good happens out of these bounds.
  */
  for(int runs = 0 ; runs < 1000 ; ++runs)
  {
    // compute RGB slope/gain (powf(XYZ[1], 1.0f/(2.0f - p->gamma[c+1])) - p->lift[c+1] + 1.0f) / MAX(RGB[c], 0.000001f);
    for(int c = 0; c < 3; ++c) RGB_gain[c] = CLAMP((powf(greys[GAIN], 1.0f / (2.0f - RGB_gamma[c])) - RGB_lift[c]) / MAX(samples_gain[c], 0.000001f), 0.75f, 1.25f);
    // compute RGB offset/lift powf(XYZ[1], 1.0f/(2.0f - p->gamma[c+1])) - RGB[c] * p->gain[c+1];
    for(int c = 0; c < 3; ++c) RGB_lift[c] = CLAMP(powf(greys[LIFT], 1.0f / (2.0f - RGB_gamma[c])) - samples_lift[c] * RGB_gain[c], -0.025f, 0.025f);
    // compute  power/gamma 2.0f - logf(0.1842f) / logf(MAX(p->gain[CHANNEL_FACTOR] * XYZ[1] + p->lift[CHANNEL_FACTOR] - 1.0f, 0.000001f));
    for(int c = 0; c < 3; ++c) RGB_gamma[c] = 2.0f - CLAMP(logf(MAX(greys[GAMMA], 0.000001f)) / logf(MAX(RGB_gain[c] * samples_gamma[c] + RGB_lift[c], 0.000001f)), 0.75f, 1.25f);
  }

  // save
  p->lift[CHANNEL_RED] = RGB_lift[0] + 1.0f;
  p->lift[CHANNEL_GREEN] = RGB_lift[1] + 1.0f;
  p->lift[CHANNEL_BLUE] = RGB_lift[2] + 1.0f;
  p->gamma[CHANNEL_RED] = RGB_gamma[0];
  p->gamma[CHANNEL_GREEN] = RGB_gamma[1];
  p->gamma[CHANNEL_BLUE] = RGB_gamma[2];
  p->gain[CHANNEL_RED] = RGB_gain[0];
  p->gain[CHANNEL_GREEN] = RGB_gain[1];
  p->gain[CHANNEL_BLUE] = RGB_gain[2];

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->lift_r, p->lift[CHANNEL_RED]);
  dt_bauhaus_slider_set(g->lift_g, p->lift[CHANNEL_GREEN]);
  dt_bauhaus_slider_set(g->lift_b, p->lift[CHANNEL_BLUE]);

  dt_bauhaus_slider_set(g->gamma_r, p->gamma[CHANNEL_RED]);
  dt_bauhaus_slider_set(g->gamma_g, p->gamma[CHANNEL_GREEN]);
  dt_bauhaus_slider_set(g->gamma_b, p->gamma[CHANNEL_BLUE]);

  dt_bauhaus_slider_set(g->gain_r, p->gain[CHANNEL_RED]);
  dt_bauhaus_slider_set(g->gain_g, p->gain[CHANNEL_GREEN]);
  dt_bauhaus_slider_set(g->gain_b, p->gain[CHANNEL_BLUE]);

  set_HSL_sliders(g->hue_lift, g->sat_lift, p->lift);
  set_HSL_sliders(g->hue_gamma, g->sat_gamma, p->gamma);
  set_HSL_sliders(g->hue_gain, g->sat_gain, p->gain);
  --darktable.gui->reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void apply_autoluma(dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

  /*
   * If some luma patches were not picked by the user, take a
   * picture-wide patch for these.
   */
  if(g->luma_patches_flags[LIFT] == INVALID)
  {
    dt_aligned_pixel_t XYZ = { 0.0f };
    dt_Lab_to_XYZ((const float *)self->picked_color_min, XYZ);
    g->luma_patches[LIFT] = XYZ[1];
    g->luma_patches_flags[LIFT] = AUTO_SELECTED;
  }
  if(g->luma_patches_flags[GAMMA] == INVALID)
  {
    dt_aligned_pixel_t XYZ = { 0.0f };
    dt_Lab_to_XYZ((const float *)self->picked_color, XYZ);
    g->luma_patches[GAMMA] = XYZ[1];
    g->luma_patches_flags[GAMMA] = AUTO_SELECTED;
  }
  if(g->luma_patches_flags[GAIN] == INVALID)
  {
    dt_aligned_pixel_t XYZ = { 0.0f };
    dt_Lab_to_XYZ((const float *)self->picked_color_max, XYZ);
    g->luma_patches[GAIN] = XYZ[1];
    g->luma_patches_flags[GAIN] = AUTO_SELECTED;
  }

  dt_iop_color_picker_reset(self, TRUE);

  /** Optimization loop :
  * We try to find the CDL curves that neutralize the 3 input luma patches
  */
  for(int runs = 0 ; runs < 100 ; ++runs)
  {
    p->gain[CHANNEL_FACTOR] = CLAMP(p->lift[CHANNEL_FACTOR] / g->luma_patches[GAIN], 0.0f, 2.0f);
    p->lift[CHANNEL_FACTOR] = CLAMP(-p->gain[CHANNEL_FACTOR] * g->luma_patches[LIFT] + 1.0f, 0.0f, 2.0f);
    p->gamma[CHANNEL_FACTOR] = CLAMP(2.0f - logf(0.1842f) / logf(MAX(p->gain[CHANNEL_FACTOR] * g->luma_patches[GAMMA] + p->lift[CHANNEL_FACTOR] - 1.0f, 0.000001f)), 0.0f, 2.0f);
  }

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->lift_factor, p->lift[CHANNEL_FACTOR]);
  dt_bauhaus_slider_set(g->gamma_factor, p->gamma[CHANNEL_FACTOR]);
  dt_bauhaus_slider_set(g->gain_factor, p->gain[CHANNEL_FACTOR]);
  --darktable.gui->reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;
  if     (picker == g->hue_lift)
    apply_lift_neutralize(self);
  else if(picker == g->hue_gamma)
    apply_gamma_neutralize(self);
  else if(picker == g->hue_gain)
    apply_gain_neutralize(self);
  else if(picker == g->lift_factor)
    apply_lift_auto(self);
  else if(picker == g->gamma_factor)
    apply_gamma_auto(self);
  else if(picker == g->gain_factor)
    apply_gain_auto(self);
  else if(picker == g->grey)
    apply_autogrey(self);
  else if(picker == g->auto_luma)
    apply_autoluma(self);
  else if(picker == g->auto_color)
    apply_autocolor(self);
  else
    fprintf(stderr, "[colorbalance] unknown color picker\n");

  _check_tuner_picker_labels(self);
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 8; // extended.cl, from programs.conf
  dt_iop_colorbalance_global_data_t *gd
      = (dt_iop_colorbalance_global_data_t *)malloc(sizeof(dt_iop_colorbalance_global_data_t));
  module->data = gd;
  gd->kernel_colorbalance = dt_opencl_create_kernel(program, "colorbalance");
  gd->kernel_colorbalance_lgg = dt_opencl_create_kernel(program, "colorbalance_lgg");
  gd->kernel_colorbalance_cdl = dt_opencl_create_kernel(program, "colorbalance_cdl");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_colorbalance_global_data_t *gd = (dt_iop_colorbalance_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_colorbalance);
  dt_opencl_free_kernel(gd->kernel_colorbalance_lgg);
  dt_opencl_free_kernel(gd->kernel_colorbalance_cdl);
  free(module->data);
  module->data = NULL;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorbalance_data_t *d = (dt_iop_colorbalance_data_t *)(piece->data);
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)p1;

  d->mode = p->mode;

  const dt_aligned_pixel_t lift = { p->lift[CHANNEL_RED], p->lift[CHANNEL_GREEN], p->lift[CHANNEL_BLUE] };
  const dt_aligned_pixel_t gamma = { p->gamma[CHANNEL_RED], p->gamma[CHANNEL_GREEN], p->gamma[CHANNEL_BLUE] };
  const dt_aligned_pixel_t gain = { p->gain[CHANNEL_RED], p->gain[CHANNEL_GREEN], p->gain[CHANNEL_BLUE] };

  switch(d->mode)
  {
    case SLOPE_OFFSET_POWER:
    {
      // Correct the luminance in RGB parameters so we don't affect it
      dt_aligned_pixel_t XYZ;

      dt_prophotorgb_to_XYZ(lift, XYZ);
      d->lift[CHANNEL_FACTOR] = p->lift[CHANNEL_FACTOR];
      d->lift[CHANNEL_RED] = (p->lift[CHANNEL_RED] - XYZ[1]) + 1.f;
      d->lift[CHANNEL_GREEN] = (p->lift[CHANNEL_GREEN] - XYZ[1]) + 1.f;
      d->lift[CHANNEL_BLUE] = (p->lift[CHANNEL_BLUE] - XYZ[1]) + 1.f;

      dt_prophotorgb_to_XYZ(gamma, XYZ);
      d->gamma[CHANNEL_FACTOR] = p->gamma[CHANNEL_FACTOR];
      d->gamma[CHANNEL_RED] = (p->gamma[CHANNEL_RED] - XYZ[1]) + 1.f;
      d->gamma[CHANNEL_GREEN] = (p->gamma[CHANNEL_GREEN] - XYZ[1]) + 1.f;
      d->gamma[CHANNEL_BLUE] = (p->gamma[CHANNEL_BLUE] - XYZ[1]) + 1.f;

      dt_prophotorgb_to_XYZ(gain, XYZ);
      d->gain[CHANNEL_FACTOR] = p->gain[CHANNEL_FACTOR];
      d->gain[CHANNEL_RED] = (p->gain[CHANNEL_RED] - XYZ[1]) + 1.f;
      d->gain[CHANNEL_GREEN] = (p->gain[CHANNEL_GREEN] - XYZ[1]) + 1.f;
      d->gain[CHANNEL_BLUE] = (p->gain[CHANNEL_BLUE] - XYZ[1]) + 1.f;

      break;
    }

    case LEGACY:
    {
      // Luminance is not corrected in lift/gamma/gain for compatibility
      for(int i = 0; i < CHANNEL_SIZE; i++)
      {
        d->lift[i] = p->lift[i];
        d->gamma[i] = p->gamma[i];
        d->gain[i] = p->gain[i];
      }

      break;
    }

    case LIFT_GAMMA_GAIN:
    {
      // Correct the luminance in RGB parameters so we don't affect it
      dt_aligned_pixel_t XYZ;

      dt_prophotorgb_to_XYZ(lift, XYZ);
      d->lift[CHANNEL_FACTOR] = p->lift[CHANNEL_FACTOR];
      d->lift[CHANNEL_RED] = (p->lift[CHANNEL_RED] - XYZ[1]) + 1.f;
      d->lift[CHANNEL_GREEN] = (p->lift[CHANNEL_GREEN] - XYZ[1]) + 1.f;
      d->lift[CHANNEL_BLUE] = (p->lift[CHANNEL_BLUE] - XYZ[1]) + 1.f;

      dt_prophotorgb_to_XYZ(gamma, XYZ);
      d->gamma[CHANNEL_FACTOR] = p->gamma[CHANNEL_FACTOR];
      d->gamma[CHANNEL_RED] = (p->gamma[CHANNEL_RED] - XYZ[1]) + 1.f;
      d->gamma[CHANNEL_GREEN] = (p->gamma[CHANNEL_GREEN] - XYZ[1]) + 1.f;
      d->gamma[CHANNEL_BLUE] = (p->gamma[CHANNEL_BLUE] - XYZ[1]) + 1.f;

      dt_prophotorgb_to_XYZ(gain, XYZ);
      d->gain[CHANNEL_FACTOR] = p->gain[CHANNEL_FACTOR];
      d->gain[CHANNEL_RED] = (p->gain[CHANNEL_RED] - XYZ[1]) + 1.f;
      d->gain[CHANNEL_GREEN] = (p->gain[CHANNEL_GREEN] - XYZ[1]) + 1.f;
      d->gain[CHANNEL_BLUE] = (p->gain[CHANNEL_BLUE] - XYZ[1]) + 1.f;

      break;
    }
  }

  d->grey = p->grey;
  d->saturation = p->saturation;
  d->saturation_out = p->saturation_out;
  d->contrast = p->contrast;
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_colorbalance_data_t));
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void set_visible_widgets(dt_iop_colorbalance_gui_data_t *g)
{
  const int mode = dt_bauhaus_combobox_get(g->mode);
  const int control_mode = dt_bauhaus_combobox_get(g->controls);

  gtk_widget_set_visible(g->master_box, mode != LEGACY);

  dt_conf_set_string("plugins/darkroom/colorbalance/controls",
                     control_mode == RGBL ? "RGBL" :
                     control_mode == BOTH ? "BOTH" : "HSL");
  gboolean show_rgbl = (control_mode == RGBL) || (control_mode == BOTH);
  gboolean show_hsl  = (control_mode == HSL)  || (control_mode == BOTH);

  gtk_widget_set_visible(g->lift_r,  show_rgbl);
  gtk_widget_set_visible(g->lift_g,  show_rgbl);
  gtk_widget_set_visible(g->lift_b,  show_rgbl);
  gtk_widget_set_visible(g->gamma_r, show_rgbl);
  gtk_widget_set_visible(g->gamma_g, show_rgbl);
  gtk_widget_set_visible(g->gamma_b, show_rgbl);
  gtk_widget_set_visible(g->gain_r,  show_rgbl);
  gtk_widget_set_visible(g->gain_g,  show_rgbl);
  gtk_widget_set_visible(g->gain_b,  show_rgbl);

  gtk_widget_set_visible(g->hue_lift,  show_hsl);
  gtk_widget_set_visible(g->sat_lift,  show_hsl);
  gtk_widget_set_visible(g->hue_gamma, show_hsl);
  gtk_widget_set_visible(g->sat_gamma, show_hsl);
  gtk_widget_set_visible(g->hue_gain,  show_hsl);
  gtk_widget_set_visible(g->sat_gain,  show_hsl);

  gtk_widget_set_visible(g->optimizer_box, mode == SLOPE_OFFSET_POWER);
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_color_picker_reset(self, TRUE);
  _check_tuner_picker_labels(self);

  gui_changed(self, NULL, NULL);
}

void gui_reset(dt_iop_module_t *self)
{
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

  for(int k=0; k<LEVELS; k++)
  {
    g->color_patches_flags[k] = INVALID;
    g->luma_patches_flags[k] = INVALID;
  }
  _check_tuner_picker_labels(self);

  dt_bauhaus_combobox_set(g->controls, HSL);

  set_visible_widgets(g);

  dt_iop_color_picker_reset(self, TRUE);
}

static void _configure_slider_blocks(gpointer instance, dt_iop_module_t *self);

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

  if(!w || w == g->mode)
  {
    set_visible_widgets(g);
    _configure_slider_blocks(NULL, self);
  }

  ++darktable.gui->reset;

  if(!w || w == g->lift_r  || w == g->lift_g  || w == g->lift_b)
    set_HSL_sliders(g->hue_lift, g->sat_lift, p->lift);
  if(!w || w == g->gamma_r || w == g->gamma_g || w == g->gamma_b)
    set_HSL_sliders(g->hue_gamma, g->sat_gamma, p->gamma);
  if(!w || w == g->gain_r  || w == g->gain_g  || w == g->gain_b)
    set_HSL_sliders(g->hue_gain, g->sat_gain, p->gain);

  --darktable.gui->reset;
}

static void controls_callback(GtkWidget *combo, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

  set_visible_widgets(g);

  dt_iop_color_picker_reset(self, TRUE);
}

#ifdef SHOW_COLOR_WHEELS
static gboolean dt_iop_area_draw(GtkWidget *widget, cairo_t *cr, dt_iop_module_t *self)
{
  float flt_bg = 0.5;
  if(gtk_widget_get_state_flags(widget) & GTK_STATE_FLAG_SELECTED) flt_bg = 0.6;
  float flt_dark = flt_bg / 1.5, flt_light = flt_bg * 1.5;

  uint32_t bg = ((255 << 24) | ((int)floor(flt_bg * 255 + 0.5) << 16) | ((int)floor(flt_bg * 255 + 0.5) << 8)
                 | (int)floor(flt_bg * 255 + 0.5));
  // bg = 0xffffffff;
  //   uint32_t dark = ((255 << 24) |
  //                  ((int)floor(flt_dark * 255 + 0.5) << 16) |
  //                  ((int)floor(flt_dark * 255 + 0.5) << 8) |
  //                  (int)floor(flt_dark * 255 + 0.5));
  uint32_t light = ((255 << 24) | ((int)floor(flt_light * 255 + 0.5) << 16)
                    | ((int)floor(flt_light * 255 + 0.5) << 8) | (int)floor(flt_light * 255 + 0.5));

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  if(width % 2 == 0) width--;
  if(height % 2 == 0) height--;
  double center_x = (float)width / 2.0, center_y = (float)height / 2.0;
  double diameter = MIN(width, height) - 4;
  double r_outside = diameter / 2.0, r_inside = r_outside * 0.87;
  double r_outside_2 = r_outside * r_outside, r_inside_2 = r_inside * r_inside;

  // clear the background
  cairo_set_source_rgb(cr, flt_bg, flt_bg, flt_bg);
  cairo_paint(cr);

  /* Create an image initialized with the ring colors */
  gint stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, width);
  guint32 *buf = (guint32 *)malloc(sizeof(guint32) * height * stride / 4);

  for(int y = 0; y < height; y++)
  {
    guint32 *p = buf + y * width;

    double dy = -(y + 0.5 - center_y);

    for(int x = 0; x < width; x++)
    {
      double dx = x + 0.5 - center_x;
      double dist = dx * dx + dy * dy;
      if(dist < r_inside_2 || dist > r_outside_2)
      {
        uint32_t col = bg;
        if((abs(dx) < 1 && abs(dy) < 3) || (abs(dx) < 3 && abs(dy) < 1)) col = light;
        *p++ = col;
        continue;
      }

      double angle = atan2(dy, dx) - M_PI_2;
      if(angle < 0.0) angle += 2.0 * M_PI;

      double hue = angle / (2.0 * M_PI);

      dt_aligned_pixel_t rgb;
      hsl2rgb(rgb, hue, 1.0, 0.5);

      *p++ = (((int)floor(rgb[0] * 255 + 0.5) << 16) | ((int)floor(rgb[1] * 255 + 0.5) << 8)
              | (int)floor(rgb[2] * 255 + 0.5));
    }
  }

  cairo_surface_t *source
      = cairo_image_surface_create_for_data((unsigned char *)buf, CAIRO_FORMAT_RGB24, width, height, stride);

  cairo_set_source_surface(cr, source, 0.0, 0.0);
  cairo_paint(cr);
  free(buf);

  // draw border
  float line_width = 1;
  cairo_set_line_width(cr, line_width);

  cairo_set_source_rgb(cr, flt_bg, flt_bg, flt_bg);
  cairo_new_path(cr);
  cairo_arc(cr, center_x, center_y, r_outside, 0.0, 2.0 * M_PI);
  cairo_stroke(cr);
  cairo_arc(cr, center_x, center_y, r_inside, 0.0, 2.0 * M_PI);
  cairo_stroke(cr);

  cairo_set_source_rgb(cr, flt_dark, flt_dark, flt_dark);
  cairo_new_path(cr);
  cairo_arc(cr, center_x, center_y, r_outside, M_PI, 1.5 * M_PI);
  cairo_stroke(cr);
  cairo_arc(cr, center_x, center_y, r_inside, 0.0, 0.5 * M_PI);
  cairo_stroke(cr);

  cairo_set_source_rgb(cr, flt_light, flt_light, flt_light);
  cairo_new_path(cr);
  cairo_arc(cr, center_x, center_y, r_outside, 0.0, 0.5 * M_PI);
  cairo_stroke(cr);
  cairo_arc(cr, center_x, center_y, r_inside, M_PI, 1.5 * M_PI);
  cairo_stroke(cr);

  // draw selector
  double r = 255 / 255.0, g = 155 / 255.0, b = 40 / 255.0;
  double h, s, v;

  gtk_rgb_to_hsv(r, g, b, &h, &s, &v);

  cairo_save(cr);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.7);

  cairo_translate(cr, center_x, center_y);
  cairo_rotate(cr, h * 2.0 * M_PI - M_PI_2);

  cairo_arc(cr, r_inside * v, 0.0, 3.0, 0, 2.0 * M_PI);
  cairo_stroke(cr);

  cairo_restore(cr);

  cairo_surface_destroy(source);

  return TRUE;
}
#endif

static void _configure_slider_blocks(gpointer instance, dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

  GtkWidget *new_container = NULL;
  GtkWidget *old_container = gtk_bin_get_child(GTK_BIN(g->main_box));

  for(int i=0; i<3; i++)
  {
    g_object_ref(G_OBJECT(g->blocks[i]));
    if(old_container) gtk_container_remove(GTK_CONTAINER(old_container), g->blocks[i]);
  }

  if(old_container) gtk_widget_destroy(old_container);

  const gchar *short_label_ops[] = { C_("color", "offset"), C_("color", "power"), C_("color", "slope") };
  const gchar *short_label_lgg[] = { C_("color", "lift"), C_("color", "gamma"), C_("color", "gain") };
  const gchar **short_label = (p->mode == SLOPE_OFFSET_POWER) ? short_label_ops : short_label_lgg;
  const gchar *long_label[]
    = { NC_("section", "shadows: lift / offset"),
        NC_("section", "mid-tones: gamma / power"),
        NC_("section", "highlights: gain / slope") };

  gchar *layout = dt_conf_get_string("plugins/darkroom/colorbalance/layout");

  if(!g_strcmp0(layout, "list"))
  {
    new_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

    for(int i=0; i<3; i++)
    {
      if(i == 0)
        gtk_label_set_text(GTK_LABEL(g->main_label), _(long_label[0]));
      else
      {
        GtkWidget *label = dt_ui_section_label_new(Q_(long_label[i]));
        gtk_container_add(GTK_CONTAINER(new_container), label);
        if(old_container) gtk_widget_show(label);
      }

      gtk_container_add(GTK_CONTAINER(new_container), g->blocks[i]);
    }
  }
  else
  {
    gtk_label_set_text(GTK_LABEL(g->main_label), _("shadows / mid-tones / highlights"));

    GtkWidget *label[3];
    for(int i=0; i<3; i++)
    {
      label[i] = gtk_label_new(_(short_label[i]));
      gtk_widget_set_tooltip_text(label[i], _(long_label[i]));
      gtk_label_set_ellipsize(GTK_LABEL(label[i]), PANGO_ELLIPSIZE_END);
      gtk_widget_set_hexpand(label[i], TRUE);
    }

    if(!g_strcmp0(layout, "columns"))
    {
      new_container = gtk_grid_new();

      gtk_grid_set_column_homogeneous(GTK_GRID(new_container), TRUE);
      gtk_grid_set_column_spacing(GTK_GRID(new_container), 8);

      for(int i=0; i<3; i++)
      {
        dt_gui_add_class(label[i], "dt_section_label");

        gtk_container_add(GTK_CONTAINER(new_container), label[i]);
        if(old_container) gtk_widget_show(label[i]);
        gtk_grid_attach_next_to(GTK_GRID(new_container), g->blocks[i], label[i], GTK_POS_BOTTOM, 1, 1);
      }
    }
    else
    {
      new_container = gtk_notebook_new();

      for(int i=0; i<3; i++) gtk_notebook_append_page(GTK_NOTEBOOK(new_container), g->blocks[i], label[i]);
    }
  }

  g_free(layout);

  for(int i=0; i<3; i++) g_object_unref(G_OBJECT(g->blocks[i]));

  gtk_container_add(GTK_CONTAINER(g->main_box), new_container);
  if(old_container) gtk_widget_show(new_container);
}

static void _cycle_layout_callback(GtkWidget *label, GdkEventButton *event, dt_iop_module_t *self)
{
  gchar *layout = dt_conf_get_string("plugins/darkroom/colorbalance/layout");

  dt_conf_set_string("plugins/darkroom/colorbalance/layout",
                     !g_strcmp0(layout, "columns") ? "tabs" :
                     !g_strcmp0(layout, "list") ? "columns" : "list");

  g_free(layout);

  _configure_slider_blocks(NULL, self);
}

#define HSL_CALLBACK(which)                                                             \
static void which##_callback(GtkWidget *slider, gpointer user_data)                     \
{                                                                                       \
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;                                 \
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;       \
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data; \
                                                                                        \
  if(darktable.gui->reset) return;                                                      \
                                                                                        \
  dt_iop_color_picker_reset(self, TRUE);                                                \
                                                                                        \
  float hsl[3] = {dt_bauhaus_slider_get(g->hue_##which) / 360.0f,                       \
                  dt_bauhaus_slider_get(g->sat_##which) / 100.0f,                       \
                  0.5f};                                                                \
                                                                                        \
  if(slider == g->hue_##which)                                                          \
    update_saturation_slider_color(g->sat_##which, hsl[0]);                             \
  set_RGB_sliders(g->which##_r, g->which##_g, g->which##_b, hsl, p->which, p->mode);    \
  dt_dev_add_history_item(darktable.develop, self, TRUE);                               \
}

HSL_CALLBACK(lift)
HSL_CALLBACK(gamma)
HSL_CALLBACK(gain)

void gui_init(dt_iop_module_t *self)
{
  dt_iop_colorbalance_gui_data_t *g = IOP_GUI_ALLOC(colorbalance);

  g->mode = NULL;

  for(int k=0; k<LEVELS; k++)
  {
    g->color_patches_flags[k] = INVALID;
    g->luma_patches_flags[k] = INVALID;
  }

  GtkWidget *mode_box = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  // mode choice
  g->mode = dt_bauhaus_combobox_from_params(self, N_("mode"));
  gtk_widget_set_tooltip_text(g->mode, _("color-grading mapping method"));

  // control choice
  g->controls = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->controls, NULL, N_("color control sliders"));
  dt_bauhaus_combobox_add(g->controls, _("HSL"));
  dt_bauhaus_combobox_add(g->controls, _("RGBL"));
  dt_bauhaus_combobox_add(g->controls, _("both"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->controls), TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->controls, _("color-grading mapping method"));
  g_signal_connect(G_OBJECT(g->controls), "value-changed", G_CALLBACK(controls_callback), self);

  const char *mode = dt_conf_get_string_const("plugins/darkroom/colorbalance/controls");
  dt_bauhaus_combobox_set(g->controls, !g_strcmp0(mode, "RGBL") ? RGBL :
                                       !g_strcmp0(mode, "BOTH") ? BOTH : HSL);

  g->master_box = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  gtk_box_pack_start(GTK_BOX(g->master_box), dt_ui_section_label_new(C_("section", "master")), FALSE, FALSE, 0);

  g->saturation = dt_bauhaus_slider_from_params(self, "saturation");
  dt_bauhaus_slider_set_soft_range(g->saturation, 0.5f, 1.5f);
  dt_bauhaus_slider_set_digits(g->saturation, 4);
  dt_bauhaus_slider_set_format(g->saturation, "%");
  gtk_widget_set_tooltip_text(g->saturation, _("saturation correction before the color balance"));

  g->saturation_out = dt_bauhaus_slider_from_params(self, "saturation_out");
  dt_bauhaus_slider_set_soft_range(g->saturation_out, 0.5f, 1.5f);
  dt_bauhaus_slider_set_digits(g->saturation_out, 4);
  dt_bauhaus_slider_set_format(g->saturation_out, "%");
  gtk_widget_set_tooltip_text(g->saturation_out, _("saturation correction after the color balance"));

  g->grey = dt_color_picker_new(self, DT_COLOR_PICKER_AREA,
            dt_bauhaus_slider_from_params(self, "grey"));
  dt_bauhaus_slider_set_format(g->grey, "%");
  gtk_widget_set_tooltip_text(g->grey, _("adjust to match a neutral tone"));

  g->contrast = dt_bauhaus_slider_from_params(self, N_("contrast"));
  dt_bauhaus_slider_set_soft_range(g->contrast, 0.5f, 1.5f);
  dt_bauhaus_slider_set_digits(g->contrast, 4);
  dt_bauhaus_slider_set_factor(g->contrast, -100.0f);
  dt_bauhaus_slider_set_offset(g->contrast, 100.0f);
  dt_bauhaus_slider_set_format(g->contrast, "%");
  gtk_widget_set_tooltip_text(g->contrast, _("contrast"));

#ifdef SHOW_COLOR_WHEELS
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, FALSE, FALSE, 0);

  GtkWidget *area = dtgtk_drawing_area_new_with_aspect_ratio(1.0);
  gtk_box_pack_start(GTK_BOX(hbox), area, TRUE, TRUE, 0);

  //   gtk_widget_add_events(g->area,
  //                         GDK_POINTER_MOTION_MASK |
  //                         GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect(G_OBJECT(area), "draw", G_CALLBACK(dt_iop_area_draw), self);
  //   g_signal_connect (G_OBJECT (area), "button-press-event",
  //                     G_CALLBACK (dt_iop_colorbalance_button_press), self);
  //   g_signal_connect (G_OBJECT (area), "motion-notify-event",
  //                     G_CALLBACK (dt_iop_colorbalance_motion_notify), self);
  //   g_signal_connect (G_OBJECT (area), "leave-notify-event",
  //                     G_CALLBACK (dt_iop_colorbalance_leave_notify), self);

  area = dtgtk_drawing_area_new_with_aspect_ratio(1.0);
  gtk_box_pack_start(GTK_BOX(hbox), area, TRUE, TRUE, 0);

  //   gtk_widget_add_events(g->area,
  //                         GDK_POINTER_MOTION_MASK |
  //                         GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect(G_OBJECT(area), "draw", G_CALLBACK(dt_iop_area_draw), self);
  //   g_signal_connect (G_OBJECT (area), "button-press-event",
  //                     G_CALLBACK (dt_iop_colorbalance_button_press), self);
  //   g_signal_connect (G_OBJECT (area), "motion-notify-event",
  //                     G_CALLBACK (dt_iop_colorbalance_motion_notify), self);
  //   g_signal_connect (G_OBJECT (area), "leave-notify-event",
  //                     G_CALLBACK (dt_iop_colorbalance_leave_notify), self);

  area = dtgtk_drawing_area_new_with_aspect_ratio(1.0);
  gtk_box_pack_start(GTK_BOX(hbox), area, TRUE, TRUE, 0);

  //   gtk_widget_add_events(g->area,
  //                         GDK_POINTER_MOTION_MASK |
  //                         GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect(G_OBJECT(area), "draw", G_CALLBACK(dt_iop_area_draw), self);
//   g_signal_connect (G_OBJECT (area), "button-press-event",
//                     G_CALLBACK (dt_iop_colorbalance_button_press), self);
//   g_signal_connect (G_OBJECT (area), "motion-notify-event",
//                     G_CALLBACK (dt_iop_colorbalance_motion_notify), self);
//   g_signal_connect (G_OBJECT (area), "leave-notify-event",
//                     G_CALLBACK (dt_iop_colorbalance_leave_notify), self);
#endif

  g->main_label = dt_ui_section_label_new(""); // is set in _configure_slider_blocks
  gtk_widget_set_tooltip_text(g->main_label, _("click to cycle layout"));
  GtkWidget *main_label_box = gtk_event_box_new();
  gtk_container_add(GTK_CONTAINER(main_label_box), g->main_label);
  g_signal_connect(G_OBJECT(main_label_box), "button-release-event", G_CALLBACK(_cycle_layout_callback), self);

  g->main_box = gtk_event_box_new(); // is filled in _configure_slider_blocks

  char field_name[10];

#define ADD_CHANNEL(which, section, c, n, N, text, span)                    \
  sprintf(field_name, "%s[%d]", #which, CHANNEL_##N);                       \
  g->which##_##c = dt_bauhaus_slider_from_params(self, field_name);         \
  dt_bauhaus_slider_set_soft_range(g->which##_##c, -span+1.0, span+1.0);    \
  dt_bauhaus_slider_set_digits(g->which##_##c, 5);                          \
  dt_bauhaus_slider_set_offset(g->which##_##c, -1.0);                       \
  dt_bauhaus_slider_set_feedback(g->which##_##c, 0);                        \
  gtk_widget_set_tooltip_text(g->which##_##c, _(text[CHANNEL_##N]));        \
  dt_bauhaus_widget_set_label(g->which##_##c, section, #n);                 \

#define ADD_BLOCK(blk, which, section, text, span, satspan)                 \
  g->blocks[blk] = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); \
                                                                            \
  sprintf(field_name, "%s[%d]", #which, CHANNEL_FACTOR);                    \
  g->which##_factor = dt_color_picker_new(self, DT_COLOR_PICKER_AREA,       \
                      dt_bauhaus_slider_from_params(self, field_name));     \
  dt_bauhaus_slider_set_soft_range(g->which##_factor, -span+1.0, span+1.0); \
  dt_bauhaus_slider_set_digits(g->which##_factor, 4);                       \
  dt_bauhaus_slider_set_factor(g->which##_factor, 100.0);                   \
  dt_bauhaus_slider_set_offset(g->which##_factor, - 100.0);                 \
  dt_bauhaus_slider_set_format(g->which##_factor,"%");                      \
  dt_bauhaus_slider_set_feedback(g->which##_factor, 0);                     \
  dt_bauhaus_slider_set_stop(g->which##_factor, 0.0, 0.0, 0.0, 0.0);        \
  dt_bauhaus_slider_set_stop(g->which##_factor, 1.0, 1.0, 1.0, 1.0);        \
  gtk_widget_set_tooltip_text(g->which##_factor, _(text[CHANNEL_FACTOR]));  \
  dt_bauhaus_widget_set_label(g->which##_factor, section, N_("factor"));    \
                                                                            \
  g->hue_##which = dt_color_picker_new(self, DT_COLOR_PICKER_AREA,          \
                   dt_bauhaus_slider_new_with_range_and_feedback(self,      \
                   0.0f, 360.0f, 0, 0.0f, 2, 0));                           \
  dt_bauhaus_widget_set_label(g->hue_##which, section, N_("hue"));          \
  dt_bauhaus_slider_set_format(g->hue_##which, "°");                        \
  dt_bauhaus_slider_set_stop(g->hue_##which, 0.0f,   1.0f, 0.0f, 0.0f);     \
  dt_bauhaus_slider_set_stop(g->hue_##which, 0.166f, 1.0f, 1.0f, 0.0f);     \
  dt_bauhaus_slider_set_stop(g->hue_##which, 0.322f, 0.0f, 1.0f, 0.0f);     \
  dt_bauhaus_slider_set_stop(g->hue_##which, 0.498f, 0.0f, 1.0f, 1.0f);     \
  dt_bauhaus_slider_set_stop(g->hue_##which, 0.664f, 0.0f, 0.0f, 1.0f);     \
  dt_bauhaus_slider_set_stop(g->hue_##which, 0.830f, 1.0f, 0.0f, 1.0f);     \
  dt_bauhaus_slider_set_stop(g->hue_##which, 1.0f,   1.0f, 0.0f, 0.0f);     \
  gtk_widget_set_tooltip_text(g->hue_##which, _("select the hue"));         \
  g_signal_connect(G_OBJECT(g->hue_##which), "value-changed",               \
                   G_CALLBACK(which##_callback), self);                     \
  gtk_box_pack_start(GTK_BOX(self->widget), g->hue_##which, TRUE, TRUE, 0); \
                                                                            \
  g->sat_##which = dt_bauhaus_slider_new_with_range_and_feedback(self,      \
                   0.0f, 100.0f, 0, 0.0f, 2, 0);                            \
  dt_bauhaus_slider_set_soft_max(g->sat_##which, satspan);                  \
  dt_bauhaus_widget_set_label(g->sat_##which, section, N_("saturation"));   \
  dt_bauhaus_slider_set_format(g->sat_##which, "%");                        \
  dt_bauhaus_slider_set_stop(g->sat_##which, 0.0f, 0.2f, 0.2f, 0.2f);       \
  dt_bauhaus_slider_set_stop(g->sat_##which, 1.0f, 1.0f, 1.0f, 1.0f);       \
  gtk_widget_set_tooltip_text(g->sat_##which, _("select the saturation"));  \
  g_signal_connect(G_OBJECT(g->sat_##which), "value-changed",               \
                   G_CALLBACK(which##_callback), self);                     \
  gtk_box_pack_start(GTK_BOX(self->widget), g->sat_##which, TRUE, TRUE, 0); \
                                                                            \
  ADD_CHANNEL(which, section, r, red, RED, text, span)                      \
  dt_bauhaus_slider_set_stop(g->which##_r, 0.0, 0.0, 1.0, 1.0);             \
  dt_bauhaus_slider_set_stop(g->which##_r, 0.5, 1.0, 1.0, 1.0);             \
  dt_bauhaus_slider_set_stop(g->which##_r, 1.0, 1.0, 0.0, 0.0);             \
  ADD_CHANNEL(which, section, g, green, GREEN, text, span)                  \
  dt_bauhaus_slider_set_stop(g->which##_g, 0.0, 1.0, 0.0, 1.0);             \
  dt_bauhaus_slider_set_stop(g->which##_g, 0.5, 1.0, 1.0, 1.0);             \
  dt_bauhaus_slider_set_stop(g->which##_g, 1.0, 0.0, 1.0, 0.0);             \
  ADD_CHANNEL(which, section, b, blue, BLUE, text, span)                    \
  dt_bauhaus_slider_set_stop(g->which##_b, 0.0, 1.0, 1.0, 0.0);             \
  dt_bauhaus_slider_set_stop(g->which##_b, 0.5, 1.0, 1.0, 1.0);             \
  dt_bauhaus_slider_set_stop(g->which##_b, 1.0, 0.0, 0.0, 1.0);             \

  static const char *lift_messages[]
    = { N_("factor of lift/offset"),
        N_("factor of red for lift/offset"),
        N_("factor of green for lift/offset"),
        N_("factor of blue for lift/offset") };

  static const char *gamma_messages[]
    = { N_("factor of gamma/power"),
        N_("factor of red for gamma/power"),
        N_("factor of green for gamma/power"),
        N_("factor of blue for gamma/power") };

  static const char *gain_messages[]
    = { N_("factor of gain/slope"),
        N_("factor of red for gain/slope"),
        N_("factor of green for gain/slope"),
        N_("factor of blue for gain/slope") };

  ADD_BLOCK(0, lift,  N_("shadows"), lift_messages, 0.05f,  5.0f)
  ADD_BLOCK(1, gamma, N_("mid-tones"), gamma_messages, 0.5f, 20.0f)
  ADD_BLOCK(2, gain,  N_("highlights"), gain_messages,  0.5f, 25.0f)
  _configure_slider_blocks(NULL, self);

  g->optimizer_box = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(C_("section", "auto optimizers")), FALSE, FALSE, 0);

  g->auto_luma = dt_color_picker_new(self, DT_COLOR_PICKER_AREA,
                 dt_bauhaus_combobox_new(self));
  dt_bauhaus_widget_set_label(g->auto_luma, NULL, N_("optimize luma"));
  gtk_widget_set_tooltip_text(g->auto_luma, _("fit the whole histogram and center the average luma"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->auto_luma, FALSE, FALSE, 0);

  g->auto_color = dt_color_picker_new(self, DT_COLOR_PICKER_AREA,
                  dt_bauhaus_combobox_new(self));
  dt_bauhaus_widget_set_label(g->auto_color, NULL, N_("neutralize colors"));
  gtk_widget_set_tooltip_text(g->auto_color, _("optimize the RGB curves to remove color casts"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->auto_color, FALSE, FALSE, 0);

  // start building top level widget
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(mode_box), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->master_box), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(main_label_box), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->main_box), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->optimizer_box), TRUE, TRUE, 0);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_PREFERENCES_CHANGE,
                                  G_CALLBACK(_configure_slider_blocks), (gpointer)self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_configure_slider_blocks), self);

  IOP_GUI_FREE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
