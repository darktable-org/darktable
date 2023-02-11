/*
    This file is part of darktable,
    Copyright (C) 2012-2021 darktable developers.

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
#include "develop/blend.h"
#include "bauhaus/bauhaus.h"
#include "common/debug.h"
#include "common/dtpthread.h"
#include "common/math.h"
#include "common/opencl.h"
#include "common/iop_profile.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/masks.h"
#include "develop/tiling.h"
#include "dtgtk/button.h"
#include "dtgtk/gradientslider.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"

#include <assert.h>
#include <gmodule.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define NEUTRAL_GRAY 0.5

const dt_develop_name_value_t dt_develop_blend_mode_names[]
    = { { NC_("blendmode", "Normal"), DEVELOP_BLEND_NORMAL2 },
        { NC_("blendmode", "Normal bounded"), DEVELOP_BLEND_BOUNDED },
        { NC_("blendmode", "Lighten"), DEVELOP_BLEND_LIGHTEN },
        { NC_("blendmode", "Darken"), DEVELOP_BLEND_DARKEN },
        { NC_("blendmode", "Multiply"), DEVELOP_BLEND_MULTIPLY },
        { NC_("blendmode", "Average"), DEVELOP_BLEND_AVERAGE },
        { NC_("blendmode", "Addition"), DEVELOP_BLEND_ADD },
        { NC_("blendmode", "Subtract"), DEVELOP_BLEND_SUBTRACT },
        { NC_("blendmode", "Difference"), DEVELOP_BLEND_DIFFERENCE2 },
        { NC_("blendmode", "Screen"), DEVELOP_BLEND_SCREEN },
        { NC_("blendmode", "Overlay"), DEVELOP_BLEND_OVERLAY },
        { NC_("blendmode", "Softlight"), DEVELOP_BLEND_SOFTLIGHT },
        { NC_("blendmode", "Hardlight"), DEVELOP_BLEND_HARDLIGHT },
        { NC_("blendmode", "Vividlight"), DEVELOP_BLEND_VIVIDLIGHT },
        { NC_("blendmode", "Linearlight"), DEVELOP_BLEND_LINEARLIGHT },
        { NC_("blendmode", "Pinlight"), DEVELOP_BLEND_PINLIGHT },
        { NC_("blendmode", "Lightness"), DEVELOP_BLEND_LIGHTNESS },
        { NC_("blendmode", "Chromaticity"), DEVELOP_BLEND_CHROMATICITY },
        { NC_("blendmode", "Hue"), DEVELOP_BLEND_HUE },
        { NC_("blendmode", "Color"), DEVELOP_BLEND_COLOR },
        { NC_("blendmode", "Coloradjustment"), DEVELOP_BLEND_COLORADJUST },
        { NC_("blendmode", "Lab lightness"), DEVELOP_BLEND_LAB_LIGHTNESS },
        { NC_("blendmode", "Lab color"), DEVELOP_BLEND_LAB_COLOR },
        { NC_("blendmode", "Lab L-channel"), DEVELOP_BLEND_LAB_L },
        { NC_("blendmode", "Lab a-channel"), DEVELOP_BLEND_LAB_A },
        { NC_("blendmode", "Lab b-channel"), DEVELOP_BLEND_LAB_B },
        { NC_("blendmode", "HSV value"), DEVELOP_BLEND_HSV_VALUE },
        { NC_("blendmode", "HSV color"), DEVELOP_BLEND_HSV_COLOR },
        { NC_("blendmode", "RGB red channel"), DEVELOP_BLEND_RGB_R },
        { NC_("blendmode", "RGB green channel"), DEVELOP_BLEND_RGB_G },
        { NC_("blendmode", "RGB blue channel"), DEVELOP_BLEND_RGB_B },
        { NC_("blendmode", "Divide"), DEVELOP_BLEND_DIVIDE },
        { NC_("blendmode", "Geometric mean"), DEVELOP_BLEND_GEOMETRIC_MEAN },
        { NC_("blendmode", "Harmonic mean"), DEVELOP_BLEND_HARMONIC_MEAN },

        /** deprecated blend modes: make them available as legacy history stacks might want them */
        { NC_("blendmode", "Difference (deprecated)"), DEVELOP_BLEND_DIFFERENCE },
        { NC_("blendmode", "Subtract inverse (deprecated)"), DEVELOP_BLEND_SUBTRACT_INVERSE },
        { NC_("blendmode", "Divide inverse (deprecated)"), DEVELOP_BLEND_DIVIDE_INVERSE },
        { "", 0 } };

const dt_develop_name_value_t dt_develop_blend_mode_flag_names[]
    = { { NC_("blendoperation", "Normal"), 0 },
        { NC_("blendoperation", "Reverse"), DEVELOP_BLEND_REVERSE },
        { "", 0 } };

const dt_develop_name_value_t dt_develop_blend_colorspace_names[]
    = { { N_("Default"), DEVELOP_BLEND_CS_NONE },
        { N_("RAW"), DEVELOP_BLEND_CS_RAW },
        { N_("Lab"), DEVELOP_BLEND_CS_LAB },
        { N_("RGB (display)"), DEVELOP_BLEND_CS_RGB_DISPLAY },
        { N_("RGB (scene)"), DEVELOP_BLEND_CS_RGB_SCENE },
        { "", 0 } };

const dt_develop_name_value_t dt_develop_mask_mode_names[]
    = { { N_("Off"), DEVELOP_MASK_DISABLED },
        { N_("Uniformly"), DEVELOP_MASK_ENABLED },
        { N_("Drawn mask"), DEVELOP_MASK_MASK | DEVELOP_MASK_ENABLED },
        { N_("Parametric mask"), DEVELOP_MASK_CONDITIONAL | DEVELOP_MASK_ENABLED },
        { N_("Raster mask"), DEVELOP_MASK_RASTER | DEVELOP_MASK_ENABLED },
        { N_("Drawn & parametric mask"), DEVELOP_MASK_MASK_CONDITIONAL | DEVELOP_MASK_ENABLED },
        { "", 0 } };

const dt_develop_name_value_t dt_develop_combine_masks_names[]
    = { { N_("Exclusive"), DEVELOP_COMBINE_NORM_EXCL },
        { N_("Inclusive"), DEVELOP_COMBINE_NORM_INCL },
        { N_("Exclusive & inverted"), DEVELOP_COMBINE_INV_EXCL },
        { N_("Inclusive & inverted"), DEVELOP_COMBINE_INV_INCL },
        { "", 0 } };

const dt_develop_name_value_t dt_develop_feathering_guide_names[]
    = { { N_("Output before blur"), DEVELOP_MASK_GUIDE_OUT_BEFORE_BLUR },
        { N_("Input before blur"), DEVELOP_MASK_GUIDE_IN_BEFORE_BLUR },
        { N_("Output after blur"), DEVELOP_MASK_GUIDE_OUT_AFTER_BLUR },
        { N_("Input after blur"), DEVELOP_MASK_GUIDE_IN_AFTER_BLUR },
        { "", 0 } };

const dt_develop_name_value_t dt_develop_invert_mask_names[]
    = { { N_("Off"), DEVELOP_COMBINE_NORM },
        { N_("On"), DEVELOP_COMBINE_INV },
        { "", 0 } };

const dt_iop_gui_blendif_colorstop_t _gradient_L[]
    = { { 0.0f,   { 0, 0, 0, 1.0 } },
        { 0.125f, { NEUTRAL_GRAY / 8, NEUTRAL_GRAY / 8, NEUTRAL_GRAY / 8, 1.0 } },
        { 0.25f,  { NEUTRAL_GRAY / 4, NEUTRAL_GRAY / 4, NEUTRAL_GRAY / 4, 1.0 } },
        { 0.5f,   { NEUTRAL_GRAY / 2, NEUTRAL_GRAY / 2, NEUTRAL_GRAY / 2, 1.0 } },
        { 1.0f,   { NEUTRAL_GRAY, NEUTRAL_GRAY, NEUTRAL_GRAY, 1.0 } } };

// The values for "a" are generated in the following way:
//   Lab (with L=[90 to 68], b=0, and a=[-56 to 56] -> sRGB (D65 linear) -> normalize with MAX(R,G,B) = 0.75
const dt_iop_gui_blendif_colorstop_t _gradient_a[] = {
    { 0.000f, { 0.0112790f, 0.7500000f, 0.5609999f, 1.0f } },
    { 0.250f, { 0.2888855f, 0.7500000f, 0.6318934f, 1.0f } },
    { 0.375f, { 0.4872486f, 0.7500000f, 0.6825501f, 1.0f } },
    { 0.500f, { 0.7500000f, 0.7499399f, 0.7496052f, 1.0f } },
    { 0.625f, { 0.7500000f, 0.5054633f, 0.5676756f, 1.0f } },
    { 0.750f, { 0.7500000f, 0.3423850f, 0.4463195f, 1.0f } },
    { 1.000f, { 0.7500000f, 0.1399815f, 0.2956989f, 1.0f } },
};

// The values for "b" are generated in the following way:
//   Lab (with L=[58 to 62], a=0, and b=[-65 to 65] -> sRGB (D65 linear) -> normalize with MAX(R,G,B) = 0.75
const dt_iop_gui_blendif_colorstop_t _gradient_b[] = {
    { 0.000f, { 0.0162050f, 0.1968228f, 0.7500000f, 1.0f } },
    { 0.250f, { 0.2027354f, 0.3168822f, 0.7500000f, 1.0f } },
    { 0.375f, { 0.3645722f, 0.4210476f, 0.7500000f, 1.0f } },
    { 0.500f, { 0.6167146f, 0.5833379f, 0.7500000f, 1.0f } },
    { 0.625f, { 0.7500000f, 0.6172369f, 0.5412091f, 1.0f } },
    { 0.750f, { 0.7500000f, 0.5590797f, 0.3071980f, 1.0f } },
    { 1.000f, { 0.7500000f, 0.4963975f, 0.0549797f, 1.0f } },
};

const dt_iop_gui_blendif_colorstop_t _gradient_gray[]
    = { { 0.0f,   { 0, 0, 0, 1.0 } },
        { 0.125f, { NEUTRAL_GRAY / 8, NEUTRAL_GRAY / 8, NEUTRAL_GRAY / 8, 1.0 } },
        { 0.25f,  { NEUTRAL_GRAY / 4, NEUTRAL_GRAY / 4, NEUTRAL_GRAY / 4, 1.0 } },
        { 0.5f,   { NEUTRAL_GRAY / 2, NEUTRAL_GRAY / 2, NEUTRAL_GRAY / 2, 1.0 } },
        { 1.0f,   { NEUTRAL_GRAY, NEUTRAL_GRAY, NEUTRAL_GRAY, 1.0 } } };

const dt_iop_gui_blendif_colorstop_t _gradient_red[] = {
    { 0.000f, { 0.0000000f, 0.0000000f, 0.0000000f, 1.0f } },
    { 0.125f, { 0.0937500f, 0.0000000f, 0.0000000f, 1.0f } },
    { 0.250f, { 0.1875000f, 0.0000000f, 0.0000000f, 1.0f } },
    { 0.500f, { 0.3750000f, 0.0000000f, 0.0000000f, 1.0f } },
    { 1.000f, { 0.7500000f, 0.0000000f, 0.0000000f, 1.0f } }
};

const dt_iop_gui_blendif_colorstop_t _gradient_green[] = {
    { 0.000f, { 0.0000000f, 0.0000000f, 0.0000000f, 1.0f } },
    { 0.125f, { 0.0000000f, 0.0937500f, 0.0000000f, 1.0f } },
    { 0.250f, { 0.0000000f, 0.1875000f, 0.0000000f, 1.0f } },
    { 0.500f, { 0.0000000f, 0.3750000f, 0.0000000f, 1.0f } },
    { 1.000f, { 0.0000000f, 0.7500000f, 0.0000000f, 1.0f } }
};

const dt_iop_gui_blendif_colorstop_t _gradient_blue[] = {
    { 0.000f, { 0.0000000f, 0.0000000f, 0.0000000f, 1.0f } },
    { 0.125f, { 0.0000000f, 0.0000000f, 0.0937500f, 1.0f } },
    { 0.250f, { 0.0000000f, 0.0000000f, 0.1875000f, 1.0f } },
    { 0.500f, { 0.0000000f, 0.0000000f, 0.3750000f, 1.0f } },
    { 1.000f, { 0.0000000f, 0.0000000f, 0.7500000f, 1.0f } }
};

// The chroma values are displayed in a gradient from {0.5,0.5,0.5} to {0.5,0.0,0.5} (pink)
const dt_iop_gui_blendif_colorstop_t _gradient_chroma[] = {
    { 0.000f, { 0.5000000f, 0.5000000f, 0.5000000f, 1.0f } },
    { 0.125f, { 0.5000000f, 0.4375000f, 0.5000000f, 1.0f } },
    { 0.250f, { 0.5000000f, 0.3750000f, 0.5000000f, 1.0f } },
    { 0.500f, { 0.5000000f, 0.2500000f, 0.5000000f, 1.0f } },
    { 1.000f, { 0.5000000f, 0.0000000f, 0.5000000f, 1.0f } }
};

// The hue values for LCh are generated in the following way:
//   LCh (with L=65 and C=37) -> sRGB (D65 linear) -> normalize with MAX(R,G,B) = 0.75
// Please keep in sync with the display in the gamma module
const dt_iop_gui_blendif_colorstop_t _gradient_LCh_hue[] = {
    { 0.000f, { 0.7500000f, 0.2200405f, 0.4480174f, 1.0f } },
    { 0.104f, { 0.7500000f, 0.2475123f, 0.2488547f, 1.0f } },
    { 0.200f, { 0.7500000f, 0.3921083f, 0.2017670f, 1.0f } },
    { 0.295f, { 0.7500000f, 0.7440329f, 0.3011876f, 1.0f } },
    { 0.377f, { 0.3813996f, 0.7500000f, 0.3799668f, 1.0f } },
    { 0.503f, { 0.0747526f, 0.7500000f, 0.7489037f, 1.0f } },
    { 0.650f, { 0.0282981f, 0.3736209f, 0.7500000f, 1.0f } },
    { 0.803f, { 0.2583821f, 0.2591069f, 0.7500000f, 1.0f } },
    { 0.928f, { 0.7500000f, 0.2788102f, 0.7492077f, 1.0f } },
    { 1.000f, { 0.7500000f, 0.2200405f, 0.4480174f, 1.0f } },
};

// The hue values for HSL are generated in the following way:
//   HSL (with S=0.5 and L=0.5) -> any RGB(linear) -> (normalize with MAX(R,G,B) = 0.75)
// Please keep in sync with the display in the gamma module
const dt_iop_gui_blendif_colorstop_t _gradient_HSL_hue[] = {
    { 0.000f, { 0.7500000f, 0.2500000f, 0.2500000f, 1.0f } },
    { 0.167f, { 0.7500000f, 0.7500000f, 0.2500000f, 1.0f } },
    { 0.333f, { 0.2500000f, 0.7500000f, 0.2500000f, 1.0f } },
    { 0.500f, { 0.2500000f, 0.7500000f, 0.7500000f, 1.0f } },
    { 0.667f, { 0.2500000f, 0.2500000f, 0.7500000f, 1.0f } },
    { 0.833f, { 0.7500000f, 0.2500000f, 0.7500000f, 1.0f } },
    { 1.000f, { 0.7500000f, 0.2500000f, 0.2500000f, 1.0f } },
};

// The hue values for JzCzhz are generated in the following way:
//   JzCzhz (with Jz=0.011 and Cz=0.01) -> sRGB(D65 linear) -> normalize with MAX(R,G,B) = 0.75
// Please keep in sync with the display in the gamma module
const dt_iop_gui_blendif_colorstop_t _gradient_JzCzhz_hue[] = {
    { 0.000f, { 0.7500000f, 0.1946971f, 0.3697612f, 1.0f } },
    { 0.082f, { 0.7500000f, 0.2278141f, 0.2291548f, 1.0f } },
    { 0.150f, { 0.7500000f, 0.3132381f, 0.1653960f, 1.0f } },
    { 0.275f, { 0.7483232f, 0.7500000f, 0.1939316f, 1.0f } },
    { 0.378f, { 0.2642865f, 0.7500000f, 0.2642768f, 1.0f } },
    { 0.570f, { 0.0233180f, 0.7493543f, 0.7500000f, 1.0f } },
    { 0.650f, { 0.1119025f, 0.5116763f, 0.7500000f, 1.0f } },
    { 0.762f, { 0.3331225f, 0.3337235f, 0.7500000f, 1.0f } },
    { 0.883f, { 0.7464700f, 0.2754816f, 0.7500000f, 1.0f } },
    { 1.000f, { 0.7500000f, 0.1946971f, 0.3697612f, 1.0f } },
};

enum _channel_indexes
{
  CHANNEL_INDEX_L = 0,
  CHANNEL_INDEX_a = 1,
  CHANNEL_INDEX_b = 2,
  CHANNEL_INDEX_C = 3,
  CHANNEL_INDEX_h = 4,
  CHANNEL_INDEX_g = 0,
  CHANNEL_INDEX_R = 1,
  CHANNEL_INDEX_G = 2,
  CHANNEL_INDEX_B = 3,
  CHANNEL_INDEX_H = 4,
  CHANNEL_INDEX_S = 5,
  CHANNEL_INDEX_l = 6,
  CHANNEL_INDEX_Jz = 4,
  CHANNEL_INDEX_Cz = 5,
  CHANNEL_INDEX_hz = 6,
};

static void _blendop_blendif_update_tab(dt_iop_module_t *module, const int tab);

static inline dt_iop_colorspace_type_t _blendif_colorpicker_cst(dt_iop_gui_blend_data_t *data)
{
  dt_iop_colorspace_type_t cst = dt_iop_color_picker_get_active_cst(data->module);
  if(cst == IOP_CS_NONE)
  {
    switch(data->channel_tabs_csp)
    {
      case DEVELOP_BLEND_CS_LAB:
        cst = IOP_CS_LAB;
        break;
      case DEVELOP_BLEND_CS_RGB_DISPLAY:
      case DEVELOP_BLEND_CS_RGB_SCENE:
        cst = IOP_CS_RGB;
        break;
      case DEVELOP_BLEND_CS_RAW:
      case DEVELOP_BLEND_CS_NONE:
        cst = IOP_CS_NONE;
        break;
    }
  }
  return cst;
}

static gboolean _blendif_blend_parameter_enabled(dt_develop_blend_colorspace_t csp, dt_develop_blend_mode_t mode)
{
  if(csp == DEVELOP_BLEND_CS_RGB_SCENE)
  {
    switch(mode & ~DEVELOP_BLEND_REVERSE)
    {
      case DEVELOP_BLEND_ADD:
      case DEVELOP_BLEND_MULTIPLY:
      case DEVELOP_BLEND_SUBTRACT:
      case DEVELOP_BLEND_SUBTRACT_INVERSE:
      case DEVELOP_BLEND_DIVIDE:
      case DEVELOP_BLEND_DIVIDE_INVERSE:
      case DEVELOP_BLEND_RGB_R:
      case DEVELOP_BLEND_RGB_G:
      case DEVELOP_BLEND_RGB_B:
        return TRUE;
      default:
        return FALSE;
    }
  }
  return FALSE;
}

static inline float _get_boost_factor(const dt_iop_gui_blend_data_t *data, const int channel, const int in_out)
{
  return exp2f(data->module->blend_params->blendif_boost_factors[data->channel[channel].param_channels[in_out]]);
}

static void _blendif_scale(dt_iop_gui_blend_data_t *data, dt_iop_colorspace_type_t cst, const float *in,
                           float *out, const dt_iop_order_iccprofile_info_t *work_profile, int in_out)
{
  out[0] = out[1] = out[2] = out[3] = out[4] = out[5] = out[6] = out[7] = -1.0f;

  switch(cst)
  {
    case IOP_CS_LAB:
      out[CHANNEL_INDEX_L] = (in[0] / _get_boost_factor(data, 0, in_out)) / 100.0f;
      out[CHANNEL_INDEX_a] = ((in[1] / _get_boost_factor(data, 1, in_out)) + 128.0f) / 256.0f;
      out[CHANNEL_INDEX_b] = ((in[2] / _get_boost_factor(data, 2, in_out)) + 128.0f) / 256.0f;
      break;
    case IOP_CS_RGB:
      if(work_profile == NULL)
        out[CHANNEL_INDEX_g] = 0.3f * in[0] + 0.59f * in[1] + 0.11f * in[2];
      else
        out[CHANNEL_INDEX_g] = dt_ioppr_get_rgb_matrix_luminance(in, work_profile->matrix_in,
                                                                 work_profile->lut_in,
                                                                 work_profile->unbounded_coeffs_in,
                                                                 work_profile->lutsize,
                                                                 work_profile->nonlinearlut);
      out[CHANNEL_INDEX_g] = out[CHANNEL_INDEX_g] / _get_boost_factor(data, 0, in_out);
      out[CHANNEL_INDEX_R] = in[0] / _get_boost_factor(data, 1, in_out);
      out[CHANNEL_INDEX_G] = in[1] / _get_boost_factor(data, 2, in_out);
      out[CHANNEL_INDEX_B] = in[2] / _get_boost_factor(data, 3, in_out);
      break;
    case IOP_CS_LCH:
      out[CHANNEL_INDEX_C] = (in[1] / _get_boost_factor(data, 3, in_out)) / (128.0f * sqrtf(2.0f));
      out[CHANNEL_INDEX_h] = in[2] / _get_boost_factor(data, 4, in_out);
      break;
    case IOP_CS_HSL:
      out[CHANNEL_INDEX_H] = in[0] / _get_boost_factor(data, 4, in_out);
      out[CHANNEL_INDEX_S] = in[1] / _get_boost_factor(data, 5, in_out);
      out[CHANNEL_INDEX_l] = in[2] / _get_boost_factor(data, 6, in_out);
      break;
    case IOP_CS_JZCZHZ:
      out[CHANNEL_INDEX_Jz] = in[0] / _get_boost_factor(data, 4, in_out);
      out[CHANNEL_INDEX_Cz] = in[1] / _get_boost_factor(data, 5, in_out);
      out[CHANNEL_INDEX_hz] = in[2] / _get_boost_factor(data, 6, in_out);
      break;
    default:
      break;
  }
}

static void _blendif_cook(dt_iop_colorspace_type_t cst, const float *in, float *out,
                          const dt_iop_order_iccprofile_info_t *const work_profile)
{
  out[0] = out[1] = out[2] = out[3] = out[4] = out[5] = out[6] = out[7] = -1.0f;

  switch(cst)
  {
    case IOP_CS_LAB:
      out[CHANNEL_INDEX_L] = in[0];
      out[CHANNEL_INDEX_a] = in[1];
      out[CHANNEL_INDEX_b] = in[2];
      break;
    case IOP_CS_RGB:
      if(work_profile == NULL)
        out[CHANNEL_INDEX_g] = (0.3f * in[0] + 0.59f * in[1] + 0.11f * in[2]) * 100.0f;
      else
        out[CHANNEL_INDEX_g] = dt_ioppr_get_rgb_matrix_luminance(in, work_profile->matrix_in,
                                                                 work_profile->lut_in,
                                                                 work_profile->unbounded_coeffs_in,
                                                                 work_profile->lutsize,
                                                                 work_profile->nonlinearlut) * 100.0f;
      out[CHANNEL_INDEX_R] = in[0] * 100.0f;
      out[CHANNEL_INDEX_G] = in[1] * 100.0f;
      out[CHANNEL_INDEX_B] = in[2] * 100.0f;
      break;
    case IOP_CS_LCH:
      out[CHANNEL_INDEX_C] = in[1] / (128.0f * sqrtf(2.0f)) * 100.0f;
      out[CHANNEL_INDEX_h] = in[2] * 360.0f;
      break;
    case IOP_CS_HSL:
      out[CHANNEL_INDEX_H] = in[0] * 360.0f;
      out[CHANNEL_INDEX_S] = in[1] * 100.0f;
      out[CHANNEL_INDEX_l] = in[2] * 100.0f;
      break;
    case IOP_CS_JZCZHZ:
      out[CHANNEL_INDEX_Jz] = in[0] * 100.0f;
      out[CHANNEL_INDEX_Cz] = in[1] * 100.0f;
      out[CHANNEL_INDEX_hz] = in[2] * 360.0f;
      break;
    default:
      break;
  }
}

static inline int _blendif_print_digits_default(float value)
{
  int digits;
  if(value < 0.0001f) digits = 0;
  else if(value < 0.01f) digits = 2;
  else if(value < 0.999f) digits = 1;
  else digits = 0;

  return digits;
}

static inline int _blendif_print_digits_ab(float value)
{
  int digits;
  if(fabsf(value) < 10.0f) digits = 1;
  else digits = 0;

  return digits;
}

static void _blendif_scale_print_ab(float value, float boost_factor, char *string, int n)
{
  const float scaled = (value * 256.0f - 128.0f) * boost_factor;
  snprintf(string, n, "%-5.*f", _blendif_print_digits_ab(scaled), scaled);
}

static void _blendif_scale_print_hue(float value, float boost_factor, char *string, int n)
{
  snprintf(string, n, "%-5.0f", value * 360.0f);
}

static void _blendif_scale_print_default(float value, float boost_factor, char *string, int n)
{
  const float scaled = value * boost_factor;
  snprintf(string, n, "%-5.*f", _blendif_print_digits_default(scaled), scaled * 100.0f);
}

static gboolean _blendif_are_output_channels_used(const dt_develop_blend_params_t *const blend,
                                                  const dt_develop_blend_colorspace_t cst)
{
  const gboolean mask_inclusive = blend->mask_combine & DEVELOP_COMBINE_INCL;
  const uint32_t mask = cst == DEVELOP_BLEND_CS_LAB
    ? DEVELOP_BLENDIF_Lab_MASK & DEVELOP_BLENDIF_OUTPUT_MASK
    : DEVELOP_BLENDIF_RGB_MASK & DEVELOP_BLENDIF_OUTPUT_MASK;
  const uint32_t active_channels = blend->blendif & mask;
  const uint32_t inverted_channels = (blend->blendif >> 16) ^ (mask_inclusive ? mask : 0);
  const uint32_t cancel_channels = inverted_channels & ~blend->blendif & mask;
  return active_channels || cancel_channels;
}

static gboolean _blendif_clean_output_channels(dt_iop_module_t *module)
{
  const dt_iop_gui_blend_data_t *const bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  if(!bd || !bd->blendif_support || !bd->blendif_inited) return FALSE;

  gboolean changed = FALSE;
  if(!bd->output_channels_shown)
  {
    const uint32_t mask = bd->csp == DEVELOP_BLEND_CS_LAB
      ? DEVELOP_BLENDIF_Lab_MASK & DEVELOP_BLENDIF_OUTPUT_MASK
      : DEVELOP_BLENDIF_RGB_MASK & DEVELOP_BLENDIF_OUTPUT_MASK;

    dt_develop_blend_params_t *const d = module->blend_params;

    // clear the output channels and invert them when needed
    const uint32_t old_blendif = d->blendif;
    const uint32_t need_inversion = d->mask_combine & DEVELOP_COMBINE_INCL ? (mask << 16) : 0;

    d->blendif = (d->blendif & ~(mask | (mask << 16))) | need_inversion;

    changed = (d->blendif != old_blendif);

    for(size_t ch = 0; ch < DEVELOP_BLENDIF_SIZE; ch++)
    {
      if((DEVELOP_BLENDIF_OUTPUT_MASK & (1 << ch))
          && (   d->blendif_parameters[ch * 4 + 0] != 0.0f
              || d->blendif_parameters[ch * 4 + 1] != 0.0f
              || d->blendif_parameters[ch * 4 + 2] != 1.0f
              || d->blendif_parameters[ch * 4 + 3] != 1.0f))
      {
        changed = TRUE;
        d->blendif_parameters[ch * 4 + 0] = 0.0f;
        d->blendif_parameters[ch * 4 + 1] = 0.0f;
        d->blendif_parameters[ch * 4 + 2] = 1.0f;
        d->blendif_parameters[ch * 4 + 3] = 1.0f;
      }
    }
  }
  return changed;
}

static void _add_wrapped_box(GtkWidget *container, GtkBox *box, gchar *help_url)
{
  GtkWidget *event_box = gtk_event_box_new();
  GtkWidget *revealer = gtk_revealer_new();
  gtk_container_add(GTK_CONTAINER(revealer), GTK_WIDGET(box));
  gtk_container_add(GTK_CONTAINER(event_box), revealer);
  gtk_container_add(GTK_CONTAINER(container), event_box);
  // event box is needed so that one can click into the area to get help
  dt_gui_add_help_link(event_box, dt_get_help_url(help_url));
}

static void _box_set_visible(GtkBox *box, gboolean visible)
{
  GtkRevealer *revealer = GTK_REVEALER(gtk_widget_get_parent(GTK_WIDGET(box)));
  gtk_revealer_set_transition_duration(revealer, dt_conf_get_int("darkroom/ui/transition_duration"));
  gtk_revealer_set_reveal_child(revealer, visible);
}

static void _blendop_masks_mode_callback(const unsigned int mask_mode, dt_iop_gui_blend_data_t *data)
{
  data->module->blend_params->mask_mode = mask_mode;

  _box_set_visible(data->top_box, mask_mode & DEVELOP_MASK_ENABLED);

  dt_iop_set_mask_mode(data->module, mask_mode);

  if((mask_mode & DEVELOP_MASK_ENABLED)
     && ((data->masks_inited && (mask_mode & DEVELOP_MASK_MASK))
         || (data->blendif_inited && (mask_mode & DEVELOP_MASK_CONDITIONAL))))
  {
    if(data->blendif_inited && (mask_mode & DEVELOP_MASK_CONDITIONAL))
    {
      dt_bauhaus_combobox_set_from_value(data->masks_combine_combo,
                                         data->module->blend_params->mask_combine & (DEVELOP_COMBINE_INV | DEVELOP_COMBINE_INCL));
      gtk_widget_hide(GTK_WIDGET(data->masks_invert_combo));
      gtk_widget_show(GTK_WIDGET(data->masks_combine_combo));
    }
    else
    {
      dt_bauhaus_combobox_set_from_value(data->masks_invert_combo,
                                         data->module->blend_params->mask_combine & DEVELOP_COMBINE_INV);
      gtk_widget_show(GTK_WIDGET(data->masks_invert_combo));
      gtk_widget_hide(GTK_WIDGET(data->masks_combine_combo));
    }

    /*
     * if this iop is operating in raw space, it has only 1 channel per pixel,
     * thus there is no alpha channel where we would normally store mask
     * that would get displayed if following button have been pressed.
     *
     * TODO: revisit if/once there semi-raw iops (e.g temperature) with blending
     */
    if(data->module->blend_colorspace(data->module, NULL, NULL) == IOP_CS_RAW)
    {
      data->module->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->showmask), FALSE);
      gtk_widget_hide(GTK_WIDGET(data->showmask));

      // disable also guided-filters on RAW based color space
      gtk_widget_set_sensitive(data->masks_feathering_guide_combo, FALSE);
      gtk_widget_hide(GTK_WIDGET(data->masks_feathering_guide_combo));
      gtk_widget_set_sensitive(data->feathering_radius_slider, FALSE);
      gtk_widget_hide(GTK_WIDGET(data->feathering_radius_slider));
      gtk_widget_set_sensitive(data->brightness_slider, FALSE);
      gtk_widget_hide(GTK_WIDGET(data->brightness_slider));
      gtk_widget_set_sensitive(data->contrast_slider, FALSE);
      gtk_widget_hide(GTK_WIDGET(data->contrast_slider));
      gtk_widget_set_sensitive(data->details_slider, FALSE);
      gtk_widget_hide(GTK_WIDGET(data->details_slider));
    }
    else
    {
      gtk_widget_show(GTK_WIDGET(data->showmask));
    }

    _box_set_visible(data->bottom_box, TRUE);
  }
  else
  {
    _box_set_visible(data->bottom_box, FALSE);
  }

  if(data->masks_inited && (mask_mode & DEVELOP_MASK_MASK))
  {
    _box_set_visible(data->masks_box, TRUE);
  }
  else if(data->masks_inited)
  {
    for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->masks_shapes[n]), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->masks_edit), FALSE);
    dt_masks_set_edit_mode(data->module, DT_MASKS_EDIT_OFF);
    _box_set_visible(data->masks_box, FALSE);
  }
  else if(data->masks_support)
  {
    for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->masks_shapes[n]), FALSE);
    _box_set_visible(data->masks_box, FALSE);
  }

  _box_set_visible(data->raster_box, data->raster_inited && (mask_mode & DEVELOP_MASK_RASTER));

  if(data->blendif_inited && (mask_mode & DEVELOP_MASK_CONDITIONAL))
  {
    _box_set_visible(data->blendif_box, TRUE);
  }
  else if(data->blendif_inited)
  {
    /* switch off color picker */
    dt_iop_color_picker_reset(data->module, FALSE);

    _box_set_visible(data->blendif_box, FALSE);
  }
  else
  {
    _box_set_visible(data->blendif_box, FALSE);
  }

  dt_dev_add_history_item(darktable.develop, data->module, TRUE);

  if(dt_conf_get_bool("accel/prefer_unmasked"))
  {
    // rebuild the accelerators
    dt_iop_connect_accels_multi(data->module->so);
  }
}

static void _blendop_blend_mode_callback(GtkWidget *combo, dt_iop_gui_blend_data_t *data)
{
  if(darktable.gui->reset) return;

  dt_develop_blend_params_t *bp = data->module->blend_params;
  dt_develop_blend_mode_t new_blend_mode = GPOINTER_TO_INT(dt_bauhaus_combobox_get_data(combo));
  if(new_blend_mode != (bp->blend_mode & DEVELOP_BLEND_MODE_MASK))
  {
    bp->blend_mode = new_blend_mode | (bp->blend_mode & DEVELOP_BLEND_REVERSE);
    if(_blendif_blend_parameter_enabled(data->blend_modes_csp, bp->blend_mode))
    {
      gtk_widget_set_sensitive(data->blend_mode_parameter_slider, TRUE);
    }
    else
    {
      bp->blend_parameter = 0.0f;
      dt_bauhaus_slider_set(data->blend_mode_parameter_slider, bp->blend_parameter);
      gtk_widget_set_sensitive(data->blend_mode_parameter_slider, FALSE);
    }
    dt_dev_add_history_item(darktable.develop, data->module, TRUE);
  }
}

static gboolean _blendop_blend_order_clicked(GtkWidget *button, GdkEventButton *event, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return TRUE;

  dt_develop_blend_params_t *bp = (dt_develop_blend_params_t *)module->blend_params;
  const gboolean active = !(bp->blend_mode & DEVELOP_BLEND_REVERSE);
  if(!active)
    bp->blend_mode &= ~DEVELOP_BLEND_REVERSE;
  else
    bp->blend_mode |= DEVELOP_BLEND_REVERSE;

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), active);

  dt_dev_add_history_item(darktable.develop, module, TRUE);
  dt_control_queue_redraw_widget(GTK_WIDGET(button));

  return TRUE;
}

static void _blendop_masks_combine_callback(GtkWidget *combo, dt_iop_gui_blend_data_t *data)
{
  dt_develop_blend_params_t *const d = data->module->blend_params;

  const unsigned combine = GPOINTER_TO_UINT(dt_bauhaus_combobox_get_data(data->masks_combine_combo));
  d->mask_combine &= ~(DEVELOP_COMBINE_INV | DEVELOP_COMBINE_INCL);
  d->mask_combine |= combine;

  // inverts the parametric mask channels that are not used
  if(data->blendif_support && data->blendif_inited)
  {
    const uint32_t mask = data->csp == DEVELOP_BLEND_CS_LAB ? DEVELOP_BLENDIF_Lab_MASK : DEVELOP_BLENDIF_RGB_MASK;
    const uint32_t unused_channels = mask & ~d->blendif;
    d->blendif &= ~(unused_channels << 16);
    if(d->mask_combine & DEVELOP_COMBINE_INCL)
    {
      d->blendif |= unused_channels << 16;
    }
    _blendop_blendif_update_tab(data->module, data->tab);
  }

  _blendif_clean_output_channels(data->module);
  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
}

static void _blendop_masks_invert_callback(GtkWidget *combo, dt_iop_gui_blend_data_t *data)
{
  unsigned int invert = GPOINTER_TO_UINT(dt_bauhaus_combobox_get_data(data->masks_invert_combo))
                        & DEVELOP_COMBINE_INV;
  if(invert)
    data->module->blend_params->mask_combine |= DEVELOP_COMBINE_INV;
  else
    data->module->blend_params->mask_combine &= ~DEVELOP_COMBINE_INV;
  _blendif_clean_output_channels(data->module);
  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
}

static void _blendop_blendif_sliders_callback(GtkDarktableGradientSlider *slider, dt_iop_gui_blend_data_t *data)
{
  if(darktable.gui->reset) return;

  dt_develop_blend_params_t *bp = data->module->blend_params;

  const dt_iop_gui_blendif_channel_t *channel = &data->channel[data->tab];

  const int in_out = (slider == data->filter[1].slider) ? 1 : 0;
  dt_develop_blendif_channels_t ch = channel->param_channels[in_out];
  GtkLabel **label = data->filter[in_out].label;

  if(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data->colorpicker))
     && !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data->colorpicker_set_values)))
  {
    dt_iop_color_picker_reset(data->module, FALSE);
  }

  float *parameters = &(bp->blendif_parameters[4 * ch]);

  dt_pthread_mutex_lock(&data->lock);
  for(int k = 0; k < 4; k++) parameters[k] = dtgtk_gradient_slider_multivalue_get_value(slider, k);
  dt_pthread_mutex_unlock(&data->lock);

  const float boost_factor = _get_boost_factor(data, data->tab, in_out);
  for(int k = 0; k < 4; k++)
  {
    char text[256];
    (channel->scale_print)(parameters[k], boost_factor, text, sizeof(text));
    gtk_label_set_text(label[k], text);
  }

  /** de-activate processing of this channel if maximum span is selected */
  if(parameters[1] == 0.0f && parameters[2] == 1.0f)
    bp->blendif &= ~(1 << ch);
  else
    bp->blendif |= (1 << ch);

  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
}

static void _blendop_blendif_sliders_reset_callback(GtkDarktableGradientSlider *slider,
                                                    dt_iop_gui_blend_data_t *data)
{
  if(darktable.gui->reset) return;

  dt_develop_blend_params_t *bp = data->module->blend_params;

  const dt_iop_gui_blendif_channel_t *channel = &data->channel[data->tab];

  const int in_out = (slider == data->filter[1].slider) ? 1 : 0;
  dt_develop_blendif_channels_t ch = channel->param_channels[in_out];

  // invert the parametric mask if needed
  if(bp->mask_combine & DEVELOP_COMBINE_INCL)
    bp->blendif |= (1 << (16 + ch));
  else
    bp->blendif &= ~(1 << (16 + ch));

  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
  _blendop_blendif_update_tab(data->module, data->tab);
}

static void _blendop_blendif_polarity_callback(GtkToggleButton *togglebutton, dt_iop_gui_blend_data_t *data)
{
  if(darktable.gui->reset) return;

  int active = gtk_toggle_button_get_active(togglebutton);

  dt_develop_blend_params_t *bp = data->module->blend_params;

  const dt_iop_gui_blendif_channel_t *channel = &data->channel[data->tab];

  const int in_out = (GTK_WIDGET(togglebutton) == data->filter[1].polarity) ? 1 : 0;
  dt_develop_blendif_channels_t ch = channel->param_channels[in_out];
  GtkDarktableGradientSlider *slider = data->filter[in_out].slider;

  if(!active)
    bp->blendif |= (1 << (ch + 16));
  else
    bp->blendif &= ~(1 << (ch + 16));

  dtgtk_gradient_slider_multivalue_set_marker(
      slider, active ? GRADIENT_SLIDER_MARKER_LOWER_OPEN_BIG : GRADIENT_SLIDER_MARKER_UPPER_OPEN_BIG, 0);
  dtgtk_gradient_slider_multivalue_set_marker(
      slider, active ? GRADIENT_SLIDER_MARKER_UPPER_FILLED_BIG : GRADIENT_SLIDER_MARKER_LOWER_FILLED_BIG, 1);
  dtgtk_gradient_slider_multivalue_set_marker(
      slider, active ? GRADIENT_SLIDER_MARKER_UPPER_FILLED_BIG : GRADIENT_SLIDER_MARKER_LOWER_FILLED_BIG, 2);
  dtgtk_gradient_slider_multivalue_set_marker(
      slider, active ? GRADIENT_SLIDER_MARKER_LOWER_OPEN_BIG : GRADIENT_SLIDER_MARKER_UPPER_OPEN_BIG, 3);

  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
  dt_control_queue_redraw_widget(GTK_WIDGET(togglebutton));
}

static float log10_scale_callback(GtkWidget *self, float inval, int dir)
{
  float outval;
  const float tiny = 1.0e-4f;
  switch(dir)
  {
    case GRADIENT_SLIDER_SET:
      outval = (log10(CLAMP(inval, 0.0001f, 1.0f)) + 4.0f) / 4.0f;
      break;
    case GRADIENT_SLIDER_GET:
      outval = CLAMP(exp(M_LN10 * (4.0f * inval - 4.0f)), 0.0f, 1.0f);
      if(outval <= tiny) outval = 0.0f;
      if(outval >= 1.0f - tiny) outval = 1.0f;
      break;
    default:
      outval = inval;
  }
  return outval;
}


static float magnifier_scale_callback(GtkWidget *self, float inval, int dir)
{
  float outval;
  const float range = 6.0f;
  const float invrange = 1.0f/range;
  const float scale = tanh(range * 0.5f);
  const float invscale = 1.0f/scale;
  const float eps = 1.0e-6f;
  const float tiny = 1.0e-4f;
  switch(dir)
  {
    case GRADIENT_SLIDER_SET:
      outval = (invscale * tanh(range * (CLAMP(inval, 0.0f, 1.0f) - 0.5f)) + 1.0f) * 0.5f;
      if(outval <= tiny) outval = 0.0f;
      if(outval >= 1.0f - tiny) outval = 1.0f;
      break;
    case GRADIENT_SLIDER_GET:
      outval = invrange * atanh((2.0f * CLAMP(inval, eps, 1.0f - eps) - 1.0f) * scale) + 0.5f;
      if(outval <= tiny) outval = 0.0f;
      if(outval >= 1.0f - tiny) outval = 1.0f;
      break;
    default:
      outval = inval;
  }
  return outval;
}

static int _blendop_blendif_disp_alternative_worker(GtkWidget *widget, dt_iop_module_t *module, int mode,
                                                    float (*scale_callback)(GtkWidget*, float, int), const char *label)
{
  dt_iop_gui_blend_data_t *data = module->blend_data;
  GtkDarktableGradientSlider *slider = (GtkDarktableGradientSlider *)widget;

  int in_out = (slider == data->filter[1].slider) ? 1 : 0;

  dtgtk_gradient_slider_multivalue_set_scale_callback(slider, (mode == 1) ? scale_callback : NULL);
  gchar *text = g_strdup_printf("%s%s",
                                (in_out == 0) ? _("Input") : _("Output"),
                                (mode == 1) ? label : "");
  gtk_label_set_text(data->filter[in_out].head, text);
  g_free(text);

  return (mode == 1) ? 1 : 0;
}


static int _blendop_blendif_disp_alternative_mag(GtkWidget *widget, dt_iop_module_t *module, int mode)
{
  return _blendop_blendif_disp_alternative_worker(widget, module, mode, magnifier_scale_callback, _(" (Zoom)"));
}

static int _blendop_blendif_disp_alternative_log(GtkWidget *widget, dt_iop_module_t *module, int mode)
{
  return _blendop_blendif_disp_alternative_worker(widget, module, mode, log10_scale_callback, _(" (Log)"));
}

static void _blendop_blendif_disp_alternative_reset(GtkWidget *widget, dt_iop_module_t *module)
{
  (void) _blendop_blendif_disp_alternative_worker(widget, module, 0, NULL, "");
}


static dt_iop_colorspace_type_t _blendop_blendif_get_picker_colorspace(dt_iop_gui_blend_data_t *bd)
{
  dt_iop_colorspace_type_t picker_cst = IOP_CS_NONE;

  if(bd->channel_tabs_csp == DEVELOP_BLEND_CS_RGB_DISPLAY)
  {
    if(bd->tab < 4)
      picker_cst = IOP_CS_RGB;
    else
      picker_cst = IOP_CS_HSL;
  }
  else if(bd->channel_tabs_csp == DEVELOP_BLEND_CS_RGB_SCENE)
  {
    if(bd->tab < 4)
      picker_cst = IOP_CS_RGB;
    else
      picker_cst = IOP_CS_JZCZHZ;
  }
  else if(bd->channel_tabs_csp == DEVELOP_BLEND_CS_LAB)
  {
    if(bd->tab < 3)
      picker_cst = IOP_CS_LAB;
    else
      picker_cst = IOP_CS_LCH;
  }

  return picker_cst;
}

static inline int _blendif_print_digits_picker(float value)
{
  int digits;
  if(value < 10.0f) digits = 2;
  else digits = 1;

  return digits;
}

static void _update_gradient_slider_pickers(GtkWidget *callback_dummy, dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *data = module->blend_data;

  dt_iop_color_picker_set_cst(module, _blendop_blendif_get_picker_colorspace(data));

  float *raw_mean, *raw_min, *raw_max;

  ++darktable.gui->reset;

  for(int in_out = 1; in_out >= 0; in_out--)
  {
    if(in_out)
    {
      raw_mean = module->picked_output_color;
      raw_min = module->picked_output_color_min;
      raw_max = module->picked_output_color_max;
    }
    else
    {
      raw_mean = module->picked_color;
      raw_min = module->picked_color_min;
      raw_max = module->picked_color_max;
    }

    if((gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data->colorpicker)) ||
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data->colorpicker_set_values))) &&
       (raw_min[0] != INFINITY))
    {
      float picker_mean[8], picker_min[8], picker_max[8];
      float cooked[8];

      const dt_develop_blend_colorspace_t blend_csp = data->channel_tabs_csp;
      const dt_iop_colorspace_type_t cst = _blendif_colorpicker_cst(data);
      const dt_iop_order_iccprofile_info_t *work_profile = (blend_csp == DEVELOP_BLEND_CS_RGB_SCENE)
          ? dt_ioppr_get_pipe_current_profile_info(module, module->dev->pipe)
          : dt_ioppr_get_iop_work_profile_info(module, module->dev->iop);

      _blendif_scale(data, cst, raw_mean, picker_mean, work_profile, in_out);
      _blendif_scale(data, cst, raw_min, picker_min, work_profile, in_out);
      _blendif_scale(data, cst, raw_max, picker_max, work_profile, in_out);
      _blendif_cook(cst, raw_mean, cooked, work_profile);

      gchar *text = g_strdup_printf("(%.*f)", _blendif_print_digits_picker(cooked[data->tab]), cooked[data->tab]);

      dtgtk_gradient_slider_multivalue_set_picker_meanminmax(
          data->filter[in_out].slider,
          CLAMP(picker_mean[data->tab], 0.0f, 1.0f),
          CLAMP(picker_min[data->tab], 0.0f, 1.0f),
          CLAMP(picker_max[data->tab], 0.0f, 1.0f));
      gtk_label_set_text(data->filter[in_out].picker_label, text);

      g_free(text);
    }
    else
    {
      dtgtk_gradient_slider_multivalue_set_picker(data->filter[in_out].slider, NAN);
      gtk_label_set_text(data->filter[in_out].picker_label, "");
    }
  }

  --darktable.gui->reset;
}


static void _blendop_blendif_update_tab(dt_iop_module_t *module, const int tab)
{
  dt_iop_gui_blend_data_t *data = module->blend_data;
  dt_develop_blend_params_t *bp = module->blend_params;
  dt_develop_blend_params_t *dp = module->default_blendop_params;

  ++darktable.gui->reset;

  const dt_iop_gui_blendif_channel_t *channel = &data->channel[tab];

  for(int in_out = 1; in_out >= 0; in_out--)
  {
    const dt_develop_blendif_channels_t ch = channel->param_channels[in_out];
    dt_iop_gui_blendif_filter_t *sl = &data->filter[in_out];

    float *parameters = &(bp->blendif_parameters[4 * ch]);
    float *defaults = &(dp->blendif_parameters[4 * ch]);

    const int polarity = !(bp->blendif & (1 << (ch + 16)));

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sl->polarity), polarity);

    dtgtk_gradient_slider_multivalue_set_marker(
        sl->slider,
        polarity ? GRADIENT_SLIDER_MARKER_LOWER_OPEN_BIG : GRADIENT_SLIDER_MARKER_UPPER_OPEN_BIG, 0);
    dtgtk_gradient_slider_multivalue_set_marker(
        sl->slider,
        polarity ? GRADIENT_SLIDER_MARKER_UPPER_FILLED_BIG : GRADIENT_SLIDER_MARKER_LOWER_FILLED_BIG, 1);
    dtgtk_gradient_slider_multivalue_set_marker(
        sl->slider,
        polarity ? GRADIENT_SLIDER_MARKER_UPPER_FILLED_BIG : GRADIENT_SLIDER_MARKER_LOWER_FILLED_BIG, 2);
    dtgtk_gradient_slider_multivalue_set_marker(
        sl->slider,
        polarity ? GRADIENT_SLIDER_MARKER_LOWER_OPEN_BIG : GRADIENT_SLIDER_MARKER_UPPER_OPEN_BIG, 3);

    dt_pthread_mutex_lock(&data->lock);
    for(int k = 0; k < 4; k++)
    {
      dtgtk_gradient_slider_multivalue_set_value(sl->slider, parameters[k], k);
      dtgtk_gradient_slider_multivalue_set_resetvalue(sl->slider, defaults[k], k);
    }
    dt_pthread_mutex_unlock(&data->lock);

    const float boost_factor = _get_boost_factor(data, tab, in_out);
    for(int k = 0; k < 4; k++)
    {
      char text[256];
      channel->scale_print(parameters[k], boost_factor, text, sizeof(text));
      gtk_label_set_text(sl->label[k], text);
    }

    dtgtk_gradient_slider_multivalue_clear_stops(sl->slider);

    for(int k = 0; k < channel->numberstops; k++)
    {
      dtgtk_gradient_slider_multivalue_set_stop(sl->slider, channel->colorstops[k].stoppoint,
                                                channel->colorstops[k].color);
    }

    dtgtk_gradient_slider_multivalue_set_increment(sl->slider, channel->increment);

    if(channel->altdisplay)
    {
      data->altmode[tab][in_out] = channel->altdisplay(GTK_WIDGET(sl->slider), module, data->altmode[tab][in_out]);
    }
    else
    {
      _blendop_blendif_disp_alternative_reset(GTK_WIDGET(sl->slider), module);
    }
  }

  _update_gradient_slider_pickers(NULL, module);

  const gboolean boost_factor_enabled = channel->boost_factor_enabled;
  float boost_factor = 0.0f;
  if(boost_factor_enabled)
  {
    boost_factor = bp->blendif_boost_factors[channel->param_channels[0]] - channel->boost_factor_offset;
  }
  gtk_widget_set_sensitive(GTK_WIDGET(data->channel_boost_factor_slider), boost_factor_enabled);
  dt_bauhaus_slider_set(GTK_WIDGET(data->channel_boost_factor_slider), boost_factor);

  --darktable.gui->reset;
}


static void _blendop_blendif_tab_switch(GtkNotebook *notebook, GtkWidget *page, guint page_num,
                                        dt_iop_gui_blend_data_t *data)
{
  if(darktable.gui->reset || !data || !data->blendif_inited) return;
  const int cst_old = _blendop_blendif_get_picker_colorspace(data);
  dt_iop_color_picker_reset(data->module, FALSE);

  data->tab = page_num;

  if(cst_old != _blendop_blendif_get_picker_colorspace(data)
     && (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data->colorpicker))
         || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data->colorpicker_set_values))))
  {
    dt_iop_color_picker_set_cst(data->module, _blendop_blendif_get_picker_colorspace(data));
    dt_dev_reprocess_all(data->module->dev);
    dt_control_queue_redraw();
  }

  _blendop_blendif_update_tab(data->module, data->tab);
}

static void _blendop_blendif_boost_factor_callback(GtkWidget *slider, dt_iop_gui_blend_data_t *data)
{
  if(darktable.gui->reset || !data || !data->blendif_inited) return;
  dt_develop_blend_params_t *bp = data->module->blend_params;
  const int tab = data->tab;

  const float value = dt_bauhaus_slider_get(slider);
  for(int in_out = 1; in_out >= 0; in_out--)
  {
    const int ch = data->channel[tab].param_channels[in_out];
    float off = 0.0f;
    if(data->csp == DEVELOP_BLEND_CS_LAB && (ch == DEVELOP_BLENDIF_A_in || ch == DEVELOP_BLENDIF_A_out
        || ch == DEVELOP_BLENDIF_B_in || ch == DEVELOP_BLENDIF_B_out))
    {
      off = 0.5f;
    }
    const float new_value = value + data->channel[tab].boost_factor_offset;
    const float old_value = bp->blendif_boost_factors[ch];
    const float factor = exp2f(old_value) / exp2f(new_value);
    float *parameters = &(bp->blendif_parameters[4 * ch]);
    if(parameters[0] > 0.0f) parameters[0] = clamp_range_f((parameters[0] - off) * factor + off, 0.0f, 1.0f);
    if(parameters[1] > 0.0f) parameters[1] = clamp_range_f((parameters[1] - off) * factor + off, 0.0f, 1.0f);
    if(parameters[2] < 1.0f) parameters[2] = clamp_range_f((parameters[2] - off) * factor + off, 0.0f, 1.0f);
    if(parameters[3] < 1.0f) parameters[3] = clamp_range_f((parameters[3] - off) * factor + off, 0.0f, 1.0f);
    if(parameters[1] == 0.0f && parameters[2] == 1.0f)
      bp->blendif &= ~(1 << ch);
    bp->blendif_boost_factors[ch] = new_value;
  }
  _blendop_blendif_update_tab(data->module, tab);

  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
}

static void _blendop_blendif_details_callback(GtkWidget *slider, dt_iop_gui_blend_data_t *data)
{
  if(darktable.gui->reset || !data || !data->blendif_inited) return;
  dt_develop_blend_params_t *bp = data->module->blend_params;
  const float oldval = bp->details;
  bp->details = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, data->module, TRUE);

  if((oldval == 0.0f) && (bp->details != 0.0f))
  {
    dt_dev_reprocess_all(data->module->dev);
    dt_control_queue_redraw();
  }
}

static gboolean _blendop_blendif_showmask_clicked(GtkToggleButton *button, GdkEventButton *event, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return TRUE;

  if(event->button == 1)
  {
    const int has_mask_display = module->request_mask_display & (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL);

    module->request_mask_display &= ~(DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL | DT_DEV_PIXELPIPE_DISPLAY_ANY);

    if(dt_modifier_is(event->state, GDK_CONTROL_MASK | GDK_SHIFT_MASK))
      module->request_mask_display |= (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL);
    else if(dt_modifier_is(event->state, GDK_SHIFT_MASK))
      module->request_mask_display |= DT_DEV_PIXELPIPE_DISPLAY_CHANNEL;
    else if(dt_modifier_is(event->state, GDK_CONTROL_MASK))
      module->request_mask_display |= DT_DEV_PIXELPIPE_DISPLAY_MASK;
    else
      module->request_mask_display |= (has_mask_display ? 0 : DT_DEV_PIXELPIPE_DISPLAY_MASK);

    gtk_toggle_button_set_active(button,
                                 module->request_mask_display != DT_DEV_PIXELPIPE_DISPLAY_NONE);

    if(module->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), TRUE);

    ++darktable.gui->reset;
    // (re)set the header mask indicator too
    if(module->mask_indicator)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->mask_indicator),
                                   module->request_mask_display != DT_DEV_PIXELPIPE_DISPLAY_NONE);
    --darktable.gui->reset;

    dt_iop_request_focus(module);
    dt_iop_refresh_center(module);
  }

  return TRUE;
}

static gboolean _blendop_masks_modes_none_clicked(GtkWidget *button, GdkEventButton *event, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return TRUE;
  dt_iop_gui_blend_data_t *data = module->blend_data;

  if(event->button == 1 && data->selected_mask_mode != button)
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->selected_mask_mode),
                                 FALSE); // unsets currently toggled if any

    _blendop_masks_mode_callback(DEVELOP_MASK_DISABLED, data);
    data->selected_mask_mode = button;

    // remove the mask indicator
    add_remove_mask_indicator(module, FALSE);

    /* and finally remove hinter messages */
    dt_control_hinter_message(darktable.control, "");
  }

  return TRUE;
}

static gboolean _blendop_masks_modes_toggle(GtkToggleButton *button, dt_iop_module_t *module, const unsigned int mask_mode)
{
  if(darktable.gui->reset) return FALSE;
  dt_iop_gui_blend_data_t *data = module->blend_data;

  const gboolean was_toggled = !gtk_toggle_button_get_active(button);
  gtk_toggle_button_set_active(button, was_toggled);

  // avoids trying to untoggle the cancel button
  if(data->selected_mask_mode
     != g_list_nth_data(data->masks_modes_toggles,
                        g_list_index(data->masks_modes, GUINT_TO_POINTER(DEVELOP_MASK_DISABLED))))
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->selected_mask_mode), FALSE);
  }

  if(was_toggled)
  {
    _blendop_masks_mode_callback(mask_mode, data);
    data->selected_mask_mode = GTK_WIDGET(button);
  }
  else
  {
    _blendop_masks_mode_callback(DEVELOP_MASK_DISABLED, data);
    data->selected_mask_mode = GTK_WIDGET(
      g_list_nth_data(data->masks_modes_toggles,
                      g_list_index(data->masks_modes, (gconstpointer)DEVELOP_MASK_DISABLED)));
  }
  // (un)set the mask indicator, but not for uniform blend
  if(mask_mode == DEVELOP_MASK_ENABLED) add_remove_mask_indicator(module, FALSE);
  else add_remove_mask_indicator(module, was_toggled);
  ++darktable.gui->reset;
  if(was_toggled && module->mask_indicator)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->mask_indicator),
                                   gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data->showmask)));
  --darktable.gui->reset;

  return TRUE;
}

static gboolean _blendop_masks_modes_uni_toggled(GtkToggleButton *button, GdkEventButton *event, dt_iop_module_t *module)
{
  return _blendop_masks_modes_toggle(button, module, DEVELOP_MASK_ENABLED);
}

static gboolean _blendop_masks_modes_drawn_toggled(GtkToggleButton *button, GdkEventButton *event, dt_iop_module_t *module)
{
  return _blendop_masks_modes_toggle(button, module, DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK);
}

static gboolean _blendop_masks_modes_param_toggled(GtkToggleButton *button, GdkEventButton *event, dt_iop_module_t *module)
{
  return _blendop_masks_modes_toggle(button, module, DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL);
}

static gboolean _blendop_masks_modes_both_toggled(GtkToggleButton *button, GdkEventButton *event, dt_iop_module_t *module)
{
  return _blendop_masks_modes_toggle(button, module, DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK_CONDITIONAL);
}

static gboolean _blendop_masks_modes_raster_toggled(GtkToggleButton *button, GdkEventButton *event, dt_iop_module_t *module)
{
  return _blendop_masks_modes_toggle(button, module, DEVELOP_MASK_ENABLED | DEVELOP_MASK_RASTER);
}

static gboolean _blendop_blendif_suppress_toggled(GtkToggleButton *togglebutton, GdkEventButton *event, dt_iop_module_t *module)
{
  module->suppress_mask = !gtk_toggle_button_get_active(togglebutton);
  if(darktable.gui->reset) return FALSE;

  if(module->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), TRUE);
  dt_iop_request_focus(module);

  gtk_toggle_button_set_active(togglebutton, module->suppress_mask);

  dt_control_queue_redraw_widget(GTK_WIDGET(togglebutton));
  dt_iop_refresh_center(module);

  return TRUE;
}

static gboolean _blendop_blendif_reset(GtkButton *button, GdkEventButton *event, dt_iop_module_t *module)
{
  module->blend_params->blendif = module->default_blendop_params->blendif;
  memcpy(module->blend_params->blendif_parameters, module->default_blendop_params->blendif_parameters,
         4 * DEVELOP_BLENDIF_SIZE * sizeof(float));
  module->blend_params->details = module->default_blendop_params->details;

  dt_iop_color_picker_reset(module, FALSE);
  dt_iop_gui_update_blendif(module);
  dt_dev_add_history_item(darktable.develop, module, TRUE);

  return TRUE;
}

static gboolean _blendop_blendif_invert(GtkButton *button, GdkEventButton *event, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return TRUE;

  const dt_iop_gui_blend_data_t *data = module->blend_data;

  unsigned int toggle_mask = 0;

  switch(data->channel_tabs_csp)
  {
    case DEVELOP_BLEND_CS_LAB:
      toggle_mask = DEVELOP_BLENDIF_Lab_MASK << 16;
      break;
    case DEVELOP_BLEND_CS_RGB_DISPLAY:
    case DEVELOP_BLEND_CS_RGB_SCENE:
      toggle_mask = DEVELOP_BLENDIF_RGB_MASK << 16;
      break;
    case DEVELOP_BLEND_CS_RAW:
    case DEVELOP_BLEND_CS_NONE:
      toggle_mask = 0;
      break;
  }

  module->blend_params->blendif ^= toggle_mask;
  module->blend_params->mask_combine ^= DEVELOP_COMBINE_MASKS_POS;
  module->blend_params->mask_combine ^= DEVELOP_COMBINE_INCL;
  dt_iop_gui_update_blending(module);
  dt_dev_add_history_item(darktable.develop, module, TRUE);

  return TRUE;
}

static gboolean _blendop_masks_add_shape(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  if(darktable.gui->reset || event->button != GDK_BUTTON_PRIMARY) return TRUE;

  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)self->blend_data;

  gboolean continuous = dt_modifier_is(event->state, GDK_CONTROL_MASK);

  // find out who we are
  int this = -1;
  for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
  {
    if(widget == bd->masks_shapes[n])
    {
      this = n;
      break;
    }
  }

  if(this < 0) return FALSE;

  // set all shape buttons to inactive
  for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_shapes[n]), FALSE);

  // we want to be sure that the iop has focus
  dt_iop_request_focus(self);
  dt_iop_color_picker_reset(self, FALSE);
  bd->masks_shown = DT_MASKS_EDIT_FULL;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), TRUE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), FALSE);
  // we create the new form
  dt_masks_form_t *form = dt_masks_create(bd->masks_type[this]);
  dt_masks_change_form_gui(form);
  darktable.develop->form_gui->creation_module = self;

  if(continuous)
  {
    darktable.develop->form_gui->creation_continuous = TRUE;
    darktable.develop->form_gui->creation_continuous_module = self;
  }

  dt_control_queue_redraw_center();

  return TRUE;
}

static gboolean _blendop_masks_show_and_edit(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return FALSE;
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)self->blend_data;

  if(event->button == 1)
  {
    ++darktable.gui->reset;

    dt_iop_request_focus(self);
    dt_iop_color_picker_reset(self, FALSE);

    dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, self->blend_params->mask_id);
    if(grp && (grp->type & DT_MASKS_GROUP) && grp->points)
    {
      const gboolean control_button_pressed = dt_modifier_is(event->state, GDK_CONTROL_MASK);

      switch(bd->masks_shown)
      {
        case DT_MASKS_EDIT_FULL:
          bd->masks_shown = control_button_pressed ? DT_MASKS_EDIT_RESTRICTED : DT_MASKS_EDIT_OFF;
          break;

        case DT_MASKS_EDIT_RESTRICTED:
          bd->masks_shown = !control_button_pressed ? DT_MASKS_EDIT_FULL : DT_MASKS_EDIT_OFF;
          break;

        default:
        case DT_MASKS_EDIT_OFF:
          bd->masks_shown = control_button_pressed ? DT_MASKS_EDIT_RESTRICTED : DT_MASKS_EDIT_FULL;
      }
    }
    else
    {
      bd->masks_shown = DT_MASKS_EDIT_OFF;
      /* remove hinter messages */
      dt_control_hinter_message(darktable.control, "");
    }

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), bd->masks_shown != DT_MASKS_EDIT_OFF);
    dt_masks_set_edit_mode(self, bd->masks_shown);

    // set all add shape buttons to inactive
    for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_shapes[n]), FALSE);

    --darktable.gui->reset;

    return TRUE;
  }

  return FALSE;
}

static gboolean _blendop_masks_polarity_callback(GtkToggleButton *togglebutton, GdkEventButton *event, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return TRUE;

  const int active = !gtk_toggle_button_get_active(togglebutton);
  gtk_toggle_button_set_active(togglebutton, active);

  dt_develop_blend_params_t *bp = (dt_develop_blend_params_t *)self->blend_params;

  if(active)
    bp->mask_combine |= DEVELOP_COMBINE_MASKS_POS;
  else
    bp->mask_combine &= ~DEVELOP_COMBINE_MASKS_POS;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  dt_control_queue_redraw_widget(GTK_WIDGET(togglebutton));

  return TRUE;
}

gboolean blend_color_picker_apply(dt_iop_module_t *module, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_gui_blend_data_t *data = module->blend_data;

  if(picker == data->colorpicker_set_values)
  {
    if(darktable.gui->reset) return TRUE;

    ++darktable.gui->reset;

    dt_develop_blend_params_t *bp = module->blend_params;

    const int tab = data->tab;
    dt_aligned_pixel_t raw_min, raw_max;
    float picker_min[8] DT_ALIGNED_PIXEL, picker_max[8] DT_ALIGNED_PIXEL;
    dt_aligned_pixel_t picker_values;

    const int in_out = ((dt_key_modifier_state() == GDK_CONTROL_MASK) && data->output_channels_shown) ? 1 : 0;

    if(in_out)
    {
      for(size_t i = 0; i < 4; i++)
      {
        raw_min[i] = module->picked_output_color_min[i];
        raw_max[i] = module->picked_output_color_max[i];
      }
    }
    else
    {
      for(size_t i = 0; i < 4; i++)
      {
        raw_min[i] = module->picked_color_min[i];
        raw_max[i] = module->picked_color_max[i];
      }
    }

    const dt_iop_gui_blendif_channel_t *channel = &data->channel[data->tab];
    dt_develop_blendif_channels_t ch = channel->param_channels[in_out];
    dt_iop_gui_blendif_filter_t *sl = &data->filter[in_out];

    float *parameters = &(bp->blendif_parameters[4 * ch]);

    const dt_develop_blend_colorspace_t blend_csp = data->channel_tabs_csp;
    const dt_iop_colorspace_type_t cst = _blendif_colorpicker_cst(data);
    const dt_iop_order_iccprofile_info_t *work_profile = (blend_csp == DEVELOP_BLEND_CS_RGB_SCENE)
        ? dt_ioppr_get_pipe_current_profile_info(piece->module, piece->pipe)
        : dt_ioppr_get_iop_work_profile_info(module, module->dev->iop);

    gboolean reverse_hues = FALSE;
    if(cst == IOP_CS_HSL && tab == CHANNEL_INDEX_H)
    {
      if((raw_max[3] - raw_min[3]) < (raw_max[0] - raw_min[0]) && raw_min[3] < 0.5f && raw_max[3] > 0.5f)
      {
        raw_max[0] = raw_max[3] < 0.5f ? raw_max[3] + 0.5f : raw_max[3] - 0.5f;
        raw_min[0] = raw_min[3] < 0.5f ? raw_min[3] + 0.5f : raw_min[3] - 0.5f;
        reverse_hues = TRUE;
      }
    }
    else if((cst == IOP_CS_LCH && tab == CHANNEL_INDEX_h) || (cst == IOP_CS_JZCZHZ && tab == CHANNEL_INDEX_hz))
    {
      if((raw_max[3] - raw_min[3]) < (raw_max[2] - raw_min[2]) && raw_min[3] < 0.5f && raw_max[3] > 0.5f)
      {
        raw_max[2] = raw_max[3] < 0.5f ? raw_max[3] + 0.5f : raw_max[3] - 0.5f;
        raw_min[2] = raw_min[3] < 0.5f ? raw_min[3] + 0.5f : raw_min[3] - 0.5f;
        reverse_hues = TRUE;
      }
    }

    _blendif_scale(data, cst, raw_min, picker_min, work_profile, in_out);
    _blendif_scale(data, cst, raw_max, picker_max, work_profile, in_out);

    const float feather = 0.01f;

    if(picker_min[tab] > picker_max[tab])
    {
      const float tmp = picker_min[tab];
      picker_min[tab] = picker_max[tab];
      picker_max[tab] = tmp;
    }

    picker_values[0] = CLAMP(picker_min[tab] - feather, 0.f, 1.f);
    picker_values[1] = CLAMP(picker_min[tab] + feather, 0.f, 1.f);
    picker_values[2] = CLAMP(picker_max[tab] - feather, 0.f, 1.f);
    picker_values[3] = CLAMP(picker_max[tab] + feather, 0.f, 1.f);

    if(picker_values[1] > picker_values[2])
    {
      picker_values[1] = CLAMP(picker_min[tab], 0.f, 1.f);
      picker_values[2] = CLAMP(picker_max[tab], 0.f, 1.f);
    }

    picker_values[0] = CLAMP(picker_values[0], 0.f, picker_values[1]);
    picker_values[3] = CLAMP(picker_values[3], picker_values[2], 1.f);

    dt_pthread_mutex_lock(&data->lock);
    for(int k = 0; k < 4; k++)
      dtgtk_gradient_slider_multivalue_set_value(sl->slider, picker_values[k], k);
    dt_pthread_mutex_unlock(&data->lock);

    // update picked values
    _update_gradient_slider_pickers(NULL, module);

    const float boost_factor = _get_boost_factor(data, data->tab, in_out);
    for(int k = 0; k < 4; k++)
    {
      char text[256];
      channel->scale_print(dtgtk_gradient_slider_multivalue_get_value(sl->slider, k), boost_factor,
                           text, sizeof(text));
      gtk_label_set_text(sl->label[k], text);
    }

    --darktable.gui->reset;

    // save values to parameters
    dt_pthread_mutex_lock(&data->lock);
    for(int k = 0; k < 4; k++)
      parameters[k] = dtgtk_gradient_slider_multivalue_get_value(sl->slider, k);
    dt_pthread_mutex_unlock(&data->lock);

    // de-activate processing of this channel if maximum span is selected
    if(parameters[1] == 0.0f && parameters[2] == 1.0f)
      bp->blendif &= ~(1 << ch);
    else
      bp->blendif |= (1 << ch);

    // set the polarity of the channel to include the picked values
    if(reverse_hues == ((bp->mask_combine & DEVELOP_COMBINE_INV) == DEVELOP_COMBINE_INV))
      bp->blendif &= ~(1 << (16 + ch));
    else
      bp->blendif |= 1 << (16 + ch);

    dt_dev_add_history_item(darktable.develop, module, TRUE);
    _blendop_blendif_update_tab(module, tab);

    return TRUE;
  }
  else if(picker == data->colorpicker)
  {
    if(darktable.gui->reset) return TRUE;

    _update_gradient_slider_pickers(NULL, module);

    return TRUE;
  }
  else return FALSE; // needs to be handled by module
}

static gboolean _blendif_change_blend_colorspace(dt_iop_module_t *module, dt_develop_blend_colorspace_t cst)
{
  switch(cst)
  {
    case DEVELOP_BLEND_CS_RAW:
    case DEVELOP_BLEND_CS_LAB:
    case DEVELOP_BLEND_CS_RGB_DISPLAY:
    case DEVELOP_BLEND_CS_RGB_SCENE:
      break;
    default:
      cst = dt_develop_blend_default_module_blend_colorspace(module);
      break;
  }
  if(cst != module->blend_params->blend_cst)
  {
    dt_develop_blend_init_blendif_parameters(module->blend_params, cst);

    // look for last history item for this module with the selected blending mode to copy parametric mask settings
    for(const GList *history = g_list_last(darktable.develop->history); history; history = g_list_previous(history))
    {
      const dt_dev_history_item_t *data = (dt_dev_history_item_t *)(history->data);
      if(data->module == module && data->blend_params->blend_cst == cst)
      {
        const dt_develop_blend_params_t *hp = data->blend_params;
        dt_develop_blend_params_t *np = module->blend_params;

        np->blend_mode = hp->blend_mode;
        np->blend_parameter = hp->blend_parameter;
        np->blendif = hp->blendif;
        memcpy(np->blendif_parameters, hp->blendif_parameters, sizeof(hp->blendif_parameters));
        memcpy(np->blendif_boost_factors, hp->blendif_boost_factors, sizeof(hp->blendif_boost_factors));
        break;
      }
    }

    dt_iop_gui_blend_data_t *bd = module->blend_data;
    const int cst_old = _blendop_blendif_get_picker_colorspace(bd);
    dt_dev_add_new_history_item(darktable.develop, module, FALSE);
    dt_iop_gui_update(module);

    if(cst_old != _blendop_blendif_get_picker_colorspace(bd) &&
       (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(bd->colorpicker)) ||
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(bd->colorpicker_set_values))))
    {
      dt_iop_color_picker_set_cst(bd->module, _blendop_blendif_get_picker_colorspace(bd));
      dt_dev_reprocess_all(bd->module->dev);
      dt_control_queue_redraw();
    }

    return TRUE;
  }
  return FALSE;
}

static void _blendif_select_colorspace(GtkMenuItem *menuitem, dt_iop_module_t *module)
{
  dt_develop_blend_colorspace_t cst = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(menuitem), "dt-blend-cst"));
  if(_blendif_change_blend_colorspace(module, cst))
  {
    gtk_widget_queue_draw(module->widget);
  }
}

static void _blendif_show_output_channels(GtkMenuItem *menuitem, dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  if(!bd || !bd->blendif_support || !bd->blendif_inited) return;
  if(!bd->output_channels_shown)
  {
    bd->output_channels_shown = TRUE;
    dt_iop_gui_update(module);
  }
}

static void _blendif_hide_output_channels(GtkMenuItem *menuitem, dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  if(!bd || !bd->blendif_support || !bd->blendif_inited) return;
  if(bd->output_channels_shown)
  {
    bd->output_channels_shown = FALSE;
    if(_blendif_clean_output_channels(module))
    {
      dt_dev_add_history_item(darktable.develop, module, TRUE);
    }
    dt_iop_gui_update(module);
  }
}

static void _blendif_options_callback(GtkButton *button, GdkEventButton *event, dt_iop_module_t *module)
{
  if(event->button != 1 && event->button != 2) return;
  const dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  if(!bd || !bd->blendif_support || !bd->blendif_inited) return;

  GtkWidget *mi;
  GtkMenu *menu = darktable.gui->presets_popup_menu;
  if(menu) gtk_widget_destroy(GTK_WIDGET(menu));
  darktable.gui->presets_popup_menu = GTK_MENU(gtk_menu_new());
  menu = darktable.gui->presets_popup_menu;

  // add a section to switch blending color spaces
  const dt_develop_blend_colorspace_t module_cst = dt_develop_blend_default_module_blend_colorspace(module);
  const dt_develop_blend_colorspace_t module_blend_cst = module->blend_params->blend_cst;
  if(module_cst == DEVELOP_BLEND_CS_LAB || module_cst == DEVELOP_BLEND_CS_RGB_DISPLAY
      || module_cst == DEVELOP_BLEND_CS_RGB_SCENE)
  {

    mi = gtk_menu_item_new_with_label(_("Reset to default blend colorspace"));
    g_object_set_data_full(G_OBJECT(mi), "dt-blend-cst", GINT_TO_POINTER(DEVELOP_BLEND_CS_NONE), NULL);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_blendif_select_colorspace), module);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

    // only show Lab blending when the module is a Lab module to avoid using it at the wrong place (Lab blending
    // should not be activated for RGB modules before colorin and after colorout)
    if(module_cst == DEVELOP_BLEND_CS_LAB)
    {
      mi = gtk_check_menu_item_new_with_label(_("Lab"));
      dt_gui_add_class(mi, "dt_transparent_background");
      if(module_blend_cst == DEVELOP_BLEND_CS_LAB)
      {
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), TRUE);
        dt_gui_add_class(mi, "active_menu_item");
      }
      g_object_set_data_full(G_OBJECT(mi), "dt-blend-cst", GINT_TO_POINTER(DEVELOP_BLEND_CS_LAB), NULL);
      g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_blendif_select_colorspace), module);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }

    mi = gtk_check_menu_item_new_with_label(_("RGB (display)"));
    dt_gui_add_class(mi, "dt_transparent_background");
    if(module_blend_cst == DEVELOP_BLEND_CS_RGB_DISPLAY)
    {
      gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), TRUE);
      dt_gui_add_class(mi, "active_menu_item");
    }
    g_object_set_data_full(G_OBJECT(mi), "dt-blend-cst", GINT_TO_POINTER(DEVELOP_BLEND_CS_RGB_DISPLAY), NULL);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_blendif_select_colorspace), module);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

    mi = gtk_check_menu_item_new_with_label(_("RGB (scene)"));
    dt_gui_add_class(mi, "dt_transparent_background");
    if(module_blend_cst == DEVELOP_BLEND_CS_RGB_SCENE)
    {
      gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), TRUE);
      dt_gui_add_class(mi, "active_menu_item");
    }
    g_object_set_data_full(G_OBJECT(mi), "dt-blend-cst", GINT_TO_POINTER(DEVELOP_BLEND_CS_RGB_SCENE), NULL);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_blendif_select_colorspace), module);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    if(bd->output_channels_shown)
    {
      mi = gtk_menu_item_new_with_label(_("Reset and hide output channels"));
      g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_blendif_hide_output_channels), module);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }
    else
    {
      mi = gtk_menu_item_new_with_label(_("Show output channels"));
      g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_blendif_show_output_channels), module);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }
  }

  dt_gui_menu_popup(darktable.gui->presets_popup_menu, GTK_WIDGET(button), GDK_GRAVITY_SOUTH_EAST, GDK_GRAVITY_NORTH_EAST);

  dtgtk_button_set_active(DTGTK_BUTTON(button), FALSE);
}

// activate channel/mask view
static void _blendop_blendif_channel_mask_view(GtkWidget *widget, dt_iop_module_t *module, dt_dev_pixelpipe_display_mask_t mode)
{
  dt_iop_gui_blend_data_t *data = module->blend_data;

  dt_dev_pixelpipe_display_mask_t new_request_mask_display = module->request_mask_display | mode;

  // in case user requests channel display: get the cannel
  if(new_request_mask_display & DT_DEV_PIXELPIPE_DISPLAY_CHANNEL)
  {
    dt_dev_pixelpipe_display_mask_t channel = data->channel[data->tab].display_channel;

    if(widget == GTK_WIDGET(data->filter[1].slider))
      channel |= DT_DEV_PIXELPIPE_DISPLAY_OUTPUT;

    new_request_mask_display &= ~DT_DEV_PIXELPIPE_DISPLAY_ANY;
    new_request_mask_display |= channel;
  }

  // only if something has changed: reprocess center view
  if(new_request_mask_display != module->request_mask_display)
  {
    module->request_mask_display = new_request_mask_display;
    dt_iop_refresh_center(module);
  }
}

// toggle channel/mask view
static void _blendop_blendif_channel_mask_view_toggle(GtkWidget *widget, dt_iop_module_t *module, dt_dev_pixelpipe_display_mask_t mode)
{
  dt_iop_gui_blend_data_t *data = module->blend_data;

  dt_dev_pixelpipe_display_mask_t new_request_mask_display = module->request_mask_display & ~DT_DEV_PIXELPIPE_DISPLAY_STICKY;

  // toggle mode
  if(module->request_mask_display & mode)
    new_request_mask_display &= ~mode;
  else
    new_request_mask_display |= mode;

  dt_pthread_mutex_lock(&data->lock);
  if(new_request_mask_display & DT_DEV_PIXELPIPE_DISPLAY_STICKY)
    data->save_for_leave |= DT_DEV_PIXELPIPE_DISPLAY_STICKY;
  else
    data->save_for_leave &= ~DT_DEV_PIXELPIPE_DISPLAY_STICKY;
  dt_pthread_mutex_unlock(&data->lock);

  new_request_mask_display &= ~DT_DEV_PIXELPIPE_DISPLAY_ANY;

  // in case user requests channel display: get the cannel
  if(new_request_mask_display & DT_DEV_PIXELPIPE_DISPLAY_CHANNEL)
  {
    dt_dev_pixelpipe_display_mask_t channel = data->channel[data->tab].display_channel;

    if(widget == GTK_WIDGET(data->filter[1].slider))
      channel |= DT_DEV_PIXELPIPE_DISPLAY_OUTPUT;

    new_request_mask_display &= ~DT_DEV_PIXELPIPE_DISPLAY_ANY;
    new_request_mask_display |= channel;
  }

  if(new_request_mask_display != module->request_mask_display)
  {
    module->request_mask_display = new_request_mask_display;
    dt_iop_refresh_center(module);
  }
}


// magic mode: if mouse cursor enters a gradient slider with shift and/or control pressed we
// enter channel display and/or mask display mode
static gboolean _blendop_blendif_enter(GtkWidget *widget, GdkEventCrossing *event, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return FALSE;
  dt_iop_gui_blend_data_t *data = module->blend_data;

  dt_dev_pixelpipe_display_mask_t mode = 0;

  // depending on shift modifiers we activate channel and/or mask display
  if(dt_modifier_is(event->state, GDK_SHIFT_MASK | GDK_CONTROL_MASK))
  {
    mode = (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL);
  }
  else if(dt_modifier_is(event->state, GDK_SHIFT_MASK))
  {
    mode = DT_DEV_PIXELPIPE_DISPLAY_CHANNEL;
  }
  else if(dt_modifier_is(event->state, GDK_CONTROL_MASK))
  {
    mode = DT_DEV_PIXELPIPE_DISPLAY_MASK;
  }

  dt_pthread_mutex_lock(&data->lock);
  if(mode && data->timeout_handle)
  {
    // purge any remaining timeout handlers
    g_source_remove(data->timeout_handle);
    data->timeout_handle = 0;
  }
  else if(!data->timeout_handle && !(data->save_for_leave & DT_DEV_PIXELPIPE_DISPLAY_STICKY))
  {
    // save request_mask_display to restore later
    data->save_for_leave = module->request_mask_display & ~DT_DEV_PIXELPIPE_DISPLAY_STICKY;
  }
  dt_pthread_mutex_unlock(&data->lock);

  _blendop_blendif_channel_mask_view(widget, module, mode);

  gtk_widget_grab_focus(widget);
  return FALSE;
}


// handler for delayed mask/channel display mode switch-off
static gboolean _blendop_blendif_leave_delayed(gpointer data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)data;
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  int reprocess = 0;

  dt_pthread_mutex_lock(&bd->lock);
  // restore saved request_mask_display and reprocess image
  if(bd->timeout_handle && (module->request_mask_display != (bd->save_for_leave & ~DT_DEV_PIXELPIPE_DISPLAY_STICKY)))
  {
    module->request_mask_display = bd->save_for_leave & ~DT_DEV_PIXELPIPE_DISPLAY_STICKY;
    reprocess = 1;
  }
  bd->timeout_handle = 0;
  dt_pthread_mutex_unlock(&bd->lock);

  if(reprocess)
    dt_iop_refresh_center(module);
  // return FALSE and thereby terminate the handler
  return FALSE;
}

// de-activate magic mode when leaving the gradient slider
static gboolean _blendop_blendif_leave(GtkWidget *widget, GdkEventCrossing *event, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return FALSE;
  dt_iop_gui_blend_data_t *data = module->blend_data;

  // do not immediately switch-off mask/channel display in case user leaves gradient only briefly.
  // instead we activate a handler function that gets triggered after some timeout
  dt_pthread_mutex_lock(&data->lock);
  if(!(module->request_mask_display & DT_DEV_PIXELPIPE_DISPLAY_STICKY) && !data->timeout_handle &&
    (module->request_mask_display != (data->save_for_leave & ~DT_DEV_PIXELPIPE_DISPLAY_STICKY)))
      data->timeout_handle = g_timeout_add(1000, _blendop_blendif_leave_delayed, module);
  dt_pthread_mutex_unlock(&data->lock);

  return FALSE;
}


static gboolean _blendop_blendif_key_press(GtkWidget *widget, GdkEventKey *event, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return FALSE;

  dt_iop_gui_blend_data_t *data = module->blend_data;
  gboolean handled = FALSE;

  const int tab = data->tab;
  const int in_out = (widget == GTK_WIDGET(data->filter[1].slider)) ? 1 : 0;

  switch(event->keyval)
  {
    case GDK_KEY_a:
    case GDK_KEY_A:
      if(data->channel[tab].altdisplay)
        data->altmode[tab][in_out] = (data->channel[tab].altdisplay)(widget, module, data->altmode[tab][in_out] + 1);
      handled = TRUE;
      break;
    case GDK_KEY_c:
      _blendop_blendif_channel_mask_view_toggle(widget, module, DT_DEV_PIXELPIPE_DISPLAY_CHANNEL);
      handled = TRUE;
      break;
    case GDK_KEY_C:
      _blendop_blendif_channel_mask_view_toggle(widget, module, DT_DEV_PIXELPIPE_DISPLAY_CHANNEL | DT_DEV_PIXELPIPE_DISPLAY_STICKY);
      handled = TRUE;
      break;
    case GDK_KEY_m:
    case GDK_KEY_M:
      _blendop_blendif_channel_mask_view_toggle(widget, module, DT_DEV_PIXELPIPE_DISPLAY_MASK);
      handled = TRUE;
  }

  if(handled)
    dt_iop_request_focus(module);

  return handled;
}


#define COLORSTOPS(gradient) sizeof(gradient) / sizeof(dt_iop_gui_blendif_colorstop_t), gradient

const dt_iop_gui_blendif_channel_t Lab_channels[]
    = { { N_("L"), N_("Sliders for L channel"), 1.0f / 100.0f, COLORSTOPS(_gradient_L), TRUE, 0.0f,
          { DEVELOP_BLENDIF_L_in, DEVELOP_BLENDIF_L_out }, DT_DEV_PIXELPIPE_DISPLAY_L,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log, N_("Lightness") },
        { N_("A"), N_("Sliders for a channel"), 1.0f / 256.0f, COLORSTOPS(_gradient_a), TRUE, 0.0f,
          { DEVELOP_BLENDIF_A_in, DEVELOP_BLENDIF_A_out }, DT_DEV_PIXELPIPE_DISPLAY_a,
          _blendif_scale_print_ab, _blendop_blendif_disp_alternative_mag, N_("Green/red") },
        { N_("B"), N_("Sliders for b channel"), 1.0f / 256.0f, COLORSTOPS(_gradient_b), TRUE, 0.0f,
          { DEVELOP_BLENDIF_B_in, DEVELOP_BLENDIF_B_out }, DT_DEV_PIXELPIPE_DISPLAY_b,
          _blendif_scale_print_ab, _blendop_blendif_disp_alternative_mag, N_("Blue/yellow") },
        { N_("C"), N_("Sliders for chroma channel (of LCh)"), 1.0f / 100.0f, COLORSTOPS(_gradient_chroma),
          TRUE, 0.0f,
          { DEVELOP_BLENDIF_C_in, DEVELOP_BLENDIF_C_out }, DT_DEV_PIXELPIPE_DISPLAY_LCH_C,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log, N_("Saturation") },
        { N_("H"), N_("Sliders for hue channel (of LCh)"), 1.0f / 360.0f, COLORSTOPS(_gradient_LCh_hue),
          FALSE, 0.0f,
          { DEVELOP_BLENDIF_h_in, DEVELOP_BLENDIF_h_out }, DT_DEV_PIXELPIPE_DISPLAY_LCH_h,
          _blendif_scale_print_hue, NULL, N_("Hue") },
        { NULL } };

const dt_iop_gui_blendif_channel_t rgb_channels[]
    = { { N_("G"), N_("Sliders for gray value"), 1.0f / 255.0f, COLORSTOPS(_gradient_gray), TRUE, 0.0f,
          { DEVELOP_BLENDIF_GRAY_in, DEVELOP_BLENDIF_GRAY_out }, DT_DEV_PIXELPIPE_DISPLAY_GRAY,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log, N_("Gray") },
        { N_("R"), N_("Sliders for red channel"), 1.0f / 255.0f, COLORSTOPS(_gradient_red), TRUE, 0.0f,
          { DEVELOP_BLENDIF_RED_in, DEVELOP_BLENDIF_RED_out }, DT_DEV_PIXELPIPE_DISPLAY_R,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log, N_("Red") },
        { N_("G"), N_("Sliders for green channel"), 1.0f / 255.0f, COLORSTOPS(_gradient_green), TRUE, 0.0f,
          { DEVELOP_BLENDIF_GREEN_in, DEVELOP_BLENDIF_GREEN_out }, DT_DEV_PIXELPIPE_DISPLAY_G,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log, N_("Green") },
        { N_("B"), N_("Sliders for blue channel"), 1.0f / 255.0f, COLORSTOPS(_gradient_blue), TRUE, 0.0f,
          { DEVELOP_BLENDIF_BLUE_in, DEVELOP_BLENDIF_BLUE_out }, DT_DEV_PIXELPIPE_DISPLAY_B,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log, N_("Blue") },
        { N_("H"), N_("Sliders for hue channel (of HSL)"), 1.0f / 360.0f, COLORSTOPS(_gradient_HSL_hue),
          FALSE, 0.0f,
          { DEVELOP_BLENDIF_H_in, DEVELOP_BLENDIF_H_out }, DT_DEV_PIXELPIPE_DISPLAY_HSL_H,
          _blendif_scale_print_hue, NULL, N_("Hue") },
        { N_("S"), N_("Sliders for chroma channel (of HSL)"), 1.0f / 100.0f, COLORSTOPS(_gradient_chroma),
          FALSE, 0.0f,
          { DEVELOP_BLENDIF_S_in, DEVELOP_BLENDIF_S_out }, DT_DEV_PIXELPIPE_DISPLAY_HSL_S,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log, N_("Chroma") },
        { N_("L"), N_("Sliders for value channel (of HSL)"), 1.0f / 100.0f, COLORSTOPS(_gradient_gray),
          FALSE, 0.0f,
          { DEVELOP_BLENDIF_l_in, DEVELOP_BLENDIF_l_out }, DT_DEV_PIXELPIPE_DISPLAY_HSL_l,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log, N_("Luminance") },
        { NULL } };

const dt_iop_gui_blendif_channel_t rgbj_channels[]
    = { { N_("G"), N_("Sliders for gray value"), 1.0f / 255.0f, COLORSTOPS(_gradient_gray), TRUE, 0.0f,
          { DEVELOP_BLENDIF_GRAY_in, DEVELOP_BLENDIF_GRAY_out }, DT_DEV_PIXELPIPE_DISPLAY_GRAY,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log, N_("Gray") },
        { N_("R"), N_("Sliders for red channel"), 1.0f / 255.0f, COLORSTOPS(_gradient_red), TRUE, 0.0f,
          { DEVELOP_BLENDIF_RED_in, DEVELOP_BLENDIF_RED_out }, DT_DEV_PIXELPIPE_DISPLAY_R,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log, N_("Red") },
        { N_("G"), N_("Sliders for green channel"), 1.0f / 255.0f, COLORSTOPS(_gradient_green), TRUE, 0.0f,
          { DEVELOP_BLENDIF_GREEN_in, DEVELOP_BLENDIF_GREEN_out }, DT_DEV_PIXELPIPE_DISPLAY_G,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log, N_("Green") },
        { N_("B"), N_("Sliders for blue channel"), 1.0f / 255.0f, COLORSTOPS(_gradient_blue), TRUE, 0.0f,
          { DEVELOP_BLENDIF_BLUE_in, DEVELOP_BLENDIF_BLUE_out }, DT_DEV_PIXELPIPE_DISPLAY_B,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log, N_("Blue") },
        { N_("Jz"), N_("Sliders for value channel (of JzCzhz)"), 1.0f / 100.0f, COLORSTOPS(_gradient_gray),
          TRUE, -6.64385619f, // cf. _blend_init_blendif_boost_parameters
          { DEVELOP_BLENDIF_Jz_in, DEVELOP_BLENDIF_Jz_out }, DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_Jz,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log, N_("Luminance") },
        { N_("Cz"), N_("Sliders for chroma channel (of JzCzhz)"), 1.0f / 100.0f, COLORSTOPS(_gradient_chroma),
          TRUE, -6.64385619f, // cf. _blend_init_blendif_boost_parameters
          { DEVELOP_BLENDIF_Cz_in, DEVELOP_BLENDIF_Cz_out }, DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_Cz,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log, N_("Chroma") },
        { N_("Hz"), N_("Sliders for hue channel (of JzCzhz)"), 1.0f / 360.0f, COLORSTOPS(_gradient_JzCzhz_hue),
          FALSE, 0.0f,
          { DEVELOP_BLENDIF_hz_in, DEVELOP_BLENDIF_hz_out }, DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_hz,
          _blendif_scale_print_hue, NULL, N_("Hue") },
        { NULL } };

const char *slider_tooltip[] = { N_("Adjustment based on input received by this module:\n* range defined by upper markers: "
                                    "blend fully\n* range defined by lower markers: do not blend at all\n* range between "
                                    "adjacent upper/lower markers: blend gradually"),
                                 N_("Adjustment based on unblended output of this module:\n* range defined by upper "
                                    "markers: blend fully\n* range defined by lower markers: do not blend at all\n* range "
                                    "between adjacent upper/lower markers: blend gradually") };


void dt_iop_gui_update_blendif(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;

  if(!bd || !bd->blendif_support || !bd->blendif_inited) return;

  ++darktable.gui->reset;

  dt_pthread_mutex_lock(&bd->lock);
  if(bd->timeout_handle)
  {
    g_source_remove(bd->timeout_handle);
    bd->timeout_handle = 0;
    if(module->request_mask_display != (bd->save_for_leave & ~DT_DEV_PIXELPIPE_DISPLAY_STICKY))
    {
      module->request_mask_display = bd->save_for_leave & ~DT_DEV_PIXELPIPE_DISPLAY_STICKY;
      dt_dev_reprocess_all(module->dev);//DBG
    }
  }
  dt_pthread_mutex_unlock(&bd->lock);

  /* update output channel mask visibility */
  gtk_widget_set_visible(GTK_WIDGET(bd->filter[1].box), bd->output_channels_shown);

  /* update tabs */
  if(bd->channel_tabs_csp != bd->csp)
  {
    bd->channel = NULL;

    switch(bd->csp)
    {
      case DEVELOP_BLEND_CS_LAB:
        bd->channel = Lab_channels;
        break;
      case DEVELOP_BLEND_CS_RGB_DISPLAY:
        bd->channel = rgb_channels;
        break;
      case DEVELOP_BLEND_CS_RGB_SCENE:
        bd->channel = rgbj_channels;
        break;
      default:
        assert(FALSE); // blendif not supported for RAW, which is already caught upstream; we should not get
                       // here
    }

    dt_iop_color_picker_reset(module, TRUE);

    /* remove tabs before adding others */
    dt_gui_container_destroy_children(GTK_CONTAINER(bd->channel_tabs));

    bd->channel_tabs_csp = bd->csp;

    int index = 0;
    for(const dt_iop_gui_blendif_channel_t *ch = bd->channel; ch->label != NULL; ch++, index++)
    {
      dt_ui_notebook_page(bd->channel_tabs, ch->label, _(ch->tooltip));
      gtk_widget_show_all(GTK_WIDGET(gtk_notebook_get_nth_page(bd->channel_tabs, index)));
    }

    bd->tab = 0;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(bd->channel_tabs), bd->tab);
  }

  _blendop_blendif_update_tab(module, bd->tab);

  --darktable.gui->reset;
}


void dt_iop_gui_init_blendif(GtkWidget *blendw, dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

  bd->blendif_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE));
  // add event box so that one can click into the area to get help for parametric masks
  _add_wrapped_box(blendw, bd->blendif_box, "masks_parametric");

  /* create and add blendif support if module supports it */
  if(bd->blendif_support)
  {
    GtkWidget *section = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    gtk_box_pack_start(GTK_BOX(section), dt_ui_label_new(_("Parametric mask")), TRUE, TRUE, 0);
    dt_gui_add_class(section, "dt_section_label");

    dt_iop_togglebutton_new(module, "blend`tools", N_("Reset blend mask settings"), NULL,
                            G_CALLBACK(_blendop_blendif_reset), FALSE, 0, 0,
                            dtgtk_cairo_paint_reset, section);

    gtk_box_pack_start(GTK_BOX(bd->blendif_box), GTK_WIDGET(section), TRUE, FALSE, 0);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    bd->tab = 0;
    bd->channel_tabs_csp = DEVELOP_BLEND_CS_NONE;
    bd->channel_tabs = GTK_NOTEBOOK(gtk_notebook_new());
    dt_action_define_iop(module, "blend", N_("Channel"), GTK_WIDGET(bd->channel_tabs), &dt_action_def_tabs_none);

    gtk_notebook_set_scrollable(bd->channel_tabs, TRUE);
    gtk_box_pack_start(GTK_BOX(header), GTK_WIDGET(bd->channel_tabs), TRUE, TRUE, 0);

    // a little padding between the notbook with all channels and the icons for pickers.
    gtk_box_pack_start(GTK_BOX(header), gtk_label_new(""),
                       FALSE, FALSE, DT_PIXEL_APPLY_DPI(10));

    bd->colorpicker = dt_color_picker_new(module, DT_COLOR_PICKER_POINT_AREA | DT_COLOR_PICKER_IO, header);
    gtk_widget_set_tooltip_text(bd->colorpicker, _("Pick GUI color from image\nctrl+click or right-click to select an area"));
    gtk_widget_set_name(bd->colorpicker, "keep-active");

    bd->colorpicker_set_values = dt_color_picker_new(module, DT_COLOR_PICKER_AREA | DT_COLOR_PICKER_IO, header);
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(bd->colorpicker_set_values),
                                 dtgtk_cairo_paint_colorpicker_set_values, 0, NULL);
    dt_gui_add_class(bd->colorpicker_set_values, "dt_transparent_background");
    gtk_widget_set_tooltip_text(bd->colorpicker_set_values, _("Set the range based on an area from the image\n"
                                                              "drag to use the input image\n"
                                                              "ctrl+drag to use the output image"));

    GtkWidget *btn = dt_iop_togglebutton_new(module, "blend`tools", N_("Invert all channel's polarities"), NULL,
                                             G_CALLBACK(_blendop_blendif_invert), FALSE, 0, 0,
                                             dtgtk_cairo_paint_invert, header);
    dt_gui_add_class(btn, "dt_ignore_fg_state");

    gtk_box_pack_start(GTK_BOX(bd->blendif_box), GTK_WIDGET(header), TRUE, FALSE, 0);

    for(int in_out = 1; in_out >= 0; in_out--)
    {
      dt_iop_gui_blendif_filter_t *sl = &bd->filter[in_out];

      GtkWidget *slider_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

      sl->slider = DTGTK_GRADIENT_SLIDER_MULTIVALUE(dtgtk_gradient_slider_multivalue_new_with_name(4,
                                                   in_out ? "blend-upper" : "blend-lower"));
      gtk_box_pack_start(GTK_BOX(slider_box), GTK_WIDGET(sl->slider), TRUE, TRUE, 0);

      sl->polarity = dtgtk_togglebutton_new(dtgtk_cairo_paint_plusminus, 0, NULL);
      dt_gui_add_class(sl->polarity, "dt_ignore_fg_state");
      dt_gui_add_class(sl->polarity, "dt_transparent_background");
      gtk_widget_set_tooltip_text(sl->polarity, _("Toggle polarity. Best seen by enabling 'display mask'"));
      gtk_box_pack_end(GTK_BOX(slider_box), GTK_WIDGET(sl->polarity), FALSE, FALSE, 0);

      GtkWidget *label_box = gtk_grid_new();
      gtk_grid_set_column_homogeneous(GTK_GRID(label_box), TRUE);

      sl->head = GTK_LABEL(dt_ui_label_new(in_out ? _("Output") : _("Input")));
      gtk_grid_attach(GTK_GRID(label_box), GTK_WIDGET(sl->head), 0, 0, 1, 1);

      GtkWidget *overlay = gtk_overlay_new();
      gtk_grid_attach(GTK_GRID(label_box), overlay, 1, 0, 3, 1);

      sl->picker_label = GTK_LABEL(gtk_label_new(""));
      gtk_widget_set_name(GTK_WIDGET(sl->picker_label), "blend-data");
      gtk_label_set_xalign(sl->picker_label, .0);
      gtk_label_set_yalign(sl->picker_label, 1.0);
      gtk_container_add(GTK_CONTAINER(overlay), GTK_WIDGET(sl->picker_label));

      for(int k = 0; k < 4; k++)
      {
        sl->label[k] = GTK_LABEL(gtk_label_new(NULL));
        gtk_widget_set_name(GTK_WIDGET(sl->label[k]), "blend-data");
        gtk_label_set_xalign(sl->label[k], .35 + k * .65/3);
        gtk_label_set_yalign(sl->label[k], k % 2);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), GTK_WIDGET(sl->label[k]));
      }

      gtk_widget_set_tooltip_text(GTK_WIDGET(sl->slider), _("Double-click to reset.\nPress 'a' to toggle available slider modes.\nPress 'c' to toggle view of channel data.\nPress 'm' to toggle mask view."));
      gtk_widget_set_tooltip_text(GTK_WIDGET(sl->head), _(slider_tooltip[in_out]));

      g_signal_connect(G_OBJECT(sl->slider), "value-changed", G_CALLBACK(_blendop_blendif_sliders_callback), bd);
      g_signal_connect(G_OBJECT(sl->slider), "value-reset", G_CALLBACK(_blendop_blendif_sliders_reset_callback), bd);
      g_signal_connect(G_OBJECT(sl->slider), "leave-notify-event", G_CALLBACK(_blendop_blendif_leave), module);
      g_signal_connect(G_OBJECT(sl->slider), "enter-notify-event", G_CALLBACK(_blendop_blendif_enter), module);
      g_signal_connect(G_OBJECT(sl->slider), "key-press-event", G_CALLBACK(_blendop_blendif_key_press), module);
      g_signal_connect(G_OBJECT(sl->polarity), "toggled", G_CALLBACK(_blendop_blendif_polarity_callback), bd);

      sl->box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE));
      gtk_box_pack_start(GTK_BOX(sl->box), GTK_WIDGET(label_box), TRUE, FALSE, 0);
      gtk_box_pack_start(GTK_BOX(sl->box), GTK_WIDGET(slider_box), TRUE, FALSE, 0);
      gtk_box_pack_start(GTK_BOX(bd->blendif_box), GTK_WIDGET(sl->box), TRUE, FALSE, 0);
    }

    bd->channel_boost_factor_slider = dt_bauhaus_slider_new_with_range(module, 0.0f, 18.0f, 0, 0.0f, 3);
    dt_bauhaus_slider_set_format(bd->channel_boost_factor_slider, _(" EV"));
    dt_bauhaus_widget_set_label(bd->channel_boost_factor_slider, N_("Blend"), N_("Boost factor"));
    dt_bauhaus_slider_set_soft_range(bd->channel_boost_factor_slider, 0.0, 3.0);
    gtk_widget_set_tooltip_text(bd->channel_boost_factor_slider, _("Adjust the boost factor of the channel mask"));
    gtk_widget_set_sensitive(bd->channel_boost_factor_slider, FALSE);

    g_signal_connect(G_OBJECT(bd->channel_boost_factor_slider), "value-changed",
                     G_CALLBACK(_blendop_blendif_boost_factor_callback), bd);

    gtk_box_pack_start(GTK_BOX(bd->blendif_box), GTK_WIDGET(bd->channel_boost_factor_slider), TRUE, FALSE, 0);

    g_signal_connect(G_OBJECT(bd->channel_tabs), "switch_page", G_CALLBACK(_blendop_blendif_tab_switch), bd);
    g_signal_connect(G_OBJECT(bd->colorpicker), "toggled", G_CALLBACK(_update_gradient_slider_pickers), module);
    g_signal_connect(G_OBJECT(bd->colorpicker_set_values), "toggled", G_CALLBACK(_update_gradient_slider_pickers), module);

    bd->blendif_inited = 1;
  }
}

void dt_iop_gui_update_masks(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  dt_develop_blend_params_t *bp = module->blend_params;

  if(!bd || !bd->masks_support || !bd->masks_inited) return;

  ++darktable.gui->reset;

  /* update masks state */
  dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, module->blend_params->mask_id);
  dt_bauhaus_combobox_clear(bd->masks_combo);
  if(grp && (grp->type & DT_MASKS_GROUP) && grp->points)
  {
    char txt[512];
    const guint n = g_list_length(grp->points);
    snprintf(txt, sizeof(txt), ngettext("%d Shape used", "%d shapes used", n), n);
    dt_bauhaus_combobox_add(bd->masks_combo, txt);
  }
  else
  {
    dt_bauhaus_combobox_add(bd->masks_combo, _("No mask used"));
    bd->masks_shown = DT_MASKS_EDIT_OFF;
    // reset the gui
    dt_masks_set_edit_mode(module, DT_MASKS_EDIT_OFF);
  }
  dt_bauhaus_combobox_set(bd->masks_combo, 0);

  if(bd->masks_support)
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), bd->masks_shown != DT_MASKS_EDIT_OFF);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_polarity),
                                 bp->mask_combine & DEVELOP_COMBINE_MASKS_POS);
  }

  // update buttons status
  for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
  {
    if(module->dev->form_gui && module->dev->form_visible && module->dev->form_gui->creation
       && module->dev->form_gui->creation_module == module
       && (module->dev->form_visible->type & bd->masks_type[n]))
    {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_shapes[n]), TRUE);
    }
    else
    {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_shapes[n]), FALSE);
    }
  }

  --darktable.gui->reset;
}

void dt_iop_gui_init_masks(GtkWidget *blendw, dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

  bd->masks_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  _add_wrapped_box(blendw, bd->masks_box, "masks_drawn");

  /* create and add masks support if module supports it */
  if(bd->masks_support)
  {
    bd->masks_combo_ids = NULL;
    bd->masks_shown = DT_MASKS_EDIT_OFF;

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    dt_gui_add_class(hbox, "dt_section_label"); // make the combo look like a section label

    bd->masks_combo = dt_bauhaus_combobox_new(module);
    dt_bauhaus_widget_set_label(bd->masks_combo, N_("Blend"), N_("Drawn mask"));
    dt_bauhaus_widget_set_section(bd->masks_combo, TRUE);

    dt_bauhaus_combobox_add(bd->masks_combo, _("No mask used"));
    g_signal_connect(G_OBJECT(bd->masks_combo), "value-changed",
                     G_CALLBACK(dt_masks_iop_value_changed_callback), module);
    dt_bauhaus_combobox_add_populate_fct(bd->masks_combo, dt_masks_iop_combo_populate);
    gtk_box_pack_start(GTK_BOX(hbox), bd->masks_combo, TRUE, TRUE, 0);

    bd->masks_polarity = dt_iop_togglebutton_new(module, "blend`tools", N_("Toggle polarity of drawn mask"), NULL,
                                                 G_CALLBACK(_blendop_masks_polarity_callback),
                                                 FALSE, 0, 0, dtgtk_cairo_paint_plusminus, hbox);
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(bd->masks_polarity), dtgtk_cairo_paint_plusminus, 0, NULL);
    dt_gui_add_class(bd->masks_polarity, "dt_ignore_fg_state");

    GtkWidget *abox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    bd->masks_edit = dt_iop_togglebutton_new(module, "blend`tools", N_("Show and edit mask elements"), N_("Show and edit in restricted mode"),
                                             G_CALLBACK(_blendop_masks_show_and_edit),
                                             FALSE, 0, 0, dtgtk_cairo_paint_masks_eye, abox);

    bd->masks_type[0] = DT_MASKS_GRADIENT;
    bd->masks_shapes[0] = dt_iop_togglebutton_new(module, "blend`shapes", N_("Add gradient"), N_("Add multiple gradients"),
                                                  G_CALLBACK(_blendop_masks_add_shape),
                                                  FALSE, 0, 0, dtgtk_cairo_paint_masks_gradient, abox);

    bd->masks_type[4] = DT_MASKS_BRUSH;
    bd->masks_shapes[4] = dt_iop_togglebutton_new(module, "blend`shapes", N_("Add brush"), N_("Add multiple brush strokes"),
                                                  G_CALLBACK(_blendop_masks_add_shape),
                                                  FALSE, 0, 0, dtgtk_cairo_paint_masks_brush, abox);

    bd->masks_type[1] = DT_MASKS_PATH;
    bd->masks_shapes[1] = dt_iop_togglebutton_new(module, "blend`shapes", N_("Add path"), N_("Add multiple paths"),
                                                  G_CALLBACK(_blendop_masks_add_shape),
                                                  FALSE, 0, 0, dtgtk_cairo_paint_masks_path, abox);

    bd->masks_type[2] = DT_MASKS_ELLIPSE;
    bd->masks_shapes[2] = dt_iop_togglebutton_new(module, "blend`shapes", N_("Add ellipse"), N_("Add multiple ellipses"),
                                                  G_CALLBACK(_blendop_masks_add_shape),
                                                  FALSE, 0, 0, dtgtk_cairo_paint_masks_ellipse, abox);

    bd->masks_type[3] = DT_MASKS_CIRCLE;
    bd->masks_shapes[3] = dt_iop_togglebutton_new(module, "blend`shapes", N_("Add circle"), N_("Add multiple circles"),
                                                  G_CALLBACK(_blendop_masks_add_shape),
                                                  FALSE, 0, 0, dtgtk_cairo_paint_masks_circle, abox);

    gtk_box_pack_start(GTK_BOX(bd->masks_box), GTK_WIDGET(hbox), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->masks_box), GTK_WIDGET(abox), TRUE, TRUE, 0);

    bd->masks_inited = 1;
  }
}

typedef struct raster_combo_entry_t
{
  dt_iop_module_t *module;
  int id;
} raster_combo_entry_t;

static void _raster_combo_populate(GtkWidget *w, struct dt_iop_module_t **m)
{
  dt_iop_module_t *module = *m;
  dt_iop_request_focus(module);

  dt_bauhaus_combobox_clear(w);

  raster_combo_entry_t *entry = (raster_combo_entry_t *)malloc(sizeof(raster_combo_entry_t));
  entry->module = NULL;
  entry->id = 0;
  dt_bauhaus_combobox_add_full(w, _("No mask used"), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, entry, free, TRUE);

  int i = 1;

  for(GList* iter = darktable.develop->iop; iter; iter = g_list_next(iter))
  {
    dt_iop_module_t *iop = (dt_iop_module_t *)iter->data;
    if(iop == module)
      break;

    GHashTableIter masks_iter;
    gpointer key, value;

    g_hash_table_iter_init(&masks_iter, iop->raster_mask.source.masks);
    while(g_hash_table_iter_next(&masks_iter, &key, &value))
    {
      const int id = GPOINTER_TO_INT(key);
      const char *modulename = (char *)value;
      entry = (raster_combo_entry_t *)malloc(sizeof(raster_combo_entry_t));
      entry->module = iop;
      entry->id = id;
      dt_bauhaus_combobox_add_full(w, modulename, DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, entry, free, TRUE);
      if(iop == module->raster_mask.sink.source && module->raster_mask.sink.id == id)
        dt_bauhaus_combobox_set(w, i);
      i++;
    }
  }
}

static void _raster_value_changed_callback(GtkWidget *widget, struct dt_iop_module_t *module)
{
  raster_combo_entry_t *entry = dt_bauhaus_combobox_get_data(widget);

  // nothing to do
  if(entry->module == module->raster_mask.sink.source && entry->id == module->raster_mask.sink.id)
    return;

  if(module->raster_mask.sink.source)
  {
    // we no longer use this one
    g_hash_table_remove(module->raster_mask.sink.source->raster_mask.source.users, module);
  }

  module->raster_mask.sink.source = entry->module;
  module->raster_mask.sink.id = entry->id;

  gboolean reprocess = FALSE;

  if(entry->module)
  {
    reprocess = dt_iop_is_raster_mask_used(entry->module, 0) == FALSE;
    g_hash_table_add(entry->module->raster_mask.source.users, module);

    // update blend_params!
    memcpy(module->blend_params->raster_mask_source, entry->module->op, sizeof(module->blend_params->raster_mask_source));
    module->blend_params->raster_mask_instance = entry->module->multi_priority;
    module->blend_params->raster_mask_id = entry->id;
  }
  else
  {
    memset(module->blend_params->raster_mask_source, 0, sizeof(module->blend_params->raster_mask_source));
    module->blend_params->raster_mask_instance = 0;
    module->blend_params->raster_mask_id = 0;
  }

  dt_dev_add_history_item(module->dev, module, TRUE);

  if(reprocess)
    dt_dev_reprocess_all(module->dev);
}

void dt_iop_gui_update_raster(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  dt_develop_blend_params_t *bp = module->blend_params;

  if(!bd || !bd->masks_support || !bd->raster_inited) return;

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->raster_polarity), bp->raster_mask_invert);

  _raster_combo_populate(bd->raster_combo, &module);
}

static void _raster_polarity_callback(GtkToggleButton *togglebutton, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  dt_develop_blend_params_t *bp = (dt_develop_blend_params_t *)self->blend_params;

  bp->raster_mask_invert = gtk_toggle_button_get_active(togglebutton);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  dt_control_queue_redraw_widget(GTK_WIDGET(togglebutton));
}

void dt_iop_gui_init_raster(GtkWidget *blendw, dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

  bd->raster_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  _add_wrapped_box(blendw, bd->raster_box, "masks_raster");

  /* create and add raster support if module supports it (it's coupled to masks at the moment) */
  if(bd->masks_support)
  {
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    bd->raster_combo = dt_bauhaus_combobox_new(module);
    dt_bauhaus_widget_set_label(bd->raster_combo, N_("Blend"), N_("Raster mask"));
    dt_bauhaus_combobox_add(bd->raster_combo, _("No mask used"));
    g_signal_connect(G_OBJECT(bd->raster_combo), "value-changed",
                     G_CALLBACK(_raster_value_changed_callback), module);
    dt_bauhaus_combobox_add_populate_fct(bd->raster_combo, _raster_combo_populate);
    gtk_box_pack_start(GTK_BOX(hbox), bd->raster_combo, TRUE, TRUE, 0);

    bd->raster_polarity = dtgtk_togglebutton_new(dtgtk_cairo_paint_plusminus, 0, NULL);
    dt_gui_add_class(bd->raster_polarity, "dt_ignore_fg_state");
    gtk_widget_set_tooltip_text(bd->raster_polarity, _("Toggle polarity of raster mask"));
    g_signal_connect(G_OBJECT(bd->raster_polarity), "toggled", G_CALLBACK(_raster_polarity_callback), module);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->raster_polarity), FALSE);
    gtk_box_pack_start(GTK_BOX(hbox), bd->raster_polarity, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(bd->raster_box), GTK_WIDGET(hbox), TRUE, TRUE, 0);

    bd->raster_inited = 1;
  }
}

void dt_iop_gui_cleanup_blending(dt_iop_module_t *module)
{
  if(!module->blend_data) return;
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

  dt_pthread_mutex_lock(&bd->lock);
  if(bd->timeout_handle)
    g_source_remove(bd->timeout_handle);

  g_list_free(bd->masks_modes);
  g_list_free(bd->masks_modes_toggles);
  free(bd->masks_combo_ids);
  dt_pthread_mutex_unlock(&bd->lock);
  dt_pthread_mutex_destroy(&bd->lock);

  g_free(module->blend_data);
  module->blend_data = NULL;
}


static gboolean _add_blendmode_combo(GtkWidget *combobox, dt_develop_blend_mode_t mode)
{
  for(const dt_develop_name_value_t *bm = dt_develop_blend_mode_names; *bm->name; bm++)
  {
    if(bm->value == mode)
    {
      dt_bauhaus_combobox_add_full(combobox, Q_(bm->name), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, GUINT_TO_POINTER(bm->value), NULL, TRUE);

      return TRUE;
    }
  }

  return FALSE;
}

static GtkWidget *_combobox_new_from_list(dt_iop_module_t *module, const gchar *label,
                                          const dt_develop_name_value_t *list, uint32_t *field, const gchar *tooltip)
{
  GtkWidget *combo = dt_bauhaus_combobox_new(module);

  if(field)
    dt_bauhaus_widget_set_field(combo, field, DT_INTROSPECTION_TYPE_ENUM);
  dt_bauhaus_widget_set_label(combo, N_("Blend"), label);
  gtk_widget_set_tooltip_text(combo, tooltip);
  for(; *list->name; list++)
    dt_bauhaus_combobox_add_full(combo, _(list->name), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,
                                 GUINT_TO_POINTER(list->value), NULL, TRUE);

  return combo;
}

void dt_iop_gui_update_blending(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

  if(!(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING) || !bd || !bd->blend_inited) return;

  ++darktable.gui->reset;

  // update color space from parameters
  const dt_develop_blend_colorspace_t default_csp = dt_develop_blend_default_module_blend_colorspace(module);
  switch(default_csp)
  {
    case DEVELOP_BLEND_CS_RAW:
      bd->csp = DEVELOP_BLEND_CS_RAW;
      break;
    case DEVELOP_BLEND_CS_LAB:
    case DEVELOP_BLEND_CS_RGB_DISPLAY:
    case DEVELOP_BLEND_CS_RGB_SCENE:
      switch(module->blend_params->blend_cst)
      {
        case DEVELOP_BLEND_CS_LAB:
        case DEVELOP_BLEND_CS_RGB_DISPLAY:
        case DEVELOP_BLEND_CS_RGB_SCENE:
          bd->csp = module->blend_params->blend_cst;
          break;
        default:
          bd->csp = default_csp;
          break;
      }
      break;
    case DEVELOP_BLEND_CS_NONE:
    default:
      bd->csp = DEVELOP_BLEND_CS_NONE;
      break;
  }

  const unsigned int mode = g_list_index(bd->masks_modes, GUINT_TO_POINTER(module->blend_params->mask_mode));

  // unsets currently toggled if any, won't try to untoggle the cancel button
  if(bd->selected_mask_mode
     != g_list_nth_data(bd->masks_modes_toggles,
                        g_list_index(bd->masks_modes, GUINT_TO_POINTER(DEVELOP_MASK_DISABLED))))
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->selected_mask_mode), FALSE);
  }

  if(mode > 0)
  {
    GtkToggleButton *to_be_activated = GTK_TOGGLE_BUTTON(g_list_nth_data(bd->masks_modes_toggles, mode));
    gtk_toggle_button_set_active(to_be_activated, TRUE);
    bd->selected_mask_mode = GTK_WIDGET(to_be_activated);
  }
  else
  {
    bd->selected_mask_mode = g_list_nth_data(
        bd->masks_modes_toggles, g_list_index(bd->masks_modes, GUINT_TO_POINTER(DEVELOP_MASK_DISABLED)));
  }

  // (un)set the mask indicator
  add_remove_mask_indicator(module, (module->blend_params->mask_mode != DEVELOP_MASK_DISABLED) &&
                            (module->blend_params->mask_mode != DEVELOP_MASK_ENABLED));

  // initialization of blending modes
  if(bd->csp != bd->blend_modes_csp)
  {
    dt_bauhaus_combobox_clear(bd->blend_modes_combo);

    if(bd->csp == DEVELOP_BLEND_CS_LAB
       || bd->csp == DEVELOP_BLEND_CS_RGB_DISPLAY
       || bd->csp == DEVELOP_BLEND_CS_RAW )
    {
      dt_bauhaus_combobox_add_section(bd->blend_modes_combo, _("Normal & difference modes"));
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_NORMAL2);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_BOUNDED);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_AVERAGE);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_DIFFERENCE2);
      dt_bauhaus_combobox_add_section(bd->blend_modes_combo, _("Lighten modes"));
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_LIGHTEN);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_ADD);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_SCREEN);
      dt_bauhaus_combobox_add_section(bd->blend_modes_combo, _("Darken modes"));
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_DARKEN);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_SUBTRACT);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_MULTIPLY);
      dt_bauhaus_combobox_add_section(bd->blend_modes_combo, _("Contrast enhancing modes"));
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_OVERLAY);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_SOFTLIGHT);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_HARDLIGHT);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_VIVIDLIGHT);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_LINEARLIGHT);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_PINLIGHT);

      if(bd->csp == DEVELOP_BLEND_CS_LAB)
      {
        dt_bauhaus_combobox_add_section(bd->blend_modes_combo, _("Color channel modes"));
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_LAB_LIGHTNESS);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_LAB_A);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_LAB_B);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_LAB_COLOR);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_LIGHTNESS);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_CHROMATICITY);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_HUE);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_COLOR);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_COLORADJUST);
      }
      else if(bd->csp == DEVELOP_BLEND_CS_RGB_DISPLAY)
      {
        dt_bauhaus_combobox_add_section(bd->blend_modes_combo, _("Color channel modes"));
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_RGB_R);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_RGB_G);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_RGB_B);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_LIGHTNESS);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_HSV_VALUE);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_CHROMATICITY);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_HSV_COLOR);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_HUE);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_COLOR);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_COLORADJUST);
      }
    }
    else if(bd->csp == DEVELOP_BLEND_CS_RGB_SCENE)
    {
      dt_bauhaus_combobox_add_section(bd->blend_modes_combo, _("Normal & arithmetic modes"));
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_NORMAL2);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_MULTIPLY);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_DIVIDE);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_ADD);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_SUBTRACT);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_DIFFERENCE2);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_AVERAGE);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_GEOMETRIC_MEAN);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_HARMONIC_MEAN);
      dt_bauhaus_combobox_add_section(bd->blend_modes_combo, _("Color channel modes"));
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_RGB_R);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_RGB_G);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_RGB_B);
      dt_bauhaus_combobox_add_section(bd->blend_modes_combo, _("Chromaticity & lightness modes"));
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_LIGHTNESS);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_CHROMATICITY);
    }
    bd->blend_modes_csp = bd->csp;
  }

  dt_develop_blend_mode_t blend_mode = module->blend_params->blend_mode & DEVELOP_BLEND_MODE_MASK;
  if(!dt_bauhaus_combobox_set_from_value(bd->blend_modes_combo, blend_mode))
  {
    // add deprecated blend mode
    dt_bauhaus_combobox_add_section(bd->blend_modes_combo, _("Deprecated modes"));
    if(!_add_blendmode_combo(bd->blend_modes_combo, blend_mode))
    {
      // should never happen: unknown blend mode
      dt_control_log("unknown blend mode '%d' in module '%s'", blend_mode, module->op);
      module->blend_params->blend_mode = DEVELOP_BLEND_NORMAL2;
      blend_mode = DEVELOP_BLEND_NORMAL2;
    }

    dt_bauhaus_combobox_set_from_value(bd->blend_modes_combo, blend_mode);
  }

  gboolean blend_mode_reversed = (module->blend_params->blend_mode & DEVELOP_BLEND_REVERSE) == DEVELOP_BLEND_REVERSE;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->blend_modes_blend_order), blend_mode_reversed);

  dt_bauhaus_slider_set(bd->blend_mode_parameter_slider, module->blend_params->blend_parameter);
  gtk_widget_set_sensitive(bd->blend_mode_parameter_slider,
                           _blendif_blend_parameter_enabled(bd->blend_modes_csp, module->blend_params->blend_mode));
  gtk_widget_set_visible(bd->blend_mode_parameter_slider, bd->blend_modes_csp == DEVELOP_BLEND_CS_RGB_SCENE);

  dt_bauhaus_combobox_set_from_value(bd->masks_combine_combo,
                                     module->blend_params->mask_combine & (DEVELOP_COMBINE_INV | DEVELOP_COMBINE_INCL));
  dt_bauhaus_combobox_set_from_value(bd->masks_invert_combo,
                                     module->blend_params->mask_combine & DEVELOP_COMBINE_INV);
  dt_bauhaus_slider_set(bd->opacity_slider, module->blend_params->opacity);
  dt_bauhaus_combobox_set_from_value(bd->masks_feathering_guide_combo,
                                     module->blend_params->feathering_guide);
  dt_bauhaus_slider_set(bd->feathering_radius_slider, module->blend_params->feathering_radius);
  dt_bauhaus_slider_set(bd->blur_radius_slider, module->blend_params->blur_radius);
  dt_bauhaus_slider_set(bd->brightness_slider, module->blend_params->brightness);
  dt_bauhaus_slider_set(bd->contrast_slider, module->blend_params->contrast);
  dt_bauhaus_slider_set(bd->details_slider, module->blend_params->details);

  /* reset all alternative display modes for blendif */
  memset(bd->altmode, 0, sizeof(bd->altmode));

  // force the visibility of output channels if they contain some setting
  bd->output_channels_shown = bd->output_channels_shown
      || _blendif_are_output_channels_used(module->blend_params, bd->csp);

  dt_iop_gui_update_blendif(module);
  dt_iop_gui_update_masks(module);
  dt_iop_gui_update_raster(module);

  /* now show hide controls as required */
  const unsigned int mask_mode = module->blend_params->mask_mode;

  _box_set_visible(bd->top_box, mask_mode & DEVELOP_MASK_ENABLED);

  const dt_image_t img = module->dev->image_storage;
  gtk_widget_set_visible(bd->details_slider, dt_image_is_rawprepare_supported(&img));

  if((mask_mode & DEVELOP_MASK_ENABLED)
     && ((bd->masks_inited && (mask_mode & DEVELOP_MASK_MASK))
         || (bd->blendif_inited && (mask_mode & DEVELOP_MASK_CONDITIONAL))))
  {
    if(bd->blendif_inited && (mask_mode & DEVELOP_MASK_CONDITIONAL))
    {
      gtk_widget_hide(GTK_WIDGET(bd->masks_invert_combo));
      gtk_widget_show(GTK_WIDGET(bd->masks_combine_combo));
    }
    else
    {
      gtk_widget_show(GTK_WIDGET(bd->masks_invert_combo));
      gtk_widget_hide(GTK_WIDGET(bd->masks_combine_combo));
    }

    /*
     * if this iop is operating in raw space, it has only 1 channel per pixel,
     * thus there is no alpha channel where we would normally store mask
     * that would get displayed if following button have been pressed.
     *
     * TODO: revisit if/once there semi-raw iops (e.g temperature) with blending
     */
    if(module->blend_colorspace(module, NULL, NULL) == IOP_CS_RAW)
    {
      module->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->showmask), FALSE);
      // (re)set the header mask indicator too
      if(module->mask_indicator)
          gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->mask_indicator), FALSE);
      gtk_widget_hide(GTK_WIDGET(bd->showmask));
    }
    else
    {
      gtk_widget_show(GTK_WIDGET(bd->showmask));
    }

    _box_set_visible(bd->bottom_box, TRUE);
  }
  else
  {
    module->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->showmask), FALSE);
    // (re)set the header mask indicator too
    if(module->mask_indicator)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->mask_indicator), FALSE);
    module->suppress_mask = 0;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->suppress), FALSE);

    _box_set_visible(bd->bottom_box, FALSE);
  }

  if(bd->masks_inited && (mask_mode & DEVELOP_MASK_MASK))
  {
    _box_set_visible(bd->masks_box, TRUE);
  }
  else if(bd->masks_inited)
  {
    dt_masks_set_edit_mode(module, DT_MASKS_EDIT_OFF);

    _box_set_visible(bd->masks_box, FALSE);
  }
  else
  {
    _box_set_visible(bd->masks_box, FALSE);
  }

  _box_set_visible(bd->raster_box, bd->raster_inited && (mask_mode & DEVELOP_MASK_RASTER));

  if(bd->blendif_inited && (mask_mode & DEVELOP_MASK_CONDITIONAL))
  {
    _box_set_visible(bd->blendif_box, TRUE);
  }
  else if(bd->blendif_inited)
  {
    /* switch off color picker */
    dt_iop_color_picker_reset(module, FALSE);

    _box_set_visible(bd->blendif_box, FALSE);
  }
  else
  {
    _box_set_visible(bd->blendif_box, FALSE);
  }

  if(module->hide_enable_button)
    gtk_widget_hide(GTK_WIDGET(bd->masks_modes_box));
  else
    gtk_widget_show(GTK_WIDGET(bd->masks_modes_box));

  --darktable.gui->reset;
}

void dt_iop_gui_blending_lose_focus(dt_iop_module_t *module)
{
  if(darktable.gui->reset) return;
  if(!module) return;

  const int has_mask_display = module->request_mask_display & (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL);
  const int suppress = module->suppress_mask;

  if((module->flags() & IOP_FLAGS_SUPPORTS_BLENDING) && module->blend_data)
  {
    dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->showmask), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->suppress), FALSE);
    module->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
    module->suppress_mask = 0;

    // (re)set the header mask indicator too
    ++darktable.gui->reset;
    if(module->mask_indicator)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->mask_indicator), FALSE);
    --darktable.gui->reset;

    if(bd->masks_support)
    {
      // unselect all tools
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), FALSE);
      dt_masks_set_edit_mode(module, DT_MASKS_EDIT_OFF);

      for(int k=0; k < DEVELOP_MASKS_NB_SHAPES; k++)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_shapes[k]), FALSE);
    }

    dt_pthread_mutex_lock(&bd->lock);
    bd->save_for_leave = DT_DEV_PIXELPIPE_DISPLAY_NONE;
    if(bd->timeout_handle)
    {
      // purge any remaining timeout handlers
      g_source_remove(bd->timeout_handle);
      bd->timeout_handle = 0;
    }
    dt_pthread_mutex_unlock(&bd->lock);

    // reprocess main center image if needed
    if(has_mask_display || suppress)
      dt_iop_refresh_center(module);
  }
}

void dt_iop_gui_blending_reload_defaults(dt_iop_module_t *module)
{
  if(!module) return;
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!bd || !bd->blendif_support || !bd->blendif_inited) return;
  bd->output_channels_shown = FALSE;
}

void dt_iop_gui_init_blending(GtkWidget *iopw, dt_iop_module_t *module)
{
  /* create and add blend mode if module supports it */
  if(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
  {
    ++darktable.gui->reset;
    --darktable.bauhaus->skip_accel;

    module->blend_data = g_malloc0(sizeof(dt_iop_gui_blend_data_t));
    dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

    bd->iopw = iopw;
    bd->module = module;
    bd->csp = DEVELOP_BLEND_CS_NONE;
    bd->blend_modes_csp = DEVELOP_BLEND_CS_NONE;
    bd->channel_tabs_csp = DEVELOP_BLEND_CS_NONE;
    bd->output_channels_shown = FALSE;
    dt_iop_colorspace_type_t cst = module->blend_colorspace(module, NULL, NULL);
    bd->blendif_support = (cst == IOP_CS_LAB || cst == IOP_CS_RGB);
    bd->masks_support = !(module->flags() & IOP_FLAGS_NO_MASKS);

    bd->masks_modes = NULL;
    bd->masks_modes_toggles = NULL;

    dt_pthread_mutex_init(&bd->lock, NULL);
    dt_pthread_mutex_lock(&bd->lock);
    bd->timeout_handle = 0;
    bd->save_for_leave = 0;
    dt_pthread_mutex_unlock(&bd->lock);

    //toggle buttons creation for masks modes
    GtkWidget *but = NULL;

    // DEVELOP_MASK_DISABLED
    but = dt_iop_togglebutton_new(module, "blend`masks", N_("Off"), NULL, G_CALLBACK(_blendop_masks_modes_none_clicked),
                                  FALSE, 0, 0, dtgtk_cairo_paint_cancel, NULL);
    bd->masks_modes = g_list_append(bd->masks_modes, GUINT_TO_POINTER(DEVELOP_MASK_DISABLED));
    bd->masks_modes_toggles = g_list_append(bd->masks_modes_toggles , GTK_WIDGET(but));

    // DEVELOP_MASK_ENABLED
    but = dt_iop_togglebutton_new(module, "blend`masks", N_("Uniformly"), NULL, G_CALLBACK(_blendop_masks_modes_uni_toggled),
                                  FALSE, 0, 0, dtgtk_cairo_paint_masks_uniform, NULL);
    bd->masks_modes = g_list_append(bd->masks_modes, GUINT_TO_POINTER(DEVELOP_MASK_ENABLED));
    bd->masks_modes_toggles  = g_list_append(bd->masks_modes_toggles , GTK_WIDGET(but));

    if(bd->masks_support) //DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK
    {
      but = dt_iop_togglebutton_new(module, "blend`masks", N_("Drawn mask"), NULL, G_CALLBACK(_blendop_masks_modes_drawn_toggled),
                                    FALSE, 0, 0, dtgtk_cairo_paint_masks_drawn, NULL);
      bd->masks_modes = g_list_append(bd->masks_modes, GUINT_TO_POINTER(DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK));
      bd->masks_modes_toggles = g_list_append(bd->masks_modes_toggles, GTK_WIDGET(but));
    }
    if(bd->blendif_support) //DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL
    {
      but = dt_iop_togglebutton_new(module, "blend`masks", N_("Parametric mask"), NULL, G_CALLBACK(_blendop_masks_modes_param_toggled),
                                    FALSE, 0, 0, dtgtk_cairo_paint_masks_parametric, NULL);
      bd->masks_modes
          = g_list_append(bd->masks_modes, GUINT_TO_POINTER(DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL));
      bd->masks_modes_toggles = g_list_append(bd->masks_modes_toggles, GTK_WIDGET(but));
    }

    if(bd->blendif_support && bd->masks_support) //DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK_CONDITIONAL
    {
      but = dt_iop_togglebutton_new(module, "blend`masks", N_("Drawn & parametric mask"), NULL, G_CALLBACK(_blendop_masks_modes_both_toggled),
                                    FALSE, 0, 0, dtgtk_cairo_paint_masks_drawn_and_parametric, NULL);
      bd->masks_modes
          = g_list_append(bd->masks_modes, GUINT_TO_POINTER(DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK_CONDITIONAL));
      bd->masks_modes_toggles = g_list_append(bd->masks_modes_toggles, GTK_WIDGET(but));
    }

    if(bd->masks_support) //DEVELOP_MASK_ENABLED | DEVELOP_MASK_RASTER
    {
      but = dt_iop_togglebutton_new(module, "blend`masks", N_("Raster mask"), NULL, G_CALLBACK(_blendop_masks_modes_raster_toggled),
                                    FALSE, 0, 0, dtgtk_cairo_paint_masks_raster, NULL);
      bd->masks_modes
          = g_list_append(bd->masks_modes, GUINT_TO_POINTER(DEVELOP_MASK_ENABLED | DEVELOP_MASK_RASTER));
      bd->masks_modes_toggles = g_list_append(bd->masks_modes_toggles, GTK_WIDGET(but));
    }

    GtkWidget *presets_button = dtgtk_button_new(dtgtk_cairo_paint_presets, 0, NULL);
    gtk_widget_set_tooltip_text(presets_button, _("Blending options"));
    if(bd->blendif_support)
    {
      g_signal_connect(G_OBJECT(presets_button), "button-press-event", G_CALLBACK(_blendif_options_callback), module);
    }
    else
    {
      gtk_widget_set_sensitive(GTK_WIDGET(presets_button), FALSE);
    }

    // initial state is no mask
    bd->selected_mask_mode = GTK_WIDGET(
        g_list_nth_data(bd->masks_modes_toggles,
                        g_list_index(bd->masks_modes, (gconstpointer)DEVELOP_MASK_DISABLED)));

    GtkWidget *blend_modes_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    bd->blend_modes_combo = dt_bauhaus_combobox_new(module);
    dt_bauhaus_widget_set_label(bd->blend_modes_combo, N_("Blend"), N_("Blend mode"));
    gtk_widget_set_tooltip_text(bd->blend_modes_combo, _("Choose blending mode"));

    g_signal_connect(G_OBJECT(bd->blend_modes_combo), "value-changed",
                     G_CALLBACK(_blendop_blend_mode_callback), bd);
    dt_gui_add_help_link(GTK_WIDGET(bd->blend_modes_combo), dt_get_help_url("masks_blending_op"));
    gtk_box_pack_start(GTK_BOX(blend_modes_hbox), bd->blend_modes_combo, TRUE, TRUE, 0);

    bd->blend_modes_blend_order = dt_iop_togglebutton_new(module, "blend`tools", N_("Toggle blend order"), NULL,
                                                          G_CALLBACK(_blendop_blend_order_clicked), FALSE,
                                                          0, 0, dtgtk_cairo_paint_invert, blend_modes_hbox);
    gtk_widget_set_tooltip_text(bd->blend_modes_blend_order, _("Toggle the blending order between the input and the output of the module,"
                                                               "\nby default the output will be blended on top of the input,"
                                                               "\norder can be reversed by clicking on the icon (input on top of output)"));

    bd->blend_mode_parameter_slider = dt_bauhaus_slider_new_with_range(module, -18.0f, 18.0f, 0, 0.0f, 3);
    dt_bauhaus_widget_set_field(bd->blend_mode_parameter_slider, &module->blend_params->blend_parameter, DT_INTROSPECTION_TYPE_FLOAT);
    dt_bauhaus_widget_set_label(bd->blend_mode_parameter_slider, N_("Blend"), N_("Blend fulcrum"));
    dt_bauhaus_slider_set_format(bd->blend_mode_parameter_slider, _(" EV"));
    dt_bauhaus_slider_set_soft_range(bd->blend_mode_parameter_slider, -3.0, 3.0);
    gtk_widget_set_tooltip_text(bd->blend_mode_parameter_slider, _("Adjust the fulcrum used by some blending"
                                                                   " operations"));
    gtk_widget_set_visible(bd->blend_mode_parameter_slider, FALSE);

    bd->opacity_slider = dt_bauhaus_slider_new_with_range(module, 0.0, 100.0, 0, 100.0, 0);
    dt_bauhaus_widget_set_field(bd->opacity_slider, &module->blend_params->opacity, DT_INTROSPECTION_TYPE_FLOAT);
    dt_bauhaus_widget_set_label(bd->opacity_slider, N_("Blend"), N_("Opacity"));
    dt_bauhaus_slider_set_format(bd->opacity_slider, "%");
    module->fusion_slider = bd->opacity_slider;
    gtk_widget_set_tooltip_text(bd->opacity_slider, _("Set the opacity of the blending"));

    bd->masks_combine_combo = _combobox_new_from_list(module, _("Combine masks"), dt_develop_combine_masks_names, NULL,
                                                      _("How to combine individual drawn mask and different channels of parametric mask"));
    g_signal_connect(G_OBJECT(bd->masks_combine_combo), "value-changed",
                     G_CALLBACK(_blendop_masks_combine_callback), bd);
    dt_gui_add_help_link(GTK_WIDGET(bd->masks_combine_combo), dt_get_help_url("masks_combined"));

    bd->masks_invert_combo = _combobox_new_from_list(module, _("Invert mask"), dt_develop_invert_mask_names, NULL,
                                                     _("Apply mask in normal or inverted mode"));
    g_signal_connect(G_OBJECT(bd->masks_invert_combo), "value-changed",
                     G_CALLBACK(_blendop_masks_invert_callback), bd);

    bd->details_slider = dt_bauhaus_slider_new_with_range(module, -1.0f, 1.0f, 0, 0.0f, 2);
    dt_bauhaus_widget_set_label(bd->details_slider, N_("Blend"), N_("Details threshold"));
    dt_bauhaus_slider_set_format(bd->details_slider, "%");
    gtk_widget_set_tooltip_text(bd->details_slider, _("Adjust the threshold for the details mask (using raw data), "
                                                      "\npositive values selects areas with strong details, "
                                                      "\nnegative values select flat areas"));
    g_signal_connect(G_OBJECT(bd->details_slider), "value-changed", G_CALLBACK(_blendop_blendif_details_callback), bd);

    bd->masks_feathering_guide_combo = _combobox_new_from_list(module, _("Feathering guide"), dt_develop_feathering_guide_names,
                                                               &module->blend_params->feathering_guide,
                                                               _("Choose to guide mask by input or output image and"
                                                                 "\nchoose to apply feathering before or after mask blur"));

    bd->feathering_radius_slider = dt_bauhaus_slider_new_with_range(module, 0.0, 250.0, 0, 0.0, 1);
    dt_bauhaus_widget_set_field(bd->feathering_radius_slider, &module->blend_params->feathering_radius, DT_INTROSPECTION_TYPE_FLOAT);
    dt_bauhaus_widget_set_label(bd->feathering_radius_slider, N_("Blend"), N_("Feathering radius"));
    dt_bauhaus_slider_set_format(bd->feathering_radius_slider, " px");
    gtk_widget_set_tooltip_text(bd->feathering_radius_slider, _("Spatial radius of feathering"));

    bd->blur_radius_slider = dt_bauhaus_slider_new_with_range(module, 0.0, 100.0, 0, 0.0, 1);
    dt_bauhaus_widget_set_field(bd->blur_radius_slider, &module->blend_params->blur_radius, DT_INTROSPECTION_TYPE_FLOAT);
    dt_bauhaus_widget_set_label(bd->blur_radius_slider, N_("Blend"), N_("Blurring radius"));
    dt_bauhaus_slider_set_format(bd->blur_radius_slider, " px");
    gtk_widget_set_tooltip_text(bd->blur_radius_slider, _("Radius for gaussian blur of blend mask"));

    bd->brightness_slider = dt_bauhaus_slider_new_with_range(module, -1.0, 1.0, 0, 0.0, 2);
    dt_bauhaus_widget_set_field(bd->brightness_slider, &module->blend_params->brightness, DT_INTROSPECTION_TYPE_FLOAT);
    dt_bauhaus_widget_set_label(bd->brightness_slider, N_("Blend"), N_("Mask opacity"));
    dt_bauhaus_slider_set_format(bd->brightness_slider, "%");
    gtk_widget_set_tooltip_text(bd->brightness_slider, _("Shifts and tilts the tone curve of the blend mask to adjust its "
                                                         "brightness without affecting fully transparent/fully opaque "
                                                         "regions"));

    bd->contrast_slider = dt_bauhaus_slider_new_with_range(module, -1.0, 1.0, 0, 0.0, 2);
    dt_bauhaus_widget_set_field(bd->contrast_slider, &module->blend_params->contrast, DT_INTROSPECTION_TYPE_FLOAT);
    dt_bauhaus_widget_set_label(bd->contrast_slider, N_("Blend"), N_("Mask contrast"));
    dt_bauhaus_slider_set_format(bd->contrast_slider, "%");
    gtk_widget_set_tooltip_text(bd->contrast_slider, _("Gives the tone curve of the blend mask an s-like shape to "
                                                       "adjust its contrast"));

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(hbox), dt_ui_label_new(_("Mask refinement")), TRUE, TRUE, 0);
    dt_gui_add_class(hbox, "dt_section_label");

    bd->showmask = dt_iop_togglebutton_new(module, "blend`tools", N_("Display mask and/or color channel"), NULL, G_CALLBACK(_blendop_blendif_showmask_clicked),
                                           FALSE, 0, 0, dtgtk_cairo_paint_showmask, hbox);
    gtk_widget_set_tooltip_text(bd->showmask, _("Display mask and/or color channel. Ctrl+click to display mask, "
                                                "shift+click to display channel. Hover over parametric mask slider to "
                                                "select channel for display"));
    dt_gui_add_class(bd->showmask, "dt_transparent_background");

    bd->suppress = dt_iop_togglebutton_new(module, "blend`tools", N_("Temporarily switch off blend mask"), NULL, G_CALLBACK(_blendop_blendif_suppress_toggled),
                                           FALSE, 0, 0, dtgtk_cairo_paint_eye_toggle, hbox);
    gtk_widget_set_tooltip_text(bd->suppress, _("Temporarily switch off blend mask. Only for module in focus"));
    dt_gui_add_class(bd->suppress, "dt_transparent_background");

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(iopw), GTK_WIDGET(box), TRUE, TRUE, 0);

    //box enclosing the mask mode selection buttons
    bd->masks_modes_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
    //mask selection buttons packing in mask_box
    for(GList *l = bd->masks_modes_toggles; l; l = g_list_next(l))
    {
      gtk_box_pack_start(GTK_BOX(bd->masks_modes_box), GTK_WIDGET(l->data), TRUE, TRUE, 0);
    }
    gtk_box_pack_start(GTK_BOX(bd->masks_modes_box), GTK_WIDGET(presets_button), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(bd->masks_modes_box), FALSE, FALSE, 0);
    dt_gui_add_help_link(GTK_WIDGET(bd->masks_modes_box), dt_get_help_url("masks_blending"));
    gtk_widget_set_name(GTK_WIDGET(bd->masks_modes_box), "blending-tabs");

    bd->top_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
    gtk_box_pack_start(GTK_BOX(bd->top_box), blend_modes_hbox, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->top_box), bd->blend_mode_parameter_slider, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->top_box), bd->opacity_slider, TRUE, TRUE, 0);
    _add_wrapped_box(box, bd->top_box, NULL);

    dt_iop_gui_init_masks(iopw, module);
    dt_iop_gui_init_raster(iopw, module);
    dt_iop_gui_init_blendif(iopw, module);

    bd->bottom_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
    gtk_box_pack_start(GTK_BOX(bd->bottom_box), GTK_WIDGET(bd->masks_combine_combo), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->bottom_box), GTK_WIDGET(bd->masks_invert_combo), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->bottom_box), hbox, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->bottom_box), bd->details_slider, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->bottom_box), bd->masks_feathering_guide_combo, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->bottom_box), bd->feathering_radius_slider, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->bottom_box), bd->blur_radius_slider, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->bottom_box), bd->brightness_slider, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->bottom_box), bd->contrast_slider, TRUE, TRUE, 0);
    _add_wrapped_box(iopw, bd->bottom_box, "masks_refinement");

    gtk_widget_set_name(GTK_WIDGET(bd->top_box), "blending-box");
    gtk_widget_set_name(GTK_WIDGET(bd->masks_box), "blending-box");
    gtk_widget_set_name(GTK_WIDGET(bd->raster_box), "blending-box");
    gtk_widget_set_name(GTK_WIDGET(bd->blendif_box), "blending-box");
    gtk_widget_set_name(GTK_WIDGET(bd->bottom_box), "blending-box");
    gtk_widget_set_name(GTK_WIDGET(iopw), "blending-wrapper");

    bd->blend_inited = 1;

    ++darktable.bauhaus->skip_accel;
    --darktable.gui->reset;
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
