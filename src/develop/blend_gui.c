/*
    This file is part of darktable,
    Copyright (C) 2012-2026 darktable developers.

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
#include "common/gdk_event_utils.h"

#include "develop/blend.h"
#include "develop/blend_gui_internal.h"
#include "bauhaus/bauhaus.h"
#include "common/database.h"
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
#include "dtgtk/expander.h"
#include "dtgtk/togglebutton.h"
#include "dtgtk/gradientslider.h"
#include "dtgtk/icon.h"
#include "gui/draw.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/preferences.h"
#include "libs/lib.h"
#include "gui/presets.h"

#include <assert.h>
#include <gmodule.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define NEUTRAL_GRAY 0.5

// the opacity of a row or control that has no effect right now: disabled,
// solo-suppressed, or in a bypassed group
#define MASK_DIMMED_OPACITY 0.45

const dt_introspection_type_enum_tuple_t dt_develop_blend_mode_names[]
    = { { NC_("blendmode", "normal"),
          DEVELOP_BLEND_NORMAL2 },
        { NC_("blendmode", "average"),
          DEVELOP_BLEND_AVERAGE },
        { NC_("blendmode", "difference"),
          DEVELOP_BLEND_DIFFERENCE2 },

        { NC_("blendmode", "normal bounded"),
          DEVELOP_BLEND_BOUNDED },
        { NC_("blendmode", "lighten"),
          DEVELOP_BLEND_LIGHTEN },
        { NC_("blendmode", "darken"),
          DEVELOP_BLEND_DARKEN },
        { NC_("blendmode", "screen"),
          DEVELOP_BLEND_SCREEN },

        { NC_("blendmode", "multiply"),
          DEVELOP_BLEND_MULTIPLY },
        { NC_("blendmode", "divide"),
          DEVELOP_BLEND_DIVIDE },
        { NC_("blendmode", "addition"),
          DEVELOP_BLEND_ADD },
        { NC_("blendmode", "subtract"),
          DEVELOP_BLEND_SUBTRACT },
        { NC_("blendmode", "geometric mean"),
          DEVELOP_BLEND_GEOMETRIC_MEAN },
        { NC_("blendmode", "harmonic mean"),
          DEVELOP_BLEND_HARMONIC_MEAN },

        { NC_("blendmode", "overlay"),
          DEVELOP_BLEND_OVERLAY },
        { NC_("blendmode", "softlight"),
          DEVELOP_BLEND_SOFTLIGHT },
        { NC_("blendmode", "hardlight"),
          DEVELOP_BLEND_HARDLIGHT },
        { NC_("blendmode", "vividlight"),
          DEVELOP_BLEND_VIVIDLIGHT },
        { NC_("blendmode", "linearlight"),
          DEVELOP_BLEND_LINEARLIGHT },
        { NC_("blendmode", "pinlight"),
          DEVELOP_BLEND_PINLIGHT },

        { NC_("blendmode", "lightness"),
          DEVELOP_BLEND_LIGHTNESS },
        { NC_("blendmode", "chromaticity"),
          DEVELOP_BLEND_CHROMATICITY },

        { NC_("blendmode", "Lab lightness"),
          DEVELOP_BLEND_LAB_LIGHTNESS },
        { NC_("blendmode", "Lab a-channel"),
          DEVELOP_BLEND_LAB_A },
        { NC_("blendmode", "Lab b-channel"),
          DEVELOP_BLEND_LAB_B },
        { NC_("blendmode", "Lab color"),
          DEVELOP_BLEND_LAB_COLOR },

        { NC_("blendmode", "RGB red channel"),
          DEVELOP_BLEND_RGB_R },
        { NC_("blendmode", "RGB green channel"),
          DEVELOP_BLEND_RGB_G },
        { NC_("blendmode", "RGB blue channel"),
          DEVELOP_BLEND_RGB_B },
        { NC_("blendmode", "HSV value"),
          DEVELOP_BLEND_HSV_VALUE },
        { NC_("blendmode", "HSV color"),
          DEVELOP_BLEND_HSV_COLOR },

        { NC_("blendmode", "hue"),
          DEVELOP_BLEND_HUE },
        { NC_("blendmode", "color"),
          DEVELOP_BLEND_COLOR },
        { NC_("blendmode", "coloradjustment"),
          DEVELOP_BLEND_COLORADJUST },

        /** deprecated blend modes: make them available as legacy
         * history stacks might want them */

        { NC_("blendmode", "difference (deprecated)"),
          DEVELOP_BLEND_DIFFERENCE },
        { NC_("blendmode", "subtract inverse (deprecated)"),
          DEVELOP_BLEND_SUBTRACT_INVERSE },
        { NC_("blendmode", "divide inverse (deprecated)"),
          DEVELOP_BLEND_DIVIDE_INVERSE },
        { NC_("blendmode", "Lab L-channel (deprecated)"),
          DEVELOP_BLEND_LAB_L },
        { } };

const dt_introspection_type_enum_tuple_t dt_develop_blend_mode_flag_names[]
    = { { NC_("blendoperation", "normal"), 0 },
        { NC_("blendoperation", "reverse"), DEVELOP_BLEND_REVERSE },
        { } };

const dt_introspection_type_enum_tuple_t dt_develop_blend_colorspace_names[]
    = { { N_("default"),
          DEVELOP_BLEND_CS_NONE },
        { N_("RAW"),
          DEVELOP_BLEND_CS_RAW },
        { N_("Lab"),
          DEVELOP_BLEND_CS_LAB },
        { N_("RGB (display)"),
          DEVELOP_BLEND_CS_RGB_DISPLAY },
        { N_("RGB (scene)"),
          DEVELOP_BLEND_CS_RGB_SCENE },
        { } };

const dt_introspection_type_enum_tuple_t dt_develop_mask_mode_names[] = {
  { N_("off"), DEVELOP_MASK_DISABLED },
  { N_("uniformly"), DEVELOP_MASK_ENABLED },
  { N_("drawn mask"), DEVELOP_MASK_MASK | DEVELOP_MASK_ENABLED },
  { N_("parametric mask"), DEVELOP_MASK_CONDITIONAL | DEVELOP_MASK_ENABLED },
  { N_("raster mask"), DEVELOP_MASK_RASTER | DEVELOP_MASK_ENABLED },
  { N_("drawn & parametric mask"), DEVELOP_MASK_MASK_CONDITIONAL | DEVELOP_MASK_ENABLED },
  { N_("flexi mask"), DEVELOP_MASK_FLEXI | DEVELOP_MASK_ENABLED },
  {}
};

const dt_introspection_type_enum_tuple_t dt_develop_combine_masks_names[]
    = { { N_("exclusive"),            DEVELOP_COMBINE_NORM_EXCL },
        { N_("inclusive"),            DEVELOP_COMBINE_NORM_INCL },
        { N_("exclusive & inverted"), DEVELOP_COMBINE_INV_EXCL },
        { N_("inclusive & inverted"), DEVELOP_COMBINE_INV_INCL },
        { } };

const dt_introspection_type_enum_tuple_t dt_develop_feathering_guide_names[]
    = { { N_("output before blur"), DEVELOP_MASK_GUIDE_OUT_BEFORE_BLUR },
        { N_("input before blur"),  DEVELOP_MASK_GUIDE_IN_BEFORE_BLUR },
        { N_("output after blur"),  DEVELOP_MASK_GUIDE_OUT_AFTER_BLUR },
        { N_("input after blur"),   DEVELOP_MASK_GUIDE_IN_AFTER_BLUR },
        { } };

const dt_introspection_type_enum_tuple_t dt_develop_invert_mask_names[]
    = { { N_("off"), DEVELOP_COMBINE_NORM },
        { N_("on"), DEVELOP_COMBINE_INV },
        { } };

const dt_iop_gui_blendif_colorstop_t _gradient_L[]
    = { { 0.0f,   { 0, 0, 0, 1.0 } },
        { 0.125f, { NEUTRAL_GRAY / 8, NEUTRAL_GRAY / 8, NEUTRAL_GRAY / 8, 1.0 } },
        { 0.25f,  { NEUTRAL_GRAY / 4, NEUTRAL_GRAY / 4, NEUTRAL_GRAY / 4, 1.0 } },
        { 0.5f,   { NEUTRAL_GRAY / 2, NEUTRAL_GRAY / 2, NEUTRAL_GRAY / 2, 1.0 } },
        { 1.0f,   { NEUTRAL_GRAY, NEUTRAL_GRAY, NEUTRAL_GRAY, 1.0 } } };

// The values for "a" are generated in the following way:
//   Lab (with L=[90 to 68], b=0, and a=[-56 to 56]
//    -> sRGB (D65 linear) -> normalize with MAX(R,G,B) = 0.75
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
//   Lab (with L=[58 to 62], a=0, and b=[-65 to 65]
//    -> sRGB (D65 linear) -> normalize with MAX(R,G,B) = 0.75
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
//   JzCzhz (with Jz=0.011 and Cz=0.01) -> sRGB(D65 linear)
//     -> normalize with MAX(R,G,B) = 0.75
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

static gboolean _module_has_drawn_shapes(const dt_iop_module_t *module);
static void _blendop_mask_enable(dt_iop_module_t *module);
static void _queue_masks_list_rebuild(dt_iop_module_t *module);
static void _queue_link_peers_rebuild(const dt_iop_module_t *module);
static void _auto_expand_selected_row(dt_iop_module_t *module, const dt_mask_id_t id);

static gboolean _blendif_blend_parameter_enabled(dt_develop_blend_colorspace_t csp,
                                                 const dt_develop_blend_mode_t mode)
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

// a channel's boost factor, from a parametric form's own boost factors
// (p->blendif_boost_factors) and its colorspace's channel table
static inline float _boost_factor(const float *blendif_boost_factors,
                                  const dt_iop_gui_blendif_channel_t *channels,
                                  const int channel,
                                  const int in_out)
{
  return exp2f(blendif_boost_factors[channels[channel].param_channels[in_out]]);
}

// normalize a raw picked pixel into each channel's [0,1] display range,
// boost-factor corrected (see _boost_factor)
static void _blendif_scale(const float *blendif_boost_factors,
                           const dt_iop_gui_blendif_channel_t *channels,
                           dt_iop_colorspace_type_t cst,
                           const float *in,
                           float *out,
                           const dt_iop_order_iccprofile_info_t *work_profile,
                           const int in_out)
{
  out[0] = out[1] = out[2] = out[3] = out[4] = out[5] = out[6] = out[7] = -1.0f;

#define BOOST(idx) _boost_factor(blendif_boost_factors, channels, idx, in_out)

  switch(cst)
  {
    case IOP_CS_LAB:
      out[CHANNEL_INDEX_L] = (in[0] / BOOST(0)) / 100.0f;
      out[CHANNEL_INDEX_a] = ((in[1] / BOOST(1)) + 128.0f) / 256.0f;
      out[CHANNEL_INDEX_b] = ((in[2] / BOOST(2)) + 128.0f) / 256.0f;
      break;
    case IOP_CS_RGB:
      if(work_profile == NULL)
        out[CHANNEL_INDEX_g] = 0.3f * in[0] + 0.59f * in[1] + 0.11f * in[2];
      else
        out[CHANNEL_INDEX_g] = dt_ioppr_get_rgb_matrix_luminance
          (in, work_profile->matrix_in,
           work_profile->lut_in,
           work_profile->unbounded_coeffs_in,
           work_profile->lutsize,
           work_profile->nonlinearlut);
      out[CHANNEL_INDEX_g] = out[CHANNEL_INDEX_g] / BOOST(0);
      out[CHANNEL_INDEX_R] = in[0] / BOOST(1);
      out[CHANNEL_INDEX_G] = in[1] / BOOST(2);
      out[CHANNEL_INDEX_B] = in[2] / BOOST(3);
      break;
    case IOP_CS_LCH:
      out[CHANNEL_INDEX_C] = (in[1] / BOOST(3)) / (128.0f * M_SQRT2_F);
      out[CHANNEL_INDEX_h] = in[2] / BOOST(4);
      break;
    case IOP_CS_HSL:
      out[CHANNEL_INDEX_H] = in[0] / BOOST(4);
      out[CHANNEL_INDEX_S] = in[1] / BOOST(5);
      out[CHANNEL_INDEX_l] = in[2] / BOOST(6);
      break;
    case IOP_CS_JZCZHZ:
      out[CHANNEL_INDEX_Jz] = in[0] / BOOST(4);
      out[CHANNEL_INDEX_Cz] = in[1] / BOOST(5);
      out[CHANNEL_INDEX_hz] = in[2] / BOOST(6);
      break;
    default:
      break;
  }
#undef BOOST
}

static inline int _blendif_print_digits_default(const float value)
{
  int digits;
  if(value < 0.0001f) digits = 0;
  else if(value < 0.01f) digits = 2;
  else if(value < 0.999f) digits = 1;
  else digits = 0;

  return digits;
}

static inline int _blendif_print_digits_ab(const float value)
{
  int digits;
  if(fabsf(value) < 10.0f) digits = 1;
  else digits = 0;

  return digits;
}

static void _blendif_scale_print_ab(const float value,
                                    const float boost_factor,
                                    char *string,
                                    int n)
{
  const float scaled = (value * 256.0f - 128.0f) * boost_factor;
  snprintf(string, n, "%-5.*f", _blendif_print_digits_ab(scaled), scaled);
}

static void _blendif_scale_print_hue(const float value,
                                     const float boost_factor,
                                     char *string,
                                     const int n)
{
  snprintf(string, n, "%-5.0f", value * 360.0f);
}

static void _blendif_scale_print_default(const float value,
                                         const float boost_factor,
                                         char *string,
                                         const int n)
{
  const float scaled = value * boost_factor;
  snprintf(string, n, "%-5.*f", _blendif_print_digits_default(scaled), scaled * 100.0f);
}

static void _add_wrapped_box(GtkWidget *container,
                             GtkBox *box,
                             gchar *help_url)
{
  GtkWidget *event_box = gtk_event_box_new();
  GtkWidget *revealer = gtk_revealer_new();
  gtk_container_add(GTK_CONTAINER(revealer), GTK_WIDGET(box));
  gtk_container_add(GTK_CONTAINER(event_box), revealer);
  gtk_container_add(GTK_CONTAINER(container), event_box);
  // event box is needed so that one can click into the area to get help
  dt_gui_add_help_link(event_box, help_url);
  gtk_widget_set_name(GTK_WIDGET(box), "blending-box");
}

static void _box_set_visible(GtkBox *box, gboolean visible)
{
  if(!box) return;

  GtkRevealer *revealer = GTK_REVEALER(gtk_widget_get_parent(GTK_WIDGET(box)));
  gtk_revealer_set_transition_duration(revealer,
                                       dt_conf_get_int("darkroom/ui/transition_duration"));
  gtk_revealer_set_reveal_child(revealer, visible);
}

// a move can flip a container between constant size and height-for-width:
// exposure's body without the panel is all fixed-size sliders, with it it
// holds wrapping labels. gtk3 caches that mode per widget and a parent reads
// its children's cached modes *before* re-measuring them, so on the way back
// every ancestor kept "constant size", measured the panel at its minimum
// width (much taller) and never recovered: blank gaps in the module. Measuring
// the chain bottom-up refreshes each mode before the next parent reads it
static void _refresh_request_modes(GtkWidget *from)
{
  for(GtkWidget *a = from; a; a = gtk_widget_get_parent(a))
  {
    gtk_widget_queue_resize(a);
    int unused = 0; // gtk refuses the query with both outputs NULL
    gtk_widget_get_preferred_width(a, &unused, NULL);
  }
}

// re-home a widget into a new parent (no-op if already there), preserving its
// shown state. Used by the panel host to move the whole flexi panel between
// its possible homes (see masks_gui_panel_host.c)
void dt_masks_gui_reparent_into(GtkWidget *w,
                                GtkWidget *parent,
                                const gboolean at_end,
                                const gboolean expand)
{
  if(!w || !parent || !GTK_IS_WIDGET(w) || !GTK_IS_BOX(parent)) return;
  GtkWidget *cur = gtk_widget_get_parent(w);
  if(cur == parent) return;

  const gboolean was_visible = gtk_widget_get_visible(w);

  g_object_ref(w);
  if(cur) gtk_container_remove(GTK_CONTAINER(cur), w);

  if(at_end)
    gtk_box_pack_end(GTK_BOX(parent), w, expand, expand, 0);
  else
  {
    if(expand) gtk_widget_set_hexpand(w, TRUE);
    dt_gui_box_add(parent, w);
  }

  if(was_visible) gtk_widget_show(w);
  g_object_unref(w);

  _refresh_request_modes(parent);
  _refresh_request_modes(cur);
}

// a fixed spacer that only separates two runs of buttons, without competing
// for the row's slack: one icon wide, from .dt_masks_button_gap in darktable.css
static GtkWidget *_pack_gap(GtkWidget *box)
{
  GtkWidget *gap = dt_gui_hbox();
  dt_gui_add_class(gap, "dt_masks_button_gap");
  gtk_widget_show(gap);
  dt_gui_box_add(box, gap);
  return gap;
}

// dt_history_item_get_name gives markup, which a plain-text label, menu or
// tooltip would show escaped ("&amp;")
static gchar *_module_plain_name(const dt_iop_module_t *m)
{
  gchar *markup = dt_history_item_get_name(m);
  gchar *text = NULL;
  if(!pango_parse_markup(markup, -1, 0, NULL, &text, NULL, NULL)) text = g_strdup(markup);
  g_free(markup);
  return text;
}

// defined much further down (grouping shape rows / naming clusters); forward
// declared here so the import menu can group its shapes by kind the same way
// the mask list clusters same-kind elements
static guint _form_kind(const dt_masks_form_t *form);
static const char *_kind_name(const guint kind, const gboolean plural);
// defined further down, with the rest of the raster element, group naming and
// refinement code
static void _add_raster_mask(dt_iop_module_t *self,
                             dt_iop_module_t *src,
                             const dt_mask_id_t id);
static const char *_flexi_op_short_name(const dt_masks_state_t flexi_op);
static void _refresh_mask_display(const dt_iop_module_t *module);
static void _flexi_refine_follow_selection(dt_iop_gui_blend_data_t *bd);
static dt_mask_id_t _mask_group_cid(dt_iop_module_t *module);
static void _select_mask_group_if_none(dt_iop_gui_blend_data_t *bd);
static void _sync_group_notes(dt_iop_gui_blend_data_t *bd);

// ---- linking and copying elements between modules' masks -----------------
// A shape or AI object can sit in several modules' masks at once: each mask's
// point refers to the same form, so editing the form changes it in all of
// them, while the point's own opacity, invert state and refinement stay per
// module. That is a link. A copy is a new form, independent from the
// start. Parametric channels are only ever copied: the same thresholds select
// something else in another module's pixels

// a shape or AI object: what can be linked. A parametric or raster element
// is not, and neither is a clone/heal source nor a whole group
static gboolean _form_is_shape(const dt_masks_form_t *f)
{
  return f
         && !(f->type & (DT_MASKS_GROUP | DT_MASKS_CLONE | DT_MASKS_NON_CLONE
                         | DT_MASKS_PARAMETRIC | DT_MASKS_RASTER))
         && _form_kind(f);
}

GList *dt_masks_model_form_users(const dt_mask_id_t fid)
{
  GList *users = NULL;
  for(GList *l = darktable.develop ? darktable.develop->iop : NULL; l; l = g_list_next(l))
  {
    dt_iop_module_t *m = l->data;
    dt_masks_form_t *grp = dt_masks_gui_module_mask_group(m);
    if(grp && dt_masks_gui_group_point(grp, fid)) users = g_list_append(users, m);
  }
  return users;
}

// append the shapes of group `grp` and of the groups nested in it, bottom-up
static void _append_shapes(const dt_masks_form_t *grp, GList **out, const int depth)
{
  if(!grp || depth > DT_MASKS_NESTING_MAX) return;
  for(const GList *l = grp->points; l; l = g_list_next(l))
  {
    const dt_masks_point_group_t *pt = l->data;
    if(dt_masks_point_is_marker(pt)) continue;
    const dt_masks_form_t *f = dt_masks_get_from_id(darktable.develop, pt->formid);
    if(f && f != grp && (f->type & DT_MASKS_GROUP))
      _append_shapes(f, out, depth + 1);
    else if(_form_is_shape(f))
      *out = g_list_append(*out, GINT_TO_POINTER(pt->formid));
  }
}

GList *dt_masks_model_module_shapes(dt_iop_module_t *src)
{
  GList *out = NULL;
  _append_shapes(dt_masks_gui_module_mask_group(src), &out, 0);
  return out;
}

GList *dt_masks_model_import_forms(dt_iop_module_t *module,
                                   dt_iop_module_t *src,
                                   GList *fids,
                                   const gboolean copy)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  dt_masks_form_t *src_grp = src ? dt_masks_gui_module_mask_group(src) : NULL;
  GList *added = NULL;
  for(GList *l = fids; l; l = g_list_next(l))
  {
    const dt_mask_id_t fid = GPOINTER_TO_INT(l->data);
    dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
    if(grp && dt_masks_gui_group_point(grp, fid)) continue;

    const dt_mask_id_t id = copy ? dt_masks_form_copy(darktable.develop, fid) : fid;
    dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, id);
    if(!form) continue;
    dt_masks_point_group_t *pt = dt_masks_group_insert_point(darktable.develop, module, form);
    if(!pt) continue;

    // it looks the way it does in the mask it comes from; the operator is the
    // target group's, set by the insertion above
    const dt_masks_point_group_t *spt = src_grp ? dt_masks_gui_group_point(src_grp, fid) : NULL;
    if(spt)
    {
      pt->opacity = spt->opacity;
      pt->state = (pt->state & ~DT_MASKS_STATE_INVERSE) | (spt->state & DT_MASKS_STATE_INVERSE);
      pt->refinement = spt->refinement;
    }

    // the next one lands above this one, in the same group
    if(bd && bd->insert_active) bd->insert_after_fid = id;
    added = g_list_append(added, GINT_TO_POINTER(id));
  }
  return added;
}

// move a hash table entry keyed by form id over to another id
static void _remap_formid_key(GHashTable *table, const dt_mask_id_t from, const dt_mask_id_t to)
{
  gpointer value = NULL;
  if(table && g_hash_table_lookup_extended(table, GINT_TO_POINTER(from), NULL, &value))
  {
    g_hash_table_steal(table, GINT_TO_POINTER(from));
    g_hash_table_insert(table, GINT_TO_POINTER(to), value);
  }
}

// defined further down, next to the row index it walks
static int _model_form_uses_in_mask(dt_iop_module_t *module, const dt_mask_id_t fid);

dt_mask_id_t dt_masks_model_unlink_form_point(dt_iop_module_t *module,
                                              const dt_mask_id_t fid,
                                              dt_masks_point_group_t *pt)
{
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  if(!pt) pt = grp ? dt_masks_gui_group_point(grp, fid) : NULL;
  if(!pt) return INVALID_MASKID;
  const dt_mask_id_t nid = dt_masks_form_copy(darktable.develop, fid);
  if(!dt_is_valid_maskid(nid)) return INVALID_MASKID;
  pt->formid = nid;

  // the panel knows elements by id: carry each reference over, or the copy
  // loses its selection and its expanded state. Groups are known by their
  // markers, which an element's unlinking leaves alone.
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  // where the mask still holds another reference to the original, that state
  // describes a row that is still there: moving it to the copy would take the
  // selection and the expanded state away from the row that kept the shape
  if(!bd || _model_form_uses_in_mask(module, fid) > 0) return nid;
  dt_mask_id_t *refs[] = {
    &bd->panel_selected_formid,    &bd->solo_formid,
    &bd->soloedit_formid,          &bd->masks_last_expanded_elem,
    &bd->masks_refine_scope_formid, &bd->insert_after_fid,
  };
  for(size_t k = 0; k < G_N_ELEMENTS(refs); k++)
    if(*refs[k] == fid) *refs[k] = nid;
  _remap_formid_key(bd->masks_props_expanded, fid, nid);
  return nid;
}

// what a pick in the import menu does. Its target carries `a` and `b`, as
// noted per op; a source module travels as an index into the menu's module
// table (see _masks_import_module_index), since a GVariant cannot carry it
typedef enum _masks_import_op_t
{
  _IMPORT_LINK_ONE = 0,    // a: source module (-1: none), b: the form
  _IMPORT_COPY_ONE,
  _IMPORT_LINK_GROUP,      // a: source module, b: its group's head (INVALID_MASKID: all)
  _IMPORT_COPY_GROUP,
  _IMPORT_COPY_PARAMETRIC, // a: source module, b: the form
  _IMPORT_ADD_MASK,        // a: raster source (see _raster_sources_collect)
  _IMPORT_USE_MASK,
} _masks_import_op_t;

typedef struct _masks_raster_source_entry_t
{
  dt_iop_module_t *src;
  dt_mask_id_t id;
  char *name;
} _masks_raster_source_entry_t;

static void _raster_source_entry_free(gpointer data)
{
  _masks_raster_source_entry_t *entry = data;
  if(entry)
  {
    g_free(entry->name);
    g_free(entry);
  }
}

// every mask another module offers as a raster source, in pipe order: those
// upstream of `module` into `usable`, those downstream, which are processed
// after it and so never available to it, into `later`
static void _raster_sources_collect(dt_iop_module_t *module, GPtrArray *usable, GPtrArray *later)
{
  gboolean past = FALSE;
  for(GList *iter = darktable.develop->iop; iter; iter = g_list_next(iter))
  {
    dt_iop_module_t *iop = iter->data;
    if(iop == module)
    {
      past = TRUE;
      continue;
    }
    if(!iop->raster_mask.source.masks) continue;

    GHashTableIter masks_iter;
    gpointer key, value;
    g_hash_table_iter_init(&masks_iter, iop->raster_mask.source.masks);
    while(g_hash_table_iter_next(&masks_iter, &key, &value))
    {
      // the name the source advertises the mask under (module display name,
      // or the mask name/path for an external source, see
      // dt_iop_advertise_rastermask). It is markup, and the menu and the
      // replace dialog show plain text, so an "&" would read "&amp;"
      const char *name = value ? (const char *)value : iop->name();
      _masks_raster_source_entry_t *entry = g_new0(_masks_raster_source_entry_t, 1);
      entry->src = iop;
      entry->id = GPOINTER_TO_INT(key);
      if(!pango_parse_markup(name, -1, 0, NULL, &entry->name, NULL, NULL))
        entry->name = g_strdup(name);
      g_ptr_array_add(past ? later : usable, entry);
    }
  }
}

// commit shapes or parametric channels brought in from another module
static void _masks_import_forms(dt_iop_module_t *module,
                                dt_iop_module_t *src,
                                GList *fids,
                                const gboolean copy)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  GList *added = dt_masks_model_import_forms(module, src, fids, copy);
  if(!added) return;
  const dt_mask_id_t last = GPOINTER_TO_INT(g_list_last(added)->data);
  g_list_free(added);
  dt_print(DT_DEBUG_MASKS, "[masks] %s %d element(s) into '%s'", copy ? "copied" : "linked",
           g_list_length(fids), module->op);

  bd->panel_selected_formid = last;
  if(darktable.develop->form_gui) darktable.develop->form_gui->panel_selected_formid = last;
  dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
  _queue_link_peers_rebuild(module);
  dt_masks_iop_update(module);
  dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
}

static gboolean _mask_has_elements(const dt_masks_form_t *grp);

// "use the mask of": this module's mask becomes a single raster element
// reading the source's mask. Everything else in it goes, after asking
static void _masks_use_mask_of(dt_iop_module_t *module,
                               const _masks_raster_source_entry_t *entry)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(_mask_has_elements(dt_masks_gui_module_mask_group(module)))
  {
    if(!dt_gui_show_yes_no_dialog(
         _("replace the mask?"), "",
         _("this removes every element from this module's mask and uses the mask"
           " of %s in their place"),
         entry->name))
      return;
    dt_masks_gui_reset_mask_core(module);
  }
  // the reset left no group to aim at: the raster element starts the mask
  bd->insert_active = FALSE;
  _add_raster_mask(module, entry->src, entry->id);
  _flexi_refine_follow_selection(bd);
  dt_masks_gui_refresh_canvas_edit(module);
}

static void _masks_import_pick_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  GtkWidget *btn = GTK_WIDGET(user_data);
  dt_iop_module_t *module = g_object_get_data(G_OBJECT(btn), "module");
  GPtrArray *mods = g_object_get_data(G_OBJECT(btn), "import_modules");
  GPtrArray *rasters = g_object_get_data(G_OBJECT(btn), "import_rasters");
  int op = 0, a = -1, b = INVALID_MASKID;
  g_variant_get(parameter, "(iii)", &op, &a, &b);
  if(darktable.gui->active_popover_menu)
    gtk_popover_popdown(GTK_POPOVER(darktable.gui->active_popover_menu));
  if(!module || !module->blend_data) return;

  if(op == _IMPORT_ADD_MASK || op == _IMPORT_USE_MASK)
  {
    if(!rasters || a < 0 || a >= (int)rasters->len) return;
    const _masks_raster_source_entry_t *entry = g_ptr_array_index(rasters, a);
    if(op == _IMPORT_USE_MASK)
      _masks_use_mask_of(module, entry);
    else
      _add_raster_mask(module, entry->src, entry->id);
    return;
  }

  dt_iop_module_t *src = (mods && a >= 0 && a < (int)mods->len) ? g_ptr_array_index(mods, a) : NULL;
  const gboolean whole = op == _IMPORT_LINK_GROUP || op == _IMPORT_COPY_GROUP;
  if(whole && !src) return;
  GList *fids = whole ? dt_masks_model_module_shapes(src) : g_list_prepend(NULL, GINT_TO_POINTER(b));
  const gboolean copy =
    op == _IMPORT_COPY_ONE || op == _IMPORT_COPY_GROUP || op == _IMPORT_COPY_PARAMETRIC;
  _masks_import_forms(module, src, fids, copy);
  g_list_free(fids);
}

// a popover menu's entry reads its label with mnemonics (gtkmodelbutton.c:406
// in GTK 3.24), so a single "_" in a shape's or module's name would vanish and
// underline the next character instead. Caller frees
static gchar *_menu_label_escape(const char *name)
{
  gchar **parts = g_strsplit(name ? name : "", "_", -1);
  gchar *label = g_strjoinv("__", parts);
  g_strfreev(parts);
  return label;
}

static void _masks_import_append(GMenu *menu,
                                 const char *label,
                                 const _masks_import_op_t op,
                                 const int a,
                                 const int b)
{
  gchar *escaped = _menu_label_escape(label);
  GMenuItem *it = g_menu_item_new(escaped, NULL);
  g_free(escaped);
  g_menu_item_set_action_and_target_value(it, "masks_import.pick",
                                          g_variant_new("(iii)", (int)op, a, b));
  g_menu_append_item(menu, it);
  g_object_unref(it);
}

#define _IMPORT_MAX_KIND_BUCKETS 16

// find (or create, appending to menu) the submenu for a given shape kind
static GMenu *_masks_import_kind_bucket(
  GMenu *menu, guint *kinds, GMenu **submenus, int *n_buckets, const guint kind)
{
  for(int k = 0; k < *n_buckets; k++)
    if(kinds[k] == kind) return submenus[k];
  if(*n_buckets >= _IMPORT_MAX_KIND_BUCKETS) return NULL;

  GMenu *sub = g_menu_new();
  kinds[*n_buckets] = kind;
  submenus[*n_buckets] = sub;
  (*n_buckets)++;

  g_menu_append_submenu(menu, _kind_name(kind, TRUE), G_MENU_MODEL(sub));
  return sub;
}

// index of `m` in the menu's module table, adding it if new
static int _masks_import_module_index(GPtrArray *mods, dt_iop_module_t *m)
{
  if(!m) return -1;
  for(guint k = 0; k < mods->len; k++)
    if(g_ptr_array_index(mods, k) == m) return (int)k;
  g_ptr_array_add(mods, m);
  return (int)mods->len - 1;
}

// a path that is one of an AI object's members: the object is the element,
// importing one of its paths on its own would split it apart
static gboolean _masks_import_is_object_member(const dt_mask_id_t formid)
{
  for(GList *l = darktable.develop->forms; l; l = g_list_next(l))
  {
    const dt_masks_form_t *f = l->data;
    if(!(f->type & DT_MASKS_OBJECT)) continue;
    for(GList *p = f->points; p; p = g_list_next(p))
      if(((dt_masks_point_group_t *)p->data)->formid == formid) return TRUE;
  }
  return FALSE;
}

// shapes of `src`'s mask (see dt_masks_model_module_shapes) the mask `grp` does not
// use yet
static GList *_masks_import_candidates(dt_iop_module_t *src, dt_masks_form_t *grp)
{
  GList *fids = dt_masks_model_module_shapes(src);
  for(GList *l = fids; l;)
  {
    GList *next = g_list_next(l);
    if(grp && dt_masks_gui_group_point(grp, GPOINTER_TO_INT(l->data))) fids = g_list_delete_link(fids, l);
    l = next;
  }
  return fids;
}


// "link shapes" or "copy shapes": every shape and AI object that another
// module's mask uses, or that no mask does, and that this module's mask does
// not use yet, reachable by the module it comes from and by its kind. Added
// to `menu` as one section. Returns how many there are
static int _masks_import_fill_shapes(GMenu *menu,
                                     dt_iop_module_t *module,
                                     GPtrArray *mods,
                                     const gboolean copy)
{
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  const _masks_import_op_t one = copy ? _IMPORT_COPY_ONE : _IMPORT_LINK_ONE;
  const _masks_import_op_t group = copy ? _IMPORT_COPY_GROUP : _IMPORT_LINK_GROUP;
  GMenu *by_module = g_menu_new();
  GMenu *by_type = g_menu_new();
  guint kinds[_IMPORT_MAX_KIND_BUCKETS];
  GMenu *kind_submenus[_IMPORT_MAX_KIND_BUCKETS];
  int n_kinds = 0;
  int n = 0;

  for(GList *l = darktable.develop->iop; l; l = g_list_next(l))
  {
    dt_iop_module_t *src = l->data;
    if(src == module) continue;
    GList *shapes = _masks_import_candidates(src, grp);
    if(!shapes) continue;
    const int a = _masks_import_module_index(mods, src);
    GMenu *sub = g_menu_new();

    GMenu *whole = g_menu_new();
    _masks_import_append(whole, _("all shapes"), group, a, INVALID_MASKID);
    g_menu_append_section(sub, NULL, G_MENU_MODEL(whole));
    g_object_unref(whole);

    GMenu *each = g_menu_new();
    for(GList *s = shapes; s; s = g_list_next(s))
    {
      const dt_mask_id_t fid = GPOINTER_TO_INT(s->data);
      const dt_masks_form_t *f = dt_masks_get_from_id(darktable.develop, fid);
      _masks_import_append(each, f->name, one, a, fid);
    }
    g_menu_append_section(sub, NULL, G_MENU_MODEL(each));
    g_object_unref(each);

    // a menu label is plain text, and an "&" would read "&amp;"
    gchar *plain = _module_plain_name(src);
    gchar *mlabel = _menu_label_escape(plain);
    g_menu_append_submenu(by_module, mlabel, G_MENU_MODEL(sub));
    g_free(mlabel);
    g_free(plain);
    g_object_unref(sub);
    n += g_list_length(shapes);
    g_list_free(shapes);
  }

  // every shape once, by kind, plus the ones no module uses at all
  GMenu *unused = g_menu_new();
  int n_unused = 0;
  for(GList *l = darktable.develop->forms; l; l = g_list_next(l))
  {
    const dt_masks_form_t *f = l->data;
    if(!_form_is_shape(f) || _masks_import_is_object_member(f->formid)) continue;
    if(grp && dt_masks_gui_group_point(grp, f->formid)) continue;
    GList *users = dt_masks_model_form_users(f->formid);
    dt_iop_module_t *owner = users ? users->data : NULL;
    g_list_free(users);
    if(!owner)
    {
      _masks_import_append(unused, f->name, one, -1, f->formid);
      n_unused++;
      n++;
    }
    GMenu *bucket =
      _masks_import_kind_bucket(by_type, kinds, kind_submenus, &n_kinds, _form_kind(f));
    if(bucket)
      _masks_import_append(bucket, f->name, one, _masks_import_module_index(mods, owner),
                           f->formid);
  }
  if(n_unused)
    g_menu_append_submenu(by_module, _("not currently used"), G_MENU_MODEL(unused));
  g_object_unref(unused);

  if(n)
  {
    GMenu *sec = g_menu_new();
    g_menu_append_submenu(sec, _("by source module"), G_MENU_MODEL(by_module));
    g_menu_append_submenu(sec, _("by type"), G_MENU_MODEL(by_type));
    g_menu_append_section(menu, NULL, G_MENU_MODEL(sec));
    g_object_unref(sec);
  }
  g_object_unref(by_module);
  g_object_unref(by_type);
  for(int k = 0; k < n_kinds; k++) g_object_unref(kind_submenus[k]);
  return n;
}

// "copy parametric channel": every other module's parametric channels. One
// set up in another blend colorspace is listed but cannot be picked: its
// stored channel would be read through this module's channel table (see
// _parametric_get_mask_roi in masks/parametric.c). Returns how many there are
static int _masks_import_fill_parametric(GMenu *menu, dt_iop_module_t *module, GPtrArray *mods)
{
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  const uint32_t csp = (uint32_t)module->blend_params->blend_cst;
  int n = 0;
  for(GList *l = darktable.develop->iop; l; l = g_list_next(l))
  {
    dt_iop_module_t *src = l->data;
    dt_masks_form_t *sgrp = src == module ? NULL : dt_masks_gui_module_mask_group(src);
    if(!sgrp) continue;
    GMenu *ok = g_menu_new();
    GMenu *other = g_menu_new();
    int n_ok = 0, n_other = 0;
    for(GList *p = sgrp->points; p; p = g_list_next(p))
    {
      const dt_mask_id_t fid = ((dt_masks_point_group_t *)p->data)->formid;
      const dt_masks_form_t *f = dt_masks_get_from_id(darktable.develop, fid);
      if(!f || !(f->type & DT_MASKS_PARAMETRIC) || !f->points) continue;
      if(grp && dt_masks_gui_group_point(grp, fid)) continue;
      const dt_masks_point_parametric_t *pp = f->points->data;
      if(pp->colorspace == csp)
      {
        _masks_import_append(ok, f->name, _IMPORT_COPY_PARAMETRIC,
                             _masks_import_module_index(mods, src), fid);
        n_ok++;
      }
      else
      {
        g_menu_append(other, f->name, "masks_import.unavailable");
        n_other++;
      }
    }
    if(n_ok || n_other)
    {
      GMenu *sub = g_menu_new();
      if(n_ok) g_menu_append_section(sub, NULL, G_MENU_MODEL(ok));
      if(n_other)
        g_menu_append_section(sub, _("other blend colorspace"), G_MENU_MODEL(other));
      // a menu label is plain text, and an "&" would read "&amp;"
      gchar *plain = _module_plain_name(src);
      gchar *mlabel = _menu_label_escape(plain);
      g_menu_append_submenu(menu, mlabel, G_MENU_MODEL(sub));
      g_free(mlabel);
      g_free(plain);
      g_object_unref(sub);
      n += n_ok + n_other;
    }
    g_object_unref(ok);
    g_object_unref(other);
  }
  return n;
}

// "add the mask of" / "use the mask of": the raster sources from
// _raster_sources_collect, the downstream ones listed but not pickable.
// Returns how many there are
static int _masks_import_fill_raster(GMenu *menu,
                                     GPtrArray *usable,
                                     GPtrArray *later,
                                     const _masks_import_op_t op)
{
  if(usable->len)
  {
    GMenu *ok = g_menu_new();
    for(guint k = 0; k < usable->len; k++)
    {
      const _masks_raster_source_entry_t *entry = g_ptr_array_index(usable, k);
      _masks_import_append(ok, entry->name, op, (int)k, 0);
    }
    g_menu_append_section(menu, NULL, G_MENU_MODEL(ok));
    g_object_unref(ok);
  }
  if(later->len)
  {
    GMenu *na = g_menu_new();
    for(guint k = 0; k < later->len; k++)
    {
      const _masks_raster_source_entry_t *entry = g_ptr_array_index(later, k);
      g_menu_append(na, entry->name, "masks_import.unavailable");
    }
    g_menu_append_section(menu, _("processed later in the pipe"), G_MENU_MODEL(na));
    g_object_unref(na);
  }
  return (int)(usable->len + later->len);
}

// removing a shape from a module's group only detaches it
// (dt_masks_form_remove in masks.c): it stays in dev->forms, unused, until
// dt_masks_cleanup_unused purges it. Offered here, where those shapes show,
// under "not currently used"
static void _masks_import_cleanup_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  dt_masks_cleanup_unused(darktable.develop);
  dt_control_log(_("unused shapes removed"));
  dt_masks_gui_build_list(module);
  if(darktable.gui->active_popover_menu)
    gtk_popover_popdown(GTK_POPOVER(darktable.gui->active_popover_menu));
}

// the reach of the cleanup above ends at the history stack: a shape only an
// earlier, since replaced step still uses has to stay for that step. Removing
// those too means dropping the steps, so this compresses history first, after
// saying so -- it takes undo and redo history with it.
static void _masks_import_compress_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  if(darktable.gui->active_popover_menu)
    gtk_popover_popdown(GTK_POPOVER(darktable.gui->active_popover_menu));
  if(!dt_gui_show_yes_no_dialog(
       _("compress history and clean up unused shapes?"), "",
       _("this compresses the history stack, dropping every earlier step and"
         " anything you could redo, then deletes every shape no module uses.\n\n"
         "shapes still used by earlier steps can only be removed this way.")))
    return;
  dt_dev_history_truncate(darktable.develop, TRUE);
  dt_masks_cleanup_unused(darktable.develop);
  dt_control_log(_("history compressed and unused shapes removed"));
  // reloading history can rebuild module instances: go through the focused
  // module rather than the one this menu was opened on
  dt_iop_module_t *module = darktable.develop->gui_module;
  if(module && module->blend_data) dt_masks_gui_build_list(module);
}

// run a click gesture before the widget's own event handling: in the capture
// phase, a press its handler claims (dt_gui_claim) reaches neither the
// widget's own gesture (a GtkButton's "clicked" included) nor the row the
// widget sits in. For a button whose press does something else entirely
static void _press_before_widget(GtkGestureSingle *gesture)
{
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(gesture), GTK_PHASE_CAPTURE);
}

// the import menu: everything that brings in what another module already has.
// Shapes and AI objects are linked or copied (see the linking section above),
// parametric channels copied, and another module's whole mask arrives as a
// raster element, always live: added to the selected group, or replacing this
// module's mask outright. Submenu entries get no tooltips (see
// _popover_menu_apply_tooltips in gui/gtk.c), so section captions carry the
// explanations instead
static void _masks_import_btn_pressed(GtkGestureSingle *gesture,
                                      const int n_press,
                                      const double x,
                                      const double y,
                                      dt_iop_module_t *module)
{
  if(gtk_gesture_single_get_current_button(gesture) != GDK_BUTTON_PRIMARY) return;
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!bd) return;
  dt_gui_claim(gesture);
  GtkWidget *btn = dt_gui_get_widget(gesture);
  dt_iop_request_focus(module);

  if(gtk_widget_get_action_group(btn, "masks_import") == NULL)
  {
    GActionEntry pick_entries[] = {
      { "pick", _masks_import_pick_action, "(iii)", NULL },
    };
    GActionEntry module_entries[] = {
      { "cleanup",  _masks_import_cleanup_action,  NULL, NULL },
      { "compress", _masks_import_compress_action, NULL, NULL },
    };
    GSimpleActionGroup *sag = g_simple_action_group_new();
    g_action_map_add_action_entries(G_ACTION_MAP(sag), pick_entries,
                                    G_N_ELEMENTS(pick_entries), btn);
    g_action_map_add_action_entries(G_ACTION_MAP(sag), module_entries,
                                    G_N_ELEMENTS(module_entries), module);
    // entries shown for context but not pickable point here
    GSimpleAction *unavailable = g_simple_action_new("unavailable", NULL);
    g_simple_action_set_enabled(unavailable, FALSE);
    g_action_map_add_action(G_ACTION_MAP(sag), G_ACTION(unavailable));
    g_object_unref(unavailable);
    gtk_widget_insert_action_group(btn, "masks_import", G_ACTION_GROUP(sag));
    g_object_unref(sag);
  }
  g_object_set_data(G_OBJECT(btn), "module", module);

  GPtrArray *mods = g_ptr_array_new();
  GPtrArray *usable = g_ptr_array_new_with_free_func(_raster_source_entry_free);
  GPtrArray *later = g_ptr_array_new_with_free_func(_raster_source_entry_free);
  _raster_sources_collect(module, usable, later);

  GMenu *menu = g_menu_new();

  GMenu *link_menu = g_menu_new();
  GMenu *copy_menu = g_menu_new();
  const int n_shapes = _masks_import_fill_shapes(link_menu, module, mods, FALSE);
  _masks_import_fill_shapes(copy_menu, module, mods, TRUE);
  GMenu *param_menu = g_menu_new();
  const int n_param =
    bd->blendif_support ? _masks_import_fill_parametric(param_menu, module, mods) : 0;
  // the captions sit on the top level, so the difference between linking and
  // copying is read before either submenu is opened
  if(n_shapes)
  {
    GMenu *sec_link = g_menu_new();
    g_menu_append_submenu(sec_link, _("link shapes"), G_MENU_MODEL(link_menu));
    g_menu_append_section(menu, _("shared: editing one changes it everywhere"),
                          G_MENU_MODEL(sec_link));
    g_object_unref(sec_link);
  }
  if(n_shapes || n_param)
  {
    GMenu *sec_copy = g_menu_new();
    if(n_shapes)
      g_menu_append_submenu(sec_copy, _("copy shapes"), G_MENU_MODEL(copy_menu));
    if(n_param)
      g_menu_append_submenu(sec_copy, _("copy parametric channel"), G_MENU_MODEL(param_menu));
    g_menu_append_section(menu, _("independent copies"), G_MENU_MODEL(sec_copy));
    g_object_unref(sec_copy);
  }
  g_object_unref(link_menu);
  g_object_unref(copy_menu);
  g_object_unref(param_menu);

  GMenu *add_menu = g_menu_new();
  GMenu *use_menu = g_menu_new();
  const int n_raster = _masks_import_fill_raster(add_menu, usable, later, _IMPORT_ADD_MASK);
  _masks_import_fill_raster(use_menu, usable, later, _IMPORT_USE_MASK);
  if(n_raster)
  {
    GMenu *sec_mask = g_menu_new();
    g_menu_append_submenu(sec_mask, _("add the mask of"), G_MENU_MODEL(add_menu));
    g_menu_append_submenu(sec_mask, _("use the mask of"), G_MENU_MODEL(use_menu));
    g_menu_append_section(menu, _("another module's whole mask, kept up to date"),
                          G_MENU_MODEL(sec_mask));
    g_object_unref(sec_mask);
  }
  g_object_unref(add_menu);
  g_object_unref(use_menu);

  if(!n_shapes && !n_param && !n_raster)
    g_menu_append(menu, _("nothing to import"), "masks_import.unavailable");

  GMenu *cleanup_sec = g_menu_new();
  g_menu_append(cleanup_sec, _("clean up unused shapes"), "masks_import.cleanup");
  g_menu_append(cleanup_sec, _("compress history and clean up unused shapes"),
                "masks_import.compress");
  g_menu_append_section(menu, NULL, G_MENU_MODEL(cleanup_sec));
  g_object_unref(cleanup_sec);

  // the picks resolve their targets through these until the next popup
  g_object_set_data_full(G_OBJECT(btn), "import_modules", mods,
                         (GDestroyNotify)g_ptr_array_unref);
  g_object_set_data_full(G_OBJECT(btn), "import_rasters", usable,
                         (GDestroyNotify)g_ptr_array_unref);
  g_ptr_array_unref(later);

  darktable.gui->active_popover_menu = dt_gui_popover_menu_from_model(btn, menu);
  gtk_popover_popup(GTK_POPOVER(darktable.gui->active_popover_menu));
  g_object_unref(menu);
}

// edit on canvas and solo edit, as one run on the panel header between two
// fixed gaps: they act on the canvas, wherever the panel is hosted. Packed
// once, into the box the header reserved for them (see masks_header_edit_box)
static void _pack_header_edit_run(dt_iop_gui_blend_data_t *bd)
{
  GtkWidget *run = bd->masks_header_edit_box;
  if(!run) return;
  _pack_gap(run);
  dt_gui_box_add(run, bd->masks_edit, bd->soloedit_mode);
  _pack_gap(run);
  // soloedit_mode carries no_show_all and is shown by the mask-mode update
  gtk_widget_show(bd->masks_edit);
  gtk_widget_show(run);
}

// a parametric row's editor: its own slider, picker and boost widgets, bound
// to its form's dt_masks_point_parametric_t (_build_param_row_editor).
// Declared here, ahead of where it is built, for the functions in between
typedef struct dt_masks_param_row_editor_t
{
  dt_mask_id_t formid;
  dt_iop_module_t *module;
  dt_iop_gui_blendif_filter_t filter[2]; // input = 0, output = 1; no polarity widget
  GtkWidget *boost_box;
  GtkWidget *boost_slider;
  // two working pickers, never shown: master_picker stands in for both in one
  // slot (_param_row_master_picker_pressed), a plain or shift click going to
  // colorpicker_set_values (set the range from input or output), a ctrl click
  // to colorpicker (pick a color, point or area)
  GtkWidget *colorpicker;
  GtkWidget *colorpicker_set_values;
  GtkWidget *master_picker;
  // the opacity slider, shown with the output slider and boost_box when the
  // row's in/out chevron is open (p->in_out, _update_param_row_visibility): a
  // parametric row has no properties expander. Applied through
  // _props_row_apply, as every row's opacity is
  GtkWidget *opacity_box;
  GtkWidget *opacity_slider;
  float opacity_last_value;
  GtkWidget *sliders_grid;
  GtkWidget *input_lbl;
  GtkWidget *input_slot;
  // the bypass "eye" and the fixed-width box it lives in, laid over the right
  // end of the slider row (input_slot/output_slot are those overlays). The two
  // are separate because they hide on different conditions: the box goes only
  // with its row, while the eye inside it comes and goes with whether the
  // channel has both sub-ranges in play. See _make_param_bypass_slot.
  GtkWidget *input_bypass_btn;
  GtkWidget *input_bypass_slot;
  GtkWidget *output_lbl;
  GtkWidget *output_slot;
  GtkWidget *output_bypass_btn;
  GtkWidget *output_bypass_slot;
  GtkWidget *name_evbox;
} dt_masks_param_row_editor_t;

static void _update_param_row_display(dt_masks_param_row_editor_t *ed);
static void _update_param_row_visibility(dt_masks_param_row_editor_t *ed);

// a parametric form's single-channel point, or NULL once the form is gone
// (deleted from under a row before the next rebuild tears it down)
static dt_masks_point_parametric_t *_param_point(const dt_mask_id_t formid)
{
  dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, formid);
  if(!form || !(form->type & DT_MASKS_PARAMETRIC) || !form->points) return NULL;
  return form->points->data;
}

// how many channels a channel table holds
static int _channel_count(const dt_iop_gui_blendif_channel_t *channels)
{
  int n = 0;
  while(channels && channels[n].label) n++;
  return n;
}

// the channel table of point `p`'s colorspace, which p->channel indexes; NULL
// for no point, or for a channel the table does not have
static const dt_iop_gui_blendif_channel_t *_param_channels(const dt_masks_point_parametric_t *p)
{
  const dt_iop_gui_blendif_channel_t *channels =
    p ? dt_develop_blendif_channels_for_csp(p->colorspace) : NULL;
  return channels && (int)p->channel >= 0 && (int)p->channel < _channel_count(channels)
           ? channels
           : NULL;
}
static gboolean _param_row_picker_apply(dt_iop_module_t *module,
                                        GtkWidget *picker,
                                        dt_dev_pixelpipe_t *pipe);

// an element's properties editor (see _build_props_row_editor): built with its
// row, or in the properties subpanel, and shown while the row is expanded
typedef struct dt_masks_props_row_editor_t
{
  dt_iop_module_t *module;
  dt_mask_id_t formid;
  GtkWidget *widget[DT_MASKS_PROPERTY_LAST];
  float last_value[DT_MASKS_PROPERTY_LAST];
  // a relative (ratio) property has no absolute neutral value, so a
  // double-click resets it to the value this row first saw
  // (_props_row_populate), captured once: it undoes this sitting's edits,
  // without storing more per shape
  gboolean relative_baseline_set;
  // path-only shrink/grow control: NULL for an opacity-only editor, and hidden
  // at runtime for anything but a path
  GtkWidget *resize_widget;
  guint resize_timer;       // debounce source id (0 = none)
  gboolean resize_updating; // guard: programmatic slider change, don't commit
} dt_masks_props_row_editor_t;

static void _refine_section_refresh(dt_iop_module_t *module);
static void _update_add_target_hints(dt_iop_module_t *module);
static void _update_refine_sensitivity(dt_iop_module_t *module);
static void _set_group_target_ext(dt_iop_module_t *module,
                                  const dt_mask_id_t cid,
                                  const dt_mask_id_t keep_entered);
static void _set_group_target(dt_iop_module_t *module, const dt_mask_id_t cid);
static void _set_form_target(dt_iop_module_t *module, const dt_mask_id_t id);
static void _element_chevron_clicked(dt_iop_module_t *module,
                                     const dt_mask_id_t id,
                                     const gboolean expanded);
static void _paint_param_inout(cairo_t *cr,
                               const gint x,
                               const gint y,
                               const gint w,
                               const gint h,
                               const gint flags,
                               void *data);
// detach members from a module's mask group without dt_masks_form_remove's
// nested history/GUI update and its "group just emptied" destruction cascade
static void _detach_group_members(dt_masks_form_t *grp, GList *fids);
static void _recompute_insert_hint(dt_iop_module_t *module);
static void _blendif_options_callback(GtkButton *button, dt_iop_module_t *module);
static gboolean _op_is_bypassed(const int state);
static DTGTKCairoPaintIconFunc _kind_icon_paint(const guint kind);
static dt_masks_form_t *_pending_form(dt_iop_module_t *module);
static DTGTKCairoPaintIconFunc _flexi_op_paint(const dt_masks_state_t flexi_op);
static GtkWidget *_make_channel_handle(const char *code, const char *tooltip);
static GtkWidget *_make_pending_shape_row(dt_iop_module_t *module, dt_masks_form_t *form);
static void _props_panel_show(dt_iop_gui_blend_data_t *bd);
static GtkWidget *_build_group_opacity_editor(dt_iop_module_t *module, const dt_mask_id_t cid);
static GtkWidget *_build_param_boost_editor(dt_iop_module_t *module, const dt_mask_id_t formid);

// the mask being off disables nothing: editing a mask control turns the mask
// on, as editing a module's parameter turns the module on
// (dt_dev_add_history_item's `enable` argument). A control is insensitive only
// when it has nothing to act on, whatever the on/off state (see also
// _update_add_target_hints and _update_refine_sensitivity). The off
// state shows on the on/off toggle alone
static void _masks_panel_apply_shape_sensitivity(dt_iop_gui_blend_data_t *data)
{
  // "edit on canvas" and "solo edit" need shapes to put on the canvas, and a
  // mask that is not locked (see _mask_lock_sync). Sensitivity is set on the
  // widget itself, not on the cluster holding it, because the hamburger
  // shares that cluster.
  const gboolean has_drawn = _module_has_drawn_shapes(data->module);
  const gboolean locked = dt_develop_blend_mask_locked(data->module->blend_params);
  if(data->masks_edit) gtk_widget_set_sensitive(data->masks_edit, has_drawn && !locked);
  // the whole edit run goes with them: with no shapes there is nothing to put
  // on the canvas or to solo. Like every header button but the expander and
  // the on/off toggle, it is also hidden while the mask is off
  const gboolean enabled = data->module->blend_params->mask_mode != DEVELOP_MASK_DISABLED;
  if(data->masks_header_edit_box)
    gtk_widget_set_visible(data->masks_header_edit_box, has_drawn && enabled);
  // the mask overlay button (data->showmask) is deliberately NOT here: it
  // follows the module header's own mask indicator, which shows only for an
  // enabled mask, since an off mask renders nothing to overlay (see
  // _blendop_masks_mode_callback)
}

// disarm every add-shape button
static void _masks_shapes_set_inactive(dt_iop_gui_blend_data_t *bd)
{
  for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
    if(bd->masks_shapes[n])
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_shapes[n]), FALSE);
}

// shapes disarmed and on-canvas editing off: nothing of the mask on canvas
static void _masks_canvas_off(dt_iop_gui_blend_data_t *bd)
{
  _masks_shapes_set_inactive(bd);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), FALSE);
  dt_masks_set_edit_mode(bd->module, DT_MASKS_EDIT_OFF);
}

static void _blendop_masks_mode_callback(const dt_develop_mask_mode_t mask_mode,
                                         dt_iop_gui_blend_data_t *data)
{
  dt_develop_blend_params_t *bp = data->module->blend_params;
  if(bp->mask_mode != mask_mode)
    dt_print(DT_DEBUG_MASKS,
             "[masks] _blendop_masks_mode_callback '%s': mask_mode 0x%x->0x%x",
             data->module->op, bp->mask_mode, mask_mode);
  const gboolean was_enabled = bp->mask_mode & DEVELOP_MASK_ENABLED;
  bp->mask_mode = mask_mode;

  const gboolean mask_enabled = mask_mode & DEVELOP_MASK_ENABLED;
  const gboolean mode_raster = mask_mode & DEVELOP_MASK_RASTER;
  const gboolean mode_drawn = mask_mode & DEVELOP_MASK_MASK;
  const gboolean mode_flexi = !mode_raster && (mask_enabled || (mask_mode & DEVELOP_MASK_FLEXI));
  const gboolean mode_parametric = mask_mode & DEVELOP_MASK_CONDITIONAL;
  // with the mask off the panel shows the same controls it would with the mask
  // on, and they stay live: touching one switches the mask on, and always into
  // flexi (see _blendop_mask_enable), so flexi is the layout to show
  const gboolean show_flexi = !mode_raster;

  _box_set_visible(data->blend_box, TRUE);
  _masks_panel_apply_shape_sensitivity(data);

  if(data->masks_blend_header)
  {
    if(mask_enabled)
      dt_gui_add_class(data->masks_blend_header, "dt_masks_enabled");
    else
      dt_gui_remove_class(data->masks_blend_header, "dt_masks_enabled");
  }
  // the docked panel is styled as a module, active while its mask is on
  if(darktable.develop->proxy.masks_flexi_host.hosted_module == data->module)
    dt_ui_flexi_panel_set_active(darktable.gui->ui, mask_enabled);

  dt_iop_advertise_rastermask(data->module, mask_mode);

  // flexi reuses the drawn-group renderer, so the refinement controls appear
  // for it as for a drawn mask
  if(mask_enabled
     && ((data->masks_inited && (mode_drawn || mode_flexi))
         || (data->blendif_support && mode_parametric)))
  {
    if(data->blendif_support && mode_parametric)
      dt_bauhaus_combobox_set_from_value(data->masks_combine_combo,
         bp->mask_combine & (DEVELOP_COMBINE_INV | DEVELOP_COMBINE_INCL));
    gtk_widget_set_visible(GTK_WIDGET(data->masks_combine_combo),
                           data->blendif_support && mode_parametric);

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

      // disable also guided-filters on RAW based color space
      GtkWidget *const raw_off[] = { data->masks_feathering_guide_combo,
                                     data->feathering_radius_slider, data->brightness_slider,
                                     data->contrast_slider, data->details_slider };
      for(size_t i = 0; i < G_N_ELEMENTS(raw_off); i++)
      {
        gtk_widget_set_sensitive(raw_off[i], FALSE);
        gtk_widget_hide(raw_off[i]);
      }
    }

    _box_set_visible(data->refine_box, TRUE);
  }
  else
  {
    // mask off: still shown, as described above
    _box_set_visible(data->refine_box, !mask_enabled);
  }

  if(data->masks_inited && show_flexi)
  {
    if(data->masks_param_channels_box)
      gtk_widget_set_visible(data->masks_param_channels_box, data->blendif_support);
    gtk_widget_set_visible(data->masks_list_area, TRUE);
    gtk_widget_set_visible(data->masks_toolbar, TRUE);
    if(data->soloedit_mode) gtk_widget_set_visible(data->soloedit_mode, TRUE);
    gtk_widget_set_visible(GTK_WIDGET(data->masks_list_box), TRUE);
    // only for a live mask: with the mask off the list keeps whatever it last
    // held, rather than being rebuilt from a group nothing is using. A list
    // never built is built anyway, so switching the mask on or off never
    // changes the groups it shows
    if(mode_flexi || data->masks_list_sig == DT_INVALID_HASH)
      dt_masks_gui_build_list(data->module);
    _box_set_visible(data->masks_box, TRUE);
    _props_panel_show(data);

    // the panel is a preview of what switching the mask on would give, so
    // nothing of it belongs on canvas
    if(!mask_enabled) _masks_canvas_off(data);
  }
  else if(data->masks_support)
  {
    if(data->masks_inited)
      _masks_canvas_off(data);
    else
      _masks_shapes_set_inactive(data);
    _box_set_visible(data->masks_box, FALSE);
    _box_set_visible(data->props_panel_box, FALSE);
  }

  // leaving flexi: drop flexi-only selection/staging state
  if(!mode_flexi)
  {
    data->panel_selected_formid = INVALID_MASKID;
    data->panel_selected_group_cid = INVALID_MASKID;
    data->canvas_hovered_formid = INVALID_MASKID;
    data->insert_active = FALSE;
  }

  // the parametric rows' pickers stand down with the list they sit in, shown
  // as above whether the mask is on or off
  if(data->blendif_support && !(data->masks_inited && show_flexi))
    dt_iop_color_picker_reset(data->module, FALSE);

  dt_dev_add_history_item(darktable.develop, data->module, TRUE);

  // rebuild the accelerators
  dt_iop_connect_accels_multi(data->module->so);

  // switching the mask on is an act of reaching for its controls, so unfold
  if(mask_enabled && !was_enabled) dt_masks_gui_panel_set_collapsed_pref(FALSE);

  // mode just changed (possibly into/out of flexi) while this module was
  // already focused: dt_iop_request_focus() is a no-op in that case, so
  // re-evaluate the flexi panel's host placement here too
  dt_iop_gui_blend_masks_panel_relocate(data->module);
}

static void _blendop_blend_mode_callback(GtkWidget *combo,
                                         dt_iop_gui_blend_data_t *data)
{
  DT_GUARD_GUI_UPDATE();

  dt_develop_blend_params_t *bp = data->module->blend_params;
  const dt_develop_blend_mode_t new_blend_mode =
    GPOINTER_TO_INT(dt_bauhaus_combobox_get_data(combo));

  if(new_blend_mode != (bp->blend_mode & DEVELOP_BLEND_MODE_MASK))
  {
    bp->blend_mode = new_blend_mode | (bp->blend_mode & DEVELOP_BLEND_REVERSE);

    if(_blendif_blend_parameter_enabled(data->blend_modes_csp, bp->blend_mode))
    {
      gtk_widget_show(data->blend_mode_parameter_slider);
    }
    else
    {
      bp->blend_parameter = 0.0f;
      dt_bauhaus_slider_set(data->blend_mode_parameter_slider, bp->blend_parameter);
      gtk_widget_hide(data->blend_mode_parameter_slider);
    }
    // blending is skipped entirely while the mask is off, so a blend mode
    // picked there would do nothing until the mask came on
    _blendop_mask_enable(data->module);
    dt_dev_add_history_item(darktable.develop, data->module, TRUE);
  }
}

static void _blendop_blend_order_clicked(GtkGestureSingle *gesture,
                                             gint n_press,
                                             gdouble x,
                                             gdouble y,
                                             dt_iop_module_t *module)
{
  DT_GUARD_GUI_UPDATE();

  GtkWidget *button = dt_gui_get_widget(gesture);

  dt_develop_blend_params_t *bp = module->blend_params;
  const gboolean active = !(bp->blend_mode & DEVELOP_BLEND_REVERSE);

  if(!active)
    bp->blend_mode &= ~DEVELOP_BLEND_REVERSE;
  else
    bp->blend_mode |= DEVELOP_BLEND_REVERSE;

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), active);

  _blendop_mask_enable(module);
  dt_dev_add_history_item(darktable.develop, module, TRUE);
  dt_control_queue_redraw_widget(GTK_WIDGET(button));
}

static void _blendop_masks_combine_callback(GtkWidget *combo,
                                            dt_iop_gui_blend_data_t *data)
{
  dt_develop_blend_params_t *const bp = data->module->blend_params;

  const uint32_t combine =
    GPOINTER_TO_UINT(dt_bauhaus_combobox_get_data(data->masks_combine_combo));
  bp->mask_combine &= ~(DEVELOP_COMBINE_INV | DEVELOP_COMBINE_INCL);
  bp->mask_combine |= combine;

  // inverts the parametric mask channels that are not used
  if(data->blendif_support)
  {
    const uint32_t mask =
      data->csp == DEVELOP_BLEND_CS_LAB
      ? DEVELOP_BLENDIF_Lab_MASK
      : DEVELOP_BLENDIF_RGB_MASK;

    const uint32_t unused_channels = mask & ~bp->blendif;

    bp->blendif &= ~(unused_channels << 16);
    if(bp->mask_combine & DEVELOP_COMBINE_INCL)
    {
      bp->blendif |= unused_channels << 16;
    }
  }

  _blendop_mask_enable(data->module);
  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
}

static float _log10_scale_callback(GtkWidget *self,
                                  const float inval,
                                  const int dir)
{
  float outval = .0f;
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


static float _magnifier_scale_callback(GtkWidget *self,
                                      const float inval,
                                      const int dir)
{
  const float range = 6.0f;
  const float invrange = 1.0f/range;
  const float scale = tanh(range * 0.5f);
  const float invscale = 1.0f/scale;
  const float eps = 1.0e-6f;
  const float tiny = 1.0e-4f;

  float outval = .0f;
  switch(dir)
  {
    case GRADIENT_SLIDER_SET:
      outval = (invscale * tanh(range *
                                (CLAMP(inval, 0.0f, 1.0f) - 0.5f)) + 1.0f) * 0.5f;
      if(outval <= tiny) outval = 0.0f;
      if(outval >= 1.0f - tiny) outval = 1.0f;
      break;
    case GRADIENT_SLIDER_GET:
      outval = invrange * atanh((2.0f *
                                 CLAMP(inval, eps, 1.0f - eps) - 1.0f) * scale) + 0.5f;
      if(outval <= tiny) outval = 0.0f;
      if(outval >= 1.0f - tiny) outval = 1.0f;
      break;
    default:
      outval = inval;
  }
  return outval;
}

// defined below, next to the other per-row editor helpers
static dt_masks_param_row_editor_t *_param_row_editor_resolve(
  GtkWidget *widget, const dt_iop_gui_blendif_channel_t **channels_out, int *ch_out);

// toggle a slider's alternative (log / magnifier) display scale. The slider
// always belongs to a per-row parametric editor, which has no text label to
// carry the scale: its tooltip names it, and a toast says it changed
static int _blendop_blendif_disp_alternative_worker(GtkWidget *widget,
                                                    dt_iop_module_t *module,
                                                    const int mode,
                                                    float (*scale_callback)(GtkWidget*, float, int),
                                                    const char *label)
{
  GtkDarktableGradientSlider *slider = (GtkDarktableGradientSlider *)widget;

  dtgtk_gradient_slider_multivalue_set_scale_callback
    (slider,
     (mode == 1) ? scale_callback : NULL);

  const dt_iop_gui_blendif_channel_t *channels;
  int ch;
  dt_masks_param_row_editor_t *ed = _param_row_editor_resolve(widget, &channels, &ch);
  if(ed)
  {
    const int in_out = (widget == GTK_WIDGET(ed->filter[1].slider)) ? 1 : 0;
    ed->filter[in_out].altmode = (mode == 1) ? 1 : 0;
    ed->filter[in_out].altmode_name = (mode == 1) ? label : NULL;
    _update_param_row_display(ed);
    const char *which = in_out ? _("output") : _("input");
    if(mode == 1)
      dt_toast_log(_("%s slider: %s scale"), which, label);
    else
      dt_toast_log(_("%s slider: linear scale"), which);
  }

  return mode == 1;
}

static int _blendop_blendif_disp_alternative_mag(GtkWidget *widget,
                                                 dt_iop_module_t *module,
                                                 const int mode)
{
  return _blendop_blendif_disp_alternative_worker
    (widget, module, mode, _magnifier_scale_callback, _("zoom"));
}

static int _blendop_blendif_disp_alternative_log(GtkWidget *widget,
                                                 dt_iop_module_t *module,
                                                 const int mode)
{
  return _blendop_blendif_disp_alternative_worker
    (widget, module, mode, _log10_scale_callback, _("log"));
}

// the colorspace a picker samples in for channel `channel` (an index into the
// channel table) of blend colorspace `csp`
static dt_iop_colorspace_type_t
_picker_colorspace_for_channel(const dt_develop_blend_colorspace_t csp,
                               const int channel)
{
  dt_iop_colorspace_type_t picker_cst = IOP_CS_NONE;

  if(csp == DEVELOP_BLEND_CS_RGB_DISPLAY)
  {
    if(channel < 4)
      picker_cst = IOP_CS_RGB;
    else
      picker_cst = IOP_CS_HSL;
  }
  else if(csp == DEVELOP_BLEND_CS_RGB_SCENE)
  {
    if(channel < 4)
      picker_cst = IOP_CS_RGB;
    else
      picker_cst = IOP_CS_JZCZHZ;
  }
  else if(csp == DEVELOP_BLEND_CS_LAB)
  {
    if(channel < 3)
      picker_cst = IOP_CS_LAB;
    else
      picker_cst = IOP_CS_LCH;
  }

  return picker_cst;
}

static void _blendop_blendif_showmask_clicked(
  GtkGestureSingle *gesture, gint n_press, gdouble x, gdouble y, dt_iop_module_t *module)
{
  DT_GUARD_GUI_UPDATE();

  if(dt_gui_current_button(gesture) != GDK_BUTTON_PRIMARY) return;

  GtkWidget *button = dt_gui_get_widget(gesture);

  const gboolean has_mask_display =
    module->request_mask_display
    & (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL);

  module->request_mask_display &=
    ~(DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL
      | DT_DEV_PIXELPIPE_DISPLAY_ANY);

  GdkModifierType state = dt_gui_current_state(gesture);

  if(dt_modifier_is(state, GDK_CONTROL_MASK | GDK_SHIFT_MASK))
    module->request_mask_display |=
      (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL);
  else if(dt_modifier_is(state, GDK_SHIFT_MASK))
    module->request_mask_display |= DT_DEV_PIXELPIPE_DISPLAY_CHANNEL;
  else if(dt_modifier_is(state, GDK_CONTROL_MASK))
    module->request_mask_display |= DT_DEV_PIXELPIPE_DISPLAY_MASK;
  else
    module->request_mask_display |=
      (has_mask_display ? DT_DEV_PIXELPIPE_DISPLAY_NONE : DT_DEV_PIXELPIPE_DISPLAY_MASK);

  gtk_toggle_button_set_active
    (GTK_TOGGLE_BUTTON(button),
     module->request_mask_display != DT_DEV_PIXELPIPE_DISPLAY_NONE);

  if(module->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), TRUE);

  DT_ENTER_GUI_UPDATE();

  // (re)set the header mask indicator too
  if(module->mask_indicator)
    gtk_toggle_button_set_active
      (GTK_TOGGLE_BUTTON(module->mask_indicator),
       module->request_mask_display != DT_DEV_PIXELPIPE_DISPLAY_NONE);

  DT_LEAVE_GUI_UPDATE();

  dt_iop_request_focus(module);
  _refresh_mask_display(module);
}

static void _update_mask_enable_toggle_tooltip(GtkWidget *toggle, const gboolean enabled)
{
  if(!toggle) return;
  // a right-click is the way to the blending options (see
  // _blendop_mask_enable_toggled), so the tooltip says so
  gtk_widget_set_tooltip_text(toggle,
                              enabled
                              ? _("mask enabled\nclick to disable\nright-click for blending options")
                              : _("mask disabled\nclick to enable\nright-click for blending options"));
}

// the lock's own button, the module header's indicator, and what the lock
// takes out of reach. The panel's controls are made insensitive wholesale
// rather than each refusing its own edit. The on/off toggle stays sensitive:
// its right-click opens the blending options, whose panel settings are view,
// not mask (its left-click refuses instead, see _blendop_mask_enable_toggled)
static void _mask_lock_sync(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!bd || !bd->mask_lock_btn) return;

  const dt_develop_blend_params_t *bp = module->blend_params;
  const gboolean locked = dt_develop_blend_mask_locked(bp);
  const gboolean enabled = bp->mask_mode != DEVELOP_MASK_DISABLED;

  DT_ENTER_GUI_UPDATE();
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->mask_lock_btn), locked);
  DT_LEAVE_GUI_UPDATE();
  gchar *lock_tip = g_strdup_printf(
    "%s\n\n%s",
    locked ? _("mask locked: click to unlock") : _("mask unlocked: click to lock"),
    _("a locked mask cannot be edited, and is not affected by\n"
      "module reset, presets, styles or pasted module parameters.\n"
      "discarding the history still removes it"));
  gtk_widget_set_tooltip_text(bd->mask_lock_btn, lock_tip);
  g_free(lock_tip);
  // shown with an off mask too while locked: an off mask keeps its lock, and
  // this is the panel's only way to lift it
  gtk_widget_set_visible(bd->mask_lock_btn,
                         bd->masks_support && !module->hide_enable_button
                         && (enabled || locked));

  if(bd->masks_box) gtk_widget_set_sensitive(GTK_WIDGET(bd->masks_box), !locked);
  if(bd->refine_box) gtk_widget_set_sensitive(GTK_WIDGET(bd->refine_box), !locked);
  _masks_panel_apply_shape_sensitivity(bd);
  if(bd->soloedit_mode) gtk_widget_set_sensitive(bd->soloedit_mode, !locked);

  dt_iop_add_remove_mask_lock_indicator(module, locked);

  if(locked && bd->masks_shown != DT_MASKS_EDIT_OFF)
    dt_masks_set_edit_mode(module, DT_MASKS_EDIT_OFF);
}

void dt_iop_gui_blend_set_mask_lock(dt_iop_module_t *module, const gboolean lock)
{
  if(!module || !module->blend_params) return;
  dt_develop_blend_params_t *bp = module->blend_params;
  if(dt_develop_blend_mask_locked(bp) == lock) return;

  bp->mask_lock = lock ? 1 : 0;
  // locking changes nothing the module renders, so it does not switch it on
  dt_dev_add_history_item(darktable.develop, module, FALSE);
  _mask_lock_sync(module);
}

static void _mask_lock_clicked(GtkGestureSingle *gesture,
                               gint n_press,
                               gdouble x,
                               gdouble y,
                               dt_iop_module_t *module)
{
  DT_GUARD_GUI_UPDATE();
  if(dt_gui_current_button(gesture) != GDK_BUTTON_PRIMARY) return;
  dt_iop_request_focus(module);
  dt_iop_gui_blend_set_mask_lock(module,
                                 !dt_develop_blend_mask_locked(module->blend_params));
}

// switch the blend mask on (flexi), unless it is on already. Every panel
// control that writes calls it before committing, so editing a mask control
// turns the mask on, as editing a module's parameter turns the module on
// (dt_dev_add_history_item's `enable` argument). The off path of
// bd->mask_enable_toggle is the one place that must not call it
static void _blendop_mask_enable(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *data = module->blend_data;
  if(module->blend_params->mask_mode
     & (DEVELOP_MASK_MASK | DEVELOP_MASK_FLEXI | DEVELOP_MASK_RASTER))
    return;

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->mask_enable_toggle), TRUE);
  _update_mask_enable_toggle_tooltip(data->mask_enable_toggle, TRUE);
  _blendop_masks_mode_callback(DEVELOP_MASK_ENABLED | DEVELOP_MASK_FLEXI, data);
  dt_iop_add_remove_mask_indicator(module, TRUE);
  gtk_widget_set_visible(data->showmask, TRUE);
  _mask_lock_sync(module);

  // no unfolding here: _blendop_masks_mode_callback above clears the fold
  // preference and relocates the panel, which unfolds it in any position

  DT_ENTER_GUI_UPDATE();
  if(module->mask_indicator)
    gtk_toggle_button_set_active(
      GTK_TOGGLE_BUTTON(module->mask_indicator),
      gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data->showmask)));
  DT_LEAVE_GUI_UPDATE();
}

// _blendop_mask_enable for other files (see blend.h): mask_enable_toggle acts
// on a click gesture, not on "toggled", so setting the toggle active would
// not switch the mask on
void dt_iop_gui_blend_mask_enable(dt_iop_module_t *module)
{
  if(!module || !module->blend_data) return;
  // a module can support blending and still have no mask of its own
  // (IOP_FLAGS_NO_MASKS: retouch and spots keep their forms outside the blend
  // mask). It has a mask_enable_toggle all the same, so check what the toggle
  // stands for rather than whether it exists -- dt_dev_add_masks_history_item
  // calls this for every mask edit, those modules' own form edits included
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!bd->masks_support || !bd->masks_inited) return;

  // this runs inside the callback that made the edit, and switching the mask
  // on rebuilds the list, which would destroy the row and the editor that
  // callback still holds. So the rebuild is suppressed and queued for the
  // idle, and only when the mask goes on, or every edit would tear down the
  // slider being dragged. Read under history_mutex: the pipe's history replay
  // resets blend_params, mask off, before replaying
  // (dt_dev_pixelpipe_synch_all), and an edit then would read the mask as off
  dt_pthread_mutex_lock(&darktable.develop->history_mutex);
  const gboolean was_on = module->blend_params->mask_mode
                          & (DEVELOP_MASK_MASK | DEVELOP_MASK_FLEXI | DEVELOP_MASK_RASTER);
  dt_pthread_mutex_unlock(&darktable.develop->history_mutex);
  dt_iop_request_focus(module);
  if(was_on) return;

  const gboolean was_suppressed = bd->masks_rebuild_suppressed;
  bd->masks_rebuild_suppressed = TRUE;
  _blendop_mask_enable(module);
  bd->masks_rebuild_suppressed = was_suppressed;
  if(!was_suppressed) _queue_masks_list_rebuild(module);
}

void dt_iop_gui_blend_sync_pending_ai_sliders(dt_iop_module_t *module)
{
#ifdef HAVE_AI
  dt_iop_gui_blend_data_t *bd = module ? module->blend_data : NULL;
  if(!bd || !bd->pending_ai_smoothing_slider || !bd->pending_ai_cleanup_slider) return;

  float smoothing = 0.0f;
  int cleanup = 0;
  if(!dt_masks_object_creation_get_preview_params(&smoothing, &cleanup, NULL)) return;

  DT_ENTER_GUI_UPDATE();
  dt_bauhaus_slider_set(bd->pending_ai_smoothing_slider, smoothing);
  dt_bauhaus_slider_set(bd->pending_ai_cleanup_slider, (float)cleanup);
  DT_LEAVE_GUI_UPDATE();
  bd->pending_ai_smoothing_last = smoothing;
  bd->pending_ai_cleanup_last = (float)cleanup;
#else
  (void)module;
#endif
}

// the blend mask's on/off toggle. An empty mask blends uniformly (blend.c's
// "no form" fallback), which is not the same as an off module:
// DEVELOP_MASK_DISABLED skips blending (pixelpipe_hb.c), while an empty mask
// blends through a uniform mask, so blend mode and opacity still act
static void _blendop_mask_enable_toggled(
  GtkGestureSingle *gesture, gint n_press, gdouble x, gdouble y, dt_iop_module_t *module)
{
  DT_GUARD_GUI_UPDATE();
  const guint pressed = dt_gui_current_button(gesture);
  GtkWidget *button = dt_gui_get_widget(gesture);

  // the blending options open on a right-click, as the guide settings do on
  // the guides icon in the toolbar
  if(pressed == GDK_BUTTON_SECONDARY)
  {
    dt_iop_request_focus(module);
    _blendif_options_callback(GTK_BUTTON(button), module);
    return;
  }
  if(pressed != GDK_BUTTON_PRIMARY) return;

  dt_iop_gui_blend_data_t *data = module->blend_data;

  dt_iop_request_focus(module);

  // switching it on or off is a change to the mask like any other
  if(dt_develop_blend_mask_locked(module->blend_params))
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button),
                                 module->blend_params->mask_mode != DEVELOP_MASK_DISABLED);
    dt_control_log(_("the mask is locked"));
    return;
  }

  // branch on the mask, not on the button: the toggle moves between headers
  // with the panel (dt_iop_gui_blend_masks_panel_relocate) and can show a
  // stale state. Both branches set it
  if(module->blend_params->mask_mode == DEVELOP_MASK_DISABLED)
  {
    // a mask switched on for the first time starts with the default group
    // layout. Only here, at the explicit switch: the other way on is an edit
    // (dt_iop_gui_blend_mask_enable), which already puts something in the mask
    const gboolean fresh = !dt_masks_gui_module_mask_group(module);
    _blendop_mask_enable(module);
    if(fresh) dt_masks_gui_apply_default_preset(module);
  }
  else
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), FALSE);
    _update_mask_enable_toggle_tooltip(button, FALSE);
    gtk_widget_set_visible(data->showmask, FALSE);
    _blendop_masks_mode_callback(DEVELOP_MASK_DISABLED, data);
    dt_iop_add_remove_mask_indicator(module, FALSE);
    _mask_lock_sync(module);
  }

  dt_control_hinter_message("");
}

static void _blendop_masks_add_shape(GtkGestureSingle *gesture,
                                         gint n_press,
                                         gdouble x,
                                         gdouble y,
                                         dt_iop_module_t *self)
{
  GtkWidget *widget = dt_gui_get_widget(gesture);

  dt_iop_gui_blend_data_t *bd = self->blend_data;

  const GdkModifierType state = dt_gui_current_state(gesture);
  const gboolean continuous = dt_modifier_is(state, GDK_CONTROL_MASK);

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

  if(this < 0) return;

#ifdef HAVE_AI
  if(bd->masks_type[this] == DT_MASKS_OBJECT && !dt_masks_object_available())
  {
    dt_control_log(_("AI model is not available. Check preferences > AI"));
    return;
  }
#endif

  _blendop_mask_enable(self);

  // if the clicked shape is already armed, clicking it again disarms it
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))
     && darktable.develop->form_gui
     && darktable.develop->form_gui->creation
     && darktable.develop->form_gui->creation_module == self)
  {
    darktable.develop->form_gui->creation_continuous = FALSE;
    darktable.develop->form_gui->creation_continuous_module = NULL;
    _masks_shapes_set_inactive(bd);
    dt_masks_change_form_gui(NULL);
    dt_control_queue_redraw_center();
    return;
  }

  _masks_shapes_set_inactive(bd);

  // we want to be sure that the iop has focus
  dt_iop_request_focus(self);
  dt_iop_color_picker_reset(self, FALSE);
  bd->masks_shown = DT_MASKS_EDIT_FULL;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), FALSE);
  // we create the new form
  dt_masks_form_t *form = dt_masks_create(bd->masks_type[this]);
  dt_masks_change_form_gui(form);
  darktable.develop->form_gui->creation_module = self;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), TRUE);
  // make the pending-row placeholder appear immediately (see
  // dt_masks_gui_build_list's pending-row synthesis / dt_masks_gui_list_signature)
  _queue_masks_list_rebuild(self);

  if(continuous)
  {
    darktable.develop->form_gui->creation_continuous = TRUE;
    darktable.develop->form_gui->creation_continuous_module = self;
  }

  dt_control_queue_redraw_center();
}

static void _blendop_masks_show_and_edit(GtkGestureSingle *gesture,
                                             gint n_press,
                                             gdouble x,
                                             gdouble y,
                                             dt_iop_module_t *self)
{
  darktable.develop->form_gui->creation_continuous = FALSE;
  darktable.develop->form_gui->creation_continuous_module = NULL;

  dt_iop_gui_blend_data_t *bd = self->blend_data;

  dt_iop_request_focus(self);

  DT_ENTER_GUI_UPDATE();

  dt_iop_color_picker_reset(self, FALSE);

  GdkModifierType state = dt_gui_current_state(gesture);

  if(_module_has_drawn_shapes(self))
  {
    const gboolean control_button_pressed =
      dt_modifier_is(state, GDK_CONTROL_MASK);

    switch(bd->masks_shown)
    {
      case DT_MASKS_EDIT_FULL:
        bd->masks_shown = control_button_pressed
          ? DT_MASKS_EDIT_RESTRICTED
          : DT_MASKS_EDIT_OFF;
        break;

      case DT_MASKS_EDIT_RESTRICTED:
        bd->masks_shown = !control_button_pressed
          ? DT_MASKS_EDIT_FULL
          : DT_MASKS_EDIT_OFF;
        break;

      default:
      case DT_MASKS_EDIT_OFF:
        bd->masks_shown = control_button_pressed
          ? DT_MASKS_EDIT_RESTRICTED
          : DT_MASKS_EDIT_FULL;
    }
  }
  else
  {
    bd->masks_shown = DT_MASKS_EDIT_OFF;
    /* remove hinter messages */
    dt_control_hinter_message("");
  }

  gtk_toggle_button_set_active
    (GTK_TOGGLE_BUTTON(bd->masks_edit), bd->masks_shown != DT_MASKS_EDIT_OFF);
  dt_masks_set_edit_mode(self, bd->masks_shown);

  _masks_shapes_set_inactive(bd);

  DT_LEAVE_GUI_UPDATE();
}

// a blend-level color pick: every one belongs to a parametric row's own editor
gboolean blend_color_picker_apply(dt_iop_module_t *module,
                                  GtkWidget *picker,
                                  dt_dev_pixelpipe_t *pipe)
{
  return _param_row_picker_apply(module, picker, pipe);
}

// a parametric element of the module's mask, which a blend colorspace change
// removes: a form keeps the channels of the colorspace it was made in, but is
// evaluated in the module's (_parametric_get_mask_roi in masks/parametric.c),
// and channels do not map between colorspaces. The change asks first
// (_blendif_change_blend_colorspace). By id, not by pointer: removing a form
// can remove the emptied group too (dt_masks_form_remove)
typedef struct _parametric_ref_t
{
  dt_mask_id_t grpid;   // the group holding it, which is where it is removed from
  dt_mask_id_t formid;
} _parametric_ref_t;

// every parametric element in the mask, at any nesting depth -- one inside a
// subgroup is just as unable to survive the switch as a top-level one
static void _collect_parametric_forms(dt_masks_form_t *grp,
                                      GList **out,
                                      const int depth)
{
  if(!grp || depth > DT_MASKS_NESTING_MAX) return;
  for(const GList *l = grp->points; l; l = g_list_next(l))
  {
    const dt_masks_point_group_t *const pt = l->data;
    dt_masks_form_t *const f = dt_masks_get_from_id(darktable.develop, pt->formid);
    if(!f) continue;

    if(f->type & DT_MASKS_PARAMETRIC)
    {
      _parametric_ref_t *const ref = calloc(1, sizeof(_parametric_ref_t));
      ref->grpid = grp->formid;
      ref->formid = f->formid;
      *out = g_list_prepend(*out, ref);
    }
    else if(f->type & (DT_MASKS_GROUP | DT_MASKS_OBJECT))
      _collect_parametric_forms(f, out, depth + 1);
  }
}

static gboolean _blendif_change_blend_colorspace(dt_iop_module_t *module,
                                                 dt_develop_blend_colorspace_t cst)
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
    // parametric elements cannot come along (see _collect_parametric_forms),
    // so ask before deleting them, and switch nothing without a yes. Decided
    // here, for every way into a colorspace change, not by the menu disabling
    // its entries
    GList *parametrics = NULL;
    _collect_parametric_forms(dt_masks_gui_module_mask_group(module), &parametrics, 0);
    if(parametrics)
    {
      const int n = g_list_length(parametrics);
      const gboolean confirmed = dt_gui_show_yes_no_dialog(
        ngettext("remove parametric element?",
                 "remove parametric elements?", n), "",
        ngettext("this mask has %d parametric element. it stores the channels of"
                 " the colorspace it was created in, and those channels do not"
                 " exist in another colorspace, so it cannot be carried over.\n\n"
                 "change the blend colorspace and remove it?",
                 "this mask has %d parametric elements. they store the channels of"
                 " the colorspace they were created in, and those channels do not"
                 " exist in another colorspace, so they cannot be carried over.\n\n"
                 "change the blend colorspace and remove them?", n), n);
      if(!confirmed)
      {
        g_list_free_full(parametrics, free);
        return FALSE;
      }

      dt_masks_clear_form_gui(darktable.develop);
      for(const GList *l = parametrics; l; l = g_list_next(l))
      {
        const _parametric_ref_t *const ref = l->data;
        // re-resolved per iteration: an earlier removal may have taken the
        // group with it (see the _parametric_ref_t comment)
        dt_masks_form_t *const owner = dt_masks_get_from_id(darktable.develop, ref->grpid);
        dt_masks_form_t *const form = dt_masks_get_from_id(darktable.develop, ref->formid);
        if(owner && form) dt_masks_form_remove(module, owner, form);
      }
      g_list_free_full(parametrics, free);
      dt_dev_add_masks_history_item(darktable.develop, module, TRUE);

      // the panel's selection and its cached list signature can still name the
      // forms just deleted; dt_iop_gui_update() below rebuilds from these
      dt_iop_gui_blend_data_t *bd = module->blend_data;
      if(bd)
      {
        bd->panel_selected_formid = INVALID_MASKID;
        bd->masks_list_sig = DT_INVALID_HASH;
      }
    }

    dt_develop_blend_init_blendif_parameters(module->blend_params, cst);

    // look for last history item for this module with the selected
    // blending mode to copy parametric mask settings
    for(const GList *history = g_list_last(darktable.develop->history);
        history;
        history = g_list_previous(history))
    {
      const dt_dev_history_item_t *data = history->data;
      if(data->module == module && data->blend_params->blend_cst == cst)
      {
        const dt_develop_blend_params_t *hp = data->blend_params;
        dt_develop_blend_params_t *np = module->blend_params;

        np->blend_mode = hp->blend_mode;
        np->blend_parameter = hp->blend_parameter;
        np->blendif = hp->blendif;
        memcpy(np->blendif_parameters,
               hp->blendif_parameters, sizeof(hp->blendif_parameters));
        memcpy(np->blendif_boost_factors,
               hp->blendif_boost_factors, sizeof(hp->blendif_boost_factors));
        break;
      }
    }

    // no picker survives the switch: every one belongs to a parametric row,
    // and those were removed above
    dt_dev_add_new_history_item(darktable.develop, module, FALSE);
    dt_iop_gui_update(module);

    return TRUE;
  }
  return FALSE;
}

static void _masks_opacity_sticky_toggled(GtkToggleButton *mi, dt_iop_module_t *module)
{
  // the checkbox reads "sticky" (on = remember last opacity for new shapes),
  // the conf key is stored inverted (absent/FALSE = sticky, the default) so
  // it needs no preferences.xml entry -- see _new_shape_default_opacity in
  // masks.c, which is the actual place this is consumed.
  const gboolean not_sticky = !gtk_toggle_button_get_active(mi);
  dt_conf_set_bool("plugins/darkroom/masks/opacity_not_sticky", not_sticky);
  if(not_sticky) dt_conf_set_float("plugins/darkroom/masks/opacity", 1.0f);
}

// the channel-preview mode, defined further down with the rest of the hover
// machinery; the panel options menu is built up here
static gboolean _preview_on_hover_is_on(void);
static void _preview_on_hover_set(const gboolean on);

// "auto-expand selected" (masks panel hamburger -> options): whatever is
// selected -- a group, an element, or an element and the group holding it --
// is the one thing expanded, and whatever the option expanded before is
// collapsed. See _auto_expand_selected_row / _auto_expand_selected_group.
static gboolean _auto_expand_selected(void)
{
  return dt_conf_get_bool("plugins/darkroom/masks/auto_expand_selected");
}

// "element properties in subpanel" (same menu): the selected element's or
// group's properties leave the list for a collapsible section of their own
// (see _props_panel_sync)
static gboolean _props_subpanel(void)
{
  return dt_conf_get_bool("plugins/darkroom/masks/properties_subpanel");
}

// "reuse the last picked area" (same menu): a parametric element's range
// picker starts from the area the module's last pick used, and samples it at
// once, as classic blending's single picker did. Its first pick still waits
// for a drag on canvas
static gboolean _param_picker_reuses_area(void)
{
  return dt_conf_get_bool("plugins/darkroom/masks/parametric_picker_reuse_area");
}

static void _masks_picker_reuse_area_toggled(GtkToggleButton *mi, dt_iop_module_t *module)
{
  dt_conf_set_bool("plugins/darkroom/masks/parametric_picker_reuse_area",
                   gtk_toggle_button_get_active(mi));
}

// the expander options below are all read at row-build time from a conf key,
// not from anything dt_masks_gui_list_signature hashes (see _make_props_row_toggle,
// _make_shape_row, the group header build) -- without invalidating the cached
// signature here, toggling one would have no visible effect until something
// unrelated next moved the signature.
static void _masks_rebuild_for_option(dt_iop_module_t *module)
{
  if(!module) return;
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(bd) bd->masks_list_sig = DT_INVALID_HASH;
  _queue_masks_list_rebuild(module);
}

static void _masks_auto_expand_selected_toggled(GtkToggleButton *mi,
                                                dt_iop_module_t *module)
{
  const gboolean on = gtk_toggle_button_get_active(mi);
  dt_conf_set_bool("plugins/darkroom/masks/auto_expand_selected", on);
  // the rebuild below applies the option to every row whose expanded state is
  // pure GUI state (see _make_props_row_toggle's build-time rule), but a
  // parametric row's is its stored in_out field, which only
  // _auto_expand_selected_row knows how to move -- so switching the option on
  // with a parametric element selected has to expand it here and now.
  dt_iop_gui_blend_data_t *bd = module ? module->blend_data : NULL;
  if(on && bd) _auto_expand_selected_row(module, bd->panel_selected_formid);
  _masks_rebuild_for_option(module);
}

static void _masks_props_subpanel_toggled(GtkToggleButton *mi,
                                                dt_iop_module_t *module)
{
  dt_conf_set_bool("plugins/darkroom/masks/properties_subpanel",
                   gtk_toggle_button_get_active(mi));
  // the rebuild refills the subpanel, and shows or hides it (see _props_panel_sync)
  _masks_rebuild_for_option(module);
}

static void _masks_preset_notes_toggled(GtkToggleButton *mi, dt_iop_module_t *module)
{
  dt_conf_set_bool("plugins/darkroom/masks/show_preset_notes",
                   gtk_toggle_button_get_active(mi));
  _masks_rebuild_for_option(module);
}

static void _masks_show_panel_handle_toggled(GtkToggleButton *mi,
                                             dt_iop_module_t *module)
{
  dt_conf_set_bool("plugins/darkroom/masks/show_panel_handle",
                   gtk_toggle_button_get_active(mi));
  // the handle is built once, at view init, so it has to be told
  dt_ui_flexi_panel_update_handle(darktable.gui->ui);
}

static void _masks_preview_on_hover_toggled(GtkToggleButton *mi,
                                            dt_iop_module_t *module)
{
  _preview_on_hover_set(gtk_toggle_button_get_active(mi));
}

// appends the "interface options" and "parametric element options" sections
// to `box`: toggles for how the blend mask panel behaves that don't fit the
// position or colorspace sections. Check buttons under a dt_section_label,
// the way the other toolbar preference popovers are laid out (see
// global_toolbox.c's overlay settings).
#define _MASKS_OPT_CHECK(var, label, tip, active, cb)                             \
  GtkWidget *var = gtk_check_button_new_with_label(label);                        \
  gtk_widget_set_tooltip_text(var, tip);                                          \
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(var), active);                   \
  g_signal_connect(G_OBJECT(var), "toggled", G_CALLBACK(cb), module);             \
  dt_gui_box_add(box, var);

void dt_masks_gui_pref_section(GtkWidget *box, const gchar *title, const gchar *tip)
{
  GtkWidget *lb = gtk_label_new(title);
  gtk_label_set_justify(GTK_LABEL(lb), GTK_JUSTIFY_CENTER);
  dt_gui_add_class(lb, "dt_section_label");
  if(tip) gtk_widget_set_tooltip_text(lb, tip);
  dt_gui_box_add(box, lb);
}

static void _add_masks_panel_options_box(GtkWidget *box, dt_iop_module_t *module)
{
  dt_masks_gui_pref_section(box, _("interface options"),
                            _("how the blend mask panel behaves"));

  _MASKS_OPT_CHECK(
    sticky, _("sticky opacity"),
    _("when enabled (default), a new shape starts at the opacity\n"
      "last used by any shape, so adjusting opacity once carries\n"
      "over to every shape you add afterwards.\n"
      "when disabled, a new shape always starts at 100% opacity."),
    !dt_conf_get_bool("plugins/darkroom/masks/opacity_not_sticky"),
    _masks_opacity_sticky_toggled)

  _MASKS_OPT_CHECK(
    autoexpand, _("auto-expand selected"),
    _("when enabled (default), whatever you select is the one thing\n"
      "expanded, and whatever was expanded before collapses:\n"
      "selecting a group shows its elements, selecting an element\n"
      "shows its controls and its group.\n"
      "selecting something with nothing to expand leaves what is\n"
      "open as it is.\n"
      "when disabled, everything is expanded and collapsed by hand."),
    _auto_expand_selected(), _masks_auto_expand_selected_toggled)

  _MASKS_OPT_CHECK(
    props_subpanel, _("element properties in subpanel"),
    _("when enabled, the properties of the selected element or group\n"
      "are shown in a collapsible section of their own, between the\n"
      "mask list and the refinements, instead of expanding in the list.\n"
      "disabled by default."),
    _props_subpanel(), _masks_props_subpanel_toggled)

  _MASKS_OPT_CHECK(
    showhandle, _("show the mask panel's resize handle"),
    _("when enabled (default), the edge of the mask panel floating\n"
      "over the canvas carries a visible handle with an arrow showing\n"
      "which way the panel folds away.\n"
      "when disabled, the handle is invisible, like the main panels',\n"
      "and still resizes the panel by dragging and hides it on a click."),
    dt_conf_get_bool("plugins/darkroom/masks/show_panel_handle"),
    _masks_show_panel_handle_toggled)

  dt_masks_gui_pref_section(box, _("parametric element options"),
                            _("how parametric elements are picked and previewed"));

  _MASKS_OPT_CHECK(
    pickarea, _("reuse the last picked area"),
    _("when enabled (default), a parametric element's color picker\n"
      "starts from the area the module's last pick used, and sets the\n"
      "range from it at once: pick an area for one channel, then click\n"
      "the picker of the next one to set its range from the same area.\n"
      "drag on the image to pick another area.\n"
      "the first pick in a module still waits for an area dragged on\n"
      "the image.\n"
      "when disabled, every pick waits for an area dragged on the image."),
    _param_picker_reuses_area(), _masks_picker_reuse_area_toggled)

  _MASKS_OPT_CHECK(
    hover, _("preview channel under cursor"),
    _("when enabled, resting the pointer on one of the\n"
      "add-parametric-element buttons displays that channel in the\n"
      "center view, so you can see what a channel looks like before\n"
      "adding an element for it.\n"
      "the preview only starts after a short pause, so passing over\n"
      "the buttons on the way elsewhere costs nothing.\n"
      "disabled by default."),
    _preview_on_hover_is_on(),
    _masks_preview_on_hover_toggled)
}

// the preset notes switch, closing the "default group layout" section that
// dt_masks_gui_add_default_preset_box (masks_gui_presets.c) opens
static void _add_masks_preset_notes_check(GtkWidget *box, dt_iop_module_t *module)
{
  _MASKS_OPT_CHECK(
    notes, _("show preset notes"),
    _("when enabled (default), a group made by a built-in group layout\n"
      "preset carries a note on how to use it, under its header. all\n"
      "notes are open when the preset is applied; after that, only the\n"
      "selected group's. the info icon next to a group's name switches\n"
      "its note on or off without selecting the group.\n"
      "when disabled, no notes are shown."),
    dt_masks_gui_preset_notes_shown(), _masks_preset_notes_toggled)
}
#undef _MASKS_OPT_CHECK

// a radio in `box`, grouped with `group` (NULL starts a new group), carrying
// `data` under `key` for the toggled handler to read back
static GtkWidget *_masks_pref_radio(GtkWidget *box,
                                    GtkWidget *group,
                                    const gchar *label,
                                    const gchar *key,
                                    const int data,
                                    const gboolean active)
{
  GtkWidget *rb = gtk_radio_button_new_with_label_from_widget(
    group ? GTK_RADIO_BUTTON(group) : NULL, label);
  g_object_set_data(G_OBJECT(rb), key, GINT_TO_POINTER(data));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rb), active);
  dt_gui_box_add(box, rb);
  return rb;
}

static void _blendif_colorspace_radio_toggled(GtkToggleButton *rb,
                                              dt_iop_module_t *module)
{
  DT_GUARD_GUI_UPDATE();
  if(!gtk_toggle_button_get_active(rb)) return;
  const dt_develop_blend_colorspace_t cst =
    GPOINTER_TO_INT(g_object_get_data(G_OBJECT(rb), "dt-blend-cst"));
  if(_blendif_change_blend_colorspace(module, cst))
    gtk_widget_queue_draw(module->widget);
}

static gboolean _masks_options_popover_destroy(gpointer pop)
{
  gtk_widget_destroy(GTK_WIDGET(pop));
  g_object_unref(pop);
  return G_SOURCE_REMOVE;
}

// each opening builds a fresh popover on the button, and a closed one only
// hides: drop it once "closed" has finished unmapping it
static void _masks_options_popover_closed(GtkPopover *pop, gpointer user_data)
{
  g_idle_add(_masks_options_popover_destroy, g_object_ref(pop));
}

// the blend mask panel's settings, as a popover of sections like the darkroom
// toolbar's other preference popovers (guides, global toolbox): a
// dt_section_label per section, radios for exclusive choices, check buttons
// for toggles. With no module focused, only the panel-wide settings show
static void _blendif_options_callback(GtkButton *button,
                                      dt_iop_module_t *module)
{
  const dt_iop_gui_blend_data_t *bd = module ? module->blend_data : NULL;
  if(module && !bd) return;

  GtkWidget *pop = gtk_popover_new(GTK_WIDGET(button));
  g_signal_connect(G_OBJECT(pop), "closed", G_CALLBACK(_masks_options_popover_closed), NULL);
  GtkWidget *box = dt_gui_vbox();
  gtk_container_add(GTK_CONTAINER(pop), box);

  dt_masks_gui_pref_section(box, _("blend mask panel settings"), NULL);

  // the blend colorspace, where the module supports parametric masks; the
  // popover also opens on masks-only modules, for the other sections
  const dt_develop_blend_colorspace_t module_cst =
    bd ? dt_develop_blend_default_module_blend_colorspace(module) : DEVELOP_BLEND_CS_NONE;
  if(bd && bd->blendif_support
     && (module_cst == DEVELOP_BLEND_CS_LAB || module_cst == DEVELOP_BLEND_CS_RGB_DISPLAY
         || module_cst == DEVELOP_BLEND_CS_RGB_SCENE))
  {
    const dt_develop_blend_colorspace_t module_blend_cst = module->blend_params->blend_cst;
    dt_masks_gui_pref_section(box, _("blend colorspace"), NULL);

    // every entry here stays live even when the mask holds parametric elements
    // that cannot survive the switch: _blendif_change_blend_colorspace asks
    // about those and deletes them on a yes.
    DT_ENTER_GUI_UPDATE();
    GtkWidget *g = _masks_pref_radio(box, NULL, _("default"), "dt-blend-cst",
                                     DEVELOP_BLEND_CS_NONE,
                                     module_blend_cst == DEVELOP_BLEND_CS_NONE);
    GtkWidget *radios[4] = { g, NULL, NULL, NULL };
    int n = 1;
    // only offer Lab blending on a Lab module, to avoid using it at the wrong
    // place (it should not be active for RGB modules before colorin/after colorout)
    if(module_cst == DEVELOP_BLEND_CS_LAB)
      radios[n++] = _masks_pref_radio(box, g, _("Lab"), "dt-blend-cst",
                                      DEVELOP_BLEND_CS_LAB,
                                      module_blend_cst == DEVELOP_BLEND_CS_LAB);
    radios[n++] = _masks_pref_radio(box, g, _("RGB (display)"), "dt-blend-cst",
                                    DEVELOP_BLEND_CS_RGB_DISPLAY,
                                    module_blend_cst == DEVELOP_BLEND_CS_RGB_DISPLAY);
    radios[n++] = _masks_pref_radio(box, g, _("RGB (scene)"), "dt-blend-cst",
                                    DEVELOP_BLEND_CS_RGB_SCENE,
                                    module_blend_cst == DEVELOP_BLEND_CS_RGB_SCENE);
    DT_LEAVE_GUI_UPDATE();
    // the blend colorspace belongs to the mask: parametric elements are tied
    // to it (see _blendif_change_blend_colorspace)
    const gboolean locked = dt_develop_blend_mask_locked(module->blend_params);
    // connected only after the initial states are set, so building the popover
    // does not look like the user picking a colorspace
    for(int i = 0; i < n; i++)
    {
      g_signal_connect(G_OBJECT(radios[i]), "toggled",
                       G_CALLBACK(_blendif_colorspace_radio_toggled), module);
      gtk_widget_set_sensitive(radios[i], !locked);
    }
  }

  if(!bd || bd->masks_support)
  {
    dt_masks_gui_add_panel_position_box(box, module);
    _add_masks_panel_options_box(box, module);
    dt_masks_gui_add_default_preset_box(box);
    _add_masks_preset_notes_check(box, module);
  }

  gtk_widget_show_all(box);
  gtk_popover_popup(GTK_POPOVER(pop));

  // the anchor is not always a DtGtkButton: this also opens from a right-click
  // on the darkroom toolbar's mask-panel toggle, which is a DtGtkToggleButton
  if(DTGTK_IS_BUTTON(button)) dtgtk_button_set_active(DTGTK_BUTTON(button), FALSE);
}

void dt_iop_gui_blend_masks_options_popup(GtkButton *button, gpointer user_data)
{
  dt_iop_module_t *module = darktable.develop ? darktable.develop->gui_module : NULL;
  if(!module && darktable.develop)
    module = darktable.develop->proxy.masks_flexi_host.hosted_module;
  _blendif_options_callback(button, module);
}

// the parametric row editor owning `widget`, one of its range sliders (tagged
// "param-row-editor", see _build_param_row_editor), with its channel table and
// its channel's index in that table
static dt_masks_param_row_editor_t *_param_row_editor_resolve(
  GtkWidget *widget, const dt_iop_gui_blendif_channel_t **channels_out, int *ch_out)
{
  dt_masks_param_row_editor_t *ed = g_object_get_data(G_OBJECT(widget), "param-row-editor");
  const dt_masks_point_parametric_t *p = ed ? _param_point(ed->formid) : NULL;
  const dt_iop_gui_blendif_channel_t *channels = _param_channels(p);
  if(!channels) return NULL;
  *channels_out = channels;
  *ch_out = (int)p->channel;
  return ed;
}

// resolve the DT_DEV_PIXELPIPE_DISPLAY_* channel bit for a parametric row's
// own slider: each row is single-channel. Returns FALSE (leaving *channel_out
// untouched) if the row/form cannot be resolved, so callers degrade to "no
// channel view" instead of dereferencing anything
static gboolean _param_row_editor_channel(GtkWidget *widget,
                                          dt_dev_pixelpipe_display_mask_t *channel_out)
{
  const dt_iop_gui_blendif_channel_t *channels;
  int ch;
  const dt_masks_param_row_editor_t *ed = _param_row_editor_resolve(widget, &channels, &ch);
  if(!ed) return FALSE;
  dt_dev_pixelpipe_display_mask_t channel = channels[ch].display_channel;
  if(widget == GTK_WIDGET(ed->filter[1].slider))
    channel |= DT_DEV_PIXELPIPE_DISPLAY_OUTPUT;
  *channel_out = channel;
  return TRUE;
}

// toggle channel/mask view
static void _blendop_blendif_channel_mask_view_toggle
  (GtkWidget *widget,
   dt_iop_module_t *module,
   const dt_dev_pixelpipe_display_mask_t mode)
{
  dt_dev_pixelpipe_display_mask_t new_request_mask_display =
    module->request_mask_display;

  // toggle mode
  if(module->request_mask_display & mode)
    new_request_mask_display &= ~mode;
  else
    new_request_mask_display |= mode;

  new_request_mask_display &= ~DT_DEV_PIXELPIPE_DISPLAY_ANY;

  // in case user requests channel display: get the channel
  if(new_request_mask_display & DT_DEV_PIXELPIPE_DISPLAY_CHANNEL)
  {
    dt_dev_pixelpipe_display_mask_t channel;
    if(_param_row_editor_channel(widget, &channel))
    {
      new_request_mask_display &= ~DT_DEV_PIXELPIPE_DISPLAY_ANY;
      new_request_mask_display |= channel;
    }
    else
      new_request_mask_display &= ~DT_DEV_PIXELPIPE_DISPLAY_CHANNEL;
  }

  if(new_request_mask_display != module->request_mask_display)
  {
    module->request_mask_display = new_request_mask_display;
    _refresh_mask_display(module);
  }
}


// "preview channel under cursor" is a latched mode rather than a held key:
// while it is on, hovering an "add channel" button shows that channel on the
// center view, and leaving restores whatever was displayed before. The state is
// global (one working mode, not a per-module setting) so it survives moving
// between modules; it lives in the panel options menu, and
// _shortcut_toggle_preview_on_hover makes it bindable.
#define BLEND_PREVIEW_ON_HOVER_CONF "plugins/darkroom/blend/preview_channel_on_hover"

// how long the pointer rests on a button before the preview starts: each
// preview reprocesses the pipeline, so sweeping across the buttons must not
// start one per button. Keep it long enough to be noticed as a delay, or the
// preview seems to start on contact and the row cannot be crossed without one
#define BLEND_PREVIEW_ON_HOVER_DWELL_MS 500

static gboolean _preview_on_hover_is_on(void)
{
  return dt_conf_get_bool(BLEND_PREVIEW_ON_HOVER_CONF);
}

// drop a pending dwell timer, if any. Must be called from every path that
// tears down or unhovers, or the timer fires into freed blend_data.
static void _preview_on_hover_cancel_dwell(dt_iop_gui_blend_data_t *bd)
{
  if(!bd || !bd->preview_dwell_timer) return;
  g_source_remove(bd->preview_dwell_timer);
  bd->preview_dwell_timer = 0;
}

// bring module->request_mask_display in line with the mode and what is hovered:
// show the hovered channel while both hold, and restore what was displayed
// before as soon as either stops holding
static void _preview_on_hover_apply(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!bd) return;

  // hovered_channel_display is NONE when the widget's channel could not be
  // resolved; there is nothing to preview then
  const gboolean want_preview =
    _preview_on_hover_is_on()
    && bd->hovered_channel_widget
    && bd->hovered_channel_display != DT_DEV_PIXELPIPE_DISPLAY_NONE;

  dt_dev_pixelpipe_display_mask_t wanted;

  dt_pthread_mutex_lock(&bd->lock);
  if(want_preview)
  {
    // first frame of a preview: remember what to come back to
    if(!bd->hover_preview_active) bd->save_for_leave = module->request_mask_display;
    bd->hover_preview_active = TRUE;
    wanted = DT_DEV_PIXELPIPE_DISPLAY_CHANNEL | bd->hovered_channel_display;
  }
  else
  {
    // with no preview up there is nothing of ours to take down: leave
    // request_mask_display to whoever else set it
    wanted = bd->hover_preview_active
               ? bd->save_for_leave
               : module->request_mask_display;
    bd->hover_preview_active = FALSE;
  }
  dt_pthread_mutex_unlock(&bd->lock);

  if(module->request_mask_display != wanted)
  {
    module->request_mask_display = wanted;
    _refresh_mask_display(module);
  }
}

static gboolean _preview_on_hover_dwell_elapsed(gpointer user_data)
{
  dt_iop_module_t *module = user_data;
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!bd) return G_SOURCE_REMOVE;

  bd->preview_dwell_timer = 0;
  _preview_on_hover_apply(module);
  return G_SOURCE_REMOVE;
}

// shared by both hovered widget kinds: the parametric range sliders below and
// the "add channel" buttons in _rebuild_param_channel_buttons, which resolve
// their channel differently. Only the buttons carry a channel to preview; the
// sliders pass NONE and are here purely to register the hover.
static void _preview_on_hover_enter(dt_iop_module_t *module,
                                    GtkWidget *widget,
                                    const dt_dev_pixelpipe_display_mask_t channel)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;

  bd->hovered_channel_widget = widget;
  bd->hovered_channel_display = channel;
  gtk_widget_grab_focus(widget);

  // re-arm on every enter, so sweeping across the button row only ever fires
  // for the button the pointer actually settles on. Whatever was previewed
  // before stays up meanwhile, rather than flickering off and on.
  _preview_on_hover_cancel_dwell(bd);
  if(_preview_on_hover_is_on() && channel != DT_DEV_PIXELPIPE_DISPLAY_NONE)
    bd->preview_dwell_timer = g_timeout_add(BLEND_PREVIEW_ON_HOVER_DWELL_MS,
                                            _preview_on_hover_dwell_elapsed, module);
  else
    _preview_on_hover_apply(module);  // nothing to show: take any preview down now
}

static void _preview_on_hover_leave(dt_iop_module_t *module, GtkWidget *widget)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;

  // a leave for a widget we are no longer tracking would undo the enter that
  // has since taken over (adjacent buttons deliver enter before leave)
  if(bd->hovered_channel_widget != widget) return;

  bd->hovered_channel_widget = NULL;
  _preview_on_hover_cancel_dwell(bd);
  _preview_on_hover_apply(module);
}

// set the mode and let whichever module is hovering an add-channel button
// right now pick the change up without moving the pointer. Shared by the panel
// options menu item and the shortcut action (_shortcut_toggle_preview_on_hover).
static void _preview_on_hover_set(const gboolean on)
{
  dt_conf_set_bool(BLEND_PREVIEW_ON_HOVER_CONF, on);

  for(GList *m = darktable.develop ? darktable.develop->iop : NULL;
      m;
      m = g_list_next(m))
  {
    dt_iop_module_t *mod = m->data;
    dt_iop_gui_blend_data_t *bd = mod->blend_data;
    _preview_on_hover_cancel_dwell(bd);
    _preview_on_hover_apply(mod);
    // the channel buttons say whether resting on them previews, so their
    // tooltips have to be rebuilt when that stops being true
    if(bd && bd->masks_param_channels_inner) _update_add_target_hints(mod);
  }
}

static void _blendop_blendif_enter_cb(GtkEventControllerMotion *controller,
                                      double x, double y,
                                      dt_iop_module_t *module)
{
  DT_GUARD_GUI_UPDATE();

  // the sliders are where the work happens: previewing on the way to grabbing
  // one costs a pipeline reprocess nobody asked for, so they only register the
  // hover (the a/m keys need it, see _blendop_blendif_key_press_cb) and leave
  // the channel unresolved -- _preview_on_hover_apply reads NONE as "nothing to
  // preview". The add-channel buttons are the ones that preview.
  _preview_on_hover_enter(module, dt_gui_get_widget(controller),
                          DT_DEV_PIXELPIPE_DISPLAY_NONE);
}

static void _blendop_blendif_leave_cb(GtkEventControllerMotion *controller,
                                      dt_iop_module_t *module)
{
  DT_GUARD_GUI_UPDATE();

  _preview_on_hover_leave(module, dt_gui_get_widget(controller));
}

static gboolean _blendop_blendif_key_press_cb(GtkEventControllerKey *controller,
                                              guint keyval,
                                              guint keycode,
                                              GdkModifierType state,
                                              dt_iop_module_t *module)
{
  DT_GUARD_GUI_UPDATE(FALSE);

  GtkWidget *widget = dt_gui_get_widget(controller);
  dt_iop_gui_blend_data_t *data = module->blend_data;
  if(data->hovered_channel_widget != widget) return FALSE;
  gboolean handled = FALSE;

  switch(keyval)
  {
    case GDK_KEY_a:
    case GDK_KEY_A:
    {
      const dt_iop_gui_blendif_channel_t *channels;
      int ch;
      const dt_masks_param_row_editor_t *row_ed =
        _param_row_editor_resolve(widget, &channels, &ch);
      // the scale is the slider's own, so a rebuilt row starts linear like
      // its fresh slider does
      if(row_ed && ch >= 0 && channels[ch].altdisplay)
      {
        const int io = (widget == GTK_WIDGET(row_ed->filter[1].slider)) ? 1 : 0;
        channels[ch].altdisplay(widget, module, row_ed->filter[io].altmode + 1);
      }
      handled = TRUE;
      break;
    }
    case GDK_KEY_m:
    case GDK_KEY_M:
      _blendop_blendif_channel_mask_view_toggle
        (widget, module,
         DT_DEV_PIXELPIPE_DISPLAY_MASK);
      handled = TRUE;
      break;
  }

  if(handled)
    dt_iop_request_focus(module);

  return handled;
}


#define COLORSTOPS(gradient) sizeof(gradient) / sizeof(dt_iop_gui_blendif_colorstop_t), \
                             gradient

const dt_iop_gui_blendif_channel_t Lab_channels[]
    = { { N_("L"), N_("add a parametric element selecting by lightness (L)"), 1.0f / 100.0f,
            COLORSTOPS(_gradient_L), TRUE, 0.0f,
          { DEVELOP_BLENDIF_L_in, DEVELOP_BLENDIF_L_out }, DT_DEV_PIXELPIPE_DISPLAY_L,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log,
          N_("lightness") },
        { N_("a"), N_("add a parametric element selecting by green/red (a)"), 1.0f / 256.0f,
          COLORSTOPS(_gradient_a), TRUE, 0.0f,
          { DEVELOP_BLENDIF_A_in, DEVELOP_BLENDIF_A_out }, DT_DEV_PIXELPIPE_DISPLAY_a,
          _blendif_scale_print_ab, _blendop_blendif_disp_alternative_mag,
          N_("green/red") },
        { N_("b"), N_("add a parametric element selecting by blue/yellow (b)"), 1.0f / 256.0f,
          COLORSTOPS(_gradient_b), TRUE, 0.0f,
          { DEVELOP_BLENDIF_B_in, DEVELOP_BLENDIF_B_out }, DT_DEV_PIXELPIPE_DISPLAY_b,
          _blendif_scale_print_ab, _blendop_blendif_disp_alternative_mag,
          N_("blue/yellow") },
        { N_("C"), N_("add a parametric element selecting by saturation (C)"), 1.0f / 100.0f,
          COLORSTOPS(_gradient_chroma),
          TRUE, 0.0f,
          { DEVELOP_BLENDIF_C_in, DEVELOP_BLENDIF_C_out }, DT_DEV_PIXELPIPE_DISPLAY_LCH_C,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log,
          N_("saturation") },
        { N_("h"), N_("add a parametric element selecting by hue (h)"), 1.0f / 360.0f,
          COLORSTOPS(_gradient_LCh_hue),
          FALSE, 0.0f,
          { DEVELOP_BLENDIF_h_in, DEVELOP_BLENDIF_h_out }, DT_DEV_PIXELPIPE_DISPLAY_LCH_h,
          _blendif_scale_print_hue, NULL, N_("hue") },
        { NULL } };

const dt_iop_gui_blendif_channel_t rgb_channels[]
    = { { N_("g"), N_("add a parametric element selecting by gray (g)"), 1.0f / 255.0f,
            COLORSTOPS(_gradient_gray), TRUE, 0.0f,
          { DEVELOP_BLENDIF_GRAY_in, DEVELOP_BLENDIF_GRAY_out },
          DT_DEV_PIXELPIPE_DISPLAY_GRAY,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log,
          N_("gray") },
        { N_("R"), N_("add a parametric element selecting by red (R)"), 1.0f / 255.0f,
          COLORSTOPS(_gradient_red), TRUE, 0.0f,
          { DEVELOP_BLENDIF_RED_in, DEVELOP_BLENDIF_RED_out },
          DT_DEV_PIXELPIPE_DISPLAY_R,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log,
          N_("red") },
        { N_("G"), N_("add a parametric element selecting by green (G)"), 1.0f / 255.0f,
          COLORSTOPS(_gradient_green), TRUE, 0.0f,
          { DEVELOP_BLENDIF_GREEN_in, DEVELOP_BLENDIF_GREEN_out },
          DT_DEV_PIXELPIPE_DISPLAY_G,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log,
          N_("green") },
        { N_("B"), N_("add a parametric element selecting by blue (B)"), 1.0f / 255.0f,
          COLORSTOPS(_gradient_blue), TRUE, 0.0f,
          { DEVELOP_BLENDIF_BLUE_in, DEVELOP_BLENDIF_BLUE_out },
          DT_DEV_PIXELPIPE_DISPLAY_B,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log,
          N_("blue") },
        { N_("H"), N_("add a parametric element selecting by hue (H)"), 1.0f / 360.0f,
          COLORSTOPS(_gradient_HSL_hue),
          FALSE, 0.0f,
          { DEVELOP_BLENDIF_H_in, DEVELOP_BLENDIF_H_out },
          DT_DEV_PIXELPIPE_DISPLAY_HSL_H,
          _blendif_scale_print_hue, NULL,
          N_("hue") },
        { N_("S"), N_("add a parametric element selecting by chroma (S)"), 1.0f / 100.0f,
          COLORSTOPS(_gradient_chroma),
          FALSE, 0.0f,
          { DEVELOP_BLENDIF_S_in, DEVELOP_BLENDIF_S_out },
          DT_DEV_PIXELPIPE_DISPLAY_HSL_S,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log,
          N_("chroma") },
        { N_("L"), N_("add a parametric element selecting by luminance (L)"), 1.0f / 100.0f,
          COLORSTOPS(_gradient_gray),
          FALSE, 0.0f,
          { DEVELOP_BLENDIF_l_in, DEVELOP_BLENDIF_l_out },
          DT_DEV_PIXELPIPE_DISPLAY_HSL_l,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log,
          N_("luminance") },
        { NULL } };

const dt_iop_gui_blendif_channel_t rgbj_channels[]
    = { { N_("g"), N_("add a parametric element selecting by gray (g)"), 1.0f / 255.0f,
            COLORSTOPS(_gradient_gray), TRUE, 0.0f,
          { DEVELOP_BLENDIF_GRAY_in, DEVELOP_BLENDIF_GRAY_out },
          DT_DEV_PIXELPIPE_DISPLAY_GRAY,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log,
          N_("gray") },
        { N_("R"), N_("add a parametric element selecting by red (R)"), 1.0f / 255.0f,
          COLORSTOPS(_gradient_red), TRUE, 0.0f,
          { DEVELOP_BLENDIF_RED_in, DEVELOP_BLENDIF_RED_out },
          DT_DEV_PIXELPIPE_DISPLAY_R,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log,
          N_("red") },
        { N_("G"), N_("add a parametric element selecting by green (G)"), 1.0f / 255.0f,
          COLORSTOPS(_gradient_green), TRUE, 0.0f,
          { DEVELOP_BLENDIF_GREEN_in, DEVELOP_BLENDIF_GREEN_out },
          DT_DEV_PIXELPIPE_DISPLAY_G,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log,
          N_("green") },
        { N_("B"), N_("add a parametric element selecting by blue (B)"), 1.0f / 255.0f,
          COLORSTOPS(_gradient_blue), TRUE, 0.0f,
          { DEVELOP_BLENDIF_BLUE_in, DEVELOP_BLENDIF_BLUE_out },
          DT_DEV_PIXELPIPE_DISPLAY_B,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log,
          N_("blue") },
        { N_("Jz"), N_("add a parametric element selecting by luminance (Jz)"), 1.0f / 100.0f,
          COLORSTOPS(_gradient_gray),
          TRUE, -6.64385619f, // cf. _blend_init_blendif_boost_parameters
          { DEVELOP_BLENDIF_Jz_in, DEVELOP_BLENDIF_Jz_out },
          DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_Jz,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log,
          N_("luminance") },
        { N_("Cz"), N_("add a parametric element selecting by chroma (Cz)"), 1.0f / 100.0f,
          COLORSTOPS(_gradient_chroma),
          TRUE, -6.64385619f, // cf. _blend_init_blendif_boost_parameters
          { DEVELOP_BLENDIF_Cz_in, DEVELOP_BLENDIF_Cz_out },
          DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_Cz,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log,
          N_("chroma") },
        { N_("hz"), N_("add a parametric element selecting by hue (hz)"), 1.0f / 360.0f,
          COLORSTOPS(_gradient_JzCzhz_hue),
          FALSE, 0.0f,
          { DEVELOP_BLENDIF_hz_in, DEVELOP_BLENDIF_hz_out },
          DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_hz,
          _blendif_scale_print_hue, NULL,
          N_("hue") },
        { NULL } };

// the channel descriptor array for a blend colorspace (NULL-terminated), used
// by the parametric elements (add buttons, row editors) to enumerate and index
// channels. Exported (see blend.h) so parametric.c can name a form after its
// channel without duplicating the per-colorspace arrays above
const dt_iop_gui_blendif_channel_t *dt_develop_blendif_channels_for_csp(const int csp)
{
  switch(csp)
  {
  case DEVELOP_BLEND_CS_LAB: return Lab_channels;
  case DEVELOP_BLEND_CS_RGB_DISPLAY: return rgb_channels;
  case DEVELOP_BLEND_CS_RGB_SCENE: return rgbj_channels;
  default: return NULL;
  }
}

const char *slider_tooltip[] =
  { N_("adjustment based on input image received by this module:\n"
       "- upper markers: full opacity (100% mask)\n"
       "- lower markers: zero opacity (0% mask)\n"
       "- between upper/lower markers: opacity transition\n\n"
       "drag marker to adjust\n"
       "right-click marker for precise numeric entry\n"
       "double-click to reset\n"
       "press 'm' to toggle mask view\n"
       "press 'a' to toggle display modes"),
    N_("adjustment based on unblended output of this module:\n"
       "- upper markers: full opacity (100% mask)\n"
       "- lower markers: zero opacity (0% mask)\n"
       "- between upper/lower markers: opacity transition\n\n"
       "drag marker to adjust\n"
       "right-click marker for precise numeric entry\n"
       "double-click to reset\n"
       "press 'm' to toggle mask view\n"
       "press 'a' to toggle display modes") };

static void _rebuild_param_channel_buttons(dt_iop_module_t *module);

void dt_iop_gui_update_masks(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  dt_develop_blend_params_t *bp = module->blend_params;

  if(!bd || !bd->masks_support || !bd->masks_inited) return;

  DT_ENTER_GUI_UPDATE();

  /* update masks state */
  const gboolean flexi = bp->mask_mode & DEVELOP_MASK_FLEXI;
  dt_masks_form_t *grp =
    dt_masks_get_from_id(darktable.develop, module->blend_params->mask_id);
  // classic drawn mode has nothing to edit on canvas without shapes
  if(!flexi && !(grp && (grp->type & DT_MASKS_GROUP) && grp->points))
  {
    bd->masks_shown = DT_MASKS_EDIT_OFF;
    dt_masks_set_edit_mode(module, DT_MASKS_EDIT_OFF);
  }

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit),
                               bd->masks_shown != DT_MASKS_EDIT_OFF);

  // the button of the shape being drawn stays armed
  const dt_masks_form_t *pending = _pending_form(module);
  for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_shapes[n]),
                                 pending && (pending->type & bd->masks_type[n]));

  DT_LEAVE_GUI_UPDATE();

  // a panel/history/image update may have swapped the mask group out from under
  // us (and does not go through dt_masks_gui_build_list): retarget the
  // refinement controls
  _refine_section_refresh(module);
}

// ===========================================================================
// the mask list
// ---------------------------------------------------------------------------
// one row per element of the module's mask, grouped, each with its operator,
// invert and visibility controls, reorderable by drag and drop. A parametric
// element carries its own channel editor (see _build_param_row_editor)

// rebuilds the whole list, the widget a drop just landed on included. Never
// call it from a "drag-data-received" handler, only from the idle: on macOS
// gtk_drag_finish() returns before Cocoa is done with the source view, and
// destroying it then can abort a later drag (in _gdk_quartz_window_drag_begin)
static gboolean _rebuild_masks_list_idle(gpointer user_data)
{
  dt_iop_module_t *module = user_data;
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  // cleared before rebuilding, so that a request raised by the rebuild queues
  // another pass instead of being dropped
  if(bd)
  {
    bd->masks_rebuild_pending = FALSE;
    bd->masks_rebuild_idle_id = 0;
  }
  dt_masks_gui_build_list(module);
  return G_SOURCE_REMOVE;
}

// queue one deferred rebuild for all the requests of a main-loop turn: one
// gesture can raise several (a drop that reorders and reselects). The source
// id is kept for dt_iop_gui_cleanup_blending to remove: run after the module
// is gone, the idle would touch destroyed widgets
static void _queue_masks_list_rebuild(dt_iop_module_t *module)
{
  // without blend data there is no list to rebuild, and an idle that no
  // teardown could cancel would outlive the module
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!bd || bd->masks_rebuild_pending) return;
  bd->masks_rebuild_pending = TRUE;
  bd->masks_rebuild_idle_id = g_idle_add(_rebuild_masks_list_idle, (gpointer)module);
}

// the modules sharing a form all show its chain icon, so a change of who uses
// it (import, unlink, delete) must reach their panels too. Their signatures
// fold the users (see dt_masks_gui_list_signature), so the unaffected ones skip
static void _queue_link_peers_rebuild(const dt_iop_module_t *module)
{
  for(GList *l = darktable.develop->iop; l; l = g_list_next(l))
  {
    dt_iop_module_t *m = l->data;
    if(m != module && m->blend_data) _queue_masks_list_rebuild(m);
  }
}

// defined below, with the solo and solo-edit model halves
static void _toggle_soloedit(dt_iop_module_t *module, const dt_mask_id_t id);
static void _toggle_element_disable(dt_iop_module_t *module, const dt_mask_id_t id);

static gboolean _form_has_drawn_shape(const dt_masks_form_t *form, const int depth)
{
  if(!form || depth > DT_MASKS_NESTING_MAX) return FALSE;
  if(form->type & (DT_MASKS_CIRCLE | DT_MASKS_PATH | DT_MASKS_GRADIENT | DT_MASKS_ELLIPSE
                   | DT_MASKS_BRUSH
#ifdef HAVE_AI
                   | DT_MASKS_OBJECT
#endif
     ))
    return TRUE;

  if(form->type & DT_MASKS_GROUP)
  {
    for(const GList *l = form->points; l; l = g_list_next(l))
    {
      const dt_masks_point_group_t *pt = l->data;
      const dt_masks_form_t *child = dt_masks_get_from_id(darktable.develop, pt->formid);
      if(_form_has_drawn_shape(child, depth + 1)) return TRUE;
    }
  }
  return FALSE;
}

static gboolean _module_has_drawn_shapes(const dt_iop_module_t *module)
{
  if(!module || !module->blend_params) return FALSE;
  const dt_masks_form_t *grp =
    dt_masks_get_from_id(darktable.develop, module->blend_params->mask_id);
  return _form_has_drawn_shape(grp, 0);
}

dt_masks_form_t *dt_masks_gui_module_mask_group(dt_iop_module_t *module)
{
  if(!module || !module->blend_params) return NULL;
  dt_masks_form_t *grp =
    dt_masks_get_from_id(darktable.develop, module->blend_params->mask_id);
  return (grp && (grp->type & DT_MASKS_GROUP)) ? grp : NULL;
}

// A flexi mask with no group form yet shows one empty group all the same (see
// _masks_panel_pack). The form, with that group's marker, is created here, the
// first time something is written to the group, and not when the panel merely
// shows it: only a masks history item records a new form, and a module's own
// history item would record a mask id naming nothing. Callers commit with the
// module, so its mask id is recorded too.
dt_masks_form_t *dt_masks_gui_module_flexi_group(dt_iop_module_t *module, dt_mask_id_t *cid)
{
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  if(!grp && module && darktable.develop)
    grp = dt_masks_module_group_create(darktable.develop, module);
  if(!grp) return NULL;
  if(cid && !dt_is_valid_maskid(*cid) && grp->points)
    *cid = ((dt_masks_point_group_t *)grp->points->data)->formid;
  return grp;
}

// does the mask have elements, not just groups?
static gboolean _mask_has_elements(const dt_masks_form_t *grp)
{
  for(const GList *l = grp ? grp->points : NULL; l; l = g_list_next(l))
    if(!dt_masks_point_is_marker(l->data)) return TRUE;
  return FALSE;
}

// the AI object the canvas is stepped into, or INVALID_MASKID
static dt_mask_id_t _entered_object(void)
{
  const dt_masks_form_gui_t *gui = darktable.develop ? darktable.develop->form_gui : NULL;
  return gui ? gui->entered_object : INVALID_MASKID;
}

// the paths of AI object `obj`, its marker aside
static int _object_path_count(const dt_masks_form_t *obj)
{
  int n = 0;
  for(const GList *l = obj ? obj->points : NULL; l; l = g_list_next(l))
    if(!dt_masks_point_is_marker(l->data)) n++;
  return n;
}

// the list node of point `id` -- a member or a marker -- of the mask `grp`, at
// any depth, and the group form whose list holds it. The list of `grp` itself
// is read directly first, so the paths of an AI object, whose form is no
// group, are found too
static GList *_point_node_owner(dt_masks_form_t *grp,
                                const dt_mask_id_t id,
                                dt_masks_form_t **owner)
{
  for(GList *l = grp ? grp->points : NULL; l; l = g_list_next(l))
    if(((dt_masks_point_group_t *)l->data)->formid == id)
    {
      if(owner) *owner = grp;
      return l;
    }
  GList *found = grp && (grp->type & DT_MASKS_GROUP)
                   ? dt_masks_group_find_node(darktable.develop ? darktable.develop->forms
                                                                : NULL,
                                              grp, id, owner)
                   : NULL;
  // an AI object is no group, so the walk above does not enter it. The one
  // stepped into shows its paths as rows of its own group, which act on their
  // points like any other rows (see _make_shape_row)
  const dt_mask_id_t entered = _entered_object();
  if(!found && dt_is_valid_maskid(entered) && entered != id && grp
     && grp->formid != entered)
  {
    dt_masks_form_t *obj = dt_masks_get_from_id(darktable.develop, entered);
    if(obj && _point_node_owner(grp, entered, NULL))
      found = _point_node_owner(obj, id, owner);
  }
  return found;
}

// the node of point `pt` itself at any depth, with the list holding it. A mask
// can hold the same shape twice, so where a row knows its own reference this
// finds that one, not the first with its form id as _point_node_owner does.
// `pt` is only compared, never read
static GList *_point_node_at(dt_masks_form_t *grp,
                             const dt_masks_point_group_t *pt,
                             dt_masks_form_t **owner,
                             const int depth)
{
  if(!grp || !pt || depth > DT_MASKS_NESTING_MAX) return NULL;
  for(GList *l = grp->points; l; l = g_list_next(l))
    if(l->data == pt)
    {
      if(owner) *owner = grp;
      return l;
    }
  for(GList *l = grp->points; l; l = g_list_next(l))
  {
    const dt_masks_point_group_t *p = l->data;
    if(dt_masks_point_is_marker(p)) continue;
    dt_masks_form_t *f = dt_masks_get_from_id(darktable.develop, p->formid);
    if(f && f != grp && (f->type & (DT_MASKS_GROUP | DT_MASKS_OBJECT)))
    {
      GList *n = _point_node_at(f, pt, owner, depth + 1);
      if(n) return n;
    }
  }
  return NULL;
}

dt_masks_point_group_t *dt_masks_gui_group_point(dt_masks_form_t *grp, const dt_mask_id_t id)
{
  GList *node = _point_node_owner(grp, id, NULL);
  return node ? node->data : NULL;
}

static void _mask_points_into(dt_masks_form_t *grp, GList **out, const int depth)
{
  if(!grp || depth > DT_MASKS_NESTING_MAX) return;
  for(GList *l = grp->points; l; l = g_list_next(l))
  {
    dt_masks_point_group_t *pt = l->data;
    *out = g_list_prepend(*out, pt);
    if(dt_masks_point_is_marker(pt)) continue;
    dt_masks_form_t *f = dt_masks_get_from_id(darktable.develop, pt->formid);
    if(f && f != grp && (f->type & DT_MASKS_GROUP)) _mask_points_into(f, out, depth + 1);
  }
}

// every point of the mask `grp` at any depth, bottom-up, a nested group's
// right after the member that holds it. Free the list, not the points
static GList *_mask_points(dt_masks_form_t *grp)
{
  GList *out = NULL;
  _mask_points_into(grp, &out, 0);
  return g_list_reverse(out);
}

// how many times this module's own mask references form `fid`, at any depth.
// One mask can hold the same shape twice (a group linking a shape another
// group defines), which dt_masks_model_form_users cannot report: it counts a module
// once however many of its members point at the form
static int _model_form_uses_in_mask(dt_iop_module_t *module, const dt_mask_id_t fid)
{
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  if(!grp || !dt_is_valid_maskid(fid)) return 0;
  int n = 0;
  GList *pts = _mask_points(grp);
  for(GList *l = pts; l; l = g_list_next(l))
  {
    const dt_masks_point_group_t *pt = l->data;
    if(!dt_masks_point_is_marker(pt) && pt->formid == fid) n++;
  }
  g_list_free(pts);
  return n;
}

// a member point and the index of its form in the flattened copy of the mask
// the canvas edits (dt_masks_group_ungroup)
typedef struct _canvas_point_t
{
  dt_masks_point_group_t *pt;
  int pos;
} _canvas_point_t;

static void _canvas_points_into(dt_masks_form_t *grp, GArray *out, int *pos, const int depth)
{
  if(!grp || depth > DT_MASKS_NESTING_MAX) return;
  for(GList *l = grp->points; l; l = g_list_next(l))
  {
    dt_masks_point_group_t *pt = l->data;
    // a marker, or a member whose form is gone, has no form in the copy
    dt_masks_form_t *f = dt_masks_get_from_id(darktable.develop, pt->formid);
    if(!f) continue;
    const _canvas_point_t cp = { pt, *pos };
    g_array_append_val(out, cp);
    // a nested group or an AI object is replaced by its forms in the copy,
    // taking no index of its own
    if(f->type & (DT_MASKS_GROUP | DT_MASKS_OBJECT))
    {
      if(f != grp) _canvas_points_into(f, out, pos, depth + 1);
    }
    else
      (*pos)++;
  }
}

// every member point of the mask `grp` at any depth, in the canvas copy's
// order, with its form's index there. Free with g_array_free(a, TRUE)
static GArray *_canvas_points(dt_masks_form_t *grp)
{
  GArray *out = g_array_new(FALSE, FALSE, sizeof(_canvas_point_t));
  int pos = 0;
  _canvas_points_into(grp, out, &pos, 0);
  return out;
}

// is `id` one of the member ids `formids`, or a point of a nested group among
// them, at any depth?
static gboolean _members_hold(GList *formids, const dt_mask_id_t id)
{
  if(!dt_is_valid_maskid(id)) return FALSE;
  for(GList *l = formids; l; l = g_list_next(l))
  {
    const dt_mask_id_t fid = GPOINTER_TO_INT(l->data);
    if(fid == id) return TRUE;
    dt_masks_form_t *f = dt_masks_get_from_id(darktable.develop, fid);
    if(f && (f->type & DT_MASKS_GROUP) && _point_node_owner(f, id, NULL)) return TRUE;
  }
  return FALSE;
}

// a group's user-given name, or NULL if it has none: held by its marker
static const char *_group_custom_name(dt_masks_form_t *grp, const dt_mask_id_t cid)
{
  const dt_masks_point_group_t *pt = dt_masks_gui_group_point(grp, cid);
  return (pt && pt->name[0]) ? pt->name : NULL;
}

// ===========================================================================
// scoped mask refinement
//
// The refinement controls (details, feathering guide and radius, blur,
// brightness, contrast) act on the scope the list selection implies (see
// dt_masks_model_refine_scope_from_selection):
//   - GLOBAL    : blend_params->{details,...}, applied once to the finished
//                 mask. Nothing selected, or the mask's own group
//   - GROUP     : the selected group's refinement, held by its marker
//   - ELEMENT   : the selected element's own point refinement
// The per-point scopes use the per-shape refinement storage (masks v7) and its
// renderer hook (group.c, dt_develop_blend_refine_form_mask): a zero-filled
// (disabled) refinement renders exactly as no refinement.
// The REFINE_SCOPE_* values live in blend_gui_internal.h.

// enum values in the same order as dt_develop_feathering_guide_names[], so a
// combo index maps to the stored uint32 guide value (and back).
static const uint32_t _refine_guide_values[] = { DEVELOP_MASK_GUIDE_OUT_BEFORE_BLUR,
                                                 DEVELOP_MASK_GUIDE_IN_BEFORE_BLUR,
                                                 DEVELOP_MASK_GUIDE_OUT_AFTER_BLUR,
                                                 DEVELOP_MASK_GUIDE_IN_AFTER_BLUR };

// A flexi group's list holds its marker (see DT_MASKS_STATE_GROUP_MARKER)
// first, then its members, bottom-up

// the list node of point `id` -- a member or a marker -- or NULL
static GList *_point_node(dt_masks_form_t *grp, const dt_mask_id_t id)
{
  return _point_node_owner(grp, id, NULL);
}

// the group form whose list holds point `id`, a member or the group's own
// marker, or NULL
static dt_masks_form_t *_group_of(dt_masks_form_t *grp, const dt_mask_id_t id)
{
  dt_masks_form_t *owner = NULL;
  return _point_node_owner(grp, id, &owner) ? owner : NULL;
}

// put the points of `pts` (bottom-up, none of them in the list) right after
// node `at`, or at the bottom of the list for NULL, in their order
static void _insert_points_after(dt_masks_form_t *grp, GList *at, GList *pts)
{
  for(GList *l = pts; l; l = g_list_next(l))
  {
    grp->points = g_list_insert_before(grp->points, at ? at->next : grp->points, l->data);
    at = at ? at->next : grp->points;
  }
}

// the same for the one point `pt`, which then belongs to group `grp`
static void _insert_point_after(dt_masks_form_t *grp, GList *at, dt_masks_point_group_t *pt)
{
  pt->parentid = grp->formid;
  grp->points = g_list_insert_before(grp->points, at ? at->next : grp->points, pt);
}

GList *dt_masks_model_group_members(dt_masks_form_t *grp, const dt_mask_id_t id)
{
  dt_masks_form_t *owner = _group_of(grp, id);
  GList *out = NULL;
  for(GList *l = owner ? owner->points->next : NULL; l; l = g_list_next(l))
    out =
      g_list_prepend(out, GINT_TO_POINTER(((dt_masks_point_group_t *)l->data)->formid));
  return out;
}

dt_mask_id_t dt_masks_gui_group_cid_of_form(dt_masks_form_t *grp, const dt_mask_id_t fid)
{
  dt_masks_form_t *owner = _group_of(grp, fid);
  return owner ? ((dt_masks_point_group_t *)owner->points->data)->formid : INVALID_MASKID;
}

// is the group member `fid` is in bypassed, or a group holding that one at any
// depth? Its members then render nothing
static gboolean _member_group_bypassed(dt_masks_form_t *grp, const dt_mask_id_t fid)
{
  dt_mask_id_t id = fid;
  for(int depth = 0; depth <= DT_MASKS_NESTING_MAX; depth++)
  {
    dt_masks_form_t *owner = _group_of(grp, id);
    if(!owner) return FALSE;
    if(((dt_masks_point_group_t *)owner->points->data)->state & DT_MASKS_STATE_OP_BYPASS)
      return TRUE;
    if(owner == grp) return FALSE;
    id = owner->formid;
  }
  return FALSE;
}

// how many levels of nested groups the member form `f` brings: 0 for a shape
static int _form_nesting(const dt_masks_form_t *f, const int depth)
{
  if(!f || !(f->type & DT_MASKS_GROUP) || depth > DT_MASKS_NESTING_MAX) return 0;
  int deepest = 0;
  for(const GList *l = f->points; l; l = g_list_next(l))
  {
    const dt_masks_point_group_t *pt = l->data;
    if(dt_masks_point_is_marker(pt)) continue;
    const dt_masks_form_t *c = dt_masks_get_from_id(darktable.develop, pt->formid);
    if(c != f) deepest = MAX(deepest, _form_nesting(c, depth + 1));
  }
  return 1 + deepest;
}

// how many nested groups hold the list of `owner`: 0 for the mask's own
static int _list_depth(dt_masks_form_t *grp, dt_masks_form_t *owner)
{
  int depth = 0;
  while(owner && owner != grp && depth <= DT_MASKS_NESTING_MAX)
  {
    dt_masks_form_t *up = NULL;
    if(!_point_node_owner(grp, owner->formid, &up)) break;
    owner = up;
    depth++;
  }
  return depth;
}

// may the member `fid` move from the list of `from` into the list of `to`?
// A nested group never into itself or below itself, and nothing deeper than a
// walk of the mask follows (DT_MASKS_NESTING_MAX), unless it is no deeper than
// it already was
static gboolean _may_move_into(dt_masks_form_t *grp,
                               dt_masks_form_t *from,
                               dt_masks_form_t *to,
                               const dt_mask_id_t fid)
{
  if(from == to) return TRUE;
  dt_masks_form_t *f = dt_masks_get_from_id(darktable.develop, fid);
  if(f && (f->type & DT_MASKS_GROUP) && (f == to || _point_node_owner(f, to->formid, NULL)))
    return FALSE;
  const int n = _form_nesting(f, 0);
  const int depth = _list_depth(grp, to) + n;
  return depth <= DT_MASKS_NESTING_MAX || depth <= _list_depth(grp, from) + n;
}

// a new nested group form with no points, named and added to the image's
// forms. Named here: a group form has no set_form_name, which
// dt_masks_assign_unique_name needs
static dt_masks_form_t *_new_nested_group(void)
{
  dt_develop_t *dev = darktable.develop;
  dt_masks_form_t *sub = dt_masks_create(DT_MASKS_GROUP);
  if(!sub || !dev) return sub;
  int nb = 0;
  for(GList *l = dev->forms; l; l = g_list_next(l))
    if(((dt_masks_form_t *)l->data)->type == DT_MASKS_GROUP) nb++;
  gboolean taken;
  do
  {
    taken = FALSE;
    snprintf(sub->name, sizeof(sub->name), _("group #%d"), ++nb);
    for(GList *l = dev->forms; l && !taken; l = g_list_next(l))
      taken = !strcmp(((dt_masks_form_t *)l->data)->name, sub->name);
  } while(taken);
  dev->forms = g_list_append(dev->forms, sub);
  return sub;
}

// a plain reference to `sub` in the list of `owner`, right after node `at`
static void _add_nested_ref(dt_masks_form_t *owner, GList *at, dt_masks_form_t *sub)
{
  dt_masks_point_group_t *pt = calloc(1, sizeof(dt_masks_point_group_t));
  pt->formid = sub->formid;
  pt->state = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE | DT_MASKS_STATE_UNION;
  pt->opacity = 1.0f;
  pt->group_opacity = 1.0f;
  _insert_point_after(owner, at, pt);
}

// a new nested group folding with `flexi_op`, holding nothing but its marker
static dt_masks_form_t *_new_empty_nested_group(const dt_masks_state_t flexi_op)
{
  dt_masks_form_t *sub = _new_nested_group();
  if(!sub) return NULL;
  sub->points = g_list_append(NULL, dt_masks_marker_new(darktable.develop->forms, sub,
                                                        flexi_op & DT_MASKS_STATE_FLEXI_OP));
  return sub;
}

dt_mask_id_t dt_masks_model_nest_new_group(dt_masks_form_t *grp,
                                           const dt_masks_state_t flexi_op,
                                           const dt_mask_id_t cid)
{
  dt_masks_form_t *owner = _group_of(grp, cid);
  if(!owner || _list_depth(grp, owner) + 1 > DT_MASKS_NESTING_MAX) return INVALID_MASKID;
  dt_masks_form_t *sub = _new_empty_nested_group(flexi_op);
  if(!sub) return INVALID_MASKID;
  _add_nested_ref(owner, g_list_last(owner->points), sub);
  return ((dt_masks_point_group_t *)sub->points->data)->formid;
}

// the marker of an empty group on top of the list of `owner`: what "compose"
// gives the user to fill
static dt_mask_id_t _add_empty_on_top(dt_masks_form_t *owner)
{
  dt_masks_form_t *e = _new_empty_nested_group(0);
  if(!e) return INVALID_MASKID;
  _add_nested_ref(owner, g_list_last(owner->points), e);
  return ((dt_masks_point_group_t *)e->points->data)->formid;
}

dt_mask_id_t dt_masks_model_compose(dt_masks_form_t *grp,
                                    const dt_masks_point_group_t *pt,
                                    const dt_masks_state_t flexi_op)
{
  dt_masks_form_t *owner = NULL;
  GList *node = _point_node_at(grp, pt, &owner, 0);
  // the paths of an AI object move as the object
  if(!node || !(owner->type & DT_MASKS_GROUP) || (owner->type & DT_MASKS_OBJECT))
    return INVALID_MASKID;

  if(owner == grp && dt_masks_point_is_marker(node->data))
  {
    // the mask's own group stays the mask: its members and settings move into
    // a new group at its bottom, and it starts over with `flexi_op` and none
    if(_form_nesting(grp, 0) > DT_MASKS_NESTING_MAX) return INVALID_MASKID;
    dt_masks_point_group_t *root = node->data;
    dt_masks_form_t *sub = _new_nested_group();
    if(!sub) return INVALID_MASKID;
    dt_masks_point_group_t *mk = calloc(1, sizeof(dt_masks_point_group_t));
    memcpy(mk, root, sizeof(dt_masks_point_group_t));
    mk->formid = dt_masks_new_marker_id(darktable.develop->forms);
    mk->parentid = sub->formid;
    // a bypassed mask offers no compose, and the bypass is the mask's anyway
    mk->state &= ~DT_MASKS_STATE_OP_DISABLE;
    GList *members = node->next;
    node->next = NULL;
    if(members) members->prev = NULL;
    for(GList *l = members; l; l = g_list_next(l))
      ((dt_masks_point_group_t *)l->data)->parentid = sub->formid;
    sub->points = g_list_prepend(members, mk);
    root->state = (root->state & ~(DT_MASKS_STATE_FLEXI_OP | DT_MASKS_STATE_OP_INVERT))
                  | (flexi_op & DT_MASKS_STATE_FLEXI_OP);
    root->group_opacity = 1.0f;
    memset(&root->refinement, 0, sizeof(root->refinement));
    _add_nested_ref(grp, node, sub);
    return _add_empty_on_top(grp);
  }

  // a group is composed through the reference its holder has to it
  if(dt_masks_point_is_marker(node->data))
  {
    const dt_mask_id_t gid = owner->formid;
    node = _point_node_owner(grp, gid, &owner);
    if(!node) return INVALID_MASKID;
  }
  const dt_masks_point_group_t *ref = node->data;
  const int depth = _list_depth(grp, owner) + 1;
  const int below = _form_nesting(dt_masks_get_from_id(darktable.develop, ref->formid), 0);
  if(depth + MAX(1, below) > DT_MASKS_NESTING_MAX) return INVALID_MASKID;

  // the reference moves as it is, keeping its own settings, into a new group
  // that takes its place and applies nothing
  dt_masks_form_t *wrap = _new_empty_nested_group(flexi_op);
  if(!wrap) return INVALID_MASKID;
  _add_nested_ref(owner, node, wrap);
  owner->points = g_list_remove_link(owner->points, node);
  ((dt_masks_point_group_t *)node->data)->parentid = wrap->formid;
  wrap->points = g_list_concat(wrap->points, node);
  return _add_empty_on_top(wrap);
}

// the one member of the mask's own group, when that is a plain nested group
// nothing else holds, or NULL
static dt_masks_form_t *_sole_held_group(dt_masks_form_t *grp, dt_masks_point_group_t **ref)
{
  if(!grp || !grp->points || !dt_masks_point_is_marker(grp->points->data)
     || !grp->points->next || grp->points->next->next)
    return NULL;
  *ref = grp->points->next->data;
  dt_masks_form_t *sub = dt_masks_get_from_id(darktable.develop, (*ref)->formid);
  if(!sub || sub == grp || !(sub->type & DT_MASKS_GROUP)
     || (sub->type & (DT_MASKS_CLONE | DT_MASKS_OBJECT)) || !sub->points
     || !dt_masks_point_is_marker(sub->points->data))
    return NULL;
  // another module's mask keeps its members whoever holds it
  for(GList *m = darktable.develop->iop; m; m = g_list_next(m))
  {
    const dt_iop_module_t *mod = m->data;
    if(mod->blend_params && mod->blend_params->mask_id == sub->formid) return NULL;
  }
  int refs = 0;
  for(GList *f = darktable.develop->forms; f; f = g_list_next(f))
  {
    const dt_masks_form_t *g = f->data;
    if(!(g->type & DT_MASKS_GROUP)) continue;
    for(GList *l = g->points; l; l = g_list_next(l))
      if(((dt_masks_point_group_t *)l->data)->formid == sub->formid) refs++;
  }
  return refs == 1 ? sub : NULL;
}

gboolean dt_masks_model_hoist_sole_group(dt_masks_form_t *grp, dt_masks_refinement_t *whole)
{
  dt_masks_point_group_t *ref = NULL;
  dt_masks_form_t *sub = _sole_held_group(grp, &ref);
  if(!sub) return FALSE;
  dt_masks_point_group_t *root = grp->points->data;
  dt_masks_point_group_t *mk = sub->points->data;
  // the mask's own group applies nothing, and the reference nothing
  if((root->state & (DT_MASKS_STATE_OP_DISABLE | DT_MASKS_STATE_OP_INVERT))
     || root->group_opacity != 1.0f || root->refinement.enabled != DT_MASKS_REFINE_OFF
     || ref->opacity != 1.0f || ref->refinement.enabled != DT_MASKS_REFINE_OFF
     || (ref->state & (DT_MASKS_STATE_INVERSE | DT_MASKS_STATE_HIDDEN | DT_MASKS_STATE_DISABLE)))
    return FALSE;
  // the mask's own group takes no name, and the group's bypass has nowhere to go
  if(mk->name[0] || (mk->state & DT_MASKS_STATE_OP_DISABLE)) return FALSE;
  // the group's refinement becomes the whole mask's, which runs after the
  // mask's invert and opacity where the group's ran before its own
  const gboolean refined = mk->refinement.enabled != DT_MASKS_REFINE_OFF;
  if(refined
     && (whole->enabled || (mk->state & DT_MASKS_STATE_OP_INVERT) || mk->group_opacity != 1.0f))
    return FALSE;

  root->state = (root->state & ~(DT_MASKS_STATE_FLEXI_OP | DT_MASKS_STATE_OP_INVERT))
                | (mk->state & (DT_MASKS_STATE_FLEXI_OP | DT_MASKS_STATE_OP_INVERT));
  root->group_opacity = mk->group_opacity;
  if(refined) *whole = mk->refinement;
  free(ref);
  grp->points = g_list_delete_link(grp->points, grp->points->next);
  GList *members = sub->points->next;
  sub->points->next = NULL;
  if(members) members->prev = NULL;
  for(GList *l = members; l; l = g_list_next(l))
    ((dt_masks_point_group_t *)l->data)->parentid = grp->formid;
  grp->points = g_list_concat(grp->points, members);
  return TRUE;
}

GList *dt_masks_model_empty_group(dt_masks_form_t *grp, const dt_mask_id_t cid)
{
  dt_masks_form_t *owner = _group_of(grp, cid);
  GList *ids = NULL;
  for(GList *l = owner ? owner->points->next : NULL; l;)
  {
    GList *next = g_list_next(l);
    ids = g_list_prepend(ids, GINT_TO_POINTER(((dt_masks_point_group_t *)l->data)->formid));
    free(l->data);
    owner->points = g_list_delete_link(owner->points, l);
    l = next;
  }
  return g_list_reverse(ids);
}

// read the six refinement controls into r. enabled is derived from whether any
// effective parameter is non-neutral, so committing an all-neutral refinement
// leaves enabled == 0 and the renderer keeps its byte-identical fast path.
static void _refine_read_controls(dt_iop_gui_blend_data_t *bd, dt_masks_refinement_t *r)
{
  r->details = dt_bauhaus_slider_get(bd->details_slider);
  const int gi = dt_bauhaus_combobox_get(bd->masks_feathering_guide_combo);
  r->feathering_guide =
    (gi >= 0 && gi < 4) ? _refine_guide_values[gi] : DEVELOP_MASK_GUIDE_OUT_BEFORE_BLUR;
  r->feathering_radius = dt_bauhaus_slider_get(bd->feathering_radius_slider);
  r->blur_radius = dt_bauhaus_slider_get(bd->blur_radius_slider);
  r->contrast = dt_bauhaus_slider_get(bd->contrast_slider);
  r->brightness = dt_bauhaus_slider_get(bd->brightness_slider);
  r->enabled = (r->details != 0.0f || r->feathering_radius != 0.0f
                || r->blur_radius != 0.0f || r->contrast != 0.0f || r->brightness != 0.0f)
                 ? 1
                 : 0;
}

// push a refinement struct into the six controls without triggering commits.
static void _refine_set_controls(dt_iop_gui_blend_data_t *bd,
                                 const dt_masks_refinement_t *r)
{
  bd->masks_refine_updating = TRUE;
  dt_bauhaus_slider_set(bd->details_slider, r->details);
  int gi = 0;
  for(int i = 0; i < 4; i++)
    if(_refine_guide_values[i] == r->feathering_guide)
    {
      gi = i;
      break;
    }
  dt_bauhaus_combobox_set(bd->masks_feathering_guide_combo, gi);
  dt_bauhaus_slider_set(bd->feathering_radius_slider, r->feathering_radius);
  dt_bauhaus_slider_set(bd->blur_radius_slider, r->blur_radius);
  dt_bauhaus_slider_set(bd->brightness_slider, r->brightness);
  dt_bauhaus_slider_set(bd->contrast_slider, r->contrast);
  bd->masks_refine_updating = FALSE;
}

// is any module-wide refinement actually set? (so "reset mask" can skip
// committing a history item when there is nothing to clear)
static gboolean _refine_global_is_set(const dt_iop_module_t *module)
{
  const dt_develop_blend_params_t *bp = module->blend_params;
  return bp->details != 0.0f || bp->feathering_radius != 0.0f || bp->blur_radius != 0.0f
         || bp->brightness != 0.0f || bp->contrast != 0.0f;
}

// the module's own ("whole mask") refinement, as a group holds one
static dt_masks_refinement_t _refine_of_module(dt_iop_module_t *module)
{
  const dt_develop_blend_params_t *bp = module->blend_params;
  dt_masks_refinement_t r = { 0 };
  r.enabled = _refine_global_is_set(module) ? DT_MASKS_REFINE_GROUP : DT_MASKS_REFINE_OFF;
  r.details = bp->details;
  r.feathering_guide = bp->feathering_guide;
  r.feathering_radius = bp->feathering_radius;
  r.blur_radius = bp->blur_radius;
  r.brightness = bp->brightness;
  r.contrast = bp->contrast;
  return r;
}

// the reverse: the module's own refinement from `r`
static void _refine_set_global(dt_iop_module_t *module, const dt_masks_refinement_t *r)
{
  dt_develop_blend_params_t *bp = module->blend_params;
  bp->details = r->details;
  bp->feathering_guide = r->feathering_guide;
  bp->feathering_radius = r->feathering_radius;
  bp->blur_radius = r->blur_radius;
  bp->brightness = r->brightness;
  bp->contrast = r->contrast;
}

// clear the module-wide ("whole mask") refinement back to neutral, in the
// caller's blend_params. Returns TRUE if `details` was non-zero: crossing it to
// zero has to rebuild the scharr-derived detail mask, which an ordinary history
// item does not force, so the caller owes a dt_dev_reprocess_all.
// Shared by the refinement panel's own reset button and "reset mask": the two
// must clear exactly the same fields, or resetting the mask leaves a
// whole-mask refinement behind that nothing on screen still accounts for.
static gboolean _refine_clear_global(dt_iop_module_t *module)
{
  const gboolean had_details = module->blend_params->details != 0.0f;
  const dt_masks_refinement_t neutral = { .feathering_guide = _refine_guide_values[0] };
  _refine_set_global(module, &neutral);
  return had_details;
}

// load the six controls from whatever scope is currently active.
static void _refine_populate(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  dt_masks_refinement_t r = { 0 };

  if(bd->masks_refine_scope_kind == REFINE_SCOPE_GLOBAL)
    r = _refine_of_module(module);
  else
  {
    dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
    // GROUP reads the group's marker; ELEMENT reads that one specific form
    // directly
    const dt_masks_point_group_t *src = dt_masks_gui_group_point(grp, bd->masks_refine_scope_formid);
    // ...but only when the stored value belongs to the scope being shown. One
    // member's point holds either its own element refinement or a broadcast copy
    // of its group's (see dt_masks_refine_scope_t); showing one in the other's
    // controls would report a refinement this scope does not have, and the next
    // slider move would rewrite it into the wrong scope.
    if(src)
    {
      const gboolean want_group = bd->masks_refine_scope_kind == REFINE_SCOPE_GROUP;
      const gboolean is_group = src->refinement.enabled == DT_MASKS_REFINE_GROUP;
      if(src->refinement.enabled == DT_MASKS_REFINE_OFF || want_group == is_group)
        r = src->refinement;
    }
  }
  _refine_set_controls(bd, &r);
}

static void _refine_update_header(dt_iop_module_t *module);

static inline gpointer _refine_scope_key(dt_iop_gui_blend_data_t *bd)
{
  if(!bd) return GUINT_TO_POINTER(0);
  if(bd->masks_refine_scope_kind == REFINE_SCOPE_ELEMENT)
    return GUINT_TO_POINTER(dt_masks_refine_key_element(bd->masks_refine_scope_formid));
  else if(bd->masks_refine_scope_kind == REFINE_SCOPE_GROUP)
    return GUINT_TO_POINTER(dt_masks_refine_key_group(bd->masks_refine_scope_formid));
  else
    return GUINT_TO_POINTER(DT_MASKS_REFINE_KEY_GLOBAL);
}

static const char *const _masks_section_collapsed_key[DT_MASKS_SECTION_COUNT] = {
  "plugins/darkroom/masks/refinements_collapsed",
  "plugins/darkroom/masks/properties_collapsed",
  "plugins/darkroom/masks/consumers_collapsed",
};

gboolean dt_masks_model_section_expanded(const dt_masks_section_t section, const gboolean drawing)
{
  // the creation controls sit in the properties
  if(section == DT_MASKS_SECTION_PROPS && drawing) return TRUE;
  return !dt_conf_get_bool(_masks_section_collapsed_key[section]);
}

void dt_masks_model_section_save(const dt_masks_section_t section, const gboolean expanded)
{
  dt_conf_set_bool(_masks_section_collapsed_key[section], !expanded);
}

// TRUE while _section_set flips a section's toggle itself, so that
// _section_toggled does not take it for a click
static gboolean _section_applying = FALSE;

static void _section_widgets(dt_iop_gui_blend_data_t *bd, const dt_masks_section_t section,
                             GtkWidget **toggle, GtkWidget **expander, GtkWidget **content)
{
  switch(section)
  {
    case DT_MASKS_SECTION_REFINE:
      *toggle = bd->masks_refine_toggle_btn;
      *expander = bd->masks_refine_expander;
      *content = GTK_WIDGET(bd->masks_refine_sliders_box);
      break;
    case DT_MASKS_SECTION_PROPS:
      *toggle = bd->props_panel_toggle_btn;
      *expander = bd->props_panel_expander;
      *content = bd->props_panel_content;
      break;
    default:
      *toggle = bd->consumers_toggle_btn;
      *expander = bd->consumers_expander;
      *content = bd->consumers_content;
      break;
  }
}

// fold or unfold a section without saving it as the section's state
static void _section_set(dt_iop_gui_blend_data_t *bd, const dt_masks_section_t section,
                         const gboolean expanded)
{
  GtkWidget *toggle = NULL, *expander = NULL, *content = NULL;
  if(bd) _section_widgets(bd, section, &toggle, &expander, &content);
  if(!toggle || !expander || !content) return;

  _section_applying = TRUE;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), expanded);
  _section_applying = FALSE;
  dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(toggle), dtgtk_cairo_paint_solid_arrow,
                               expanded ? CPF_DIRECTION_DOWN : CPF_DIRECTION_LEFT, NULL);
  if(dtgtk_expander_get_expanded(DTGTK_EXPANDER(expander)) != expanded)
    dtgtk_expander_set_expanded(DTGTK_EXPANDER(expander), expanded);
  gtk_widget_set_visible(content, expanded);
  gtk_widget_queue_resize(content);
  // an open section's header squares off onto its card, as an open group's
  // header does onto its rail
  GtkWidget *head = dtgtk_expander_get_header(DTGTK_EXPANDER(expander));
  if(expanded)
    dt_gui_add_class(head, "dt_masks_open");
  else
    dt_gui_remove_class(head, "dt_masks_open");
}

static void _section_apply(dt_iop_gui_blend_data_t *bd, const dt_masks_section_t section)
{
  _section_set(bd, section, dt_masks_model_section_expanded(section, FALSE));
}

static void _sections_apply(dt_iop_gui_blend_data_t *bd)
{
  for(dt_masks_section_t s = 0; s < DT_MASKS_SECTION_COUNT; s++) _section_apply(bd, s);
}

// a click on a section's toggle folds that section in every module, since
// several modules can show their mask panel at once
static void _section_toggled(GtkToggleButton *btn, gpointer user_data)
{
  if(_section_applying) return;
  const dt_masks_section_t section = GPOINTER_TO_INT(user_data);
  dt_masks_model_section_save(section, gtk_toggle_button_get_active(btn));
  for(GList *l = darktable.develop ? darktable.develop->iop : NULL; l; l = g_list_next(l))
    _section_apply(((dt_iop_module_t *)l->data)->blend_data, section);
}

// a click on a section's title folds it, as its arrow `toggle` does
static void _section_header_clicked(GtkGestureSingle *gesture,
                                    gint n_press,
                                    gdouble x,
                                    gdouble y,
                                    GtkWidget *toggle)
{
  if(dt_gui_current_button(gesture) != GDK_BUTTON_PRIMARY) return;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle),
                               !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggle)));
}

// a collapsible section under the mask list: a header bar holding `label`
// and, on the right, the fold arrow (*toggle_out), above `content`. The
// selection panel's sections center their title on the bar whatever sits
// beside it; the others lead with it. The header goes in *head_out, for the
// caller to add its own buttons to
static GtkWidget *_section_new(const dt_masks_section_t section,
                               GtkWidget *label,
                               const char *toggle_tip,
                               const gboolean selection,
                               GtkWidget *content,
                               GtkWidget **head_out,
                               GtkWidget **toggle_out)
{
  GtkWidget *head = dt_gui_hbox();
  gtk_box_set_spacing(GTK_BOX(head), DT_BAUHAUS_SPACE);
  dt_gui_add_class(head, "dt_section_expander");
  dt_gui_add_class(head, "dt_masks_section");
  if(selection) dt_gui_add_class(head, "dt_masks_selection_section");

  GtkWidget *toggle =
    dtgtk_togglebutton_new(dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_DOWN, NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), TRUE);
  dt_gui_add_class(toggle, "dt_ignore_fg_state");
  dt_gui_add_class(toggle, "dt_transparent_background");
  gtk_widget_set_tooltip_text(toggle, toggle_tip);
  g_signal_connect(G_OBJECT(toggle), "toggled", G_CALLBACK(_section_toggled),
                   GINT_TO_POINTER(section));

  GtkWidget *label_evb = gtk_event_box_new();
  gtk_container_add(GTK_CONTAINER(label_evb), label);
  dt_gui_connect_click(label_evb, _section_header_clicked, NULL, toggle);
  if(selection)
    gtk_box_set_center_widget(GTK_BOX(head), label_evb);
  else
    dt_gui_box_add(head, dt_gui_expand(label_evb));
  gtk_box_pack_end(GTK_BOX(head), toggle, FALSE, FALSE, 0);

  GtkWidget *expander = dtgtk_expander_new(head, content);
  dtgtk_expander_set_expanded(DTGTK_EXPANDER(expander), TRUE);
  gtk_widget_set_name(expander, "collapse-block");

  if(head_out) *head_out = head;
  *toggle_out = toggle;
  return expander;
}

static void _refine_bypass_toggled(GtkToggleButton *btn, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  dt_iop_gui_blend_data_t *bd = module ? module->blend_data : NULL;
  if(!bd || bd->masks_refine_updating) return;

  const gboolean bypassed = gtk_toggle_button_get_active(btn);
  gpointer key = _refine_scope_key(bd);
  // pipe workers copy the set in commit_params (dt_masks_refine_bypass_commit)
  dt_pthread_mutex_lock(&bd->lock);
  if(!bd->masks_refine_bypassed)
    bd->masks_refine_bypassed = g_hash_table_new(g_direct_hash, g_direct_equal);
  g_hash_table_insert(bd->masks_refine_bypassed, key, GINT_TO_POINTER(bypassed));
  dt_pthread_mutex_unlock(&bd->lock);

  _update_refine_sensitivity(module);

  if(module->dev)
  {
    dt_dev_reprocess_all(module->dev);
    dt_control_queue_redraw();
  }
}

// the element hovered on the canvas, if it still exists (see
// bd->canvas_hovered_formid)
static dt_mask_id_t _canvas_hovered(const dt_iop_gui_blend_data_t *bd)
{
  return dt_is_valid_maskid(bd->canvas_hovered_formid)
             && dt_masks_get_from_id(darktable.develop, bd->canvas_hovered_formid)
           ? bd->canvas_hovered_formid
           : INVALID_MASKID;
}

// flexi: the refinement controls follow the list selection: a selected shape
// (or parametric/raster element) targets only that one element; a selected
// group header (no specific element within it) targets the whole group;
// nothing selected targets global. An element hovered on the canvas takes the
// selection's place. Defined here so _update_row_selection (above the scope
// helpers) can drive it.
void dt_masks_model_refine_scope_from_selection(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  const gboolean flexi = !(module->blend_params->mask_mode & DEVELOP_MASK_RASTER);
  const dt_mask_id_t hovered = _canvas_hovered(bd);
  if(flexi && dt_is_valid_maskid(hovered))
  {
    bd->masks_refine_scope_kind = REFINE_SCOPE_ELEMENT;
    bd->masks_refine_scope_formid = hovered;
  }
  else if(flexi && dt_is_valid_maskid(bd->panel_selected_formid))
  {
    bd->masks_refine_scope_kind = REFINE_SCOPE_ELEMENT;
    bd->masks_refine_scope_formid = bd->panel_selected_formid;
  }
  // the mask's own group refines the whole mask: the module-wide refinement
  // every migrated edit keeps, applied after the mask is rendered. Its marker's
  // own group refinement is not reachable from the panel
  else if(flexi && dt_is_valid_maskid(bd->panel_selected_group_cid)
          && bd->panel_selected_group_cid != _mask_group_cid(module))
  {
    bd->masks_refine_scope_kind = REFINE_SCOPE_GROUP;
    bd->masks_refine_scope_formid = bd->panel_selected_group_cid;
  }
  else
  {
    bd->masks_refine_scope_kind = REFINE_SCOPE_GLOBAL;
    bd->masks_refine_scope_formid = INVALID_MASKID;
  }
}

// drop a refinement scope whose target was removed by a route that does not
// reselect (canvas, an AI object losing its last path, undo), so that the
// caption does not keep naming it
gboolean dt_masks_model_refine_scope_prune(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  const gboolean scope_gone =
    (bd->masks_refine_scope_kind == REFINE_SCOPE_ELEMENT
     || bd->masks_refine_scope_kind == REFINE_SCOPE_GROUP)
    && !dt_masks_gui_group_point(grp, bd->masks_refine_scope_formid);
  if(!scope_gone) return FALSE;
  if(!dt_masks_gui_group_point(grp, bd->panel_selected_formid))
    bd->panel_selected_formid = INVALID_MASKID;
  if(!dt_masks_gui_group_point(grp, bd->panel_selected_group_cid))
    bd->panel_selected_group_cid = INVALID_MASKID;
  return TRUE;
}

// retarget from the selection, then reload the refinement controls for it
static void _flexi_refine_follow_selection(dt_iop_gui_blend_data_t *bd)
{
  if(!bd || !bd->blend_inited || !bd->module) return;
  dt_masks_model_refine_scope_from_selection(bd->module);
  _refine_populate(bd->module);
  // a row click changes the scope without rebuilding the list, so the
  // caption follows here
  _refine_update_header(bd->module);
  _update_refine_sensitivity(bd->module);
}

// commit a control change in GLOBAL scope, into the module-wide blend params.
// details crossing away from 0 needs a full reprocess (the detail mask has to be
// built), and a feathering or blur edit moves feather_version off 0
static void _refine_commit_global(dt_iop_gui_blend_data_t *bd, GtkWidget *w)
{
  dt_develop_blend_params_t *bp = bd->module->blend_params;

  if(w == bd->details_slider)
  {
    const float oldval = bp->details;
    bp->details = dt_bauhaus_slider_get(w);
    // refining is editing the mask, so it switches the mask on. After the
    // field is written, never before: enabling runs a gui update that would
    // repaint the control from the params and lose the value just read
    _blendop_mask_enable(bd->module);
    dt_dev_add_history_item(darktable.develop, bd->module, TRUE);
    if((oldval == 0.0f) && (bp->details != 0.0f))
    {
      dt_dev_reprocess_all(bd->module->dev);
      dt_control_queue_redraw();
    }
    return;
  }

  if(w == bd->masks_feathering_guide_combo)
  {
    const int gi = dt_bauhaus_combobox_get(w);
    if(gi >= 0 && gi < 4) bp->feathering_guide = _refine_guide_values[gi];
  }
  else if(w == bd->feathering_radius_slider)
  {
    bp->feathering_radius = dt_bauhaus_slider_get(w);
    if(bp->feather_version == 0) bp->feather_version = 1;
  }
  else if(w == bd->blur_radius_slider)
  {
    bp->blur_radius = dt_bauhaus_slider_get(w);
    if(bp->feather_version == 0) bp->feather_version = 1;
  }
  else if(w == bd->brightness_slider)
    bp->brightness = dt_bauhaus_slider_get(w);
  else if(w == bd->contrast_slider)
    bp->contrast = dt_bauhaus_slider_get(w);

  _blendop_mask_enable(bd->module);
  dt_dev_add_history_item(darktable.develop, bd->module, TRUE);
}

// commit a control change in a non-global (per-form) scope: write the refinement
// into the targeted point(s) and persist via a masks history item.
static void _refine_commit_nonglobal(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  if(!grp) return;

  dt_masks_refinement_t r = { 0 };
  _refine_read_controls(bd, &r);
  // stamp which mask this refinement is for (see dt_masks_refine_scope_t)
  if(r.enabled)
    r.enabled = (bd->masks_refine_scope_kind == REFINE_SCOPE_GROUP)
                  ? DT_MASKS_REFINE_GROUP
                  : DT_MASKS_REFINE_ELEMENT;

  // REFINE_SCOPE_GROUP writes the group's marker, ELEMENT the one element
  dt_masks_point_group_t *pt = dt_masks_gui_group_point(grp, bd->masks_refine_scope_formid);
  if(pt) pt->refinement = r;

  // reachable with the mask off: switching it off leaves its shapes in place,
  // so their refinements stay editable, and editing one switches it back on
  _blendop_mask_enable(module);
  dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
}

// shared value-changed handler for all six refinement controls.
static void _refine_control_changed(GtkWidget *w, dt_iop_gui_blend_data_t *bd)
{
  if(DT_IN_GUI_UPDATE() || !bd || !bd->blend_inited || bd->masks_refine_updating) return;
  if(bd->masks_refine_scope_kind == REFINE_SCOPE_GLOBAL)
    _refine_commit_global(bd, w);
  else
    _refine_commit_nonglobal(bd->module);
  _refine_update_header(bd->module);
}

// reset the refinement of the current scope back to neutral. For GLOBAL this
// clears blend_params (see _refine_clear_global); for the per-form scopes the
// controls are zeroed and the neutral (enabled == 0) refinement is committed
static void _refine_reset_clicked(GtkWidget *btn, dt_iop_gui_blend_data_t *bd)
{
  if(DT_IN_GUI_UPDATE() || !bd || !bd->blend_inited || bd->masks_refine_updating) return;

  // neutral refinement: all magnitudes zero, guide back to its first value
  const dt_masks_refinement_t r = { .feathering_guide = _refine_guide_values[0] };
  _refine_set_controls(bd, &r); // updates the six controls, guarded (no commit)

  if(bd->masks_refine_scope_kind == REFINE_SCOPE_GLOBAL)
  {
    const gboolean had_details = _refine_clear_global(bd->module);
    // the same rule the sliders follow, so both refinement scopes behave alike
    // here: _refine_commit_nonglobal below switches the mask on too
    _blendop_mask_enable(bd->module);
    dt_dev_add_history_item(darktable.develop, bd->module, TRUE);
    // details crossing to zero needs the same full reprocess the slider path does
    if(had_details)
    {
      dt_dev_reprocess_all(bd->module->dev);
      dt_control_queue_redraw();
    }
  }
  else
    _refine_commit_nonglobal(bd->module);

  _refine_update_header(bd->module);
}

static gboolean _icon_widget_draw(GtkWidget *w, cairo_t *cr, gpointer user_data)
{
  DTGTKCairoPaintIconFunc paint = (DTGTKCairoPaintIconFunc)user_data;
  if(!paint) return FALSE;
  GtkAllocation a;
  gtk_widget_get_allocation(w, &a);
  GdkRGBA c;
  GtkStyleContext *ctx = gtk_widget_get_style_context(w);
  const GtkStateFlags state = gtk_widget_get_state_flags(w);
  gtk_style_context_get_color(ctx, state, &c);
  cairo_set_source_rgba(cr, c.red, c.green, c.blue, c.alpha * 0.85);
  paint(cr, 0, 0, a.width, a.height, 0, NULL);
  return FALSE;
}

static GtkWidget *_make_icon_widget(DTGTKCairoPaintIconFunc paint)
{
  GtkWidget *da = gtk_drawing_area_new();
  gtk_widget_set_size_request(da, DT_PIXEL_APPLY_DPI(16), DT_PIXEL_APPLY_DPI(16));
  gtk_widget_set_valign(da, GTK_ALIGN_CENTER);
  g_signal_connect(G_OBJECT(da), "draw", G_CALLBACK(_icon_widget_draw), (gpointer)paint);
  return da;
}

// what a section acts on, as the list shows it: the icon and name of an
// element (scope REFINE_SCOPE_ELEMENT) or group (REFINE_SCOPE_GROUP), or the
// mask's own operator icon and "whole mask". The name is plain text. Caller
// frees it, and owns the icon, NULL when there is none
static gchar *_target_describe(dt_iop_module_t *module,
                               const int scope,
                               const dt_mask_id_t id,
                               GtkWidget **icon)
{
  *icon = NULL;
  if(scope == REFINE_SCOPE_ELEMENT)
  {
    dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, id);
    if(!form) return g_strdup(_("shape"));
    if(form->type & DT_MASKS_PARAMETRIC)
    {
      const gchar *code = dt_masks_parametric_type_label(form);
      if(code) *icon = _make_channel_handle(code, NULL);
    }
    else
    {
      DTGTKCairoPaintIconFunc paint = _kind_icon_paint(_form_kind(form));
      if(paint) *icon = _make_icon_widget(paint);
    }
    // the name as its row shows it: the icon already says the type
    return dt_masks_gui_form_display_name(form);
  }
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  if(scope == REFINE_SCOPE_GROUP)
  {
    const dt_masks_point_group_t *head = dt_masks_gui_group_point(grp, id);
    *icon = _make_icon_widget(_flexi_op_paint(head ? (head->state & DT_MASKS_STATE_FLEXI_OP) : 0));
    const char *custom_name = _group_custom_name(grp, id);
    return custom_name ? g_strdup(custom_name)
                       : g_strdup_printf("%s-%d", _flexi_op_short_name(head ? head->state : 0),
                                         dt_masks_gui_group_ordinal_of_cid(module, id));
  }
  // the mask is its own top group, so it carries a combine operator like any
  // other, and the header shows it the way the list's own root row does (see
  // the ghandle in _pack_group)
  const dt_masks_point_group_t *root = dt_masks_gui_group_point(grp, _mask_group_cid(module));
  *icon = _make_icon_widget(_flexi_op_paint(root ? (root->state & DT_MASKS_STATE_FLEXI_OP) : 0));
  return g_strdup(_("whole mask"));
}

// show a target in a section header's icon box and name label
static void _target_show(GtkWidget *icon_box, GtkWidget *label, GtkWidget *icon,
                         const gchar *name)
{
  if(icon_box)
  {
    dt_gui_container_destroy_children(GTK_CONTAINER(icon_box));
    if(icon)
    {
      dt_gui_box_add(icon_box, icon);
      gtk_widget_show_all(icon_box);
    }
  }
  else if(icon)
    gtk_widget_destroy(icon);
  if(label) gtk_label_set_text(GTK_LABEL(label), name ? name : "");
}

// the row opening the selection panel: the selection's icon and name,
// centered (see _target_show). The sections under it act on it
static GtkWidget *_selection_row_new(GtkWidget **icon_box, GtkWidget **name_label)
{
  *icon_box = dt_gui_hbox();
  gtk_widget_set_valign(*icon_box, GTK_ALIGN_CENTER);

  *name_label = gtk_label_new(NULL);
  // the middle, as the list's rows do, so that a module's instance name stays
  gtk_label_set_ellipsize(GTK_LABEL(*name_label), PANGO_ELLIPSIZE_MIDDLE);
  dt_gui_add_class(*name_label, "dt_masks_selection_name");

  GtkWidget *row = dt_gui_hbox(*icon_box, *name_label);
  gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
  dt_gui_add_class(row, "dt_masks_selection_row");
  return row;
}

// the selection panel's row names what the panel holds: the shape being
// drawn, else the selection the refinement follows (see
// dt_masks_model_refine_scope_from_selection). A shape being drawn has no
// refinement yet, and the one the selection has is not what the row names,
// so the refinement is disabled meanwhile
static void _selection_row_update(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module ? module->blend_data : NULL;
  if(!bd) return;

  GtkWidget *icon_w = NULL;
  gchar *name = NULL;
  const char *tip = NULL;
  const dt_masks_form_t *pending = _pending_form(module);
  if(bd->masks_refine_expander)
    gtk_widget_set_sensitive(bd->masks_refine_expander, !pending);
  if(pending)
  {
    const guint kind = _form_kind(pending);
    DTGTKCairoPaintIconFunc paint = _kind_icon_paint(kind);
    if(paint) icon_w = _make_icon_widget(paint);
    // as the pending row names it (see _make_pending_shape_row)
    name = g_strdup_printf(_("new %s"), _kind_name(kind, FALSE));
    tip = _("the shape being drawn: the sections below show its creation controls");
  }
  else
  {
    name = _target_describe(module, bd->masks_refine_scope_kind,
                            bd->masks_refine_scope_formid, &icon_w);
    tip =bd->masks_refine_scope_kind == REFINE_SCOPE_ELEMENT
            ? _("the element hovered on the canvas, or else the selected one: the sections"
                " below show and change its properties and refinements")
          : bd->masks_refine_scope_kind == REFINE_SCOPE_GROUP
            ? _("the element hovered on the canvas, or else the selected group: the"
                " sections below show and change its properties and refinements")
            : _("the element hovered on the canvas, or else the whole mask while no"
                " element or group is selected: the sections below show and change its"
                " properties and refinements");
  }
  _target_show(bd->masks_selection_icon_box, bd->masks_selection_name_label, icon_w, name);
  // on the row, the icon included
  if(bd->masks_selection_name_label)
    gtk_widget_set_tooltip_text(gtk_widget_get_parent(bd->masks_selection_name_label), tip);
  g_free(name);
}

// the refinement's header and the selection row naming what it refines,
// after the selection (see dt_masks_model_refine_scope_from_selection)
static void _refine_update_header(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module ? module->blend_data : NULL;
  if(!bd) return;

  _selection_row_update(module);

  // Update bypass button state
  gpointer key = _refine_scope_key(bd);
  gboolean bypassed = FALSE;
  if(bd->masks_refine_bypassed)
    bypassed = GPOINTER_TO_INT(g_hash_table_lookup(bd->masks_refine_bypassed, key));
  bd->masks_refine_updating = TRUE;
  if(bd->masks_refine_bypass_btn)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_refine_bypass_btn),
                                 bypassed);
  bd->masks_refine_updating = FALSE;

  // nothing to reset or disable without a refinement, which is how the header
  // says there is none. The disable keeps its state, for when one comes back
  dt_masks_refinement_t r = { 0 };
  _refine_read_controls(bd, &r);
  const gboolean has_refinement = (r.enabled != 0);
  if(bd->masks_refine_reset_btn)
    gtk_widget_set_sensitive(bd->masks_refine_reset_btn, has_refinement);
  if(bd->masks_refine_bypass_btn)
    gtk_widget_set_sensitive(bd->masks_refine_bypass_btn, has_refinement);
}

// refresh the refinement section for the scope the selection implies: the
// reset button's visibility (flexi only), the header and the sliders
static void _refine_section_refresh(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!bd) return;

  if(bd->masks_refine_reset_btn)
    gtk_widget_set_visible(bd->masks_refine_reset_btn,
                           module->blend_params->mask_mode & DEVELOP_MASK_FLEXI);

  _flexi_refine_follow_selection(bd);
}

// defined with the other badge helpers (it needs the row lookups), but
// called from _props_row_apply below on every opacity change
static void _refresh_lowop_badges(dt_iop_module_t *module);
static void _update_blend_opacity_badge(GtkWidget *badge, const float opacity);
static void
_set_badge_active(GtkWidget *badge, gboolean active, const char *tooltip_when_active);
// what a row's visibility button shows (see _make_visibility_button): solo
// and disable are mutually exclusive, so one button holds either
enum
{
  MASK_VISIBILITY_SHOWN = 0,
  MASK_VISIBILITY_SOLO,
  MASK_VISIBILITY_DISABLED,
};
static void _set_visibility_status(GtkWidget *btn, int status);

// ===========================================================================
// the rows' properties expanders
//
// shapes and raster elements have a toggle next to their solo-edit slot,
// groups one in their header, and a parametric row's in/out chevron reveals
// its opacity too. Edits go through modify_property's delta protocol, with an
// explicit target. The table below holds each property's range and what it
// does; the double-click reset is described where the control is built
// (_build_props_row_editor)
static const struct
{
  gchar *name;
  gchar *format;
  float min, max;
  gboolean relative;
  gboolean boolean;
  gchar *tooltip;
} _blend_masks_properties[DT_MASKS_PROPERTY_LAST] = {
  [DT_MASKS_PROPERTY_OPACITY] = { N_("opacity"), "%", 0, 1, FALSE, FALSE,
    N_("how strongly this element counts in the mask") },
  [DT_MASKS_PROPERTY_SIZE] = { N_("size"), "%", 0.0001, 1, TRUE, FALSE,
    N_("size of the shape") },
  [DT_MASKS_PROPERTY_HARDNESS] = { N_("hardness"), "%", 0.0001, 1, TRUE, FALSE,
    N_("how much of the brush stroke's width is at full opacity\n"
       "before it starts to fade out") },
  [DT_MASKS_PROPERTY_FEATHER] = { N_("fade-out border"), "%", 0.0001, 1, TRUE, FALSE,
    N_("width of the soft edge around the shape, over which\n"
       "it fades from full opacity to none") },
  [DT_MASKS_PROPERTY_ROTATION] = { N_("rotation"), "°", 0, 360, FALSE, FALSE,
    N_("rotation of the shape") },
  [DT_MASKS_PROPERTY_CURVATURE] = { N_("curvature"), "%", -1, 1, FALSE, FALSE,
    N_("how much the gradient's line bends: 0 is straight") },
  [DT_MASKS_PROPERTY_COMPRESSION] = { N_("compression"), "%", 0.0001, 1, TRUE, FALSE,
    N_("width of the gradient's transition from full opacity\n"
       "to none: lower makes it sharper") },
  [DT_MASKS_PROPERTY_CLEANUP] = { N_("cleanup"), "", 0, 100, FALSE, FALSE,
    N_("discards small, stray outline fragments below this size\n"
       "(in traced pixels)") },
  [DT_MASKS_PROPERTY_SMOOTHING] = { N_("smoothing"), "", 0, 1.3, FALSE, FALSE,
    N_("how closely the traced outline follows the AI selection's\n"
       "raw edge: higher gives smoother, more rounded corners") },
  [DT_MASKS_PROPERTY_REFINE] = { N_("refine mask boundary"), "", 0, 1, FALSE, TRUE,
    N_("snap the AI selection's edge to the edges in the image\n"
       "applies from the next click on the object") },
};

// does an element row of this kind carry an expander of its own?
//
// Every element has at least its opacity to show, as a slider leading its
// expanded controls (a header never shows opacity), and a parametric row's
// in/out chevron is its expander. "element properties in subpanel" moves all
// of that to its own section, leaving only a parametric row's in/out chevron,
// which shows its input and output sliders. Groups never come through here:
// their chevron reveals their members, not properties (see
// _group_expand_toggled).
gboolean dt_masks_model_row_is_expandable(const dt_masks_type_t type,
                                          const gboolean props_subpanel)
{
  if(type & DT_MASKS_PARAMETRIC) return TRUE;
  return !props_subpanel;
}

// refresh the compact value label that follows an opacity slider after a
// programmatic (and therefore signal-less) set; defined further below,
// alongside the label itself.
static void _refresh_inline_opacity_label(GtkWidget *slider);

// both defined below, next to the row index they read: the rows showing one
// form, and the refresh that pushes an edit made in one of them into the rest
static GSList *_masks_rows_for_form(dt_iop_gui_blend_data_t *bd,
                                    const dt_mask_id_t formid);
static void _refresh_sibling_prop_rows(dt_iop_module_t *module,
                                       GList *formids,
                                       GtkWidget *src);

// TRUE while a properties control commits its own edit: the commit reaches
// dt_iop_gui_blend_masks_changed, whose re-read must leave alone the slider
// the user is dragging
static gboolean _props_committing = FALSE;

// apply a property's new value to every form in `target_formids` (owned by
// the caller). modify_property takes old and new value and derives its ratio
// or delta, so *last_value must be this control's last committed value. The
// slider shows the targets' mean, and an edit moves each by the same delta or
// ratio, so that a group's members keep their differences. The canvas follows
// live (dt_masks_gui_form_create), through each shape's position in the
// canvas copy of the group

static void _props_row_apply(dt_iop_module_t *module,
                             GList *target_formids,
                             const int prop,
                             GtkWidget *widget,
                             float *last_value,
                             const gboolean allow_hide)
{
  dt_develop_t *dev = darktable.develop;
  dt_masks_form_gui_t *gui = dev->form_gui;
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  const gboolean is_bool = _blend_masks_properties[prop].boolean;

  if(!grp || !gui || !target_formids)
  {
    // only populate-style callers are allowed to hide a control -- an
    // interactive edit (allow_hide == FALSE) must never make its own widget
    // vanish out from under the user's drag; see the allow_hide comment below.
    if(allow_hide) gtk_widget_hide(widget);
    return;
  }

  const float value = is_bool
                        ? (float)gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))
                        : dt_bauhaus_slider_get(widget);
  const float old_value = *last_value;

  int count = 0;
  float sum = 0;
  float min = _blend_masks_properties[prop].min, max = _blend_masks_properties[prop].max;
  if(!is_bool)
  {
    if(_blend_masks_properties[prop].relative)
    {
      max /= min;
      min /= _blend_masks_properties[prop].max;
    }
    else
    {
      max -= min;
      min -= _blend_masks_properties[prop].max;
    }
  }

  // at any depth: a row can edit a member of a nested group. Positions index
  // the canvas's copy of the group (see _canvas_points)
  GArray *pts = _canvas_points(grp);
  // the shapes already reshaped in this pass: a mask can hold one shape
  // several times, and geometry belongs to the shape, not to the reference.
  // Applied per reference, one slider step would land several times, and
  // compound for a relative property (size)
  GList *reshaped = NULL;
  for(guint k = 0; k < pts->len; k++)
  {
    dt_masks_point_group_t *fpt = g_array_index(pts, _canvas_point_t, k).pt;
    const int fpos = g_array_index(pts, _canvas_point_t, k).pos;
    if(!g_list_find(target_formids, GINT_TO_POINTER(fpt->formid))) continue;

    dt_masks_form_t *sel = dt_masks_get_from_id(dev, fpt->formid);
    if(!sel) continue;

    if(prop == DT_MASKS_PROPERTY_OPACITY)
    {
      // changed in place and committed once after the loop:
      // dt_masks_form_change_opacity commits per form, which would be a
      // history item per member on every drag tick. Down to 0: an element or
      // group under MASK_LOW_OPACITY_WARN carries a badge (_make_lowop_badge),
      // so a shape at 0 shows, and the slider reaches the 0 it shows
      const float new_opacity = CLAMP(fpt->opacity + (value - old_value), 0.0f, 1.0f);
      fpt->opacity = new_opacity;
      sum += new_opacity;
      max = fminf(max, 1.0f - new_opacity);
      min = fmaxf(min, 0.0f - new_opacity);
      ++count;
    }
    else if(sel->functions && sel->functions->modify_property)
    {
      if(g_list_find(reshaped, GINT_TO_POINTER(fpt->formid)))
      {
        // the shape was already changed through another of its references;
        // only this reference's own canvas copy still has to follow it
        if(value != old_value) dt_masks_gui_form_create(sel, gui, fpos, dev->gui_module);
        continue;
      }
      reshaped = g_list_prepend(reshaped, GINT_TO_POINTER(fpt->formid));
      const int saved_count = count;
      sel->functions->modify_property(sel, prop, old_value, value, &sum, &count, &min,
                                      &max);
      if(count != saved_count && value != old_value)
        dt_masks_gui_form_create(sel, gui, fpos, dev->gui_module);
    }
  }
  g_array_free(pts, TRUE);
  g_list_free(reshaped);

  // visibility ("does this property even apply to the current target set") is
  // decided only at populate time -- an interactive value-change (allow_hide
  // == FALSE) must never toggle it, or a transient count==0 mid-drag (e.g.
  // while a shape is being edited) would hide the very slider the user is
  // dragging, and it would stay hidden until the row's expander is reopened.
  if(allow_hide) gtk_widget_set_visible(widget, count != 0);
  if(!count) return;

  // the sets below emit "value-changed" or "toggled" on this widget, which
  // would re-enter its handler without end
  DT_ENTER_GUI_UPDATE();
  if(is_bool)
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), (sum / count) > 0.5f);
    *last_value = (float)gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  }
  else
  {
    if(_blend_masks_properties[prop].relative)
    {
      max *= sum / count;
      min *= sum / count;
    }
    else
    {
      max += sum / count;
      min += sum / count;
    }
    if(dt_isnan(min)) min = _blend_masks_properties[prop].min;
    if(dt_isnan(max)) max = _blend_masks_properties[prop].max;
    dt_bauhaus_slider_set_soft_range(widget, min, max);
    dt_bauhaus_slider_set(widget, sum / count);
    *last_value = dt_bauhaus_slider_get(widget);
  }
  DT_LEAVE_GUI_UPDATE();

  // the set above was silent (see _refresh_inline_opacity_label): where this
  // widget is the hidden slider behind a row header's compact opacity value,
  // and the value it settled on is not the one the user's own gesture left it
  // at (a clamp, or a soft-range recompute), that label has to be told by hand
  // or it keeps reading the number from before this call.
  _refresh_inline_opacity_label(widget);

  dt_control_queue_redraw_center();

  // an opacity change can push a row (or its whole group) across the
  // low-opacity threshold -- refresh the badges in place, on every drag tick.
  // Nothing else in the panel changes, so this must not be a rebuild.
  if(prop == DT_MASKS_PROPERTY_OPACITY)
  {
    _refresh_lowop_badges(module);
  }

  // the same shape can have a row under each group that references it, and
  // every one of those rows carries its own controls: show the new value in
  // all of them, not just the one the user is dragging
  if(value != old_value) _refresh_sibling_prop_rows(module, target_formids, widget);

  // one history item for the whole gesture over every target, whatever the
  // property
  if(value != old_value)
  {
    _props_committing = TRUE;
    dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
    _props_committing = FALSE;
  }
}

// the quad of the shrink or grow slider's unit toggle: always "%", drawn
// brighter by bauhaus while active; the slider's value format names the unit
static void _props_paint_resize_unit(cairo_t *cr,
                                     const gint x,
                                     const gint y,
                                     const gint w,
                                     const gint h,
                                     const gint flags,
                                     void *data)
{
  const char *txt = "%";
  cairo_save(cr);

  const double side = MIN(w, h);
  const double fx = x + (w - side) / 2.0;
  const double fy = y + (h - side) / 2.0;

  PangoLayout *layout = pango_cairo_create_layout(cr);
  if(darktable.bauhaus->pango_font_desc)
    pango_layout_set_font_description(layout, darktable.bauhaus->pango_font_desc);
  pango_layout_set_text(layout, txt, -1);
  int tw = 0, th = 0;
  pango_layout_get_pixel_size(layout, &tw, &th);

  const double pad = DT_PIXEL_APPLY_DPI(1.0);
  const double avail = side - 2.0 * pad;
  const double scale = (tw > 0 && th > 0) ? fmin(avail / tw, avail / th) : 1.0;
  cairo_translate(cr, fx + (side - tw * scale) / 2.0, fy + (side - th * scale) / 2.0);
  cairo_scale(cr, scale, scale);
  pango_cairo_show_layout(cr, layout);
  g_object_unref(layout);
  cairo_restore(cr);
}

// resize the row's shape to the slider's offset and commit one history item
static void _props_resize_commit(dt_masks_props_row_editor_t *ed)
{
  dt_develop_t *dev = darktable.develop;
  dt_masks_form_gui_t *gui = dev->form_gui;
  dt_masks_form_t *form = dt_masks_get_from_id(dev, ed->formid);
  if(!form || !gui || !form->functions || !form->functions->resize) return;

  const int amount = (int)roundf(dt_bauhaus_slider_get(ed->resize_widget));
  const gboolean pct = dt_bauhaus_widget_get_quad_active(ed->resize_widget);

  if(!form->functions->resize(form, amount, pct) && amount < 0)
    dt_control_log(_("shrink amount too large: the path would disappear"));

  // positions index the canvas's copy of the group (see _canvas_points)
  GArray *pts = _canvas_points(dt_masks_gui_module_mask_group(ed->module));
  int pos = 0;
  for(guint k = 0; k < pts->len; k++)
    if(g_array_index(pts, _canvas_point_t, k).pt->formid == ed->formid)
    {
      pos = g_array_index(pts, _canvas_point_t, k).pos;
      break;
    }
  g_array_free(pts, TRUE);

  dt_masks_gui_form_create(form, gui, pos, dev->gui_module);
  _props_committing = TRUE;
  dt_dev_add_masks_history_item(dev, dev->gui_module, TRUE);
  _props_committing = FALSE;
  dt_control_queue_redraw_center();
}

static gboolean _props_resize_timeout(gpointer data)
{
  dt_masks_props_row_editor_t *ed = data;
  ed->resize_timer = 0;
  _props_resize_commit(ed);
  return G_SOURCE_REMOVE;
}

// resizing is expensive: commit 180 ms after the last change, not on every
// slider tick
static void _props_resize_schedule_commit(dt_masks_props_row_editor_t *ed)
{
  if(ed->resize_updating) return;
  if(ed->resize_timer) g_source_remove(ed->resize_timer);
  ed->resize_timer = g_timeout_add(180, _props_resize_timeout, ed);
}

static void _props_resize_amount_changed(GtkWidget *w, dt_masks_props_row_editor_t *ed)
{
  _props_resize_schedule_commit(ed);
}

// the unit as the slider's value suffix ("5 px", "5 %")
static void _props_resize_sync_unit(dt_masks_props_row_editor_t *ed)
{
  const gboolean pct = dt_bauhaus_widget_get_quad_active(ed->resize_widget);
  dt_bauhaus_slider_set_format(ed->resize_widget, pct ? " %" : " px");
}

// the unit toggle lives in the slider's quad; bauhaus flips the active flag
// before emitting "quad-pressed", so the new state is read directly. The unit
// preference is shared with the canvas's scroll-to-resize (path.c, group.c)
static void _props_resize_unit_quad(GtkWidget *w, dt_masks_props_row_editor_t *ed)
{
  const gboolean pct = dt_bauhaus_widget_get_quad_active(w);
  dt_conf_set_string("masks/path_resize_unit", pct ? "% of path size" : "pixels");
  _props_resize_sync_unit(ed);
  _props_resize_schedule_commit(ed);
}

// show the shrink or grow slider for a path only, at the offset the path has
// (0 for a new one, or after a size, feather or rotation edit, see
// _props_row_control_changed). Called when populated and after each of the
// row's edits
static void _props_resize_update(dt_masks_props_row_editor_t *ed)
{
  if(!ed->resize_widget) return;
  dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, ed->formid);
  const gboolean is_path = form && form->functions && form->functions->resize_get;

  if(is_path)
  {
    const gboolean pct = dt_bauhaus_widget_get_quad_active(ed->resize_widget);
    float amount = 0.0f;
    form->functions->resize_get(form, pct, &amount);

    // reflect the current offset without triggering a (re)commit
    if(ed->resize_timer)
    {
      g_source_remove(ed->resize_timer);
      ed->resize_timer = 0;
    }
    ed->resize_updating = TRUE;
    dt_bauhaus_slider_set(ed->resize_widget, roundf(amount));
    ed->resize_updating = FALSE;
  }
  gtk_widget_set_visible(ed->resize_widget, is_path);
}

// destroy-notify for a props row editor's "props-editor" data: cancels any
// pending debounced resize commit (see _props_resize_schedule_commit) before
// freeing, so a row torn down mid-debounce (list rebuild, shape deletion, ...)
// never fires a commit against a dangling ed pointer.
static void _props_row_editor_free(gpointer data)
{
  dt_masks_props_row_editor_t *ed = data;
  if(ed->resize_timer) g_source_remove(ed->resize_timer);
  g_free(ed);
}

// the target list _props_row_apply takes, for an editor's one element. Caller
// frees it
static GList *_props_row_target_formids(const dt_masks_props_row_editor_t *ed)
{
  return ed ? g_list_prepend(NULL, GINT_TO_POINTER(ed->formid)) : NULL;
}

// (re)populate every one of this row's own controls -- called once right
// after construction and whenever the row's expander is opened. Re-running
// _props_row_apply with the unchanged value (ed->last_value[]) is a neutral
// no-op that reads the targets' current mean into the slider, and recomputes
// which controls apply (count != 0) and their soft range for this row's
// target
static void _props_row_populate(dt_masks_props_row_editor_t *ed)
{
  if(!ed) return;
  GList *ids = _props_row_target_formids(ed);
  for(int i = 0; i < DT_MASKS_PROPERTY_LAST; i++)
    if(ed->widget[i])
      _props_row_apply(ed->module, ids, i, ed->widget[i], &ed->last_value[i], TRUE);
  g_list_free(ids);

  // the first time the row is populated, each relative slider's value becomes
  // its double-click reset: reset to a ratio's neutral 0 would change nothing,
  // while this undoes the edits made since the row was first shown
  if(!ed->relative_baseline_set)
  {
    for(int i = 0; i < DT_MASKS_PROPERTY_LAST; i++)
      if(ed->widget[i] && _blend_masks_properties[i].relative)
        dt_bauhaus_slider_set_default(ed->widget[i], ed->last_value[i]);
    ed->relative_baseline_set = TRUE;
  }
}

// a shape referenced more than once in one mask has a row under each group
// that references it, each with its own copy of the controls. Push an edit
// made in one of them into the others, which otherwise keep reading the shape
// as it was before the edit until something rebuilds the list.
static void _refresh_sibling_prop_rows(dt_iop_module_t *module,
                                       GList *formids,
                                       GtkWidget *src)
{
  dt_iop_gui_blend_data_t *bd = module ? module->blend_data : NULL;
  if(!bd) return;
  // _props_row_populate re-enters _props_row_apply, which comes back here:
  // refresh the siblings of an edit, never the siblings of a refresh
  static gboolean refreshing = FALSE;
  if(refreshing) return;
  refreshing = TRUE;

  // a row's controls live in its expanded properties box (see _make_shape_row)
  for(GList *f = formids; f; f = g_list_next(f))
  {
    for(GSList *r = _masks_rows_for_form(bd, GPOINTER_TO_INT(f->data)); r; r = r->next)
    {
      GtkWidget *box = g_object_get_data(G_OBJECT(r->data), "props-editor-box");
      dt_masks_props_row_editor_t *ed =
        box ? g_object_get_data(G_OBJECT(box), "props-editor") : NULL;
      if(!ed) continue;
      // the control the user is holding keeps the position they left it at;
      // repopulating it would fight the gesture (see _props_row_apply's own
      // allow_hide reasoning)
      gboolean owns_src = FALSE;
      for(int i = 0; i < DT_MASKS_PROPERTY_LAST; i++)
        if(ed->widget[i] == src) owns_src = TRUE;
      if(!owns_src) _props_row_populate(ed);
    }
  }
  refreshing = FALSE;
}

// shared value-changed/toggled handler for a props row editor's controls. The
// control's own property index is stashed on the widget at construction time
// (see "dt-prop"), so one handler can serve all ten like _refine_control_changed
// does for the six refinement controls.
static void _props_row_control_changed(GtkWidget *widget, dt_masks_props_row_editor_t *ed)
{
  if(DT_IN_GUI_UPDATE() || !ed || !ed->module || !ed->module->blend_data
     || !((dt_iop_gui_blend_data_t *)ed->module->blend_data)->blend_inited)
    return;
  const int prop = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "dt-prop"));
  GList *ids = _props_row_target_formids(ed);
  _props_row_apply(ed->module, ids, prop, widget, &ed->last_value[prop], FALSE);
  g_list_free(ids);

  // a size, feather or rotation edit drops the path's shrink or grow baseline
  // (path.c), so the resize slider reads 0 again
  if(ed->resize_widget
     && (prop == DT_MASKS_PROPERTY_SIZE || prop == DT_MASKS_PROPERTY_FEATHER
         || prop == DT_MASKS_PROPERTY_ROTATION))
    _props_resize_update(ed);
}

// build an element's properties editor: the opacity alone (raster and nested
// group rows) or every property of _blend_masks_properties (shape rows; those
// a shape does not have hide, by _props_row_apply's count == 0 rule). The
// children are shown, then the box is set no_show_all, so that an ancestor's
// show_all (dt_masks_gui_build_list) cannot open it against its expander
static GtkWidget *_build_props_row_editor(dt_iop_module_t *module,
                                          const dt_mask_id_t formid,
                                          const gboolean opacity_only)
{
  dt_masks_props_row_editor_t *ed = g_malloc0(sizeof(dt_masks_props_row_editor_t));
  ed->module = module;
  ed->formid = formid;

  GtkWidget *box = dt_gui_vbox();
  for(int i = 0; i < DT_MASKS_PROPERTY_LAST; i++)
  {
    if(opacity_only && i != DT_MASKS_PROPERTY_OPACITY) continue;

    GtkWidget *w;
    if(_blend_masks_properties[i].boolean)
    {
      w = gtk_check_button_new_with_label(_(_blend_masks_properties[i].name));
      gtk_widget_set_tooltip_text(w, _(_blend_masks_properties[i].tooltip));
      ed->last_value[i] = (float)gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
      g_object_set_data(G_OBJECT(w), "dt-prop", GINT_TO_POINTER(i));
      g_signal_connect(G_OBJECT(w), "toggled", G_CALLBACK(_props_row_control_changed),
                       ed);
    }
    else
    {
      // every property applies the change from the slider's last value
      // (_props_row_apply, seeded by _props_row_populate). Opacity's slider
      // shows an absolute position, so its double-click resets to 100%
      float defval = 0.0;
      if(i == DT_MASKS_PROPERTY_OPACITY) defval = 1.0;
      // a relative (ratio) property's neutral reading is an exact 0, which
      // modify_property treats as identity (e.g. circle.c). The slider clamps
      // to its hard min, so it gets 0 rather than the table's 0.0001: a reset
      // landing on 0.0001 would shrink the shape almost to nothing. The
      // table's min stays the soft floor everywhere else
      const float widget_min =
        _blend_masks_properties[i].relative ? 0.0f : _blend_masks_properties[i].min;
      w = dt_bauhaus_slider_new_with_range(module, widget_min,
                                           _blend_masks_properties[i].max, 0, defval, 2);
      dt_bauhaus_widget_set_label(w, N_("blend"), _blend_masks_properties[i].name);
      dt_bauhaus_slider_set_format(w, _blend_masks_properties[i].format);
      dt_bauhaus_slider_set_digits(w, 2);
      if(_blend_masks_properties[i].relative) dt_bauhaus_slider_set_log_curve(w);
      // a relative slider's double-click goes back to where it stood when the
      // row was first populated (see _props_row_populate), which nothing on
      // screen would otherwise say
      gchar *tip = g_strdup_printf(
        "%s\n%s", _(_blend_masks_properties[i].tooltip),
        _blend_masks_properties[i].relative
          ? _("double-click to undo the edits made since the properties were shown")
          : i == DT_MASKS_PROPERTY_OPACITY ? _("double-click to reset to 100%")
                                           : _("double-click to reset to 0"));
      gtk_widget_set_tooltip_text(w, tip);
      g_free(tip);
      ed->last_value[i] = dt_bauhaus_slider_get(w);
      g_object_set_data(G_OBJECT(w), "dt-prop", GINT_TO_POINTER(i));
      g_signal_connect(G_OBJECT(w), "value-changed",
                       G_CALLBACK(_props_row_control_changed), ed);
      // a bauhaus slider paints an opaque background, which would hide the
      // row's hover and selection wash (as for .dt_masks_boost_slider)
      dt_gui_add_class(w, "dt_masks_props_slider");
      // no quad: its unused width would make the slider narrower than the row
      dt_bauhaus_widget_set_quad_visibility(w, FALSE);
    }
    ed->widget[i] = w;
    dt_gui_box_add(box, w);
  }

  // the shrink or grow control of a path, with its unit stored in the config
  // and debounced calls into path.c's resize. Not in an opacity-only editor;
  // _props_resize_update hides it for anything but a path
  if(!opacity_only)
  {
    GtkWidget *w = dt_bauhaus_slider_new_with_range(module, -1000, 1000, 1, 0.0, 0);
    dt_bauhaus_widget_set_label(w, N_("blend"), N_("shrink or grow"));
    dt_bauhaus_slider_set_soft_range(w, -20, 20);
    dt_bauhaus_slider_set_format(w, "");
    gtk_widget_set_tooltip_text(
      w, _("grow (positive) or shrink (negative) this path's outline\n"
           "0 restores the outline it grew or shrank from. a size, feather\n"
           "or rotation edit makes the current outline the new 0"));
    g_signal_connect(G_OBJECT(w), "value-changed",
                     G_CALLBACK(_props_resize_amount_changed), ed);
    dt_gui_add_class(w, "dt_masks_props_slider");

    // unit (px / %) toggle in the slider's quad -- kept visible, unlike the
    // other properties sliders above, since it is this control's own setting
    dt_bauhaus_widget_set_quad_paint(w, _props_paint_resize_unit, 0, NULL);
    dt_bauhaus_widget_set_quad_toggle(w, TRUE);
    {
      const char *unit = dt_conf_get_string_const("masks/path_resize_unit");
      dt_bauhaus_widget_set_quad_active(w, !g_strcmp0(unit, "% of path size"));
    }
    dt_bauhaus_widget_set_quad_tooltip(
      w, _("shrink/grow unit: image pixels (px) or % of path size\nclick to toggle"));
    g_signal_connect(G_OBJECT(w), "quad-pressed", G_CALLBACK(_props_resize_unit_quad),
                     ed);

    ed->resize_widget = w;
    _props_resize_sync_unit(ed);
    dt_gui_box_add(box, w);

    // "size" scales the shape live; "shrink or grow" insets/outsets its outline:
    // keep this slider right below "size" instead of at the end of the list
    if(ed->widget[DT_MASKS_PROPERTY_SIZE])
    {
      GList *kids = gtk_container_get_children(GTK_CONTAINER(box));
      const gint size_pos = g_list_index(kids, ed->widget[DT_MASKS_PROPERTY_SIZE]);
      if(size_pos >= 0) gtk_box_reorder_child(GTK_BOX(box), w, size_pos + 1);
      g_list_free(kids);
    }
  }

  dt_gui_add_class(box, "dt_masks_card");
  dt_gui_add_class(box, "dt_masks_props_card");

  // a pending resize commit (_props_resize_schedule_commit) must not fire after
  // the row is gone: _props_row_editor_free removes it
  g_object_set_data_full(G_OBJECT(box), "props-editor", ed, _props_row_editor_free);
  gtk_widget_show_all(box);
  gtk_widget_set_no_show_all(box, TRUE);

  _props_row_populate(ed);
  _props_resize_update(ed);
  return box;
}

// ---- "element properties in subpanel" --------------------------------------

// does this parametric channel have a boost factor? Not every channel does
static gboolean _param_form_has_boost(const dt_masks_form_t *f)
{
  const dt_masks_point_parametric_t *p = f->points ? f->points->data : NULL;
  const dt_iop_gui_blendif_channel_t *channels = _param_channels(p);
  return channels && channels[p->channel].boost_factor_enabled;
}

// what the subpanel holds the editor of, following the selection as the
// refinements do: the selected element, or else the selected group. A shape's
// geometry goes there, a parametric channel's boost factor, and the opacity of
// anything. The AI object stepped into is no shape any more but its group, so
// only its opacity goes there. An element hovered on the canvas takes the
// selection's place
dt_masks_props_target_t dt_masks_model_props_panel_target(const dt_iop_gui_blend_data_t *bd)
{
  dt_masks_props_target_t t = { INVALID_MASKID, FALSE, FALSE, FALSE, FALSE };
  const dt_mask_id_t hovered = _canvas_hovered(bd);
  dt_mask_id_t id = dt_is_valid_maskid(hovered) ? hovered : bd->panel_selected_formid;
  if(dt_is_valid_maskid(id))
  {
    const dt_masks_form_t *f = dt_masks_get_from_id(darktable.develop, id);
    if(!f) return t;
    if(f->type & DT_MASKS_PARAMETRIC)
      t.boost = _param_form_has_boost(f);
    else if(!(f->type & (DT_MASKS_RASTER | DT_MASKS_GROUP))
            && !((f->type & DT_MASKS_OBJECT) && _entered_object() == id))
      t.shape = TRUE;
  }
  else if(dt_is_valid_maskid(bd->panel_selected_group_cid))
  {
    id = bd->panel_selected_group_cid;
    t.is_group = TRUE;
  }
  else
    return t;
  t.opacity = TRUE;
  t.id = id;
  return t;
}

// the subpanel's editor for its target (see dt_masks_model_props_panel_target)
static GtkWidget *_build_props_panel_editor(dt_iop_module_t *module,
                                            const dt_masks_props_target_t *t)
{
  GtkWidget *box = dt_gui_vbox();
  if(t->is_group)
    dt_gui_box_add(box, _build_group_opacity_editor(module, t->id));
  else if(t->shape || t->opacity)
    dt_gui_box_add(box, _build_props_row_editor(module, t->id, !t->shape));
  if(t->boost) dt_gui_box_add(box, _build_param_boost_editor(module, t->id));
  // the props editor inside is no_show_all, and already shown
  gtk_widget_show_all(box);
  return box;
}

// the subpanel shows only while it holds something: with the option on, with
// the mask list shown, and with a shape being drawn or a selection having
// properties to show there
static void _props_panel_show(dt_iop_gui_blend_data_t *bd)
{
  GtkWidget *pending = bd->pending_props_box;
  const gboolean filled = (pending && gtk_widget_get_parent(pending) == bd->props_panel_content)
                          || dt_is_valid_maskid(bd->props_panel_formid);
  _box_set_visible(bd->props_panel_box,
                   _props_subpanel() && filled && bd->masks_list_box
                   && gtk_widget_get_visible(GTK_WIDGET(bd->masks_list_box)));
}

// fill the subpanel from the selection: the creation controls of a shape being
// drawn, opened, or else the selected element's or group's properties, or else
// nothing, and hidden. Kept while it still holds what the selection asks for,
// since the editor inside may be the one being dragged; `force` rebuilds it
// anyway, after the list was rebuilt from changed data
static void _props_panel_sync(dt_iop_module_t *module, const gboolean force)
{
  dt_iop_gui_blend_data_t *bd = module ? module->blend_data : NULL;
  if(!bd || !bd->props_panel_content) return;
  const gboolean on = _props_subpanel();
  GtkWidget *pending = on ? bd->pending_props_box : NULL;
  const dt_masks_props_target_t none = { INVALID_MASKID, FALSE, FALSE, FALSE, FALSE };
  const dt_masks_props_target_t t =
    pending || !on ? none : dt_masks_model_props_panel_target(bd);
  const gboolean placed = pending && gtk_widget_get_parent(pending) == bd->props_panel_content;
  // renaming the target changes neither of what the check below keeps
  _selection_row_update(module);
  // the section opens for the creation controls without saving that, so it
  // folds back once the shape is created or the drawing canceled. Only that
  // opening leaves it differing from its saved state
  if(!pending)
  {
    if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(bd->props_panel_toggle_btn))
       != dt_masks_model_section_expanded(DT_MASKS_SECTION_PROPS, FALSE))
      _section_apply(bd, DT_MASKS_SECTION_PROPS);
  }
  if(!force
     && (pending ? placed
                 : t.id == bd->props_panel_formid && t.is_group == bd->props_panel_is_group))
    return;

  GList *kids = gtk_container_get_children(GTK_CONTAINER(bd->props_panel_content));
  for(GList *k = kids; k; k = g_list_next(k))
    if(k->data != pending) gtk_widget_destroy(k->data);
  g_list_free(kids);
  bd->props_panel_formid = t.id;
  bd->props_panel_is_group = t.is_group;

  if(pending)
  {
    if(!placed) dt_gui_box_add(bd->props_panel_content, pending);
    _section_set(bd, DT_MASKS_SECTION_PROPS, dt_masks_model_section_expanded(DT_MASKS_SECTION_PROPS, TRUE));
  }
  else if(dt_is_valid_maskid(t.id))
    dt_gui_box_add(bd->props_panel_content, _build_props_panel_editor(module, &t));
  _props_panel_show(bd);
}

// the id a tagged widget carries under `key`
static inline dt_mask_id_t _widget_id(GtkWidget *w, const char *key)
{
  return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), key));
}

// remember row or group `key` as expanded or not, across list rebuilds (see
// bd->masks_props_expanded)
static void _remember_expanded(dt_iop_gui_blend_data_t *bd,
                               const dt_mask_id_t key,
                               const gboolean expanded)
{
  if(!bd->masks_props_expanded)
    bd->masks_props_expanded = g_hash_table_new(g_direct_hash, g_direct_equal);
  g_hash_table_insert(bd->masks_props_expanded, GINT_TO_POINTER(key),
                      GINT_TO_POINTER(expanded));
}

// show or hide the box an expander chevron drives, tagged on it as `box_key`
static void _show_expanded_box(GtkWidget *btn, const char *box_key, const gboolean expanded)
{
  GtkWidget *box = g_object_get_data(G_OBJECT(btn), box_key);
  if(!box) return;
  gtk_widget_set_visible(box, expanded);
  gtk_widget_queue_resize(box);
}

// toggled handler for the shared props-row chevron built by
// _make_props_row_toggle: flips the row's remembered expand state (keyed by
// its own target id, "props-key") and shows/hides its docked editor box
// ("props-editor-box") in place -- no rebuild needed.
static void _props_row_toggled(GtkWidget *btn, dt_iop_module_t *module)
{
  if(DT_IN_GUI_UPDATE()) return;
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  const dt_mask_id_t key = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "props-key"));
  const gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(btn));

  // an element row's own toggle selects it, if it wasn't already selected
  // (never deselect: same select-only rule as every other action control, see
  // _set_form_target). The row does not see the click: the toggle claims its
  // own press.
  //
  // Not while "auto-expand selected" flips other rows' toggles itself
  // (masks_suppress_toggle_select): re-selecting a row it collapses would
  // collapse the previous one, re-selecting that, without end. Not the GUI
  // update guard either, which would skip the remembering below too
  if(!bd->masks_suppress_toggle_select) _element_chevron_clicked(module, key, active);

  _remember_expanded(bd, key, active);
  _show_expanded_box(btn, "props-editor-box", active);
}

// declared here so _group_expand_toggled can hand the option its new
// last-expanded group; defined below, once the group header lookup exists.
static void _collapse_auto_expanded_group(dt_iop_module_t *module,
                                          const dt_mask_id_t keep_cid);

// TRUE while _auto_expand_selected_group or _collapse_auto_expanded_group
// flip group chevrons. That re-enters _group_expand_toggled, which must still
// update the state and visibility but not take the flip for the user
// overriding the option: it would clear the last expanded group being set,
// and leave a stale collapse click behind
static gboolean _group_expand_enforcing = FALSE;

// set chevron `toggle` (NULL for none) to `active`, as code enforcing what is
// open rather than as the user's click when `enforcing` (see above)
static void _set_chevron(GtkWidget *toggle, const gboolean active, const gboolean enforcing)
{
  if(!toggle || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggle)) == active) return;
  const gboolean was = _group_expand_enforcing;
  _group_expand_enforcing = was || enforcing;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), active);
  _group_expand_enforcing = was;
}

static void _group_expand_toggled(GtkToggleButton *btn, gpointer user_data)
{
  if(DT_IN_GUI_UPDATE()) return;
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  dt_iop_gui_blend_data_t *bd = module ? module->blend_data : NULL;
  if(!bd) return;
  const dt_mask_id_t cid = _widget_id(GTK_WIDGET(btn), "props-key");
  const gboolean active = gtk_toggle_button_get_active(btn);
  _remember_expanded(bd, cid, active);
  _show_expanded_box(GTK_WIDGET(btn), "elem-box", active);

  if(_group_expand_enforcing) return;
  // a real click on the chevron selects the group, never deselects it, as an
  // element's chevron does (see _element_chevron_clicked): the header does not
  // see this click, which the chevron claims
  const gboolean selects = bd->panel_selected_group_cid != cid;

  // it is also the user overriding "auto-expand selected" by hand, so it
  // decides what the option considers open from here on. The selection below
  // runs auto-expand again, so tell it what just happened rather than let it
  // undo the click
  if(_auto_expand_selected())
  {
    if(active)
    {
      _collapse_auto_expanded_group(module, cid);
      bd->masks_last_expanded_group = cid;
    }
    else
    {
      bd->masks_last_expanded_group = INVALID_MASKID;
      if(selects) bd->masks_group_collapse_click = cid;
    }
  }
  if(selects) _set_group_target(module, cid);
}

// a nested group row's chevron: shows or hides the row's group, and
// remembers it by the nested group's form id, as a group header's chevron
// does by the group's (see _group_expand_toggled). "auto-expand selected"
// leaves it alone: what it shows follows the selection through
// _reveal_nesting instead
static void _subgroup_expand_toggled(GtkToggleButton *btn, gpointer user_data)
{
  if(DT_IN_GUI_UPDATE()) return;
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  dt_iop_gui_blend_data_t *bd = module ? module->blend_data : NULL;
  if(!bd) return;
  const gboolean active = gtk_toggle_button_get_active(btn);
  _remember_expanded(bd, _widget_id(GTK_WIDGET(btn), "props-key"), active);
  _show_expanded_box(GTK_WIDGET(btn), "elem-box", active);
}

// which element "auto-expand selected" keeps open at build time: the
// current selection, if it is an element that can be expanded at all, and
// otherwise whatever was expanded last (see bd->masks_last_expanded_elem).
// Selecting a group, or an element with nothing to expand, therefore leaves
// the open element open rather than collapsing the panel down to nothing.
dt_mask_id_t dt_masks_model_auto_expand_anchor(const dt_iop_gui_blend_data_t *bd)
{
  const dt_mask_id_t sel = bd->panel_selected_formid;
  if(dt_is_valid_maskid(sel))
  {
    const dt_masks_form_t *f = dt_masks_get_from_id(darktable.develop, sel);
    // a nested group's chevron shows groups, which follow the group half
    // (see _reveal_nesting), not properties
    if(f && !(f->type & DT_MASKS_GROUP)
       && dt_masks_model_row_is_expandable(f->type, _props_subpanel()))
      return sel;
  }
  return bd->masks_last_expanded_elem;
}

// the same, one level up: which group "auto-expand selected" keeps open at
// build time. A group needs no expandability test -- every group has members
// to reveal -- so this is simply the selected group, falling back to whatever
// the option opened last.
dt_mask_id_t dt_masks_model_auto_expand_group_anchor(const dt_iop_gui_blend_data_t *bd)
{
  if(dt_is_valid_maskid(bd->panel_selected_group_cid))
    return bd->panel_selected_group_cid;
  return bd->masks_last_expanded_group;
}

// a real click on an element row's chevron decides what is open, not the
// selection it also makes: expanding makes this row the one open element and
// collapses the previous one, collapsing it forgets it. Without the option,
// nothing moves.
dt_masks_chevron_click_t dt_masks_model_element_chevron_click(const dt_iop_gui_blend_data_t *bd,
                                                              const dt_mask_id_t id,
                                                              const gboolean expanded,
                                                              const gboolean auto_expand)
{
  dt_masks_chevron_click_t c = { INVALID_MASKID, bd->masks_last_expanded_elem };
  if(!auto_expand) return c;
  if(expanded)
  {
    if(dt_is_valid_maskid(c.last_expanded) && c.last_expanded != id)
      c.collapse = c.last_expanded;
    c.last_expanded = id;
  }
  else if(c.last_expanded == id)
    c.last_expanded = INVALID_MASKID;
  return c;
}

// the chevron and properties editor of a shape or raster row: a chevron styled
// like every other row expander (".dt_masks_expander"), its expanded state kept
// across rebuilds in bd->masks_props_expanded, keyed by the element's form id.
// Returns the chevron; *editor_box_out receives the editor box to dock under
// the row, its initial visibility already applied
static GtkWidget *_make_props_row_toggle(dt_iop_module_t *module,
                                         const dt_mask_id_t key,
                                         const gboolean opacity_only,
                                         const char *tooltip,
                                         GtkWidget **editor_box_out)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  // with "auto-expand selected", the anchor is the one expanded row
  // (dt_masks_model_auto_expand_anchor), not the selection: selecting
  // something with nothing to expand leaves the open row open.
  // _auto_expand_selected_row does the same in place on a selection change,
  // which rebuilds nothing
  const dt_mask_id_t anchor = dt_masks_model_auto_expand_anchor(bd);
  const gboolean is_anchor = dt_is_valid_maskid(anchor) && key == anchor;
  const gboolean expanded =
    _auto_expand_selected()
      ? is_anchor
      : bd->masks_props_expanded
          && GPOINTER_TO_INT(g_hash_table_lookup(bd->masks_props_expanded, GINT_TO_POINTER(key)));
  if(_auto_expand_selected() && is_anchor) bd->masks_last_expanded_elem = key;

  GtkWidget *editor_box = _build_props_row_editor(module, key, opacity_only);
  gtk_widget_set_visible(editor_box, expanded);

  GtkWidget *btn = dtgtk_togglebutton_new(_paint_param_inout, 0, NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), expanded);
  // an expander (chevron), not a mode toggle -- .dt_masks_expander in
  // darktable.css carries the shared chevron styling
  dt_gui_add_class(btn, "dt_masks_icon");
  dt_gui_add_class(btn, "dt_masks_expander");
  dt_gui_add_class(btn, "dt_transparent_background");
  gtk_widget_set_tooltip_text(btn, tooltip);
  g_object_set_data(G_OBJECT(btn), "props-key", GINT_TO_POINTER(key));
  g_object_set_data(G_OBJECT(btn), "props-editor-box", editor_box);
  g_signal_connect(G_OBJECT(btn), "toggled", G_CALLBACK(_props_row_toggled), module);

  if(editor_box_out) *editor_box_out = editor_box;
  return btn;
}

// the tooltip of an inline opacity slider, whose label and value are hidden,
// kept current on "value-changed". The group header sets its own
// (_group_opacity_update_tooltip)
static void _inline_opacity_tooltip_changed(GtkWidget *w, gpointer user_data)
{
  gchar *tip = g_strdup_printf(_("opacity: %.0f%%"), dt_bauhaus_slider_get(w) * 100.0f);
  gtk_widget_set_tooltip_text(w, tip);
  g_free(tip);
}

static void _inline_opacity_update_label(GtkWidget *label, const float val)
{
  GtkWidget *slider = g_object_get_data(G_OBJECT(label), "opacity-slider");
  const float max_val = slider ? dt_bauhaus_slider_get_hard_max(slider) : 1.0f;
  const float pct = (max_val > 1.5f) ? val : (val * 100.0f);
  gchar *txt = g_strdup_printf("%.0f%%", pct);
  gtk_label_set_text(GTK_LABEL(label), txt);
  g_free(txt);
}

static void _inline_opacity_slider_changed_cb(GtkWidget *slider, gpointer user_data)
{
  GtkWidget *label = user_data;
  if(GTK_IS_LABEL(label))
    _inline_opacity_update_label(label, dt_bauhaus_slider_get(slider));
}

// copy a slider's value into its value label (_make_inline_opacity_value_widget)
// after a set made under DT_IN_GUI_UPDATE(): bauhaus emits no "value-changed"
// then (_slider_set_normalized), so the slider repaints but the label would
// keep the old number
static void _refresh_inline_opacity_label(GtkWidget *slider)
{
  if(!slider) return;
  GtkWidget *label = g_object_get_data(G_OBJECT(slider), "opacity-value-label");
  if(GTK_IS_LABEL(label))
    _inline_opacity_update_label(label, dt_bauhaus_slider_get(slider));
}

static void _blend_opacity_slider_changed_cb(GtkWidget *slider, gpointer user_data)
{
  dt_iop_gui_blend_data_t *bd = user_data;
  if(!bd || !bd->blend_opacity_lowop_badge) return;
  const float val = dt_bauhaus_slider_get(slider);
  _update_blend_opacity_badge(bd->blend_opacity_lowop_badge, val / 100.0f);
  // opacity is a blending parameter and blending is skipped while the mask is
  // off, so setting it switches the mask on. Safe here: bauhaus writes the
  // field and commits before it emits "value-changed" (bauhaus.c:3970), so the
  // gui update this triggers repaints the slider from the new value
  if(!DT_IN_GUI_UPDATE()) _blendop_mask_enable(bd->module);
}

// the whisker popup's placement, over plain geometry so that it can be tested
// without a display: a square above or below the anchor (never over it, so
// the controls it drives stay visible), centered where the caller asked, and
// inside both the host panel and the work area
GdkRectangle dt_masks_model_whisker_popup_rect(const dt_masks_whisker_geom_t *g)
{
  const gint space_above = g->anchor.y - g->workarea.y;
  const gint space_below =
    (g->workarea.y + g->workarea.height) - (g->anchor.y + g->anchor.height);

  // below by preference: either it fits there, or there is at least as much
  // room below as above
  gint y = (space_below >= g->size + g->gap || space_below >= space_above)
             ? g->anchor.y + g->anchor.height + g->gap
             : g->anchor.y - g->gap - g->size;

  // neither side has the room -- a short screen, or an anchor near an edge.
  // Overlapping the anchor is bad; hanging off the work area is worse, since
  // the popup is then squashed to fit (GDK_ANCHOR_RESIZE_Y, see
  // _window_position in bauhaus.c) and a squashed color wheel stops being a
  // circle.
  y = CLAMP(y, g->workarea.y, g->workarea.y + g->workarea.height - g->size);

  // held to the panel rather than the work area: the panel is where the
  // controls are, and a popup that wandered onto the image would cover the
  // very thing the user is judging the change against
  const gint x = CLAMP(g->center_x - g->size / 2, g->panel_x,
                       g->panel_x + g->panel_w - g->size);

  const GdkRectangle rect = { x, y, g->size, g->size };
  return rect;
}

// where the whisker popup of `anchor` goes, centered on the x offset
// `center_in_anchor`, in the root coordinates that
// dt_bauhaus_widget_set_popup_position() takes. Pin it before showing it, so
// that bauhaus places it; do not move the popup window afterwards: bauhaus
// repositions it (_window_position in bauhaus.c), which on another monitor
// sends it back to the primary display
static gboolean _bauhaus_whisker_popup_rect(GtkWidget *anchor,
                                            const gint center_in_anchor,
                                            GdkRectangle *rect)
{
  GtkWidget *toplevel = gtk_widget_get_toplevel(anchor);
  GdkWindow *top_gdk =
    gtk_widget_is_toplevel(toplevel) ? gtk_widget_get_window(toplevel) : NULL;
  if(!top_gdk) return FALSE;

  gint top_x, top_y;
  gdk_window_get_origin(top_gdk, &top_x, &top_y);

  gint rx, ry;
  gtk_widget_translate_coordinates(anchor, toplevel, 0, 0, &rx, &ry);
  GtkAllocation alloc;
  gtk_widget_get_allocation(anchor, &alloc);

  dt_masks_whisker_geom_t g = { .anchor = { top_x + rx, top_y + ry,
                                            alloc.width, alloc.height },
                                .center_x = top_x + rx + center_in_anchor,
                                .size = DT_PIXEL_APPLY_DPI(180),
                                .gap = DT_PIXEL_APPLY_DPI(6) };

  GdkMonitor *mon =
    gdk_display_get_monitor_at_window(gdk_window_get_display(top_gdk), top_gdk);
  if(mon) gdk_monitor_get_workarea(mon, &g.workarea);

  g.panel_x = g.workarea.x;
  g.panel_w = g.workarea.width;
  if(dt_ui_panel_ancestor(darktable.gui->ui, DT_UI_PANEL_LEFT, anchor))
  {
    g.panel_x = top_x;
    g.panel_w = dt_ui_panel_get_size(darktable.gui->ui, DT_UI_PANEL_LEFT);
  }
  else if(dt_ui_panel_ancestor(darktable.gui->ui, DT_UI_PANEL_RIGHT, anchor))
  {
    g.panel_w = dt_ui_panel_get_size(darktable.gui->ui, DT_UI_PANEL_RIGHT);
    g.panel_x = top_x + gtk_widget_get_allocated_width(toplevel) - g.panel_w;
  }

  *rect = dt_masks_model_whisker_popup_rect(&g);
  return TRUE;
}

// pin `slider`'s popup above/below `anchor`, centered on `center_in_anchor`
// (an x offset within the anchor), and open it there
static void _show_bauhaus_whisker_popup(GtkWidget *slider,
                                        GtkWidget *anchor,
                                        const gint center_in_anchor)
{
  GdkRectangle rect;
  if(_bauhaus_whisker_popup_rect(anchor, center_in_anchor, &rect))
    dt_bauhaus_widget_set_popup_position(slider, &rect);
  dt_bauhaus_widget_show_popup(slider);
}

static gboolean _inline_opacity_popup_idle(gpointer user_data)
{
  // held by a reference: a list rebuild may have destroyed the row meanwhile,
  // which leaves it unrealized, and its slider with it
  GtkWidget *evbox = user_data;
  if(!gtk_widget_get_realized(evbox)) return G_SOURCE_REMOVE;
  GtkWidget *slider = g_object_get_data(G_OBJECT(evbox), "opacity-slider");
  if(!slider || !GTK_IS_WIDGET(slider)) return G_SOURCE_REMOVE;

  GtkAllocation alloc;
  gtk_widget_get_allocation(evbox, &alloc);
  _show_bauhaus_whisker_popup(slider, evbox, alloc.width / 2);
  return G_SOURCE_REMOVE;
}

// claimed whatever the button, so the row under the value never sees a press
// meant for it
static void _inline_opacity_pressed(GtkGestureSingle *gesture,
                                    const int n_press,
                                    const double x,
                                    const double y,
                                    gpointer user_data)
{
  dt_gui_claim(gesture);
  GtkWidget *w = dt_gui_get_widget(gesture);
  GtkWidget *slider = g_object_get_data(G_OBJECT(w), "opacity-slider");
  if(!slider) return;

  const guint button = gtk_gesture_single_get_current_button(gesture);
  if(button == GDK_BUTTON_SECONDARY)
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, _inline_opacity_popup_idle,
                    g_object_ref(w), g_object_unref);
  else if(n_press == 2 && button == GDK_BUTTON_PRIMARY)
  {
    const float max_val = dt_bauhaus_slider_get_hard_max(slider);
    dt_bauhaus_slider_set(slider, max_val > 1.5f ? 100.0f : 1.0f);
  }
}

// a discrete scroll (see dt_gui_connect_scroll), which also leaves a bare
// scroll to the panel when "darkroom/ui/sidebar_scroll_default" asks for that
static void _inline_opacity_scroll(GtkEventControllerScroll *controller,
                                   const double dx,
                                   const double dy,
                                   gpointer user_data)
{
  GtkWidget *w = dt_gui_get_widget(controller);
  GtkWidget *slider = g_object_get_data(G_OBJECT(w), "opacity-slider");
  if(!slider || !gtk_widget_is_sensitive(w)) return;

  const GdkModifierType state = dt_gui_get_current_event_state(GTK_EVENT_CONTROLLER(controller));
  const gboolean is_ctrl = (state & GDK_CONTROL_MASK) != 0;
  const gboolean is_shift = (state & GDK_SHIFT_MASK) != 0;

  const float max_val = dt_bauhaus_slider_get_hard_max(slider);
  const gboolean is_100_scale = (max_val > 1.5f);

  float step = is_100_scale ? 5.0f : 0.05f;
  if(is_ctrl)
    step = is_100_scale ? 1.0f : 0.01f;
  else if(is_shift)
    step = is_100_scale ? 10.0f : 0.10f;

  // whole steps, up (or left) raising the value
  double delta = (fabs(dx) > fabs(dy)) ? -dx : -dy;
  if(dt_conf_get_bool("masks_scroll_down_increases")) delta = -delta;

  const float current = dt_bauhaus_slider_get(slider);
  const float new_val = CLAMP(current + delta * step, 0.0f, is_100_scale ? 100.0f : 1.0f);
  dt_bauhaus_slider_set(slider, new_val);
}

static void _inline_opacity_enter(GtkEventControllerMotion *controller,
                                  gdouble x,
                                  gdouble y,
                                  gpointer user_data)
{
  dt_gui_cursor_set(dt_gui_get_widget(controller), "ns-resize", "mask/opacity");
}

static void _inline_opacity_leave(GtkEventControllerMotion *controller,
                                  gpointer user_data)
{
  dt_gui_cursor_set(dt_gui_get_widget(controller), NULL, "mask/opacity");
}

static void _inline_opacity_realize(GtkWidget *widget, gpointer user_data)
{
  dt_gui_cursor_set(widget, "ns-resize", "mask/opacity");
}

static GtkWidget *_make_inline_opacity_value_widget(GtkWidget *slider,
                                                    dt_iop_module_t *module)
{
  GtkWidget *evbox = gtk_event_box_new();
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(evbox), TRUE);
  gtk_widget_add_events(evbox, GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK
                                 | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);

  GtkWidget *label = gtk_label_new("");
  gtk_label_set_width_chars(GTK_LABEL(label), 4);
  gtk_label_set_xalign(GTK_LABEL(label), 1.0f);
  if(slider)
  {
    g_object_set_data(G_OBJECT(label), "opacity-slider", slider);
    // the reverse link, for the sets bauhaus makes silently -- see
    // _refresh_inline_opacity_label
    g_object_set_data(G_OBJECT(slider), "opacity-value-label", label);
    _inline_opacity_update_label(label, dt_bauhaus_slider_get(slider));
    g_signal_connect(G_OBJECT(slider), "value-changed",
                     G_CALLBACK(_inline_opacity_slider_changed_cb), label);
  }
  gtk_container_add(GTK_CONTAINER(evbox), label);
  dt_gui_add_class(evbox, "dt_masks_opacity_value");
  gtk_widget_set_tooltip_text(evbox,
                              _("opacity\n"
                                "scroll to adjust by 5%, shift+scroll by 10%,"
                                " ctrl+scroll by 1%\n"
                                "right-click for precise entry\n"
                                "double-click to reset to 100%"));

  g_object_set_data(G_OBJECT(evbox), "opacity-slider", slider);
  if(module) g_object_set_data(G_OBJECT(evbox), "module", module);
  g_signal_connect(G_OBJECT(evbox), "realize", G_CALLBACK(_inline_opacity_realize), NULL);
  dt_gui_connect_motion(evbox, NULL, _inline_opacity_enter, _inline_opacity_leave, NULL);
  dt_gui_connect_click(evbox, _inline_opacity_pressed, NULL, NULL);
  dt_gui_connect_scroll(evbox,
                        GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES | GTK_EVENT_CONTROLLER_SCROLL_DISCRETE,
                        _inline_opacity_scroll, NULL);

  return evbox;
}

// an icon button without an icon: a column the row has nothing for, exactly
// as big as the icons beside it whatever the theme makes of them, so every
// row keeps its icons in the same columns whichever ones it has
static GtkWidget *_header_blank_cell(void)
{
  GtkWidget *blank = dtgtk_button_new(NULL, 0, NULL);
  dt_gui_add_class(blank, "dt_masks_icon");
  dt_gui_add_class(blank, "dt_masks_blank");
  gtk_widget_set_can_focus(blank, FALSE);
  gtk_widget_set_valign(blank, GTK_ALIGN_CENTER);
  gtk_widget_show(blank);
  return blank;
}

// a primary press on the drawer, one its icon did not claim (a badge, a blank
// column), stops here: on the row or header it would toggle the selection.
// Right-click still reaches the actions menu
static void _header_drawer_pressed(GtkGestureSingle *gesture,
                                   const int n_press,
                                   const double x,
                                   const double y,
                                   gpointer user_data)
{
  if(gtk_gesture_single_get_current_button(gesture) == GDK_BUTTON_PRIMARY)
    dt_gui_claim(gesture);
}

// one drawer column holding two half-size icons, one above the other: laid
// over a blank cell, which alone gives the column its size, so it is exactly
// one icon wide and tall. Each half keeps its place when the other is
// missing, so a badge always sits at the top of its column
static GtkWidget *_header_stacked_cell(GtkWidget *top, GtkWidget *bottom)
{
  GtkWidget *halves = dt_gui_vbox();
  gtk_box_set_homogeneous(GTK_BOX(halves), TRUE);
  GtkWidget *half[2] = { top, bottom };
  for(int i = 0; i < 2; i++)
  {
    GtkWidget *w = half[i] ? half[i] : dt_gui_hbox();
    // filling it: an icon button centered in its half would take its own
    // height, which is nothing once its padding and minimum size are gone
    gtk_widget_set_halign(w, GTK_ALIGN_FILL);
    gtk_widget_set_valign(w, GTK_ALIGN_FILL);
    gtk_widget_show(w);
    dt_gui_box_add(halves, w);
  }
  gtk_widget_show(halves);

  GtkWidget *cell = gtk_overlay_new();
  gtk_container_add(GTK_CONTAINER(cell), _header_blank_cell());
  gtk_overlay_add_overlay(GTK_OVERLAY(cell), halves);
  gtk_widget_set_valign(cell, GTK_ALIGN_CENTER);
  gtk_widget_show(cell);
  return cell;
}

// pack a row header, shared by element rows, the pending row and group
// headers: <handle> <name, expanding> then, in one drawer that shares a single
// background, fixed columns counted from the right: <expander> <visibility>
// and the kind icon with the warning badge. The badge is half size, at the top
// of the first free column: over a group's half-size notes toggle
// (`stack_kind`), in the kind column of an element that has no kind icon, or
// in a column of its own beside a parametric row's picker or a link. Half
// size and stacked, the two cost one column, where they took two and kept the
// panel from fitting the narrowest side panel. A column whose icon the row
// does not have stays blank, so the icons keep their places.
// - badge: the warning badge (see _make_lowop_badge); NULL on the pending row
// - kind_icon: a group's notes toggle, a parametric row's picker, or the link
//   of a linked shape or a raster mask; NULL for none
// - stack_kind: kind_icon goes half size under the badge (a group's notes)
// - visibility: see _make_visibility_button; NULL on the pending row
// - expander: expand/collapse toggle; NULL for a row with nothing to expand
static void _pack_row_header(GtkWidget *row,
                             GtkWidget *handle,
                             GtkWidget *name,
                             GtkWidget *badge,
                             GtkWidget *kind_icon,
                             const gboolean stack_kind,
                             GtkWidget *visibility,
                             GtkWidget *expander)
{
  GtkWidget *hbox = dt_gui_hbox();

  if(handle) dt_gui_box_add(hbox, handle);
  if(name) dt_gui_box_add(hbox, dt_gui_expand(name));

  if(expander)
  {
    dt_gui_add_class(expander, "dt_masks_icon");
    dt_gui_add_class(expander, "dt_masks_expander");
    dt_gui_add_class(expander, "dt_transparent_background");
    gtk_widget_set_valign(expander, GTK_ALIGN_CENTER);
  }

  GtkWidget *drawer = dt_gui_hbox();
  dt_gui_add_class(drawer, "dt_masks_drawer");
  gtk_widget_set_valign(drawer, GTK_ALIGN_CENTER);
  dt_gui_connect_click(drawer, _header_drawer_pressed, NULL, NULL);
  gtk_box_pack_end(GTK_BOX(hbox), drawer, FALSE, FALSE, 0);

  // the columns, right to left. Each is an icon button sized by the theme,
  // like the icons of the module's sub-panel headers, so the glyphs match
  // theirs; an empty column is a blank button of the same size
  GtkWidget *cells[4] = { expander, visibility, NULL, NULL };
  int n_cells = 3;
  if(stack_kind || !kind_icon)
    cells[2] = _header_stacked_cell(badge, kind_icon);
  else
  {
    cells[2] = kind_icon;
    cells[3] = _header_stacked_cell(badge, NULL);
    n_cells = 4;
  }
  for(int i = 0; i < n_cells; i++)
    gtk_box_pack_end(GTK_BOX(drawer), cells[i] ? cells[i] : _header_blank_cell(),
                     FALSE, FALSE, 0);


  dt_gui_box_add(row, dt_gui_expand(hbox));
}

// record each leaf shape's effective hidden state, its own bit or that of any
// enclosing group, by formid. dev->form_visible holds the leaves of nested
// groups flattened (dt_masks_group_ungroup), so a group's state has to reach
// them, or hiding a nested group would leave its outlines drawn
static void _collect_effective_hidden(dt_masks_form_t *grp,
                                      const gboolean inherited_hidden,
                                      GHashTable *hidden_by_formid,
                                      const int depth)
{
  if(!grp || !(grp->type & DT_MASKS_GROUP) || depth > DT_MASKS_NESTING_MAX) return;
  gboolean group_bypassed = FALSE;
  for(GList *l = grp->points; l; l = g_list_next(l))
  {
    const dt_masks_point_group_t *pt = l->data;
    // a bypassed group's members render nothing
    if(dt_masks_point_is_marker(pt))
    {
      group_bypassed = _op_is_bypassed(pt->state);
      continue;
    }
    const gboolean hidden =
      inherited_hidden || (pt->state & (DT_MASKS_STATE_HIDDEN | DT_MASKS_STATE_DISABLE))
      || group_bypassed;
    dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, pt->formid);
    if(form && (form->type & DT_MASKS_GROUP))
      _collect_effective_hidden(form, hidden, hidden_by_formid, depth + 1);
    else
      g_hash_table_insert(hidden_by_formid, GINT_TO_POINTER(pt->formid),
                          GINT_TO_POINTER(hidden ? 1 : 0));
  }
}

// the canvas overlay (dev->form_visible) is a flattened copy of the group,
// made on entering edit mode, so hide and solo, which change the stored group,
// are copied onto it by formid here. A shape that became hidden also loses the
// selection, or it would stay highlighted
static void _sync_hidden_to_form_visible(dt_iop_module_t *module)
{
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  dt_masks_form_t *vis = darktable.develop ? darktable.develop->form_visible : NULL;
  if(!grp || !vis || !(vis->type & DT_MASKS_GROUP)) return;

  GHashTable *hidden = g_hash_table_new(g_direct_hash, g_direct_equal);
  _collect_effective_hidden(grp, FALSE, hidden, 0);

  for(GList *l = vis->points; l; l = g_list_next(l))
  {
    dt_masks_point_group_t *vp = l->data;
    gpointer val = NULL;
    if(!g_hash_table_lookup_extended(hidden, GINT_TO_POINTER(vp->formid), NULL, &val))
      continue;
    if(GPOINTER_TO_INT(val))
      vp->state |= DT_MASKS_STATE_HIDDEN;
    else
      vp->state &= ~DT_MASKS_STATE_HIDDEN;
  }
  g_hash_table_destroy(hidden);

  // a hidden shape must not remain the selected/edited one (its row would stay
  // highlighted and its canvas outline drawn as selected)
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(bd && dt_is_valid_maskid(bd->panel_selected_formid))
  {
    const dt_masks_point_group_t *selp = dt_masks_gui_group_point(grp, bd->panel_selected_formid);
    if(selp && (selp->state & DT_MASKS_STATE_HIDDEN))
    {
      bd->panel_selected_formid = INVALID_MASKID;
      if(darktable.develop->form_gui)
        darktable.develop->form_gui->panel_selected_formid = INVALID_MASKID;
    }
  }

  dt_control_queue_redraw_center();
}

// the mask list's rows and headers carry a tag (object data such as "mask-row"
// or "mask-header") and sit nested in expanders and boxes at varying depths:
// these two walk the subtree under `w`, `w` excluded, to reach them.
// _foreach_tagged calls `fn` on each tagged widget and does not look inside it
static void _foreach_tagged(GtkWidget *w,
                            const char *tag,
                            void (*fn)(GtkWidget *tagged, gpointer data),
                            gpointer data)
{
  if(!GTK_IS_CONTAINER(w)) return;
  GList *kids = gtk_container_get_children(GTK_CONTAINER(w));
  for(GList *c = kids; c; c = g_list_next(c))
  {
    GtkWidget *child = c->data;
    if(g_object_get_data(G_OBJECT(child), tag))
      fn(child, data);
    else
      _foreach_tagged(child, tag, fn, data);
  }
  g_list_free(kids);
}

// the first tagged widget `match` accepts, or NULL
static GtkWidget *_find_tagged(GtkWidget *w,
                               const char *tag,
                               gboolean (*match)(GtkWidget *tagged, gconstpointer data),
                               gconstpointer data)
{
  if(!GTK_IS_CONTAINER(w)) return NULL;
  GtkWidget *found = NULL;
  GList *kids = gtk_container_get_children(GTK_CONTAINER(w));
  for(GList *c = kids; c && !found; c = g_list_next(c))
  {
    GtkWidget *child = c->data;
    if(g_object_get_data(G_OBJECT(child), tag) && match(child, data))
      found = child;
    else
      found = _find_tagged(child, tag, match, data);
  }
  g_list_free(kids);
  return found;
}

// a group header's cid (see the header build in dt_masks_gui_build_list)
static inline dt_mask_id_t _header_cid(GtkWidget *header)
{
  return (dt_mask_id_t)GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(header), "group-key"));
}

static gboolean _header_has_cid(GtkWidget *header, gconstpointer cid)
{
  return _header_cid(header) == GPOINTER_TO_INT(cid);
}

static gboolean _has_class(GtkWidget *w, const char *cls)
{
  return w && gtk_style_context_has_class(gtk_widget_get_style_context(w), cls);
}

// the header line of an element row (tagged "mask-row"), which carries the
// row's state classes: it is the part they shade. The row itself when it has
// none, as a pending one
static GtkWidget *_row_header_line(GtkWidget *row)
{
  GtkWidget *line = g_object_get_data(G_OBJECT(row), "row-hbox");
  return line ? line : row;
}

static void _paint_row_selection(GtkWidget *row, gpointer sel)
{
  const dt_mask_id_t id = GPOINTER_TO_INT(sel);
  if(dt_is_valid_maskid(id) && _widget_id(row, "formid") == id)
    dt_gui_add_class(_row_header_line(row), "dt_masks_selected");
  else
    dt_gui_remove_class(_row_header_line(row), "dt_masks_selected");
}

static void _apply_row_selection(GtkWidget *w, const dt_mask_id_t sel)
{
  _foreach_tagged(w, "mask-row", _paint_row_selection, GINT_TO_POINTER(sel));
}

// same idea as _apply_row_selection, but for a group's header (tagged "mask-header"
// at construction, with "group-key" holding its cid): `cls` goes on its header
// row ("group-header-widget"), which it shades
static void _paint_group_header(GtkWidget *header,
                                const char *cls,
                                const gboolean on)
{
  GtkWidget *row = g_object_get_data(G_OBJECT(header), "group-header-widget");
  GtkWidget *target = row ? row : header;
  if(on)
    dt_gui_add_class(target, cls);
  else
    dt_gui_remove_class(target, cls);
}

static void _paint_group_selection(GtkWidget *header, gpointer sel)
{
  const dt_mask_id_t cid = GPOINTER_TO_INT(sel);
  _paint_group_header(header, "dt_masks_selected",
                      dt_is_valid_maskid(cid) && _header_cid(header) == cid);
}

static void _apply_group_selection(GtkWidget *w, const dt_mask_id_t sel)
{
  _foreach_tagged(w, "mask-header", _paint_group_selection, GINT_TO_POINTER(sel));
}

// everything holding the selected row or group header is selected by
// implication, up to the list `list`: the groups (their blocks carry
// "group-key") and the rows of nested groups and of the AI object stepped into
// (tagged "mask-row"). Shaded apart from the selection itself
static void _paint_ancestors_selected(GtkWidget *w, GtkWidget *list)
{
  for(GtkWidget *p = gtk_widget_get_parent(w); p && p != list; p = gtk_widget_get_parent(p))
  {
    if(g_object_get_data(G_OBJECT(p), "mask-row"))
      dt_gui_add_class(_row_header_line(p), "dt_masks_implied");
    else if(g_object_get_data(G_OBJECT(p), "group-key"))
    {
      GtkWidget *header = _find_tagged(p, "mask-header", _header_has_cid,
                                       g_object_get_data(G_OBJECT(p), "group-key"));
      if(header)
        _paint_group_header(header, "dt_masks_implied", TRUE);
    }
  }
}

static void _clear_row_implied(GtkWidget *row, gpointer data)
{
  dt_gui_remove_class(_row_header_line(row), "dt_masks_implied");
}

static void _clear_header_implied(GtkWidget *header, gpointer data)
{
  _paint_group_header(header, "dt_masks_implied", FALSE);
}

typedef struct _ancestor_walk_t
{
  GtkWidget *list;
  dt_mask_id_t formid, cid;
} _ancestor_walk_t;

static void _paint_row_ancestors(GtkWidget *row, gpointer data)
{
  const _ancestor_walk_t *a = data;
  if(_widget_id(row, "formid") == a->formid) _paint_ancestors_selected(row, a->list);
}

static void _paint_header_ancestors(GtkWidget *header, gpointer data)
{
  const _ancestor_walk_t *a = data;
  if(_header_cid(header) == a->cid) _paint_ancestors_selected(header, a->list);
}

// the ancestors of what is explicitly selected: element `formid`, or else
// group `cid`
static void _apply_ancestor_selection(GtkWidget *list,
                                      const dt_mask_id_t formid,
                                      const dt_mask_id_t cid)
{
  _foreach_tagged(list, "mask-row", _clear_row_implied, NULL);
  _foreach_tagged(list, "mask-header", _clear_header_implied, NULL);
  _ancestor_walk_t a = { list, formid, cid };
  if(dt_is_valid_maskid(formid))
    _foreach_tagged(list, "mask-row", _paint_row_ancestors, &a);
  else if(dt_is_valid_maskid(cid))
    _foreach_tagged(list, "mask-header", _paint_header_ancestors, &a);
}

// what is explicitly selected: the element, or else its group. With an
// element selected its group is selected too, but only by implication
static inline dt_mask_id_t _explicit_group_cid(const dt_iop_gui_blend_data_t *bd)
{
  return dt_is_valid_maskid(bd->panel_selected_formid) ? INVALID_MASKID
                                                       : bd->panel_selected_group_cid;
}

// set each group header's visibility button ("visibility-btn") from the solo
// state. Soloing an element refreshes only the element rows
// (_refresh_all_shape_rows), and can clear a group's solo, so the headers
// need this too
static void _paint_group_visibility(GtkWidget *header, gpointer solo_key)
{
  const guint key = GPOINTER_TO_UINT(solo_key);
  GtkWidget *visibility = g_object_get_data(G_OBJECT(header), "visibility-btn");
  const gboolean bypassed = g_object_get_data(G_OBJECT(header), "group-bypassed") != NULL;
  _set_visibility_status(visibility, bypassed ? MASK_VISIBILITY_DISABLED
                                     : (key != 0 && key == (guint)_header_cid(header))
                                       ? MASK_VISIBILITY_SOLO
                                       : MASK_VISIBILITY_SHOWN);
}

static void _apply_group_visibility(GtkWidget *w, const guint solo_key)
{
  _foreach_tagged(w, "mask-header", _paint_group_visibility, GUINT_TO_POINTER(solo_key));
}

// does the group whose members start at list node `first` read as solo-
// suppressed? Only when nothing in it is used: solo is the only thing that
// hides members (DT_MASKS_STATE_HIDDEN), and it keeps visible the soloed group,
// the element soloed and every group holding it, at any depth. An empty group
// never holds what is soloed, so it dims while any solo is on. The one rule for
// a header built with the list and one refreshed in place
static gboolean _group_solo_suppressed(const dt_iop_gui_blend_data_t *bd, GList *first)
{
  gboolean empty = TRUE;
  for(GList *m = first; m; m = g_list_next(m))
  {
    const dt_masks_point_group_t *pm = m->data;
    // a member the list drops (see _pack_group) does not count either
    if(!dt_masks_get_from_id(darktable.develop, pm->formid)) continue;
    if(!(pm->state & DT_MASKS_STATE_HIDDEN)) return FALSE;
    empty = FALSE;
  }
  return !empty || dt_is_valid_maskid(bd->solo_formid) || bd->solo_group_key != 0;
}

// dim a group header while solo leaves nothing in its group in use
// (_group_solo_suppressed), as element rows are dimmed
typedef struct _header_dimming_t
{
  const dt_iop_gui_blend_data_t *bd;
  dt_masks_form_t *grp;
} _header_dimming_t;

static void _dim_group_header(GtkWidget *header, gpointer data)
{
  const _header_dimming_t *d = data;
  const guint cid = (guint)_header_cid(header);
  const guint solo_group_key = d->bd->solo_group_key;
  GtkWidget *target = g_object_get_data(G_OBJECT(header), "group-header-widget");
  dt_masks_form_t *g = _group_of(d->grp, (dt_mask_id_t)cid);
  const gboolean suppressed = _group_solo_suppressed(d->bd, g ? g->points->next : NULL);
  const gboolean bypassed = g_object_get_data(G_OBJECT(header), "group-bypassed") != NULL;
  if(target)
  {
    if(bypassed)
    {
      GtkWidget *ghandle = g_object_get_data(G_OBJECT(header), "ghandle-widget");
      GtkWidget *lbl_box = g_object_get_data(G_OBJECT(header), "title-label-box");
      GtkWidget *labevt = lbl_box ? gtk_widget_get_parent(lbl_box) : NULL;
      if(ghandle) gtk_widget_set_opacity(ghandle, MASK_DIMMED_OPACITY);
      if(labevt) gtk_widget_set_opacity(labevt, MASK_DIMMED_OPACITY);
      gtk_widget_set_opacity(target, 1.0);
    }
    else
    {
      gtk_widget_set_opacity(target, suppressed ? MASK_DIMMED_OPACITY : 1.0);
    }
  }
  // tag the *soloed* group's whole block so its own cluster headers stay lit
  // (they dim by default under .dt_masks_solo_active -- see darktable.css); a
  // group is being shown in full, so nothing inside it should read as
  // suppressed. Other groups' blocks keep the tag off, so their clusters dim.
  GtkWidget *block = g_object_get_data(G_OBJECT(header), "header-widget");
  if(block)
  {
    if(solo_group_key != 0 && cid == solo_group_key)
      dt_gui_add_class(block, "dt_masks_soloed");
    else
      dt_gui_remove_class(block, "dt_masks_soloed");
  }
}

static void _apply_group_header_dimming(GtkWidget *w,
                                        const dt_iop_gui_blend_data_t *bd,
                                        dt_masks_form_t *grp)
{
  _header_dimming_t d = { bd, grp };
  _foreach_tagged(w, "mask-header", _dim_group_header, &d);
}

// set one group's operator handle for "invert output"
// (_group_toggle_output_invert) in place: no row changes, so there is nothing
// to rebuild
static void
_apply_group_output_invert_icon(GtkWidget *w, const guint cid, const gboolean inverted)
{
  GtkWidget *header = _find_tagged(w, "mask-header", _header_has_cid, GUINT_TO_POINTER(cid));
  GtkWidget *ghandle = header ? g_object_get_data(G_OBJECT(header), "ghandle-widget") : NULL;
  if(!ghandle) return;
  if(inverted)
    dt_gui_add_class(ghandle, "dt_masks_inverted");
  else
    dt_gui_remove_class(ghandle, "dt_masks_inverted");
  gtk_widget_queue_draw(ghandle);
}

static gboolean _row_has_formid(GtkWidget *row, gconstpointer formid)
{
  return _widget_id(row, "formid") == GPOINTER_TO_INT(formid);
}

static GtkWidget *_find_row_by_formid(GtkWidget *w, const dt_mask_id_t formid)
{
  return _find_tagged(w, "mask-row", _row_has_formid, GINT_TO_POINTER(formid));
}

// every row showing form `formid`, in build order. One mask can reference the
// same shape more than once (a group linking a shape another group defines),
// and each reference gets its own row, so this is a list rather than one
// widget. Owned by the map; the caller does not free it
static GSList *_masks_rows_for_form(dt_iop_gui_blend_data_t *bd,
                                    const dt_mask_id_t formid)
{
  if(!bd || !bd->masks_row_map || !dt_is_valid_maskid(formid)) return NULL;
  return g_hash_table_lookup(bd->masks_row_map, GINT_TO_POINTER(formid));
}

// O(1) shape-row lookup by form id via the masks_row_map index (see blend.h),
// used by every per-formid whole-list lookup instead of a recursive tree walk.
// Falls back to the tree walk if the map is somehow cold, so behavior is never
// worse than before. Where the same shape is referenced twice this returns the
// first of its rows: anything refreshing one row per reference wants
// _masks_row_for_point instead.
static GtkWidget *_masks_row_widget(dt_iop_gui_blend_data_t *bd,
                                    const dt_mask_id_t formid)
{
  if(!bd || !dt_is_valid_maskid(formid)) return NULL;
  GSList *rows = _masks_rows_for_form(bd, formid);
  GtkWidget *w = rows ? rows->data : NULL;
  if(!w && bd->masks_list_box)
    w = _find_row_by_formid(GTK_WIDGET(bd->masks_list_box), formid);
  return w;
}

// the row built for this exact reference. Each row is tagged with the member
// point it was built from (see _make_shape_row), compared by identity only,
// so a pointer left over from an edit not yet rebuilt matches nothing and is
// never dereferenced. A shape used twice has a row per reference
static GtkWidget *_masks_row_for_point(dt_iop_gui_blend_data_t *bd,
                                       const dt_masks_point_group_t *pt)
{
  if(!bd || !pt) return NULL;
  GSList *rows = _masks_rows_for_form(bd, pt->formid);
  for(GSList *r = rows; r; r = r->next)
    if(g_object_get_data(G_OBJECT(r->data), "row-point") == pt)
      return r->data;
  // only one row can be meant when the shape is referenced once (and this is
  // also the cold-map fallback); with several, a point that matches none of
  // them is stale, and painting an arbitrary row from it is what this avoids
  return rows && rows->next ? NULL : _masks_row_widget(bd, pt->formid);
}

// the editor of the parametric row of form `formid`, or NULL. The row's box
// carries it (see _build_param_row_editor)
static dt_masks_param_row_editor_t *_param_row_editor(dt_iop_gui_blend_data_t *bd,
                                                      const dt_mask_id_t formid)
{
  GtkWidget *row_vbox = _masks_row_widget(bd, formid);
  GtkWidget *editor_box =
    row_vbox ? g_object_get_data(G_OBJECT(row_vbox), "param-editor-box") : NULL;
  return editor_box ? g_object_get_data(G_OBJECT(editor_box), "param-editor") : NULL;
}

// the panel's three DnD payload types, each used in several GtkTargetEntry
// tables below: a typo in one copy would fail silently, as a drag that never
// matches
#define DND_TARGET_ROW "dt-mask-row"
#define DND_TARGET_GROUP "dt-mask-group"
#define DND_TARGET_CLUSTER "dt-mask-cluster"

// an element dragged by its handle or its name, which carry its form id
static const GtkTargetEntry _mask_row_dnd[] = { { (gchar *)DND_TARGET_ROW,
                                                  GTK_TARGET_SAME_APP, 0 } };

// a badge (the warning) is always mapped, in its drawer column (see
// _pack_row_header): hiding it would shift the other header controls. An
// "active" flag, read by its draw handler, stands in: inactive paints nothing.
// An inactive badge has no tooltip either
static void _set_badge_active(GtkWidget *badge,
                              const gboolean active,
                              const char *tooltip_when_active)
{
  if(!badge) return;
  g_object_set_data(G_OBJECT(badge), "badge-active", GINT_TO_POINTER(active));
  gtk_widget_set_tooltip_text(badge, active ? tooltip_when_active : NULL);
  gtk_widget_queue_draw(badge);
}

static gboolean _badge_is_active(GtkWidget *badge)
{
  return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(badge), "badge-active"));
}

// what the visibility button says in each state, and what a click and a
// shift+click do from there. Solo is offered where the actions menu offers it
// (see _build_shape_actions_menu, _build_group_actions_menu): never on
// something disabled, nor on an empty group
static const char *_visibility_tooltip(const int status, const gboolean can_solo)
{
  if(status == MASK_VISIBILITY_DISABLED)
    return _("disabled: adds nothing to the mask\n"
             "click to enable");
  if(status == MASK_VISIBILITY_SOLO)
    return _("soloed: only this is used\n"
             "click to disable\n"
             "shift+click to clear solo");
  return can_solo ? _("click to disable: add nothing to the mask\n"
                      "shift+click to solo: use only this")
                  : _("click to disable: add nothing to the mask");
}

static int _visibility_status_get(GtkWidget *btn)
{
  return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "visibility-status"));
}

// an open eye, the solo eye or a crossed-out eye. Solo edit has no state
// here: it is a mode (see _soloedit_follow_selection) that follows the
// selection, so the selection highlight already says what it is doing
static void _set_visibility_status(GtkWidget *btn, const int status)
{
  if(!btn) return;
  g_object_set_data(G_OBJECT(btn), "visibility-status", GINT_TO_POINTER(status));
  dtgtk_button_set_paint(DTGTK_BUTTON(btn),
                         status == MASK_VISIBILITY_SOLO ? dtgtk_cairo_paint_eye_solo
                                                        : dtgtk_cairo_paint_eye_toggle,
                         status == MASK_VISIBILITY_DISABLED ? CPF_ACTIVE : 0, NULL);
  // lit while solo or disable is on, like a bypassed channel's eye
  if(status == MASK_VISIBILITY_SOLO)
    dt_gui_add_class(btn, "dt_masks_soloed");
  else
    dt_gui_remove_class(btn, "dt_masks_soloed");
  if(status == MASK_VISIBILITY_DISABLED)
    dt_gui_add_class(btn, "dt_masks_disabled");
  else
    dt_gui_remove_class(btn, "dt_masks_disabled");
  const gboolean can_solo = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "can-solo"));
  gtk_widget_set_tooltip_text(btn, _visibility_tooltip(status, can_solo));
  gtk_widget_queue_draw(btn);
}

// the second icon from the right on every element row and group header (see
// _pack_row_header). The caller connects its press handler and sets its status
static GtkWidget *_make_visibility_button(const gboolean can_solo)
{
  GtkWidget *btn = dtgtk_button_new(dtgtk_cairo_paint_eye_toggle, 0, NULL);
  dt_gui_add_class(btn, "dt_masks_icon");
  dt_gui_add_class(btn, "dt_masks_eye");
  gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
  g_object_set_data(G_OBJECT(btn), "can-solo", GINT_TO_POINTER(can_solo));
  return btn;
}

// --- warning badge -----------------------------------------------------------
// opacity goes down to 0 (see the CLAMP in _props_row_apply), so an element or
// group that does nothing, or next to nothing, says so on its row, and every
// group holding one on its header: what makes a mask do nothing shows with
// the groups folded (see _refresh_lowop_badges)
#define MASK_LOW_OPACITY_WARN 0.10f

// painted by hand: GtkDarktableIcon never calls gtk_render_background, so a
// plain icon child would leave the CSS-styled badge background unpainted. A
// plain triangle in the theme's color, outlined in its outline-color so it
// reads on every header shade, from the darkest to a selected one
static gboolean _lowop_badge_draw(GtkWidget *w, cairo_t *cr, gpointer user_data)
{
  if(!_badge_is_active(w)) return FALSE;
  GtkAllocation a;
  gtk_widget_get_allocation(w, &a);
  GtkStyleContext *ctx = gtk_widget_get_style_context(w);
  const GtkStateFlags state = gtk_widget_get_state_flags(w);

  gtk_render_background(ctx, cr, 0, 0, a.width, a.height);
  // padding insets the glyph, as on the drawer's other icons
  GtkBorder pad;
  gtk_style_context_get_padding(ctx, state, &pad);
  GdkRGBA fill, edge;
  gtk_style_context_get_color(ctx, state, &fill);
  GdkRGBA *outline = NULL;
  gtk_style_context_get(ctx, state, "outline-color", &outline, NULL);
  edge = outline ? *outline : (GdkRGBA){ 0.0, 0.0, 0.0, 0.6 };
  if(outline) gdk_rgba_free(outline);

  // the outline is stroked inside the box, half its width in from each edge
  const double lw = DT_PIXEL_APPLY_DPI(1.0);
  const double bw = a.width - pad.left - pad.right - lw;
  const double bh = a.height - pad.top - pad.bottom - lw;
  if(bw <= 0.0 || bh <= 0.0) return FALSE;
  // as wide as it is tall, centered in what the padding leaves
  const double side = MIN(bw, bh);
  const double x0 = pad.left + lw / 2.0 + (bw - side) / 2.0;
  const double y0 = pad.top + lw / 2.0 + (bh - side) / 2.0;
  cairo_move_to(cr, x0 + side / 2.0, y0);
  cairo_line_to(cr, x0 + side, y0 + side);
  cairo_line_to(cr, x0, y0 + side);
  cairo_close_path(cr);
  cairo_set_source_rgba(cr, fill.red, fill.green, fill.blue, fill.alpha);
  cairo_fill_preserve(cr);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_set_line_width(cr, lw);
  cairo_set_source_rgba(cr, edge.red, edge.green, edge.blue, edge.alpha);
  cairo_stroke(cr);
  return FALSE;
}

// starts inactive (blank); _refresh_lowop_badges reveals it in place, no
// list rebuild needed. Not clickable: it reports state the row's own controls
// own, so there is nothing for a click to do. Sized where it is packed: the
// drawer's 18px column (see _pack_row_header), or beside the blend opacity
static GtkWidget *_make_lowop_badge(void)
{
  GtkWidget *badge = gtk_event_box_new();
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(badge), TRUE);
  gtk_widget_set_app_paintable(badge, TRUE);
  dt_gui_add_class(badge, "dt_masks_badge");
  g_signal_connect(G_OBJECT(badge), "draw", G_CALLBACK(_lowop_badge_draw), NULL);
  return badge;
}

// show a badge with `tooltip`, or blank it for NULL. `no_effect`, when the
// first reason given is that something does nothing at all rather than
// little, only adds .dt_masks_no_effect, for a theme that wants to tell the two
// apart: the glyph is the same
static void _set_badge(GtkWidget *badge, const gchar *tooltip, const gboolean no_effect)
{
  if(!badge) return;
  if(no_effect && tooltip)
    dt_gui_add_class(badge, "dt_masks_no_effect");
  else
    dt_gui_remove_class(badge, "dt_masks_no_effect");
  _set_badge_active(badge, tooltip != NULL, tooltip);
}

// the module's own blend opacity, next to its value. Says the actual value:
// "low" alone doesn't tell 9% from 0%, which read very differently
static void _update_blend_opacity_badge(GtkWidget *badge, const float opacity)
{
  if(opacity >= MASK_LOW_OPACITY_WARN)
  {
    _set_badge(badge, NULL, FALSE);
    return;
  }
  gchar *tip = opacity <= 0.0f
    ? g_strdup(_("opacity 0%: this module has no effect on the image"))
    : g_strdup_printf(_("opacity %.0f%%: this module has very little effect on the image"),
                      opacity * 100.0f);
  _set_badge(badge, tip, FALSE);
  g_free(tip);
}

// paint one element row from `pt`'s current state, in place: its solo
// highlight, visibility button, inverted handle, and the dimming and
// insensitivity of an element that contributes nothing to the mask (disabled,
// solo-suppressed, or in a bypassed group). _make_shape_row ends here too, so
// a built row and a refreshed one cannot differ; the solo/invert/disable
// handlers call it instead of rebuilding the list, which visibly flashes the
// panel. The row itself stays interactive (selectable, draggable, soloable)
// whatever its state: only its controls are made insensitive
static void _update_shape_row_state(dt_iop_gui_blend_data_t *bd,
                                    GtkWidget *row_vbox,
                                    const dt_masks_point_group_t *pt)
{
  if(!row_vbox) return;
  const gboolean elem_disabled = (pt->state & DT_MASKS_STATE_DISABLE) != 0;
  const gboolean suppressed =
    (pt->state & DT_MASKS_STATE_HIDDEN)
    || _member_group_bypassed(dt_masks_gui_module_mask_group(bd->module), pt->formid);
  const gboolean no_effect = suppressed || elem_disabled;
  const gboolean solo = bd->solo_formid == pt->formid;

  // a soloed element stays highlighted like a hovered row, not just while the
  // mouse is over it -- a distinct class from the transient hover wash so it
  // survives hovering elsewhere in the list (see _clear_hover_classes).
  if(solo)
    dt_gui_add_class(row_vbox, "dt_masks_soloed");
  else
    dt_gui_remove_class(row_vbox, "dt_masks_soloed");

  GtkWidget *row = g_object_get_data(G_OBJECT(row_vbox), "row-hbox");
  GtkWidget *handle = g_object_get_data(G_OBJECT(row_vbox), "handle-widget");
  GtkWidget *name_evbox = g_object_get_data(G_OBJECT(row_vbox), "name-evbox");
  GtkWidget *action_icon = g_object_get_data(G_OBJECT(row_vbox), "action-icon");
  GtkWidget *visibility = g_object_get_data(G_OBJECT(row_vbox), "visibility-btn");
  GtkWidget *expand_toggle = g_object_get_data(G_OBJECT(row_vbox), "expand-toggle");

  _set_visibility_status(visibility, elem_disabled ? MASK_VISIBILITY_DISABLED
                                     : solo ? MASK_VISIBILITY_SOLO
                                            : MASK_VISIBILITY_SHOWN);

  if(handle)
  {
    if(pt->state & DT_MASKS_STATE_INVERSE)
      dt_gui_add_class(handle, "dt_masks_inverted");
    else
      dt_gui_remove_class(handle, "dt_masks_inverted");
    gtk_widget_queue_draw(handle);
  }

  // a disabled element dims its own parts, which keeps the visibility button
  // that brings it back at full strength; a suppressed one dims as a whole
  GtkWidget *const parts[] = { handle, name_evbox, action_icon, expand_toggle };
  for(size_t i = 0; i < G_N_ELEMENTS(parts); i++)
    if(parts[i]) gtk_widget_set_opacity(parts[i], elem_disabled ? MASK_DIMMED_OPACITY : 1.0);
  if(row)
    gtk_widget_set_opacity(row, suppressed && !elem_disabled ? MASK_DIMMED_OPACITY : 1.0);

  GtkWidget *param_box = g_object_get_data(G_OBJECT(row_vbox), "param-editor-box");
  GtkWidget *props_box = g_object_get_data(G_OBJECT(row_vbox), "props-editor-box");
  GtkWidget *const controls[] = { expand_toggle, action_icon, param_box, props_box };
  for(size_t i = 0; i < G_N_ELEMENTS(controls); i++)
    if(controls[i]) gtk_widget_set_sensitive(controls[i], !no_effect);

  // a parametric row draws its polarity from this same INVERSE bit, but in its
  // own sliders' markers rather than the handle icon (see
  // _update_param_row_display / _param_row_inverted): refresh it here so every
  // caller flipping the bit stays consistent, whether it came through one row
  // (_invert_element) or all of them at once (_invert_group_members).
  dt_masks_param_row_editor_t *param_ed =
    param_box ? g_object_get_data(G_OBJECT(param_box), "param-editor") : NULL;
  if(param_ed) _update_param_row_display(param_ed);
}

// defined below (it needs the per-row/-header selection appliers); declared
// here so the in-place refresh can also settle the selection, which solo can
// clear out from under it.
static void _update_row_selection(dt_iop_gui_blend_data_t *bd);

// refresh every shape/parametric row currently in the list from the module's
// mask group, in place (see _update_shape_row_state) -- used by solo, which can
// flip the hidden state of every other row at once.
static void _refresh_all_shape_rows(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  if(!bd || !bd->masks_list_box || !grp) return;
  GList *pts = _mask_points(grp);
  for(GList *l = pts; l; l = g_list_next(l))
  {
    dt_masks_point_group_t *pt = l->data;
    GtkWidget *row_vbox = _masks_row_for_point(bd, pt);
    if(row_vbox) _update_shape_row_state(bd, row_vbox, pt);
  }
  g_list_free(pts);
  // an element solo clears any active group solo (see _toggle_solo_form); make
  // sure a group header's own badge follows suit without a full rebuild
  _apply_group_visibility(GTK_WIDGET(bd->masks_list_box), bd->solo_group_key);
  const gboolean solo_active =
    dt_is_valid_maskid(bd->solo_formid) || bd->solo_group_key != 0;
  // same-kind cluster headers dim in CSS (.dt_masks_cluster_header under
  // .dt_masks_solo_active), from this class on the list box
  if(solo_active)
    dt_gui_add_class(GTK_WIDGET(bd->masks_list_box), "dt_masks_solo_active");
  else
    dt_gui_remove_class(GTK_WIDGET(bd->masks_list_box), "dt_masks_solo_active");
  _apply_group_header_dimming(GTK_WIDGET(bd->masks_list_box), bd, grp);
  _refresh_lowop_badges(module);
  // _sync_hidden_to_form_visible, which runs before, drops the selection of a
  // shape that became hidden, and _update_shape_row_state does not paint the
  // selection
  _update_row_selection(bd);
}

// what a group holds, at any depth, as its header's badge reports it
typedef struct _badge_held_t
{
  gboolean noop; // an element that does nothing at all
  gboolean low;  // an element or a group under MASK_LOW_OPACITY_WARN
} _badge_held_t;

// a group header's badge, or a nested group row's, by the group's marker id
typedef struct _badge_header_t
{
  gchar *tip;
  gboolean noop;
} _badge_header_t;

static void _badge_header_free(gpointer data)
{
  _badge_header_t *h = data;
  g_free(h->tip);
  g_free(h);
}

// one line per reason, no-effect first: a group that does nothing whatever
// its opacity says so before anything else. NULL with nothing to report
static gchar *_group_badge_tip(const float opacity, const _badge_held_t *held)
{
  GString *tip = g_string_new(NULL);
  if(held->noop)
    g_string_append(tip, _("this group contains at least one element that has no effect"));
  if(opacity < MASK_LOW_OPACITY_WARN)
  {
    if(tip->len) g_string_append_c(tip, '\n');
    if(opacity <= 0.0f)
      g_string_append(tip, _("opacity 0%: this group is fully transparent and\n"
                             "contributes nothing to the mask"));
    else
      g_string_append_printf(tip, _("opacity %.0f%%: this group has very little effect"
                                    " on the mask"),
                             opacity * 100.0f);
  }
  if(held->low)
  {
    if(tip->len) g_string_append_c(tip, '\n');
    g_string_append_printf(tip, _("this group contains at least one element or group\n"
                                  "with opacity below %.0f%%"),
                           MASK_LOW_OPACITY_WARN * 100.0f);
  }
  return g_string_free(tip, tip->len == 0);
}

// an element's own badge: doing nothing at all trumps doing little, which is
// then moot
static gchar *_element_badge_tip(dt_iop_module_t *module,
                                 const dt_masks_form_t *f,
                                 const dt_masks_point_group_t *pt,
                                 gboolean *noop)
{
  const float opacity = pt->opacity;
  // a raster element that cannot reach a mask can never contribute: the
  // renderer draws it as zero and skips its inversion (see
  // dt_masks_raster_is_unresolved). The wording covers both ways it gets
  // there: a module that is gone (the row is only removable) and one that is
  // merely switched off or no longer masking (fixable at the source)
  *noop = TRUE;
  if(dt_masks_raster_is_unresolved(module, NULL, f))
    return g_strdup(_("this raster mask has no mask to read: the module it came from\n"
                      "is switched off, no longer carries a mask, or is gone, so this\n"
                      "element selects nothing.\n"
                      "restore the source module, or remove this element"));
  if(dt_masks_parametric_is_noop(f, (pt->state & DT_MASKS_STATE_INVERSE) != 0))
    return g_strdup(_("this channel's range still covers its whole span, so it does\n"
                      "not restrict the mask yet: narrow the range to have an effect"));
  *noop = FALSE;
  if(opacity >= MASK_LOW_OPACITY_WARN) return NULL;
  return opacity <= 0.0f
    ? g_strdup(_("opacity 0%: this element is fully transparent and\n"
                 "contributes nothing to the mask"))
    : g_strdup_printf(_("opacity %.0f%%: this element has very little effect on the mask"),
                      opacity * 100.0f);
}

// a group closed by the walk below: its header's badge, and what it passes up
static void _badge_close_group(const dt_masks_point_group_t *marker,
                               const _badge_held_t *held,
                               GHashTable *headers,
                               _badge_held_t *total)
{
  if(!marker)
  {
    // members ahead of any marker: no header of their own to report them
    total->noop |= held->noop;
    total->low |= held->low;
    return;
  }
  _badge_header_t *h = g_malloc0(sizeof(_badge_header_t));
  g_hash_table_insert(headers, GINT_TO_POINTER(marker->formid), h);
  // a bypassed group adds nothing to the mask, which its eye already says
  if(_op_is_bypassed(marker->state)) return;
  h->tip = _group_badge_tip(marker->group_opacity, held);
  h->noop = held->noop;
  total->noop |= held->noop;
  total->low |= held->low || marker->group_opacity < MASK_LOW_OPACITY_WARN;
}

// badge every element row of group form `grp` (the mask, or a group nested in
// it) bottom-up, collect its headers' badges in `headers` by marker id, and
// return what it holds, for the headers around it. What adds nothing to the
// mask anyway (a disabled element, the contents of a bypassed group) gets no
// badge and passes nothing up: its eye already says so
static _badge_held_t _badge_walk(dt_iop_module_t *module,
                                 dt_masks_form_t *grp,
                                 GHashTable *headers,
                                 const int depth)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  _badge_held_t total = { FALSE, FALSE };
  if(!grp || depth > DT_MASKS_NESTING_MAX) return total;

  const dt_masks_point_group_t *marker = NULL;
  _badge_held_t held = { FALSE, FALSE };
  for(GList *l = grp->points; l; l = g_list_next(l))
  {
    const dt_masks_point_group_t *pt = l->data;
    if(dt_masks_point_is_marker(pt))
    {
      _badge_close_group(marker, &held, headers, &total);
      marker = pt;
      held = (_badge_held_t){ FALSE, FALSE };
      continue;
    }
    GtkWidget *row = _masks_row_for_point(bd, pt);
    GtkWidget *badge = row ? g_object_get_data(G_OBJECT(row), "lowop-badge") : NULL;
    const gboolean off = (pt->state & DT_MASKS_STATE_DISABLE)
                         || (marker && _op_is_bypassed(marker->state));
    dt_masks_form_t *f = dt_masks_get_from_id(darktable.develop, pt->formid);
    if(f && f != grp && (f->type & DT_MASKS_GROUP))
    {
      // walked even when off, so the headers inside it are settled too
      const _badge_held_t sub = _badge_walk(module, f, headers, depth + 1);
      if(off)
      {
        _set_badge(badge, NULL, FALSE);
        continue;
      }
      gchar *tip = _group_badge_tip(pt->opacity, &sub);
      _set_badge(badge, tip, sub.noop);
      g_free(tip);
      held.noop |= sub.noop;
      held.low |= sub.low || pt->opacity < MASK_LOW_OPACITY_WARN;
      continue;
    }
    if(off || !f)
    {
      _set_badge(badge, NULL, FALSE);
      continue;
    }
    gboolean noop = FALSE;
    gchar *tip = _element_badge_tip(module, f, pt, &noop);
    _set_badge(badge, tip, noop);
    held.noop |= noop;
    held.low |= !noop && tip != NULL;
    g_free(tip);
  }
  _badge_close_group(marker, &held, headers, &total);
  return total;
}

static void _paint_header_badge(GtkWidget *header, gpointer headers)
{
  const _badge_header_t *h = g_hash_table_lookup(headers, GINT_TO_POINTER(_header_cid(header)));
  _set_badge(g_object_get_data(G_OBJECT(header), "lowop-badge"), h ? h->tip : NULL,
             h && h->noop);
}

// refresh every badge of the module (element rows, group headers, its blend
// opacity) from the current mask, in place: no widget is created or destroyed,
// so it can run on every tick of a drag as well as at the end of a rebuild. A
// group's badge depends on everything it holds, so any change refreshes all
static void _refresh_lowop_badges(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module ? module->blend_data : NULL;
  if(!bd) return;
  if(bd->blend_opacity_lowop_badge && module->blend_params)
    _update_blend_opacity_badge(bd->blend_opacity_lowop_badge,
                                module->blend_params->opacity / 100.0f);
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  if(!bd->masks_list_box || !grp) return;
  GHashTable *headers =
    g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, _badge_header_free);
  _badge_walk(module, grp, headers, 0);
  _foreach_tagged(GTK_WIDGET(bd->masks_list_box), "mask-header", _paint_header_badge, headers);
  g_hash_table_destroy(headers);
}

void dt_iop_gui_blend_refresh_mask_badges(dt_iop_module_t *module)
{
  _refresh_lowop_badges(module);
}

// re-read every value control under w, at any depth: a shape's properties
// editor (a row's expanded controls, or the properties section's), and the
// creation sliders of a shape being drawn, which show conf defaults
static void _reread_value_controls(GtkWidget *w)
{
  dt_masks_props_row_editor_t *ed = g_object_get_data(G_OBJECT(w), "props-editor");
  const char *key = g_object_get_data(G_OBJECT(w), "dt-conf-key");
  if(ed)
  {
    _props_row_populate(ed);
    _props_resize_update(ed);
  }
  else if(key)
  {
    DT_ENTER_GUI_UPDATE();
    dt_bauhaus_slider_set(w, dt_conf_get_float(key));
    DT_LEAVE_GUI_UPDATE();
  }
  else if(GTK_IS_CONTAINER(w))
  {
    GList *kids = gtk_container_get_children(GTK_CONTAINER(w));
    for(GList *k = kids; k; k = g_list_next(k)) _reread_value_controls(k->data);
    g_list_free(kids);
  }
}

void dt_iop_gui_blend_masks_changed(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module ? module->blend_data : NULL;
  // a shape edited on canvas (scrolled, ctrl+scrolled, alt+clicked) and a
  // creation default scrolled while drawing change values the panel shows,
  // without rebuilding it
  if(!bd || !bd->masks_inited || _props_committing) return;
  if(bd->masks_list_box) _reread_value_controls(GTK_WIDGET(bd->masks_list_box));
  if(bd->props_panel_content) _reread_value_controls(bd->props_panel_content);
  // ctrl+scroll on canvas changes an opacity the badges read
  _refresh_lowop_badges(module);
}

// what solo edit leaves on the canvas: the isolated shape, or every member of
// the isolated group. None for an isolated parametric or raster element, which
// has no outline to show: dt_masks_set_edit_mode_forms then leaves the canvas
// empty
static GList *_soloedit_formids(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!bd || !dt_is_valid_maskid(bd->soloedit_formid)) return NULL;
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  const dt_masks_point_group_t *pt = grp ? dt_masks_gui_group_point(grp, bd->soloedit_formid) : NULL;
  if(pt && dt_masks_point_is_marker(pt))
    return dt_masks_model_group_members(grp, bd->soloedit_formid);
  const dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, bd->soloedit_formid);
  if(form && (form->type & (DT_MASKS_PARAMETRIC | DT_MASKS_RASTER))) return NULL;
  return g_list_prepend(NULL, GINT_TO_POINTER(bd->soloedit_formid));
}

// keep the canvas's persistent solo highlight (gui->solo_formids) in step with the
// panel's solo / solo-edit state. Unlike the hover sync (panel_hover_formids,
// cleared the moment the mouse moves elsewhere), this must survive the user
// working anywhere else in the panel or canvas, so it lives in its own list,
// recomputed here whenever solo state changes.
static void _sync_solo_canvas_highlight(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  dt_masks_form_gui_t *gui = darktable.develop->form_gui;
  if(!bd || !gui) return;
  GList *ids = NULL;
  if(dt_is_valid_maskid(bd->solo_formid))
    ids = g_list_prepend(ids, GINT_TO_POINTER(bd->solo_formid));
  ids = g_list_concat(ids, _soloedit_formids(module));
  if(bd->solo_group_key != 0)
  {
    dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
    GList *members = dt_masks_model_group_members(grp, (dt_mask_id_t)bd->solo_group_key);
    ids = g_list_concat(ids, members);
  }
  g_list_free(gui->solo_formids);
  gui->solo_formids = ids;
  dt_control_queue_redraw_center();
}

// flexi: mirror the selected shape's group operator into the new-shape operator
// (defined lower, after the operator helpers).
static void _flexi_new_op_follow_selection(dt_iop_gui_blend_data_t *bd);

// solo-edit is a mode rather than a per-element action: while it is on, canvas
// editing follows the list selection, so only the selected shape, or the
// shapes of the selected group, are shown and grabbable, and clicking down the
// list walks the isolation along with it. It drives bd->soloedit_formid
// through _toggle_soloedit rather than setting the canvas up itself. The header toggle
// is the whole indication that the mode is on; rows carry no solo-edit badge,
// since the isolated element is by definition the selected one.
#define MASKS_SOLOEDIT_MODE_CONF "plugins/darkroom/masks/solo_edit_mode"

static gboolean _soloedit_mode_is_on(void)
{
  return dt_conf_get_bool(MASKS_SOLOEDIT_MODE_CONF);
}

// what the mode would isolate for the current selection: the selected element,
// or the selected group, whose members are then the only shapes left on the
// canvas. A parametric channel or a raster mask is isolated too: it has no
// canvas geometry, so the canvas is left with no shape at all (see
// _soloedit_formids), rather than falling back to every shape of the mask.
// The mask's own group holds every shape, so selecting it is the mode's own off
// state.
dt_mask_id_t dt_masks_model_soloedit_target(dt_iop_gui_blend_data_t *bd)
{
  if(!_soloedit_mode_is_on()) return INVALID_MASKID;
  // solo and solo-edit stay mutually exclusive: while something is soloed the
  // mode stands down rather than canceling the solo behind the user's back
  // (dt_masks_model_toggle_soloedit would clear it). It re-applies on the next
  // selection change once the solo is off.
  if(dt_is_valid_maskid(bd->solo_formid) || bd->solo_group_key != 0)
    return INVALID_MASKID;

  const dt_mask_id_t cid = _explicit_group_cid(bd);
  if(dt_is_valid_maskid(cid))
    return cid != _mask_group_cid(bd->module) ? cid : INVALID_MASKID;
  if(!dt_is_valid_maskid(bd->panel_selected_formid)) return INVALID_MASKID;

  if(!dt_masks_get_from_id(darktable.develop, bd->panel_selected_formid))
    return INVALID_MASKID;
  // a path of the AI object stepped into isolates the object: stepping in is
  // for editing its paths side by side, and narrowing to the path would also
  // rebuild the canvas under the press that selected it
  const dt_mask_id_t entered = _entered_object();
  dt_masks_form_t *obj =
    dt_is_valid_maskid(entered) ? dt_masks_get_from_id(darktable.develop, entered) : NULL;
  if(obj && dt_masks_gui_group_point(obj, bd->panel_selected_formid)) return entered;
  return bd->panel_selected_formid;
}

static void _soloedit_follow_selection(dt_iop_gui_blend_data_t *bd)
{
  // _toggle_soloedit repaints the rows (_refresh_all_shape_rows), whose
  // _update_row_selection lands straight back here in the middle of the toggle.
  // One gesture, one decision
  static gboolean applying = FALSE;
  if(applying) return;

  // the canvas belongs to the focused module: following the selection of any
  // other one (the mode toggle walks every module) would put that module's
  // shapes on it
  if(!darktable.develop || darktable.develop->gui_module != bd->module) return;

  const dt_mask_id_t want = dt_masks_model_soloedit_target(bd);
  if(bd->soloedit_formid == want) return;

  // narrowing the canvas edit scope tears down and rebuilds form_visible, which
  // drops the canvas selection (dt_masks_clear_form_gui): put it back
  // afterwards. The panel keeps its own (see dt_iop_gui_blend_masks_select_form)
  const dt_mask_id_t canvas_sel = darktable.develop->mask_form_selected_id;

  applying = TRUE;
  // _toggle_soloedit is a toggle, so "off" means feeding it back the id that is
  // currently isolated
  _toggle_soloedit(bd->module,
                   dt_is_valid_maskid(want) ? want : bd->soloedit_formid);
  applying = FALSE;
  darktable.develop->mask_form_selected_id = canvas_sel;
}

// the solo-edit mode toggle, and the <blending> action bound to it. Global like
// the channel-preview mode next to it, so it survives moving between modules;
// applying it to every module also lets whichever one is showing its panel pick
// the change up without a further click.
static void _soloedit_mode_toggled(GtkGestureSingle *gesture,
                                   gint n_press,
                                   gdouble x,
                                   gdouble y,
                                   dt_iop_module_t *module)
{
  DT_GUARD_GUI_UPDATE();

  const gboolean on = !_soloedit_mode_is_on();
  dt_conf_set_bool(MASKS_SOLOEDIT_MODE_CONF, on);

  DT_ENTER_GUI_UPDATE();
  for(GList *m = darktable.develop ? darktable.develop->iop : NULL;
      m;
      m = g_list_next(m))
  {
    dt_iop_gui_blend_data_t *bd = ((dt_iop_module_t *)m->data)->blend_data;
    if(bd && bd->soloedit_mode)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->soloedit_mode), on);
  }
  DT_LEAVE_GUI_UPDATE();

  for(GList *m = darktable.develop ? darktable.develop->iop : NULL;
      m;
      m = g_list_next(m))
  {
    dt_iop_gui_blend_data_t *bd = ((dt_iop_module_t *)m->data)->blend_data;
    if(bd && bd->masks_list_box) _soloedit_follow_selection(bd);
  }
}

// lightweight: update only the selected-row border on the existing rows, without
// rebuilding the list (so it is safe to call from a button-press handler, where a
// full rebuild would destroy the row mid-press and break drag-and-drop). Also
// mirrors the persistent selection onto the canvas (gui->panel_selected_formid,
// drawn when nothing is hovered), and lets everything that follows the
// selection follow it
static void _update_row_selection(dt_iop_gui_blend_data_t *bd)
{
  if(!bd || !bd->masks_list_box) return;
  // every route that clears the group selection ends here
  _select_mask_group_if_none(bd);
  // every group's element rows are nested inside masks_list_box (under their header)
  _apply_row_selection(GTK_WIDGET(bd->masks_list_box), bd->panel_selected_formid);
  _apply_group_selection(GTK_WIDGET(bd->masks_list_box), _explicit_group_cid(bd));
  _apply_ancestor_selection(GTK_WIDGET(bd->masks_list_box), bd->panel_selected_formid,
                            bd->panel_selected_group_cid);
  if(darktable.develop && darktable.develop->form_gui)
    darktable.develop->form_gui->panel_selected_formid = bd->panel_selected_formid;
  _flexi_new_op_follow_selection(bd);
  _flexi_refine_follow_selection(bd);
  _soloedit_follow_selection(bd);
  _props_panel_sync(bd->module, FALSE);
  dt_control_queue_redraw_center();
}

// drop the transient hover wash from every header in the list
static void _clear_hover_classes(GtkWidget *w)
{
  if(!GTK_IS_WIDGET(w)) return;
  dt_gui_remove_class(w, "dt_masks_hovered");
  if(!GTK_IS_CONTAINER(w)) return;
  GList *kids = gtk_container_get_children(GTK_CONTAINER(w));
  for(GList *c = kids; c; c = g_list_next(c)) _clear_hover_classes(c->data);
  g_list_free(kids);
}

// find a group header whose member set includes formid (group headers carry
// their member ids in "group-formids"). Used as the fallback below when a
// shape's own nested row cannot be found directly, so its group header is
// highlighted instead.
static gboolean _header_has_member(GtkWidget *header, gconstpointer formid)
{
  GList *members = g_object_get_data(G_OBJECT(header), "group-formids");
  return g_list_find(members, formid) != NULL;
}

static GtkWidget *_find_collapsed_cluster_header(GtkWidget *w, const dt_mask_id_t formid)
{
  return _find_tagged(w, "group-formids", _header_has_member, GINT_TO_POINTER(formid));
}

// a path of an AI object is selected through its object, whose row stands for
// it, unless the object is stepped into: its paths then have rows of their own
dt_mask_id_t dt_masks_model_panel_formid_for(dt_iop_module_t *module, const dt_mask_id_t formid)
{
  if(!dt_is_valid_maskid(formid)) return INVALID_MASKID;
  dt_masks_form_t *mgrp = dt_masks_gui_module_mask_group(module);
  if(!mgrp || dt_masks_gui_group_point(mgrp, formid)) return formid;
  // an object in a nested group too
  dt_mask_id_t out = formid;
  GList *pts = _mask_points(mgrp);
  for(GList *l = pts; l; l = g_list_next(l))
  {
    dt_masks_form_t *f =
      dt_masks_get_from_id(darktable.develop, ((dt_masks_point_group_t *)l->data)->formid);
    if(f && (f->type & DT_MASKS_OBJECT) && dt_masks_gui_group_point(f, formid))
    {
      out = f->formid == _entered_object() ? formid : f->formid;
      break;
    }
  }
  g_list_free(pts);
  return out;
}

// canvas -> list selection sync: when a shape is selected on the canvas (click),
// highlight its row in the flexi mask list. No-op when there is no list
void dt_iop_gui_blend_masks_select_form(dt_iop_module_t *module, const dt_mask_id_t formid)
{
  if(!module) return;
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!bd || !bd->masks_list_box) return;
  const dt_mask_id_t id = dt_masks_model_panel_formid_for(module, formid);
  // a canvas with nothing selected leaves the panel's selection alone: the
  // canvas drops its own on every rebuild (dt_masks_clear_form_gui), undo's
  // included (libs/history.c _pop_undo), while the user's deselects reach the
  // panel on their own (an empty-canvas click, dt_iop_gui_blend_masks_clear_selection;
  // a delete, _clear_stale_formid_refs)
  if(!dt_is_valid_maskid(id) || bd->panel_selected_formid == id) return;
  bd->panel_selected_formid = id;

  // mirror the group the shape belongs to, same as a list click (_select_form),
  // so a canvas click also highlights/expands the group the shape lives in.
  // Unlike _set_group_target, panel_selected_formid is left set here so the
  // specific row within the group still gets its own highlight too
  dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, id);
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  bd->panel_selected_group_cid = (form && !(form->type & DT_MASKS_PARAMETRIC) && grp)
                                   ? dt_masks_gui_group_cid_of_form(grp, id)
                                   : INVALID_MASKID;

  _update_row_selection(bd);
  _auto_expand_selected_row(module, id);
}

// defined with the rest of the list-row hover machinery further down
static void _row_hover_wash(dt_iop_gui_blend_data_t *bd, GtkWidget *target);

// canvas -> list hover sync: transiently highlight the row matching the shape
// under the cursor, or its group's header as a fallback, and show it in the
// selection panel in place of the selection, which stays what it is: solo edit
// keeps the selected group's other shapes editable on the canvas. An invalid
// id just clears the hover wash, and shows the selection again
void dt_iop_gui_blend_masks_hover_form(dt_iop_module_t *module, const dt_mask_id_t formid)
{
  if(!module) return;
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!bd || !bd->masks_list_box) return;
  const dt_mask_id_t hovered = dt_masks_model_panel_formid_for(module, formid);
  if(hovered != bd->canvas_hovered_formid)
  {
    bd->canvas_hovered_formid = hovered;
    _flexi_refine_follow_selection(bd);
    _props_panel_sync(module, FALSE);
  }
  GtkWidget *box = GTK_WIDGET(bd->masks_list_box);
  if(!dt_is_valid_maskid(formid))
  {
    _row_hover_wash(bd, NULL);
    return;
  }
  // prefer the shape's own (nested) row; fall back to the group header that contains
  // it
  GtkWidget *target = _masks_row_widget(bd, formid);
  if(!target)
  {
    // a path of an AI object not stepped into: the object's row
    const dt_mask_id_t row_fid = dt_masks_model_panel_formid_for(module, formid);
    if(row_fid != formid) target = _masks_row_widget(bd, row_fid);
  }
  if(!target) target = _find_collapsed_cluster_header(box, formid);
  _row_hover_wash(bd, target);
}

void dt_iop_gui_blend_masks_entered_object_changed(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module ? module->blend_data : NULL;
  if(!bd || !bd->masks_list_box) return;
  // the object's row turns into its group, or back (see _make_shape_row)
  _queue_masks_list_rebuild(module);
}

// redraw the center view for a change to what module shows or blends
// (request_mask_display, suppress_mask). Blending reads both as it runs
// (blend.c), so no history replay is needed, unlike dt_iop_refresh_center:
// it re-commits every module, which on every focus change is costly. Cache
// keys cover neither setting, so the module's output and what follows go
static void _refresh_mask_display(const dt_iop_module_t *module)
{
  DT_GUARD_GUI_UPDATE();
  dt_develop_t *dev = module->dev;
  if(!dev || !dev->gui_attached) return;
  dt_dev_pixelpipe_cache_invalidate_later(dev->full.pipe, module->iop_order, "mask display: ");
  dt_dev_invalidate(dev);
  dt_control_queue_redraw_center();
}

// the panel's way to step the canvas into AI object `id`, or out of the one it
// is in with INVALID_MASKID: the step a double-click on the object and a click
// outside it take on the canvas
static void _step_object(dt_iop_module_t *module, const dt_mask_id_t id)
{
  dt_masks_gui_step_object(module, darktable.develop ? darktable.develop->form_gui : NULL,
                           id, TRUE, dt_is_valid_maskid(id));
}

// the whole panel selection, groups included: unlike the element selection
// dt_iop_gui_blend_masks_select_form mirrors, which leaves the group alone because
// canvas rebuilds pass through it too, this is only ever the user's click
void dt_iop_gui_blend_masks_clear_selection(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module ? module->blend_data : NULL;
  if(!bd || !bd->masks_list_box) return;
  if(!dt_is_valid_maskid(bd->panel_selected_formid)
     && !dt_is_valid_maskid(bd->panel_selected_group_cid))
    return;
  bd->panel_selected_formid = INVALID_MASKID;
  bd->panel_selected_group_cid = INVALID_MASKID;
  _update_row_selection(bd);
  _update_add_target_hints(module);
}

// icon for the parametric mask's "show output" toggle: a chevron pointing down
// when collapsed (only the input slider shown), up when expanded (the output
// slider is shown too). CPF_ACTIVE is set automatically by the togglebutton draw
// code to match the checked state, so one paint function covers both.
static void _paint_param_inout(cairo_t *cr,
                               const gint x,
                               const gint y,
                               const gint w,
                               const gint h,
                               const gint flags,
                               void *data)
{
  const gint dirmask =
    CPF_DIRECTION_UP | CPF_DIRECTION_DOWN | CPF_DIRECTION_LEFT | CPF_DIRECTION_RIGHT;
  const gint dir = (flags & CPF_ACTIVE) ? CPF_DIRECTION_DOWN : CPF_DIRECTION_LEFT;
  dtgtk_cairo_paint_solid_arrow(cr, x, y, w, h, (flags & ~dirmask) | dir, data);
}

// the expand/collapse toggle on a parametric mask's shape row (see
// _make_shape_row): same in/out semantics as legacy multi-channel blendif --
// input and output are independent, additive (AND) refinements on the same
// channel, not alternatives. p->in_out here controls both whether the output
// (and opacity) sliders are shown next to the input one, and the row's
// compact/full layout as one combined state: collapsed is a compact,
// input-only slider; expanded shows input/output/opacity all in full (see
// _update_param_row_visibility). p->in_out never touches p->blendif, so an
// output range set earlier keeps refining the mask even while its slider is
// hidden.
static void _masks_param_inout_toggled(GtkWidget *btn, dt_iop_module_t *module)
{
  if(DT_IN_GUI_UPDATE()) return;
  const dt_mask_id_t id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "formid"));
  dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, id);
  if(!form || !(form->type & DT_MASKS_PARAMETRIC) || !form->points) return;
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  const uint32_t want = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(btn)) ? 1u : 0u;
  // selects the row too, if it wasn't (never deselects)
  _element_chevron_clicked(module, id, want != 0);
  dt_masks_point_parametric_t *p = form->points->data;

  if(p->in_out == want) return;
  p->in_out = want;
  dt_print(DT_DEBUG_MASKS, "[masks] parametric form %d: show_output=%u", id, want);
  dt_dev_add_masks_history_item(darktable.develop, module, TRUE);

  // show/hide its output slider and boost box in place
  dt_masks_param_row_editor_t *ed = _param_row_editor(bd, id);
  if(ed) _update_param_row_display(ed);
}

// input channel icon for parametric editor rows (arrow entering module)
static void _paint_param_input(cairo_t *cr,
                               const gint x,
                               const gint y,
                               const gint w,
                               const gint h,
                               const gint flags,
                               void *data)
{
  cairo_save(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, w, h);
  cairo_set_line_width(cr, 0.11);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

  // vertical target line / barrier on the right
  cairo_move_to(cr, 0.82, 0.18);
  cairo_line_to(cr, 0.82, 0.82);
  cairo_stroke(cr);

  // arrow shaft pointing into it
  cairo_move_to(cr, 0.15, 0.5);
  cairo_line_to(cr, 0.65, 0.5);
  cairo_stroke(cr);

  // arrowhead pointing right
  cairo_move_to(cr, 0.42, 0.27);
  cairo_line_to(cr, 0.65, 0.5);
  cairo_line_to(cr, 0.42, 0.73);
  cairo_stroke(cr);

  cairo_restore(cr);
}

// output channel icon for parametric editor rows (arrow leaving module)
static void _paint_param_output(cairo_t *cr,
                                const gint x,
                                const gint y,
                                const gint w,
                                const gint h,
                                const gint flags,
                                void *data)
{
  cairo_save(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, w, h);
  cairo_set_line_width(cr, 0.11);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

  // vertical source line / barrier on the left
  cairo_move_to(cr, 0.18, 0.18);
  cairo_line_to(cr, 0.18, 0.82);
  cairo_stroke(cr);

  // arrow shaft pointing out of it
  cairo_move_to(cr, 0.35, 0.5);
  cairo_line_to(cr, 0.85, 0.5);
  cairo_stroke(cr);

  // arrowhead pointing right
  cairo_move_to(cr, 0.62, 0.27);
  cairo_line_to(cr, 0.85, 0.5);
  cairo_line_to(cr, 0.62, 0.73);
  cairo_stroke(cr);

  cairo_restore(cr);
}

const dt_masks_flexi_op_t dt_masks_flexi_ops[DT_MASKS_FLEXI_OPS_COUNT] = {
  { 0, dtgtk_cairo_paint_masks_maximum,
    N_("maximum (union)"), N_("maximum"),
    N_("everything any element covers: where elements overlap, the stronger one wins"),
    "max(a, b)" },
  { DT_MASKS_STATE_FLEXI_SCREEN, dtgtk_cairo_paint_masks_screen,
    N_("screen (smooth union)"), N_("screen"),
    N_("like maximum (union), but overlapping feathered edges merge smoothly, "
       "without a seam"),
    "a + b - ab" },
  { DT_MASKS_STATE_FLEXI_SUM, dtgtk_cairo_paint_masks_sum,
    N_("sum (additive union)"), N_("sum"),
    N_("like maximum (union), but opacities add up where elements overlap, "
       "clipped at full opacity"),
    "min(1, a + b)" },
  { DT_MASKS_STATE_FLEXI_MINIMUM, dtgtk_cairo_paint_masks_minimum,
    N_("minimum (intersection)"), N_("minimum"),
    N_("only the area every element covers: the weaker one wins"),
    "min(a, b)" },
  { DT_MASKS_STATE_FLEXI_PRODUCT, dtgtk_cairo_paint_masks_product,
    N_("product (smooth intersection)"), N_("product"),
    N_("like minimum (intersection), but feathered edges fade together smoothly: "
       "strong only where every element is"),
    "a * b" },
  { DT_MASKS_STATE_FLEXI_DIFFERENCE, dtgtk_cairo_paint_masks_difference,
    N_("difference"), N_("difference"),
    N_("the bottom element, minus every element above it"),
    "a * (1 - b)" },
  { DT_MASKS_STATE_FLEXI_EXCLUSION, dtgtk_cairo_paint_masks_exclusion,
    N_("exclusion"), N_("exclusion"),
    N_("areas covered by an odd number of elements: where two overlap they cancel, "
       "a third brings the area back"),
    "max(a * (1 - b), b * (1 - a))" },
};

static int _flexi_op_index(const dt_masks_state_t flexi_op)
{
  for(int i = 1; i < DT_MASKS_FLEXI_OPS_COUNT; i++)
    if(flexi_op & dt_masks_flexi_ops[i].bit) return i;
  return 0;
}

static DTGTKCairoPaintIconFunc _flexi_op_paint(const dt_masks_state_t flexi_op)
{
  return dt_masks_flexi_ops[_flexi_op_index(flexi_op)].paint;
}

static const char *_flexi_op_name(const dt_masks_state_t flexi_op)
{
  return _(dt_masks_flexi_ops[_flexi_op_index(flexi_op)].name);
}

// the operation alone, for a group's default name: the full name does not
// fit a header
static const char *_flexi_op_short_name(const dt_masks_state_t flexi_op)
{
  return _(dt_masks_flexi_ops[_flexi_op_index(flexi_op)].short_name);
}

// is the group whose marker has `state` bypassed (disabled)?
static gboolean _op_is_bypassed(const int state)
{
  return (state & DT_MASKS_STATE_OP_BYPASS) != 0;
}

static GdkPixbuf *_op_pixbuf(DTGTKCairoPaintIconFunc paint)
{
  // GTK shows a pixbuf icon at its pixel size divided by the scale factor
  // (gtkiconhelper.c, ensure_surface_for_gicon), so render it in device
  // pixels
  const int w = (int)DT_PIXEL_APPLY_DPI(16), h = w;
  const double ppd = darktable.gui->ppd;
  const int pw = (int)(w * ppd), ph = (int)(h * ppd);
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
  cairo_t *cr = cairo_create(cst);
  cairo_scale(cr, ppd, ppd);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.5));
  dt_gui_gtk_set_source_rgba(cr, DT_GUI_COLOR_BUTTON_FG, 1.0);
  paint(cr, 0, 0, w, h, 0, NULL);
  cairo_destroy(cr);
  guchar *data = cairo_image_surface_get_data(cst);
  dt_draw_cairo_to_gdk_pixbuf(data, pw, ph);
  GdkPixbuf *shared =
    gdk_pixbuf_new_from_data(data, GDK_COLORSPACE_RGB, TRUE, 8, pw, ph,
                             cairo_image_surface_get_stride(cst), NULL, NULL);
  GdkPixbuf *owned = gdk_pixbuf_copy(shared); // own the pixels, then drop the surface
  g_object_unref(shared);
  cairo_surface_destroy(cst);
  return owned;
}

// "add group": clicking the button opens an operator chooser; picking an
// operator nests a new, empty group of that operator in the target group (see
// _stage_new_group). The icon is a fixed "+": it reflects neither the
// selection nor the operator last chosen
static void _stash_base_tooltip(GtkWidget *w);
static void _new_shape_op_update(GtkWidget *btn)
{
  dtgtk_button_set_paint(DTGTK_BUTTON(btn), dtgtk_cairo_paint_plus, 0, NULL);
  // _update_add_target_hints appends where the group goes
  gtk_widget_set_tooltip_text(btn, _("add a new, empty group\n"
                                     "click to pick its operator"));
  _stash_base_tooltip(btn);
  gtk_widget_queue_draw(btn);
}

// stage an empty group of the chosen operator (defined after the helpers it
// relies on).
static void _stage_new_group(dt_iop_module_t *module, const int flexi_op);

// build a labeled "icon + name" menu item with an action target for an operator chooser
static GMenuItem *_op_gmenu_item_target(DTGTKCairoPaintIconFunc paint,
                                        const char *name,
                                        const char *tooltip,
                                        const char *action,
                                        const int target)
{
  GMenuItem *it = g_menu_item_new(_(name), NULL);
  g_menu_item_set_action_and_target_value(it, action, g_variant_new_int32(target));
  if(tooltip)
    g_menu_item_set_attribute(it, "tooltip", "s", _(tooltip));
  GdkPixbuf *pb = _op_pixbuf(paint);
  if(pb)
  {
    g_menu_item_set_icon(it, G_ICON(pb));
    g_object_unref(pb);
  }
  return it;
}

// the menu item for group operator `i` of dt_masks_flexi_ops, its formula in
// the tooltip. Every menu that picks a group's operator builds its items here
static GMenuItem *_flexi_op_gmenu_item(const int i, const char *action, const int target)
{
  GMenuItem *it = _op_gmenu_item_target(dt_masks_flexi_ops[i].paint,
                                        dt_masks_flexi_ops[i].name,
                                        NULL, action, target);
  gchar *tip = g_strdup_printf("%s\n\n%s\n%s", _(dt_masks_flexi_ops[i].tooltip),
                               dt_masks_flexi_ops[i].formula,
                               _("a: the elements below, b: the next element up"));
  g_menu_item_set_attribute(it, "tooltip", "s", tip);
  g_free(tip);
  return it;
}

// a menu of every group operator but `skip` (-1 for none), each item's
// target its operator's bit
static GMenu *_flexi_op_menu_model(const char *action, const int skip)
{
  GMenu *menu = g_menu_new();
  for(int i = 0; i < DT_MASKS_FLEXI_OPS_COUNT; i++)
  {
    if((int)dt_masks_flexi_ops[i].bit == skip) continue;
    GMenuItem *it = _flexi_op_gmenu_item(i, action, dt_masks_flexi_ops[i].bit);
    g_menu_append_item(menu, it);
    g_object_unref(it);
  }
  return menu;
}

// the add-group operator chooser (_new_shape_op_pressed) is defined later, after the
// empty-group helpers, so it can disable operators that would create two adjacent
// same-operator groups given the current selection.
static void _new_shape_op_pressed(GtkGestureSingle *gesture,
                                  const int n_press,
                                  const double x,
                                  const double y,
                                  GtkWidget *btn);

typedef void (*_op_combo_pressed_t)(GtkGestureSingle *gesture,
                                    const int n_press,
                                    const double x,
                                    const double y,
                                    GtkWidget *btn);

// operator selector: just the current-operator icon inside a bordered box, so it
// reads as a chooser (the border) rather than a plain icon button. No chevron --
// the border alone is the affordance. The inner icon button is returned via *inner.
static GtkWidget *
_make_op_combo(GtkWidget **inner, DTGTKCairoPaintIconFunc icon, _op_combo_pressed_t pressed)
{
  GtkWidget *box = dt_gui_hbox();
  dt_gui_add_class(box, "dt_masks_op_combo");
  GtkWidget *btn = dtgtk_button_new(icon, 0, NULL);
  gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
  dt_gui_box_add(box, btn);
  _press_before_widget(dt_gui_connect_click(btn, pressed, NULL, btn));
  // the wrapper box carries no_show_all (its visibility is driven by mode_flexi),
  // which also stops show_all from reaching the child: show it explicitly so the
  // box is not empty once it is made visible.
  gtk_widget_show(btn);
  if(inner) *inner = btn;
  return box;
}

// after a solo change: a solo edit whose element the solo hid has nothing on
// the canvas to edit, so drop it. TRUE means the caller must give the canvas
// the whole group again. No refresh here: both callers (_toggle_solo_form,
// _toggle_solo_group) run _refresh_all_shape_rows next
static gboolean _model_clear_soloedit_if_hidden(dt_iop_module_t *module,
                                                dt_masks_form_t *grp)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!dt_is_valid_maskid(bd->soloedit_formid)) return FALSE;
  const dt_masks_point_group_t *sp = dt_masks_gui_group_point(grp, bd->soloedit_formid);
  if(sp && (sp->state & DT_MASKS_STATE_HIDDEN))
  {
    bd->soloedit_formid = INVALID_MASKID;
    return TRUE;
  }
  return FALSE;
}


// the state half of the element solo toggle, which returns what the caller
// must do to the canvas edit scope. Solo and solo edit exclude each other
// here, not by convention at the call sites
dt_masks_solo_canvas_t dt_masks_model_toggle_solo_form(dt_iop_module_t *module,
                                                       dt_masks_form_t *grp,
                                                       const dt_mask_id_t id)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!grp || !dt_masks_gui_group_point(grp, id)) return DT_MASKS_SOLO_CANVAS_NONE;
  dt_masks_solo_canvas_t canvas = DT_MASKS_SOLO_CANVAS_NONE;

  if(bd->solo_formid == id)
  {
    dt_masks_group_isolate_state(grp, NULL, DT_MASKS_STATE_HIDDEN);
    bd->solo_formid = INVALID_MASKID;
    bd->solo_group_key = 0;
    dt_print(DT_DEBUG_MASKS, "[masks] solo off");
  }
  else
  {
    GList *one = g_list_prepend(NULL, GINT_TO_POINTER(id));
    dt_masks_group_isolate_state(grp, one, DT_MASKS_STATE_HIDDEN);
    g_list_free(one);
    bd->solo_formid = id;
    // only one thing is ever soloed: an element solo cancels any group solo
    bd->solo_group_key = 0;
    dt_print(DT_DEBUG_MASKS, "[masks] solo form %d", id);
    // solo and solo-edit are mutually exclusive: soloing unconditionally
    // drops any active solo-edit, not just one whose element the new solo happens
    // to hide (see _model_clear_soloedit_if_hidden for that narrower case).
    if(dt_is_valid_maskid(bd->soloedit_formid))
    {
      bd->soloedit_formid = INVALID_MASKID;
      canvas = DT_MASKS_SOLO_CANVAS_FULL;
    }
  }
  if(_model_clear_soloedit_if_hidden(module, grp))
    canvas = DT_MASKS_SOLO_CANVAS_FULL;
  return canvas;
}

// solo a single element: show only this shape, hiding all the others; toggling
// off clears every hidden bit (solo is the only thing that sets
// DT_MASKS_STATE_HIDDEN, so there is nothing else to preserve). Triggered from
// the row's own actions menu (see _build_shape_actions_menu) or by
// shift+clicking its visibility button (see _visibility_form_pressed)
static void _toggle_solo_form(dt_iop_module_t *module, const dt_mask_id_t id)
{
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  if(!grp || !dt_masks_gui_group_point(grp, id)) return;

  if(dt_masks_model_toggle_solo_form(module, grp, id) == DT_MASKS_SOLO_CANVAS_FULL)
    dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
  dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
  _sync_hidden_to_form_visible(module);
  // solo can flip every row's hidden state at once; refresh them all in place
  // instead of rebuilding the whole list (see _update_shape_row_state).
  _refresh_all_shape_rows(module);
  _sync_solo_canvas_highlight(module);
  // solo-edit stood down while this was soloed: taking the solo off isolates
  // the selection again (see dt_masks_model_soloedit_target)
  _soloedit_follow_selection(module->blend_data);
}

// click: disable, or enable again. Shift+click: solo, or clear it. The two
// entries of the actions menu's visibility section, through the same functions
// (see _shape_act_disable, _shape_act_solo), and solo only where the menu
// offers it: not on a disabled element. Every press toggles, the second of a
// double-click included. A right-click goes on to the row's actions menu
static void _visibility_form_pressed(GtkGestureSingle *gesture,
                                     const int n_press,
                                     const double x,
                                     const double y,
                                     dt_iop_module_t *module)
{
  if(gtk_gesture_single_get_current_button(gesture) != GDK_BUTTON_PRIMARY) return;
  dt_gui_claim(gesture);
  GtkWidget *w = dt_gui_get_widget(gesture);
  const dt_mask_id_t id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "formid"));
  if(!dt_modifier_is(dt_gui_current_state(gesture), GDK_SHIFT_MASK))
    _toggle_element_disable(module, id);
  else if(_visibility_status_get(w) != MASK_VISIBILITY_DISABLED)
    _toggle_solo_form(module, id);
}

// flexi: clear any active solo (used wherever the whole selection/visibility
// state is reset, e.g. deleting the last remaining shape).
static void _masks_clear_solo_state(dt_iop_gui_blend_data_t *bd)
{
  bd->solo_formid = INVALID_MASKID;
  bd->solo_group_key = 0;
}

// after removing shapes from the mask the canvas still draws the outlines of the
// now-gone shapes: dt_masks_clear_form_gui clears the gui points but form_visible
// still points at the (stale) edit group, so the overlay is not refreshed until
// the next unrelated action (e.g. adding a shape). Rebuild the on-canvas edit
// overlay from what remains so the ghost outlines clear immediately.
void dt_masks_gui_refresh_canvas_edit(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(bd && bd->masks_shown != DT_MASKS_EDIT_OFF)
    dt_masks_set_edit_mode(module, bd->masks_shown); // rebuilds form_visible + redraws
  else
    dt_masks_change_form_gui(NULL); // clear the overlay
  dt_control_queue_redraw_center();
}

// commit an edit to the mask's structure: its history item, a list rebuild
// and the canvas. The rebuild is deferred: the edits come from a widget's own
// event handler or menu item, still mid-dispatch on a widget a synchronous
// rebuild would destroy (see _rebuild_masks_list_idle)
static void _commit_structure_change(dt_iop_module_t *module)
{
  dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
  _queue_masks_list_rebuild(module);
  dt_masks_gui_refresh_canvas_edit(module);
}

static void _toggle_element_disable(dt_iop_module_t *module, const dt_mask_id_t id)
{
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  if(!grp) return;
  dt_masks_point_group_t *pt = dt_masks_gui_group_point(grp, id);
  if(!pt) return;
  pt->state ^= DT_MASKS_STATE_DISABLE;

  dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
  _sync_hidden_to_form_visible(module);
  // one bit on one point: the row is refreshed in place, as _invert_element
  // does. _update_shape_row_state paints everything the bit affects, so a
  // rebuild would add only a visible flash
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  _update_shape_row_state(bd, _masks_row_widget(bd, id), pt);
  // a disabled element gets no badge, and its groups' badges stop counting it
  _refresh_lowop_badges(module);
  dt_masks_gui_refresh_canvas_edit(module);
}

// the core of "reset mask": remove every element and group but the mask's
// own, left empty. No confirmation and no rebuild: the callers (the reset
// button, a group-layout preset) add those
void dt_masks_gui_reset_mask_core(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  if(grp && grp->points)
  {
    dt_masks_clear_form_gui(darktable.develop);
    // the points go, the group form stays, starting over with a marker of its
    // own: removing its elements through dt_masks_form_remove would delete it
    // with its last one, and the module's mask with it (see
    // _detach_group_members). Detached before the marker is made: picking its
    // id walks every group's points in forms, this one's included
    GList *old = grp->points;
    grp->points = NULL;
    g_list_free_full(old, free);
    grp->points = g_list_append(NULL, dt_masks_marker_new(darktable.develop->forms, grp, 0));
    // enable FALSE: this is a wipe, reached from the module's reset button and
    // from a preset apply, not a mask edit. Every other mask commit passes TRUE
    // and so switches the mask on (see dt_dev_add_masks_history_item), which
    // here would turn masking on for a module whose mask was just thrown away
    dt_dev_add_masks_history_item(darktable.develop, module, FALSE);
  }
  bd->panel_selected_group_cid = INVALID_MASKID;
  bd->panel_selected_formid = INVALID_MASKID;
  _masks_clear_solo_state(bd);

  // element and group refinements went with their points; the whole-mask one
  // is in blend_params, and would stay applied with nothing in the panel
  // showing it
  if(_refine_global_is_set(module))
  {
    const gboolean had_details = _refine_clear_global(module);
    dt_dev_add_history_item(darktable.develop, module, TRUE);
    if(had_details) // see _refine_clear_global
    {
      dt_dev_reprocess_all(module->dev);
      dt_control_queue_redraw();
    }
  }

  // the per-formid scratch (which refinements are bypassed, which rows are
  // expanded) is keyed by ids that no longer exist after the wipe
  dt_pthread_mutex_lock(&bd->lock);
  if(bd->masks_refine_bypassed) g_hash_table_remove_all(bd->masks_refine_bypassed);
  dt_pthread_mutex_unlock(&bd->lock);
  if(bd->masks_props_expanded) g_hash_table_remove_all(bd->masks_props_expanded);
  bd->masks_refine_scope_kind = REFINE_SCOPE_GLOBAL;
  bd->masks_refine_scope_formid = INVALID_MASKID;
  _queue_link_peers_rebuild(module);
}

static void _masks_row_drag_get(GtkWidget *w,
                                GdkDragContext *ctx,
                                GtkSelectionData *sel,
                                guint info,
                                guint time,
                                gpointer user_data)
{
  const dt_mask_id_t id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "formid"));
  gtk_selection_data_set(sel, gtk_selection_data_get_target(sel), 8, (const guchar *)&id,
                         sizeof(id));
}

// the selection a moved element leaves: the element, inside its new group. A
// parametric channel selects no group, as a click on it does not
static void _select_moved_element(dt_iop_module_t *module,
                                  dt_masks_form_t *grp,
                                  const dt_mask_id_t src)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  bd->panel_selected_formid = src;
  dt_masks_form_t *sform = dt_masks_get_from_id(darktable.develop, src);
  bd->panel_selected_group_cid = (sform && !(sform->type & DT_MASKS_PARAMETRIC))
                                   ? dt_masks_gui_group_cid_of_form(grp, src)
                                   : INVALID_MASKID;
}

// the model half of the element-onto-element drop, apart from the GTK handler
// so that the model tests (src/tests/unittests/masks/test_flexi_model.c) run
// the same code. Changes grp->points and the panel's selection, not history,
// the pipe or the widgets: the caller commits. `above` means the shape shows
// above the target, later in the bottom-up list. TRUE if anything moved
gboolean dt_masks_model_drop_element_onto_element(dt_iop_module_t *module,
                                                  dt_masks_form_t *grp,
                                                  const dt_mask_id_t src,
                                                  const dt_mask_id_t dst,
                                                  const gboolean above)
{
  if(!grp || src == dst) return FALSE;
  return dt_masks_model_drop_point_onto_point(module, grp, dt_masks_gui_group_point(grp, src),
                                              dt_masks_gui_group_point(grp, dst), above);
}

gboolean dt_masks_model_drop_point_onto_point(dt_iop_module_t *module,
                                              dt_masks_form_t *grp,
                                              const dt_masks_point_group_t *sp,
                                              const dt_masks_point_group_t *dp,
                                              const gboolean above)
{
  if(!grp || !sp || !dp || sp->formid == dp->formid) return FALSE;
  const dt_mask_id_t src = sp->formid;
  dt_masks_form_t *sowner = NULL, *downer = NULL;
  GList *s = _point_node_at(grp, sp, &sowner, 0);
  GList *d = _point_node_at(grp, dp, &downer, 0);
  // elements both: a group is dropped onto through its header. Across nesting
  // levels, only where it may go (see _may_move_into)
  if(!s || !d || dt_masks_point_is_marker(s->data) || dt_masks_point_is_marker(d->data)
     || !_may_move_into(grp, sowner, downer, src))
    return FALSE;

  // sitting among its members is all it takes to join dst's group: the
  // group's settings are its marker's. d->prev is at worst that marker
  dt_masks_point_group_t *spt = s->data;
  sowner->points = g_list_delete_link(sowner->points, s);
  _insert_point_after(downer, above ? d : d->prev, spt);

  // a moved element should stay selected at the end of the drag -- otherwise
  // it lands in its new spot with no visible indication of what just moved
  _select_moved_element(module, grp, src);
  return TRUE;
}

// the reference the element row of widget `w` (the row itself, or a widget
// inside it) shows, while it is still in the mask and still shape `id`; else
// the first reference to `id`. A mask can hold a shape twice, and then its
// form id alone names the wrong row
static const dt_masks_point_group_t *_row_reference(dt_masks_form_t *grp,
                                                    GtkWidget *w,
                                                    const dt_mask_id_t id)
{
  GtkWidget *row = w ? g_object_get_data(G_OBJECT(w), "row-vbox") : NULL;
  if(!row) row = w;
  const dt_masks_point_group_t *pt = row ? g_object_get_data(G_OBJECT(row), "row-point") : NULL;
  if(pt && _point_node_at(grp, pt, NULL, 0) && pt->formid == id) return pt;
  return dt_masks_gui_group_point(grp, id);
}

// the element row a drop on target `w` lands next to: the row itself, or a
// cluster's top row when landing above it and its bottom row below
static GtkWidget *_element_drop_row(GtkWidget *w, const gboolean above)
{
  GtkWidget *row = g_object_get_data(G_OBJECT(w), above ? "drop-row-top" : "drop-row-bottom");
  return row ? row : w;
}

// say why a group dropped somewhere stays put, when the reason is one the
// user cannot see: it would go inside itself, or nest too deep. Every other
// refused drop is a no-op the drop line already showed
static void _explain_refused_drop(dt_masks_form_t *grp,
                                  const dt_masks_point_group_t *sp,
                                  const dt_mask_id_t dst)
{
  dt_masks_form_t *sowner = NULL, *downer = NULL;
  const dt_masks_form_t *f = sp ? dt_masks_get_from_id(darktable.develop, sp->formid) : NULL;
  if(!f || !(f->type & DT_MASKS_GROUP) || !_point_node_at(grp, sp, &sowner, 0)
     || !_point_node_owner(grp, dst, &downer))
    return;
  if(!_may_move_into(grp, sowner, downer, sp->formid))
    dt_control_log(_("a group cannot go inside itself, and groups nest at most %d deep"),
                   DT_MASKS_NESTING_MAX);
}

// a group dragged by its header: its marker and its members move as a unit
static const GtkTargetEntry _mask_group_dnd[] = { { (gchar *)DND_TARGET_GROUP,
                                                    GTK_TARGET_SAME_APP, 0 } };

// a same-kind element cluster dragged by its header: every one of its members
// moves together, as one contiguous block preserving their relative order (see
// dt_masks_gui_cluster_move)
static const GtkTargetEntry _mask_cluster_dnd[] = { { (gchar *)DND_TARGET_CLUSTER,
                                                      GTK_TARGET_SAME_APP, 0 } };

// the list takes all three payloads (see _drop_target_at); the receive handler
// routes on the entry info below
enum
{
  DND_MASK_GROUP = 0,
  DND_MASK_ROW = 1,
  DND_MASK_CLUSTER = 2
};
static const GtkTargetEntry _mask_hdr_dnd[] = {
  { (gchar *)DND_TARGET_GROUP, GTK_TARGET_SAME_APP, DND_MASK_GROUP },
  { (gchar *)DND_TARGET_ROW, GTK_TARGET_SAME_APP, DND_MASK_ROW },
  { (gchar *)DND_TARGET_CLUSTER, GTK_TARGET_SAME_APP, DND_MASK_CLUSTER }
};

// where a drop lands, placed against the innermost of these under the pointer
// (see _drop_target_at). The line the motion handler draws and the move the
// receive handler makes both come from here, so the two cannot disagree:
//
// - an element: an element row or a cluster (tagged "drop-item"), header line
//   and body. Its top half lands above it, its bottom half below. Rows display
//   bottom-up, so above is later in the list
// - a list: a group's elements box, or an open cluster's (tagged
//   "drop-list-owner"). Its elements are targets of their own, so what is left
//   to hit are the gaps between them, which land above the element under the
//   gap. Above its first element or below its last, the list's owner decides
// - a group: its block, title included (see _drop_on_group)
typedef struct dt_masks_drop_t
{
  GtkWidget *frame; // the element or group it lands beside, or the group it goes in
  gboolean inside;  // inside the group `frame`, on top of it
  gboolean above;   // beside `frame`: above it, else below
} dt_masks_drop_t;

// the list child standing for drop frame `f`: the frame itself, or for a
// nested group shown as its group, the box its block is packed in
static GtkWidget *_drop_item_of(GtkWidget *f)
{
  GtkWidget *item = g_object_get_data(G_OBJECT(f), "drop-list-item");
  return item ? item : f;
}

// the drop frame list child `c` stands for, or NULL for a child that is no
// element (a group's opacity slider, the row of a shape being drawn)
static GtkWidget *_drop_frame_of_item(GtkWidget *c)
{
  GtkWidget *f = g_object_get_data(G_OBJECT(c), "drop-frame");
  if(f) return f;
  return g_object_get_data(G_OBJECT(c), "drop-item") ? c : NULL;
}

// the elements of `list` in `in`'s coordinates: its first (top) and last
// (bottom) one with their edges, and the topmost one reaching below `y`, the
// one under a gap at `y`. FALSE when it shows none
typedef struct dt_masks_drop_list_t
{
  GtkWidget *first, *last, *under;
  int first_top, last_bottom;
} dt_masks_drop_list_t;

static gboolean _drop_list_items(GtkWidget *list,
                                 GtkWidget *in,
                                 const int y,
                                 dt_masks_drop_list_t *l)
{
  *l = (dt_masks_drop_list_t){ 0 };
  int under_top = 0;
  GList *kids = list ? gtk_container_get_children(GTK_CONTAINER(list)) : NULL;
  for(GList *k = kids; k; k = g_list_next(k))
  {
    GtkWidget *c = k->data;
    GtkWidget *f = _drop_frame_of_item(c);
    gint cx = 0, cy = 0;
    // mapped, not visible: a collapsed group's elements stay visible in a
    // hidden box, at stale positions
    if(!f || !gtk_widget_get_mapped(c) || !gtk_widget_translate_coordinates(c, in, 0, 0, &cx, &cy))
      continue;
    const int bottom = cy + gtk_widget_get_allocated_height(c);
    if(!l->first || cy < l->first_top) l->first = f, l->first_top = cy;
    if(!l->last || bottom > l->last_bottom) l->last = f, l->last_bottom = bottom;
    if(bottom > y && (!l->under || cy < under_top)) l->under = f, under_top = cy;
  }
  g_list_free(kids);
  return l->first != NULL;
}

// how high the bands along the top and bottom edge of group block `f` are,
// which land beside it, with its title's offset in *ty; 0 for a group with
// nothing beside it. A nested group shown as its group sits among its holder's
// elements
static int _group_drop_edge(GtkWidget *f, gint *ty)
{
  GtkWidget *title = g_object_get_data(G_OBJECT(f), "drop-title");
  gint tx = 0;
  if(!g_object_get_data(G_OBJECT(f), "drop-list-item") || !title
     || !gtk_widget_translate_coordinates(title, f, 0, 0, &tx, ty))
    return 0;
  return MIN(gtk_widget_get_allocated_height(title) / 2,
             gtk_widget_get_allocated_height(f) / 3);
}

// a group's block. The top edge of its title lands above the group, its bottom
// edge below it: a band as high as half the title, or a third of the block
// when the block is no more than its title. Below its last element it lands
// there, at the bottom of the group. Everything else lands inside it, on top,
// right where its top element shows.
//
// A group with nothing beside it (the mask's own, the group of a nested group
// shown as an element row) takes the edges inside too, as does any drop with
// shift held.
static dt_masks_drop_t _drop_on_group(GtkWidget *f, const int y)
{
  const dt_masks_drop_t inside = { f, TRUE, FALSE };
  if(dt_modifier_is(dt_key_modifier_state(), GDK_SHIFT_MASK)) return inside;

  gint ty = 0;
  const int edge = _group_drop_edge(f, &ty);
  if(edge)
  {
    if(y < ty + edge) return (dt_masks_drop_t){ f, FALSE, TRUE };
    if(y >= gtk_widget_get_allocated_height(f) - edge) return (dt_masks_drop_t){ f, FALSE, FALSE };
  }
  dt_masks_drop_list_t l;
  if(_drop_list_items(g_object_get_data(G_OBJECT(f), "drop-list"), f, y, &l)
     && y >= l.last_bottom)
    return (dt_masks_drop_t){ l.last, FALSE, FALSE };
  return inside;
}

static dt_masks_drop_t _drop_at(GtkWidget *w, const int y)
{
  if(g_object_get_data(G_OBJECT(w), "drop-item"))
  {
    const int h = gtk_widget_get_allocated_height(w);
    return (dt_masks_drop_t){ w, FALSE, h > 0 && y < h / 2 };
  }
  GtkWidget *owner = g_object_get_data(G_OBJECT(w), "drop-list-owner");
  if(!owner) return _drop_on_group(w, y);

  dt_masks_drop_list_t l;
  if(_drop_list_items(w, w, y, &l) && y >= l.first_top && y < l.last_bottom && l.under)
    return (dt_masks_drop_t){ l.under, FALSE, TRUE };
  // translate_coordinates needs a common ancestor and realized widgets; when
  // it cannot answer, the pointer is somewhere in the owner, and only that
  gint ox = 0, oy = 0;
  if(!gtk_widget_translate_coordinates(w, owner, 0, y, &ox, &oy)) oy = 0;
  return _drop_at(owner, oy);
}

// the slot between two elements is both "below the upper one" and "above the
// lower one", drawn as the lower one's top edge so that it shows as one line,
// not two a few pixels apart. Only the slot below a list's last element is on
// its bottom edge. Found by on-screen geometry: the list packs from the end,
// so its children's order does not say where they show
static void _drop_line(const dt_masks_drop_t d, GtkWidget **w, gboolean *above)
{
  *w = d.frame;
  *above = d.above;
  if(d.above) return;
  GtkWidget *item = _drop_item_of(d.frame);
  GtkWidget *list = gtk_widget_get_parent(item);
  if(!GTK_IS_CONTAINER(list)) return;
  gint iy = 0, ix = 0;
  if(!gtk_widget_translate_coordinates(item, list, 0, 0, &ix, &iy)) return;
  GtkWidget *next = NULL;
  int next_y = 0;
  GList *kids = gtk_container_get_children(GTK_CONTAINER(list));
  for(GList *k = kids; k; k = g_list_next(k))
  {
    GtkWidget *c = k->data;
    GtkWidget *f = _drop_frame_of_item(c);
    gint cx = 0, cy = 0;
    if(c == item || !f || !gtk_widget_get_mapped(c)
       || !gtk_widget_translate_coordinates(c, list, 0, 0, &cx, &cy) || cy <= iy)
      continue;
    if(!next || cy < next_y) next = f, next_y = cy;
  }
  g_list_free(kids);
  if(next)
  {
    *w = next;
    *above = TRUE;
  }
}

static void _masks_group_drag_get(GtkWidget *w,
                                  GdkDragContext *ctx,
                                  GtkSelectionData *sel,
                                  guint info,
                                  guint time,
                                  gpointer user_data)
{
  // the group's id, its marker's: an empty group has no member to name it by
  const dt_mask_id_t id = _header_cid(w);
  dt_print(DT_DEBUG_MASKS, "[masks dnd] group drag-data-get id=%d", id);
  gtk_selection_data_set(sel, gtk_selection_data_get_target(sel), 8, (const guchar *)&id,
                         sizeof(id));
}

// a cluster's DnD payload is every member's formid, packed as a plain array --
// order does not matter on the receive side (dt_masks_gui_cluster_move re-derives the
// members' relative order from grp->points itself), so the "hover-formids" list
// already stashed on the header (see _pack_group_elements) is reused as-is.
static void _masks_cluster_drag_get(GtkWidget *w,
                                    GdkDragContext *ctx,
                                    GtkSelectionData *sel,
                                    guint info,
                                    guint time,
                                    gpointer user_data)
{
  GList *ids = g_object_get_data(G_OBJECT(w), "hover-formids");
  const int n = g_list_length(ids);
  dt_mask_id_t *buf = g_malloc_n(MAX(n, 1), sizeof(dt_mask_id_t));
  int i = 0;
  for(GList *l = ids; l; l = g_list_next(l)) buf[i++] = GPOINTER_TO_INT(l->data);
  dt_print(DT_DEBUG_MASKS, "[masks dnd] cluster drag-data-get n=%d", n);
  gtk_selection_data_set(sel, gtk_selection_data_get_target(sel), 8, (const guchar *)buf,
                         n * (int)sizeof(dt_mask_id_t));
  g_free(buf);
}

// unpack a cluster's DnD payload (see _masks_cluster_drag_get) back into a
// GList of formids. Caller frees.
static GList *_cluster_ids_from_selection(GtkSelectionData *sel)
{
  const gint len = gtk_selection_data_get_length(sel);
  if(len <= 0 || len % (gint)sizeof(dt_mask_id_t) != 0) return NULL;
  const dt_mask_id_t *buf = (const dt_mask_id_t *)gtk_selection_data_get_data(sel);
  const int n = len / (int)sizeof(dt_mask_id_t);
  GList *ids = NULL;
  for(int i = 0; i < n; i++) ids = g_list_prepend(ids, GINT_TO_POINTER(buf[i]));
  return ids;
}

// select the group a drag moved, as an element drop selects its element
// (dt_masks_model_drop_point_onto_point): it shows what moved, and the next
// "add group" goes by the selection
static void _select_moved_group(dt_iop_module_t *module, const dt_mask_id_t cid)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!bd) return;
  bd->panel_selected_formid = INVALID_MASKID;
  bd->panel_selected_group_cid = cid;
}

// every drop ends here. A move reorders the fold the pipe evaluates, so it is
// committed like any other edit, or the canvas would keep the old render
static void _finish_drop(dt_iop_module_t *module,
                         GdkDragContext *ctx,
                         const gboolean ok,
                         const guint time)
{
  if(ok) dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
  gtk_drag_finish(ctx, ok, FALSE, time);
  if(ok) _queue_masks_list_rebuild(module);
}

// the dragged group `src` inside group `dst`, on top of it
static gboolean _drop_group_inside(dt_iop_module_t *module,
                                   const dt_mask_id_t src,
                                   const dt_mask_id_t dst)
{
  const gboolean ok = dt_masks_model_move_group(module, src, dst, FALSE, TRUE);
  // a moved group stays selected (see _select_moved_group)
  if(ok)
    _select_moved_group(module, src);
  else if(src != dst)
  {
    dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
    const dt_masks_form_t *sub = dt_masks_model_nested_group_of(grp, src);
    if(sub) _explain_refused_drop(grp, dt_masks_gui_group_point(grp, sub->formid), dst);
  }
  return ok;
}

// the model half of the element-onto-group-header drop, split as
// dt_masks_model_drop_element_onto_element is: the element joins the group,
// on top
gboolean dt_masks_model_drop_element_onto_group(dt_iop_module_t *module,
                                                dt_masks_form_t *grp,
                                                const dt_mask_id_t src,
                                                const dt_mask_id_t dst)
{
  if(!grp || src == dst) return FALSE;
  return dt_masks_model_drop_point_onto_group(module, grp, dt_masks_gui_group_point(grp, src), dst);
}

gboolean dt_masks_model_drop_point_onto_group(dt_iop_module_t *module,
                                              dt_masks_form_t *grp,
                                              const dt_masks_point_group_t *sp,
                                              const dt_mask_id_t dst)
{
  if(!grp || !sp || sp->formid == dst) return FALSE;
  const dt_mask_id_t src = sp->formid;
  dt_masks_form_t *sowner = NULL;
  GList *s = _point_node_at(grp, sp, &sowner, 0);
  dt_masks_form_t *downer = _group_of(grp, dst);
  // as for a drop onto an element
  if(!s || dt_masks_point_is_marker(s->data) || !downer || !_may_move_into(grp, sowner, downer, src))
    return FALSE;
  // already in that group: nothing to do
  if(sowner == downer) return FALSE;

  dt_masks_point_group_t *spt = s->data;
  sowner->points = g_list_delete_link(sowner->points, s);
  _insert_point_after(downer, g_list_last(downer->points), spt);

  // a moved element should stay selected at the end of the drag
  _select_moved_element(module, grp, src);
  return TRUE;
}

// drop what `sel` carries beside the reference `dp`, above or below it: an
// element, a cluster, or a group dragged by its header, which moves as the
// nested group it is in its holder's list. Every drop that lands beside
// something, an element or a group, goes through here
static gboolean _drop_beside(dt_iop_module_t *module,
                             GdkDragContext *ctx,
                             GtkSelectionData *sel,
                             const guint info,
                             const dt_masks_point_group_t *dp,
                             const gboolean above)
{
  if(!dp) return FALSE;
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  if(info == DND_MASK_CLUSTER)
  {
    GList *ids = _cluster_ids_from_selection(sel);
    const gboolean ok = ids && dt_masks_gui_cluster_move(module, ids, dp->formid, FALSE, above);
    g_list_free(ids);
    return ok;
  }
  if(gtk_selection_data_get_length(sel) != (gint)sizeof(dt_mask_id_t)) return FALSE;
  const dt_mask_id_t src = *(const dt_mask_id_t *)gtk_selection_data_get_data(sel);
  // a group's header carries the group's id
  const dt_masks_form_t *sub = info == DND_MASK_GROUP ? dt_masks_model_nested_group_of(grp, src) : NULL;
  const dt_masks_point_group_t *sp =
    info != DND_MASK_GROUP ? _row_reference(grp, gtk_drag_get_source_widget(ctx), src)
    : sub                  ? dt_masks_gui_group_point(grp, sub->formid)
                           : NULL;
  const gboolean ok = dt_masks_model_drop_point_onto_point(module, grp, sp, dp, above);
  // a moved group stays selected as a group, as on any other drop
  if(ok && sub) _select_moved_group(module, src);
  if(!ok) _explain_refused_drop(grp, sp, dp->formid);
  return ok;
}

// drop what `sel` carries inside group `cid`, on top of it
static gboolean _drop_inside(dt_iop_module_t *module,
                             GdkDragContext *ctx,
                             GtkSelectionData *sel,
                             const guint info,
                             const dt_mask_id_t cid)
{
  if(info == DND_MASK_CLUSTER)
  {
    GList *ids = _cluster_ids_from_selection(sel);
    const gboolean ok = ids && dt_masks_gui_cluster_move(module, ids, cid, TRUE, FALSE);
    g_list_free(ids);
    return ok;
  }
  if(gtk_selection_data_get_length(sel) != (gint)sizeof(dt_mask_id_t)) return FALSE;
  const dt_mask_id_t src = *(const dt_mask_id_t *)gtk_selection_data_get_data(sel);
  if(info == DND_MASK_GROUP) return _drop_group_inside(module, src, cid);
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  const dt_masks_point_group_t *sp = _row_reference(grp, gtk_drag_get_source_widget(ctx), src);
  const gboolean ok = dt_masks_model_drop_point_onto_group(module, grp, sp, cid);
  if(!ok) _explain_refused_drop(grp, sp, cid);
  return ok;
}

// make the move drop `d` stands for
static gboolean _drop_apply(dt_iop_module_t *module,
                            GdkDragContext *ctx,
                            GtkSelectionData *sel,
                            const guint info,
                            const dt_masks_drop_t d)
{
  if(!d.frame) return FALSE;
  if(d.inside) return _drop_inside(module, ctx, sel, info, _header_cid(d.frame));
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  if(g_object_get_data(G_OBJECT(d.frame), "group-key"))
  {
    // beside a group is beside the nested group it shows, in its holder's list
    const dt_masks_form_t *nested = dt_masks_model_nested_group_of(grp, _header_cid(d.frame));
    return nested
           && _drop_beside(module, ctx, sel, info, dt_masks_gui_group_point(grp, nested->formid), d.above);
  }
  GtkWidget *row = _element_drop_row(d.frame, d.above);
  const dt_mask_id_t dst = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "formid"));
  return _drop_beside(module, ctx, sel, info, _row_reference(grp, row, dst), d.above);
}

// the expand/collapse chevron of the group header for `gcid`, or NULL: group
// headers are tagged "mask-header", carry their cid under "group-key", and
// hold their own toggle under "group-expand-toggle" (see the header build).
static GtkWidget *_find_group_expand_toggle(GtkWidget *w, const dt_mask_id_t gcid)
{
  GtkWidget *header = _find_tagged(w, "mask-header", _header_has_cid, GINT_TO_POINTER(gcid));
  return header ? g_object_get_data(G_OBJECT(header), "group-expand-toggle") : NULL;
}

// an element row's own expander, whatever kind of row it is: a drawn shape's
// (or an expandable raster row's) props chevron, or a parametric row's in/out
// chevron. Every row that has one tags its row_vbox with it at build time (see
// _make_shape_row's "expand-toggle"), so this needs no per-kind knowledge and
// returns NULL for a row that has nothing to expand.
static GtkWidget *_row_expand_toggle(dt_iop_gui_blend_data_t *bd, const dt_mask_id_t id)
{
  GtkWidget *row = dt_is_valid_maskid(id) ? _masks_row_widget(bd, id) : NULL;
  return row ? g_object_get_data(G_OBJECT(row), "expand-toggle") : NULL;
}

// open every nested group row, and the group holding each such row, between
// the top of the list and point `id`, so a selection inside a nested group is
// never left folded away. Opening them is not the user choosing which group
// "auto-expand selected" keeps open, hence _group_expand_enforcing
static void _reveal_nesting(dt_iop_module_t *module, const dt_mask_id_t id)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  if(!bd->masks_list_box || !dt_is_valid_maskid(id)) return;
  dt_mask_id_t cur = id;
  for(int depth = 0; depth <= DT_MASKS_NESTING_MAX; depth++)
  {
    dt_masks_form_t *owner = NULL;
    if(!_point_node_owner(grp, cur, &owner) || owner == grp) break;
    _set_chevron(_row_expand_toggle(bd, owner->formid), TRUE, TRUE);
    const dt_mask_id_t gcid = dt_masks_gui_group_cid_of_form(grp, owner->formid);
    if(dt_is_valid_maskid(gcid))
      _set_chevron(_find_group_expand_toggle(GTK_WIDGET(bd->masks_list_box), gcid), TRUE, TRUE);
    cur = owner->formid;
  }
}

// fold or unfold the same-kind cluster whose revealer is `rev`, its arrow and
// remembered state with it (see bd->masks_cluster_expanded)
static void _cluster_set_revealed(dt_iop_gui_blend_data_t *bd,
                                  GtkWidget *rev,
                                  const gboolean revealed)
{
  gtk_revealer_set_reveal_child(GTK_REVEALER(rev), revealed);
  GtkWidget *arrow = g_object_get_data(G_OBJECT(rev), "arrow");
  if(arrow)
  {
    dtgtk_button_set_paint(DTGTK_BUTTON(arrow), dtgtk_cairo_paint_dropdown,
                           revealed ? 0 : CPF_DIRECTION_UP, NULL);
    gtk_widget_queue_draw(arrow);
  }
  if(bd && bd->masks_cluster_expanded)
    g_hash_table_insert(bd->masks_cluster_expanded,
                        g_object_get_data(G_OBJECT(rev), "cluster-key"),
                        GINT_TO_POINTER(revealed));
}

// reveal every container of a row: its cluster, its group and any nested
// groups holding it. Same-kind drawn shapes fold into a collapsible cluster
// once there are enough of them (see _pack_group_elements), and a row inside
// a *collapsed* one is still in the widget tree (a GtkRevealer keeps its child
// while hidden), so anything done to it would happen out of sight
static void
_reveal_containers_for_row(dt_iop_module_t *module, GtkWidget *row, const dt_mask_id_t id)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  for(GtkWidget *w = row; w && w != GTK_WIDGET(bd->masks_list_box); w = gtk_widget_get_parent(w))
    if(GTK_IS_REVEALER(w) && !gtk_revealer_get_reveal_child(GTK_REVEALER(w)))
      _cluster_set_revealed(bd, w, TRUE);

  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  const dt_mask_id_t gcid = dt_masks_gui_group_cid_of_form(grp, id);
  if(dt_is_valid_maskid(gcid))
  {
    _remember_expanded(bd, gcid, TRUE);
    if(bd->masks_list_box)
      _set_chevron(_find_group_expand_toggle(GTK_WIDGET(bd->masks_list_box), gcid), TRUE, FALSE);
  }
  _reveal_nesting(module, id);
}

// expand or collapse one element row without a click's side effects. A
// shape or raster row's toggle is GUI state, flipped as is. A parametric row's
// chevron is in_out, stored in the form, and its handler commits a history
// item: selecting must not add an undo step, so the field is set and the row
// refreshed with the handler guarded out. The pipe does not read in_out, and
// it is saved with the next edit
static void _set_row_expanded(dt_iop_module_t *module,
                              const dt_mask_id_t id,
                              const gboolean expanded)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  GtkWidget *toggle = _row_expand_toggle(bd, id);
  if(!toggle || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggle)) == expanded) return;

  // a props chevron carries the editor box it drives, a nested group row's
  // the box of its group; a parametric row's in/out chevron carries neither
  if(g_object_get_data(G_OBJECT(toggle), "props-editor-box")
     || g_object_get_data(G_OBJECT(toggle), "elem-box"))
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), expanded);
    return;
  }

  dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, id);
  if(!form || !(form->type & DT_MASKS_PARAMETRIC) || !form->points) return;
  dt_masks_point_parametric_t *p = form->points->data;
  p->in_out = expanded ? 1u : 0u;
  DT_ENTER_GUI_UPDATE(); // keep _masks_param_inout_toggled out of this
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), expanded);
  DT_LEAVE_GUI_UPDATE();
  dt_masks_param_row_editor_t *ed = _param_row_editor(bd, id);
  if(ed) _update_param_row_display(ed);
}

// "auto-expand selected" (masks panel hamburger -> options): while enabled,
// exactly one element row, the last selected one that has an expander, is
// expanded. _make_props_row_toggle applies the same rule at build time (see
// dt_masks_model_auto_expand_anchor); a selection never rebuilds the list, so
// this enforces it in place.
//
// `id` is only a candidate: one with nothing to expand (a group, an element
// while "element properties in subpanel" is on, no selection) leaves the open
// element open rather than shifting the panel for nothing
static void _auto_expand_selected_row(dt_iop_module_t *module, const dt_mask_id_t id)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!_auto_expand_selected()) return;

  GtkWidget *row = dt_is_valid_maskid(id) ? _masks_row_widget(bd, id) : NULL;
  GtkWidget *toggle = _row_expand_toggle(bd, id);
  // `id` has nothing to expand -- leave the last-expanded element alone. A
  // nested group's chevron shows its group, not properties: collapsing it for
  // the next selection could fold that selection away (see _reveal_nesting)
  if(!toggle || g_object_get_data(G_OBJECT(toggle), "elem-box")) return;

  if(bd->masks_last_expanded_elem == id
     && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggle)))
    return; // already the one that's expanded

  // masks_suppress_toggle_select: _props_row_toggled selects the row it
  // toggles, which for the row collapsed here would land back in this
  // function and recurse. Not DT_ENTER_GUI_UPDATE: _props_row_toggled would
  // then skip the state and visibility update these calls are for
  bd->masks_suppress_toggle_select = TRUE;

  // collapse only the row this option opened last, not every other row
  if(dt_is_valid_maskid(bd->masks_last_expanded_elem)
     && bd->masks_last_expanded_elem != id)
    _set_row_expanded(module, bd->masks_last_expanded_elem, FALSE);

  _reveal_containers_for_row(module, row, id);
  _set_row_expanded(module, id, TRUE);
  bd->masks_last_expanded_elem = id;

  bd->masks_suppress_toggle_select = FALSE;
}

// collapse whichever group "auto-expand selected" last opened, unless it is
// `keep_cid`. Split out so _group_expand_toggled can reuse it when a real
// click on a chevron takes over as the one open group.
static void _collapse_auto_expanded_group(dt_iop_module_t *module,
                                          const dt_mask_id_t keep_cid)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  const dt_mask_id_t prev = bd->masks_last_expanded_group;
  if(!dt_is_valid_maskid(prev) || prev == keep_cid || !bd->masks_list_box) return;
  // a group holding `keep_cid` in a nested group stays open to show it
  GList *prev_members = dt_masks_model_group_members(dt_masks_gui_module_mask_group(module), prev);
  const gboolean holds_keep = _members_hold(prev_members, keep_cid);
  g_list_free(prev_members);
  if(holds_keep) return;
  _set_chevron(_find_group_expand_toggle(GTK_WIDGET(bd->masks_list_box), prev), FALSE, TRUE);
}

// the group half of "auto-expand selected": selecting a group opens it and
// closes the one opened before, exactly as _auto_expand_selected_row does for
// elements one level down. Selecting an *element* comes through here too --
// _set_form_target sets the group half of the selection first (see
// _set_group_target) -- so picking an element opens both its group and itself.
//
// A group's chevron reveals its members, not a properties panel, so this
// tracks its own "at most one open" state (bd->masks_last_expanded_group)
// rather than sharing the element one. Clearing the selection is a no-op, same
// candidate-only rule the element half follows: whatever is open stays open
// instead of the panel collapsing to nothing.
static void _auto_expand_selected_group(dt_iop_module_t *module,
                                        const dt_mask_id_t cid)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!_auto_expand_selected()) return;

  // a chevron click the user just made collapses the group *and* selects it,
  // which lands here -- honor the click instead of undoing it. One-shot,
  // cleared by whichever selection arrives first so it can never go stale.
  const dt_mask_id_t collapsed_by_click = bd->masks_group_collapse_click;
  bd->masks_group_collapse_click = INVALID_MASKID;

  if(!dt_is_valid_maskid(cid) || collapsed_by_click == cid) return;
  if(!bd->masks_list_box) return;

  GtkWidget *toggle = _find_group_expand_toggle(GTK_WIDGET(bd->masks_list_box), cid);
  if(!toggle) return;
  if(bd->masks_last_expanded_group == cid
     && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggle)))
    return; // already the one that's open

  _collapse_auto_expanded_group(module, cid);
  _set_chevron(toggle, TRUE, TRUE);
  _reveal_nesting(module, cid);
  bd->masks_last_expanded_group = cid;
}

// the panel's selection state machine, apart from the widget and canvas
// effects so that it can be tested without a display
// (src/tests/unittests/masks/test_flexi_model.c). These decide what a click
// selects; _set_form_target and _set_group_target apply it.
//
// Selection has two levels -- a group, and an element within it -- and the
// contract is that every reachable state is one click away:
//
//   click a group       -> that group selected
//   click it again      -> the mask's own group selected (it cannot be
//                          deselected: one group is always selected)
//   click the group of the selected element
//                       -> that group selected, the element dropped
//   click an element    -> that element selected, inside its group
//   click it again      -> the element is dropped, its GROUP stays selected
//   click elsewhere     -> that thing selected
//
// deselecting an element lands in its group: clearing both levels would make
// selecting that group again take two clicks
dt_masks_panel_sel_t dt_masks_model_click_element(const dt_iop_gui_blend_data_t *bd,
                                                  dt_masks_form_t *grp,
                                                  const dt_mask_id_t id)
{
  dt_masks_panel_sel_t s = { INVALID_MASKID, INVALID_MASKID };
  // an element's group is selected alongside it either way -- what differs is
  // whether the element itself survives the click
  s.group_cid = dt_masks_gui_group_cid_of_form(grp, id);
  if(bd->panel_selected_formid != id) s.formid = id;
  return s;
}

dt_masks_panel_sel_t dt_masks_model_click_group(const dt_iop_gui_blend_data_t *bd,
                                                const dt_mask_id_t cid)
{
  dt_masks_panel_sel_t s = { INVALID_MASKID, INVALID_MASKID };
  // only a group selected by itself deselects: one selected because it holds
  // the selected element is selected in the element's place
  const gboolean deselect = dt_is_valid_maskid(bd->panel_selected_group_cid)
                            && bd->panel_selected_group_cid == cid
                            && !dt_is_valid_maskid(bd->panel_selected_formid);
  // deselecting lands on the mask's own group, which therefore stays selected
  // when clicked again
  s.group_cid = deselect ? _mask_group_cid(bd->module) : cid;
  return s;
}

// point the panel's element selection at `id`, and its group selection at the
// group holding it. Never deselects: that is _select_form's, for a click on
// the title. auto_expand=FALSE skips _auto_expand_selected_row's
// collapse-the-previous-row/expand-this-one side effect -- used by a
// right-click (see _row_click_press): the reflow it causes would leave the
// actions menu anchored to where the header *was*. Every other caller goes
// through the plain _set_form_target wrapper below (auto_expand=TRUE)
static void _set_form_target_ext(dt_iop_module_t *module,
                                 const dt_mask_id_t id,
                                 const gboolean auto_expand)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  _set_group_target_ext(module, dt_masks_gui_group_cid_of_form(grp, id), id);
  bd->panel_selected_formid = id;
  if(dt_is_valid_maskid(id))
  {
    GtkWidget *row = _masks_row_widget(bd, id);
    _reveal_containers_for_row(module, row, id);
  }
  _update_row_selection(bd);
  if(auto_expand) _auto_expand_selected_row(module, id);
}

static void _set_form_target(dt_iop_module_t *module, const dt_mask_id_t id)
{
  _set_form_target_ext(module, id, TRUE);
}

// a real click on an element row's chevron, props or parametric in/out alike
// (shift+click on the row drives the same chevron). It also selects the row,
// but that selection must not run auto-expand: the option would re-open a row
// the click just collapsed, before the handler got to act on it.
static void _element_chevron_clicked(dt_iop_module_t *module,
                                     const dt_mask_id_t id,
                                     const gboolean expanded)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  const dt_masks_chevron_click_t c =
    dt_masks_model_element_chevron_click(bd, id, expanded, _auto_expand_selected());
  if(dt_is_valid_maskid(c.collapse))
  {
    // programmatic: must not read as a click on that row's own chevron (see
    // _props_row_toggled for the recursion this flag prevents)
    const gboolean was = bd->masks_suppress_toggle_select;
    bd->masks_suppress_toggle_select = TRUE;
    _set_row_expanded(module, c.collapse, FALSE);
    bd->masks_suppress_toggle_select = was;
  }
  bd->masks_last_expanded_elem = c.last_expanded;
  if(bd->panel_selected_formid != id) _set_form_target_ext(module, id, FALSE);
}

// select an element by clicking its title. Clicking the title of an already-
// selected element deselects it (toggle), mirroring the group header's own
// title-click behavior (see _select_group) -- only the title click routes
// through here, see _set_form_target above for the select-only variant.
// Deselecting an element drops back to its GROUP being selected (see
// dt_masks_model_click_element for the whole contract)
static void _select_form(dt_iop_module_t *module, const dt_mask_id_t id)
{
  const dt_masks_panel_sel_t s =
    dt_masks_model_click_element(module->blend_data, dt_masks_gui_module_mask_group(module), id);
  if(dt_is_valid_maskid(s.formid)) _set_form_target(module, s.formid);
  else _set_group_target(module, s.group_cid);
}

// stable type-label prefix for a form ("circle", "Lightness", ...), recomputed
// from form->type (and, for parametric, its channel) every time rather than
// parsed out of form->name -- so it survives repeated renames, see
// _row_click_press / _rename_commit.
static const char *_form_type_prefix(const dt_masks_form_t *form)
{
  if(form->type & DT_MASKS_PARAMETRIC) return dt_masks_parametric_type_label(form);
  return _kind_name(_form_kind(form), FALSE);
}

// form->name with its stable type prefix stripped -- the row's own icon (and,
// for parametric, the channel badge) already say what kind this is, so
// repeating it in the text would be redundant. Used both for the row label and
// to prefill the rename entry with only the editable part. Caller frees.
gchar *dt_masks_gui_form_display_name(const dt_masks_form_t *form)
{
  const char *prefix = _form_type_prefix(form);
  if(!prefix) prefix = "";
  const size_t plen = strlen(prefix);
  const char *rest = form->name;
  if(g_str_has_prefix(form->name, prefix)
     && (form->name[plen] == ' ' || form->name[plen] == '\0'))
  {
    rest = form->name + plen;
    while(*rest == ' ') rest++;
  }
  // a raster element named by its type alone shows its source's current name,
  // so renaming the source renames it; a name of its own stops that
  if((form->type & DT_MASKS_RASTER) && !*rest)
  {
    const dt_iop_module_t *src = dt_masks_raster_source(form);
    if(src) return _module_plain_name(src);
    const dt_masks_point_raster_t *p = form->points ? form->points->data : NULL;
    return g_strdup(p ? p->source : "");
  }
  return g_strdup(rest);
}

gboolean dt_masks_model_rename_form(dt_masks_form_t *form, const char *txt)
{
  if(!form || !txt) return FALSE;
  char name[sizeof(form->name)];
  // the entry edits only the part after the type prefix, so a rename replaces
  // the auto-assigned "#<id>" without ever dropping the "what is this"
  // indication. Emptying a raster element's name makes it follow its source
  if(*txt)
    g_snprintf(name, sizeof(name), "%s %s", _form_type_prefix(form), txt);
  else if(form->type & DT_MASKS_RASTER)
    g_strlcpy(name, _form_type_prefix(form), sizeof(name));
  else
    return FALSE;
  if(!strcmp(name, form->name)) return FALSE;
  dt_strlcpy_to_fixed(form->name, name, sizeof(form->name));
  return TRUE;
}

// a shared element shows the chain and offers "unlink". A raster element never
// does: it has nothing shared to edit, and unlinking it would change nothing.
// Nor does a parametric channel, which is only ever copied: the same range
// selects something else in another module's pixels
gboolean dt_masks_model_form_is_linked(const dt_masks_form_t *form)
{
  if(!form || (form->type & (DT_MASKS_RASTER | DT_MASKS_PARAMETRIC))) return FALSE;
  GList *users = dt_masks_model_form_users(form->formid);
  const gboolean linked = !g_list_shorter_than(users, 2);
  g_list_free(users);
  return linked;
}

static dt_iop_module_t *_linked_next_user(const dt_iop_module_t *module,
                                          const dt_mask_id_t fid);

// the tooltip of a linked element's chain icon, naming the other modules that
// use it. `uses_here` is how often this module's own mask references the form
// (see _model_form_uses_in_mask): a shape can be linked without any other
// module being involved. NULL when it is neither shared nor repeated
static gchar *_linked_tooltip(const dt_iop_module_t *module,
                              const dt_mask_id_t fid,
                              const dt_masks_form_t *form,
                              const int uses_here)
{
  GList *users = dt_masks_model_form_users(fid);
  GString *names = g_string_new(NULL);
  for(GList *l = users; l; l = g_list_next(l))
  {
    if(l->data == module) continue;
    gchar *name = _module_plain_name(l->data);
    if(names->len) g_string_append(names, ", ");
    g_string_append(names, name);
    g_free(name);
  }
  g_list_free(users);
  if(!names->len)
  {
    g_string_free(names, TRUE);
    // no other module uses it, but this mask can reference it more than once
    if(uses_here > 1)
      return g_strdup_printf(
        _("used %d times in this mask\n"
          "this shape is shared: editing it changes every row it\n"
          "appears in, while each row keeps its own opacity,\n"
          "inversion and refinements\n"
          "click to select it"),
        uses_here);
    return NULL;
  }
  const char *format =
    (form->type & DT_MASKS_OBJECT)
      ? _("linked with %s\n"
          "this AI object is shared: editing it changes it in every\n"
          "module it is linked with, while its opacity, inversion and\n"
          "refinements stay separate\n"
          "right-click the row and pick \"unlink\" to give this module\n"
          "its own copy")
      : _("linked with %s\n"
          "this shape is shared: editing it changes it in every\n"
          "module it is linked with, while its opacity, inversion and\n"
          "refinements stay separate\n"
          "right-click the row and pick \"unlink\" to give this module\n"
          "its own copy");
  gchar *tip = g_strdup_printf(format, names->str);
  g_string_free(names, TRUE);
  dt_iop_module_t *next = _linked_next_user(module, fid);
  gchar *next_name = next ? _module_plain_name(next) : NULL;
  gchar *full = next_name
    ? g_strdup_printf(_("%s\nclick to go to it in %s"), tip, next_name) : NULL;
  g_free(next_name);
  if(full)
  {
    g_free(tip);
    tip = full;
  }
  return tip;
}

// the chain that leads to a mask living elsewhere, the same wherever it shows:
// a linked element's other module, a raster element's source, a consumer of
// this module's raster mask (see .dt_masks_link)
static GtkWidget *_make_link_button(const char *tooltip, GCallback on_click, gpointer data)
{
  GtkWidget *link = dtgtk_button_new(dtgtk_cairo_paint_link, 0, NULL);
  gtk_widget_set_halign(link, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(link, GTK_ALIGN_CENTER);
  dt_gui_add_class(link, "dt_masks_icon");
  dt_gui_add_class(link, "dt_masks_link");
  gtk_widget_set_tooltip_text(link, tooltip);
  g_signal_connect(G_OBJECT(link), "clicked", G_CALLBACK(on_click), data);
  return link;
}

// a slot like a parametric row's picker box, so the drawer columns line up
static GtkWidget *_link_action_slot(GtkWidget *link)
{
  GtkWidget *slot = dt_gui_hbox(link);
  gtk_widget_set_valign(slot, GTK_ALIGN_CENTER);
  return slot;
}

// expanded and focused, the way every link in the panel goes to a module
static void _go_to_module(dt_iop_module_t *m)
{
  if(!m->expanded)
    dt_iop_gui_set_expanded(m, TRUE, dt_conf_get_bool("darkroom/ui/single_module"));
  dt_iop_request_focus(m);
}

// the module a linked element's chain leads to: the next one using the form
// after `module` in pipe order, wrapping, so clicking the chain in each panel
// in turn visits every module it is linked with. NULL when no other module
// uses it (a form only repeated within this mask)
static dt_iop_module_t *_linked_next_user(const dt_iop_module_t *module,
                                          const dt_mask_id_t fid)
{
  GList *users = dt_masks_model_form_users(fid);
  GList *self = g_list_find(users, module);
  dt_iop_module_t *next = NULL;
  for(GList *l = self ? g_list_next(self) : users; l && !next; l = g_list_next(l))
    if(l->data != module) next = l->data;
  for(GList *l = users; l && l != self && !next; l = g_list_next(l))
    if(l->data != module) next = l->data;
  g_list_free(users);
  return next;
}

// go to the linked element in its next module, with its row selected there;
// a form only repeated within this mask is selected here
static void _linked_link_clicked(GtkButton *button, dt_iop_module_t *module)
{
  const dt_mask_id_t fid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "formid"));
  dt_iop_module_t *next = _linked_next_user(module, fid);
  if(!next)
  {
    _set_form_target_ext(module, fid, FALSE);
    return;
  }
  _go_to_module(next);
  if(dt_dev_gui_module() != next || !next->blend_data) return;
  dt_iop_gui_blend_masks_panel_show();
  _set_form_target_ext(next, fid, FALSE);
}

static GtkWidget *_make_linked_link(dt_iop_module_t *module,
                                    const dt_mask_id_t fid,
                                    const char *tooltip)
{
  GtkWidget *link = _make_link_button(tooltip, G_CALLBACK(_linked_link_clicked), module);
  g_object_set_data(G_OBJECT(link), "formid", GINT_TO_POINTER(fid));
  return link;
}

static void _rename_commit(GtkWidget *entry, dt_iop_module_t *module)
{
  if(g_object_get_data(G_OBJECT(entry), "done")) return; // guard double commit
  g_object_set_data(G_OBJECT(entry), "done", GINT_TO_POINTER(1));
  const dt_mask_id_t id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(entry), "formid"));
  dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, id);
  gchar *txt = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));
  if(txt) g_strstrip(txt);
  if(dt_masks_model_rename_form(form, txt))
  {
    dt_print(DT_DEBUG_MASKS, "[masks] form %d renamed to '%s'", id, form->name);
    dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
  }
  g_free(txt);
  // deferred: this runs in the "activate" or "focus-out-event" of `entry`,
  // inside the row a rebuild destroys, and queued events would still reach
  // the destroyed widgets (see _queue_masks_list_rebuild)
  _queue_masks_list_rebuild(module);
}

static gboolean _rename_focus_out(GtkWidget *entry, GdkEvent *e, dt_iop_module_t *module)
{
  _rename_commit(entry, module);
  return FALSE;
}


// start inline rename on `evbox` (swap its label for an entry): shared by a
// ctrl+click on the row's name (_row_click_press) and the "rename" entry
// in the row's actions menu (_build_shape_actions_menu).
static void
_start_rename_element(GtkWidget *evbox, dt_iop_module_t *module, const dt_mask_id_t id)
{
  // same gesture as renaming a module (ctrl+click), for consistency -- see
  // _iop_plugin_header_released in imageop.c
  GtkWidget *child = gtk_bin_get_child(GTK_BIN(evbox));
  if(child && GTK_IS_ENTRY(child))
  {
    // already renaming (a quick second ctrl+click): focus the entry. Do not
    // destroy it: its focus-out-event commits and rebuilds the list in the
    // middle of the destroy, which crashes
    gtk_widget_grab_focus(child);
    return;
  }
  if(child) gtk_widget_destroy(child);
  GtkWidget *entry = gtk_entry_new();
  // frameless and unpadded (.dt_masks_rename_entry), so the entry is no taller
  // than the label it replaces and the row keeps its height
  gtk_entry_set_has_frame(GTK_ENTRY(entry), FALSE);
  dt_gui_add_class(entry, "dt_masks_rename_entry");
  // and wider: a stock entry asks for about 20 characters, where the label it
  // replaces asks for one (see gtk_label_set_max_width_chars in
  // _make_shape_row). It still fills the width the row gives it
  gtk_entry_set_width_chars(GTK_ENTRY(entry), 1);
  gtk_entry_set_max_width_chars(GTK_ENTRY(entry), 1);
  dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, id);
  if(form)
  {
    // prefill with just the part after the type prefix, so the prefix
    // itself is never in the editable text and can't be typed over
    gchar *rest = dt_masks_gui_form_display_name(form);
    gtk_entry_set_text(GTK_ENTRY(entry), rest);
    g_free(rest);
  }
  g_object_set_data(G_OBJECT(entry), "formid", GINT_TO_POINTER(id));
  gtk_container_add(GTK_CONTAINER(evbox), entry);
  g_signal_connect(G_OBJECT(entry), "activate", G_CALLBACK(_rename_commit), module);
  g_signal_connect(G_OBJECT(entry), "focus-out-event", G_CALLBACK(_rename_focus_out),
                   module);
  gtk_widget_show(entry);
  gtk_widget_grab_focus(entry);
}

// a deleted formid can be left behind in several bd fields that reference a
// specific shape by id (element selection, solo, solo-edit): left there,
// _flexi_refine_follow_selection keeps a "valid" but nonexistent
// panel_selected_formid, and the refinement caption goes on naming the deleted
// shape. Called by every delete path, once per deleted formid.
static void _clear_stale_formid_refs(dt_iop_gui_blend_data_t *bd, const dt_mask_id_t id)
{
  if(!bd || !dt_is_valid_maskid(id)) return;
  if(bd->panel_selected_formid == id) bd->panel_selected_formid = INVALID_MASKID;
  if(bd->solo_formid == id) bd->solo_formid = INVALID_MASKID;
  if(bd->soloedit_formid == id) bd->soloedit_formid = INVALID_MASKID;
  // "auto-expand selected" (_auto_expand_selected_row): a stale id would make
  // _make_props_row_toggle expand the wrong row on the next rebuild
  if(bd->masks_last_expanded_elem == id) bd->masks_last_expanded_elem = NO_MASKID;
}

// the list holding member point `pt`, found by the point itself rather than by
// its form id: a form referenced twice has two points, and only `pt` is the
// one a row stands for
static dt_masks_form_t *_point_owner_form(dt_masks_form_t *grp,
                                          const dt_masks_point_group_t *pt)
{
  if(g_list_find(grp->points, pt)) return grp;
  for(GList *l = darktable.develop->forms; l; l = g_list_next(l))
  {
    dt_masks_form_t *f = l->data;
    if(g_list_find(f->points, pt)) return f;
  }
  return NULL;
}

// delete a single shape from the module's mask group: shared by the "delete"
// entry in the row's actions menu (_build_shape_actions_menu), a nested
// group's delete, and dt_iop_gui_blend_delete_element (the canvas's delete).
// `pt` is the member point to remove, as the row that asked holds it; NULL
// takes the first reference to `id`
static void _delete_single_shape(dt_iop_module_t *module,
                                 const dt_mask_id_t id,
                                 dt_masks_point_group_t *pt)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, id);
  if(!grp || !form) return;
  // deferred, so it sees the group after the removal
  _queue_link_peers_rebuild(module);
  _clear_stale_formid_refs(bd, id);
  dt_masks_clear_form_gui(darktable.develop);
  // not dt_masks_form_remove(): it commits a history item between removing
  // the point and testing whether the group is empty, and then acts on `grp`
  // across that reentry, which can destroy the module's mask group with a
  // member still in it. _detach_group_members() changes only grp->points. An
  // emptied group stays: its marker does
  dt_masks_form_t *owner = pt ? _point_owner_form(grp, pt) : NULL;
  if(owner)
  {
    owner->points = g_list_remove(owner->points, pt);
    free(pt);
  }
  else
  {
    GList *one = g_list_prepend(NULL, GINT_TO_POINTER(id));
    _detach_group_members(grp, one);
    g_list_free(one);
  }
  dt_print(DT_DEBUG_MASKS, "[masks] form %d deleted from panel", id);
  _commit_structure_change(module);
}

void dt_iop_gui_blend_delete_element(dt_iop_module_t *module, const dt_mask_id_t id)
{
  if(module && module->blend_data) _delete_single_shape(module, id, NULL);
}

// forward declared here (defined much further down, near _build_shape_actions_menu's
// other caller) so _row_click_press's own right-click can open the same menu
// without reordering half the file.
static void _build_shape_actions_menu(GtkWidget *anchor,
                                      dt_iop_module_t *module,
                                      const dt_mask_id_t id,
                                      GtkWidget *handle,
                                      GtkWidget *evbox);

// once the actions menu closes, auto-expand the row it was opened on, if it
// is still selected and the option is on: not when it opens
// (_row_click_press), where the reflow would move the menu. On "hide", which
// every way of closing emits, unlike "deactivate", which comes before the
// item's "activate"
static void _shape_popover_closed(GtkPopover *popover, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  const dt_mask_id_t id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(popover), "formid"));
  dt_iop_gui_blend_data_t *bd = module ? module->blend_data : NULL;
  if(bd && bd->panel_selected_formid == id) _auto_expand_selected_row(module, id);
}

// an AI object of several paths, which can be stepped into to edit them one
// by one. One of a single path already acts as that path
static gboolean _is_multi_path_object(const dt_mask_id_t id)
{
  const dt_masks_form_t *obj = dt_masks_get_from_id(darktable.develop, id);
  return obj && (obj->type & DT_MASKS_OBJECT) && _object_path_count(obj) >= 2;
}

// step into AI object `id` to edit its paths one by one, or back out of it
// with `inside`: shared by a double-click on its row and its actions menu
static void _toggle_object_paths(dt_iop_module_t *module,
                                 const dt_mask_id_t id,
                                 const gboolean inside)
{
  if(!_is_multi_path_object(id)) return;
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  // the paths are picked on the canvas, so going in turns editing on there
  if(!inside && bd && bd->masks_edit
     && !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(bd->masks_edit)))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), TRUE);
  _step_object(module, inside ? INVALID_MASKID : id);
}

// the press and release handlers of a row's three plain click surfaces: the
// handle, the name and the row's background between its controls, so that a
// click does the same on any of them. Each carries the "formid",
// "handle-widget" and "name-evbox" tags (_make_shape_row):
//  * ctrl+click:         rename
//  * shift+click:        toggle this element's properties/expanded view
//  * right-click:        open the actions menu
//  * plain click/release: select (toggles off if already selected)
//  * double-click:       step into an AI object's paths, or out of them
// No double-click to solo: the first click's release already toggled the
// selection, so the element would read as deselected by the second press
//
// Every press is claimed by the surface it lands on, so a surface nested in
// another (the handle and the name sit on the row's background) is the only
// one to see the press and its release. A claim does not stop the surface's
// own drag source, which takes the same press from the widget's signals
static void _row_click_press(GtkGestureSingle *gesture,
                             const int n_press,
                             const double x,
                             const double y,
                             dt_iop_module_t *module)
{
  dt_gui_claim(gesture);
  GtkWidget *w = dt_gui_get_widget(gesture);
  const dt_mask_id_t id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "formid"));
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  const guint button = gtk_gesture_single_get_current_button(gesture);
  // a fresh press always starts a new interaction -- clear any stale flag a
  // previous press's drag-begin set but whose release never arrived to
  // consume (e.g. a drag canceled by Escape), so it cannot wrongly swallow
  // this press's own eventual release (see _row_drag_begin / masks_row_click_handled).
  bd->masks_row_click_handled = FALSE;
  bd->masks_skip_group_select_release = FALSE;
  if(button == GDK_BUTTON_PRIMARY && dt_modifier_is(dt_gui_current_state(gesture), GDK_CONTROL_MASK))
  {
    if(bd->panel_selected_formid != id) _set_form_target(module, id);
    GtkWidget *evbox = g_object_get_data(G_OBJECT(w), "name-evbox");
    _start_rename_element(evbox, module, id);
    return;
  }
  // double-click on an AI object: in to its paths, or back out, as on the
  // canvas. The first click's release has already toggled the selection, and
  // may have stepped out with it, so the step goes by where the canvas was
  // before that click; the object is selected again, and the second release
  // must not toggle it back off
  if(n_press == 2 && button == GDK_BUTTON_PRIMARY && _is_multi_path_object(id))
  {
    const gboolean inside = bd->masks_row_click_entered == id;
    if(bd->panel_selected_formid != id) _set_form_target_ext(module, id, FALSE);
    bd->masks_skip_group_select_release = TRUE;
    _toggle_object_paths(module, id, inside);
    return;
  }
  if(button == GDK_BUTTON_SECONDARY)
  {
    // select the shape for the menu's actions, but do not auto-expand it:
    // the reflow would move the menu (see _set_form_target_ext)
    if(bd->panel_selected_formid != id) _set_form_target_ext(module, id, FALSE);
    GtkWidget *handle = g_object_get_data(G_OBJECT(w), "handle-widget");
    GtkWidget *evbox = g_object_get_data(G_OBJECT(w), "name-evbox");
    _build_shape_actions_menu(w, module, id, handle, evbox);
    g_object_set_data(G_OBJECT(darktable.gui->active_popover_menu), "formid", GINT_TO_POINTER(id));
    g_signal_connect(G_OBJECT(darktable.gui->active_popover_menu), "closed", G_CALLBACK(_shape_popover_closed), module);
    GdkRectangle rect = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(darktable.gui->active_popover_menu), &rect);
    gtk_popover_popup(GTK_POPOVER(darktable.gui->active_popover_menu));
    return;
  }
  // selection happens on release (a release is not delivered when a drag
  // started, so dragging never also selects, see _row_click_release). A
  // primary click focuses the module, as one on the module's body does: the
  // claim keeps the press from reaching the body
  if(button == GDK_BUTTON_PRIMARY) dt_iop_request_focus(module);
}

// shift+click on any of a row's non-specific click surfaces toggles its
// expander, by driving the chevron the row's handle was tagged with at build
// time (see _make_shape_row / the group header block's "expand-toggle" data).
// No-op if untagged
static void _toggle_expand_widget(GtkWidget *src)
{
  GtkWidget *btn = g_object_get_data(G_OBJECT(src), "expand-toggle");
  if(!btn) return;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn),
                               !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(btn)));
}

// dragging a row, a group or a cluster of the list leaves the list as it is:
// what is open stays open and what is folded stays folded, and nothing is
// selected, so nothing the drag aims at moves when it starts. A folded group or
// cluster the drag rests on for MASKS_SPRING_DELAY_MS opens (see _drop_motion),
// a delay so that a drag sweeping past folded groups does not open them all,
// and it shows its members' headers alone: its note, opacity slider and the
// editors (tagged "drag-hide" where they are built) stay hidden. What a drag
// opened stays open while it lasts, so passing over it again moves nothing,
// and folds again once it ends, but for where the element landed (see
// _masks_drag_restore)
#define MASKS_SPRING_DELAY_MS 600

static struct
{
  gboolean active;       // a drag of the list's own is in progress
  gboolean dropped;      // and it landed, see _drop_received
  dt_mask_id_t formid;   // the element row it drags, or INVALID_MASKID
  gdouble start_x, start_y; // the pointer, in root coordinates, when it began
  GtkWidget *candidate;  // the folded group or cluster it rests on (weak)
  guint timer;           // opens the candidate once the rest is long enough
  GPtrArray *opened;     // the groups and clusters it opened
} _masks_drag = { FALSE, FALSE, INVALID_MASKID, 0.0, 0.0, NULL, 0, NULL };

static void _pointer_root_position(gdouble *x, gdouble *y)
{
  GdkSeat *seat = gdk_display_get_default_seat(gdk_display_get_default());
  GdkDevice *pointer = seat ? gdk_seat_get_pointer(seat) : NULL;
  *x = *y = 0.0;
  if(pointer) gdk_device_get_position_double(pointer, NULL, x, y);
}

static void _masks_drag_set_candidate(GtkWidget *w)
{
  if(_masks_drag.candidate == w) return;
  if(_masks_drag.timer) g_source_remove(_masks_drag.timer);
  _masks_drag.timer = 0;
  if(_masks_drag.candidate)
    g_object_remove_weak_pointer(G_OBJECT(_masks_drag.candidate),
                                 (gpointer *)&_masks_drag.candidate);
  _masks_drag.candidate = w;
  if(w) g_object_add_weak_pointer(G_OBJECT(w), (gpointer *)&_masks_drag.candidate);
}

static void _masks_drag_restore(dt_iop_module_t *module, GtkWidget *landed, GtkWidget *frame);

static void _masks_drag_begin(dt_iop_module_t *module, const dt_mask_id_t formid)
{
  // a drag GTK never reported the end of leaves its state behind (see
  // _masks_drag_failed): settle it first
  _masks_drag_restore(module, NULL, NULL);
  _masks_drag_set_candidate(NULL);
  if(_masks_drag.opened)
    g_ptr_array_set_size(_masks_drag.opened, 0);
  else
    _masks_drag.opened = g_ptr_array_new();
  _masks_drag.active = TRUE;
  _masks_drag.dropped = FALSE;
  _masks_drag.formid = formid;
  _pointer_root_position(&_masks_drag.start_x, &_masks_drag.start_y);
}

// a click on the handle or name arms the row's drag source, so it can end in
// "drag-begin" instead of a release: a real drag, or on macOS a click without
// movement
// The row is not selected here, which would move the list under the drag (see
// _masks_drag): a drop selects what it moved, and a drag that ends where it
// began without a drop was such a click, which _masks_drag_end turns into the
// selection it should have made. See masks_row_click_handled's own comment in
// blend.h for how this pairs with _row_click_release to avoid acting twice.
static void _row_drag_begin(GtkWidget *w, GdkDragContext *dc, dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!bd) return;
  bd->masks_row_click_entered = _entered_object();
  bd->masks_row_click_handled = TRUE;
  _masks_drag_begin(module, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "formid")));
}

// matching release for _row_click_press's plain-click case. ctrl+click and
// right-click are both handled entirely on press and must not also do
// anything here. Connected as "released" alone, never through
// dt_gui_connect_click(), which would also turn the cancel a starting drag
// causes into a release
static void _row_click_release(GtkGestureSingle *gesture,
                               const int n_press,
                               const double x,
                               const double y,
                               dt_iop_module_t *module)
{
  if(gtk_gesture_single_get_current_button(gesture) != GDK_BUTTON_PRIMARY) return;
  const GdkModifierType state = dt_gui_current_state(gesture);
  if(dt_modifier_is(state, GDK_CONTROL_MASK)) return;
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(bd->masks_skip_group_select_release)
  {
    bd->masks_skip_group_select_release = FALSE;
    return;
  }
  // _row_drag_begin already selected this row for this same press -- see its
  // own comment. Consume the flag and stop, so this release cannot also
  // toggle the selection it just set (or run the shift-click branch a second
  // time for a gesture drag-begin already resolved).
  if(bd->masks_row_click_handled)
  {
    bd->masks_row_click_handled = FALSE;
    return;
  }
  GtkWidget *w = dt_gui_get_widget(gesture);
  const dt_mask_id_t id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "formid"));
  bd->masks_row_click_entered = _entered_object();
  if(dt_modifier_is(state, GDK_SHIFT_MASK))
  {
    if(bd->panel_selected_formid != id) _set_form_target_ext(module, id, FALSE);
    _toggle_expand_widget(g_object_get_data(G_OBJECT(w), "handle-widget"));
    return;
  }
  _select_form(module, id);
}

// the row or header a crossing event box drives the hover wash for: for an
// element row, the row the evbox or editor is packed in (row_vbox wraps
// row_evbox); for a group or cluster header, the evbox's own child (hdr_evbox
// wraps hdr). NULL if neither shape matches
static GtkWidget *_row_widget_for_hover(GtkWidget *w)
{
  GtkWidget *parent = gtk_widget_get_parent(w);
  if(_has_class(parent, "dt_masks_row")) return parent;
  if(GTK_IS_BIN(w))
  {
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(w));
    if(_has_class(child, "dt_masks_header")) return child;
  }
  return NULL;
}

// the row the wash is on, weak since a list rebuild destroys it. Taking it off
// that one row beats walking the list for every row a scroll passes over, and
// the walk is only needed when this is unknown -- a leave is not always
// reliably paired with the matching enter (the pointer can move from one row's
// own GdkWindow straight onto an adjacent row's without a clean crossing
// sequence for the first one), and a stale wash is easily mistaken for that row
// being selected, since both look alike
static GtkWidget *_hover_washed_row = NULL;

static void _hover_washed_set(GtkWidget *row)
{
  if(_hover_washed_row)
    g_object_remove_weak_pointer(G_OBJECT(_hover_washed_row),
                                 (gpointer *)&_hover_washed_row);
  _hover_washed_row = row;
  if(_hover_washed_row)
    g_object_add_weak_pointer(G_OBJECT(_hover_washed_row),
                              (gpointer *)&_hover_washed_row);
}

// the hover wash in the list, on the header line of the row or header
// `target` or on none. Cheap, so it follows the pointer at once, unlike the
// canvas half below
static void _row_hover_wash(dt_iop_gui_blend_data_t *bd, GtkWidget *target)
{
  if(_hover_washed_row)
    dt_gui_remove_class(_hover_washed_row, "dt_masks_hovered");
  else if(bd && bd->masks_list_box)
    _clear_hover_classes(GTK_WIDGET(bd->masks_list_box));
  if(target)
  {
    target = _row_header_line(target);
    dt_gui_add_class(target, "dt_masks_hovered");
  }
  _hover_washed_set(target);
}

// the hovered shapes solo edit has brought onto the canvas (see
// _soloedit_hover_scope), so the canvas is only rebuilt when they change
static GList *_soloedit_hover_extra = NULL;

static gboolean _canvas_shows_form(const dt_mask_id_t formid)
{
  const dt_masks_form_t *vis = darktable.develop->form_visible;
  for(const GList *l = vis ? vis->points : NULL; l; l = g_list_next(l))
    if(((const dt_masks_point_group_t *)l->data)->formid == formid) return TRUE;
  return FALSE;
}

// solo edit narrows the canvas to the isolated shapes, so a hovered row
// outside them had nothing on the canvas to highlight. Bring its shapes in for
// as long as the hover lasts, and back out after: the list can then find any
// shape, not only those of the current selection. Parametric and raster
// elements have no outline to bring in
static void _soloedit_hover_scope(dt_iop_module_t *module, const GList *hovered)
{
  // rebuilding the canvas would end the shape being drawn
  if(darktable.develop->form_gui && darktable.develop->form_gui->creation)
  {
    g_list_free(_soloedit_hover_extra);
    _soloedit_hover_extra = NULL;
    return;
  }

  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!dt_is_valid_maskid(bd->soloedit_formid))
  {
    // solo edit going off rebuilt the canvas as the whole mask already
    g_list_free(_soloedit_hover_extra);
    _soloedit_hover_extra = NULL;
    return;
  }

  GList *solo = _soloedit_formids(module);
  GList *extra = NULL;
  for(const GList *l = hovered; l; l = g_list_next(l))
  {
    if(g_list_find(solo, l->data)) continue;
    const dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, GPOINTER_TO_INT(l->data));
    if(!form || (form->type & (DT_MASKS_PARAMETRIC | DT_MASKS_RASTER))) continue;
    extra = g_list_append(extra, l->data);
  }

  // checked against the canvas too, not only against the last hover: any
  // re-narrowing of solo edit (a selection change) drops what a hover added
  gboolean same = g_list_length(extra) == g_list_length(_soloedit_hover_extra);
  for(const GList *a = extra, *b = _soloedit_hover_extra; same && a;
      a = g_list_next(a), b = g_list_next(b))
    same = a->data == b->data && _canvas_shows_form(GPOINTER_TO_INT(a->data));

  if(!same)
  {
    // rebuilding the canvas drops its selection: put it back, as
    // _soloedit_follow_selection does
    const dt_mask_id_t canvas_sel = darktable.develop->mask_form_selected_id;
    GList *ids = g_list_concat(g_list_copy(solo), g_list_copy(extra));
    dt_masks_set_edit_mode_forms(module, ids, DT_MASKS_EDIT_FULL);
    g_list_free(ids);
    darktable.develop->mask_form_selected_id = canvas_sel;
  }

  g_list_free(solo);
  g_list_free(_soloedit_hover_extra);
  _soloedit_hover_extra = extra;
}

// apply a hover: the wash in the list, and the shapes the canvas highlights
static void _row_hover_apply(dt_iop_module_t *module, GtkWidget *w)
{
  dt_masks_form_gui_t *gui = darktable.develop ? darktable.develop->form_gui : NULL;
  dt_iop_gui_blend_data_t *bd = module ? module->blend_data : NULL;
  if(!gui || !bd) return;
  _soloedit_hover_scope(module, w ? g_object_get_data(G_OBJECT(w), "hover-formids") : NULL);
  g_list_free(gui->panel_hover_formids);
  gui->panel_hover_formids =
    w ? g_list_copy(g_object_get_data(G_OBJECT(w), "hover-formids")) : NULL;
  _row_hover_wash(bd, w ? _row_widget_for_hover(w) : NULL);
  dt_control_queue_redraw_center();
}

// applying a hover costs a walk of the list and a canvas redraw, which draws
// every shape of the mask (dt_group_events_post_expose) -- cheap once, but a
// scroll drags row after row under a pointer that never moves, and with a
// complex mask those redraws outpace the scrolling itself. So the first
// crossing of a burst is applied at once and the rest at most this often,
// with the last one always landing
#define MASKS_HOVER_APPLY_MS 100
// and while the list is actually scrolling, none is applied at all: what the
// pointer passes over on the way is nothing the user is pointing at. The hover
// settles this long after the scrolling stops
#define MASKS_HOVER_SETTLE_MS 150
static guint _hover_apply_source = 0;
static gint64 _hover_apply_last = 0;
static gint64 _hover_scroll_seen = 0;
static gdouble _hover_scroll_pos = -1.0;
static dt_iop_module_t *_hover_pending_module = NULL;
// weak, since a list rebuild destroys the row while this waits on it. It then
// reads as "no row", which is what a rebuild means for a hover anyway
static GtkWidget *_hover_pending_row = NULL;

static void _hover_pending_set(dt_iop_module_t *module, GtkWidget *w)
{
  if(_hover_pending_row)
    g_object_remove_weak_pointer(G_OBJECT(_hover_pending_row),
                                 (gpointer *)&_hover_pending_row);
  _hover_pending_row = w;
  if(_hover_pending_row)
    g_object_add_weak_pointer(G_OBJECT(_hover_pending_row),
                              (gpointer *)&_hover_pending_row);
  _hover_pending_module = module;
}

// has the list scrolled since this was last asked? Read from the adjustment of
// whichever scrolled window currently holds the panel, so it needs no signal of
// its own and survives the panel moving between its hosts
static gboolean _hover_list_scrolling(const gint64 now)
{
  dt_iop_gui_blend_data_t *bd =
    _hover_pending_module ? _hover_pending_module->blend_data : NULL;
  GtkWidget *list = bd && bd->masks_list_box ? GTK_WIDGET(bd->masks_list_box) : NULL;
  GtkWidget *sw = list ? gtk_widget_get_ancestor(list, GTK_TYPE_SCROLLED_WINDOW) : NULL;
  if(sw)
  {
    const gdouble pos =
      gtk_adjustment_get_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw)));
    if(pos != _hover_scroll_pos)
    {
      // the first sample is a reading, not a movement
      if(_hover_scroll_pos >= 0.0) _hover_scroll_seen = now;
      _hover_scroll_pos = pos;
    }
  }
  return now < _hover_scroll_seen + MASKS_HOVER_SETTLE_MS * 1000;
}

// when the pending hover may be applied: not before the rate limit, and not
// while the list is still scrolling
static gint64 _row_hover_due(const gint64 now)
{
  gint64 due = _hover_apply_last + MASKS_HOVER_APPLY_MS * 1000;
  if(_hover_list_scrolling(now))
    due = MAX(due, _hover_scroll_seen + MASKS_HOVER_SETTLE_MS * 1000);
  return due;
}

static gboolean _row_hover_deferred(gpointer user_data);

static void _row_hover_arm(const gint64 delay)
{
  _hover_apply_source =
    g_timeout_add((guint)(delay / 1000 + 1), _row_hover_deferred, NULL);
}

static gboolean _row_hover_deferred(gpointer user_data)
{
  _hover_apply_source = 0;
  const gint64 now = g_get_monotonic_time();
  const gint64 due = _row_hover_due(now);
  if(due > now)
  {
    // still scrolling: come back once it has stopped
    _row_hover_arm(due - now);
    return G_SOURCE_REMOVE;
  }
  _hover_apply_last = now;
  dt_iop_module_t *module = _hover_pending_module;
  GtkWidget *w = _hover_pending_row;
  _hover_pending_set(NULL, NULL);
  if(module && module->blend_data) _row_hover_apply(module, w);
  return G_SOURCE_REMOVE;
}

// the module's panel is going away: nothing pending may outlive it
static void _row_hover_cancel(const dt_iop_module_t *module)
{
  if(_hover_pending_module != module) return;
  if(_hover_apply_source) g_source_remove(_hover_apply_source);
  _hover_apply_source = 0;
  _hover_pending_set(NULL, NULL);
}

static void _row_hover_schedule(dt_iop_module_t *module, GtkWidget *w)
{
  _hover_pending_set(module, w);
  if(_hover_apply_source) return; // the one on its way takes the latest target
  const gint64 now = g_get_monotonic_time();
  const gint64 due = _row_hover_due(now);
  if(now >= due)
    _row_hover_deferred(NULL);
  else
    _row_hover_arm(due - now);
}

// list -> canvas hover: hovering a mask-list row highlights its shape on the
// canvas; hovering a cluster header highlights every member shape. The hover
// target ids are carried on the event box as "hover-formids" (a one-element list
// for a single row, the whole member set for a cluster header). The box has a
// real window so crossings into its child buttons report GDK_NOTIFY_INFERIOR,
// which we ignore so the hover stays stable across the row's controls.
// Also drives the row's own hover wash in the list (mirroring the canvas ->
// list sync in dt_iop_gui_blend_masks_hover_form), so hovering a row highlights it
// exactly like hovering its shape on the canvas does.
static void _row_crossing(GtkEventControllerMotion *controller,
                          const gboolean entering,
                          dt_iop_module_t *module)
{
  GdkEvent *event = dt_gui_get_current_event(GTK_EVENT_CONTROLLER(controller));
  if(!event) return;
  const GdkEventCrossing *ev = &event->crossing;
  // interacting with one of this row's controls must keep the shape highlighted
  // for as long as the interaction lasts, not just while the pointer happens to
  // sit inside the row: dragging a slider (or opening a bauhaus popup, which
  // takes a gtk grab) delivers a leave the moment the grab starts, and the drag
  // itself routinely carries the pointer well outside the row. Both are ignored
  // here -- the matching ungrab crossing, or the next real pointer crossing,
  // settles the hover once the interaction is over.
  const gboolean ignore =
    ev->detail == GDK_NOTIFY_INFERIOR
    || (!entering
        && (ev->mode == GDK_CROSSING_GRAB || ev->mode == GDK_CROSSING_GTK_GRAB
            || (ev->state & (GDK_BUTTON1_MASK | GDK_BUTTON2_MASK | GDK_BUTTON3_MASK))));
  gdk_event_free(event);
  if(ignore || !darktable.develop || !darktable.develop->form_gui) return;
  // the wash is what the pointer expects to see immediately; the canvas, which
  // redraws every shape of the mask, follows when it can. While the list is
  // scrolling neither happens: the rows going past are not being pointed at
  GtkWidget *w = dt_gui_get_widget(controller);
  if(!_hover_list_scrolling(g_get_monotonic_time()))
    _row_hover_wash(module->blend_data, entering ? _row_widget_for_hover(w) : NULL);
  _row_hover_schedule(module, entering ? w : NULL);
}

static void _row_enter(GtkEventControllerMotion *controller,
                       const double x,
                       const double y,
                       dt_iop_module_t *module)
{
  _row_crossing(controller, TRUE, module);
}

static void _row_leave(GtkEventControllerMotion *controller, dt_iop_module_t *module)
{
  _row_crossing(controller, FALSE, module);
}

// make windowed event box `evbox` drive the hover of `formids`, which it takes
// (see _row_crossing)
static void _wire_row_hover(GtkWidget *evbox, dt_iop_module_t *module, GList *formids)
{
  g_object_set_data_full(G_OBJECT(evbox), "hover-formids", formids,
                         (GDestroyNotify)g_list_free);
  dt_gui_connect_motion(evbox, NULL, _row_enter, _row_leave, module);
}

// a windowed event box around `child`, hovering element `fid`: a windowless
// box only sees crossings in the gaps between its children
static GtkWidget *_element_hover_box(GtkWidget *child, dt_iop_module_t *module, const dt_mask_id_t fid)
{
  GtkWidget *evbox = gtk_event_box_new();
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(evbox), TRUE);
  gtk_container_add(GTK_CONTAINER(evbox), child);
  _wire_row_hover(evbox, module, g_list_prepend(NULL, GINT_TO_POINTER(fid)));
  return evbox;
}

// --- group headers -----------------------------------------------------------
// every group has a header in the list (see dt_masks_gui_build_list);
// same-kind runs of elements in a group fold into a cluster, so that tens of
// brush strokes stay manageable (see cluster_min in _pack_group_elements)

// a group is named, and numbered, after how it combines its own members.
// Numbered by operator family: the unions share one series and the
// intersections another
int dt_masks_gui_flexi_op_index_for_state(const int state)
{
  if(state & (DT_MASKS_STATE_FLEXI_MINIMUM | DT_MASKS_STATE_FLEXI_PRODUCT)) return 1;
  if(state & DT_MASKS_STATE_FLEXI_DIFFERENCE) return 2;
  if(state & DT_MASKS_STATE_FLEXI_EXCLUSION) return 3;
  return 0; // the unions: maximum (no bit), screen, sum
}

// commit a group's rename entry: the text is the group's name, held by its
// marker. The group of a mask with no group form yet becomes real here
// (see dt_masks_gui_module_flexi_group)
static void _group_rename_commit(GtkWidget *entry, dt_iop_module_t *module)
{
  if(g_object_get_data(G_OBJECT(entry), "done")) return; // guard double commit
  g_object_set_data(G_OBJECT(entry), "done", GINT_TO_POINTER(1));
  gchar *txt = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));
  if(txt) g_strstrip(txt);
  dt_mask_id_t cid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(entry), "group-cid"));
  if(txt && *txt)
  {
    dt_masks_point_group_t *marker = dt_masks_gui_group_point(dt_masks_gui_module_flexi_group(module, &cid), cid);
    if(marker)
    {
      // the marker is stored as raw bytes, so no tail of the old name may stay
      dt_strlcpy_to_fixed(marker->name, txt, sizeof(marker->name));
      dt_print(DT_DEBUG_MASKS, "[masks] group %d renamed to '%s'", cid, txt);
      dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
    }
  }
  g_free(txt);
  // deferred -- see the same comment on _rename_commit
  _queue_masks_list_rebuild(module);
}

static gboolean
_group_rename_focus_out(GtkWidget *entry, GdkEvent *e, dt_iop_module_t *module)
{
  _group_rename_commit(entry, module);
  return FALSE;
}

// Escape abandons the edit and restores whatever text the entry started with
// (a custom name, or empty if there wasn't one yet) instead of committing --
// shared by both a populated and an empty group's rename entry. Sets the same
// "done" guard _group_rename_commit uses, so the focus-out event the
// subsequent rebuild's teardown fires on this entry does not also commit.
static gboolean _group_rename_key_pressed(GtkEventControllerKey *controller,
                                          const guint keyval,
                                          const guint keycode,
                                          const GdkModifierType state,
                                          dt_iop_module_t *module)
{
  if(keyval != GDK_KEY_Escape) return FALSE;
  g_object_set_data(G_OBJECT(dt_gui_get_widget(controller)), "done", GINT_TO_POINTER(1));
  // a cancel changes no data, so the list signature is made stale by hand:
  // dt_masks_gui_build_list would skip the rebuild, and only a rebuild brings
  // back the label the entry replaced (see _start_group_rename)
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(bd) bd->masks_list_sig = DT_INVALID_HASH;
  _queue_masks_list_rebuild(module);
  return TRUE;
}

void dt_iop_gui_blend_masks_creation_ended(dt_iop_module_t *module)
{
  if(!module || !module->blend_data) return;
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  _masks_shapes_set_inactive(bd);
  // deferred, not direct: dt_masks_change_form_gui is itself called from the
  // middle of dt_masks_set_edit_mode, before edit_mode and selection state have
  // finished transitioning, and a synchronous rebuild there reenters
  // dt_masks_gui_build_list on that half-set state (see the note in
  // dt_masks_change_form_gui). The idle fires once the caller has unwound.
  _queue_masks_list_rebuild(module);
}

// dev->forms/history was rewritten wholesale from under the panel (undo/redo,
// jump to a history step, style paste, snapshot restore, compress history, a
// module reset -- see dt_dev_reload_history_items). The groups came back with
// the forms; what has to start over is the panel's one-shot selection seeding
// and the signature it skips a rebuild on
void dt_iop_gui_blend_forms_reloaded(dt_iop_module_t *module)
{
  if(!module) return;
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!bd) return;
  // a reload reaches every module, but only one showing the list has one to
  // rebuild. Rebuilding them all on each undo is slow, and disturbs the right
  // panel's scroll position
  const gboolean had_content = bd->masks_list_sig != DT_INVALID_HASH;
  const gboolean flexi =
    module->blend_params && (module->blend_params->mask_mode & DEVELOP_MASK_FLEXI);
  if(had_content || flexi)
  {
    bd->masks_list_sig = DT_INVALID_HASH;
    if(bd->masks_list_box) _queue_masks_list_rebuild(module);
  }
}

// move every member of a dragged cluster together, preserving their relative
// (bottom-up) order, to the position/group a drop indicates -- the same move
// dt_masks_model_drop_point_onto_point / dt_masks_model_drop_point_onto_group do for one shape,
// generalized to a same-kind run's whole member set. `dst` is either a group's
// id (dst_is_group: the cluster lands on top of it) or the target row's own
// formid (drop lands directly above/below it, per `above`). Returns FALSE
// (no-op) if `member_ids` is empty, `dst` is itself one of the members, or
// the cluster is dropped on the group it is already in.
gboolean dt_masks_gui_cluster_move(dt_iop_module_t *module,
                                   GList *member_ids,
                                   const dt_mask_id_t dst,
                                   const gboolean dst_is_group,
                                   const gboolean above)
{
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  if(!grp || !member_ids) return FALSE;

  for(GList *l = member_ids; l; l = g_list_next(l))
    if(GPOINTER_TO_INT(l->data) == dst) return FALSE;
  dt_masks_form_t *downer = NULL, *sowner = NULL;
  GList *d = _point_node_owner(grp, dst, &downer);
  if(!d || (!dst_is_group && dt_masks_point_is_marker(d->data))) return FALSE;
  // a cluster's members sit next to each other in one group's list
  if(!_point_node_owner(grp, GPOINTER_TO_INT(member_ids->data), &sowner)) return FALSE;

  // recover the cluster's own relative order from the list holding it (the
  // DnD payload itself carries no meaningful order, see
  // _masks_cluster_drag_get)
  GList *ordered = NULL;
  for(GList *l = sowner->points; l; l = g_list_next(l))
    if(!dt_masks_point_is_marker(l->data)
       && g_list_find(member_ids,
                      GINT_TO_POINTER(((dt_masks_point_group_t *)l->data)->formid)))
      ordered = g_list_append(ordered, l->data);

  // as for one element: never onto its own group, never where a nested
  // group may not go (see _may_move_into)
  gboolean ok = ordered != NULL && !(dst_is_group && sowner == downer);
  for(GList *l = ordered; l && ok; l = g_list_next(l))
    ok = _may_move_into(grp, sowner, downer, ((dt_masks_point_group_t *)l->data)->formid);
  if(!ok)
  {
    g_list_free(ordered);
    return FALSE;
  }

  for(GList *l = ordered; l; l = g_list_next(l))
  {
    sowner->points = g_list_remove(sowner->points, l->data);
    ((dt_masks_point_group_t *)l->data)->parentid = downer->formid;
  }
  // on top of dst's group, or next to dst: a member, so d->prev is at worst
  // its group's marker
  GList *at = dst_is_group ? g_list_last(downer->points) : above ? d : d->prev;
  _insert_points_after(downer, at, ordered);
  g_list_free(ordered);
  return TRUE;
}

// group `cid` of the mask `grp` of `module`, named as its header names it
static gchar *_group_label(dt_iop_module_t *module, dt_masks_form_t *grp, const dt_mask_id_t cid)
{
  if(cid == _mask_group_cid(module)) return g_strdup(_("whole mask"));
  const char *custom = _group_custom_name(grp, cid);
  if(custom) return g_strdup(custom);
  const dt_masks_point_group_t *head = dt_masks_gui_group_point(grp, cid);
  return g_strdup_printf("%s-%d", _flexi_op_short_name(head ? head->state : 0),
                         dt_masks_gui_group_ordinal_of_cid(module, cid));
}

// remember a widget's tooltip text as set at construction time, so a later
// disabled-state update (_update_add_target_hints) can append a hint
// without clobbering the button's own description
static void _stash_base_tooltip(GtkWidget *w)
{
  gchar *base = gtk_widget_get_tooltip_text(w);
  g_object_set_data_full(G_OBJECT(w), "dt-base-tooltip", base, g_free);
}

// append `hint` (may be "") to a widget's construction-time tooltip, replacing
// whatever hint was appended last time round
static void _append_tooltip_hint(GtkWidget *w, const char *hint)
{
  const char *base = g_object_get_data(G_OBJECT(w), "dt-base-tooltip");
  if(!base) return;
  gchar *tt = g_strconcat(base, hint, NULL);
  gtk_widget_set_tooltip_text(w, tt);
  g_free(tt);
}

// the group a new element lands in: the selected one, or else the mask's own.
// Both the add buttons' tooltips (_update_add_target_hints) and the insertion
// (_recompute_insert_hint) ask here, so that they cannot disagree
dt_mask_id_t dt_masks_gui_resolve_add_target(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  if(dt_is_valid_maskid(bd->panel_selected_group_cid) && grp
     && dt_masks_gui_group_point(grp, bd->panel_selected_group_cid))
    return bd->panel_selected_group_cid;
  // a mask with no group form yet has no id for its group, and its first
  // element creates it (see dt_masks_group_insert_point)
  return _mask_group_cid(module);
}

// the whole-mask refinement acts on the final mask, so it is always
// sensitive. A group or element refinement needs a member to refine: an empty
// group adds nothing to the mask. Called after each list rebuild, from
// dt_iop_gui_update_blending, and when the selection retargets the scope
// (_flexi_refine_follow_selection)
static void _update_refine_sensitivity(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!bd) return;

  gboolean active = TRUE;
  if(bd->masks_refine_scope_kind == REFINE_SCOPE_GROUP)
  {
    dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
    GList *ids = dt_masks_model_group_members(grp, bd->masks_refine_scope_formid);
    active = ids != NULL;
    g_list_free(ids);
  }
  else if(bd->masks_refine_scope_kind == REFINE_SCOPE_ELEMENT)
  {
    // masks_refine_scope_formid stays stale until the next selection change
    // when its element is deleted, so check that the point still exists
    dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
    active = grp && dt_masks_gui_group_point(grp, bd->masks_refine_scope_formid) != NULL;
  }
  // REFINE_SCOPE_GLOBAL always targets something real

  // is the current target's refinement bypassed?
  gpointer key = _refine_scope_key(bd);
  gboolean bypassed = FALSE;
  if(bd->masks_refine_bypassed)
    bypassed = GPOINTER_TO_INT(g_hash_table_lookup(bd->masks_refine_bypassed, key));
  if(bypassed) active = FALSE;

  if(bd->masks_refine_section_label)
    _append_tooltip_hint(bd->masks_refine_section_label,
                         active     ? ""
                         : bypassed ? _("\n(disabled for this target: the eye on the left"
                                        " enables it again)")
                                    : _("\n(the target is empty, so there is nothing to"
                                        " refine: deselect it to refine the whole mask)"));

  if(bd->masks_feathering_guide_combo)
    gtk_widget_set_sensitive(bd->masks_feathering_guide_combo, active);
  if(bd->feathering_radius_slider)
    gtk_widget_set_sensitive(bd->feathering_radius_slider, active);
  if(bd->blur_radius_slider) gtk_widget_set_sensitive(bd->blur_radius_slider, active);
  if(bd->brightness_slider) gtk_widget_set_sensitive(bd->brightness_slider, active);
  if(bd->contrast_slider) gtk_widget_set_sensitive(bd->contrast_slider, active);
  if(bd->details_slider) gtk_widget_set_sensitive(bd->details_slider, active);
}

// say on the add-element controls (shapes, parametric channels, new group,
// import) where what they add lands, and refresh the refinement-scope combo to
// match the current selection. Shared by dt_masks_gui_build_list (full
// rebuild) and the lightweight, no-rebuild selection paths
// (_set_group_target, _select_group) so group selection never needs a full
// list rebuild just to keep these in step.
static void _update_add_target_hints(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  // the group dt_masks_gui_resolve_add_target picks: the selected one, or the
  // mask's own
  const dt_mask_id_t target = dt_masks_gui_resolve_add_target(module);
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  gchar *group_name =
    grp && target != _mask_group_cid(module) ? _group_label(module, grp, target) : NULL;
  gchar *hint = group_name ? g_strdup_printf(_("\n(added to the selected group, %s)"), group_name)
                           : g_strdup(_("\n(added to the whole mask)"));
  for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
    if(bd->masks_shapes[n]) _append_tooltip_hint(bd->masks_shapes[n], hint);
  // these also say so when the channel-preview mode is on, since then resting
  // on one does something beyond adding an element (see
  // _preview_on_hover_enter)
  if(bd->masks_param_channels_inner)
  {
    gchar *ch_hint =
      _preview_on_hover_is_on()
        ? g_strconcat(hint,
                      _("\nrest the pointer here to preview this channel"),
                      NULL)
        : g_strdup(hint);
    for(GList *l =
          gtk_container_get_children(GTK_CONTAINER(bd->masks_param_channels_inner));
        l; l = g_list_delete_link(l, l))
      _append_tooltip_hint(GTK_WIDGET(l->data), ch_hint);
    g_free(ch_hint);
  }

  // a new group goes where a new element would (see _stage_new_group)
  if(bd->masks_new_op)
  {
    gchar *group_hint =
      group_name ? g_strdup_printf(_("\n(nested in %s)"), group_name)
                 : g_strdup(_("\n(nested in the whole mask)"));
    _append_tooltip_hint(bd->masks_new_op, group_hint);
    g_free(group_hint);
  }

  // import adds elements to the same group
  if(bd->masks_import_btn)
  {
    gchar *tt = g_strconcat(_("link or copy shapes from other modules, copy their parametric\n"
                              "channels, or add or use another module's whole mask\n"
                              "(click to pick)"),
                            hint, NULL);
    gtk_widget_set_tooltip_text(bd->masks_import_btn, tt);
    g_free(tt);
  }
  g_free(hint);
  g_free(group_name);

  // forms may have been added, removed or renamed, or the selected group changed
  _refine_section_refresh(module);
}

// recompute the insertion hint read by dt_masks_gui_form_save_creation from the
// current target. The target itself is resolved by
// dt_masks_gui_resolve_add_target, shared with the add buttons' tooltips so
// that they say where the element lands
static void _recompute_insert_hint(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  bd->insert_active = FALSE;
  bd->insert_after_fid = INVALID_MASKID;

  // on top of the target group: after its top member, or after its marker when
  // it has none. The group of a mask with no group form yet has neither, and
  // dt_masks_group_insert_point creates it with the element
  dt_masks_form_t *g = _group_of(dt_masks_gui_module_mask_group(module),
                                 dt_masks_gui_resolve_add_target(module));
  if(g)
  {
    bd->insert_active = TRUE;
    bd->insert_after_fid = ((dt_masks_point_group_t *)g_list_last(g->points)->data)->formid;
  }
}

dt_masks_form_t *dt_masks_model_nested_group_of(dt_masks_form_t *grp, const dt_mask_id_t cid)
{
  dt_masks_form_t *owner = NULL;
  GList *node = _point_node_owner(grp, cid, &owner);
  return node && dt_masks_point_is_marker(node->data) && owner != grp ? owner : NULL;
}

gboolean dt_masks_model_move_group(dt_iop_module_t *module,
                                   const dt_mask_id_t src_cid,
                                   const dt_mask_id_t dst_cid,
                                   const gboolean above,
                                   const gboolean inside)
{
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  GList *dst = _point_node(grp, dst_cid);
  if(src_cid == dst_cid || !dst || !dt_masks_point_is_marker(dst->data)) return FALSE;
  // a group moves as the member its holder has for it, and the mask's own
  // group, which has no holder, never moves
  dt_masks_form_t *src = dt_masks_model_nested_group_of(grp, src_cid);
  if(!src) return FALSE;
  if(inside) return dt_masks_model_drop_element_onto_group(module, grp, src->formid, dst_cid);
  // beside a group is among its holder's members, and nothing is beside the
  // mask's own group
  dt_masks_form_t *beside = dt_masks_model_nested_group_of(grp, dst_cid);
  return beside
         && dt_masks_model_drop_element_onto_element(module, grp, src->formid, beside->formid, above);
}

// highest number currently held by a live group of flexi operator `mode`
// (see dt_masks_gui_flexi_op_index_for_state), 0 if none. A new group takes one past this, so
// a number is never handed out while a peer still shows it, and a series
// restarts at 1 once its last group is gone.
int dt_masks_gui_group_ord_max_for_flexi_op(dt_iop_module_t *module, const int mode)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  int mx = 0;

  if(bd->group_ordinals)
  {
    // numbers are the mask's, nested groups' included
    GList *pts = _mask_points(grp);
    for(GList *l = pts; l; l = g_list_next(l))
    {
      const dt_masks_point_group_t *head = l->data;
      if(!dt_masks_point_is_marker(head)) continue;
      if(dt_masks_gui_flexi_op_index_for_state(head->state) != mode) continue;
      const int ord = GPOINTER_TO_INT(
        g_hash_table_lookup(bd->group_ordinals, GINT_TO_POINTER(head->formid)));
      if(ord > mx) mx = ord;
    }
    g_list_free(pts);
  }
  return mx;
}

// a deleted group can leave bd->solo_group_key pointing at a cid that no
// longer identifies any group (the formid-keyed siblings are cleared by
// _clear_stale_formid_refs). Left stale, every row and header keeps reading
// "some group is soloed, and it isn't me" and dims to 45% opacity.
// Self-healing at rebuild (like dt_masks_gui_prune_group_ordinals) rather than chasing
// every mutation call site
void dt_masks_gui_prune_stale_solo(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(bd->solo_group_key == 0) return;
  const dt_masks_point_group_t *pt =
    dt_masks_gui_group_point(dt_masks_gui_module_mask_group(module), (dt_mask_id_t)bd->solo_group_key);
  if(!pt || !dt_masks_point_is_marker(pt)) bd->solo_group_key = 0;
}

// drop remembered numbers whose group no longer exists, so a series can restart
// at 1 once emptied (and the table does not grow across edits/images)
void dt_masks_gui_prune_group_ordinals(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!bd->group_ordinals) return;
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);

  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, bd->group_ordinals);
  while(g_hash_table_iter_next(&it, &k, &v))
  {
    const dt_masks_point_group_t *pt = dt_masks_gui_group_point(grp, GPOINTER_TO_INT(k));
    if(!pt || !dt_masks_point_is_marker(pt)) g_hash_table_iter_remove(&it);
  }
}

/* the group's displayed number: assigned once and kept while the group
   exists, in bd->group_ordinals by group id. Not its position, or deleting
   maximum-1 would turn maximum-2 into maximum-1, as if the wrong group had
   gone */
static int _group_ordinal_any(dt_iop_module_t *module, const dt_mask_id_t cid)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;

  // the group of a mask with no group form yet
  if(!dt_is_valid_maskid(cid)) return 1;
  const dt_masks_point_group_t *head = dt_masks_gui_group_point(dt_masks_gui_module_mask_group(module), cid);
  if(!head) return 0;
  // the mask's own group is "whole mask", not a numbered group: numbering it
  // would start its nested groups of the same mode at 2
  if(cid == _mask_group_cid(module)) return 0;

  if(!bd->group_ordinals)
    bd->group_ordinals = g_hash_table_new(g_direct_hash, g_direct_equal);

  int ord =
    GPOINTER_TO_INT(g_hash_table_lookup(bd->group_ordinals, GINT_TO_POINTER(cid)));
  if(ord <= 0)
  {
    ord = dt_masks_gui_group_ord_max_for_flexi_op(module, dt_masks_gui_flexi_op_index_for_state(head->state)) + 1;
    g_hash_table_insert(bd->group_ordinals, GINT_TO_POINTER(cid), GINT_TO_POINTER(ord));
  }
  return ord;
}

/* number every group that has none, in render order (bottom up), so that a
   first build numbers groups as they are stacked; a later group takes the
   next free number of its operator. Called once per rebuild, after
   dt_masks_gui_prune_group_ordinals */
static void _assign_group_ordinals(dt_iop_module_t *module)
{
  GList *pts = _mask_points(dt_masks_gui_module_mask_group(module));
  for(GList *l = pts; l; l = g_list_next(l))
    if(dt_masks_point_is_marker(l->data))
      _group_ordinal_any(module, ((dt_masks_point_group_t *)l->data)->formid);
  g_list_free(pts);
}

// 1-based per-operator ordinal of group `cid` (see _group_ordinal_any)
int dt_masks_gui_group_ordinal_of_cid(dt_iop_module_t *module, const dt_mask_id_t cid)
{
  return _group_ordinal_any(module, cid);
}

// "add group": a new empty group, selected, on top of the members of the
// group new elements go to -- the selected one, or the mask's own -- folding
// its members with the flexi operator `flexi_op`. A group is part of the
// mask now, so it is recorded
static void _stage_new_group(dt_iop_module_t *module, const int flexi_op)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  dt_mask_id_t cid = dt_masks_gui_resolve_add_target(module);
  dt_masks_form_t *grp = dt_masks_gui_module_flexi_group(module, &cid);
  if(!grp) return;
  const dt_mask_id_t nid = dt_masks_model_nest_new_group(grp, flexi_op, cid);
  if(!dt_is_valid_maskid(nid))
  {
    dt_control_log(_("this group is nested as deep as groups go"));
    return;
  }
  bd->panel_selected_formid = INVALID_MASKID;
  bd->panel_selected_group_cid = nid;
  bd->masks_new_group_op = flexi_op & DT_MASKS_STATE_FLEXI_OP;
  dt_print(DT_DEBUG_MASKS, "[masks] add group %d inside group %d flexi_op=0x%x", nid, cid,
           flexi_op);
  dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
  dt_masks_gui_build_list(module);
}

// select a real group by its header. The selected group is where the next drawn
// shape lands and what the refinement controls target. Clicking the already
// selected group selects the mask's own group instead (see dt_masks_model_click_group)
static void
_select_group(dt_iop_module_t *module, const dt_mask_id_t cid)
{
  // any open parametric editor stays open across a group-target change --
  // it is bound to a specific form, not to which group is selected.
  const dt_masks_panel_sel_t s = dt_masks_model_click_group(module->blend_data, cid);
  _set_group_target(module, s.group_cid);
}

// core of group selection: point the panel's "where do new elements go" target
// at the group cid (INVALID_MASKID to clear it), then update the header/row
// highlight and the dependent controls (add-element sensitivity, refinement
// scope) in place -- no list rebuild, so this never disturbs the GTK focus
// chain and never triggers the containing scrolled viewport to auto-scroll to
// a re-created widget (see _select_group / _select_form, which funnel group
// selection through here instead of dt_masks_gui_build_list).
// Selecting anything but the AI object stepped into steps out of it, as a click
// outside it on the canvas does; keep_entered is the element an element
// selection is on its way to, which may be that object.
static void _set_group_target_ext(dt_iop_module_t *module,
                                  const dt_mask_id_t cid,
                                  const dt_mask_id_t keep_entered)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  const dt_mask_id_t was_target = bd->panel_selected_group_cid;
  bd->panel_selected_formid = INVALID_MASKID;
  bd->panel_selected_group_cid = cid;
  _select_mask_group_if_none(bd);
  // the notes a just applied preset opened all stay open until work moves on
  if(bd->panel_selected_group_cid != was_target) bd->masks_notes_all_open = FALSE;
  bd->masks_shown = DT_MASKS_EDIT_FULL;
  dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
  // dt_masks_set_edit_mode(FULL) just rebuilt form_visible as the *whole*
  // group, which would silently widen an active solo-edit back to every
  // shape's outline (e.g. any group/header selection change routes through
  // here). Re-narrow immediately so solo-edit's canvas scope survives any
  // selection change while it's active.
  if(dt_is_valid_maskid(bd->soloedit_formid))
  {
    GList *ids = _soloedit_formids(module);
    dt_masks_set_edit_mode_forms(module, ids, DT_MASKS_EDIT_FULL);
    g_list_free(ids);
  }
  _update_row_selection(bd);
  _sync_group_notes(bd);
  _update_add_target_hints(module);
  // every group selection funnels through here, including the one an element
  // selection makes on its way to _set_form_target -- so this is the single
  // place the group half of "auto-expand selected" has to act
  _auto_expand_selected_group(module, bd->panel_selected_group_cid);
  const dt_mask_id_t entered = _entered_object();
  dt_masks_form_t *obj =
    dt_is_valid_maskid(entered) ? dt_masks_get_from_id(darktable.develop, entered) : NULL;
  // the object's own group and paths are inside it too
  const gboolean inside = keep_entered == entered
                          || (obj && ((dt_is_valid_maskid(keep_entered)
                                       && dt_masks_gui_group_point(obj, keep_entered))
                                      || (dt_is_valid_maskid(cid) && dt_masks_gui_group_point(obj, cid))));
  if(dt_is_valid_maskid(entered) && !inside) _step_object(module, INVALID_MASKID);
}

static void _set_group_target(dt_iop_module_t *module, const dt_mask_id_t cid)
{
  _set_group_target_ext(module, cid, INVALID_MASKID);
}

// flexi only: keep the insertion hint (where the next drawn shape lands) in step
// with the current selection on the no-rebuild selection path. The add-group icon
// is intentionally NOT touched here -- it only changes when the user picks an
// operator from the add-group menu.
static void _flexi_new_op_follow_selection(dt_iop_gui_blend_data_t *bd)
{
  if(!bd->module) return;
  if(bd->module->blend_params->mask_mode & DEVELOP_MASK_RASTER) return;
  _recompute_insert_hint(bd->module);
}

// the final click on an operator item of the add-group chooser
static void _new_shape_op_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  if(darktable.gui->active_popover_menu)
    gtk_popover_popdown(GTK_POPOVER(darktable.gui->active_popover_menu));
  if(module) _stage_new_group(module, g_variant_get_int32(parameter));
}

// the add-group operator chooser: every operator a group can fold its members
// with
static void _new_shape_op_pressed(GtkGestureSingle *gesture,
                                  const int n_press,
                                  const double x,
                                  const double y,
                                  GtkWidget *btn)
{
  if(gtk_gesture_single_get_current_button(gesture) != GDK_BUTTON_PRIMARY) return;
  dt_gui_claim(gesture);

  dt_iop_module_t *module = g_object_get_data(G_OBJECT(btn), "module");
  GActionGroup *action_group = gtk_widget_get_action_group(btn, "masks_new_op");
  if(action_group == NULL)
  {
    GActionEntry action_entries[] =
    {
      { "add", _new_shape_op_action, "i", NULL },
    };
    action_group = G_ACTION_GROUP(g_simple_action_group_new());
    g_action_map_add_action_entries(G_ACTION_MAP(action_group), action_entries,
                                    G_N_ELEMENTS(action_entries), module);
    gtk_widget_insert_action_group(btn, "masks_new_op", action_group);
    g_object_unref(action_group);
  }

  GMenu *menu = _flexi_op_menu_model("masks_new_op.add", -1);
  darktable.gui->active_popover_menu = dt_gui_popover_menu_from_model(btn, menu);
  gtk_popover_popup(GTK_POPOVER(darktable.gui->active_popover_menu));
  g_object_unref(menu);
}

// the group layout presets, which build a whole set of groups at once. They
// live on the toolbar rather than in the panel settings because that is what
// they are: a bulk "add group", not a preference
static void _masks_presets_pressed(GtkGestureSingle *gesture,
                                   const int n_press,
                                   const double x,
                                   const double y,
                                   dt_iop_module_t *module)
{
  if(gtk_gesture_single_get_current_button(gesture) != GDK_BUTTON_PRIMARY) return;
  if(!module->blend_data) return;
  if(module->blend_params->mask_mode & DEVELOP_MASK_RASTER) return;
  dt_gui_claim(gesture);
  GtkWidget *btn = dt_gui_get_widget(gesture);
  GMenu *menu = g_menu_new();
  dt_masks_gui_add_presets_menu(menu, btn, module);
  darktable.gui->active_popover_menu = dt_gui_popover_menu_from_model(btn, menu);
  gtk_popover_popup(GTK_POPOVER(darktable.gui->active_popover_menu));
  g_object_unref(menu);
}

// the bits identifying a shape's kind (ignoring clone/state flags)
static guint _form_kind(const dt_masks_form_t *form)
{
  return form->type
         & (DT_MASKS_CIRCLE | DT_MASKS_PATH | DT_MASKS_GRADIENT | DT_MASKS_ELLIPSE
            | DT_MASKS_BRUSH | DT_MASKS_PARAMETRIC | DT_MASKS_RASTER
#ifdef HAVE_AI
            | DT_MASKS_OBJECT
#endif
         );
}

/* detach every listed member from the module's mask group, and nothing else.
   Not dt_masks_form_remove(): with the group's last point gone, it deletes the
   group form too, which resets blend_params.mask_id to NO_MASKID and leaves
   the panel nothing to render from; and _group_reset_members keeps the group.

   The shapes stay in dev->forms, unused, until "clean up unused shapes"
   (_masks_import_cleanup_action). Callers commit one history item and one
   rebuild */
static void _detach_group_members(dt_masks_form_t *grp, GList *fids)
{
  // one point per listed id: a form listed twice is referenced twice
  for(GList *l = fids; l; l = g_list_next(l))
  {
    dt_masks_form_t *owner = NULL;
    GList *p = _point_node_owner(grp, GPOINTER_TO_INT(l->data), &owner);
    if(!p || dt_masks_point_is_marker(p->data)) continue;
    free(p->data);
    owner->points = g_list_delete_link(owner->points, p);
  }
}

// members whose form is gone from dev->forms. They render nothing (see
// _group_get_mask_roi_flexi) and have no row, but would count as members: a
// group holding only them would not read as empty. Dropped like any other
// detach, once per point: a form can be referenced more than once. Returns
// how many went
int dt_masks_model_prune_dangling_members(dt_masks_form_t *grp)
{
  GList *gone = NULL;
  // nested groups' members too: _detach_group_members takes each from the
  // list that holds it
  GList *pts = _mask_points(grp);
  for(GList *l = pts; l; l = g_list_next(l))
  {
    const dt_masks_point_group_t *pt = l->data;
    // a group marker refers to no form by design
    if(dt_masks_point_is_marker(pt)) continue;
    if(!dt_masks_get_from_id(darktable.develop, pt->formid))
      gone = g_list_prepend(gone, GINT_TO_POINTER(pt->formid));
  }
  g_list_free(pts);
  const int n = g_list_length(gone);
  if(gone) _detach_group_members(grp, gone);
  g_list_free(gone);
  return n;
}

// remove elements from the module's mask; their groups stay, even emptied
static void _delete_elements(dt_iop_module_t *module, GList *fids)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  if(!grp || !fids) return;
  dt_masks_clear_form_gui(darktable.develop);
  for(GList *l = fids; l; l = g_list_next(l))
    _clear_stale_formid_refs(bd, GPOINTER_TO_INT(l->data));
  _detach_group_members(grp, fids);
  _commit_structure_change(module);
}

// "delete group": the group goes, and its elements with it, as the member its
// holder has for it. The mask's own group stays: the mask is that group
static void _group_delete(dt_iop_module_t *module, const dt_mask_id_t cid)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  const dt_masks_form_t *nested =
    dt_masks_model_nested_group_of(dt_masks_gui_module_mask_group(module), cid);
  if(!nested) return;
  if(bd->panel_selected_group_cid == cid) bd->panel_selected_group_cid = INVALID_MASKID;
  _delete_single_shape(module, nested->formid, NULL);
}

// "empty group": its elements go, the group stays where it is, selected, with
// its settings and its number
static void _group_reset_members(dt_iop_module_t *module, const dt_mask_id_t cid)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  if(!grp) return;
  dt_masks_clear_form_gui(darktable.develop);
  GList *fids = dt_masks_model_empty_group(grp, cid);
  for(GList *l = fids; l; l = g_list_next(l))
    _clear_stale_formid_refs(bd, GPOINTER_TO_INT(l->data));
  g_list_free(fids);
  bd->panel_selected_group_cid = cid;
  _commit_structure_change(module);
}

static void _group_toggle_bypass(dt_iop_module_t *module, dt_mask_id_t cid);
static void _build_group_actions_menu(GtkWidget *anchor,
                                      dt_iop_module_t *module,
                                      const dt_mask_id_t cid,
                                      GtkWidget *lbl_box);

// start inline rename on a group's title: swap `lbl_box`'s label child for an
// entry, same gesture as renaming an element (ctrl+click, see
// _start_rename_element) -- but the typed text names the group (see
// _group_rename_commit). Shared by ctrl+click anywhere on the header that is
// not a control of its own (_group_header_press) and the "rename" entry of
// the group's actions menu
static void _start_group_rename(GtkWidget *lbl_box,
                                dt_iop_module_t *module,
                                const dt_mask_id_t cid)
{
  // the mask's own group is labeled "whole mask", never by a name of its own
  if(!lbl_box || cid == _mask_group_cid(module)) return;
  GtkWidget *current = g_object_get_data(G_OBJECT(lbl_box), "title-child");
  if(current && GTK_IS_ENTRY(current))
  {
    // already renaming -- see _row_click_press for why a fast repeated
    // ctrl+click must re-focus rather than destroy/recreate the entry
    gtk_widget_grab_focus(current);
    return;
  }
  // renaming acts on the group, so it should select it too (never deselect --
  // same select-only rule every other action control follows, see
  // _set_form_target) and the selection should still be there
  // once the rename commits: neither commit path touches selection, and
  // committing only ever rebuilds the list (which preserves it), so
  // selecting here is the one place this needs to happen.
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(bd->panel_selected_group_cid != cid) _set_group_target(module, cid);
  const char *custom = _group_custom_name(dt_masks_gui_module_mask_group(module), cid);
  if(current) gtk_widget_destroy(current);
  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_has_frame(GTK_ENTRY(entry), FALSE);
  dt_gui_add_class(entry, "dt_masks_rename_entry");
  // as narrow a request as the title label it replaces (see
  // _start_rename_element)
  gtk_entry_set_width_chars(GTK_ENTRY(entry), 1);
  gtk_entry_set_max_width_chars(GTK_ENTRY(entry), 1);
  if(custom) gtk_entry_set_text(GTK_ENTRY(entry), custom);
  g_object_set_data(G_OBJECT(entry), "group-cid", GINT_TO_POINTER(cid));
  g_object_set_data(G_OBJECT(lbl_box), "title-child", entry);
  dt_gui_box_add(lbl_box, dt_gui_expand(entry));
  gtk_box_reorder_child(GTK_BOX(lbl_box), entry, 0);
  g_signal_connect(G_OBJECT(entry), "activate", G_CALLBACK(_group_rename_commit), module);
  g_signal_connect(G_OBJECT(entry), "focus-out-event",
                   G_CALLBACK(_group_rename_focus_out), module);
  dt_gui_connect_key(entry, _group_rename_key_pressed, module);
  // disarm the header's drag source (on the ancestor tagged "group-key")
  // while renaming: it can arm on the ctrl+click without movement (as in
  // _row_drag_begin) and grab the pointer, taking the focus off the entry,
  // whose focus-out commits and destroys it. The rebuild that ends the rename
  // arms it again
  for(GtkWidget *w = lbl_box; w; w = gtk_widget_get_parent(w))
    if(g_object_get_data(G_OBJECT(w), "group-key"))
    {
      gtk_drag_source_unset(w);
      break;
    }
  gtk_widget_show(entry);
  gtk_widget_grab_focus(entry);
}

// the header event box: a plain primary press is left unclaimed, so it goes on
// to the module's body, which focuses the module (the group is selected on
// release, see below). Right-click opens the operator/actions menu (see below).
static void _group_header_press(GtkGestureSingle *gesture,
                                const int n_press,
                                const double x,
                                const double y,
                                dt_iop_module_t *module)
{
  // no double-click-to-solo: the first click's release always runs first,
  // (de)selecting the group before the second press is recognized. A group is
  // soloed from its actions menu (see _build_group_actions_menu) or by
  // shift+clicking its visibility button (see _visibility_group_pressed).
  // rename and the actions menu select the group first, as they do an element
  // (see _row_click_press), so the highlight shows what they act on
  GtkWidget *w = dt_gui_get_widget(gesture);
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  const dt_mask_id_t cid =
    (dt_mask_id_t)GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(w), "group-key"));
  const guint button = gtk_gesture_single_get_current_button(gesture);
  const gboolean rename = button == GDK_BUTTON_PRIMARY
                          && dt_modifier_is(dt_gui_current_state(gesture), GDK_CONTROL_MASK);
  const gboolean menu = button == GDK_BUTTON_SECONDARY;
  if((rename || menu) && bd->panel_selected_group_cid != cid)
    _set_group_target(module, cid);
  if(rename)
  {
    dt_gui_claim(gesture);
    _start_group_rename(g_object_get_data(G_OBJECT(w), "title-label-box"), module, cid);
    return;
  }
  if(menu)
  {
    dt_gui_claim(gesture);
    GtkWidget *lbl_box = g_object_get_data(G_OBJECT(w), "title-label-box");
    _build_group_actions_menu(w, module, cid, lbl_box);
    GdkRectangle rect = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(darktable.gui->active_popover_menu), &rect);
    gtk_popover_popup(GTK_POPOVER(darktable.gui->active_popover_menu));
    return;
  }
  bd->masks_skip_group_select_release = FALSE;
}

// select the group on release (a release is not delivered when a drag started,
// so dragging a group never also selects it). One after a drag that did end
// here anyway (see _group_drag_begin) takes the select-only branch instead: it
// selects the group if it wasn't already selected, but never deselects it --
// only a click on the title itself toggles selection off (see _select_group).
// Shift+click has no special meaning here: it falls through to a plain select,
// same as an unmodified click. Connected as "released" alone, as an element
// row's release is (see _row_click_release)
static void _group_header_release(GtkGestureSingle *gesture,
                                  const int n_press,
                                  const double x,
                                  const double y,
                                  dt_iop_module_t *module)
{
  if(gtk_gesture_single_get_current_button(gesture) != GDK_BUTTON_PRIMARY) return;
  // ctrl+click renames on press (_group_header_press), as in
  // _row_click_release: selecting here can queue a rebuild, which would
  // destroy the rename entry before anything is typed
  if(dt_modifier_is(dt_gui_current_state(gesture), GDK_CONTROL_MASK)) return;
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  const dt_mask_id_t cid =
    (dt_mask_id_t)GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(dt_gui_get_widget(gesture)),
                                                     "group-key"));
  if(bd->masks_skip_group_select_release)
  {
    bd->masks_skip_group_select_release = FALSE;
    if(bd->panel_selected_group_cid != cid) _set_group_target(module, cid);
    return;
  }
  _select_group(module, cid);
}

// the group's body (its block) uses the two handlers above, so that body and
// header agree on what a click means. The header and the rows sit inside the
// block, and a press none of them claims goes on to it, where the same toggle
// would undo the first. So act only on events for the block's own window: the
// part no child covers (padding, indent, gaps)
static gboolean _event_on_own_window(GtkGestureSingle *gesture)
{
  GdkEvent *event = dt_gui_get_current_event(GTK_EVENT_CONTROLLER(gesture));
  if(!event) return FALSE;
  const gboolean own =
    gdk_event_get_window(event) == gtk_widget_get_window(dt_gui_get_widget(gesture));
  gdk_event_free(event);
  return own;
}

static void _group_block_press(GtkGestureSingle *gesture,
                               const int n_press,
                               const double x,
                               const double y,
                               dt_iop_module_t *module)
{
  if(_event_on_own_window(gesture)) _group_header_press(gesture, n_press, x, y, module);
}

static void _group_block_release(GtkGestureSingle *gesture,
                                 const int n_press,
                                 const double x,
                                 const double y,
                                 dt_iop_module_t *module)
{
  if(_event_on_own_window(gesture)) _group_header_release(gesture, n_press, x, y, module);
}

// the state half of the group solo toggle, as dt_masks_model_toggle_solo_form
dt_masks_solo_canvas_t dt_masks_model_toggle_solo_group(dt_iop_module_t *module,
                                                        dt_masks_form_t *grp,
                                                        const guint key,
                                                        GList *members)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!grp) return DT_MASKS_SOLO_CANVAS_NONE;
  dt_masks_solo_canvas_t canvas = DT_MASKS_SOLO_CANVAS_NONE;

  if(bd->solo_group_key == key)
  {
    dt_masks_group_isolate_state(grp, NULL, DT_MASKS_STATE_HIDDEN);
    bd->solo_group_key = 0;
  }
  else
  {
    dt_masks_group_isolate_state(grp, members, DT_MASKS_STATE_HIDDEN);
    // only one thing is ever soloed: a group solo cancels any element solo
    bd->solo_formid = INVALID_MASKID;
    bd->solo_group_key = key;
    // same mutual-exclusivity rule as dt_masks_model_toggle_solo_form
    if(dt_is_valid_maskid(bd->soloedit_formid))
    {
      bd->soloedit_formid = INVALID_MASKID;
      canvas = DT_MASKS_SOLO_CANVAS_FULL;
    }
  }
  if(_model_clear_soloedit_if_hidden(module, grp))
    canvas = DT_MASKS_SOLO_CANVAS_FULL;
  return canvas;
}

// solo a whole group: show only its member shapes, hiding all others.
// Toggling off restores every hidden bit (solo is the only thing that sets
// DT_MASKS_STATE_HIDDEN). Triggered from the group's actions menu (see
// _build_group_actions_menu) or by clicking its solo badge to clear it
static void _toggle_solo_group(dt_iop_module_t *module, const dt_mask_id_t cid)
{
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  if(!grp) return;

  GList *members = dt_masks_model_group_members(grp, cid);
  const dt_masks_solo_canvas_t canvas =
    dt_masks_model_toggle_solo_group(module, grp, (guint)cid, members);
  g_list_free(members);
  if(canvas == DT_MASKS_SOLO_CANVAS_FULL)
    dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
  dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
  _sync_hidden_to_form_visible(module);
  // solo changes no row, so refresh them in place, as _toggle_solo_form does.
  // _refresh_all_shape_rows destroys no widget, so it is safe inside the menu
  // item's dispatch
  _refresh_all_shape_rows(module);
  _sync_solo_canvas_highlight(module);
  // same as _toggle_solo_form
  _soloedit_follow_selection(module->blend_data);
}

// the same, for a group (see _group_act_disable, _group_act_solo): solo is not
// offered on a disabled or an empty group
static void _visibility_group_pressed(GtkGestureSingle *gesture,
                                      const int n_press,
                                      const double x,
                                      const double y,
                                      dt_iop_module_t *module)
{
  if(gtk_gesture_single_get_current_button(gesture) != GDK_BUTTON_PRIMARY) return;
  dt_gui_claim(gesture);
  GtkWidget *w = dt_gui_get_widget(gesture);
  const dt_mask_id_t cid = _header_cid(w);
  if(!dt_modifier_is(dt_gui_current_state(gesture), GDK_SHIFT_MASK))
    _group_toggle_bypass(module, cid);
  else if(_visibility_status_get(w) != MASK_VISIBILITY_DISABLED
          && GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "can-solo")))
    _toggle_solo_group(module, cid);
}

// set a group's flexi operator, on its marker. Maximum sets no bit, so a
// group that never touches this stays byte-identical
static void
_flexi_op_apply(dt_iop_module_t *module, dt_mask_id_t cid, const dt_masks_state_t flexi_op)
{
  dt_masks_point_group_t *marker = dt_masks_gui_group_point(dt_masks_gui_module_flexi_group(module, &cid), cid);
  if(!marker) return;
  marker->state = (marker->state & ~DT_MASKS_STATE_FLEXI_OP) | (flexi_op & DT_MASKS_STATE_FLEXI_OP);
  dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
  dt_masks_gui_build_list(module);
}

// the marker of the mask's own group, the one every other group nests in:
// the first of its list. INVALID_MASKID for a mask with no group form yet
static dt_mask_id_t _mask_group_cid(dt_iop_module_t *module)
{
  const dt_masks_form_t *grp = module ? dt_masks_gui_module_mask_group(module) : NULL;
  return grp && grp->points && dt_masks_point_is_marker(grp->points->data)
           ? ((dt_masks_point_group_t *)grp->points->data)->formid
           : INVALID_MASKID;
}

// one group is always selected: with none, the mask's own. That is also why it
// is the one group a click cannot deselect (see dt_masks_model_click_group)
static void _select_mask_group_if_none(dt_iop_gui_blend_data_t *bd)
{
  if(!bd || dt_is_valid_maskid(bd->panel_selected_group_cid)) return;
  bd->panel_selected_group_cid = _mask_group_cid(bd->module);
}

static void _flexi_op_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  GtkWidget *anchor = GTK_WIDGET(user_data);
  dt_iop_module_t *module = g_object_get_data(G_OBJECT(anchor), "module");
  const dt_mask_id_t cid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(anchor), "flexi_op_cid"));
  const dt_masks_state_t flexi_op = (dt_masks_state_t)g_variant_get_int32(parameter);
  if(darktable.gui->active_popover_menu)
    gtk_popover_popdown(GTK_POPOVER(darktable.gui->active_popover_menu));
  if(module) _flexi_op_apply(module, cid, flexi_op);
}

// build and show the flexi operator chooser (every dt_masks_flexi_ops
// entry) for a group. Shared by a direct click on the chooser button and the
// "change operator of selected group" shortcut
static void _build_flexi_op_menu(GtkWidget *anchor, dt_iop_module_t *module, const dt_mask_id_t cid)
{
  g_object_set_data(G_OBJECT(anchor), "module", module);
  g_object_set_data(G_OBJECT(anchor), "flexi_op_cid", GINT_TO_POINTER(cid));

  GActionGroup *action_group = gtk_widget_get_action_group(anchor, "masks_flexi_op");
  if(action_group == NULL)
  {
    GActionEntry action_entries[] =
    {
      { "set", _flexi_op_action, "i", "0" },
    };
    action_group = G_ACTION_GROUP(g_simple_action_group_new());
    g_action_map_add_action_entries(G_ACTION_MAP(action_group), action_entries,
                                    G_N_ELEMENTS(action_entries), anchor);
    gtk_widget_insert_action_group(anchor, "masks_flexi_op", action_group);
    g_object_unref(action_group);
  }

  // a stateful action makes the items radio items: the one whose target is
  // the group's current operator shows checked
  const dt_masks_point_group_t *head = dt_masks_gui_group_point(dt_masks_gui_module_mask_group(module), cid);
  GAction *set = g_action_map_lookup_action(G_ACTION_MAP(action_group), "set");
  g_simple_action_set_state(G_SIMPLE_ACTION(set),
                            g_variant_new_int32(head ? (head->state & DT_MASKS_STATE_FLEXI_OP) : 0));

  GMenu *menu = _flexi_op_menu_model("masks_flexi_op.set", -1);
  darktable.gui->active_popover_menu = dt_gui_popover_menu_from_model(anchor, menu);
  gtk_popover_popup(GTK_POPOVER(darktable.gui->active_popover_menu));
  g_object_unref(menu);
}

static void _group_flexi_op_pressed(GtkGestureSingle *gesture,
                                    const int n_press,
                                    const double x,
                                    const double y,
                                    GtkWidget *btn)
{
  // the icon is the operator chooser only: a right-click stops here, or it
  // would propagate to the header and open the group's actions menu
  const guint button = gtk_gesture_single_get_current_button(gesture);
  if(button != GDK_BUTTON_PRIMARY && button != GDK_BUTTON_SECONDARY) return;
  dt_gui_claim(gesture);
  dt_iop_module_t *module = g_object_get_data(G_OBJECT(btn), "module");
  if(button == GDK_BUTTON_PRIMARY && module) _build_flexi_op_menu(btn, module, _header_cid(btn));
}

// bypass or restore a whole group, on its marker. Bypass is a modifier, so the
// group's operator stays as it was
static void _group_toggle_bypass(dt_iop_module_t *module, dt_mask_id_t cid)
{
  dt_masks_point_group_t *marker = dt_masks_gui_group_point(dt_masks_gui_module_flexi_group(module, &cid), cid);
  if(!marker) return;
  marker->state ^= DT_MASKS_STATE_OP_BYPASS;
  _commit_structure_change(module);
}

// flip every member's own inversion bit independently (not a group-wide state
// to set/clear): ON, OFF, ON becomes OFF, ON, OFF. A one-shot action, not a
// persistent "group is inverted" mode -- shared by the actions menu's
// "invert all elements" entry and the "invert selected group" shortcut. Not
// the same operation as _group_toggle_output_invert below: inverting every
// member and folding is mathematically different from folding and then
// inverting the result, for anything but a single-member group (see
// DT_MASKS_STATE_OP_INVERT in masks.h).
static void _invert_group_members(dt_iop_module_t *module, const dt_mask_id_t cid)
{
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  GList *members = dt_masks_model_group_members(grp, cid);
  if(!members) return;
  for(GList *l = members; l; l = g_list_next(l))
  {
    dt_masks_point_group_t *pt = dt_masks_gui_group_point(grp, GPOINTER_TO_INT(l->data));
    if(!pt) continue;
    pt->state ^= DT_MASKS_STATE_INVERSE;
  }
  g_list_free(members);
  dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
  // inversion changes no row, only how rows look: refresh them in place, as
  // _invert_element does, rather than rebuilding with a visible flash
  _refresh_all_shape_rows(module);
}

// invert-output (DT_MASKS_STATE_OP_INVERT, "true" group invert): a persistent
// flag on the group's marker, unlike _invert_group_members' one-shot member
// flip. Individual members' own DT_MASKS_STATE_INVERSE bits are untouched --
// the two are independent.
static void _group_toggle_output_invert(dt_iop_module_t *module, const dt_mask_id_t cid)
{
  dt_masks_point_group_t *marker = dt_masks_gui_group_point(dt_masks_gui_module_mask_group(module), cid);
  if(!marker) return;
  marker->state ^= DT_MASKS_STATE_OP_INVERT;
  dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
  // changes no row, only the group's handle: updated in place
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(bd && bd->masks_list_box)
    _apply_group_output_invert_icon(GTK_WIDGET(bd->masks_list_box), (guint)cid,
                                    (marker->state & DT_MASKS_STATE_OP_INVERT) != 0);
}

// the group header's opacity slider's tooltip stands in for its own hidden
// label/value (see the header build below), so it must track live drag
// ticks, not just report the value the slider was built with.
static void _group_opacity_update_tooltip(GtkWidget *slider, const float value)
{
  gchar *tip = g_strdup_printf(_("opacity: %.0f%%\n"
                                 "applied on top of -- not instead of -- each "
                                 "element's own opacity, the two multiply together"),
                               value * 100.0f);
  gtk_widget_set_tooltip_text(slider, tip);
  g_free(tip);
}

// set the group's own persistent, multiplicative opacity (see
// dt_masks_point_group_t.group_opacity), on its marker -- an absolute value,
// unlike every other multi-target properties row (_props_row_apply's delta
// convention): a group header always represents exactly one group, so there is
// no multi-select ambiguity a delta needs to resolve.
static void _group_opacity_changed(GtkWidget *w, dt_iop_module_t *module)
{
  if(DT_IN_GUI_UPDATE()) return;
  dt_mask_id_t cid = _header_cid(w);
  dt_masks_point_group_t *marker = dt_masks_gui_group_point(dt_masks_gui_module_flexi_group(module, &cid), cid);
  if(!marker) return;
  const float value = dt_bauhaus_slider_get(w);
  marker->group_opacity = value;
  _group_opacity_update_tooltip(w, value);
  dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
  dt_control_queue_redraw_center();
  // an opacity change can push this group -- or, since it scales every
  // member's own effective opacity too, any of its elements -- across the
  // low-opacity threshold; refresh every badge in the panel, not just this
  // group's own (mirrors _props_row_apply's own call for the same reason).
  _refresh_lowop_badges(module);
}

// the group's opacity as a full labeled slider, leading its expanded contents
// or in the properties subpanel. Drives the persisted group_opacity through
// _group_opacity_changed
static GtkWidget *_build_group_opacity_editor(dt_iop_module_t *module, const dt_mask_id_t cid)
{
  const dt_masks_point_group_t *head_pt = dt_masks_gui_group_point(dt_masks_gui_module_mask_group(module), cid);
  const float go = head_pt ? head_pt->group_opacity : 1.0f;

  GtkWidget *ex_op = dt_bauhaus_slider_new_with_range(
    module, _blend_masks_properties[DT_MASKS_PROPERTY_OPACITY].min,
    _blend_masks_properties[DT_MASKS_PROPERTY_OPACITY].max, 0, 1.0f, 2);
  dt_bauhaus_widget_set_label(ex_op, N_("blend"), N_("opacity"));
  dt_bauhaus_slider_set_format(ex_op, "%");
  dt_bauhaus_slider_set_digits(ex_op, 2);
  dt_bauhaus_widget_set_quad_visibility(ex_op, FALSE);
  dt_gui_add_class(ex_op, "dt_masks_props_slider");
  DT_ENTER_GUI_UPDATE(); // populate only -- must not fire _group_opacity_changed
  dt_bauhaus_slider_set(ex_op, go);
  DT_LEAVE_GUI_UPDATE();
  _group_opacity_update_tooltip(ex_op, go);
  g_object_set_data(G_OBJECT(ex_op), "group-key", GUINT_TO_POINTER(cid));
  g_signal_connect(G_OBJECT(ex_op), "value-changed",
                   G_CALLBACK(_group_opacity_changed), module);
  // a bypassed group contributes nothing, see the header's own sensitivity
  if(head_pt && _op_is_bypassed(head_pt->state))
    gtk_widget_set_sensitive(ex_op, FALSE);

  GtkWidget *ex_op_box = dt_gui_vbox(ex_op);
  dt_gui_add_class(ex_op_box, "dt_masks_card");
  dt_gui_add_class(ex_op_box, "dt_masks_group_card");
  return ex_op_box;
}

// the group an actions-menu item acts on, and its module
static dt_iop_module_t *_group_act_target(gpointer u, dt_mask_id_t *cid)
{
  GtkWidget *anchor = GTK_WIDGET(u);
  *cid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(anchor), "group_act_cid"));
  if(darktable.gui->active_popover_menu)
    gtk_popover_popdown(GTK_POPOVER(darktable.gui->active_popover_menu));
  return g_object_get_data(G_OBJECT(anchor), "module");
}

static void _group_act_disable(GSimpleAction *action, GVariant *param, gpointer u)
{
  dt_mask_id_t cid;
  dt_iop_module_t *module = _group_act_target(u, &cid);
  if(module) _group_toggle_bypass(module, cid);
}

static void _group_act_solo(GSimpleAction *action, GVariant *param, gpointer u)
{
  dt_mask_id_t cid;
  dt_iop_module_t *module = _group_act_target(u, &cid);
  if(module) _toggle_solo_group(module, cid);
}

static void _group_act_invert_output(GSimpleAction *action, GVariant *param, gpointer u)
{
  dt_mask_id_t cid;
  dt_iop_module_t *module = _group_act_target(u, &cid);
  if(module) _group_toggle_output_invert(module, cid);
}

static void _group_act_invert_elems(GSimpleAction *action, GVariant *param, gpointer u)
{
  dt_mask_id_t cid;
  dt_iop_module_t *module = _group_act_target(u, &cid);
  if(module) _invert_group_members(module, cid);
}

static void _group_act_rename(GSimpleAction *action, GVariant *param, gpointer u)
{
  GtkWidget *lbl_box = g_object_get_data(G_OBJECT(u), "group_act_lbl_box");
  dt_mask_id_t cid;
  dt_iop_module_t *module = _group_act_target(u, &cid);
  if(module && lbl_box) _start_group_rename(lbl_box, module, cid);
}

static void _group_act_empty(GSimpleAction *action, GVariant *param, gpointer u)
{
  dt_mask_id_t cid;
  dt_iop_module_t *module = _group_act_target(u, &cid);
  if(module) _group_reset_members(module, cid);
}

static void _group_act_delete(GSimpleAction *action, GVariant *param, gpointer u)
{
  dt_mask_id_t cid;
  dt_iop_module_t *module = _group_act_target(u, &cid);
  if(module) _group_delete(module, cid);
}

// "compose": the element or group of `pt` goes into a new group folding with
// `flexi_op`, under a new empty group, which is selected so the next shape
// drawn lands in it
static void _compose(dt_iop_module_t *module,
                     const dt_masks_point_group_t *pt,
                     const dt_masks_state_t flexi_op)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  if(!grp || !pt) return;
  const gboolean whole = dt_masks_point_is_marker(pt) && pt->formid == _mask_group_cid(module);
  dt_masks_clear_form_gui(darktable.develop);
  const dt_mask_id_t eid = dt_masks_model_compose(grp, pt, flexi_op);
  if(!dt_is_valid_maskid(eid))
  {
    dt_control_log(_("this group is nested as deep as groups go"));
    return;
  }

  // the whole mask's refinement refined what is now the group at its bottom,
  // and goes with it: the whole mask starts over. A refinement that group
  // already holds, which only a migrated edit can give the mask's own group,
  // leaves it where it is
  gboolean had_details = FALSE;
  if(whole && _refine_global_is_set(module))
  {
    const dt_masks_point_group_t *ref = grp->points->next->data;
    dt_masks_form_t *sub = dt_masks_get_from_id(darktable.develop, ref->formid);
    dt_masks_point_group_t *mk = sub ? sub->points->data : NULL;
    if(mk && mk->refinement.enabled == DT_MASKS_REFINE_OFF)
    {
      mk->refinement = _refine_of_module(module);
      had_details = _refine_clear_global(module);
    }
  }

  bd->panel_selected_formid = INVALID_MASKID;
  bd->panel_selected_group_cid = eid;
  // the list rebuild keeps the refinement controls on their old scope
  _flexi_refine_follow_selection(bd);
  dt_print(DT_DEBUG_MASKS, "[masks] compose %d flexi_op=0x%x, empty group %d", pt->formid,
           flexi_op, eid);
  // with the module: a moved whole-mask refinement is in its blend params
  _commit_structure_change(module);
  if(had_details) // see _refine_clear_global
  {
    dt_dev_reprocess_all(module->dev);
    dt_control_queue_redraw();
  }
}

// the group form whose tree compose and simplify restructure for group `cid`:
// the mask's own, or a nested group's. NULL for an AI object, whose paths
// move as one
static dt_masks_form_t *_restructurable_group(dt_iop_module_t *module, const dt_mask_id_t cid)
{
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  dt_masks_form_t *g = cid == _mask_group_cid(module) ? grp : dt_masks_model_nested_group_of(grp, cid);
  return g && (g->type & DT_MASKS_GROUP) && !(g->type & DT_MASKS_OBJECT) ? g : NULL;
}

// "simplify": make the tree under group `cid` shallower where that renders
// the same mask (dt_masks_group_simplify). The whole mask also takes over a
// lone group it holds
static void _simplify(dt_iop_module_t *module, const dt_mask_id_t cid)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  if(!grp) return;
  const gboolean whole = cid == _mask_group_cid(module);
  dt_masks_form_t *target = _restructurable_group(module, cid);
  if(!target) return;

  dt_masks_clear_form_gui(darktable.develop);
  dt_masks_refinement_t r = _refine_of_module(module);
  const gboolean had_refine = r.enabled != DT_MASKS_REFINE_OFF;
  gboolean changed = FALSE;
  for(int pass = 0; pass <= DT_MASKS_NESTING_MAX; pass++)
  {
    gboolean again = dt_masks_group_simplify(darktable.develop->forms, target);
    if(whole) again |= dt_masks_model_hoist_sole_group(grp, &r);
    changed |= again;
    if(!again) break;
  }
  if(!changed)
  {
    dt_control_log(_("nothing to simplify"));
    return;
  }

  // a hoisted group's refinement is the whole mask's now
  if(!had_refine && r.enabled != DT_MASKS_REFINE_OFF) _refine_set_global(module, &r);

  bd->panel_selected_formid = INVALID_MASKID;
  bd->panel_selected_group_cid = cid;
  // a hoist moved a refinement into the whole mask's, which may be on screen
  _flexi_refine_follow_selection(bd);
  _commit_structure_change(module);
}

// a check action of an actions menu, owned by `map` alone
static void _add_check_action(GActionMap *map,
                              const char *name,
                              const gboolean active,
                              GCallback activate,
                              gpointer data)
{
  GSimpleAction *action = g_simple_action_new_stateful(name, NULL, g_variant_new_boolean(active));
  g_signal_connect_data(action, "activate", activate, data, NULL, 0);
  g_action_map_add_action(map, G_ACTION(action));
  g_object_unref(action);
}

// a menu entry with a tooltip (see _popover_menu_apply_tooltips in gui/gtk.c)
static void _menu_append_tip(GMenu *menu, const char *label, const char *action,
                             const char *tooltip)
{
  GMenuItem *it = g_menu_item_new(label, action);
  g_menu_item_set_attribute(it, "tooltip", "s", tooltip);
  g_menu_append_item(menu, it);
  g_object_unref(it);
}

// the "compose" entry opening `compose`, which it takes
static void _append_compose_submenu(GMenu *section, GMenu *compose)
{
  GMenuItem *it = g_menu_item_new_submenu(_("compose"), G_MENU_MODEL(compose));
  g_menu_item_set_attribute(it, "tooltip", "s",
                            _("use this as a building block for a larger mask, for example\n"
                              "to subtract from it or to intersect it with something else:\n"
                              "it goes into a new group of the chosen operator,\n"
                              "with an empty group on top to draw into"));
  g_menu_append_item(section, it);
  g_object_unref(it);
  g_object_unref(compose);
}

static void _group_act_compose(GSimpleAction *action, GVariant *param, gpointer u)
{
  dt_mask_id_t cid;
  dt_iop_module_t *module = _group_act_target(u, &cid);
  if(module)
    _compose(module, dt_masks_gui_group_point(dt_masks_gui_module_mask_group(module), cid),
             (dt_masks_state_t)g_variant_get_int32(param));
}

static void _group_act_simplify(GSimpleAction *action, GVariant *param, gpointer u)
{
  dt_mask_id_t cid;
  dt_iop_module_t *module = _group_act_target(u, &cid);
  if(module) _simplify(module, cid);
}

// the "compose" submenu: every operator but `current`, whose group composing
// would only add one more member to. `keep_current` offers it anyway, for a
// member past the base of a group folding in order: `a - (b - c)` is no
// `a - b - c`
static GMenu *_compose_menu(const char *action,
                            const dt_masks_state_t current,
                            const gboolean keep_current)
{
  return _flexi_op_menu_model(action, keep_current ? -1 : (int)(current & DT_MASKS_STATE_FLEXI_OP));
}

static void _build_group_actions_menu(GtkWidget *anchor,
                                      dt_iop_module_t *module,
                                      const dt_mask_id_t cid,
                                      GtkWidget *lbl_box)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  const dt_masks_point_group_t *marker = dt_masks_gui_group_point(grp, cid);
  const gboolean bypassed = marker && _op_is_bypassed(marker->state);
  const gboolean output_inverted = marker && (marker->state & DT_MASKS_STATE_OP_INVERT);
  // what acts on the members has nothing to act on in an empty group
  GList *members = dt_masks_model_group_members(grp, cid);
  const gboolean has_members = members != NULL;
  g_list_free(members);

  g_object_set_data(G_OBJECT(anchor), "module", module);
  g_object_set_data(G_OBJECT(anchor), "group_act_cid", GINT_TO_POINTER(cid));
  g_object_set_data(G_OBJECT(anchor), "group_act_lbl_box", lbl_box);

  GSimpleActionGroup *sag = g_simple_action_group_new();
  GActionMap *map = G_ACTION_MAP(sag);

  _add_check_action(map, "disable", bypassed, G_CALLBACK(_group_act_disable), anchor);
  _add_check_action(map, "solo", bd->solo_group_key == (guint)cid, G_CALLBACK(_group_act_solo),
                    anchor);
  _add_check_action(map, "invert_output", output_inverted,
                    G_CALLBACK(_group_act_invert_output), anchor);

  GActionEntry action_entries[] =
  {
    { "invert_elems", _group_act_invert_elems, NULL, NULL },
    { "compose",      _group_act_compose,      "i",  NULL },
    { "simplify",     _group_act_simplify,     NULL, NULL },
    { "rename",       _group_act_rename,       NULL, NULL },
    { "empty",        _group_act_empty,        NULL, NULL },
    { "delete",       _group_act_delete,       NULL, NULL },
  };
  g_action_map_add_action_entries(map, action_entries, G_N_ELEMENTS(action_entries), anchor);

  // the mask's own group is the mask, and stays (see _group_delete)
  const gboolean deletable =
    dt_masks_model_nested_group_of(dt_masks_gui_module_mask_group(module), cid) != NULL;

  gtk_widget_insert_action_group(anchor, "masks_group_act", G_ACTION_GROUP(sag));
  g_object_unref(sag);

  GMenu *menu = g_menu_new();

  // visibility
  GMenu *sec_vis = g_menu_new();
  _menu_append_tip(sec_vis, _("disable"), "masks_group_act.disable",
                   _("switch this group off: it adds nothing to the mask,\n"
                     "and keeps its elements and settings for when it is back on\n"
                     "clicking the eye on its header does the same"));
  if(!bypassed && has_members)
    _menu_append_tip(sec_vis, _("solo"), "masks_group_act.solo",
                     _("use only this group, to see what it contributes\n"
                       "shift+clicking the eye on its header does the same\n"
                       "choose this entry again to go back"));
  g_menu_append_section(menu, _("visibility"), G_MENU_MODEL(sec_vis));
  g_object_unref(sec_vis);

  if(!bypassed && has_members)
  {
    // mask operations
    GMenu *sec_ops = g_menu_new();
    _menu_append_tip(sec_ops, _("invert output"), "masks_group_act.invert_output",
                     _("invert what the group gives, once its elements are combined.\n"
                       "stays on until chosen again"));
    _menu_append_tip(sec_ops, _("invert all elements"), "masks_group_act.invert_elems",
                     _("flip the invert of each element on its own, before they\n"
                       "are combined: a different mask from inverting the output"));
    if(_restructurable_group(module, cid))
    {
      GMenu *compose = _compose_menu("masks_group_act.compose", marker->state, FALSE);
      _append_compose_submenu(sec_ops, compose);
      GMenuItem *simplify = g_menu_item_new(_("simplify"), "masks_group_act.simplify");
      g_menu_item_set_attribute(simplify, "tooltip", "s",
                                _("remove the groups inside this one that change nothing:\n"
                                  "empty groups, groups holding a single element,\n"
                                  "and groups using the operator of the group holding them"));
      g_menu_append_item(sec_ops, simplify);
      g_object_unref(simplify);
    }
    g_menu_append_section(menu, _("mask operations"), G_MENU_MODEL(sec_ops));
    g_object_unref(sec_ops);
  }

  // edit
  GMenu *sec_edit = g_menu_new();
  if(cid != _mask_group_cid(module))
    _menu_append_tip(sec_edit, _("rename"), "masks_group_act.rename",
                     _("give this group a name of its own\n"
                       "ctrl+click on the group header does the same"));
  // the mask is its own top group, so emptying that one empties the mask:
  // "empty group" would name a group the user never sees as one
  if(has_members)
  {
    if(cid == _mask_group_cid(module))
      _menu_append_tip(sec_edit, _("reset mask"), "masks_group_act.empty",
                       _("remove every element and group from the mask"));
    else
      _menu_append_tip(sec_edit, _("empty group"), "masks_group_act.empty",
                       _("remove this group's elements, keeping the group\n"
                         "with its operator, settings and name"));
  }
  if(deletable)
    _menu_append_tip(sec_edit, _("delete group"), "masks_group_act.delete",
                     _("remove this group and every element in it"));
  g_menu_append_section(menu, _("edit"), G_MENU_MODEL(sec_edit));
  g_object_unref(sec_edit);

  darktable.gui->active_popover_menu = dt_gui_popover_menu_from_model(anchor, menu);
  g_object_unref(menu);
}

// the state half of the solo-edit toggle, the third of the mutually
// exclusive states with dt_masks_model_toggle_solo_form and
// dt_masks_model_toggle_solo_group
dt_masks_solo_canvas_t dt_masks_model_toggle_soloedit(dt_iop_module_t *module,
                                                      dt_masks_form_t *grp,
                                                      const dt_mask_id_t id)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(bd->soloedit_formid == id)
  {
    bd->soloedit_formid = INVALID_MASKID;
    return DT_MASKS_SOLO_CANVAS_FULL;
  }

  bd->soloedit_formid = id;
  // solo and solo-edit are mutually exclusive (see the matching clear in
  // dt_masks_model_toggle_solo_form/dt_masks_model_toggle_solo_group) -- drop any active solo
  // and restore every element's visibility, since solo-edit only isolates
  // what is editable, not what is shown.
  if(dt_is_valid_maskid(bd->solo_formid) || bd->solo_group_key != 0)
  {
    dt_masks_group_isolate_state(grp, NULL, DT_MASKS_STATE_HIDDEN);
    bd->solo_formid = INVALID_MASKID;
    bd->solo_group_key = 0;
  }
  return DT_MASKS_SOLO_CANVAS_ONE;
}

// solo-edit a single element or group: only its outlines/handles are editable
// on the canvas, while the full mask still computes so every shape's effect is
// still visible in the mask overlay. Toggling off restores editing of the whole
// mask. Driven by the solo edit mode following the selection (see
// _soloedit_follow_selection)
static void _toggle_soloedit(dt_iop_module_t *module, const dt_mask_id_t id)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  const gboolean had_solo =
    dt_is_valid_maskid(bd->solo_formid) || bd->solo_group_key != 0;
  const dt_masks_solo_canvas_t canvas =
    dt_masks_model_toggle_soloedit(module, dt_masks_gui_module_mask_group(module), id);

  if(canvas == DT_MASKS_SOLO_CANVAS_ONE)
  {
    GList *ids = _soloedit_formids(module);
    dt_masks_set_edit_mode_forms(module, ids, DT_MASKS_EDIT_FULL);
    g_list_free(ids);
    if(had_solo) _sync_hidden_to_form_visible(module);
  }
  else
    dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
  // for the solo dt_masks_model_toggle_soloedit may have cleared, whose hidden
  // bits the rows paint from. Not deferred: _refresh_all_shape_rows destroys
  // no widget (as in _toggle_solo_group)
  _refresh_all_shape_rows(module);
}

// the header row of group block `block` (its "drop-title" holds it), or NULL
static GtkWidget *_block_header_line(GtkWidget *block)
{
  GtkWidget *title = g_object_get_data(G_OBJECT(block), "drop-title");
  return title && GTK_IS_BIN(title) ? gtk_bin_get_child(GTK_BIN(title)) : NULL;
}

// a drop target is the group's block and its header row: the block shades its
// body, the header lights up as a hovered one
static void _mark_drop_target(GtkWidget *block, const gboolean on)
{
  GtkWidget *hdr = _block_header_line(block);
  if(on)
  {
    dt_gui_add_class(block, "dt_masks_drop_target");
    if(hdr) dt_gui_add_class(hdr, "dt_masks_drop_target");
  }
  else
  {
    dt_gui_remove_class(block, "dt_masks_drop_target");
    if(hdr) dt_gui_remove_class(hdr, "dt_masks_drop_target");
  }
}

static void _clear_drop_classes(GtkWidget *f)
{
  _mark_drop_target(f, FALSE);
  dt_gui_remove_class(f, "dt_masks_drop_above");
  dt_gui_remove_class(f, "dt_masks_drop_below");
}

// the one drop indicator shown while a drag hovers the list: the insertion line
// where the element would land, drawn across the width it would have there,
// and the group it would land in, lit up. Either widget may be destroyed by a
// rebuild under it
static struct
{
  GtkWidget *line;
  const char *cls;
  GtkWidget *group;
} _drop_indicator = { NULL, NULL, NULL };

static void _drop_indicator_track(GtkWidget **slot, GtkWidget *w)
{
  if(*slot)
  {
    _clear_drop_classes(*slot);
    g_object_remove_weak_pointer(G_OBJECT(*slot), (gpointer *)slot);
  }
  *slot = w;
  if(w) g_object_add_weak_pointer(G_OBJECT(w), (gpointer *)slot);
}

// the widget an insertion line along an edge of drop frame `frame` is painted
// on. A group block paints its own edges (see .dt_masks_group_block). An element or
// a cluster paints nothing itself, and the parts it holds would cover a line
// drawn on it, so the line goes on the part showing at that edge: its header
// at the top, its bottom-most part at the bottom (an open editor's card, or a
// cluster's bottom member), as wide as the element
static GtkWidget *_drop_line_paint_widget(GtkWidget *frame, const gboolean above)
{
  GtkWidget *header = g_object_get_data(G_OBJECT(frame), "drop-header");
  if(!header || above) return header ? header : frame;
  GtkWidget *bottom = NULL;
  int bottom_y = -1;
  GList *kids = gtk_container_get_children(GTK_CONTAINER(frame));
  for(GList *k = kids; k; k = g_list_next(k))
  {
    GtkWidget *c = k->data;
    gint cx = 0, cy = 0;
    if(!gtk_widget_get_mapped(c) || gtk_widget_get_allocated_height(c) <= 0
       || !gtk_widget_translate_coordinates(c, frame, 0, 0, &cx, &cy) || cy <= bottom_y)
      continue;
    bottom = c;
    bottom_y = cy;
  }
  g_list_free(kids);
  if(!bottom || gtk_widget_is_ancestor(header, bottom)) return header;
  GtkWidget *part = GTK_IS_BIN(bottom) ? gtk_bin_get_child(GTK_BIN(bottom)) : bottom;
  // an open cluster: the line runs under its bottom member
  GtkWidget *member = GTK_IS_REVEALER(bottom) ? g_object_get_data(G_OBJECT(frame), "drop-row-bottom") : NULL;
  if(member) return _drop_line_paint_widget(member, FALSE);
  return part ? part : header;
}

static void _drop_indicator_set(GtkWidget *line, const char *cls, GtkWidget *group)
{
  if(_drop_indicator.line == line && !g_strcmp0(_drop_indicator.cls, cls)
     && _drop_indicator.group == group)
    return;
  _drop_indicator_track(&_drop_indicator.line, line);
  _drop_indicator_track(&_drop_indicator.group, group);
  _drop_indicator.cls = cls;
  if(group) _mark_drop_target(group, TRUE);
  if(line) dt_gui_add_class(line, cls);
}

// show the insertion line along the top (`above`) or bottom edge of drop frame
// `frame`, and light up `group`
static void _drop_indicator_show(GtkWidget *frame, const gboolean above, GtkWidget *group)
{
  GtkWidget *paint = frame ? _drop_line_paint_widget(frame, above) : NULL;
  const char *cls = paint == frame
                      ? (above ? "dt_masks_drop_above" : "dt_masks_drop_below")
                      : (above ? "dt_masks_drop_above" : "dt_masks_drop_below");
  _drop_indicator_set(paint, cls, group);
}

// the group block a drop lands in: the group itself for a drop inside it, else
// the one holding the list its frame is an element of (a cluster is no group,
// so one is passed through). NULL when the frame is in no group's list
static GtkWidget *_drop_group_of(const dt_masks_drop_t d)
{
  if(d.inside) return d.frame;
  for(GtkWidget *w = gtk_widget_get_parent(_drop_item_of(d.frame)); w; w = gtk_widget_get_parent(w))
    if(g_object_get_data(G_OBJECT(w), "drop-list")) return w;
  return NULL;
}

// a group block or a cluster, which a drag resting on it opens (see _masks_drag)
static gboolean _drag_springs(GtkWidget *w)
{
  return g_object_get_data(G_OBJECT(w), "group-expand-toggle")
         || g_object_get_data(G_OBJECT(w), "cluster-revealer");
}

static gboolean _drag_spring_folded(GtkWidget *w)
{
  GtkWidget *toggle = g_object_get_data(G_OBJECT(w), "group-expand-toggle");
  if(toggle)
    return gtk_widget_get_sensitive(toggle)
           && !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggle));
  GtkWidget *rev = g_object_get_data(G_OBJECT(w), "cluster-revealer");
  return rev && !gtk_revealer_get_reveal_child(GTK_REVEALER(rev));
}

// open or fold group block or cluster `w` as code enforcing it, not as the
// user's click: no selection, and "auto-expand selected" is not told
static void _drag_spring_set_open(dt_iop_gui_blend_data_t *bd, GtkWidget *w, const gboolean open)
{
  GtkWidget *toggle = g_object_get_data(G_OBJECT(w), "group-expand-toggle");
  if(toggle)
    _set_chevron(toggle, open, TRUE);
  else
    _cluster_set_revealed(bd, g_object_get_data(G_OBJECT(w), "cluster-revealer"), open);
}

static void _drag_hide_tagged(GtkWidget *w, gpointer data)
{
  if(!gtk_widget_get_visible(w)) return;
  g_object_set_data(G_OBJECT(w), "drag-hidden", GINT_TO_POINTER(1));
  gtk_widget_hide(w);
}

static void _drag_show_tagged(GtkWidget *w, gpointer data)
{
  if(!g_object_get_data(G_OBJECT(w), "drag-hidden")) return;
  g_object_set_data(G_OBJECT(w), "drag-hidden", NULL);
  gtk_widget_show(w);
}

// the drag has rested on its candidate long enough: open it, its members'
// headers alone
static gboolean _masks_drag_spring(gpointer user_data)
{
  dt_iop_module_t *module = user_data;
  _masks_drag.timer = 0;
  GtkWidget *w = _masks_drag.candidate;
  if(!_masks_drag.active || !w || !_drag_spring_folded(w)) return G_SOURCE_REMOVE;
  _foreach_tagged(w, "drag-hide", _drag_hide_tagged, NULL);
  _drag_spring_set_open(module->blend_data, w, TRUE);
  g_ptr_array_add(_masks_drag.opened, w);
  return G_SOURCE_REMOVE;
}

// the folded group or cluster under drop target `t` the drag rests on: the
// innermost group or cluster holding it, when that one is folded
static void _masks_drag_rest_on(dt_iop_module_t *module, GtkWidget *list, GtkWidget *t)
{
  GtkWidget *spring = NULL;
  for(GtkWidget *g = t; g && g != list; g = gtk_widget_get_parent(g))
    if(_drag_springs(g))
    {
      if(_drag_spring_folded(g)) spring = g;
      break;
    }
  if(spring == _masks_drag.candidate) return;
  _masks_drag_set_candidate(spring);
  if(spring) _masks_drag.timer = g_timeout_add(MASKS_SPRING_DELAY_MS, _masks_drag_spring, module);
}

// end the drag's own state: fold back what it opened, but for where the
// element landed: its group `landed` and every group holding that one, and a
// cluster it landed among, one holding its drop frame `frame` (both NULL for
// a drag that did not land). Then show again what it hid
static void _masks_drag_restore(dt_iop_module_t *module, GtkWidget *landed, GtkWidget *frame)
{
  if(!_masks_drag.active) return;
  _masks_drag.active = FALSE;
  _masks_drag_set_candidate(NULL);
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  for(guint i = 0; _masks_drag.opened && i < _masks_drag.opened->len; i++)
  {
    GtkWidget *w = g_ptr_array_index(_masks_drag.opened, i);
    const gboolean keep =
      (landed && (w == landed || gtk_widget_is_ancestor(landed, w)))
      || (frame && g_object_get_data(G_OBJECT(w), "cluster-revealer")
          && gtk_widget_is_ancestor(frame, w));
    if(!keep) _drag_spring_set_open(bd, w, FALSE);
  }
  if(_masks_drag.opened) g_ptr_array_set_size(_masks_drag.opened, 0);
  if(bd && bd->masks_list_box)
    _foreach_tagged(GTK_WIDGET(bd->masks_list_box), "drag-hide", _drag_show_tagged, NULL);
}

// a drag of the list's own has ended. One that landed was restored by
// _drop_received already; any other folds back. An element row's drag that
// ends without a drop where it began was a click the drag source took (see
// _row_drag_begin), and selects its row
static void _masks_drag_end(GtkWidget *w, GdkDragContext *dc, dt_iop_module_t *module)
{
  const gboolean was_click = _masks_drag.active && !_masks_drag.dropped
                             && dt_is_valid_maskid(_masks_drag.formid);
  _masks_drag_restore(module, NULL, NULL);
  if(!was_click || !module->blend_data) return;
  gdouble x, y;
  _pointer_root_position(&x, &y);
  gint threshold = 8;
  g_object_get(gtk_settings_get_default(), "gtk-dnd-drag-threshold", &threshold, NULL);
  if(fabs(x - _masks_drag.start_x) <= threshold && fabs(y - _masks_drag.start_y) <= threshold)
    _set_form_target_ext(module, _masks_drag.formid, FALSE);
}

// a drag that did not land. GTK on macOS emits only this then, never
// "drag-end" (gtkdnd-quartz.c destroys the drag, emitting it, on success alone),
// so a canceled drag is settled here, or what it opened and hid would stay so
static gboolean _masks_drag_failed(GtkWidget *w,
                                   GdkDragContext *dc,
                                   GtkDragResult result,
                                   dt_iop_module_t *module)
{
  _masks_drag_end(w, dc, module);
  return FALSE;
}

// the whole list is one drop target, deciding from the pointer's position.
// The widgets a drop can be placed against (see _drop_at) are only tagged
// "drop-target"; this finds the innermost one under the pointer, with the
// point in its coordinates in *ty. Do not make them GTK drop targets: GTK
// sends drag-motion to the next target before drag-leave to the last
// (gtk_drag_find_widget, gtkdnd.c), so a leave would wipe the new feedback,
// and the drop would go to whichever target GTK found first
static GtkWidget *_drop_target_at(GtkWidget *w, const int x, const int y, int *ty)
{
  GtkWidget *found = NULL;
  if(g_object_get_data(G_OBJECT(w), "drop-target"))
  {
    found = w;
    *ty = y;
  }
  if(!GTK_IS_CONTAINER(w)) return found;
  GList *kids = gtk_container_get_children(GTK_CONTAINER(w));
  for(GList *k = kids; k; k = g_list_next(k))
  {
    GtkWidget *c = k->data;
    gint cx = 0, cy = 0;
    if(!gtk_widget_get_mapped(c) || !gtk_widget_translate_coordinates(w, c, x, y, &cx, &cy)
       || cx < 0 || cy < 0 || cx >= gtk_widget_get_allocated_width(c)
       || cy >= gtk_widget_get_allocated_height(c))
      continue;
    // siblings do not overlap: the one holding the point is the only one
    GtkWidget *inner = _drop_target_at(c, cx, cy, ty);
    if(inner) found = inner;
    break;
  }
  g_list_free(kids);
  return found;
}

// where a drop at (x, y) on the list lands; no frame when it lands nowhere.
//
// The band along a group's bottom edge (see _drop_on_group) lands below the
// group even over the group's own last element, which otherwise covers it:
// without that, the last element of a group could not be moved out below it.
// The innermost group whose band holds the pointer is the one it lands below
static dt_masks_drop_t _drop_on_list(GtkWidget *list, const int x, const int y)
{
  int ty = 0;
  GtkWidget *t = _drop_target_at(list, x, y, &ty);
  if(!t) return (dt_masks_drop_t){ NULL, FALSE, FALSE };
  if(!dt_modifier_is(dt_key_modifier_state(), GDK_SHIFT_MASK))
    for(GtkWidget *g = t; g && g != list; g = gtk_widget_get_parent(g))
    {
      gint gx = 0, gy = 0, title_y = 0;
      const int edge = g_object_get_data(G_OBJECT(g), "drop-target")
                         ? _group_drop_edge(g, &title_y) : 0;
      if(edge && gtk_widget_translate_coordinates(list, g, x, y, &gx, &gy)
         && gy >= gtk_widget_get_allocated_height(g) - edge)
        return (dt_masks_drop_t){ g, FALSE, FALSE };
    }
  return _drop_at(t, ty);
}

// the payload of the list's own that a drag carries (see _mask_hdr_dnd), or
// GDK_NONE. Every one of them is same-app, so it is read from the drag's source
// widget: gtk_drag_dest_find_target gets there too, but on macOS it first turns
// every type on the system pasteboard into an atom, and logs a critical for
// each one GTK cannot name, on every motion event of every drag
static GdkAtom _drop_target(GtkWidget *dest, GdkDragContext *dc)
{
  GtkWidget *src = gtk_drag_get_source_widget(dc);
  GtkTargetList *offered = src ? gtk_drag_source_get_target_list(src) : NULL;
  GtkTargetList *taken = gtk_drag_dest_get_target_list(dest);
  if(!offered || !taken) return GDK_NONE;
  gint n = 0;
  GtkTargetEntry *entries = gtk_target_table_new_from_list(offered, &n);
  GdkAtom found = GDK_NONE;
  for(gint i = 0; i < n && found == GDK_NONE; i++)
  {
    const GdkAtom atom = gdk_atom_intern(entries[i].target, FALSE);
    if(gtk_target_list_find(taken, atom, NULL)) found = atom;
  }
  gtk_target_table_free(entries, n);
  return found;
}

static gboolean _drop_motion(
  GtkWidget *w, GdkDragContext *dc, gint x, gint y, guint time, gpointer user_data)
{
  const dt_masks_drop_t d = _drop_target(w, dc) == GDK_NONE
                              ? (dt_masks_drop_t){ NULL, FALSE, FALSE }
                              : _drop_on_list(w, x, y);
  gdk_drag_status(dc, d.frame ? GDK_ACTION_MOVE : 0, time);
  if(_masks_drag.active)
  {
    int ty = 0;
    _masks_drag_rest_on(user_data, w, d.frame ? _drop_target_at(w, x, y, &ty) : NULL);
  }
  if(!d.frame)
    _drop_indicator_set(NULL, NULL, NULL);
  else if(d.inside)
  {
    // it lands on top, so the line is on its top element's top edge, and an
    // empty or folded group is only lit up
    dt_masks_drop_list_t l;
    const gboolean any =
      _drop_list_items(g_object_get_data(G_OBJECT(d.frame), "drop-list"), d.frame, 0, &l);
    _drop_indicator_show(any ? l.first : NULL, TRUE, d.frame);
  }
  else
  {
    GtkWidget *line = NULL;
    gboolean above = FALSE;
    _drop_line(d, &line, &above);
    _drop_indicator_show(line, above, _drop_group_of(d));
  }
  return TRUE;
}

static void _drop_leave(GtkWidget *w, GdkDragContext *dc, guint time, gpointer user_data)
{
  _drop_indicator_set(NULL, NULL, NULL);
  _masks_drag_set_candidate(NULL);
}

// the drop itself: ask for the data, which _drop_received moves. Not
// GTK_DEST_DEFAULT_DROP, which finishes the drag a second time after the
// handler has, as a successful move that asks the source to delete its data
static gboolean _drop_drop(
  GtkWidget *w, GdkDragContext *dc, gint x, gint y, guint time, gpointer user_data)
{
  const GdkAtom target = _drop_target(w, dc);
  if(target == GDK_NONE || !_drop_on_list(w, x, y).frame)
    gtk_drag_finish(dc, FALSE, FALSE, time);
  else
    gtk_drag_get_data(w, dc, target, time);
  return TRUE;
}

// the data of a drop, requested by _drop_drop: where it lands is decided again
// from the drop's own position, as the motion handler did
static void _drop_received(GtkWidget *w,
                           GdkDragContext *ctx,
                           gint x,
                           gint y,
                           GtkSelectionData *sel,
                           guint info,
                           guint time,
                           dt_iop_module_t *module)
{
  const dt_masks_drop_t d = _drop_on_list(w, x, y);
  const gboolean ok = _drop_apply(module, ctx, sel, info, d);
  dt_print(DT_DEBUG_MASKS, "[masks dnd] info=%u dropped %s %p ok=%d", info,
           d.inside ? "inside" : d.above ? "above" : "below", (void *)d.frame, ok);
  // before the rebuild the drop queues, which keeps what the drag opened open
  _masks_drag.dropped = TRUE;
  _masks_drag_restore(module, ok ? _drop_group_of(d) : NULL, ok ? d.frame : NULL);
  _finish_drop(module, ctx, ok, time);
}

// tag `w` as something a drop can be placed against (see _drop_at)
static void _set_drop_target(GtkWidget *w)
{
  g_object_set_data(G_OBJECT(w), "drop-target", GINT_TO_POINTER(1));
}

// a group drag has begun: a drag is not a click, so suppress the button-release
// that selects the group. Some platforms (notably macOS) still deliver a release
// to the drag source when the drag ends, which would otherwise toggle the group's
// selection right after a reorder.
static void _group_drag_begin(GtkWidget *w, GdkDragContext *dc, dt_iop_module_t *module)
{
  dt_print(DT_DEBUG_MASKS, "[masks dnd] group drag-begin");
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(bd) bd->masks_skip_group_select_release = TRUE;
  _masks_drag_begin(module, INVALID_MASKID);
}

static void _cluster_drag_begin(GtkWidget *w, GdkDragContext *dc, dt_iop_module_t *module)
{
  _masks_drag_begin(module, INVALID_MASKID);
}

static GtkWidget *_make_drag_handle(DTGTKCairoPaintIconFunc kind_paint,
                                    gboolean enabled,
                                    const char *tooltip);

#ifdef HAVE_AI
// the "value-changed" of the pending row's two AI sliders (smoothing,
// cleanup): a delta from the widget's last value, as
// dt_masks_object_creation_apply_property takes. No list rebuild, which
// would interrupt the drag
static void _pending_ai_slider_changed(GtkWidget *widget, dt_iop_module_t *module)
{
  if(DT_IN_GUI_UPDATE()) return;
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!bd) return;
  const dt_masks_property_t prop =
    (dt_masks_property_t)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "dt-prop"));
  const float new_val = dt_bauhaus_slider_get(widget);
  float *last = (prop == DT_MASKS_PROPERTY_SMOOTHING) ? &bd->pending_ai_smoothing_last
                                                      : &bd->pending_ai_cleanup_last;
  const float old_val = *last;
  *last = new_val;
  dt_masks_object_creation_apply_property(prop, old_val, new_val);
}

static void _pending_ai_refine_toggled(GtkToggleButton *button, gpointer user_data)
{
  if(DT_IN_GUI_UPDATE()) return;
  const gboolean on = gtk_toggle_button_get_active(button);
  dt_masks_object_creation_apply_property(DT_MASKS_PROPERTY_REFINE, !on, on);
}
#endif

// the "value-changed" of a pending-row slider editing a creation default: the
// conf key a shape's scroll handler edits while drawing (e.g. circle.c's
// DT_MASKS_CONF(form->type, circle, size)). There is no form to modify yet,
// so the absolute value goes to the key, as a scroll does. The key on the
// widget is a string literal from DT_MASKS_CONF, not owned
static void _pending_conf_slider_changed(GtkWidget *widget, gpointer user_data)
{
  if(DT_IN_GUI_UPDATE()) return;
  const char *key = g_object_get_data(G_OBJECT(widget), "dt-conf-key");
  dt_conf_set_float(key, dt_bauhaus_slider_get(widget));
  dt_control_queue_redraw_center();
}

// the ellipse's size: radius_a ("dt-conf-key"), with radius_b
// ("dt-conf-key2") scaled by the same factor to keep the aspect ratio, as
// _ellipse_events_mouse_scrolled does
static void _pending_ellipse_size_changed(GtkWidget *widget, gpointer user_data)
{
  if(DT_IN_GUI_UPDATE()) return;
  const char *key_a = g_object_get_data(G_OBJECT(widget), "dt-conf-key");
  const char *key_b = g_object_get_data(G_OBJECT(widget), "dt-conf-key2");
  const float old_a = dt_conf_get_float(key_a);
  const float new_a = dt_bauhaus_slider_get(widget);
  if(old_a > 0.0f)
  {
    const float factor = new_a / old_a;
    dt_conf_set_float(key_b, dt_conf_get_float(key_b) * factor);
  }
  dt_conf_set_float(key_a, new_a);
  dt_control_queue_redraw_center();
}

// builds one such slider, seeded from the conf key's current value. `key2`,
// when given, scales with `key` (see _pending_ellipse_size_changed)
static GtkWidget *_pending_conf_slider_new(dt_iop_module_t *module,
                                           const char *key,
                                           const char *key2,
                                           const char *label,
                                           const float min,
                                           const float max,
                                           const int digits,
                                           const char *format,
                                           const char *tooltip)
{
  GtkWidget *w =
    dt_bauhaus_slider_new_with_range(module, min, max, 0, dt_conf_get_float(key), digits);
  dt_bauhaus_widget_set_label(w, N_("blend"), label);
  if(format) dt_bauhaus_slider_set_format(w, format);
  if(tooltip) gtk_widget_set_tooltip_text(w, tooltip);
  g_object_set_data(G_OBJECT(w), "dt-conf-key", (gpointer)key);
  if(key2)
  {
    g_object_set_data(G_OBJECT(w), "dt-conf-key2", (gpointer)key2);
    g_signal_connect(G_OBJECT(w), "value-changed", G_CALLBACK(_pending_ellipse_size_changed),
                     NULL);
  }
  else
    g_signal_connect(G_OBJECT(w), "value-changed", G_CALLBACK(_pending_conf_slider_changed),
                     NULL);
  dt_gui_add_class(w, "dt_masks_props_slider");
  dt_bauhaus_widget_set_quad_visibility(w, FALSE);
  return w;
}

// the same, for shape property `prop` as the properties editor shows it
static GtkWidget *_pending_prop_slider(dt_iop_module_t *module,
                                       const dt_masks_property_t prop,
                                       const char *key,
                                       const char *key2,
                                       const int digits,
                                       const char *tooltip)
{
  return _pending_conf_slider_new(module, key, key2, _blend_masks_properties[prop].name,
                                  _blend_masks_properties[prop].min,
                                  _blend_masks_properties[prop].max, digits,
                                  _blend_masks_properties[prop].format, tooltip);
}

// absorb clicks on the pending row, its handle included, so reaching for
// sliders does not bubble up to group_block and deselect or disarm the shape
static void _pending_shape_pressed(GtkGestureSingle *gesture,
                                   const int n_press,
                                   const double x,
                                   const double y,
                                   gpointer user_data)
{
  dt_gui_claim(gesture);
}

// a placeholder row for the shape being drawn (dev->form_gui->creation), in
// the group it will land in. There is no member point yet, so it has no
// rename, drag, delete or expander. Rebuilt with the list, never edited in
// place (see the pending state in dt_masks_gui_list_signature).
//
// Its sliders edit the creation defaults each shape's mouse-scroll handler
// edits while drawing (e.g. circle.c's DT_MASKS_CONF(type, circle, size)):
// absolute values the next shape starts with, not the deltas
// _build_props_row_editor applies to a committed shape. Opacity sets the
// sticky default (see _new_shape_default_opacity), and an AI object's
// smoothing and cleanup go to dt_masks_object_creation_apply_property, its
// outline being traced rather than seeded from a conf value
static GtkWidget *_make_pending_shape_row(dt_iop_module_t *module, dt_masks_form_t *form)
{
  const guint kind = _form_kind(form);

  GtkWidget *row = dt_gui_hbox();
  GtkWidget *handle = _make_drag_handle(
    _kind_icon_paint(kind), FALSE,
    _("this shape has not been added yet -- finish drawing it on canvas to add it"));

  gchar *text = g_strdup_printf(_("new %s"), _kind_name(kind, FALSE));
  GtkWidget *name = gtk_label_new(text);
  g_free(text);
  gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_max_width_chars(GTK_LABEL(name), 1);
  dt_gui_add_class(name, "dt_masks_row_name");
  gtk_widget_set_tooltip_text(
    name,
    _("this shape has not been added yet -- finish drawing it on canvas to add it"));

  // the same header as a committed row (see _pack_row_header), with an empty
  // drawer: no element yet to warn about, hide or expand. Its columns stay
  // blank so the name lines up
  _pack_row_header(row, handle, name, NULL,
                   NULL, FALSE, NULL, NULL);

  GtkWidget *row_vbox = dt_gui_vbox(row);
  dt_gui_add_class(row_vbox, "dt_masks_row");
  dt_gui_add_class(row_vbox, "dt_masks_pending");

  // the sliders go in a box styled as _build_props_row_editor's
  // (.dt_masks_props_card), so that they are inset as a committed row's
  GtkWidget *props_box = dt_gui_vbox();
  dt_gui_add_class(props_box, "dt_masks_card");
  dt_gui_add_class(props_box, "dt_masks_props_card");

  // opacity leads, as it does a committed row's expanded controls. It sets
  // the *sticky default* opacity (the value the shape gets on commit, see
  // _new_shape_default_opacity in masks/masks.c): there is no shape yet
  dt_gui_box_add(props_box,
                 _pending_prop_slider(module, DT_MASKS_PROPERTY_OPACITY,
                                      "plugins/darkroom/masks/opacity", NULL, 2,
                                      _("opacity of the next shape, before it is placed")));

  if(kind == DT_MASKS_PATH)
  {
    // a path has no size before it is committed, but each new node's feather
    // starts from this default (_path_events_button_pressed), which no
    // scroll adjusts while drawing
    dt_gui_box_add(props_box,
                   _pending_prop_slider(
                     module, DT_MASKS_PROPERTY_FEATHER, DT_MASKS_CONF(form->type, path, border),
                     NULL, 2,
                     _("fade-out border the next node placed on this path will start with")));
  }
  else if(kind == DT_MASKS_CIRCLE)
  {
    dt_gui_box_add(props_box,
                   _pending_prop_slider(module, DT_MASKS_PROPERTY_SIZE,
                                        DT_MASKS_CONF(form->type, circle, size), NULL, 2,
                                        _("radius of the next circle, before it is placed --\n"
                                          "same as scrolling on canvas")));
    dt_gui_box_add(props_box,
                   _pending_prop_slider(module, DT_MASKS_PROPERTY_FEATHER,
                                        DT_MASKS_CONF(form->type, circle, border), NULL, 2,
                                        _("fade-out border of the next circle, before it is"
                                          " placed --\nsame as shift+scrolling on canvas")));
  }
  else if(kind == DT_MASKS_ELLIPSE)
  {
    // size (radius_a) scales radius_b by the same factor, as the scroll
    // gesture does, to keep the aspect ratio (see _pending_ellipse_size_changed)
    dt_gui_box_add(props_box,
                   _pending_prop_slider(module, DT_MASKS_PROPERTY_SIZE,
                                        DT_MASKS_CONF(form->type, ellipse, radius_a),
                                        DT_MASKS_CONF(form->type, ellipse, radius_b), 2,
                                        _("size of the next ellipse, before it is placed --\n"
                                          "same as scrolling on canvas")));
    dt_gui_box_add(props_box,
                   _pending_prop_slider(module, DT_MASKS_PROPERTY_FEATHER,
                                        DT_MASKS_CONF(form->type, ellipse, border), NULL, 2,
                                        _("fade-out border of the next ellipse, before it is"
                                          " placed --\nsame as shift+scrolling on canvas")));
    dt_gui_box_add(props_box,
                   _pending_prop_slider(module, DT_MASKS_PROPERTY_ROTATION,
                                        DT_MASKS_CONF(form->type, ellipse, rotation), NULL, 1,
                                        _("rotation of the next ellipse, before it is placed"
                                          " --\nsame as ctrl+shift+scrolling on canvas")));
  }
  else if(kind == DT_MASKS_GRADIENT)
  {
    dt_gui_box_add(props_box,
                   _pending_conf_slider_new(
                     module, DT_MASKS_CONF(form->type, gradient, compression), NULL,
                     _blend_masks_properties[DT_MASKS_PROPERTY_COMPRESSION].name, 0.001f, 1.0f,
                     2, "%",
                     _("compression of the next gradient, before it is placed --\n"
                       "same as shift+scrolling on canvas")));
    dt_gui_box_add(props_box,
                   _pending_conf_slider_new(
                     module, DT_MASKS_CONF(form->type, gradient, curvature), NULL,
                     _blend_masks_properties[DT_MASKS_PROPERTY_CURVATURE].name, -2.0f, 2.0f,
                     2, NULL,
                     _("curvature of the next gradient, before it is placed --\n"
                       "same as scrolling on canvas")));
  }
  else if(kind == DT_MASKS_BRUSH)
  {
    dt_gui_box_add(props_box,
                   _pending_prop_slider(module, DT_MASKS_PROPERTY_SIZE,
                                        DT_MASKS_CONF(form->type, brush, border), NULL, 2,
                                        _("width of the next brush stroke -- same as scrolling"
                                          " on canvas")));
    dt_gui_box_add(props_box,
                   _pending_prop_slider(module, DT_MASKS_PROPERTY_HARDNESS,
                                        DT_MASKS_CONF(form->type, brush, hardness), NULL, 2,
                                        _("hardness of the next brush stroke -- same as"
                                          " shift+scrolling on canvas")));
  }

#ifdef HAVE_AI
  if(kind == DT_MASKS_OBJECT)
  {
    dt_iop_gui_blend_data_t *bd = module->blend_data;
    float smoothing = 0.0f;
    int cleanup = 0;
    dt_masks_object_creation_get_preview_params(&smoothing, &cleanup, NULL);

    bd->pending_ai_smoothing_last = smoothing;
    GtkWidget *sm = dt_bauhaus_slider_new_with_range(
      module, _blend_masks_properties[DT_MASKS_PROPERTY_SMOOTHING].min,
      _blend_masks_properties[DT_MASKS_PROPERTY_SMOOTHING].max, 0, smoothing, 2);
    dt_bauhaus_widget_set_label(
      sm, N_("blend"), _blend_masks_properties[DT_MASKS_PROPERTY_SMOOTHING].name);
    dt_bauhaus_slider_set_format(
      sm, _blend_masks_properties[DT_MASKS_PROPERTY_SMOOTHING].format);
    dt_bauhaus_slider_set_digits(sm, 2);
    gtk_widget_set_tooltip_text(
      sm, _("how closely the traced outline follows the AI selection's raw edge.\n"
            "lower: a tighter, more angular fit to the selection.\n"
            "higher: a looser fit with smoother, more rounded corners.\n"
            "same as scrolling on the canvas while drawing"));
    g_object_set_data(G_OBJECT(sm), "dt-prop",
                      GINT_TO_POINTER(DT_MASKS_PROPERTY_SMOOTHING));
    g_signal_connect(G_OBJECT(sm), "value-changed",
                     G_CALLBACK(_pending_ai_slider_changed), module);
    // the other pending sliders' look and full width (see _pending_conf_slider_new)
    dt_gui_add_class(sm, "dt_masks_props_slider");
    dt_bauhaus_widget_set_quad_visibility(sm, FALSE);
    dt_gui_box_add(props_box, sm);
    bd->pending_ai_smoothing_slider = sm;

    bd->pending_ai_cleanup_last = (float)cleanup;
    GtkWidget *cl = dt_bauhaus_slider_new_with_range(
      module, _blend_masks_properties[DT_MASKS_PROPERTY_CLEANUP].min,
      _blend_masks_properties[DT_MASKS_PROPERTY_CLEANUP].max, 0, (float)cleanup, 0);
    dt_bauhaus_widget_set_label(cl, N_("blend"),
                                _blend_masks_properties[DT_MASKS_PROPERTY_CLEANUP].name);
    dt_bauhaus_slider_set_format(
      cl, _blend_masks_properties[DT_MASKS_PROPERTY_CLEANUP].format);
    gtk_widget_set_tooltip_text(
      cl,
      _("discards small, stray outline fragments below this size (in traced pixels).\n"
        "higher: removes more small islands/holes, at the risk of dropping\n"
        "genuinely small parts of the selection.\n"
        "same as shift+scrolling on the canvas while drawing"));
    g_object_set_data(G_OBJECT(cl), "dt-prop",
                      GINT_TO_POINTER(DT_MASKS_PROPERTY_CLEANUP));
    g_signal_connect(G_OBJECT(cl), "value-changed",
                     G_CALLBACK(_pending_ai_slider_changed), module);
    dt_gui_add_class(cl, "dt_masks_props_slider");
    dt_bauhaus_widget_set_quad_visibility(cl, FALSE);
    dt_gui_box_add(props_box, cl);
    bd->pending_ai_cleanup_slider = cl;

    // snapping the selection's edge to the image's while an object is being
    // made. It applies from the next click on the object (see
    // _object_modify_property in object.c)
    gboolean refine = FALSE;
    dt_masks_object_creation_get_preview_params(NULL, NULL, &refine);
    GtkWidget *rf = gtk_check_button_new_with_label(
      _(_blend_masks_properties[DT_MASKS_PROPERTY_REFINE].name));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rf), refine);
    gtk_widget_set_tooltip_text(rf, _(_blend_masks_properties[DT_MASKS_PROPERTY_REFINE].tooltip));
    g_signal_connect(G_OBJECT(rf), "toggled", G_CALLBACK(_pending_ai_refine_toggled), NULL);
    dt_gui_box_add(props_box, rf);
  }
#endif

  // brush-only: pen pressure and stroke smoothing. Global preferences, not
  // shape parameters, that only act while a stroke is captured (brush.c), so
  // they are offered here and never on a committed shape. They are given the
  // module as action: without one, dt_gui_preferences_enum builds the
  // label-less widget of the preferences dialog, and the label set below is
  // never drawn
  if(kind == DT_MASKS_BRUSH)
  {
    if(darktable.gui->have_pen_pressure)
    {
      GtkWidget *pressure =
        dt_gui_preferences_enum(DT_ACTION(module), "pressure_sensitivity");
      dt_bauhaus_widget_set_label(pressure, N_("blend"), N_("pressure"));
      dt_gui_box_add(props_box, pressure);
    }

    GtkWidget *smoothing = dt_gui_preferences_enum(DT_ACTION(module), "brush_smoothing");
    dt_bauhaus_widget_set_label(smoothing, N_("blend"), N_("smoothing"));
    dt_gui_box_add(props_box, smoothing);
  }

  // with "element properties in subpanel" the row keeps only its header, in the
  // group the shape lands in, and the controls go to the subpanel, which the
  // rebuild building this row fills right after (see _props_panel_sync)
  if(_props_subpanel())
  {
    gtk_widget_show_all(props_box);
    ((dt_iop_gui_blend_data_t *)module->blend_data)->pending_props_box = props_box;
  }
  else
  {
    g_object_set_data(G_OBJECT(props_box), "drag-hide", GINT_TO_POINTER(1));
    dt_gui_box_add(row_vbox, props_box);
  }

  GtkWidget *pending_evbox = gtk_event_box_new();
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(pending_evbox), TRUE);
  dt_gui_connect_click(pending_evbox, _pending_shape_pressed, NULL, NULL);
  gtk_container_add(GTK_CONTAINER(pending_evbox), row_vbox);

  gtk_widget_show_all(pending_evbox);
  return pending_evbox;
}

// the event box wrapping a group header, carrying its click wiring, its drag
// source and the tags a ctrl+click rename and the solo dimming look the header
// up by. It is no drop target: the list is the only one (see _drop_target_at).
//
// `source_targets`/`drag_get` NULL means "not a drag source": the mask's own
// group has nowhere to move to.
static GtkWidget *_make_group_header_evbox(dt_iop_module_t *module,
                                           GtkWidget *hdr,
                                           GtkWidget *lbl_box,
                                           const GtkTargetEntry *source_targets,
                                           GCallback drag_get)
{
  GtkWidget *evbox = gtk_event_box_new();
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(evbox), TRUE);
  gtk_container_add(GTK_CONTAINER(evbox), hdr);

  // ctrl+click rename finds the title by this tag (see _group_header_press)
  g_object_set_data(G_OBJECT(evbox), "title-label-box", lbl_box);
  // solo dimming must reach the header row itself, never an enclosing block --
  // the member rows already dim individually, so dimming a block would
  // double-dim them (see _apply_group_header_dimming)
  g_object_set_data(G_OBJECT(evbox), "group-header-widget", hdr);

  GtkGestureSingle *gesture = dt_gui_connect_click(evbox, _group_header_press, NULL, module);
  g_signal_connect(gesture, "released", G_CALLBACK(_group_header_release), module);

  if(source_targets && drag_get)
  {
    // also a drag source for its own reorder, in addition to the grip handle in
    // column 0 -- grabbing anywhere on the row moves the group
    gtk_drag_source_set(evbox, GDK_BUTTON1_MASK, source_targets, 1, GDK_ACTION_MOVE);
    g_signal_connect_data(G_OBJECT(evbox), "drag-data-get", drag_get, NULL, NULL, 0);
    g_signal_connect(G_OBJECT(evbox), "drag-begin", G_CALLBACK(_group_drag_begin),
                     module);
    g_signal_connect(G_OBJECT(evbox), "drag-end", G_CALLBACK(_masks_drag_end), module);
    g_signal_connect(G_OBJECT(evbox), "drag-failed", G_CALLBACK(_masks_drag_failed), module);
  }
  return evbox;
}

// invert a single element's mask polarity: the row's actions menu "invert" and
// the "invert selected element" shortcut
static void _invert_element(dt_iop_module_t *module, const dt_mask_id_t id)
{
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  dt_masks_point_group_t *pt = grp ? dt_masks_gui_group_point(grp, id) : NULL;
  if(!pt) return;
  pt->state ^= DT_MASKS_STATE_INVERSE;
  dt_print(DT_DEBUG_MASKS, "[masks] form %d inverse=%d", id,
           !!(pt->state & DT_MASKS_STATE_INVERSE));
  dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
  // update this row's own state in place -- a full rebuild here would tear
  // down and recreate the whole list, which visibly flashes the panel for
  // what is just one bit.
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  GtkWidget *row_vbox = _masks_row_widget(bd, id);
  // this also flips a parametric row's own slider markers, which carry the
  // polarity a shape row shows on its handle icon (see _update_shape_row_state)
  _update_shape_row_state(bd, row_vbox, pt);
}

// the element a shape actions-menu item acts on, and its module. Every shape
// action pops the menu down through here, stateful (check) entries included,
// so every entry closes it the same way and _shape_popover_closed (the
// deferred auto-expand, connected in _row_click_press) runs for all
static dt_iop_module_t *_shape_act_target(gpointer u, dt_mask_id_t *id)
{
  GtkWidget *anchor = GTK_WIDGET(u);
  *id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(anchor), "shape_act_id"));
  if(darktable.gui->active_popover_menu)
    gtk_popover_popdown(GTK_POPOVER(darktable.gui->active_popover_menu));
  return g_object_get_data(G_OBJECT(anchor), "module");
}

static void _shape_act_disable(GSimpleAction *action, GVariant *param, gpointer u)
{
  dt_mask_id_t id;
  dt_iop_module_t *module = _shape_act_target(u, &id);
  if(module) _toggle_element_disable(module, id);
}

static void _shape_act_solo(GSimpleAction *action, GVariant *param, gpointer u)
{
  dt_mask_id_t id;
  dt_iop_module_t *module = _shape_act_target(u, &id);
  if(module) _toggle_solo_form(module, id);
}

static void _shape_act_invert(GSimpleAction *action, GVariant *param, gpointer u)
{
  dt_mask_id_t id;
  dt_iop_module_t *module = _shape_act_target(u, &id);
  if(module) _invert_element(module, id);
}

static void _shape_act_rename(GSimpleAction *action, GVariant *param, gpointer u)
{
  GtkWidget *evbox = g_object_get_data(G_OBJECT(u), "shape_act_evbox");
  dt_mask_id_t id;
  dt_iop_module_t *module = _shape_act_target(u, &id);
  if(evbox && module) _start_rename_element(evbox, module, id);
}

// the panel's way into an AI object's paths and back out (see _step_object)
static void _shape_act_edit_paths(GSimpleAction *action, GVariant *param, gpointer u)
{
  dt_mask_id_t id;
  dt_iop_module_t *module = _shape_act_target(u, &id);
  if(module) _toggle_object_paths(module, id, _entered_object() == id);
}

// give this module its own copy of a linked shape or AI object: the other
// modules keep the original
static void _unlink_element(dt_iop_module_t *module,
                            const dt_mask_id_t id,
                            dt_masks_point_group_t *pt)
{
  dt_masks_clear_form_gui(darktable.develop);
  const dt_mask_id_t nid = dt_masks_model_unlink_form_point(module, id, pt);
  if(!dt_is_valid_maskid(nid)) return;
  dt_print(DT_DEBUG_MASKS, "[masks] form %d unlinked in '%s' as %d", id, module->op, nid);
  _commit_structure_change(module);
  _queue_link_peers_rebuild(module);
}

static void _shape_act_unlink(GSimpleAction *action, GVariant *param, gpointer u)
{
  // the row the menu was opened on, so a shape held twice unlinks the one the
  // user clicked (see _build_shape_actions_menu)
  dt_masks_point_group_t *pt = g_object_get_data(G_OBJECT(u), "shape_act_point");
  dt_mask_id_t id;
  dt_iop_module_t *module = _shape_act_target(u, &id);
  if(module) _unlink_element(module, id, pt);
}

// a raster element's mask is edited where it is made: its source module gets
// the focus, expanded, with its mask on the canvas
static void _edit_raster_source(const dt_masks_form_t *form)
{
  dt_iop_module_t *src = dt_masks_raster_source(form);
  if(!src) return;
  _go_to_module(src);
  dt_iop_gui_blend_data_t *sbd = src->blend_data;
  if(sbd && dt_masks_gui_module_mask_group(src))
  {
    sbd->masks_shown = DT_MASKS_EDIT_FULL;
    dt_masks_set_edit_mode(src, DT_MASKS_EDIT_FULL);
  }
}

static void _raster_source_link_clicked(GtkButton *button, gpointer user_data)
{
  const dt_mask_id_t id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "formid"));
  _edit_raster_source(dt_masks_get_from_id(darktable.develop, id));
}

// a raster element's way to its source, in the action slot left of the
// opacity. NULL while the source is missing
static GtkWidget *_make_raster_source_link(const dt_masks_form_t *form)
{
  const dt_iop_module_t *src = dt_masks_raster_source(form);
  if(!src) return NULL;
  gchar *src_name = _module_plain_name(src);
  gchar *tip = g_strdup_printf(_("focus %s and edit its mask on the canvas"), src_name);
  GtkWidget *link = _make_link_button(tip, G_CALLBACK(_raster_source_link_clicked), NULL);
  g_free(tip);
  g_free(src_name);
  g_object_set_data(G_OBJECT(link), "formid", GINT_TO_POINTER(form->formid));
  return _link_action_slot(link);
}

// does `m` read `src`'s raster mask? Either as its whole mask in raster mode or
// through a raster element of its group, and only downstream of `src` with
// both it and its mask on, since the pipe feeds nothing else
static gboolean _reads_raster_of(const dt_iop_module_t *m, const dt_iop_module_t *src)
{
  const dt_develop_blend_params_t *bp = m->blend_params;
  if(m == src || !m->enabled || !bp || !(bp->mask_mode & DEVELOP_MASK_ENABLED)
     || m->iop_order <= src->iop_order)
    return FALSE;
  if(bp->mask_mode & DEVELOP_MASK_RASTER)
    return dt_iop_module_is(src, bp->raster_mask_source)
           && src->multi_priority == bp->raster_mask_instance;
  const dt_masks_form_t *grp = dt_masks_get_from_id(m->dev, bp->mask_id);
  return dt_masks_group_find_raster_of(m->dev->forms, grp, src, NO_MASKID, TRUE) != NULL;
}

// the dual of a raster element's source link (see _make_raster_source_link):
// a consumer's mask is edited in its own panel
static void _consumer_clicked(GtkButton *button, dt_iop_module_t *m)
{
  // the row may outlive its module until the next sync
  if(!darktable.develop || !g_list_find(darktable.develop->iop, m)) return;
  _go_to_module(m);
  if(dt_dev_gui_module() == m) dt_iop_gui_blend_masks_panel_show();
}

// list the modules reading this module's raster mask, and show the section
// only while there is one
static void _consumers_sync(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module ? module->blend_data : NULL;
  if(!bd || !bd->consumers_content || !module->dev) return;

  GList *consumers = NULL;
  dt_hash_t sig = DT_INITHASH;
  for(GList *l = module->dev->iop; l; l = g_list_next(l))
  {
    dt_iop_module_t *m = l->data;
    if(!_reads_raster_of(m, module)) continue;
    consumers = g_list_prepend(consumers, m);
    gchar *name = dt_history_item_get_name(m);
    sig = dt_hash(sig, &m, sizeof(m));
    sig = dt_hash(sig, name, strlen(name));
    g_free(name);
  }
  consumers = g_list_reverse(consumers);

  if(sig != bd->consumers_sig)
  {
    bd->consumers_sig = sig;
    dt_gui_container_destroy_children(GTK_CONTAINER(bd->consumers_content));
    for(const GList *l = consumers; l; l = g_list_next(l))
    {
      dt_iop_module_t *m = l->data;
      // the name is markup, escaped by dt_history_get_name_label; the tooltip
      // is plain text and takes the plain name
      gchar *name = dt_history_item_get_name(m);
      GtkWidget *label = gtk_label_new(NULL);
      gtk_label_set_markup(GTK_LABEL(label), name);
      g_free(name);
      gchar *plain = _module_plain_name(m);
      gchar *tip = g_strdup_printf(_("focus %s and show its mask panel"), plain);
      g_free(plain);
      gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
      gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
      // one button for the whole row, so it highlights as one wherever the
      // pointer is. Its chain is the link button's glyph, not a button of its
      // own, and brightens with the row (see .dt_masks_link_glyph)
      GtkWidget *chain = dtgtk_icon_new(dtgtk_cairo_paint_link, 0, NULL);
      gtk_widget_set_size_request(chain, DT_PIXEL_APPLY_DPI(11), DT_PIXEL_APPLY_DPI(11));
      gtk_widget_set_valign(chain, GTK_ALIGN_CENTER);
      dt_gui_add_class(chain, "dt_masks_link_glyph");
      GtkWidget *row = gtk_button_new();
      gtk_container_add(GTK_CONTAINER(row), dt_gui_hbox(dt_gui_expand(label), chain));
      dt_gui_add_class(row, "dt_masks_consumer_row");
      gtk_widget_set_tooltip_text(row, tip);
      g_free(tip);
      g_signal_connect(row, "clicked", G_CALLBACK(_consumer_clicked), m);
      dt_gui_box_add(bd->consumers_content, row);
    }
    gtk_widget_show_all(bd->consumers_content);
    _section_apply(bd, DT_MASKS_SECTION_CONSUMERS);
  }
  _box_set_visible(bd->consumers_box, consumers != NULL);
  g_list_free(consumers);
}

static void _consumers_history_changed(gpointer instance, dt_iop_module_t *module)
{
  _consumers_sync(module);
}

void dt_iop_gui_blend_module_renamed(dt_iop_module_t *module)
{
  // raster elements following this module's name show it in other panels
  if(module && darktable.develop) _queue_link_peers_rebuild(module);
}

static void _shape_act_delete(GSimpleAction *action, GVariant *param, gpointer u)
{
  // the row the menu was opened on, so a shape held twice loses the reference
  // the user clicked, as with unlink
  dt_masks_point_group_t *pt = g_object_get_data(G_OBJECT(u), "shape_act_point");
  dt_mask_id_t id;
  dt_iop_module_t *module = _shape_act_target(u, &id);
  if(module) _delete_single_shape(module, id, pt);
}

// the member point a shape actions menu acts on: the row's own, where it has one
static const dt_masks_point_group_t *_shape_act_point(GtkWidget *anchor,
                                                      dt_iop_module_t *module,
                                                      const dt_mask_id_t id)
{
  const dt_masks_point_group_t *pt = g_object_get_data(G_OBJECT(anchor), "shape_act_point");
  return pt ? pt : dt_masks_gui_group_point(dt_masks_gui_module_mask_group(module), id);
}

static void _shape_act_compose(GSimpleAction *action, GVariant *param, gpointer u)
{
  dt_mask_id_t id;
  dt_iop_module_t *module = _shape_act_target(u, &id);
  if(module)
    _compose(module, _shape_act_point(GTK_WIDGET(u), module, id),
             (dt_masks_state_t)g_variant_get_int32(param));
}

// the "compose" submenu of member `pt`, or NULL where it cannot be composed:
// a path of an AI object moves with the object
static GMenu *_shape_compose_menu(dt_masks_form_t *grp, const dt_masks_point_group_t *pt)
{
  dt_masks_form_t *owner = NULL;
  GList *node = pt ? _point_node_at(grp, pt, &owner, 0) : NULL;
  if(!node || !(owner->type & DT_MASKS_GROUP) || (owner->type & DT_MASKS_OBJECT)) return NULL;
  const int flexi_op = ((dt_masks_point_group_t *)owner->points->data)->state & DT_MASKS_STATE_FLEXI_OP;
  // composing the base with its group's operator adds a member, as it does
  // anywhere in a group whose members fold in any order
  const gboolean ordered =
    flexi_op & (DT_MASKS_STATE_FLEXI_DIFFERENCE | DT_MASKS_STATE_FLEXI_EXCLUSION);
  return _compose_menu("masks_shape_act.compose", flexi_op, ordered && owner->points->next != node);
}

static void _build_shape_actions_menu(GtkWidget *anchor,
                                      dt_iop_module_t *module,
                                      const dt_mask_id_t id,
                                      GtkWidget *handle,
                                      GtkWidget *evbox)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  // the exact reference this menu was opened on: one mask can hold the same
  // shape twice, and then the form id alone names neither row. The row's box
  // carries the member point it was built from (see _make_shape_row); a row
  // without one takes the first match
  dt_masks_point_group_t *row_pt = g_object_get_data(G_OBJECT(anchor), "row-point");
  const dt_masks_point_group_t *pt = row_pt ? row_pt
                                     : grp ? dt_masks_gui_group_point(grp, id) : NULL;

  const gboolean elem_disabled = pt && (pt->state & DT_MASKS_STATE_DISABLE);
  const gboolean elem_inverted = pt && (pt->state & DT_MASKS_STATE_INVERSE);

  g_object_set_data(G_OBJECT(anchor), "module", module);
  g_object_set_data(G_OBJECT(anchor), "shape_act_id", GINT_TO_POINTER(id));
  g_object_set_data(G_OBJECT(anchor), "shape_act_point", row_pt);
  g_object_set_data(G_OBJECT(anchor), "shape_act_evbox", evbox);

  GSimpleActionGroup *sag = g_simple_action_group_new();
  GActionMap *map = G_ACTION_MAP(sag);

  _add_check_action(map, "disable", elem_disabled, G_CALLBACK(_shape_act_disable), anchor);
  _add_check_action(map, "solo", bd->solo_formid == id, G_CALLBACK(_shape_act_solo), anchor);
  _add_check_action(map, "invert", elem_inverted, G_CALLBACK(_shape_act_invert), anchor);

  GActionEntry action_entries[] =
  {
    { "rename",      _shape_act_rename,      NULL, NULL },
    { "compose",     _shape_act_compose,     "i",  NULL },
    { "unlink",      _shape_act_unlink,      NULL, NULL },
    { "edit_paths",  _shape_act_edit_paths,  NULL, NULL },
    { "delete",      _shape_act_delete,      NULL, NULL },
  };
  g_action_map_add_action_entries(map, action_entries, G_N_ELEMENTS(action_entries), anchor);

  gtk_widget_insert_action_group(anchor, "masks_shape_act", G_ACTION_GROUP(sag));
  g_object_unref(sag);

  const dt_masks_form_t *elem = dt_masks_get_from_id(darktable.develop, id);
  // shared with another module, or held twice by this mask: either way this
  // row shows the same shape as some other row (see _model_form_uses_in_mask).
  // Only a shape can be either; a parametric channel is always copied
  const gboolean linked =
    dt_masks_model_form_is_linked(elem)
    || (_form_is_shape(elem) && _model_form_uses_in_mask(module, id) > 1);

  GMenu *menu = g_menu_new();

  GMenu *sec_vis = g_menu_new();
  _menu_append_tip(sec_vis, _("disable"), "masks_shape_act.disable",
                   _("switch this element off: it adds nothing to the mask,\n"
                     "and keeps its settings for when it is back on\n"
                     "clicking the eye on its row does the same"));
  if(!elem_disabled)
    _menu_append_tip(sec_vis, _("solo"), "masks_shape_act.solo",
                     _("use only this element, to see what it contributes\n"
                       "shift+clicking the eye on its row does the same\n"
                       "choose this entry again to go back"));
  g_menu_append_section(menu, _("visibility"), G_MENU_MODEL(sec_vis));
  g_object_unref(sec_vis);

  if(!elem_disabled)
  {
    GMenu *sec_ops = g_menu_new();
    _menu_append_tip(sec_ops, _("invert"), "masks_shape_act.invert",
                     _("invert this element: it selects what it left out"));
    GMenu *compose = _shape_compose_menu(grp, pt);
    if(compose) _append_compose_submenu(sec_ops, compose);
    g_menu_append_section(menu, _("mask operations"), G_MENU_MODEL(sec_ops));
    g_object_unref(sec_ops);
  }

  GMenu *sec_edit = g_menu_new();
  _menu_append_tip(sec_edit, _("rename"), "masks_shape_act.rename",
                   _("give this element a name of its own\n"
                     "ctrl+click on the row does the same"));
  if(linked)
  {
    GMenuItem *it = g_menu_item_new(_("unlink"), "masks_shape_act.unlink");
    g_menu_item_set_attribute(it, "tooltip", "s",
      _("give this row its own copy, so editing it no longer changes the other"
        " rows and modules it is linked with"));
    g_menu_append_item(sec_edit, it);
    g_object_unref(it);
  }
  // a single-path object already acts as that one path
  if(elem && (elem->type & DT_MASKS_OBJECT) && _object_path_count(elem) >= 2)
  {
    const gboolean inside = _entered_object() == id;
    GMenuItem *it = g_menu_item_new(inside ?_("stop editing individual paths")
                                           : _("edit individual paths"),
                                    "masks_shape_act.edit_paths");
    g_menu_item_set_attribute(it, "tooltip", "s",
      _("edit and remove the AI object's paths one by one on the canvas,"
        " like double-clicking it there or its row here"));
    g_menu_append_item(sec_edit, it);
    g_object_unref(it);
  }
  _menu_append_tip(sec_edit, _("delete"), "masks_shape_act.delete",
                   linked ? _("remove this element here; the other rows and modules"
                              " it is linked with keep it")
                          : _("remove this element from the mask"));
  g_menu_append_section(menu, _("edit"), G_MENU_MODEL(sec_edit));
  g_object_unref(sec_edit);

  darktable.gui->active_popover_menu = dt_gui_popover_menu_from_model(anchor, menu);
  g_object_unref(menu);
}

// human-readable name for a shape kind (the _form_kind bit), singular or plural.
// Used to label the same-kind element clusters ("3 circles").
static const char *_kind_name(const guint kind, const gboolean plural)
{
  switch(kind)
  {
  case DT_MASKS_CIRCLE: return plural ? _("circles") : _("circle");
  case DT_MASKS_ELLIPSE: return plural ? _("ellipses") : _("ellipse");
  case DT_MASKS_PATH: return plural ? _("paths") : _("path");
  case DT_MASKS_GRADIENT: return plural ? _("gradients") : _("gradient");
  case DT_MASKS_BRUSH: return plural ? _("brushes") : _("brush");
  case DT_MASKS_PARAMETRIC: return plural ? _("parametric masks") : _("parametric mask");
  case DT_MASKS_RASTER: return plural ? _("raster masks") : _("raster mask");
#ifdef HAVE_AI
  case DT_MASKS_OBJECT: return plural ? _("AI objects") : _("AI object");
#endif
  default: return plural ? _("shapes") : _("shape");
  }
}

// same glyph the add-toolbar button for this kind uses, so a row's icon
// matches the icon the user picked it from
static DTGTKCairoPaintIconFunc _kind_icon_paint(const guint kind)
{
  switch(kind)
  {
  case DT_MASKS_CIRCLE: return dtgtk_cairo_paint_masks_circle;
  case DT_MASKS_ELLIPSE: return dtgtk_cairo_paint_masks_ellipse;
  case DT_MASKS_PATH: return dtgtk_cairo_paint_masks_path;
  case DT_MASKS_GRADIENT: return dtgtk_cairo_paint_masks_gradient;
  case DT_MASKS_BRUSH: return dtgtk_cairo_paint_masks_brush;
  case DT_MASKS_PARAMETRIC: return dtgtk_cairo_paint_masks_parametric;
  case DT_MASKS_RASTER: return dtgtk_cairo_paint_masks_raster;
#ifdef HAVE_AI
  case DT_MASKS_OBJECT: return dtgtk_cairo_paint_masks_object;
#endif
  default: return NULL;
  }
}

static void _pack_group_elements(dt_iop_module_t *module,
                                 dt_masks_form_t *grp,
                                 GtkWidget *container,
                                 GList *fids);
static void _pack_subgroup(dt_iop_module_t *module, dt_masks_form_t *sub, GtkWidget *box);
static gboolean _nested_as_group(const dt_masks_point_group_t *pt, const dt_masks_form_t *form);

// --- drag handle ----------------------------------------------------------
// the grip that reorders a row: a windowed event box that is the drag source
// itself, so that the press lands on its window and the drag always arms. A
// whole row as drag source would not: its children's windows take the press.
//
// a row of one kind (a shape, a same-kind cluster) draws its kind icon in the
// handle, which is then both the grip and the kind; other rows (a group
// mixing kinds, an empty group) keep the grip dots

static gboolean _drag_handle_draw(GtkWidget *w, cairo_t *cr, gpointer user_data)
{
  const gboolean disabled =
    GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "handle-disabled"));
  GtkAllocation a;
  gtk_widget_get_allocation(w, &a);
  GdkRGBA c;
  GtkStyleContext *ctx = gtk_widget_get_style_context(w);
  const GtkStateFlags state = gtk_widget_get_state_flags(w);

  // this widget is app-paintable (see _make_drag_handle), so its normal CSS
  // background is never drawn automatically -- paint it explicitly, so
  // .dt_masks_lead.dt_masks_inverted (darktable.css) can swap this handle to a
  // light background / dark foreground, reading as a true color inversion
  // rather than a color tint over the icon.
  gtk_render_background(ctx, cr, 0, 0, a.width, a.height);
  gtk_style_context_get_color(ctx, state, &c);

  DTGTKCairoPaintIconFunc paint = (DTGTKCairoPaintIconFunc)user_data;
  if(paint)
  {
    // a meaningful type icon needs to actually read, unlike the subtle grip dots
    cairo_set_source_rgba(cr, c.red, c.green, c.blue, c.alpha * (disabled ? 0.35 : 0.85));
    // the glyph is inset by the CSS padding (.dt_masks_lead), which an event
    // box does not add to its size: the plate stays put, only the glyph shrinks
    GtkBorder pad;
    gtk_style_context_get_padding(ctx, state, &pad);
    paint(cr, pad.left, pad.top, a.width - pad.left - pad.right,
          a.height - pad.top - pad.bottom, 0, NULL);
    return FALSE;
  }

  // a disabled handle is drawn faint so it reads as "present but inactive"
  cairo_set_source_rgba(cr, c.red, c.green, c.blue, c.alpha * (disabled ? 0.16 : 0.5));
  const double r = MAX(1.0, DT_PIXEL_APPLY_DPI(1.1));
  const double dx = DT_PIXEL_APPLY_DPI(2.3);
  const double dy = DT_PIXEL_APPLY_DPI(3.2);
  const double cx = a.width * 0.5;
  const double cy = a.height * 0.5;
  for(int ix = -1; ix <= 1; ix += 2)
    for(int iy = -1; iy <= 1; iy++)
    {
      cairo_arc(cr, cx + ix * dx, cy + iy * dy, r, 0, 2.0 * M_PI);
      cairo_fill(cr);
    }
  return FALSE;
}

// build a drag-handle column. A glyph is always drawn (so columns line up and the
// affordance is visible on every reorderable row type): `kind_paint` when the row's
// kind maps to one icon, otherwise the generic grip dots (see _drag_handle_draw).
// When `enabled` is false it is drawn faint/disabled and is not a drag source.
// `tooltip` explains how to drag (enabled) or why the row cannot be moved/is not
// draggable (disabled). The caller wires the drag source + payload separately.
static GtkWidget *_make_drag_handle(DTGTKCairoPaintIconFunc kind_paint,
                                    gboolean enabled,
                                    const char *tooltip)
{
  GtkWidget *eb = gtk_event_box_new();
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(eb), TRUE);
  gtk_widget_set_app_paintable(eb, TRUE);
  gtk_widget_set_size_request(eb, DT_PIXEL_APPLY_DPI(18), DT_PIXEL_APPLY_DPI(18));
  gtk_widget_set_valign(eb, GTK_ALIGN_CENTER);
  // a rounded plate behind every handle, always -- not just when inverted --
  // so a blocky icon (e.g. the raster mask checkerboard) reads as a rounded
  // chip like the rest of the panel instead of a bare rectangle (see
  // .dt_masks_lead in darktable.css; _drag_handle_draw paints this
  // background itself since the handle is app-paintable)
  dt_gui_add_class(eb, "dt_masks_lead");
  if(!enabled) g_object_set_data(G_OBJECT(eb), "handle-disabled", GINT_TO_POINTER(1));
  if(tooltip) gtk_widget_set_tooltip_text(eb, tooltip);
  g_signal_connect(G_OBJECT(eb), "draw", G_CALLBACK(_drag_handle_draw), (gpointer)kind_paint);
  return eb;
}

// a parametric row's lead handle: its channel code ("hz", "Cz"), which says
// more than a generic parametric glyph. A plain label, so .dt_masks_lead and
// .dt_masks_lead.dt_masks_inverted style it without a draw handler
static GtkWidget *_make_channel_handle(const char *code, const char *tooltip)
{
  GtkWidget *eb = gtk_event_box_new();
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(eb), TRUE);
  // same square footprint as _make_drag_handle's icon plate, regardless of
  // how many characters the channel code has -- a one-off "hz" or "Cz" chip
  // must not read as a wider/differently-shaped column than every other
  // row's icon handle
  gtk_widget_set_size_request(eb, DT_PIXEL_APPLY_DPI(18), DT_PIXEL_APPLY_DPI(18));
  gtk_widget_set_valign(eb, GTK_ALIGN_CENTER);
  dt_gui_add_class(eb, "dt_masks_lead");
  dt_gui_add_class(eb, "dt_masks_channel");
  GtkWidget *lbl = gtk_label_new(code);
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.5f);
  gtk_label_set_justify(GTK_LABEL(lbl), GTK_JUSTIFY_CENTER);
  gtk_widget_set_halign(lbl, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
  gtk_container_add(GTK_CONTAINER(eb), lbl);
  if(tooltip) gtk_widget_set_tooltip_text(eb, tooltip);
  return eb;
}

// ---- per-row parametric mask editor ---------------------------------------
// each parametric row has its own input and output sliders, boost factor and
// picker, bound to its form's dt_masks_point_parametric_t (see
// _build_param_row_editor)

// is this row's own element inverted (DT_MASKS_STATE_INVERSE on its group
// point)? The displayed slider polarity flips to match
static gboolean _param_row_inverted(dt_iop_module_t *module, const dt_mask_id_t formid)
{
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  const dt_masks_point_group_t *gp = grp ? dt_masks_gui_group_point(grp, formid) : NULL;
  return gp && (gp->state & DT_MASKS_STATE_INVERSE);
}

gboolean dt_masks_gui_param_channel_is_used(const dt_masks_point_parametric_t *p,
                                              const dt_iop_gui_blendif_channel_t *channel,
                                              const int in_out)
{
  if(!p || !channel) return FALSE;
  const int ch = channel->param_channels[in_out];
  const float *const r = &p->blendif_parameters[4 * ch];
  const gboolean is_default_range =
    (r[0] == 0.0f && r[1] == 0.0f && r[2] == 1.0f && r[3] == 1.0f);
  const gboolean bit_active = (p->blendif & (1u << ch)) != 0;
  return !is_default_range || bit_active;
}

// which of a parametric row's controls are shown, from the channel's state.
// A collapsed row adapts to which sub-ranges the user has actually touched, so
// an untouched channel does not show a slider that says nothing; an expanded
// row always shows both. Split from the widget update below so the rule can be
// tested without a row -- see test_flexi_panel.c.
//
// A parametric row's opacity follows the rule every other element row's
// does: a full slider leading the expanded controls, so only once the row is
// expanded. `props_subpanel` is "element properties in subpanel", which takes
// the boost factor and that opacity slider out of the row and into its own
// section.
dt_masks_param_vis_t dt_masks_model_param_row_visibility(const gboolean expanded,
                                                         const gboolean in_used,
                                                         const gboolean out_used,
                                                         const gboolean boost_enabled,
                                                         const gboolean props_subpanel)
{
  dt_masks_param_vis_t v = { TRUE, FALSE, FALSE, FALSE, FALSE };

  v.opacity = expanded && !props_subpanel;

  if(expanded)
  {
    v.input = TRUE;
    v.output = TRUE;
    v.boost = boost_enabled && !props_subpanel;
  }
  else if(in_used && out_used)
  {
    v.input = TRUE;
    v.output = TRUE;
  }
  else if(!in_used && out_used)
  {
    v.input = FALSE;
    v.output = TRUE;
  }
  else
  {
    // only input used, or neither used (no-op default state)
    v.input = TRUE;
    v.output = FALSE;
  }

  // the per-sub-range bypass toggles only mean something when both are in play
  v.bypass = in_used && out_used;
  return v;
}

static void _param_slider_fit_eye(dt_masks_param_row_editor_t *ed);

static void _update_param_row_visibility(dt_masks_param_row_editor_t *ed)
{
  const dt_masks_point_parametric_t *p = _param_point(ed->formid);
  if(!p) return;
  const dt_iop_gui_blendif_channel_t *channels = _param_channels(p);
  const dt_iop_gui_blendif_channel_t *channel = channels ? &channels[p->channel] : NULL;

  const gboolean in_used = dt_masks_gui_param_channel_is_used(p, channel, 0);
  const gboolean out_used = dt_masks_gui_param_channel_is_used(p, channel, 1);
  const dt_masks_param_vis_t vis =
    dt_masks_model_param_row_visibility(p->in_out != 0, in_used, out_used,
                                        channel && channel->boost_factor_enabled,
                                        _props_subpanel());

  // the eye and the box it sits in hide on different conditions: the box
  // follows its slider row, while the eye follows vis.bypass. The box keeps
  // its width either way, which is what the slider's margin is fitted to (see
  // _param_slider_fit_eye).
  const gboolean show_io[2] = { vis.input, vis.output };
  GtkWidget *const lbl[2] = { ed->input_lbl, ed->output_lbl };
  GtkWidget *const slot[2] = { ed->input_slot, ed->output_slot };
  GtkWidget *const eye_slot[2] = { ed->input_bypass_slot, ed->output_bypass_slot };
  GtkWidget *const eye[2] = { ed->input_bypass_btn, ed->output_bypass_btn };
  for(int i = 0; i < 2; i++)
  {
    if(lbl[i]) gtk_widget_set_visible(lbl[i], show_io[i]);
    if(slot[i]) gtk_widget_set_visible(slot[i], show_io[i]);
    if(eye_slot[i]) gtk_widget_set_visible(eye_slot[i], show_io[i]);
    if(eye[i])
    {
      gtk_widget_set_visible(eye[i], vis.bypass);
      gtk_widget_set_sensitive(eye[i], vis.bypass);
    }
  }

  if(ed->sliders_grid)
  {
    gtk_widget_set_visible(ed->sliders_grid, TRUE);
    gtk_widget_queue_resize(ed->sliders_grid);
  }
  if(ed->boost_box)
  {
    gtk_widget_set_visible(ed->boost_box, vis.boost);
    gtk_widget_queue_resize(ed->boost_box);
  }
  if(ed->opacity_box)
  {
    gtk_widget_set_visible(ed->opacity_box, vis.opacity);
    gtk_widget_queue_resize(ed->opacity_box);
  }

  // the eye boxes may only just have been shown
  _param_slider_fit_eye(ed);
}

// the slider's tooltip: its range values, which the row shows nowhere else, and
// its display scale when that is not the linear one
static void _param_row_slider_set_tooltip(const dt_iop_gui_blendif_filter_t *sl,
                                          const int in_out,
                                          char range_text[4][256])
{
  const char *which = in_out ? _("output") : _("input");
  gchar *head = sl->altmode_name ? g_strdup_printf("%s (%s)", which, sl->altmode_name)
                                 : g_strdup(which);
  gchar *full_tip =
    g_strdup_printf("%s: %s  %s  %s  %s\n\n%s", head,
                    range_text[0], range_text[1], range_text[2], range_text[3],
                    _(slider_tooltip[in_out]));
  gtk_widget_set_tooltip_text(GTK_WIDGET(sl->slider), full_tip);
  g_free(full_tip);
  g_free(head);
}

// refresh this row's own slider markers/values/boost-slider display from its
// form's current values
static void _update_param_row_display(dt_masks_param_row_editor_t *ed)
{
  const dt_masks_point_parametric_t *p = _param_point(ed->formid);
  if(!p) return;
  const dt_iop_gui_blendif_channel_t *channels = _param_channels(p);
  if(!channels) return;
  const dt_iop_gui_blendif_channel_t *channel = &channels[p->channel];
  const gboolean single_inv = _param_row_inverted(ed->module, ed->formid);

  DT_ENTER_GUI_UPDATE();
  for(int in_out = 1; in_out >= 0; in_out--)
  {
    const dt_develop_blendif_channels_t ch = channel->param_channels[in_out];
    dt_iop_gui_blendif_filter_t *sl = &ed->filter[in_out];
    const float *parameters = &p->blendif_parameters[4 * ch];
    const float *defaults =
      &ed->module->default_blendop_params->blendif_parameters[4 * ch];

    // the element's invert (single_inv, which the handle icon shows too, see
    // _invert_element) is the row's polarity. p->blendif's polarity bit stays
    // at its non-inverted default for a single-channel form (see
    // _add_parametric_channel)
    // the outer markers are the open ones, the inner the filled ones
    const int open = single_inv ? GRADIENT_SLIDER_MARKER_UPPER_OPEN_BIG
                                : GRADIENT_SLIDER_MARKER_LOWER_OPEN_BIG;
    const int filled = single_inv ? GRADIENT_SLIDER_MARKER_LOWER_FILLED_BIG
                                  : GRADIENT_SLIDER_MARKER_UPPER_FILLED_BIG;
    for(int k = 0; k < 4; k++)
      dtgtk_gradient_slider_multivalue_set_marker(sl->slider,
                                                  (k == 0 || k == 3) ? open : filled, k);

    for(int k = 0; k < 4; k++)
    {
      dtgtk_gradient_slider_multivalue_set_value(sl->slider, parameters[k], k);
      dtgtk_gradient_slider_multivalue_set_resetvalue(sl->slider, defaults[k], k);
    }

    const float boost_factor =
      _boost_factor(p->blendif_boost_factors, channels, p->channel, in_out);
    char range_text[4][256];
    for(int k = 0; k < 4; k++)
      channel->scale_print(parameters[k], boost_factor, range_text[k],
                           sizeof(range_text[k]));
    _param_row_slider_set_tooltip(sl, in_out, range_text);

    dtgtk_gradient_slider_multivalue_clear_stops(sl->slider);
    for(int k = 0; k < channel->numberstops; k++)
      dtgtk_gradient_slider_multivalue_set_stop(
        sl->slider, channel->colorstops[k].stoppoint, channel->colorstops[k].color);
    dtgtk_gradient_slider_multivalue_set_increment(sl->slider, channel->increment);
  }

  const gboolean boost_enabled = channel->boost_factor_enabled;
  if(boost_enabled)
    dt_bauhaus_slider_set(ed->boost_slider,
                          p->blendif_boost_factors[channel->param_channels[0]]
                            - channel->boost_factor_offset);

  if(ed->input_bypass_btn)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ed->input_bypass_btn),
                                 (p->disabled & 1) != 0);
  if(ed->output_bypass_btn)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ed->output_bypass_btn),
                                 (p->disabled & 2) != 0);

  DT_LEAVE_GUI_UPDATE();

  _update_param_row_visibility(ed);
}

// commit a blendif edit straight to this row's own form (no module->blend_params
// scratch involved)
static void _param_form_commit(dt_iop_module_t *module, const dt_mask_id_t formid)
{
  dt_print(DT_DEBUG_MASKS, "[masks] parametric form %d: blendif edit committed", formid);
  dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
}

static void _param_channel_bypass_toggled(GtkToggleButton *btn, gpointer user_data)
{
  DT_GUARD_GUI_UPDATE();
  dt_masks_param_row_editor_t *ed = (dt_masks_param_row_editor_t *)user_data;
  if(!ed) return;
  dt_masks_point_parametric_t *p = _param_point(ed->formid);
  if(!p) return;

  const int in_out = (btn == GTK_TOGGLE_BUTTON(ed->output_bypass_btn)) ? 1 : 0;
  const gboolean bypassed = gtk_toggle_button_get_active(btn);

  if(bypassed)
    p->disabled |= (1u << in_out);
  else
    p->disabled &= ~(1u << in_out);

  _param_form_commit(ed->module, ed->formid);
  _update_param_row_display(ed);
  _refresh_lowop_badges(ed->module);

  if(ed->module && ed->module->dev)
  {
    dt_dev_reprocess_all(ed->module->dev);
    dt_control_queue_redraw();
  }
}

// what a range slider of the parametric row `ed` edits: the row's point,
// returned, the channels of its kind, which of its two sliders this is (0 for
// the input, 1 for the output) and the blendif channel it sets. NULL when the
// row's element or its channels are gone
static dt_masks_point_parametric_t *_param_slider_target(const dt_masks_param_row_editor_t *ed,
                                                         const GtkDarktableGradientSlider *slider,
                                                         const dt_iop_gui_blendif_channel_t **channels,
                                                         int *in_out,
                                                         dt_develop_blendif_channels_t *ch)
{
  dt_masks_point_parametric_t *p = _param_point(ed->formid);
  if(!p) return NULL;
  *channels = _param_channels(p);
  if(!*channels) return NULL;
  *in_out = (slider == ed->filter[1].slider) ? 1 : 0;
  *ch = (*channels)[p->channel].param_channels[*in_out];
  return p;
}

static void _param_row_slider_callback(GtkDarktableGradientSlider *slider,
                                       dt_masks_param_row_editor_t *ed)
{
  DT_GUARD_GUI_UPDATE();
  const dt_iop_gui_blendif_channel_t *channels;
  int in_out;
  dt_develop_blendif_channels_t ch;
  dt_masks_point_parametric_t *p = _param_slider_target(ed, slider, &channels, &in_out, &ch);
  if(!p) return;

  // a manual drag on this row's own slider means the user is done with
  // whatever range this row's picker last set -- turn the picker off so it
  // doesn't keep overwriting the values being dragged on the next pick.
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ed->colorpicker))
     || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ed->colorpicker_set_values)))
    dt_iop_color_picker_reset(ed->module, FALSE);

  float *parameters = &p->blendif_parameters[4 * ch];
  for(int k = 0; k < 4; k++)
    parameters[k] = dtgtk_gradient_slider_multivalue_get_value(slider, k);

  const float boost_factor =
    _boost_factor(p->blendif_boost_factors, channels, p->channel, in_out);
  char range_text[4][256];
  for(int k = 0; k < 4; k++)
    channels[p->channel].scale_print(parameters[k], boost_factor, range_text[k],
                                     sizeof(range_text[k]));

  // keep the range tooltip in sync with a live drag too, not just the initial
  // build
  _param_row_slider_set_tooltip(&ed->filter[in_out], in_out, range_text);

  if(parameters[1] == 0.0f && parameters[2] == 1.0f)
    p->blendif &= ~(1 << ch);
  else
    p->blendif |= (1 << ch);

  _param_form_commit(ed->module, ed->formid);
  _update_param_row_visibility(ed);
  // a drag can take the range to or from the full span, which does nothing:
  // refresh the badges in place, as an opacity drag does
  _refresh_lowop_badges(ed->module);
}

static void _param_row_slider_reset_callback(GtkDarktableGradientSlider *slider,
                                             dt_masks_param_row_editor_t *ed)
{
  DT_GUARD_GUI_UPDATE();
  const dt_iop_gui_blendif_channel_t *channels;
  int in_out;
  dt_develop_blendif_channels_t ch;
  dt_masks_point_parametric_t *p = _param_slider_target(ed, slider, &channels, &in_out, &ch);
  if(!p) return;

  // reset always clears polarity back to "not inverted" for this channel; the
  // element's own invert (the row's actions menu) is what actually flips it
  p->blendif &= ~(1 << (16 + ch));

  _param_form_commit(ed->module, ed->formid);
  _update_param_row_display(ed);
  // a reset routinely lands this element's range back at the no-op full
  // span -- see the matching comment on _param_row_slider_callback above
  _refresh_lowop_badges(ed->module);
  // the row's pickers are deferred (DT_COLOR_PICKER_DEFERRED_AREA) and resume
  // from the box they last sampled; after a reset the next pick waits for a
  // new box instead
  dt_iop_color_picker_forget(ed->colorpicker_set_values);
  dt_iop_color_picker_forget(ed->colorpicker);
}

// a range slider (see _build_param_row_filter) has no equivalent of a bauhaus
// slider's right-click "type an exact value" popup. The functions below add
// one for the parametric rows' sliders alone (see
// _param_row_slider_precise_pressed), rather than changing
// GtkDarktableGradientSlider, which is shared well beyond masks.
//
// A node's position is in the [0,1] domain channel->scale_print formats.
// scale_print has no parser, but its three implementations
// (_blendif_scale_print_default/_ab/_hue, matched by function pointer) are
// exactly invertible formulas, so a typed value round-trips exactly
float dt_masks_gui_param_row_slider_precise_display(const dt_iop_gui_blendif_channel_t *channel,
                                                    const float boost_factor,
                                                    const float frac)
{
  if(channel->scale_print == _blendif_scale_print_hue) return frac * 360.0f;
  if(channel->scale_print == _blendif_scale_print_ab)
    return (frac * 256.0f - 128.0f) * boost_factor;
  return frac * boost_factor * 100.0f; // _blendif_scale_print_default
}

float dt_masks_gui_param_row_slider_precise_parse(const dt_iop_gui_blendif_channel_t *channel,
                                                  const float boost_factor,
                                                  const float typed)
{
  if(channel->scale_print == _blendif_scale_print_hue) return typed / 360.0f;
  if(channel->scale_print == _blendif_scale_print_ab)
    return (typed / boost_factor + 128.0f) / 256.0f;
  return (typed / 100.0f) / boost_factor; // _blendif_scale_print_default
}

// turn one of the popup's own bauhaus-slider values into the [0,1] channel
// fraction of node *k_out, the node the popup edits, for both a real commit
// (_param_row_slider_precise_value_changed) and a hover preview
// (_param_row_slider_precise_hover_preview). FALSE (nothing to do) if the
// row's own form/channel data went away mid-interaction
static gboolean _param_row_slider_precise_context(GtkWidget *slider,
                                                  const float bauhaus_value,
                                                  gint *k_out,
                                                  float *newfrac_out)
{
  const gint k = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(slider), "precise-marker"));
  if(k < 0) return FALSE;
  const dt_iop_gui_blendif_channel_t *channels;
  int ch;
  dt_masks_param_row_editor_t *ed = _param_row_editor_resolve(slider, &channels, &ch);
  if(!ed) return FALSE;
  const dt_masks_point_parametric_t *p = _param_point(ed->formid);
  const int in_out = (slider == GTK_WIDGET(ed->filter[1].slider)) ? 1 : 0;
  const float boost_factor = _boost_factor(p->blendif_boost_factors, channels, ch, in_out);

  *k_out = k;
  *newfrac_out =
    dt_masks_gui_param_row_slider_precise_parse(&channels[ch], boost_factor, bauhaus_value);
  return TRUE;
}

// restore `slider`'s markers to the popup's baseline (see
// _param_row_slider_precise_open), then move node `k`: from the baseline, not
// from the last tick, so that a pushed neighbor eases back when the gesture
// reverses (gradientslider.c's _slider_move keeps no baseline)
static void _param_row_slider_precise_restore_baseline(GtkWidget *slider)
{
  gdouble *baseline = g_object_get_data(G_OBJECT(slider), "precise-baseline");
  if(!baseline) return;
  DT_ENTER_GUI_UPDATE();
  dtgtk_gradient_slider_multivalue_set_values(DTGTK_GRADIENT_SLIDER(slider), baseline);
  DT_LEAVE_GUI_UPDATE();
}

// drop the hover preview's pending settle (see
// _param_row_slider_precise_hover_preview); whether one was pending
static gboolean _param_row_slider_precise_cancel_settle(GtkWidget *slider)
{
  const guint pending =
    GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(slider), "precise-hover-settle"));
  if(!pending) return FALSE;
  g_source_remove(pending);
  g_object_set_data(G_OBJECT(slider), "precise-hover-settle", NULL);
  return TRUE;
}

// move the popover's node on each commit of the embedded bauhaus slider: a
// drag, a scroll, a typed value, or a hover preview settling
// (_param_row_slider_precise_hover_settled), not only on close
static void _param_row_slider_precise_value_changed(GtkWidget *bauhaus_slider,
                                                    GtkWidget *slider)
{
  if(DT_IN_GUI_UPDATE()) return;

  // this is a real commit: whatever the hover-preview debounce still had
  // pending is moot now, drop it rather than let it fire a redundant commit
  // a moment later
  _param_row_slider_precise_cancel_settle(slider);

  gint k;
  float newfrac;
  if(!_param_row_slider_precise_context(slider, dt_bauhaus_slider_get(bauhaus_slider), &k,
                                        &newfrac))
    return;

  _param_row_slider_precise_restore_baseline(slider);

  // emits "value-changed", so _param_row_slider_callback commits as for a
  // drag. "_pushing", as a drag does: crossing a neighbor pushes it along
  // (the embedded slider has the channel's full range for this, see
  // _param_row_slider_precise_open)
  dtgtk_gradient_slider_multivalue_set_value_pushing(DTGTK_GRADIENT_SLIDER(slider),
                                                     newfrac, k);
}

// how long the pointer rests on the popup's slider before a hovered position
// is committed (see _param_row_slider_precise_hover_preview and _settled)
#define DT_MASKS_PRECISE_HOVER_SETTLE_MS 200

// commit the previewed value once the pointer has rested for
// DT_MASKS_PRECISE_HOVER_SETTLE_MS, through the slider's "value-changed" as a
// drag does (_param_row_slider_precise_value_changed)
static gboolean _param_row_slider_precise_hover_settled(gpointer user_data)
{
  // held by a reference: a slider destroyed meanwhile has lost its parent
  GtkWidget *bauhaus_slider = user_data;
  if(!gtk_widget_get_parent(bauhaus_slider)) return G_SOURCE_REMOVE;
  GtkWidget *slider =
    g_object_get_data(G_OBJECT(bauhaus_slider), "precise-hover-preview-data");
  if(slider) g_object_set_data(G_OBJECT(slider), "precise-hover-settle", NULL);
  const float *value =
    g_object_get_data(G_OBJECT(bauhaus_slider), "precise-hover-last-value");
  // `value` is in the slider's raw domain (_slider_normalized_to_value in
  // bauhaus.c), which dt_bauhaus_slider_set() takes. Not
  // dt_bauhaus_slider_set_val(): it applies the factor and offset, and the
  // nodes would jump when the pointer stops
  if(value) dt_bauhaus_slider_set(bauhaus_slider, *value);
  return G_SOURCE_REMOVE;
}

// the dt_bauhaus_static_hover_preview_t hook (bauhaus.h), on each motion over
// the popup's slider with no button held: show node k, and the neighbors it
// pushes, at the hovered value on the row's slider, without changing the form,
// and arm the settle timer that commits it. A popup closed first discards the
// preview (_param_row_slider_precise_closed)
static void _param_row_slider_precise_hover_preview(GtkWidget *bauhaus_slider,
                                                    float value,
                                                    gpointer user_data)
{
  GtkWidget *slider = user_data;

  gint k;
  float newfrac;
  if(!_param_row_slider_precise_context(slider, value, &k, &newfrac)) return;

  // the popup shows the hovered value too. Under DT_IN_GUI_UPDATE, so that it
  // emits no "value-changed" (_slider_set_normalized): only the settle timer
  // or a click commits
  DT_ENTER_GUI_UPDATE();
  dt_bauhaus_slider_set(bauhaus_slider, value);
  DT_LEAVE_GUI_UPDATE();

  _param_row_slider_precise_restore_baseline(slider);

  DT_ENTER_GUI_UPDATE();
  dtgtk_gradient_slider_multivalue_set_value_pushing(DTGTK_GRADIENT_SLIDER(slider),
                                                     newfrac, k);
  DT_LEAVE_GUI_UPDATE();

  // redraw now, the row's window and the popup's alike: GTK does not run its
  // idle-priority redraws between back-to-back motion events, so both would
  // only catch up once the pointer stopped. gdk_window_process_updates is
  // deprecated, but GTK3 has no other way to force a synchronous redraw (GTK4
  // has no equivalent: its frame clock replaces it)
  GdkWindow *slider_window = gtk_widget_get_window(slider);
  GdkWindow *popup_window = gtk_widget_get_window(bauhaus_slider);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  if(slider_window) gdk_window_process_updates(slider_window, TRUE);
  if(popup_window && popup_window != slider_window)
    gdk_window_process_updates(popup_window, TRUE);
#pragma GCC diagnostic pop

  g_object_set_data(G_OBJECT(bauhaus_slider), "precise-hover-preview-data", slider);
  float *stored_value = g_new(float, 1);
  *stored_value = value;
  g_object_set_data_full(G_OBJECT(bauhaus_slider), "precise-hover-last-value",
                         stored_value, g_free);

  _param_row_slider_precise_cancel_settle(slider);
  const guint handle =
    g_timeout_add(DT_MASKS_PRECISE_HOVER_SETTLE_MS,
                  _param_row_slider_precise_hover_settled, bauhaus_slider);
  g_object_set_data(G_OBJECT(slider), "precise-hover-settle", GUINT_TO_POINTER(handle));
}

// destroy the closed popover and clear the object data that
// _param_row_slider_precise_pressed reads (which node has a popover open), so
// that no stale pointer is read. On Escape, a click outside, or the embedded
// slider's popup closing (_param_row_slider_precise_popup_hidden)
static void _param_row_slider_precise_closed(GtkPopover *popover, GtkWidget *slider)
{
  // a preview not yet settled was never committed: put the markers back
  // where the popup found them. What has settled stays committed
  if(_param_row_slider_precise_cancel_settle(slider))
    _param_row_slider_precise_restore_baseline(slider);
  g_object_set_data(G_OBJECT(slider), "precise-baseline", NULL);

  g_object_set_data(G_OBJECT(slider), "precise-popover", NULL);
  g_object_set_data(G_OBJECT(slider), "precise-marker", GINT_TO_POINTER(-1));
  // stop pinning this node's highlight now that its editor is gone (see
  // _param_row_slider_precise_open, which pins it on open).
  DTGTK_GRADIENT_SLIDER(slider)->pinned = -1;
  gtk_widget_queue_draw(slider);
  gtk_widget_destroy(GTK_WIDGET(popover));
}

// the "hide" of darktable.bauhaus->popup.window, the one popup window all
// bauhaus widgets share, connected when this popup opens: on Enter, Escape or
// a click outside, the anchor popover closes too, instead of needing a
// second dismissal
static void _param_row_slider_precise_popup_hidden(GtkWidget *bauhaus_popup_window,
                                                   GtkWidget *popover)
{
  g_signal_handlers_disconnect_by_func(bauhaus_popup_window,
                                       _param_row_slider_precise_popup_hidden, popover);
  if(GTK_IS_POPOVER(popover)) gtk_popover_popdown(GTK_POPOVER(popover));
}

// the embedded slider (see _param_row_slider_precise_open) is not realized
// yet at the point its anchor popover is first mapped, and _popup_show in
// bauhaus.c reads its GdkWindow to work out which toplevel the popup gets
// anchored to. Deferred to a plain idle instead of the "map" signal so this
// runs strictly after the size-allocate/realize pass that follows mapping
// (GTK services its own pending resizes at a higher priority than
// G_PRIORITY_DEFAULT_IDLE, so by the time this fires the widget is real).
static gboolean _param_row_slider_precise_open_idle(gpointer user_data)
{
  GtkWidget *bauhaus_slider = user_data;
  if(!GTK_IS_WIDGET(bauhaus_slider)) return G_SOURCE_REMOVE;
  GtkWidget *popover =
    g_object_get_data(G_OBJECT(bauhaus_slider), "precise-anchor-popover");
  GtkWidget *slider =
    g_object_get_data(G_OBJECT(bauhaus_slider), "precise-anchor-slider");
  const gint marker_x = GPOINTER_TO_INT(
    g_object_get_data(G_OBJECT(bauhaus_slider), "precise-anchor-marker-x"));
  if(slider && GTK_IS_WIDGET(slider))
    _show_bauhaus_whisker_popup(bauhaus_slider, slider, marker_x);
  else
    dt_bauhaus_widget_show_popup(bauhaus_slider);
  if(popover)
    g_signal_connect(G_OBJECT(darktable.bauhaus->popup.window), "hide",
                     G_CALLBACK(_param_row_slider_precise_popup_hidden), popover);
  return G_SOURCE_REMOVE;
}

// open an invisible anchor popover at node k, holding a bauhaus slider bound
// to the node's value, and open that slider's popup at once: the user edits in
// a normal bauhaus popup (_popup_show in bauhaus.c), with no extra click
static void _param_row_slider_precise_open(GtkWidget *slider,
                                           dt_masks_param_row_editor_t *ed,
                                           const gint k)
{
  dt_masks_point_parametric_t *p = _param_point(ed->formid);
  if(!p) return;
  const dt_iop_gui_blendif_channel_t *channels = _param_channels(p);
  if(!channels) return;
  const dt_iop_gui_blendif_channel_t *channel = &channels[p->channel];
  const int in_out = (slider == GTK_WIDGET(ed->filter[1].slider)) ? 1 : 0;
  const float boost_factor =
    _boost_factor(p->blendif_boost_factors, channels, p->channel, in_out);

  GtkDarktableGradientSlider *gslider = DTGTK_GRADIENT_SLIDER(slider);

  // keep the node highlighted while its editor is open: the popup opens away
  // from the slider (_bauhaus_whisker_popup_rect), and the hover highlight
  // (gradientslider.c's hovered_marker) goes when the pointer leaves it.
  // Cleared in _param_row_slider_precise_closed
  gslider->pinned = k;
  gtk_widget_queue_draw(slider);

  // the channel's whole range in display units (as
  // dt_masks_gui_param_row_slider_precise_display), not the room the
  // neighbors leave: a drag can push a neighbor along (_slider_move's
  // FREE_MARKERS branch in gradientslider.c), and so can this, through
  // dtgtk_gradient_slider_multivalue_set_value_pushing
  const double lo_frac = 0.0;
  const double hi_frac = 1.0;
  const gboolean is_hue = channel->scale_print == _blendif_scale_print_hue;
  const gboolean is_ab = channel->scale_print == _blendif_scale_print_ab;
  const float lo = dt_masks_gui_param_row_slider_precise_display(channel, boost_factor, lo_frac);
  const float hi = dt_masks_gui_param_row_slider_precise_display(channel, boost_factor, hi_frac);
  const float cur =
    dt_masks_gui_param_row_slider_precise_display(channel, boost_factor, gslider->position[k]);
  const int digits = is_hue ? 0 : 2;

  GtkWidget *bauhaus_slider =
    dt_bauhaus_slider_new_with_range(ed->module, lo, hi, 0, cur, digits);
  // exactly "°", no leading space: that string, together with a 360-wide
  // range, is what bauhaus matches on to give a slider the color-wheel popup
  // and the wrap-around past either end that an angle wants (see
  // _is_full_circle in bauhaus.c) -- the same treatment color balance rgb's
  // own hue sliders get. It is not a decoration to translate either, for the
  // same reason.
  dt_bauhaus_slider_set_format(bauhaus_slider, is_hue ? "°" : is_ab ? "" : "%");
  dt_bauhaus_widget_hide_label(bauhaus_slider);
  dt_bauhaus_widget_set_quad_visibility(bauhaus_slider, FALSE);
  dt_bauhaus_slider_set_val(bauhaus_slider, cur);
  // carry the channel's own gradient over from the row's range slider, so the
  // popup is colored like the track it is editing rather than a neutral bar.
  // The color wheel above needs it too: without stops it falls back to a bare
  // dial with no hues on it at all (see _draw_color_wheel in bauhaus.c).
  for(int s = 0; s < channel->numberstops; s++)
    dt_bauhaus_slider_set_stop(bauhaus_slider, channel->colorstops[s].stoppoint,
                               channel->colorstops[s].color.red,
                               channel->colorstops[s].color.green,
                               channel->colorstops[s].color.blue);
  // fill the popover's own width fully -- a bauhaus widget's halign otherwise
  // defaults to only claiming its own (narrow) natural width even inside an
  // expand+fill box slot.
  gtk_widget_set_hexpand(bauhaus_slider, TRUE);
  gtk_widget_set_halign(bauhaus_slider, GTK_ALIGN_FILL);
  // this popup opens away from the pointer on purpose (see
  // _bauhaus_whisker_popup_rect, positioned against the row's own
  // bounds, not the click point) -- tell bauhaus's own popup motion handler
  // not to apply its usual "opened at the pointer" assumptions (hover alone
  // dragging the value, auto-reject once the pointer strays too far from
  // where it opened): see the "static_popup" check in _window_motion_handle.
  g_object_set_data(G_OBJECT(bauhaus_slider), "dt-bauhaus-static-popup",
                    GINT_TO_POINTER(1));
  g_signal_connect(G_OBJECT(bauhaus_slider), "value-changed",
                   G_CALLBACK(_param_row_slider_precise_value_changed), slider);
  // a static popup gives up bauhaus's "bare hover drags the value", so a hover
  // preview hook gets the value under the pointer instead and decides what to
  // preview and when to commit (see _param_row_slider_precise_hover_preview).
  // Every marker's position as the popup found it is the baseline both a drag
  // and a preview recompute from, so a neighbor pushed aside eases back when
  // the gesture reverses, and what a dismissed popup reverts to
  gdouble *baseline = g_new(gdouble, GRADIENT_SLIDER_MAX_POSITIONS);
  dtgtk_gradient_slider_multivalue_get_values(gslider, baseline);
  g_object_set_data_full(G_OBJECT(slider), "precise-baseline", baseline, g_free);
  g_object_set_data(G_OBJECT(bauhaus_slider), "dt-bauhaus-static-hover-preview",
                    (gpointer)_param_row_slider_precise_hover_preview);
  g_object_set_data(G_OBJECT(bauhaus_slider), "dt-bauhaus-static-hover-preview-data",
                    slider);

  GtkWidget *popover = gtk_popover_new(slider);
  // not modal: the bauhaus popup (_popup_show in bauhaus.c) is a window of
  // its own with its own grab, which a modal anchor's grab would beat, leaving
  // the popup deaf to clicks and keys
  gtk_popover_set_modal(GTK_POPOVER(popover), FALSE);
  // transparent but mapped: the anchor only gives the embedded slider a place
  // on screen for its popup to open from. By CSS
  // (popover.dt_masks_precise_anchor): darktable.css's "popover { opacity: 1 }"
  // beats gtk_widget_set_opacity()
  dt_gui_add_class(popover, "dt_masks_precise_anchor");
  GtkWidget *box = dt_gui_hbox(bauhaus_slider);
  gtk_widget_set_size_request(box, DT_PIXEL_APPLY_DPI(160), -1);
  gtk_container_add(GTK_CONTAINER(popover), box);
  gtk_widget_show_all(box);

  // anchored at the node's x, as a bauhaus popup opens over its value
  GtkAllocation alloc;
  gtk_widget_get_allocation(slider, &alloc);
  const int usable = MAX(alloc.width - gslider->margin_left - gslider->margin_right, 1);
  // the slider's full height, so that GTK places the popup above or below
  // the whole slider, not over its middle
  const GdkRectangle rect = { gslider->margin_left + (int)(gslider->position[k] * usable),
                              0, 1, alloc.height };
  gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);

  g_signal_connect(G_OBJECT(popover), "closed",
                   G_CALLBACK(_param_row_slider_precise_closed), slider);
  g_object_set_data(G_OBJECT(bauhaus_slider), "precise-anchor-popover", popover);
  // consulted by _param_row_slider_precise_open_idle to place the real
  // bauhaus popup against the slider's own bounds instead of this anchor's.
  g_object_set_data(G_OBJECT(bauhaus_slider), "precise-anchor-slider", slider);
  // the node's x within the slider, which _bauhaus_whisker_popup_rect
  // centers the popup on, from the slider's allocation
  g_object_set_data(G_OBJECT(bauhaus_slider), "precise-anchor-marker-x",
                    GINT_TO_POINTER(gslider->margin_left
                                    + (gint)(gslider->position[k] * usable)));

  g_object_set_data(G_OBJECT(slider), "precise-popover", popover);
  g_object_set_data(G_OBJECT(slider), "precise-marker", GINT_TO_POINTER(k));

  gtk_popover_popup(GTK_POPOVER(popover));
  g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, _param_row_slider_precise_open_idle,
                  g_object_ref(bauhaus_slider), g_object_unref);
}

// right-click on one of this row's own range-slider nodes: instead of the
// widget's own right-click handling (see _gradient_slider_button_pressed in
// dtgtk/gradientslider.c), pop up the precise-entry UI above for the node
// nearest the click, closing it again on a second right-click on the same
// node (toggle). Claims the right-clicks it handles, ahead of the slider's own
// gesture (see _press_before_widget)
static void _param_row_slider_precise_pressed(GtkGestureSingle *gesture,
                                              const int n_press,
                                              const double x,
                                              const double y,
                                              gpointer user_data)
{
  if(gtk_gesture_single_get_current_button(gesture) != GDK_BUTTON_SECONDARY) return;

  GtkWidget *widget = dt_gui_get_widget(gesture);
  dt_masks_param_row_editor_t *ed =
    g_object_get_data(G_OBJECT(widget), "param-row-editor");
  if(!ed) return;

  GtkDarktableGradientSlider *gslider = DTGTK_GRADIENT_SLIDER(widget);
  const gint k = gslider->active >= 0 ? gslider->active : gslider->selected;
  if(k < 0 || k >= gslider->positions) return;

  dt_gui_claim(gesture);
  GtkWidget *existing = g_object_get_data(G_OBJECT(widget), "precise-popover");
  const gint existing_k =
    GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "precise-marker"));
  if(existing)
  {
    const gboolean same = (existing_k == k);
    // synchronously fires "closed" (see _param_row_slider_precise_closed),
    // which destroys it and clears both object-data slots before this
    // function goes on to read them again below
    gtk_popover_popdown(GTK_POPOVER(existing));
    if(same) return;
  }

  _param_row_slider_precise_open(widget, ed, k);
}

// set a parametric channel's boost factor, rescaling its ranges so they keep
// selecting the same values. Shared by the row's own boost slider and the
// properties subpanel's (see _build_param_boost_editor)
static void _param_boost_apply(dt_iop_module_t *module,
                               const dt_mask_id_t formid,
                               const float value)
{
  dt_masks_point_parametric_t *p = _param_point(formid);
  if(!p) return;
  const dt_iop_gui_blendif_channel_t *channels = _param_channels(p);
  if(!channels) return;
  const dt_iop_gui_blendif_channel_t *channel = &channels[p->channel];

  for(int in_out = 1; in_out >= 0; in_out--)
  {
    const int ch = channel->param_channels[in_out];
    float off = 0.0f;
    if(p->colorspace == DEVELOP_BLEND_CS_LAB
       && (ch == DEVELOP_BLENDIF_A_in || ch == DEVELOP_BLENDIF_A_out
           || ch == DEVELOP_BLENDIF_B_in || ch == DEVELOP_BLENDIF_B_out))
      off = 0.5f;
    const float new_value = value + channel->boost_factor_offset;
    const float old_value = p->blendif_boost_factors[ch];
    const float factor = exp2f(old_value) / exp2f(new_value);
    float *parameters = &p->blendif_parameters[4 * ch];
    if(parameters[0] > 0.0f) parameters[0] = CLIP((parameters[0] - off) * factor + off);
    if(parameters[1] > 0.0f) parameters[1] = CLIP((parameters[1] - off) * factor + off);
    if(parameters[2] < 1.0f) parameters[2] = CLIP((parameters[2] - off) * factor + off);
    if(parameters[3] < 1.0f) parameters[3] = CLIP((parameters[3] - off) * factor + off);
    if(parameters[1] == 0.0f && parameters[2] == 1.0f) p->blendif &= ~(1 << ch);
    p->blendif_boost_factors[ch] = new_value;
  }
  _param_form_commit(module, formid);
}

static void _param_row_boost_factor_callback(GtkWidget *slider,
                                             dt_masks_param_row_editor_t *ed)
{
  if(DT_IN_GUI_UPDATE()) return;
  _param_boost_apply(ed->module, ed->formid, dt_bauhaus_slider_get(slider));
  _update_param_row_display(ed);
}

// the subpanel's boost slider: the row's input and output sliders read their
// values through the boost factor, so they are refreshed with it
static void _param_panel_boost_changed(GtkWidget *slider, dt_iop_module_t *module)
{
  if(DT_IN_GUI_UPDATE()) return;
  const dt_mask_id_t formid =
    GPOINTER_TO_INT(g_object_get_data(G_OBJECT(slider), "param-formid"));
  _param_boost_apply(module, formid, dt_bauhaus_slider_get(slider));
  dt_masks_param_row_editor_t *ed = _param_row_editor(module->blend_data, formid);
  if(ed) _update_param_row_display(ed);
}

// a parametric channel's boost factor, for the properties subpanel: built like
// the row's own (see _build_param_row_editor), bound to the form by id
static GtkWidget *_build_param_boost_editor(dt_iop_module_t *module, const dt_mask_id_t formid)
{
  const dt_masks_point_parametric_t *p = _param_point(formid);
  const dt_iop_gui_blendif_channel_t *channels = _param_channels(p);

  GtkWidget *slider = dt_bauhaus_slider_new_with_range(module, 0.0f, 18.0f, 0, 0.0f, 3);
  dt_bauhaus_slider_set_format(slider, _(" EV"));
  dt_bauhaus_widget_set_label(slider, N_("blend"), N_("boost factor"));
  dt_bauhaus_slider_set_soft_range(slider, 0.0, 3.0);
  dt_bauhaus_widget_set_quad_visibility(slider, FALSE);
  gtk_widget_set_tooltip_text(
    slider,
    _("adjust the channel boost factor.\nincrease to allow matching values over 100%"));
  dt_gui_add_class(slider, "dt_masks_boost_slider");
  if(channels)
  {
    const dt_iop_gui_blendif_channel_t *channel = &channels[p->channel];
    DT_ENTER_GUI_UPDATE();
    dt_bauhaus_slider_set(slider, p->blendif_boost_factors[channel->param_channels[0]]
                                    - channel->boost_factor_offset);
    DT_LEAVE_GUI_UPDATE();
  }
  g_object_set_data(G_OBJECT(slider), "param-formid", GINT_TO_POINTER(formid));
  g_signal_connect(G_OBJECT(slider), "value-changed",
                   G_CALLBACK(_param_panel_boost_changed), module);
  GtkWidget *box = dt_gui_vbox(slider);
  dt_gui_add_class(box, "dt_masks_boost_box");
  return box;
}

// the parametric row's own opacity slider (packed alongside output/boost,
// under the same p->in_out gate -- see _update_param_row_visibility): commits
// via the same shared _props_row_apply every other row kind's opacity control
// uses, scoped to just this one form.
static void _param_row_opacity_changed(GtkWidget *widget, dt_masks_param_row_editor_t *ed)
{
  if(DT_IN_GUI_UPDATE() || !ed) return;
  GList *ids = g_list_prepend(NULL, GINT_TO_POINTER(ed->formid));
  _props_row_apply(ed->module, ids, DT_MASKS_PROPERTY_OPACITY, widget,
                   &ed->opacity_last_value, FALSE);
  g_list_free(ids);
}

// find the per-row editor struct owning `picker` (tagged "param-row-formid" at
// creation, see _build_param_row_editor), or NULL if `picker` is not one of
// this module's per-row picker buttons.
static dt_masks_param_row_editor_t *_param_row_editor_for_picker(dt_iop_module_t *module,
                                                                 GtkWidget *picker)
{
  return _param_row_editor(module->blend_data, _widget_id(picker, "param-row-formid"));
}

// arm the picker for this row's channel and colorspace before it samples:
// the pipe converts the pick into the colorspace the picker was last armed
// for, and for another channel's (an RGB pick for a JzCzhz "hz" row)
// _blendif_scale leaves the channel unwritten, a zero-width range
static void _update_param_row_slider_pickers(dt_masks_param_row_editor_t *ed);

static void _param_row_arm_picker_cst(GtkWidget *button, dt_masks_param_row_editor_t *ed)
{
  const dt_masks_point_parametric_t *p = _param_point(ed->formid);
  if(!p) return;
  dt_iop_color_picker_set_cst(
    ed->module, _picker_colorspace_for_channel(
                  (dt_develop_blend_colorspace_t)p->colorspace, (int)p->channel));
  // also refresh (or clear) this row's picker marker/label to match the
  // button's new armed/disarmed state
  _update_param_row_slider_pickers(ed);

  const gboolean btn_active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
  const dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, ed->formid);
  const char *element_name = (form && form->name[0]) ? form->name : _("parametric");

  if(button == ed->colorpicker_set_values)
  {
    const gboolean is_output =
      GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "pick-output")) != 0;
    if(btn_active)
      dt_toast_log(is_output ? _("output picker of %s armed")
                             : _("input picker of %s armed"),
                   element_name);
    else
      dt_toast_log(is_output ? _("output picker of %s unarmed")
                             : _("input picker of %s unarmed"),
                   element_name);
  }
  else if(button == ed->colorpicker)
  {
    if(btn_active)
      dt_toast_log(_("color picker of %s armed"), element_name);
    else
      dt_toast_log(_("color picker of %s unarmed"), element_name);
  }

  // keep the one visible button's own look in sync with whichever of the two
  // real (hidden) pickers this "toggled" came from -- see
  // _param_row_master_picker_pressed.
  if(ed->master_picker)
  {
    const gboolean active =
      gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ed->colorpicker))
      || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ed->colorpicker_set_values));
    if(!active && ed->colorpicker_set_values)
      g_object_set_data(G_OBJECT(ed->colorpicker_set_values), "pick-output", GINT_TO_POINTER(0));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ed->master_picker), active);
  }
}

// consolidated front-end for a parametric row's two color pickers (saves a
// slot in the row's action cluster): one visible button standing in for both
// hidden-but-functional real ones. It has no picker logic of its own, only
// this dispatch, so its own click/toggle must never fire (see the claim above)
//   plain click      -> colorpicker_set_values (area only), applied to the
//                       input range (see _param_row_picker_apply)
//   shift+click      -> same picker, applied to the output range instead (the
//                       modifier is read again at apply time, on the canvas
//                       pick/drag, not here)
//   ctrl+click       -> colorpicker, point mode
//   ctrl+right-click -> colorpicker, area mode
static void _param_row_master_picker_pressed(GtkGesture *gesture,
                                             gint n_press,
                                             gdouble x,
                                             gdouble y,
                                             dt_masks_param_row_editor_t *ed)
{
  const gboolean ctrl = dt_modifier_is(dt_key_modifier_state(), GDK_CONTROL_MASK);
  const gboolean shift = dt_modifier_is(dt_key_modifier_state(), GDK_SHIFT_MASK);
  const gboolean right =
    gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture))
    == GDK_BUTTON_SECONDARY;
  if(ctrl)
    dt_color_picker_click(ed->colorpicker, right);
  else if(!right)
  {
    g_object_set_data(G_OBJECT(ed->colorpicker_set_values), "pick-output",
                      GINT_TO_POINTER(shift));
    // arming, rather than disarming: start from the area the module's last
    // pick used, so one area sets the range of channel after channel
    dt_iop_gui_blend_data_t *bd = ed->module->blend_data;
    if(_param_picker_reuses_area() && bd && bd->param_pick_box_set
       && !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ed->colorpicker_set_values)))
      dt_iop_color_picker_reuse_area(ed->colorpicker_set_values, bd->param_pick_box);
    dt_color_picker_click(ed->colorpicker_set_values, FALSE);
  }
}

// the profile a parametric point's picked colors are read through
static const dt_iop_order_iccprofile_info_t *
_param_work_profile(dt_iop_module_t *module,
                    const dt_masks_point_parametric_t *p,
                    dt_dev_pixelpipe_t *pipe)
{
  return p->colorspace == DEVELOP_BLEND_CS_RGB_SCENE
           ? dt_ioppr_get_pipe_current_profile_info(module, pipe)
           : dt_ioppr_get_iop_work_profile_info(module, module->dev->iop);
}

// the "pick GUI color" picker changes no value: it moves the mean, min and
// max markers on the row's slider to where the sampled color falls
static void _update_param_row_slider_pickers(dt_masks_param_row_editor_t *ed)
{
  const dt_masks_point_parametric_t *p = _param_point(ed->formid);
  if(!p) return;
  const dt_iop_gui_blendif_channel_t *channels = _param_channels(p);
  if(!channels) return;

  dt_iop_module_t *module = ed->module;
  float *raw_mean, *raw_min, *raw_max;

  DT_ENTER_GUI_UPDATE();

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

    dt_iop_gui_blendif_filter_t *sl = &ed->filter[in_out];

    if((gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ed->colorpicker))
        || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ed->colorpicker_set_values)))
       && (raw_min[0] != FLT_MAX))
    {
      float picker_mean[8], picker_min[8], picker_max[8];

      const dt_iop_colorspace_type_t cst = _picker_colorspace_for_channel(
        (dt_develop_blend_colorspace_t)p->colorspace, (int)p->channel);
      const dt_iop_order_iccprofile_info_t *work_profile =
        _param_work_profile(module, p, module->dev->full.pipe);

      _blendif_scale(p->blendif_boost_factors, channels, cst, raw_mean, picker_mean,
                        work_profile, in_out);
      _blendif_scale(p->blendif_boost_factors, channels, cst, raw_min, picker_min,
                        work_profile, in_out);
      _blendif_scale(p->blendif_boost_factors, channels, cst, raw_max, picker_max,
                        work_profile, in_out);
      const int tab = (int)p->channel;
      dtgtk_gradient_slider_multivalue_set_picker_meanminmax(
        sl->slider, CLAMP(picker_mean[tab], 0.0f, 1.0f),
        CLAMP(picker_min[tab], 0.0f, 1.0f), CLAMP(picker_max[tab], 0.0f, 1.0f));
    }
    else
      dtgtk_gradient_slider_multivalue_set_picker(sl->slider, NAN);
  }

  DT_LEAVE_GUI_UPDATE();
}

// applies a picked color straight into this row's own form (no bp scratch),
// for either of the row's two pickers (see blend_color_picker_apply)
static gboolean _param_row_picker_apply(dt_iop_module_t *module,
                                        GtkWidget *picker,
                                        dt_dev_pixelpipe_t *pipe)
{
  dt_masks_param_row_editor_t *ed = _param_row_editor_for_picker(module, picker);
  if(!ed) return FALSE;
  dt_masks_point_parametric_t *p = _param_point(ed->formid);
  if(!p) return FALSE;
  const dt_iop_gui_blendif_channel_t *channels = _param_channels(p);
  if(!channels) return FALSE;

  if(picker == ed->colorpicker_set_values)
  {
    DT_TRY_GUI_UPDATE(TRUE);

    // the area this range comes from, for the next parametric pick to reuse
    // (see _param_row_master_picker_pressed). A deferred picker's blank box,
    // before any drag, is no area
    const dt_iop_color_picker_t *inst =
      g_object_get_data(G_OBJECT(picker), DT_COLOR_PICKER_INSTANCE_KEY);
    static const dt_pickerbox_t blank = { 0 };
    dt_iop_gui_blend_data_t *bd = module->blend_data;
    if(inst && bd && memcmp(inst->pick_box, blank, sizeof(blank)))
    {
      memcpy(bd->param_pick_box, inst->pick_box, sizeof(bd->param_pick_box));
      bd->param_pick_box_set = TRUE;
    }

    const int tab = (int)p->channel;
    dt_aligned_pixel_t raw_min, raw_max;
    float picker_min[8] DT_ALIGNED_PIXEL, picker_max[8] DT_ALIGNED_PIXEL;
    dt_aligned_pixel_t picker_values;

    // shift picks the output range: ctrl selects the other picker (see
    // _param_row_master_picker_pressed)
    const gboolean armed_shift =
      GPOINTER_TO_INT(g_object_get_data(G_OBJECT(picker), "pick-output"));
    const gboolean current_shift =
      dt_modifier_is(dt_key_modifier_state(), GDK_SHIFT_MASK);
    const int in_out = (armed_shift || current_shift) ? 1 : 0;

    const float *picked_min = in_out ? module->picked_output_color_min : module->picked_color_min;
    const float *picked_max = in_out ? module->picked_output_color_max : module->picked_color_max;
    for(size_t i = 0; i < 4; i++)
    {
      raw_min[i] = picked_min[i];
      raw_max[i] = picked_max[i];
    }

    const dt_iop_gui_blendif_channel_t *channel = &channels[p->channel];
    const dt_develop_blendif_channels_t ch = channel->param_channels[in_out];
    dt_iop_gui_blendif_filter_t *sl = &ed->filter[in_out];
    float *parameters = &p->blendif_parameters[4 * ch];

    // always derive from this row's own channel rather than trusting
    // dt_iop_color_picker_get_active_cst()'s stored state -- with several
    // rows' pickers sharing one module-wide picker object, that state only
    // reflects whichever row last armed it (see _param_row_arm_picker_cst),
    // which is unreliable to re-derive at apply time.
    const dt_iop_colorspace_type_t cst = _picker_colorspace_for_channel(
      (dt_develop_blend_colorspace_t)p->colorspace, (int)p->channel);
    const dt_iop_order_iccprofile_info_t *work_profile = _param_work_profile(module, p, pipe);

    gboolean reverse_hues = FALSE;
    if(cst == IOP_CS_HSL && tab == CHANNEL_INDEX_H)
    {
      if((raw_max[3] - raw_min[3]) < (raw_max[0] - raw_min[0]) && raw_min[3] < 0.5f
         && raw_max[3] > 0.5f)
      {
        raw_max[0] = raw_max[3] < 0.5f ? raw_max[3] + 0.5f : raw_max[3] - 0.5f;
        raw_min[0] = raw_min[3] < 0.5f ? raw_min[3] + 0.5f : raw_min[3] - 0.5f;
        reverse_hues = TRUE;
      }
    }
    else if((cst == IOP_CS_LCH && tab == CHANNEL_INDEX_h)
            || (cst == IOP_CS_JZCZHZ && tab == CHANNEL_INDEX_hz))
    {
      if((raw_max[3] - raw_min[3]) < (raw_max[2] - raw_min[2]) && raw_min[3] < 0.5f
         && raw_max[3] > 0.5f)
      {
        raw_max[2] = raw_max[3] < 0.5f ? raw_max[3] + 0.5f : raw_max[3] - 0.5f;
        raw_min[2] = raw_min[3] < 0.5f ? raw_min[3] + 0.5f : raw_min[3] - 0.5f;
        reverse_hues = TRUE;
      }
    }

    _blendif_scale(p->blendif_boost_factors, channels, cst, raw_min, picker_min,
                      work_profile, in_out);
    _blendif_scale(p->blendif_boost_factors, channels, cst, raw_max, picker_max,
                      work_profile, in_out);

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

    for(int k = 0; k < 4; k++)
      dtgtk_gradient_slider_multivalue_set_value(sl->slider, picker_values[k], k);

    DT_LEAVE_GUI_UPDATE();

    for(int k = 0; k < 4; k++)
      parameters[k] = dtgtk_gradient_slider_multivalue_get_value(sl->slider, k);

    if(parameters[1] == 0.0f && parameters[2] == 1.0f)
      p->blendif &= ~(1 << ch);
    else
      p->blendif |= (1 << ch);

    // reverse_hues alone sets the polarity bit: a single-channel form has no
    // whole-mask invert, and its element invert is applied by the fold
    if(reverse_hues)
      p->blendif |= 1 << (16 + ch);
    else
      p->blendif &= ~(1 << (16 + ch));

    _param_form_commit(module, ed->formid);
    _update_param_row_display(ed);
    // a pick can take the range to or from the full span, as a drag can (see
    // _param_row_slider_callback); the range is set without "value-changed",
    // so the badges are refreshed here
    _refresh_lowop_badges(module);

    return TRUE;
  }
  else if(picker == ed->colorpicker)
  {
    DT_GUARD_GUI_UPDATE(TRUE);
    _update_param_row_slider_pickers(ed);
    return TRUE;
  }
  return FALSE;
}

// build one input-or-output slider bundle. There is no per-slider polarity
// button: the element's own invert (the row's actions menu) stands in for it
static void _build_param_row_filter(dt_iop_gui_blendif_filter_t *sl, const int in_out)
{
  sl->slider =
    DTGTK_GRADIENT_SLIDER_MULTIVALUE(dtgtk_gradient_slider_multivalue_new(4));
  dt_gui_add_class(GTK_WIDGET(sl->slider), "dt_masks_range_slider");
  gtk_widget_set_tooltip_text(GTK_WIDGET(sl->slider), _(slider_tooltip[in_out]));
  sl->altmode = 0;
  sl->altmode_name = NULL;
}

// the eye floats over the right end of its slider row instead of taking a
// grid column of its own, and the slider gives back the eye's width, so that
// even a handle at 1.0 stops short of the eye.
// Run on style changes, not size-allocate: a resize queued from inside an
// allocation is dropped, and the margin would wait for some later relayout.
static void _param_slider_fit_eye(dt_masks_param_row_editor_t *ed)
{
  GtkWidget *eyes[2] = { ed->input_bypass_slot, ed->output_bypass_slot };
  for(int i = 0; i < 2; i++)
  {
    GtkWidget *slider = GTK_WIDGET(ed->filter[i].slider);
    if(!slider || !eyes[i]) continue;
    gint eye_w = 0;
    gtk_widget_get_preferred_width(eyes[i], &eye_w, NULL);
    // no width yet (unstyled, or hidden with its row): the next style change
    // or _update_param_row_visibility tries again
    if(eye_w <= 0) continue;
    if(gtk_widget_get_margin_end(slider) != eye_w)
      gtk_widget_set_margin_end(slider, eye_w);
  }
}

// connected with g_signal_connect_object on the editor box, which owns ed
// (freed with it), so this is disconnected before ed goes away
static void _param_slider_style_updated(GtkWidget *widget,
                                        GtkWidget *wrap)
{
  dt_masks_param_row_editor_t *ed = g_object_get_data(G_OBJECT(wrap), "param-editor");
  if(ed) _param_slider_fit_eye(ed);
}

// the "temporarily disable this channel" eye that sits at the right end of a
// parametric row's input/output slider
static GtkWidget *_make_param_bypass_btn(const char *tooltip,
                                         dt_masks_param_row_editor_t *ed)
{
  GtkWidget *btn = dtgtk_togglebutton_new(dtgtk_cairo_paint_eye_toggle, 0, NULL);
  // -bypass-btn carries the look (dimmed until hovered or checked),
  // -param-bypass-btn the geometry: exactly the row header's expander button,
  // so the two read as one column rather than two similar icons that happen to
  // be near each other
  dt_gui_add_class(btn, "dt_masks_bypass");
  dt_gui_add_class(btn, "dt_masks_icon");
  dt_gui_add_class(btn, "dt_masks_channel_eye");
  gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(btn, tooltip);
  g_signal_connect(G_OBJECT(btn), "toggled",
                   G_CALLBACK(_param_channel_bypass_toggled), ed);
  return btn;
}

// the fixed-width box the eye above lives in, laid over the right end of its
// slider row (see _param_slider_fit_eye).
//
// The eye comes and goes with whether the channel has both sub-ranges in play
// (see _update_param_row_visibility); the box keeps its width whether or not
// the eye is in it, so the slider's margin does not change as the user edits.
//
// That width is the row header's expander button, and the editor's right edge
// is flush with the header's (see .dt_masks_param_card in darktable.css), so
// the eyes stack directly under the expander.
static GtkWidget *_make_param_bypass_slot(GtkWidget *btn)
{
  GtkWidget *slot = dt_gui_hbox();
  dt_gui_add_class(slot, "dt_masks_bypass_slot");
  // an overlay child fills the overlay unless told otherwise
  gtk_widget_set_halign(slot, GTK_ALIGN_END);
  gtk_widget_set_valign(slot, GTK_ALIGN_CENTER);
  dt_gui_box_add(slot, btn);
  return slot;
}

// build the editor of parametric row `form`: returns the sliders and boost
// factor to pack under the row, and in *picker_box_out the picker button
// (with its two hidden pickers) for the row's header (see _make_shape_row).
// The editor struct is freed with the returned widget
static GtkWidget *_build_param_row_editor(dt_iop_module_t *module,
                                          dt_masks_form_t *form,
                                          GtkWidget **picker_box_out)
{
  const dt_masks_point_parametric_t *p = form->points ? form->points->data : NULL;
  if(!p)
  {
    if(picker_box_out) *picker_box_out = NULL;
    return NULL;
  }

  dt_masks_param_row_editor_t *ed = g_malloc0(sizeof(dt_masks_param_row_editor_t));
  ed->formid = form->formid;
  ed->module = module;

  _build_param_row_filter(&ed->filter[0], 0);
  _build_param_row_filter(&ed->filter[1], 1);

  for(int in_out = 0; in_out < 2; in_out++)
  {
    dt_iop_gui_blendif_filter_t *sl = &ed->filter[in_out];
    g_signal_connect(G_OBJECT(sl->slider), "value-changed",
                     G_CALLBACK(_param_row_slider_callback), ed);
    g_signal_connect(G_OBJECT(sl->slider), "value-reset",
                     G_CALLBACK(_param_row_slider_reset_callback), ed);
    // back-reference so _param_row_editor_channel can resolve THIS row's own
    // channel from the slider alone
    g_object_set_data(G_OBJECT(sl->slider), "param-row-editor", ed);
    dt_gui_connect_motion(sl->slider, NULL, _blendop_blendif_enter_cb,
                          _blendop_blendif_leave_cb, module);
    dt_gui_connect_key(sl->slider, _blendop_blendif_key_press_cb, module);
    // right-click: precise numeric entry for the nearest node (see
    // _param_row_slider_precise_pressed), replacing this widget's own built-in
    // right-click behavior just for these range sliders.
    _press_before_widget(dt_gui_connect_click(sl->slider, _param_row_slider_precise_pressed,
                                              NULL, NULL));
  }

  // both real pickers stay fully functional (dt_color_picker_click below
  // arms them programmatically), just never shown -- see master_picker,
  // built after them, which is the row's one visible button.
  GtkWidget *picker_box = dt_gui_hbox();
  gtk_widget_set_valign(picker_box, GTK_ALIGN_CENTER);
  ed->colorpicker = dt_color_picker_new(module,
                                        DT_COLOR_PICKER_POINT_AREA | DT_COLOR_PICKER_IO
                                          | DT_COLOR_PICKER_DEFERRED_AREA,
                                        picker_box);
  gtk_widget_set_no_show_all(ed->colorpicker, TRUE);
  gtk_widget_hide(ed->colorpicker);
  g_object_set_data(G_OBJECT(ed->colorpicker), "param-row-formid",
                    GINT_TO_POINTER(ed->formid));
  g_signal_connect(G_OBJECT(ed->colorpicker), "toggled",
                   G_CALLBACK(_param_row_arm_picker_cst), ed);

  // deferred: don't sample a big default box the instant this arms (see
  // DT_COLOR_PICKER_DEFERRED_AREA) -- wait for the user's own drag on canvas,
  // so the range isn't set from ~96% of the image before they've picked
  // anything.
  ed->colorpicker_set_values = dt_color_picker_new(
    module, DT_COLOR_PICKER_AREA | DT_COLOR_PICKER_IO | DT_COLOR_PICKER_DEFERRED_AREA,
    picker_box);
  gtk_widget_set_no_show_all(ed->colorpicker_set_values, TRUE);
  gtk_widget_hide(ed->colorpicker_set_values);
  g_object_set_data(G_OBJECT(ed->colorpicker_set_values), "param-row-formid",
                    GINT_TO_POINTER(ed->formid));
  g_signal_connect(G_OBJECT(ed->colorpicker_set_values), "toggled",
                   G_CALLBACK(_param_row_arm_picker_cst), ed);

  // the one visible button standing in for both -- see
  // _param_row_master_picker_pressed for the modifier dispatch. Built the
  // same way dt_color_picker_new's own buttons are (dtgtk togglebutton +
  // CAPTURE-phase gesture claiming the press), since it needs the identical
  // "my handler fully owns click/toggle state" behavior but with no
  // dt_iop_color_picker_t of its own to hand that off to.
  ed->master_picker = dtgtk_togglebutton_new(dtgtk_cairo_paint_colorpicker, 0, NULL);
  dt_gui_add_class(ed->master_picker, "dt_transparent_background");
  dt_gui_add_class(ed->master_picker, "dt_masks_icon");
  dt_gui_add_class(ed->master_picker, "dt_masks_picker");
  gtk_widget_set_valign(ed->master_picker, GTK_ALIGN_CENTER);
  gtk_widget_set_name(ed->master_picker, "keep-active");
  gtk_widget_set_tooltip_text(ed->master_picker,
                              _("click: set the input range from an area picked on the image\n"
                                "shift+click: set the output range the same way\n"
                                "ctrl+click: pick GUI color, shown on the sliders (point)\n"
                                "ctrl+right-click: pick GUI color (area)"));
  GtkGesture *master_gesture = gtk_gesture_multi_press_new(ed->master_picker);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(master_gesture),
                                             GTK_PHASE_CAPTURE);
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(master_gesture), 0);
  dt_gui_add_controller(ed->master_picker, master_gesture);
  g_signal_connect(master_gesture, "pressed",
                   G_CALLBACK(_param_row_master_picker_pressed), ed);
  // claimed in the CAPTURE phase, so the button's own click-to-toggle never runs
  g_signal_connect(master_gesture, "begin", G_CALLBACK(dt_gui_gesture_claim), NULL);
  dt_gui_box_add(picker_box, ed->master_picker);

  ed->boost_slider = dt_bauhaus_slider_new_with_range(module, 0.0f, 18.0f, 0, 0.0f, 3);
  dt_bauhaus_slider_set_format(ed->boost_slider, _(" EV"));
  dt_bauhaus_widget_set_label(ed->boost_slider, N_("blend"), N_("boost factor"));
  dt_bauhaus_slider_set_soft_range(ed->boost_slider, 0.0, 3.0);
  // this slider has no quad icon, so hide the quad area entirely instead of
  // leaving an empty reserved patch to its right
  dt_bauhaus_widget_set_quad_visibility(ed->boost_slider, FALSE);
  gtk_widget_set_tooltip_text(
    ed->boost_slider,
    _("adjust the channel boost factor.\nincrease to allow matching values over 100%"));
  g_signal_connect(G_OBJECT(ed->boost_slider), "value-changed",
                   G_CALLBACK(_param_row_boost_factor_callback), ed);
  dt_gui_add_class(ed->boost_slider, "dt_masks_boost_slider");
  ed->boost_box = dt_gui_vbox(ed->boost_slider);
  dt_gui_add_class(ed->boost_box, "dt_masks_boost_box");

  // the opacity slider, leading the controls the in/out chevron expands, as
  // a shape row's does (see dt_masks_model_param_row_visibility). Labeled like
  // boost_box, and applied through _props_row_apply like any opacity
  ed->opacity_slider = dt_bauhaus_slider_new_with_range(
    module, _blend_masks_properties[DT_MASKS_PROPERTY_OPACITY].min,
    _blend_masks_properties[DT_MASKS_PROPERTY_OPACITY].max, 0, 1.0, 2);
  dt_bauhaus_widget_set_label(ed->opacity_slider, N_("blend"),
                              _blend_masks_properties[DT_MASKS_PROPERTY_OPACITY].name);
  dt_bauhaus_slider_set_format(ed->opacity_slider,
                               _blend_masks_properties[DT_MASKS_PROPERTY_OPACITY].format);
  dt_bauhaus_slider_set_digits(ed->opacity_slider, 2);
  // no quad icon -- see the same call for the shape/group properties sliders
  dt_bauhaus_widget_set_quad_visibility(ed->opacity_slider, FALSE);
  ed->opacity_last_value = dt_bauhaus_slider_get(ed->opacity_slider);
  g_object_set_data(G_OBJECT(ed->opacity_slider), "dt-prop",
                    GINT_TO_POINTER(DT_MASKS_PROPERTY_OPACITY));
  g_signal_connect(G_OBJECT(ed->opacity_slider), "value-changed",
                   G_CALLBACK(_param_row_opacity_changed), ed);
  // the slider's opaque background would hide the row's hover and selection
  // wash, as for the properties sliders
  dt_gui_add_class(ed->opacity_slider, "dt_masks_props_slider");
  g_signal_connect(G_OBJECT(ed->opacity_slider), "value-changed",
                   G_CALLBACK(_inline_opacity_tooltip_changed), NULL);
  // the same margins as boost_box
  ed->opacity_box = dt_gui_vbox(ed->opacity_slider);
  dt_gui_add_class(ed->opacity_box, "dt_masks_opacity_box");

  GtkWidget *sliders_grid = gtk_grid_new();
  gtk_grid_set_column_homogeneous(GTK_GRID(sliders_grid), FALSE);
  gtk_grid_set_column_spacing(GTK_GRID(sliders_grid), DT_PIXEL_APPLY_DPI(4));
  gtk_grid_set_row_spacing(GTK_GRID(sliders_grid), DT_PIXEL_APPLY_DPI(2));

  GtkWidget *input_lbl = _make_icon_widget(_paint_param_input);
  gtk_widget_set_tooltip_text(input_lbl, _(slider_tooltip[0]));
  dt_gui_add_class(input_lbl, "dt_masks_channel_icon");
  gtk_grid_attach(GTK_GRID(sliders_grid), input_lbl, 0, 0, 1, 1);
  ed->input_lbl = input_lbl;

  GtkWidget *input_slot = dt_gui_hbox();
  gtk_widget_set_hexpand(input_slot, TRUE);
  gtk_widget_set_valign(GTK_WIDGET(ed->filter[0].slider), GTK_ALIGN_CENTER);
  dt_gui_box_add(input_slot, dt_gui_expand(ed->filter[0].slider));

  GtkWidget *input_bypass_btn =
    _make_param_bypass_btn(_("temporarily disable this input channel"), ed);
  ed->input_bypass_slot = _make_param_bypass_slot(input_bypass_btn);
  ed->input_bypass_btn = input_bypass_btn;

  // an overlay, not a grid column: the slider is a windowed widget, so the eye
  // would otherwise be painted over wherever the two share pixels
  GtkWidget *input_overlay = gtk_overlay_new();
  // the overlay is no_show_all (below), so nothing else shows what is in it
  gtk_widget_show_all(input_slot);
  gtk_container_add(GTK_CONTAINER(input_overlay), input_slot);
  gtk_overlay_add_overlay(GTK_OVERLAY(input_overlay), ed->input_bypass_slot);
  // clicks on the eye box's empty space, with the eye hidden, reach the slider
  gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(input_overlay),
                                       ed->input_bypass_slot, TRUE);
  gtk_widget_set_hexpand(input_overlay, TRUE);
  gtk_grid_attach(GTK_GRID(sliders_grid), input_overlay, 1, 0, 1, 1);
  ed->input_slot = input_overlay;

  GtkWidget *output_lbl = _make_icon_widget(_paint_param_output);
  gtk_widget_set_tooltip_text(output_lbl, _(slider_tooltip[1]));
  dt_gui_add_class(output_lbl, "dt_masks_channel_icon");
  gtk_grid_attach(GTK_GRID(sliders_grid), output_lbl, 0, 1, 1, 1);
  ed->output_lbl = output_lbl;

  GtkWidget *output_slot = dt_gui_hbox();
  gtk_widget_set_hexpand(output_slot, TRUE);
  gtk_widget_set_valign(GTK_WIDGET(ed->filter[1].slider), GTK_ALIGN_CENTER);
  dt_gui_box_add(output_slot, dt_gui_expand(ed->filter[1].slider));

  GtkWidget *output_bypass_btn =
    _make_param_bypass_btn(_("temporarily disable this output channel"), ed);
  ed->output_bypass_slot = _make_param_bypass_slot(output_bypass_btn);
  ed->output_bypass_btn = output_bypass_btn;

  // an overlay, not a grid column: the slider is a windowed widget, so the eye
  // would otherwise be painted over wherever the two share pixels
  GtkWidget *output_overlay = gtk_overlay_new();
  // the overlay is no_show_all (below), so nothing else shows what is in it
  gtk_widget_show_all(output_slot);
  gtk_container_add(GTK_CONTAINER(output_overlay), output_slot);
  gtk_overlay_add_overlay(GTK_OVERLAY(output_overlay), ed->output_bypass_slot);
  // clicks on the eye box's empty space, with the eye hidden, reach the slider
  gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(output_overlay),
                                       ed->output_bypass_slot, TRUE);
  gtk_widget_set_hexpand(output_overlay, TRUE);
  gtk_grid_attach(GTK_GRID(sliders_grid), output_overlay, 1, 1, 1, 1);
  ed->output_slot = output_overlay;

  ed->sliders_grid = sliders_grid;

  // opacity leads the expanded controls, matching where a shape row's own
  // opacity slider sits in its props editor (see _build_props_row_editor,
  // where DT_MASKS_PROPERTY_OPACITY is the first property in the table)
  GtkWidget *wrap = dt_gui_vbox(ed->opacity_box, sliders_grid, ed->boost_box);
  dt_gui_add_class(wrap, "dt_masks_card");
  dt_gui_add_class(wrap, "dt_masks_param_card");

  _update_param_row_display(ed);
  g_object_set_data_full(G_OBJECT(wrap), "param-editor", ed, g_free);

  GtkWidget *fit_on_style[] = { GTK_WIDGET(ed->filter[0].slider), ed->input_bypass_slot,
                                GTK_WIDGET(ed->filter[1].slider), ed->output_bypass_slot };
  for(int i = 0; i < G_N_ELEMENTS(fit_on_style); i++)
    g_signal_connect_object(G_OBJECT(fit_on_style[i]), "style-updated",
                            G_CALLBACK(_param_slider_style_updated), wrap, 0);

  gtk_widget_show_all(wrap);
  gtk_widget_set_no_show_all(ed->input_lbl, TRUE);
  gtk_widget_set_no_show_all(ed->input_slot, TRUE);
  gtk_widget_set_no_show_all(ed->input_bypass_slot, TRUE);
  gtk_widget_set_no_show_all(ed->input_bypass_btn, TRUE);
  gtk_widget_set_no_show_all(ed->output_lbl, TRUE);
  gtk_widget_set_no_show_all(ed->output_slot, TRUE);
  gtk_widget_set_no_show_all(ed->output_bypass_slot, TRUE);
  gtk_widget_set_no_show_all(ed->output_bypass_btn, TRUE);
  gtk_widget_set_no_show_all(ed->boost_box, TRUE);
  gtk_widget_set_no_show_all(ed->opacity_box, TRUE);
  _update_param_row_visibility(ed);
  // the slider's range and visibility, as _props_row_populate sets them,
  // after the show_all above, which would undo a hide
  {
    GList *ids = g_list_prepend(NULL, GINT_TO_POINTER(ed->formid));
    _props_row_apply(module, ids, DT_MASKS_PROPERTY_OPACITY, ed->opacity_slider,
                     &ed->opacity_last_value, TRUE);
    g_list_free(ids);
  }
  // _props_row_apply sets the value under DT_IN_GUI_UPDATE, which emits no
  // "value-changed", so the tooltip is set here
  _inline_opacity_tooltip_changed(ed->opacity_slider, NULL);
  if(picker_box_out) *picker_box_out = picker_box;
  return wrap;
}

// make `w` take a click as the element's row header does, with the same two
// handlers and the three keys they read: the row's event box and its
// editors. Each is a windowed widget of its own (row_vbox has no window), and
// a click none takes reaches the group's block, which selects the group
static void _wire_element_click_surface(GtkWidget *w,
                                        dt_iop_module_t *module,
                                        const dt_mask_id_t fid,
                                        GtkWidget *handle,
                                        GtkWidget *name_evbox)
{
  // _row_click_press/_release read all three off `w`: the id to act on, the
  // handle the right-click actions menu anchors to, and the entry ctrl+click
  // rename swaps in. Neither uses the event's coordinates, so it does not
  // matter that these surfaces have different origins.
  g_object_set_data(G_OBJECT(w), "formid", GINT_TO_POINTER(fid));
  g_object_set_data(G_OBJECT(w), "handle-widget", handle);
  g_object_set_data(G_OBJECT(w), "name-evbox", name_evbox);
  GtkGestureSingle *gesture = dt_gui_connect_click(w, _row_click_press, NULL, module);
  g_signal_connect(gesture, "released", G_CALLBACK(_row_click_release), module);
}

// a nested group's body, around its group: pressing there is not
// the start of a drag, and releasing selects the nested group's element, but
// never deselects it. Only events on the body's own window count, as for a
// group's block (see _event_on_own_window): its groups' clicks bubble up here
static void _subgroup_body_press(GtkGestureSingle *gesture,
                                 const int n_press,
                                 const double x,
                                 const double y,
                                 dt_iop_module_t *module)
{
  if(_event_on_own_window(gesture)
     && gtk_gesture_single_get_current_button(gesture) == GDK_BUTTON_PRIMARY)
    dt_gui_claim(gesture);
}

static void _subgroup_body_release(GtkGestureSingle *gesture,
                                   const int n_press,
                                   const double x,
                                   const double y,
                                   dt_iop_module_t *module)
{
  if(!_event_on_own_window(gesture)
     || gtk_gesture_single_get_current_button(gesture) != GDK_BUTTON_PRIMARY)
    return;
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  const dt_mask_id_t fid =
    GPOINTER_TO_INT(g_object_get_data(G_OBJECT(dt_gui_get_widget(gesture)), "formid"));
  if(bd && bd->panel_selected_formid != fid) _set_form_target(module, fid);
}

// an element row's header line squares off onto its editor's rail while any
// of its editors shows (.dt_masks_open in darktable.css), as a group's header
// does onto its elements' rail (see _sync_group_open)
static void _sync_element_open(GtkWidget *editor, GParamSpec *pspec, gpointer row_vbox)
{
  static const char *const keys[] = { "param-editor-box", "props-editor-box",
                                      "subgroup-box" };
  gboolean open = FALSE;
  for(size_t i = 0; i < G_N_ELEMENTS(keys); i++)
  {
    GtkWidget *e = g_object_get_data(G_OBJECT(row_vbox), keys[i]);
    if(e && gtk_widget_get_visible(e)) open = TRUE;
  }
  if(open)
    dt_gui_add_class(_row_header_line(row_vbox), "dt_masks_open");
  else
    dt_gui_remove_class(_row_header_line(row_vbox), "dt_masks_open");
}

// build one element row: the header line (handle, name, and the drawer of
// warning badge, kind icon or picker, visibility and expander), wrapped in an
// event box that drives the canvas hover, and under it whatever the row
// expands to (a parametric row's sliders, a shape's properties, a nested
// group's groups). Returns the row's vertical container, which carries the
// selection highlight
static GtkWidget *_make_shape_row(dt_iop_module_t *module,
                                  dt_masks_point_group_t *fpt,
                                  dt_masks_form_t *form)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  const dt_mask_id_t fid = fpt->formid;
  GtkWidget *row = dt_gui_hbox();

  // column 0 -- drag handle (the reliable drag source for moving the shape onto
  // another group), showing this shape's own kind icon (circle/path/...), or,
  // for a parametric row, its channel code (e.g. "hz", "Cz") instead. One slot
  // doubling as the drag affordance and the "what kind is this" indicator
  // either way (see _make_drag_handle / _make_channel_handle).
  const guint kind = _form_kind(form);
  const gchar *channel_code =
    (form->type & DT_MASKS_PARAMETRIC) ? dt_masks_parametric_type_label(form) : NULL;
  // a member that is a nested group shows its group under its row, and
  // so does the AI object stepped into on the canvas: it shows as the group it
  // is, its paths as rows of that group, edited one by one as they are there
  const gboolean is_subgroup = (form->type & DT_MASKS_GROUP)
                               || ((form->type & DT_MASKS_OBJECT) && _entered_object() == fid);
  // opacity is never in a header: it is a full slider leading the row's
  // expanded controls, which is what gives a raster row anything to expand
  // (see dt_masks_model_row_is_expandable). A parametric row's own copy of this is
  // applied in dt_masks_model_param_row_visibility, since its slider only appears once
  // the row is actually expanded.
  const gboolean props_subpanel = _props_subpanel();
  const gboolean expandable = dt_masks_model_row_is_expandable(form->type, props_subpanel);
  // the header line alone, which an open row shades like a group's header
  // (see _sync_element_open)
  dt_gui_add_class(row, "dt_masks_header");
  dt_gui_add_class(row, "dt_masks_element_header");
  // one tooltip for the row's three plain click surfaces, which share their
  // handlers (_row_click_press, _row_click_release), built from the gestures
  // this row offers. Freed after the last of them (row_evbox)
  const gboolean multi_path = _is_multi_path_object(fid);
  const gboolean entered = multi_path && _entered_object() == fid;
  GString *tip = g_string_new(NULL);
  if(entered)
    g_string_append(tip, _("an AI object whose paths are edited one by one, shown below it\n"));
  else if(is_subgroup)
    g_string_append(tip, _("a group inside this group, shown below this row\n"));
  g_string_append(tip, _("click to select, click again to deselect\n"));
  if(multi_path)
    g_string_append(tip, entered ? _("double-click to stop editing its paths\n")
                                 : _("double-click to edit its paths one by one\n"));
  g_string_append(tip, _("ctrl+click to rename\n"));
  // what shift+click toggles: the chevron this row gets below, if any
  if(is_subgroup)
    g_string_append(tip, entered ? _("shift+click to show/hide its paths\n")
                                 : _("shift+click to show/hide the group\n"));
  else if(form->type & DT_MASKS_PARAMETRIC)
    g_string_append(tip, _("shift+click to show/hide this channel's expanded controls\n"));
  else if(expandable)
    g_string_append(tip, (form->type & DT_MASKS_RASTER)
                           ? _("shift+click to show/hide this raster mask's opacity slider\n")
                           : _("shift+click to show/hide its expanded controls\n"));
  g_string_append(tip, _("right-click for its actions: disable, solo, invert, compose,"
                         " rename, delete\n"));
  g_string_append(tip, _("drag to rearrange: drop it onto a group to put it inside, onto"
                         " the group's top or bottom edge to put it beside"));
  gchar *row_tip = g_string_free(tip, FALSE);
  GtkWidget *handle =
    channel_code ? _make_channel_handle(channel_code, row_tip)
                 : _make_drag_handle((form->type & DT_MASKS_GROUP) ? dtgtk_cairo_paint_masks_multi
                                                                   : _kind_icon_paint(kind),
                                     TRUE, row_tip);
  // an inverted element fills its handle (see _update_shape_row_state)
  g_signal_connect(G_OBJECT(handle), "drag-data-get", G_CALLBACK(_masks_row_drag_get),
                   NULL);
  g_signal_connect(G_OBJECT(handle), "drag-begin", G_CALLBACK(_row_drag_begin), module);
  g_signal_connect(G_OBJECT(handle), "drag-end", G_CALLBACK(_masks_drag_end), module);
  g_signal_connect(G_OBJECT(handle), "drag-failed", G_CALLBACK(_masks_drag_failed), module);
  gtk_drag_source_set(handle, GDK_BUTTON1_MASK, _mask_row_dnd, 1, GDK_ACTION_MOVE);

  // the name, without the type prefix the handle already shows (see
  // _form_type_prefix). Gestures as for the handle (_row_click_press)
  gchar *display_name = dt_masks_gui_form_display_name(form);
  GtkWidget *name = gtk_label_new(display_name);
  g_free(display_name);
  gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
  // a label ellipsizes only below its natural width, so that is capped too,
  // or a long name would take its full width from the row
  gtk_label_set_max_width_chars(GTK_LABEL(name), 1);
  // a little breathing room between the lead handle and the name, so the
  // text doesn't sit flush against the handle's own rounded plate
  dt_gui_add_class(name, "dt_masks_row_name");
  GtkWidget *evbox = gtk_event_box_new();
  // the rename gesture (see _start_rename_element) swaps evbox's own child
  // for a GtkEntry in place, so evbox must contain the label alone -- the
  // solo badge is packed as evbox's own sibling in `row` instead (see below).
  gtk_container_add(GTK_CONTAINER(evbox), name);
  // hexpand is set below, once name_expand is known (a parametric row's name
  // must not claim any of the header's free width -- see name_expand).
  gtk_widget_set_tooltip_text(evbox, row_tip);
  // the handle, the name and the row's background are one click surface,
  // each carrying all three tags (self-references included)
  _wire_element_click_surface(handle, module, fid, handle, evbox);
  _wire_element_click_surface(evbox, module, fid, handle, evbox);
  // like the grip handle in column 0, the name is a drag source, so grabbing
  // it starts the same drag (claiming the press leaves the drag source armed;
  // selection happens on release, see _row_click_release). A drop is placed
  // against the whole row, row_vbox below
  g_signal_connect(G_OBJECT(evbox), "drag-data-get", G_CALLBACK(_masks_row_drag_get),
                   NULL);
  gtk_drag_source_set(evbox, GDK_BUTTON1_MASK, _mask_row_dnd, 1, GDK_ACTION_MOVE);
  g_signal_connect(G_OBJECT(evbox), "drag-begin", G_CALLBACK(_row_drag_begin), module);
  g_signal_connect(G_OBJECT(evbox), "drag-end", G_CALLBACK(_masks_drag_end), module);
  g_signal_connect(G_OBJECT(evbox), "drag-failed", G_CALLBACK(_masks_drag_failed), module);

  GtkWidget *param_editor = NULL;
  GtkWidget *param_picker_box = NULL;
  // a drawn shape's or a raster mask's properties editor (see
  // _make_props_row_toggle)
  GtkWidget *props_editor_box = NULL;
  // the row's own expander, whichever widget that is for this row kind: the
  // chevron the row header shows, which shift+click on the lead handle or the
  // title also drives (see _row_click_release, and _auto_expand_selected_row).
  // NULL for a row with nothing to expand: a shape or raster mask while
  // "element properties in subpanel" is on
  GtkWidget *expand_toggle = NULL;
  // a nested group, shown under its row (see _pack_subgroup)
  GtkWidget *subgroup_box = NULL;
  if(is_subgroup)
  {
    // its opacity and inversion apply to its finished mask, as a shape's do
    // to the shape's; its chevron shows its group instead of properties
    subgroup_box = dt_gui_vbox();
    dt_gui_add_class(subgroup_box, "dt_masks_list");
    dt_gui_add_class(subgroup_box, "dt_masks_group_elements");
    // with the subpanel, its opacity slider is there while it is selected
    if(!props_subpanel)
    {
      GtkWidget *ex_op = _build_props_row_editor(module, fid, TRUE);
      dt_gui_add_class(ex_op, "dt_masks_group_card");
      g_object_set_data(G_OBJECT(ex_op), "drag-hide", GINT_TO_POINTER(1));
      dt_gui_box_add(subgroup_box, ex_op);
    }
    _pack_subgroup(module, form, subgroup_box);

    // open unless closed by hand, and always while it holds the selection
    const gboolean holds_selection =
      (dt_is_valid_maskid(bd->panel_selected_formid)
       && _point_node_owner(form, bd->panel_selected_formid, NULL))
      || (dt_is_valid_maskid(bd->panel_selected_group_cid)
          && _point_node_owner(form, bd->panel_selected_group_cid, NULL));
    gpointer stored = NULL;
    const gboolean expanded =
      holds_selection || !bd->masks_props_expanded
      || !g_hash_table_lookup_extended(bd->masks_props_expanded, GINT_TO_POINTER(fid), NULL,
                                       &stored)
      || GPOINTER_TO_INT(stored);
    gtk_widget_set_visible(subgroup_box, expanded);
    expand_toggle = dtgtk_togglebutton_new(_paint_param_inout, 0, NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(expand_toggle), expanded);
    dt_gui_add_class(expand_toggle, "dt_transparent_background");
    dt_gui_add_class(expand_toggle, "dt_masks_icon");
    dt_gui_add_class(expand_toggle, "dt_masks_expander");
    gtk_widget_set_valign(expand_toggle, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(expand_toggle, _("show/hide this group's groups"));
    g_object_set_data(G_OBJECT(expand_toggle), "props-key", GINT_TO_POINTER(fid));
    g_object_set_data(G_OBJECT(expand_toggle), "elem-box", subgroup_box);
    g_signal_connect(G_OBJECT(expand_toggle), "toggled",
                     G_CALLBACK(_subgroup_expand_toggled), module);
  }
  else if(form->type & DT_MASKS_PARAMETRIC)
  {
    // its in/out chevron (see _masks_param_inout_toggled)
    const dt_masks_point_parametric_t *p = form->points ? form->points->data : NULL;
    expand_toggle = dtgtk_togglebutton_new(_paint_param_inout, 0, NULL);
    gtk_widget_set_valign(expand_toggle, GTK_ALIGN_CENTER);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(expand_toggle), p && p->in_out);
    dt_gui_add_class(expand_toggle, "dt_masks_icon");
    dt_gui_add_class(expand_toggle, "dt_masks_expander");
    dt_gui_add_class(expand_toggle, "dt_transparent_background");
    gtk_widget_set_tooltip_text(
      expand_toggle,
      _("show/hide this channel's expanded controls (input and output sliders,"
        " boost factor)"));
    g_object_set_data(G_OBJECT(expand_toggle), "formid", GINT_TO_POINTER(fid));
    g_signal_connect(G_OBJECT(expand_toggle), "toggled",
                     G_CALLBACK(_masks_param_inout_toggled), module);
    // built here (rather than after row_vbox exists, below) so its picker
    // button is ready to pack into the header actions cluster next to the
    // expander -- it is a per-channel control, not part of the slider editor
    param_editor = _build_param_row_editor(module, form, &param_picker_box);
    dt_masks_param_row_editor_t *ped =
      param_editor ? g_object_get_data(G_OBJECT(param_editor), "param-editor") : NULL;

    if(ped)
    {
      ped->name_evbox = evbox;
      _update_param_row_visibility(ped);
    }
  }
  else if(form->type & DT_MASKS_RASTER)
  {
    // opacity is a raster mask's only property (modify_property is NULL for a
    // raster form), so its slider is all it expands to, unless that goes in
    // the properties subpanel
    if(expandable)
      expand_toggle = _make_props_row_toggle(
        module, fid, TRUE, _("show/hide this raster mask's opacity slider"), &props_editor_box);
  }
  else
  {
    // opacity leads the expanded controls, then everything else this shape
    // has (size, hardness, feather, rotation, curvature, compression, cleanup,
    // smoothing, refine-mask-boundary).
    // "element properties in subpanel" shows them there instead, for the
    // selected shape (see _props_panel_sync), so the row has nothing to expand
    if(expandable)
    {
      const char *props_tip =
        (form->type & DT_MASKS_OBJECT)
          ? _("show/hide this object's expanded controls (smoothing, cleanup, etc.)")
          : _("show/hide this shape's expanded controls (size, hardness, etc.)");
      expand_toggle =
        _make_props_row_toggle(module, fid, FALSE, props_tip, &props_editor_box);
    }
  }

  if(expand_toggle)
  {
    g_object_set_data(G_OBJECT(handle), "expand-toggle", expand_toggle);
    g_object_set_data(G_OBJECT(evbox), "expand-toggle", expand_toggle);
  }

  // visibility: disabled, soloed or neither, set by _update_shape_row_state
  // below; a click and a shift+click switch them (see _visibility_form_pressed)
  GtkWidget *visibility = _make_visibility_button(TRUE);
  g_object_set_data(G_OBJECT(visibility), "formid", GINT_TO_POINTER(fid));
  _press_before_widget(dt_gui_connect_click(visibility, _visibility_form_pressed, NULL, module));

  // low-opacity warning: blank unless this element's opacity is under
  // MASK_LOW_OPACITY_WARN. Its initial state is set by the
  // _refresh_lowop_badges call at the end of dt_masks_gui_build_list, once the
  // row is registered in bd->masks_row_map (it isn't yet, here).
  GtkWidget *lowop_badge = _make_lowop_badge();

  // a linked shape carries a chain in the kind icon's column, where a
  // parametric row has its picker: linked across modules, or
  // referenced more than once by this mask itself, both being the same shape
  // shown in two places. Parametric channels are copied, never linked (see
  // dt_masks_group_add_members_of); a raster element's chain leads to its
  // source instead (see _make_raster_source_link)
  GtkWidget *linked_slot = NULL;
  const gboolean shape = !(form->type & (DT_MASKS_PARAMETRIC | DT_MASKS_RASTER));
  const int uses_here = shape ? _model_form_uses_in_mask(module, fid) : 0;
  if(shape && (dt_masks_model_form_is_linked(form) || uses_here > 1))
  {
    gchar *linked_tip = _linked_tooltip(module, fid, form, uses_here);
    if(linked_tip)
    {
      GtkWidget *chain = _make_linked_link(module, fid, linked_tip);
      g_free(linked_tip);
      gtk_widget_show(chain);
      linked_slot = _link_action_slot(chain);
    }
  }

  GtkWidget *action_icon = (form->type & DT_MASKS_PARAMETRIC) ? param_picker_box
                           : (form->type & DT_MASKS_RASTER)   ? _make_raster_source_link(form)
                                                              : linked_slot;
  _pack_row_header(row, handle, evbox, lowop_badge, action_icon, FALSE,
                   visibility, expand_toggle);

  // an event box around the row drives the canvas hover feedback (labels +
  // highlight), which hovering it limits to this shape. Its real window
  // reports crossings into the row's own buttons as GDK_NOTIFY_INFERIOR, so
  // the hover stays active over the whole row. A click landing in a gap
  // between the row's controls acts as a click on the handle or name
  GtkWidget *row_evbox = _element_hover_box(row, module, fid);
  _wire_element_click_surface(row_evbox, module, fid, handle, evbox);
  gtk_widget_set_tooltip_text(row_evbox, row_tip);
  g_free(row_tip); // last of the three widgets that needed it (handle, evbox, row_evbox)

  // each row gets its own vertical container so its editors can be docked
  // directly underneath it (see below). The border highlight is on this box
  // (a GtkBox renders its CSS frame reliably; a GtkEventBox does not) and
  // carries the form id so _update_row_selection can find it without a
  // rebuild.
  GtkWidget *row_vbox = dt_gui_vbox();
  // every element kind (drawn shape, parametric, raster) goes through this
  // same function. Its state classes go on its header line (see
  // _row_header_line), which is what they shade
  dt_gui_add_class(row_vbox, "dt_masks_row");
  dt_gui_box_add(row_vbox, row_evbox);
  // what an insertion line above the element is drawn on (see
  // _drop_line_paint_widget)
  g_object_set_data(G_OBJECT(row_vbox), "drop-header", row);
  g_object_set_data(G_OBJECT(row_evbox), "row-vbox", row_vbox);
  g_object_set_data(G_OBJECT(evbox), "row-vbox", row_vbox);
  g_object_set_data(G_OBJECT(handle), "row-vbox", row_vbox);
  // a drop is placed against the whole element, header line and whatever it
  // shows under it (editors, a nested group's groups), so its halves are the
  // element's: a drop on its body lands beside it, never in the group behind
  g_object_set_data(G_OBJECT(row_vbox), "drop-item", GINT_TO_POINTER(1));
  _set_drop_target(row_vbox);

  g_object_set_data(G_OBJECT(row_vbox), "mask-row", GINT_TO_POINTER(1));
  g_object_set_data(G_OBJECT(row_vbox), "formid", GINT_TO_POINTER(fid));
  // which reference this row is: a mask can hold the same shape in two groups,
  // and then form id alone no longer identifies a row (see _masks_row_for_point)
  g_object_set_data(G_OBJECT(row_vbox), "row-point", fpt);
  // index this row for O(1) lookup by form id (see _masks_row_widget); the map is
  // cleared at the top of dt_masks_gui_build_list, so entries never outlive their widget.
  // One entry per form holds every row built for it, in build order. The list is
  // stolen and re-inserted rather than looked up and updated in place: the map
  // frees the list it holds, and inserting over a head it already stores would
  // free the very list this row was just appended to.
  if(bd->masks_row_map)
  {
    GSList *rows = g_hash_table_lookup(bd->masks_row_map, GINT_TO_POINTER(fid));
    g_hash_table_steal(bd->masks_row_map, GINT_TO_POINTER(fid));
    rows = g_slist_append(rows, row_vbox);
    g_hash_table_insert(bd->masks_row_map, GINT_TO_POINTER(fid), rows);
  }
  // tag the row's own interactive widgets so _update_shape_row_state can refresh
  // them in place (toggle states, opacity) without a full list rebuild.
  g_object_set_data(G_OBJECT(row_vbox), "row-hbox", row);
  g_object_set_data(G_OBJECT(row_vbox), "handle-widget", handle);
  g_object_set_data(G_OBJECT(row_vbox), "name-evbox", evbox);
  if(action_icon) g_object_set_data(G_OBJECT(row_vbox), "action-icon", action_icon);
  g_object_set_data(G_OBJECT(row_vbox), "visibility-btn", visibility);
  g_object_set_data(G_OBJECT(row_vbox), "lowop-badge", lowop_badge);
  if(expand_toggle) g_object_set_data(G_OBJECT(row_vbox), "expand-toggle", expand_toggle);
  // as "param-editor-box" below, for _update_shape_row_state to make the
  // editor insensitive while the row is solo-suppressed
  if(props_editor_box)
    g_object_set_data(G_OBJECT(row_vbox), "props-editor-box", props_editor_box);
  if(dt_is_valid_maskid(bd->panel_selected_formid) && fid == bd->panel_selected_formid)
    dt_gui_add_class(row, "dt_masks_selected");

  // every parametric mask row gets its own permanently visible slider editor
  // (see _build_param_row_editor) -- no expand/collapse or docking needed
  if(param_editor)
  {
    // inset via CSS (.dt_masks_param_card in darktable.css). Hovering any of
    // its sliders or pickers drives the row's hover too, and clicking its
    // background selects this element, not its group
    GtkWidget *param_evbox = _element_hover_box(param_editor, module, fid);
    _wire_element_click_surface(param_evbox, module, fid, handle, evbox);
    g_object_set_data(G_OBJECT(param_evbox), "drag-hide", GINT_TO_POINTER(1));
    dt_gui_box_add(row_vbox, param_evbox);
    // the editor itself, not its hover wrapper: _build_param_row_editor
    // attached the "param-editor" data to this exact widget
    g_object_set_data(G_OBJECT(row_vbox), "param-editor-box", param_editor);
  }

  // shape/raster rows' own properties editor, docked and hover-wrapped the
  // same way the parametric editor above is (see _make_props_row_toggle for
  // the toggle that shows/hides it).
  if(props_editor_box)
  {
    // inset via CSS (.dt_masks_props_card in darktable.css)
    GtkWidget *props_evbox = _element_hover_box(props_editor_box, module, fid);
    _wire_element_click_surface(props_evbox, module, fid, handle, evbox);
    g_object_set_data(G_OBJECT(props_evbox), "drag-hide", GINT_TO_POINTER(1));
    dt_gui_box_add(row_vbox, props_evbox);
  }

  // indented under the row by the rail every group's elements have (see
  // .dt_masks_group_elements in darktable.css). Its groups are selected, dropped
  // onto and dimmed through their own headers, as the top list's are
  if(subgroup_box)
  {
    // its own window, so a click around its group stays inside the element
    // instead of selecting the group holding it, which would step out of the AI
    // object stepped into (see _set_group_target_ext)
    GtkWidget *sub_evbox = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(sub_evbox), TRUE);
    gtk_container_add(GTK_CONTAINER(sub_evbox), subgroup_box);
    g_object_set_data(G_OBJECT(sub_evbox), "formid", GINT_TO_POINTER(fid));
    GtkGestureSingle *sub_gesture = dt_gui_connect_click(sub_evbox, _subgroup_body_press, NULL,
                                                         module);
    g_signal_connect(sub_gesture, "released", G_CALLBACK(_subgroup_body_release), module);
    g_object_bind_property(subgroup_box, "visible", sub_evbox, "visible",
                           G_BINDING_SYNC_CREATE);
    dt_gui_box_add(row_vbox, sub_evbox);
    g_object_set_data(G_OBJECT(row_vbox), "subgroup-box", subgroup_box);
  }

  // disconnected with row_vbox, which is destroyed together with its editors
  GtkWidget *const editors[] = { param_editor, props_editor_box, subgroup_box };
  for(size_t i = 0; i < G_N_ELEMENTS(editors); i++)
    if(editors[i])
      g_signal_connect_object(editors[i], "notify::visible",
                              G_CALLBACK(_sync_element_open), row_vbox, 0);
  _sync_element_open(NULL, NULL, row_vbox);

  // the state every later refresh paints, so a built row cannot differ
  _update_shape_row_state(bd, row_vbox, fpt);
  return row_vbox;
}

// order-independent fold of a {key -> flag} GHashTable into an accumulator, so
// its contribution to the signature does not depend on GHashTable iteration
// order (which is unspecified).
static guint64 _fold_flag_table(GHashTable *t)
{
  if(!t) return 0;
  guint64 acc = 1469598103934665603ULL; // FNV offset basis, just a seed
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, t);
  while(g_hash_table_iter_next(&it, &k, &v))
    acc += (guint64)GPOINTER_TO_INT(k) * 2654435761u + (guint64)(GPOINTER_TO_INT(v) != 0);
  return acc;
}

// a hash of everything dt_masks_gui_build_list builds the tree from: the mask model
// (dt_masks_group_hash already folds every point's formid/state/opacity/
// refinement in order plus each leaf form's own config, so add/delete/reorder/
// operator/opacity/refinement/solo-via-HIDDEN and parametric config all move it)
// plus the UI-state the build consults (mask mode, selection/solo,
// cluster/props expansion). When this is unchanged since the last build the
// rebuilt tree would be byte-identical, so the whole teardown/rebuild can be
// skipped.
dt_hash_t dt_masks_gui_list_signature(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);

  dt_hash_t sig = dt_masks_group_hash(DT_INITHASH, grp);

  // dt_masks_group_hash does not fold form->name or orphan point states, but
  // the rows display them, so fold each member form's state and name in points
  // order, nested groups' members included
  GList *pts = _mask_points(grp);
  for(GList *p = pts; p; p = g_list_next(p))
  {
    const dt_masks_point_group_t *pt = p->data;
    sig = dt_hash(sig, &pt->formid, sizeof(pt->formid));
    sig = dt_hash(sig, &pt->state, sizeof(pt->state));
    sig = dt_hash(sig, &pt->group_opacity, sizeof(pt->group_opacity));
    sig = dt_hash(sig, &pt->opacity, sizeof(pt->opacity));
    sig = dt_hash(sig, &pt->refinement, sizeof(pt->refinement));
    const dt_masks_form_t *f = dt_masks_get_from_id(darktable.develop, pt->formid);
    if(f)
    {
      // what the row shows, which for a raster element can be its source's name
      gchar *shown = dt_masks_gui_form_display_name(f);
      sig = dt_hash(sig, shown, strlen(shown));
      g_free(shown);
    }
    if(pt->name[0]) sig = dt_hash(sig, pt->name, strlen(pt->name));
    // which preset notes a group shows under its header
    if(pt->preset_note[0]) sig = dt_hash(sig, pt->preset_note, strlen(pt->preset_note));
    // the chain icon and its tooltip follow which modules share the form,
    // which another module changes without touching this group
    GList *users = dt_masks_model_form_users(pt->formid);
    for(GList *u = users; u; u = g_list_next(u)) sig = dt_hash(sig, &u->data, sizeof(u->data));
    g_list_free(users);
  }
  g_list_free(pts);

  const uint32_t mode = module->blend_params->mask_mode;
  sig = dt_hash(sig, &mode, sizeof(mode));

  // selection / solo (drive per-row/per-header CSS classes and badges), and
  // the AI object stepped into, whose row then shows as its group (see
  // _make_shape_row): stepping in or out changes nothing else hashed here
  const int32_t ui[6] = {
    bd->panel_selected_formid,   bd->panel_selected_group_cid, bd->solo_formid,
    (int32_t)bd->solo_group_key, bd->soloedit_formid,          _entered_object()
  };
  sig = dt_hash(sig, ui, sizeof(ui));

  // cluster / props expansion (each drives a revealer's initial state)
  const guint64 folds[2] = { _fold_flag_table(bd->masks_cluster_expanded),
                             _fold_flag_table(bd->masks_props_expanded) };
  sig = dt_hash(sig, folds, sizeof(folds));

  // the shape being drawn for this module, which is not in grp->points: its
  // row comes and goes with it. Its type is enough, as the pending row's
  // sliders update in place (dt_iop_gui_blend_sync_pending_ai_sliders). What
  // the list shows must be in this hash, or a rebuild is skipped
  {
    const dt_masks_form_gui_t *fg = darktable.develop->form_gui;
    const dt_masks_form_t *pending = (fg && fg->creation && fg->creation_module == module)
                                       ? darktable.develop->form_visible
                                       : NULL;
    const int32_t pending_type = pending ? (int32_t)pending->type : -1;
    sig = dt_hash(sig, &pending_type, sizeof(pending_type));
  }

  return sig;
}

// settle the model the panel shows before a widget is built: drop empty AI
// objects and dangling members, renumber groups, drop stale solo state, fix
// the selection. Touches no widgets. FALSE when there is nothing to render
static gboolean _masks_panel_reconcile(dt_iop_module_t *module,
                                       dt_masks_form_t *grp,
                                       const gboolean flexi)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;

  // an AI object left without paths (by an earlier cleanup, or a path form
  // lost otherwise) is never shown or edited again: drop it, and any member
  // whose form is gone (see dt_masks_model_prune_dangling_members). Both
  // render as before, so no history item is needed; the next one carries the
  // change
  if(flexi && darktable.develop)
  {
    dt_pthread_mutex_lock(&darktable.develop->history_mutex);
    const int pruned = dt_masks_prune_empty_objects(&darktable.develop->forms);
    const int dangling = dt_masks_model_prune_dangling_members(grp);
    dt_pthread_mutex_unlock(&darktable.develop->history_mutex);
    if(pruned) _queue_link_peers_rebuild(module);
    if(dangling)
      dt_print(DT_DEBUG_MASKS, "[masks] '%s': dropped %d member(s) whose form is gone",
               module->op, dangling);
  }

  // group numbers are identities, not positions: forget the ones whose group is
  // gone, then number any group still without one (see _group_ordinal_any).
  // Before any packing, so every header and caption below reads the same,
  // already-settled number.
  if(flexi)
  {
    dt_masks_gui_prune_group_ordinals(module);
    _assign_group_ordinals(module);
  }
  dt_masks_gui_prune_stale_solo(module);

  // one line per rebuild describing what the panel is about to render from, so a
  // panel that goes blank/empty can be told apart from a mask that really lost
  // its content
  dt_print(DT_DEBUG_MASKS,
           "[masks] panel rebuild '%s': mask_id=%d grp=%s points=%d flexi=%d",
           module->op, module->blend_params->mask_id, grp ? "ok" : "NULL",
           grp ? g_list_length(grp->points) : -1, flexi ? 1 : 0);
  // a flexi mask always shows a group: its own, or with no group form yet the
  // one it will have (see dt_masks_gui_module_flexi_group)
  if(!flexi) return FALSE;

  // one group is always selected: a selection whose group is gone (deleted,
  // undone) falls back to the mask's own, as a deselect does. The
  // mask's own group then targets the whole mask's refinement (see
  // dt_masks_model_refine_scope_from_selection)
  if(dt_is_valid_maskid(bd->panel_selected_group_cid)
     && !dt_masks_gui_group_point(grp, bd->panel_selected_group_cid))
    bd->panel_selected_group_cid = INVALID_MASKID;
  _select_mask_group_if_none(bd);

  // refresh the insert hint now (not just at the very end, its other call
  // site) so it reflects any selection change made just above, in time for
  // the pending-row placement below to target the right group.
  // Idempotent/side-effect-free to call twice in one pass.
  _recompute_insert_hint(module);
  return TRUE;
}

// a group header squares its bottom-left corner onto the rail below it while
// its elements show (.dt_masks_open in darktable.css). Every route that
// opens or closes a group shows or hides its element box, so following that
// box covers them all
static void _sync_group_open(GtkWidget *elem_box, GParamSpec *pspec, gpointer hdr)
{
  GList *kids = gtk_container_get_children(GTK_CONTAINER(elem_box));
  if(gtk_widget_get_visible(elem_box) && kids)
    dt_gui_add_class(GTK_WIDGET(hdr), "dt_masks_open");
  else
    dt_gui_remove_class(GTK_WIDGET(hdr), "dt_masks_open");
  g_list_free(kids);
}

// a page dot: filled, dimmed by the button's opacity unless its page shows
static void _paint_note_dot(cairo_t *cr,
                            const gint x,
                            const gint y,
                            const gint w,
                            const gint h,
                            const gint flags,
                            void *data)
{
  cairo_arc(cr, x + w / 2.0, y + h / 2.0, MIN(w, h) * 0.2, 0, 2 * M_PI);
  cairo_fill(cr);
}

// show page `page` of a note: its stack child, which dot is lit, and whether
// there is a page before or after it
static void _group_note_show_page(GtkWidget *note, const int page)
{
  GtkWidget *stack = g_object_get_data(G_OBJECT(note), "note-stack");
  gchar *name = g_strdup_printf("%d", page);
  gtk_stack_set_visible_child_name(GTK_STACK(stack), name);
  g_free(name);
  g_object_set_data(G_OBJECT(note), "page", GINT_TO_POINTER(page));
  GList *dots = g_object_get_data(G_OBJECT(note), "note-dots");
  int i = 0;
  for(GList *d = dots; d; d = g_list_next(d), i++)
    gtk_widget_set_opacity(GTK_WIDGET(d->data), i == page ? 1.0 : 0.35);
  GtkWidget *prev = g_object_get_data(G_OBJECT(note), "note-prev");
  GtkWidget *next = g_object_get_data(G_OBJECT(note), "note-next");
  if(prev) gtk_widget_set_sensitive(prev, page > 0);
  if(next) gtk_widget_set_sensitive(next, page < i - 1);
}

// every way of changing page ends here: the page is remembered per group, so
// a list rebuild keeps it
static void _group_note_set_page(dt_iop_module_t *module, GtkWidget *note, const int page)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  const int n = g_list_length(g_object_get_data(G_OBJECT(note), "note-dots"));
  if(page < 0 || page >= n) return;
  const dt_mask_id_t cid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(note), "group-key"));
  if(!bd->masks_note_page) bd->masks_note_page = g_hash_table_new(g_direct_hash, g_direct_equal);
  g_hash_table_insert(bd->masks_note_page, GINT_TO_POINTER(cid), GINT_TO_POINTER(page));
  _group_note_show_page(note, page);
}

// a dot switches the note to its page
static void _group_note_dot_clicked(GtkButton *dot, dt_iop_module_t *module)
{
  _group_note_set_page(module, g_object_get_data(G_OBJECT(dot), "note"),
                       GPOINTER_TO_INT(g_object_get_data(G_OBJECT(dot), "page")));
}

// the arrows either side of the dots step one page back or on ("step": -1, 1)
static void _group_note_step_clicked(GtkButton *arrow, dt_iop_module_t *module)
{
  GtkWidget *note = g_object_get_data(G_OBJECT(arrow), "note");
  const int step = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(arrow), "step"));
  _group_note_set_page(module, note,
                       GPOINTER_TO_INT(g_object_get_data(G_OBJECT(note), "page")) + step);
}

static GtkWidget *_make_group_note_arrow(dt_iop_module_t *module,
                                         GtkWidget *note,
                                         const int step)
{
  GtkWidget *arrow = dtgtk_button_new(dtgtk_cairo_paint_solid_arrow,
                                      step < 0 ? CPF_DIRECTION_LEFT : 0, NULL);
  gtk_widget_set_tooltip_text(arrow, step < 0 ? _("previous page") : _("next page"));
  g_object_set_data(G_OBJECT(arrow), "note", note);
  g_object_set_data(G_OBJECT(arrow), "step", GINT_TO_POINTER(step));
  g_signal_connect(G_OBJECT(arrow), "clicked", G_CALLBACK(_group_note_step_clicked), module);
  return arrow;
}

// one page of a note, as Pango markup. A page that does not parse, which a
// translation can break too, is shown as the plain text it is rather than as
// nothing, which is what GTK makes of broken markup
static GtkWidget *_group_note_label(const char *text)
{
  GtkWidget *label = gtk_label_new(NULL);
  if(pango_parse_markup(text, -1, 0, NULL, NULL, NULL, NULL))
    gtk_label_set_markup(GTK_LABEL(label), text);
  else
    gtk_label_set_text(GTK_LABEL(label), text);
  return label;
}

// which preset notes are open. A note switched on or off with the info icon
// by its group's name stays that way. Otherwise every note is open right
// after a preset is applied, to show what the layout is for, and from the
// first selection of another group on only the note of the group new elements
// go to (the selected one, or the one holding the selected element)
static gboolean _group_note_is_open(dt_iop_gui_blend_data_t *bd, GtkWidget *note)
{
  const dt_mask_id_t cid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(note), "group-key"));
  const int by_hand = bd->masks_note_open
    ? GPOINTER_TO_INT(g_hash_table_lookup(bd->masks_note_open, GINT_TO_POINTER(cid)))
    : 0;
  if(by_hand) return by_hand == 1;
  return bd->masks_notes_all_open || cid == bd->panel_selected_group_cid;
}

// show or fold one note, and restyle what it touches: the header squares its
// bottom edge onto the note (unless the opacity slider below keeps it
// squared anyway), and an empty group's card holds nothing else to show
static void _sync_group_note(dt_iop_gui_blend_data_t *bd, GtkWidget *note)
{
  const gboolean open = _group_note_is_open(bd, note);
  gtk_widget_set_visible(note, open);

  GtkWidget *toggle = g_object_get_data(G_OBJECT(note), "note-toggle");
  if(toggle)
  {
    DT_ENTER_GUI_UPDATE();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), open);
    DT_LEAVE_GUI_UPDATE();
  }

  const gboolean joined = _has_class(note, "dt_masks_joined");
  GtkWidget *hdr = g_object_get_data(G_OBJECT(note), "note-hdr");
  if(hdr && !joined)
  {
    if(open)
      dt_gui_add_class(hdr, "dt_masks_has_card");
    else
      dt_gui_remove_class(hdr, "dt_masks_has_card");
  }

  GtkWidget *card = gtk_widget_get_parent(note);
  if(card && g_object_get_data(G_OBJECT(note), "note-empty"))
  {
    gboolean others = FALSE;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(card));
    for(GList *k = kids; k; k = g_list_next(k))
      if(k->data != note && gtk_widget_get_visible(k->data)) others = TRUE;
    g_list_free(kids);
    // no_show_all, or the list's show_all brings a folded card back
    gtk_widget_set_no_show_all(card, !open && !others);
    gtk_widget_set_visible(card, open || others);
  }
}

static void _sync_group_notes_in(dt_iop_gui_blend_data_t *bd, GtkWidget *w)
{
  if(_has_class(w, "dt_masks_note"))
  {
    _sync_group_note(bd, w);
    return;
  }
  if(!GTK_IS_CONTAINER(w)) return;
  GList *kids = gtk_container_get_children(GTK_CONTAINER(w));
  for(GList *k = kids; k; k = g_list_next(k)) _sync_group_notes_in(bd, k->data);
  g_list_free(kids);
}

// selection moves without a rebuild, so it reopens the notes in place
static void _sync_group_notes(dt_iop_gui_blend_data_t *bd)
{
  if(bd && bd->masks_list_box) _sync_group_notes_in(bd, GTK_WIDGET(bd->masks_list_box));
}

// the info icon in a group's header switches its note on or off, and only that:
// the header does not see its click, which the toggle claims
static void _group_note_toggled(GtkToggleButton *toggle, dt_iop_module_t *module)
{
  if(DT_IN_GUI_UPDATE()) return;
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  GtkWidget *note = g_object_get_data(G_OBJECT(toggle), "note");
  if(!note) return;
  const dt_mask_id_t cid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(note), "group-key"));
  if(!bd->masks_note_open) bd->masks_note_open = g_hash_table_new(g_direct_hash, g_direct_equal);
  g_hash_table_insert(bd->masks_note_open, GINT_TO_POINTER(cid),
                      GINT_TO_POINTER(gtk_toggle_button_get_active(toggle) ? 1 : 2));
  _sync_group_note(bd, note);
}

// a preset group's note: its pages (untranslated, from the presets file) in a
// stack, one at a time, and a row of dots between a previous and a next arrow
// to move between them when there is more than one. The stack is as tall as
// the longest page, so the navigation stays put as the pages change
static GtkWidget *_make_group_note(dt_iop_module_t *module,
                                   const dt_mask_id_t cid,
                                   GPtrArray *pages)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  GtkWidget *note = dt_gui_vbox();
  dt_gui_add_class(note, "dt_masks_card");
  dt_gui_add_class(note, "dt_masks_note");
  g_object_set_data(G_OBJECT(note), "group-key", GINT_TO_POINTER(cid));

  GtkWidget *stack = gtk_stack_new();
  gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_NONE);
  for(guint i = 0; i < pages->len; i++)
  {
    GtkWidget *label = _group_note_label(_((const char *)g_ptr_array_index(pages, i)));
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_yalign(GTK_LABEL(label), 0.0f);
    // wraps to whatever width the panel has instead of asking for its own
    gtk_label_set_max_width_chars(GTK_LABEL(label), 1);
    gchar *name = g_strdup_printf("%u", i);
    gtk_stack_add_named(GTK_STACK(stack), label, name);
    g_free(name);
  }
  g_object_set_data(G_OBJECT(note), "note-stack", stack);

  dt_gui_box_add(note, stack);

  int page = 0;
  if(pages->len > 1)
  {
    GtkWidget *dots = dt_gui_hbox();
    dt_gui_add_class(dots, "dt_masks_note_dots");
    gtk_widget_set_halign(dots, GTK_ALIGN_CENTER);
    GtkWidget *prev = _make_group_note_arrow(module, note, -1);
    g_object_set_data(G_OBJECT(note), "note-prev", prev);
    dt_gui_box_add(dots, prev);
    GList *dot_list = NULL;
    for(guint i = 0; i < pages->len; i++)
    {
      GtkWidget *dot = dtgtk_button_new(_paint_note_dot, 0, NULL);
      gchar *tip = g_strdup_printf(_("page %u of %u"), i + 1, pages->len);
      gtk_widget_set_tooltip_text(dot, tip);
      g_free(tip);
      g_object_set_data(G_OBJECT(dot), "note", note);
      g_object_set_data(G_OBJECT(dot), "page", GINT_TO_POINTER(i));
      g_signal_connect(G_OBJECT(dot), "clicked", G_CALLBACK(_group_note_dot_clicked), module);
      dt_gui_box_add(dots, dot);
      dot_list = g_list_append(dot_list, dot);
    }
    g_object_set_data_full(G_OBJECT(note), "note-dots", dot_list, (GDestroyNotify)g_list_free);
    GtkWidget *next = _make_group_note_arrow(module, note, 1);
    g_object_set_data(G_OBJECT(note), "note-next", next);
    dt_gui_box_add(dots, next);
    dt_gui_box_add(note, dots);
    if(bd->masks_note_page)
      page = GPOINTER_TO_INT(g_hash_table_lookup(bd->masks_note_page, GINT_TO_POINTER(cid)));
    // the presets file may have lost pages since
    if(page >= (int)pages->len) page = 0;
  }
  // a stack only selects a visible child, and the list's own show_all comes
  // after this
  gtk_widget_show_all(stack);
  _group_note_show_page(note, page);
  // whether it shows is _sync_group_note's call, not the list's show_all
  gtk_widget_show_all(note);
  gtk_widget_set_no_show_all(note, TRUE);
  return note;
}

// one group of the panel: its header, and its element rows nested under it.
// `grp` is the group's form, NULL for a mask with no group form yet, `marker`
// holds the group's settings, and its members start at `first`
static void _pack_group(dt_iop_module_t *module,
                        dt_masks_form_t *grp,
                        const dt_masks_point_group_t *marker,
                        GList *first,
                        dt_masks_form_t *pending_form,
                        GtkWidget *container)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  const guint cid = (guint)marker->formid;
  // the group's resolvable member ids, top-first (g_list_prepend)
  GList *formids = NULL;
  int n = 0;
  for(GList *m = first; m; m = g_list_next(m))
  {
    dt_masks_point_group_t *pm = m->data;
    if(!dt_masks_get_from_id(darktable.develop, pm->formid))
    {
      // a member absent from dev->forms has no row, though the pipe's copy may
      // still render it: logged, so that the missing row can be explained
      dt_print(
        DT_DEBUG_MASKS,
        "[masks] panel: group member %d of '%s' not in dev->forms -- row dropped",
        pm->formid, module->op);
      continue;
    }
    formids = g_list_prepend(formids, GINT_TO_POINTER(pm->formid));
    n++;
  }
  const gboolean empty = n == 0;
  const dt_masks_state_t group_flexi_op = marker->state & DT_MASKS_STATE_FLEXI_OP;
  const int opstate = marker->state;
  // a bypassed group contributes nothing, so nothing inside it can have any
  // visible effect: everything below is built insensitive except the operator
  // handle, which is the way back (see the sensitivity block after `hdr`).
  const gboolean group_bypassed = _op_is_bypassed(opstate);
  // the mask's own group: every other group nests in it
  // (dev-doc/masks_data_model.md). Its header is a group header like any
  // other, carrying the whole-mask actions, but it cannot be deleted, moved or
  // deselected
  const gboolean is_root = !grp || grp == dt_masks_gui_module_mask_group(module);
  // persistent "true" group invert (DT_MASKS_STATE_OP_INVERT, see
  // _group_toggle_output_invert) -- unlike group_bypassed this does not
  // affect what is built below (an inverted group still contributes to the
  // mask, just flipped), only the handle's look and its tooltip.
  const gboolean group_inverted = (opstate & DT_MASKS_STATE_OP_INVERT) != 0;

  // label: "<mode>-<id>" -- the group's flexi operator and its per-mode id
  // (shared with empty groups and the refinement caption). Once the group
  // is given a custom name (ctrl+click the title, masks v7) that replaces the
  // default label outright rather than being appended to it -- the "<op>-<id>"
  // form only exists as a placeholder until the user names the thing. No
  // disclosure triangle (groups don't expand).
  const int gord = dt_masks_gui_group_ordinal_of_cid(module, (dt_mask_id_t)cid);
  // the mask's own group is always "whole mask": it names what the header
  // stands for, so it cannot be renamed (see _start_group_rename)
  const char *custom_name = _group_custom_name(grp, (dt_mask_id_t)cid);
  gchar *txt = is_root       ? g_strdup(_("whole mask"))
               : custom_name ? g_strdup(custom_name)
                             : g_strdup_printf("%s-%d", _flexi_op_short_name(group_flexi_op), gord);
  GtkWidget *lbl = gtk_label_new(txt);
  g_free(txt);
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
  // ellipsize, now that the title column has a fixed width (see labevt's
  // size request below) instead of taking whatever it needs -- same reason
  // an element row's own name label ellipsizes (see _make_shape_row). The
  // max-width-chars cap is what actually makes that fixed width stick
  // (see the matching comment on the element row's own name label) --
  // without it a long custom name still claims its full natural width
  // whenever the header has room, at the opacity slider's expense.
  gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_max_width_chars(GTK_LABEL(lbl), 1);
  // visibility: disabled, soloed or neither, as for an element row. Soloing an
  // element elsewhere only refreshes rows in place, not headers (see
  // _refresh_all_shape_rows), and resets this through _apply_group_visibility
  // without a full list rebuild. An empty group has nothing to solo
  const gboolean group_solo = bd->solo_group_key == cid;
  GtkWidget *group_visibility = _make_visibility_button(!empty);
  g_object_set_data(G_OBJECT(group_visibility), "group-key", GUINT_TO_POINTER(cid));
  _set_visibility_status(group_visibility, group_bypassed ? MASK_VISIBILITY_DISABLED
                                           : group_solo   ? MASK_VISIBILITY_SOLO
                                                          : MASK_VISIBILITY_SHOWN);
  _press_before_widget(dt_gui_connect_click(group_visibility, _visibility_group_pressed, NULL,
                                            module));
  // low-opacity warning for the whole group (blank by default, activated in place by
  // _refresh_lowop_badges, which also sets its initial state at the end of
  // this rebuild)
  GtkWidget *group_lowop_badge = _make_lowop_badge();
  // just the (possibly swapped-for-a-rename-entry) title: badges in here would
  // change this box's width with their visibility, throwing off the title
  // column every other row shares (see labevt's size request below).
  GtkWidget *lbl_box = dt_gui_hbox();
  dt_gui_add_class(lbl_box, "dt_masks_row_name");
  dt_gui_box_add(lbl_box, dt_gui_expand(lbl));
  // a preset group's notes, and the info icon in the header's kind icon
  // column that switches them on and off (see _group_note_is_open)
  GPtrArray *note = dt_masks_gui_preset_notes_shown() ? dt_masks_gui_preset_notes(marker->preset_note) : NULL;
  GtkWidget *note_toggle = NULL;
  if(note)
  {
    note_toggle = dtgtk_togglebutton_new(dtgtk_cairo_paint_info, 0, NULL);
    dt_gui_add_class(note_toggle, "dt_transparent_background");
    dt_gui_add_class(note_toggle, "dt_masks_notes");
    gtk_widget_set_valign(note_toggle, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(note_toggle, _("show or hide how to use this group"));
    g_signal_connect(G_OBJECT(note_toggle), "toggled", G_CALLBACK(_group_note_toggled), module);
  }

  // tagged so _group_header_press's ctrl+click can find (and later replace)
  // whichever of lbl / the rename entry currently occupies this slot
  g_object_set_data(G_OBJECT(lbl_box), "title-child", lbl);
  GtkWidget *labevt = gtk_event_box_new();
  // windowless: the label must not capture the button-press/motion stream, or the
  // header's group drag source (on hdr_evbox) never arms when the user grabs the
  // label text -- the natural place to grab a row to drag it.
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(labevt), FALSE);
  gtk_container_add(GTK_CONTAINER(labevt), lbl_box);
  // expands to absorb whatever width the drawer doesn't need, same as an
  // element row's own name column. The request is just a floor so it never gets
  // squeezed to nothing: one icon wide, room for the ellipsis and a letter.
  // Any wider and this row alone kept the panel from fitting the narrowest
  // side panel
  gtk_widget_set_size_request(labevt, DT_PIXEL_APPLY_DPI(18), -1);
  gtk_widget_set_hexpand(labevt, TRUE);

  // a group nested in another is one member of its holder, which the holder's
  // operator combines with the others (dev-doc/masks_data_model.md)
  const gboolean nested = grp && grp != dt_masks_gui_module_mask_group(module);
  // a nested group shown in place of its member row (see _nested_as_group):
  // its header moves the nested group. One whose member row shows moves by
  // that row
  const dt_masks_point_group_t *nested_ref =
    nested ? dt_masks_gui_group_point(dt_masks_gui_module_mask_group(module), grp->formid) : NULL;
  const gboolean movable = nested_ref && _nested_as_group(nested_ref, grp);

  // the title's tooltip, built from what this header offers: the mask's own
  // group is neither renamed nor moved
  GString *title_tip = g_string_new(NULL);
  if(is_root)
  {
    g_string_append(title_tip, _("the whole mask: every element and group is inside it\n"
                                 "click to select it: the refinements then act on the"
                                 " whole mask\n"));
    g_string_append(title_tip, _("right-click for the mask's actions"));
  }
  else
  {
    if(empty)
      g_string_append(title_tip, _("empty group: select it, then draw a shape, or drop"
                                   " one here, to fill it\n"));
    g_string_append(title_tip, _("click to select this group, click again to select the"
                                 " whole mask\n"
                                 "ctrl+click to rename\n"));
    if(movable)
      g_string_append(title_tip, _("drag to rearrange: drop it onto a group to put it inside,"
                                   " onto the group's top or bottom edge to put it beside\n"));
    g_string_append(title_tip, _("right-click for the group's actions: disable, solo,"
                                 " invert, compose, rename, delete"));
  }
  gtk_widget_set_tooltip_text(labevt, title_tip->str);
  g_string_free(title_tip, TRUE);
  // the group's operator: how it folds its own elements, from the bottom one
  // up (dev-doc/masks_data_model.md)
  gchar *ghandle_tip = g_strdup_printf(
    is_root && group_bypassed ? _("mask operator: %s (disabled)\n"
                                  "the mask keeps its elements, but contributes nothing\n"
                                  "click the eye to switch it back on\n"
                                  "click to change the operator")
    : is_root ? _("mask operator: %s\n"
                  "how the mask combines its elements and groups, from the bottom"
                  " one up\n"
                  "click to change the operator")
    : group_bypassed ? _("group operator: %s (disabled)\n"
                       "this group keeps its elements and its place, but contributes"
                       " nothing to the mask\n"
                       "click the eye to switch it back on\n"
                       "click to change the operator")
                   : _("group operator: %s\n"
                       "how this group combines its elements, from the bottom one up\n"
                       "click to change the operator"),
    _flexi_op_name(group_flexi_op));
  GtkWidget *ghandle_btn = NULL;
  GtkWidget *ghandle =
    _make_op_combo(&ghandle_btn, _flexi_op_paint(group_flexi_op), _group_flexi_op_pressed);
  dt_gui_remove_class(ghandle, "dt_masks_op_combo");
  dt_gui_add_class(ghandle, "dt_masks_lead_box");
  dt_gui_add_class(ghandle_btn, "dt_masks_lead");
  // the generic button hover outranks .dt_masks_lead and would paint a plate
  // under it, and recolor an inverted lead's glyph into its plate
  dt_gui_add_class(ghandle_btn, "dt_no_hover");
  // as an element's lead (see _make_drag_handle): padding insets the glyph
  gtk_widget_set_size_request(ghandle_btn, DT_PIXEL_APPLY_DPI(18), DT_PIXEL_APPLY_DPI(18));
  gtk_widget_set_valign(ghandle, GTK_ALIGN_CENTER);

  if(group_inverted) dt_gui_add_class(ghandle_btn, "dt_masks_inverted");
  g_object_set_data(G_OBJECT(ghandle_btn), "module", module);
  g_object_set_data(G_OBJECT(ghandle_btn), "title-label-box", lbl_box);
  g_object_set_data(G_OBJECT(ghandle_btn), "group-key", GUINT_TO_POINTER(cid));
  gtk_widget_set_tooltip_text(ghandle_btn, ghandle_tip);
  g_free(ghandle_tip);

  GtkWidget *hdr = dt_gui_hbox();
  dt_gui_add_class(hdr, "dt_masks_header");
  // a subtle resting background distinct from plain element rows, so this
  // reads as a group heading even when nothing is selected (see
  // .dt_masks_group_header in darktable.css)
  dt_gui_add_class(hdr, "dt_masks_group_header");
  if(group_solo) dt_gui_add_class(hdr, "dt_masks_soloed");

  // a selection inside one of its nested groups is inside it too
  const gboolean has_selected = _members_hold(formids, bd->panel_selected_formid)
                                || _members_hold(formids, bd->panel_selected_group_cid);

  // "auto-expand selected", group half: only the anchor group is open, the
  // selected one or else the one the option opened last, as
  // _auto_expand_selected_group keeps it on a selection change. Without
  // either, the remembered state below applies, open by default
  const dt_mask_id_t group_anchor = dt_masks_model_auto_expand_group_anchor(bd);
  const gboolean group_auto_exp =
    _auto_expand_selected() && dt_is_valid_maskid(group_anchor);

  // with the anchor nested in it, this group has to be open to show it
  const gboolean group_expanded =
    group_auto_exp
      ? ((dt_mask_id_t)cid == group_anchor || _members_hold(formids, group_anchor))
      : (has_selected || !bd->masks_props_expanded
         || !g_hash_table_contains(bd->masks_props_expanded, GUINT_TO_POINTER(cid))
         || GPOINTER_TO_INT(
           g_hash_table_lookup(bd->masks_props_expanded, GUINT_TO_POINTER(cid))));

  if(group_auto_exp && group_expanded) bd->masks_last_expanded_group = (dt_mask_id_t)cid;

  if(group_expanded) _remember_expanded(bd, cid, TRUE);

  // an empty group has nothing to show or hide
  GtkWidget *group_expand_toggle = NULL;
  if(!empty)
  {
    group_expand_toggle = dtgtk_togglebutton_new(_paint_param_inout, 0, NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(group_expand_toggle), group_expanded);
    dt_gui_add_class(group_expand_toggle, "dt_transparent_background");
    dt_gui_add_class(group_expand_toggle, "dt_masks_icon");
    dt_gui_add_class(group_expand_toggle, "dt_masks_expander");
    gtk_widget_set_tooltip_text(group_expand_toggle,
                                _("show/hide this group's elements"));
    g_object_set_data(G_OBJECT(group_expand_toggle), "props-key", GUINT_TO_POINTER(cid));
    g_signal_connect(G_OBJECT(group_expand_toggle), "toggled",
                     G_CALLBACK(_group_expand_toggled), module);
  }

  // an empty group's still shows, disabled, so every group header has the same
  // icons. It is not the group's toggle: nothing looks it up, or flips it
  GtkWidget *group_expander = group_expand_toggle;
  if(empty)
  {
    group_expander = dtgtk_togglebutton_new(_paint_param_inout, 0, NULL);
    gtk_widget_set_sensitive(group_expander, FALSE);
    gtk_widget_set_tooltip_text(group_expander, _("this group has no elements to show"));
  }

  _pack_row_header(hdr, ghandle, labevt, group_lowop_badge, note_toggle, TRUE,
                   group_visibility, group_expander);
  // dimmed when the group contributes nothing: suppressed by a solo, or the
  // whole group bypassed (in which case the visibility button that brings it
  // back stays at full opacity)
  if(group_bypassed)
  {
    gtk_widget_set_opacity(ghandle, MASK_DIMMED_OPACITY);
    gtk_widget_set_opacity(labevt, MASK_DIMMED_OPACITY);
  }
  else if(_group_solo_suppressed(bd, first))
  {
    gtk_widget_set_opacity(hdr, MASK_DIMMED_OPACITY);
  }

  // an event box wraps the header so the canvas<->list hover sync can locate
  // this group by any member id (it carries "group-formids") and so clicking
  // it selects the group / right-clicking opens its actions menu.
  GtkWidget *hdr_evbox = _make_group_header_evbox(
    module, hdr, lbl_box,
    movable ? _mask_group_dnd : NULL,
    movable ? G_CALLBACK(_masks_group_drag_get) : NULL);
  g_object_set_data_full(G_OBJECT(hdr_evbox), "group-formids", g_list_copy(formids),
                         (GDestroyNotify)g_list_free);
  _wire_row_hover(hdr_evbox, module, g_list_copy(formids));

  // the drag source, when the group moves, is wired by _make_group_header_evbox
  // above

  // the group is selected on release, and right-click opens its actions menu
  // (see _group_header_press). The base group is tagged, for that menu
  g_object_set_data(G_OBJECT(hdr_evbox), "group-key", GUINT_TO_POINTER(cid));
  // "title-label-box" (ctrl+click rename) is tagged by
  // _make_group_header_evbox, shared with the staged-group header.
  // tagged so _apply_group_selection (a lightweight, no-rebuild selection update)
  // can find this header and toggle its highlight in place
  g_object_set_data(G_OBJECT(hdr_evbox), "mask-header", GINT_TO_POINTER(1));
  // tagged so _apply_group_visibility can find and set this header's own
  // visibility button in place too
  g_object_set_data(G_OBJECT(hdr_evbox), "visibility-btn", group_visibility);
  // same, for the group's warning badge (see _refresh_lowop_badges)
  g_object_set_data(G_OBJECT(hdr_evbox), "lowop-badge", group_lowop_badge);
  // press/release are connected by _make_group_header_evbox above

  // the header and this group's elements as one block, the element rows
  // nested under the header and rendered bottom-up. It is an event box, so a click on
  // the group's body outside its rows (padding, indent, gaps) selects the
  // group; children with their own windows still take their clicks first
  GtkWidget *group_block = gtk_event_box_new();
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(group_block), TRUE);
  GtkWidget *block_inner = dt_gui_vbox();
  gtk_container_add(GTK_CONTAINER(group_block), block_inner);
  dt_gui_add_class(group_block, "dt_masks_group_block");
  dt_gui_box_add(block_inner, hdr_evbox);
  // the list's first header, whose gap from the top is the list's padding
  if(is_root) dt_gui_add_class(hdr, "dt_masks_root");
  g_object_set_data(G_OBJECT(hdr_evbox), "header-widget", group_block);
  // a drop on the group is placed against its title (see _drop_on_group). A
  // nested group shown in place of its member row is a member of the list it
  // is packed in, `container`: a drop can land beside it there
  g_object_set_data(G_OBJECT(group_block), "drop-title", hdr_evbox);
  if(movable)
  {
    g_object_set_data(G_OBJECT(group_block), "drop-list-item", container);
    g_object_set_data(G_OBJECT(container), "drop-frame", group_block);
  }
  // "header-widget" is the whole block, which a soloed group tags; the
  // selection shades "group-header-widget" (hdr, tagged by
  // _make_group_header_evbox), and so does solo dimming, as the member rows
  // dim themselves.
  // The operator handle, for _apply_group_output_invert_icon
  g_object_set_data(G_OBJECT(hdr_evbox), "ghandle-widget", ghandle_btn);
  // for _apply_group_header_dimming, which grays the controls of a group a
  // solo suppresses, and must not re-enable those of a bypassed group
  if(group_bypassed)
    g_object_set_data(G_OBJECT(hdr_evbox), "group-bypassed", GINT_TO_POINTER(1));

  // a drop is placed against the group's block, title included (see
  // _drop_on_group), wherever its elements and the gaps between them are not
  // under the pointer
  g_object_set_data_full(G_OBJECT(group_block), "group-formids", g_list_copy(formids),
                         (GDestroyNotify)g_list_free);
  g_object_set_data(G_OBJECT(group_block), "group-expand-toggle", group_expand_toggle);
  _set_drop_target(group_block);

  // a click on the group's body acts as one on its header, through the same
  // handlers, which read the keys the header carries ("group-formids" is set
  // above)
  g_object_set_data(G_OBJECT(group_block), "group-key", GUINT_TO_POINTER(cid));
  g_object_set_data(G_OBJECT(group_block), "title-label-box", lbl_box);
  GtkGestureSingle *block_gesture = dt_gui_connect_click(group_block, _group_block_press, NULL,
                                                         module);
  g_signal_connect(block_gesture, "released", G_CALLBACK(_group_block_release), module);

  // highlight the group's header row when its group is the selected one (see
  // _paint_group_selection). Held by a selected element, it is selected by
  // implication instead (see _apply_ancestor_selection)
  if(dt_is_valid_maskid(_explicit_group_cid(bd))
     && (dt_mask_id_t)cid == _explicit_group_cid(bd))
    dt_gui_add_class(hdr, "dt_masks_selected");

  GtkWidget *elem_box = dt_gui_vbox();
  // indent/inset entirely via CSS (.dt_masks_group_elements in
  // darktable.css), not hardcoded here
  dt_gui_add_class(elem_box, "dt_masks_list");
  dt_gui_add_class(elem_box, "dt_masks_group_elements");
  gtk_widget_set_visible(elem_box, empty || group_expanded);
  if(group_expand_toggle)
    g_object_set_data(G_OBJECT(group_expand_toggle), "elem-box", elem_box);

  // a preset group's note on how to use it, leading the group's card. It is
  // the card's top part, the opacity slider below (if any) the rest: the
  // note leaves its bottom edge open onto it (.dt_masks_joined)
  const gboolean list_slider = !_props_subpanel();
  if(note)
  {
    GtkWidget *note_w = _make_group_note(module, cid, note);
    if(list_slider) dt_gui_add_class(note_w, "dt_masks_joined");
    g_object_set_data(G_OBJECT(note_w), "note-toggle", note_toggle);
    g_object_set_data(G_OBJECT(note_toggle), "note", note_w);
    g_object_set_data(G_OBJECT(note_w), "note-hdr", hdr);
    if(empty) g_object_set_data(G_OBJECT(note_w), "note-empty", GINT_TO_POINTER(1));
    g_object_set_data(G_OBJECT(note_w), "drag-hide", GINT_TO_POINTER(1));
    dt_gui_box_add(elem_box, note_w);
  }

  // the group's opacity, a labeled slider leading its expanded contents, or
  // in the properties subpanel while the group is selected. Packed first
  // after the note, so it stays above the member rows (packed from the
  // bottom, see _pack_group_elements) and the pending row
  if(list_slider)
  {
    GtkWidget *slider_box = _build_group_opacity_editor(module, cid);
    g_object_set_data(G_OBJECT(slider_box), "drag-hide", GINT_TO_POINTER(1));
    dt_gui_box_add(elem_box, slider_box);
  }
  // the card hangs straight off the header as one block, so the header
  // squares its bottom edge onto it (.dt_masks_has_card). Onto a note
  // only while it is open, see _sync_group_note
  if(list_slider) dt_gui_add_class(hdr, "dt_masks_has_card");

  _pack_group_elements(module, grp, elem_box, g_list_reverse(g_list_copy(formids)));
  // the gaps between its elements are placed against them (see _drop_at)
  g_object_set_data(G_OBJECT(elem_box), "drop-list-owner", group_block);
  g_object_set_data(G_OBJECT(group_block), "drop-list", elem_box);
  _set_drop_target(elem_box);

  // if a shape is currently being drawn and this group is where it would land
  // (see _recompute_insert_hint), show its disposable placeholder row at the
  // top of this group's elements -- exactly where the real row lands once it
  // commits (a new element is inserted above the group's current top member).
  if(pending_form
     && (!grp
         || (bd->insert_active
             && dt_masks_gui_group_cid_of_form(grp, bd->insert_after_fid) == (dt_mask_id_t)cid)))
    dt_gui_box_add(elem_box, _make_pending_shape_row(module, pending_form));

  // a childless box would still open a gap under the header with its
  // padding: hidden, by its children rather than `empty`, which counts only
  // members. Only ever hides; no_show_all, or the show_all in
  // _masks_panel_pack shows it again
  {
    GList *kids = gtk_container_get_children(GTK_CONTAINER(elem_box));
    if(!kids)
    {
      gtk_widget_set_no_show_all(elem_box, TRUE);
      gtk_widget_set_visible(elem_box, FALSE);
    }
    g_list_free(kids);
  }

  dt_gui_box_add(block_inner, elem_box);
  // disconnected with hdr, which the block destroys together with elem_box
  g_signal_connect_object(elem_box, "notify::visible", G_CALLBACK(_sync_group_open), hdr, 0);
  _sync_group_open(elem_box, NULL, hdr);

  gtk_box_pack_end(GTK_BOX(container), group_block, FALSE, FALSE, 0);

  g_list_free(formids);
}

// the single shape currently being drawn on canvas for this module (if any)
// -- not a real grp->points member yet, rendered as a disposable placeholder
// row instead (see _make_pending_shape_row). NULL whenever nothing is being
// drawn, or it belongs to a different module.
static dt_masks_form_t *_pending_form(dt_iop_module_t *module)
{
  const dt_masks_form_gui_t *pending_fg = darktable.develop->form_gui;
  return (pending_fg && pending_fg->creation && pending_fg->creation_module == module)
           ? darktable.develop->form_visible
           : NULL;
}

// the nested group `sub`, packed into `box` the way the mask's own group packs
// into masks_list_box: in place of the row of the member that holds it, or
// under that row
static void _pack_subgroup(dt_iop_module_t *module, dt_masks_form_t *sub, GtkWidget *box)
{
  // a malformed tree can hold a group inside itself: stop where every
  // recursive walk of the mask stops. The panel is built on the GUI thread only
  static int depth = 0;
  if(depth >= DT_MASKS_NESTING_MAX || !sub->points) return;
  depth++;
  _pack_group(module, sub, sub->points->data, sub->points->next, _pending_form(module), box);
  depth--;
}

// the header line of a row or group packed into a group's elements box: the
// first element, group or cluster header below `w`. NULL for something with no
// header line (the group opacity slider)
static GtkWidget *_mask_row_header(GtkWidget *w)
{
  if(_has_class(w, "dt_masks_header")) return w;
  if(!GTK_IS_CONTAINER(w)) return NULL;
  GtkWidget *found = NULL;
  GList *kids = gtk_container_get_children(GTK_CONTAINER(w));
  for(GList *k = kids; k && !found; k = g_list_next(k))
    found = _mask_row_header(GTK_WIDGET(k->data));
  g_list_free(kids);
  return found;
}

// a group's rail, drawn rather than left to its CSS border so it can stop at
// its last member, with a tick across the indent to each member's header line
// (a tree view's ├ and └). The CSS still sets all of it. The width is the
// box's border-left-width, whose border is transparent and so only reserves
// the room. The color is the group HEADER's outline-color, which GTK computes
// but never draws with no outline-style: the header's own state rules
// (selected, implied, hover) set it beside its background, so the rail follows
// its header in every state, hover included, which no selector on the box
// could do since hover is a class on the header, a sibling. `user_data` is
// that header (see _hook_mask_rails)
static gboolean _masks_rail_draw(GtkWidget *box, cairo_t *cr, gpointer user_data)
{
  GtkWidget *hdr_of_box = GTK_WIDGET(user_data);
  GtkStyleContext *ctx = gtk_widget_get_style_context(box);
  const GtkStateFlags state = gtk_style_context_get_state(ctx);
  GtkBorder border;
  gtk_style_context_get_border(ctx, state, &border);
  GtkStyleContext *hctx = gtk_widget_get_style_context(hdr_of_box);
  GdkRGBA *color = NULL;
  gtk_style_context_get(hctx, gtk_style_context_get_state(hctx), "outline-color", &color,
                        NULL);
  if(!color || border.left <= 0 || color->alpha <= 0.0)
  {
    if(color) gdk_rgba_free(color);
    return FALSE;
  }

  const int w = border.left;
  // the lowest tick is the rail's end, drawn below as a rounded elbow (└).
  // Found by position, not list order: members are packed from the end, so
  // the list does not run top to bottom
  int last_top = -1, last_hx = 0;
  GList *kids = gtk_container_get_children(GTK_CONTAINER(box));
  for(GList *k = kids; k; k = g_list_next(k))
  {
    GtkWidget *child = GTK_WIDGET(k->data);
    if(!gtk_widget_get_visible(child)) continue;
    GtkWidget *hdr = _mask_row_header(child);
    if(!hdr || !gtk_widget_get_visible(hdr)) continue;
    // center on the header's visible line, not on its allocation: a CSS margin
    // is part of a GTK3 widget's allocation, and a group header carries a
    // top one (.dt_masks_group_header), which put its tick
    // in the gap above it
    GtkStyleContext *mctx = gtk_widget_get_style_context(hdr);
    GtkBorder margin;
    gtk_style_context_get_margin(mctx, gtk_style_context_get_state(mctx), &margin);
    const int line = gtk_widget_get_allocated_height(hdr) - margin.top - margin.bottom;
    int hx = 0, hy = 0;
    if(!gtk_widget_translate_coordinates(hdr, box, margin.left, margin.top + line / 2,
                                         &hx, &hy))
      continue;
    // from the rail's inner edge to the header's left edge, centered on its
    // line; the lowest one is held back for the elbow
    const int top = hy - w / 2;
    if(top > last_top)
    {
      if(last_top >= 0) cairo_rectangle(cr, w, last_top, last_hx - w, w);
      last_top = top;
      last_hx = hx;
    }
    else
      cairo_rectangle(cr, w, top, hx - w, w);
    if(g_getenv("DT_MASKS_PANEL_DUMP"))
    {
      GtkAllocation ba;
      gtk_widget_get_allocation(box, &ba);
      dt_print(DT_DEBUG_ALWAYS,
               "[masks-rail] box %dx%d rail=%d child=%s#%s hdr=%s#%s h=%d m=%d/%d/%d"
               " -> tick x=[%d,%d) y=%d",
               ba.width, ba.height, w, G_OBJECT_TYPE_NAME(child),
               gtk_widget_get_name(child), G_OBJECT_TYPE_NAME(hdr),
               gtk_widget_get_name(hdr), gtk_widget_get_allocated_height(hdr),
               margin.top, margin.bottom, margin.left, w, hx, top);
    }
  }
  g_list_free(kids);

  if(last_top >= 0)
  {
    // the elbow's outer radius comes from the CSS with the rail's width and
    // color. Read through the "border-radius" shorthand, the only radius GTK3
    // lets a style context query: it reports the top-left corner, as an int
    // (pack_border_radius in gtkcssshorthandpropertyimpl.c). The per-corner
    // longhands have no query function at all, and asking for one aborts the
    // varargs read and crashes. Clamped so the curve never runs past the
    // rail's top or the last tick's end
    int radius = 0;
    gtk_style_context_get(ctx, state, "border-radius", &radius, NULL);
    const int bottom = last_top + w;
    const int r = CLAMP(radius, w, MIN(bottom, last_hx));
    // rail down to where the curve starts, then the last tick from where it
    // ends, joined by a quarter ring of outer radius r and inner r - w
    cairo_rectangle(cr, 0, 0, w, bottom - r);
    cairo_rectangle(cr, r, last_top, last_hx - r, w);
    cairo_new_sub_path(cr);
    cairo_arc_negative(cr, r, bottom - r, r, G_PI, G_PI / 2.0);
    cairo_arc(cr, r, bottom - r, r - w, G_PI / 2.0, G_PI);
    cairo_close_path(cr);
  }
  gdk_cairo_set_source_rgba(cr, color);
  cairo_fill(cr);
  gdk_rgba_free(color);
  return FALSE;
}

// hook every group's elements box up to its drawn rail (_masks_rail_draw),
// paired with the header it hangs from: the header sits before the box in the
// same parent, so the first header below that parent is it. Done over the
// finished tree rather than while packing, because a group reaches the list
// through more than one packing path (_pack_group directly, _pack_subgroup for
// a nested one, _make_shape_row for a subgroup row)
static void _hook_mask_rails(GtkWidget *w)
{
  if(_has_class(w, "dt_masks_group_elements")
     && !g_object_get_data(G_OBJECT(w), "rail-drawn"))
  {
    // a box holding the opacity slider alone draws no rail (the rail runs to
    // the last member's tick), so the slider's block must run flush under the
    // whole header instead of stopping at a rail that is not there
    // (.dt_masks_card_only). Tagged here rather than where the box is
    // built: an empty nested group is built as an element row (see
    // _nested_as_group), not by _pack_group, and every box passes through here
    // A preset's note is part of the same card (see _pack_group)
    GList *kids = gtk_container_get_children(GTK_CONTAINER(w));
    gboolean card_only = kids != NULL;
    for(GList *k = kids; k; k = g_list_next(k))
    {
      if(!_has_class(k->data, "dt_masks_group_card") && !_has_class(k->data, "dt_masks_note"))
        card_only = FALSE;
    }
    if(card_only) dt_gui_add_class(w, "dt_masks_card_only");
    g_list_free(kids);

    GtkWidget *hdr = _mask_row_header(gtk_widget_get_parent(w));
    if(hdr)
    {
      g_object_set_data(G_OBJECT(w), "rail-drawn", GINT_TO_POINTER(TRUE));
      // both tied to the other widget's lifetime, so neither outlives it
      g_signal_connect_object(G_OBJECT(w), "draw", G_CALLBACK(_masks_rail_draw), hdr,
                              G_CONNECT_AFTER);
      // a state change restyles the header, not its sibling box: redraw the
      // rail with it, or it keeps the old state's color
      g_signal_connect_object(G_OBJECT(hdr), "style-updated",
                              G_CALLBACK(gtk_widget_queue_draw), w, G_CONNECT_SWAPPED);
    }
  }
  if(!GTK_IS_CONTAINER(w)) return;
  GList *kids = gtk_container_get_children(GTK_CONTAINER(w));
  for(GList *k = kids; k; k = g_list_next(k)) _hook_mask_rails(GTK_WIDGET(k->data));
  g_list_free(kids);
}

// build the panel's row tree from the reconciled model.
// Every mutation happens in _masks_panel_reconcile above, so this only reads.
static void _masks_panel_pack(dt_iop_module_t *module, dt_masks_form_t *grp)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  dt_masks_form_t *pending_form = _pending_form(module);
  GtkWidget *list = GTK_WIDGET(bd->masks_list_box);

  DT_ENTER_GUI_UPDATE();

  if(!bd->masks_cluster_expanded)
    bd->masks_cluster_expanded = g_hash_table_new(g_direct_hash, g_direct_equal);

  // the mask's own group, its header with its element rows nested under it.
  // A nested group's block sits among them the same way (see _pack_subgroup)
  if(grp && grp->points)
    _pack_group(module, grp, grp->points->data, grp->points->next, pending_form, list);
  else
  {
    // a mask with no group form yet shows the group it will have, empty,
    // which its first edit creates (see dt_masks_gui_module_flexi_group)
    dt_masks_point_group_t none = { 0 };
    none.formid = INVALID_MASKID;
    none.state = DT_MASKS_STATE_GROUP_MARKER;
    none.group_opacity = 1.0f;
    _pack_group(module, NULL, &none, NULL, pending_form, list);
  }

  _hook_mask_rails(list);

  // the box carries no_show_all (flexi-only), which makes gtk_widget_show_all on
  // the box itself a no-op; show each header explicitly, then reveal the box.
  GList *children = gtk_container_get_children(GTK_CONTAINER(bd->masks_list_box));
  for(GList *c = children; c; c = g_list_next(c))
    gtk_widget_show_all(GTK_WIDGET(c->data));
  g_list_free(children);
  _sync_group_notes(bd);
  gtk_widget_set_visible(GTK_WIDGET(bd->masks_list_box), TRUE);

  // a scope whose target is gone follows the surviving selection instead
  if(dt_masks_model_refine_scope_prune(module)) _flexi_refine_follow_selection(bd);

  // keep the canvas mirror of the persistent selection in step with the rebuild
  if(darktable.develop && darktable.develop->form_gui)
    darktable.develop->form_gui->panel_selected_formid = bd->panel_selected_formid;

  DT_LEAVE_GUI_UPDATE();

  // the insertion hint must always reflect the current target after a rebuild
  _recompute_insert_hint(module);

  // with its last shape gone there is nothing left to edit on the canvas
  _masks_panel_apply_shape_sensitivity(bd);
  if(bd->masks_edit && !_module_has_drawn_shapes(module)
     && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(bd->masks_edit)))
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), FALSE);
    dt_masks_set_edit_mode(module, DT_MASKS_EDIT_OFF);
  }

  _update_add_target_hints(module);
  _update_refine_sensitivity(module);
  _sync_solo_canvas_highlight(module);
  // badges are built hidden and revealed from the current opacities -- after the
  // show_all pass above (which cannot force them on, they carry no_show_all) and
  // after every row is registered in bd->masks_row_map
  _refresh_lowop_badges(module);
  // the rows paint their own selection as they are built, not what holds them
  _apply_ancestor_selection(GTK_WIDGET(bd->masks_list_box), bd->panel_selected_formid,
                            bd->panel_selected_group_cid);
  // the data the rows were rebuilt from changed, and so may the shape's editor
  _props_panel_sync(module, TRUE);
}

// one line per widget under the masks list: type, CSS node name, style
// classes, visibility and allocated size. Debug-only, reached solely through
// DT_MASKS_PANEL_DUMP (see the call at the end of dt_masks_gui_build_list).
static void _dump_masks_panel_tree(GtkWidget *w, const int depth)
{
  if(!w) return;
  GString *cls = g_string_new(NULL);
  GList *classes = gtk_style_context_list_classes(gtk_widget_get_style_context(w));
  for(GList *c = classes; c; c = g_list_next(c))
    g_string_append_printf(cls, ".%s", (const char *)c->data);
  g_list_free(classes);

  GtkAllocation a;
  gtk_widget_get_allocation(w, &a);
  GList *kids =
    GTK_IS_CONTAINER(w) ? gtk_container_get_children(GTK_CONTAINER(w)) : NULL;
  const char *name = gtk_widget_get_name(w);

  dt_print(DT_DEBUG_ALWAYS, "[masks-tree] %*s%s #%s %s vis=%d x=%d y=%d %dx%d kids=%d",
           depth * 2, "", G_OBJECT_TYPE_NAME(w), name ? name : "-",
           cls->str[0] ? cls->str : "-", gtk_widget_get_visible(w), a.x, a.y,
           a.width, a.height, g_list_length(kids));
  g_string_free(cls, TRUE);

  for(GList *k = kids; k; k = g_list_next(k))
    _dump_masks_panel_tree(GTK_WIDGET(k->data), depth + 1);
  g_list_free(kids);
}

static gboolean _dump_masks_panel_tree_idle(gpointer user_data)
{
  dt_iop_module_t *module = user_data;
  const dt_iop_gui_blend_data_t *bd = module ? module->blend_data : NULL;
  if(bd && bd->masks_list_box)
  {
    dt_print(DT_DEBUG_ALWAYS, "[masks-tree] ==== %s ====", module->op);
    _dump_masks_panel_tree(GTK_WIDGET(bd->masks_list_box), 0);
  }
  return G_SOURCE_REMOVE;
}

void dt_masks_gui_build_list(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!bd || !bd->masks_list_box) return;
  if(bd->masks_rebuild_suppressed) return;

  if(bd->masks_rebuild_idle_id)
  {
    g_source_remove(bd->masks_rebuild_idle_id);
    bd->masks_rebuild_idle_id = 0;
  }
  bd->masks_rebuild_pending = FALSE;

  // nothing the tree is built from changed: skip the rebuild, which makes
  // redundant requests cheap. DT_INVALID_HASH (a new bd) never matches
  const dt_hash_t sig = dt_masks_gui_list_signature(module);
  if(sig != DT_INVALID_HASH && sig == bd->masks_list_sig)
  {
    dt_print(DT_DEBUG_MASKS, "[masks] build skipped (signature unchanged, 0x%llx)",
             (unsigned long long)sig);
    return;
  }
  bd->masks_list_sig = sig;
  // what a drag opened and hid is destroyed with the rows (see _masks_drag)
  _masks_drag_set_candidate(NULL);
  if(_masks_drag.opened) g_ptr_array_set_size(_masks_drag.opened, 0);

  // rebuilding destroys the rows without delivering leave events, so clear any
  // pending hover feedback to avoid a highlight sticking on the canvas
  if(darktable.develop->form_gui)
  {
    g_list_free(darktable.develop->form_gui->panel_hover_formids);
    darktable.develop->form_gui->panel_hover_formids = NULL;
    darktable.develop->form_gui->canvas_hover_formid = INVALID_MASKID;
  }

  // the wipe destroys the parametric rows' pickers, while
  // darktable.lib->proxy.colorpicker.picker_proxy may still point at one: the
  // next click on any picker would reset the destroyed one and crash. Other
  // pickers live as long as their module, so only this panel needs it. Before
  // the wipe: dt_iop_color_picker_reset unsets the picker's toggle
  dt_iop_color_picker_reset(module, FALSE);

  dt_gui_container_remove_children(GTK_CONTAINER(bd->masks_list_box));

  // the pending-row sliders (if any) are children of masks_list_box and were
  // just destroyed by the wipe above -- forget the stale pointers so
  // dt_iop_gui_blend_sync_pending_ai_sliders can tell "no active session" from
  // "the row just hasn't been (re)built yet" apart. _make_pending_shape_row
  // repopulates these below if a pending row is actually built this pass.
  bd->pending_ai_smoothing_slider = NULL;
  bd->pending_ai_cleanup_slider = NULL;
  // the same for the creation controls a pending row built for the shape
  // properties subpanel: it still shows them until _props_panel_sync replaces them
  bd->pending_props_box = NULL;

  // reset the formid -> row index; it is repopulated as _make_shape_row builds
  // each row below. Cleared here (before any new rows) so it never holds a
  // pointer to a just-destroyed row.
  if(!bd->masks_row_map)
    bd->masks_row_map = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                              (GDestroyNotify)g_slist_free);
  else
    g_hash_table_remove_all(bd->masks_row_map);

  // dt_dev_pixelpipe_synch_all() resets module->blend_params and replays the
  // history under dev->history_mutex, so a read without it can see the mask
  // mid-reset, as empty
  dt_pthread_mutex_lock(&darktable.develop->history_mutex);
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  const gboolean flexi = !(module->blend_params->mask_mode & DEVELOP_MASK_RASTER);
  dt_pthread_mutex_unlock(&darktable.develop->history_mutex);

  if(!_masks_panel_reconcile(module, grp, flexi))
  {
    gtk_widget_set_visible(GTK_WIDGET(bd->masks_list_box), FALSE);
    _recompute_insert_hint(module);
    _props_panel_sync(module, TRUE);
    return;
  }

  _masks_panel_pack(module, grp);

  // DT_MASKS_PANEL_DUMP=1 prints the finished list as a tree: widget type, CSS
  // node name, style classes, visibility and allocated size. The panel's look
  // is decided by CSS over a tree this file builds through several different
  // paths (a group via _pack_group, a nested one as an element row
  // via _make_shape_row), and working out from the source which rule reaches
  // which widget is unreliable. Deferred to an idle so the sizes printed are
  // the ones after layout, not the stale ones from before it.
  if(g_getenv("DT_MASKS_PANEL_DUMP"))
    g_idle_add(_dump_masks_panel_tree_idle, module);
}

// expand/collapse a same-kind element cluster, from the "revealer" its
// triangle button or header carries
static void _element_cluster_toggle(GtkGestureSingle *gesture, dt_iop_module_t *module)
{
  GtkWidget *rev = g_object_get_data(G_OBJECT(dt_gui_get_widget(gesture)), "revealer");
  _cluster_set_revealed(module->blend_data, rev,
                        !gtk_revealer_get_reveal_child(GTK_REVEALER(rev)));
}

// the triangle toggles on its own press: it is not a drag source. The press is
// claimed before the header ever sees it, so the two never both toggle for one
// click
static void _element_cluster_arrow_pressed(GtkGestureSingle *gesture,
                                           const int n_press,
                                           const double x,
                                           const double y,
                                           dt_iop_module_t *module)
{
  if(gtk_gesture_single_get_current_button(gesture) != GDK_BUTTON_PRIMARY) return;
  dt_gui_claim(gesture);
  _element_cluster_toggle(gesture, module);
}

// the header toggles on release: it is the cluster's drag source, and a drag
// delivers no release, so dragging the cluster never also toggles it (same
// split _row_click_press/_release use)
static void _element_cluster_released(GtkGestureSingle *gesture,
                                      const int n_press,
                                      const double x,
                                      const double y,
                                      dt_iop_module_t *module)
{
  if(gtk_gesture_single_get_current_button(gesture) == GDK_BUTTON_PRIMARY)
    _element_cluster_toggle(gesture, module);
}

// right-click deletes every member of the cluster; a primary press is left to
// the drag source and to the release (see _element_cluster_released)
static void _element_cluster_press(GtkGestureSingle *gesture,
                                   const int n_press,
                                   const double x,
                                   const double y,
                                   dt_iop_module_t *module)
{
  if(gtk_gesture_single_get_current_button(gesture) != GDK_BUTTON_SECONDARY) return;
  dt_gui_claim(gesture);
  GList *members = g_object_get_data(G_OBJECT(dt_gui_get_widget(gesture)), "hover-formids");
  _delete_elements(module, members);
}

// a member that is a nested group, shown as a group of its own like a
// top-level one: it holds one group, and its reference carries nothing the
// group's header cannot show (migration moves a reference's opacity and
// inversion onto the group's marker where that is exact, masks.c
// _fold_nested_refs). Anything else keeps the element row that shows it
static gboolean _nested_as_group(const dt_masks_point_group_t *pt, const dt_masks_form_t *form)
{
  if(!(form->type & DT_MASKS_GROUP) || (form->type & (DT_MASKS_CLONE | DT_MASKS_OBJECT)))
    return FALSE;
  if(pt->opacity != 1.0f || pt->refinement.enabled != DT_MASKS_REFINE_OFF
     || (pt->state & (DT_MASKS_STATE_INVERSE | DT_MASKS_STATE_HIDDEN | DT_MASKS_STATE_DISABLE)))
    return FALSE;
  if(!form->points || !dt_masks_point_is_marker(form->points->data)) return FALSE;
  for(const GList *l = g_list_next(form->points); l; l = g_list_next(l))
    if(dt_masks_point_is_marker(l->data)) return FALSE;
  return TRUE;
}

// pack one group's element rows into `container`, nested under that group's
// header. `grp` is the group form whose list holds the group: the mask's own,
// or a nested group's. `fids` is the group's member ids bottom-up
// (consumed/freed here). Same-kind drawn shapes fold into expand/collapse
// clusters; parametric forms are never folded (each keeps its own inline
// editor)
static void _pack_group_elements(dt_iop_module_t *module,
                                 dt_masks_form_t *grp,
                                 GtkWidget *container,
                                 GList *fids)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!grp || !fids)
  {
    g_list_free(fids);
    return;
  }

  // build the element rows in bottom-up order, then fold adjacent same-kind runs
  // into expand/collapse clusters (a lone shape stays a plain row). Pack with
  // pack_end so the bottom member sits at the bottom.
  const int n = g_list_length(fids);
  GtkWidget **rows = g_malloc0_n(n, sizeof(GtkWidget *));
  guint *kinds = g_malloc0_n(n, sizeof(guint));
  dt_mask_id_t *fid_of = g_malloc0_n(n, sizeof(dt_mask_id_t));
  int nr = 0;
  for(GList *l = fids; l; l = g_list_next(l))
  {
    const dt_mask_id_t fid = GPOINTER_TO_INT(l->data);
    dt_masks_point_group_t *fpt = dt_masks_gui_group_point(grp, fid);
    dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, fid);
    if(!fpt || !form)
    {
      // header rendered but this element's row dropped -- "the group looks empty"
      dt_print(DT_DEBUG_MASKS,
               "[masks] panel: element %d of '%s' dropped (point=%s form=%s)", fid,
               module->op, fpt ? "ok" : "MISSING", form ? "ok" : "MISSING");
      continue;
    }
    if(_nested_as_group(fpt, form))
    {
      // packed as its own group, header and all, in this element's place
      rows[nr] = dt_gui_vbox();
      _pack_subgroup(module, form, rows[nr]);
    }
    else
      rows[nr] = _make_shape_row(module, fpt, form);
    kinds[nr] = _form_kind(form);
    fid_of[nr] = fid;
    nr++;
  }
  g_list_free(fids);

  // fold runs of >= 3 adjacent same-kind drawn elements into expand/collapse
  // clusters to cut clutter. Only adjacent ones: a group folds its members in
  // list order, so gathering scattered members would either misstate that order
  // or have to reorder the group. Dragging a member next to another kind thus
  // takes it out of its cluster. Parametric and raster forms are never
  // clustered (each has its own inline editor), nor are nested groups. pack_end
  // keeps the bottom member at the bottom.
  const int cluster_min = 3;
  for(int i = 0; i < nr;)
  {
    const guint kind = kinds[i];
    int count = 1;
    while(i + count < nr && kinds[i + count] == kind) count++;

    // kind 0 is a nested group (see _form_kind): each shows its own group
    if(count < cluster_min || kind == DT_MASKS_PARAMETRIC || kind == DT_MASKS_RASTER
       || kind == 0)
    {
      gtk_box_pack_end(GTK_BOX(container), rows[i], FALSE, FALSE, 0);
      i++;
      continue;
    }

    // nested one level deeper than a plain (unclustered) element row, via CSS
    // (.dt_masks_cluster_elements' own margin-left in darktable.css), so expanding
    // a cluster visually reads as revealing its members as its own children.
    GtkWidget *inner = dt_gui_vbox();
    dt_gui_add_class(inner, "dt_masks_cluster_elements");
    GList *member_fids = NULL;
    gboolean contains_selected = FALSE;
    for(int k = i; k < i + count; k++)
    {
      gtk_box_pack_end(GTK_BOX(inner), rows[k], FALSE, FALSE, 0);
      member_fids = g_list_prepend(member_fids, GINT_TO_POINTER(fid_of[k]));
      if(dt_is_valid_maskid(bd->panel_selected_formid)
         && fid_of[k] == bd->panel_selected_formid)
        contains_selected = TRUE;
    }

    // a same-kind cluster: a header that only folds and unfolds. Its state is
    // keyed by its first member, so that it survives a rebuild; only a TRUE
    // recorded there opens it
    const guint cid = (guint)fid_of[i];
    const gboolean expanded =
      contains_selected
      || (g_hash_table_contains(bd->masks_cluster_expanded, GUINT_TO_POINTER(cid))
          && GPOINTER_TO_INT(
            g_hash_table_lookup(bd->masks_cluster_expanded, GUINT_TO_POINTER(cid))));
    if(expanded && bd->masks_cluster_expanded)
      g_hash_table_insert(bd->masks_cluster_expanded, GUINT_TO_POINTER(cid),
                          GINT_TO_POINTER(TRUE));

    gchar *txt = g_strdup_printf("%d %s", count, _kind_name(kind, TRUE));
    GtkWidget *lbl = gtk_label_new(txt);
    g_free(txt);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    GtkWidget *arrow =
      dtgtk_button_new(dtgtk_cairo_paint_dropdown, expanded ? 0 : CPF_DIRECTION_UP, NULL);
    gtk_widget_set_valign(arrow, GTK_ALIGN_CENTER);
    // kind icon in the same column the member rows' drag handle occupies (see
    // _make_shape_row), so a collapsed cluster still shows what it is -- just
    // column-aligned; the actual drag source is hdr_evbox below (the whole
    // header row, like a group's), not this icon itself
    // no tooltip of its own: the header's, below, covers the icon too
    GtkWidget *kicon = _make_drag_handle(_kind_icon_paint(kind), TRUE, NULL);
    // label and disclosure triangle side-by-side
    GtkWidget *lblbox = dt_gui_hbox();
    gtk_box_set_spacing(GTK_BOX(lblbox), DT_PIXEL_APPLY_DPI(4));
    dt_gui_box_add(lblbox, lbl, arrow);
    GtkWidget *chdr = dt_gui_hbox();
    dt_gui_add_class(chdr, "dt_masks_header");
    dt_gui_add_class(chdr, "dt_masks_cluster_header");
    dt_gui_box_add(chdr, kicon, dt_gui_expand(lblbox));
    GtkWidget *hdr_evbox = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(hdr_evbox), TRUE);
    gtk_container_add(GTK_CONTAINER(hdr_evbox), chdr);
    gtk_widget_set_tooltip_text(hdr_evbox, _("elements of one kind, next to each other,"
                                             " folded into one row\n"
                                             "click to expand or collapse\n"
                                             "drag anywhere in the row to move them all"
                                             " together, like a single element\n"
                                             "right-click to delete them all"));

    GtkWidget *rev = gtk_revealer_new();
    gtk_container_add(GTK_CONTAINER(rev), inner);
    gtk_revealer_set_reveal_child(GTK_REVEALER(rev), expanded);

    // both the header and the triangle toggle: the triangle, a button, takes
    // its own press. The header toggles on release (_element_cluster_released)
    g_object_set_data(G_OBJECT(hdr_evbox), "revealer", rev);
    // on the revealer itself, so a member row can walk up its own ancestors to
    // find (and force open) its enclosing cluster (see _reveal_containers_for_row)
    g_object_set_data(G_OBJECT(rev), "arrow", arrow);
    g_object_set_data(G_OBJECT(rev), "cluster-key", GUINT_TO_POINTER(cid));
    _wire_row_hover(hdr_evbox, module, member_fids);
    GtkGestureSingle *cluster_gesture =
      dt_gui_connect_click(hdr_evbox, _element_cluster_press, NULL, module);
    g_signal_connect(cluster_gesture, "released", G_CALLBACK(_element_cluster_released), module);
    // draggable as a block, moving every member together (see dt_masks_gui_cluster_move):
    // "hover-formids" set just above already holds every member's formid, reused
    // as-is by _masks_cluster_drag_get.
    gtk_drag_source_set(hdr_evbox, GDK_BUTTON1_MASK, _mask_cluster_dnd, 1,
                        GDK_ACTION_MOVE);
    g_signal_connect(G_OBJECT(hdr_evbox), "drag-data-get",
                     G_CALLBACK(_masks_cluster_drag_get), NULL);
    g_signal_connect(G_OBJECT(hdr_evbox), "drag-begin", G_CALLBACK(_cluster_drag_begin), module);
    g_signal_connect(G_OBJECT(hdr_evbox), "drag-end", G_CALLBACK(_masks_drag_end), module);
    g_signal_connect(G_OBJECT(hdr_evbox), "drag-failed", G_CALLBACK(_masks_drag_failed), module);
    g_object_set_data(G_OBJECT(arrow), "revealer", rev);
    _press_before_widget(dt_gui_connect_click(arrow, _element_cluster_arrow_pressed, NULL,
                                              module));

    GtkWidget *cbox = dt_gui_vbox();
    dt_gui_box_add(cbox, hdr_evbox, rev);
    g_object_set_data(G_OBJECT(cbox), "drop-header", chdr);
    g_object_set_data(G_OBJECT(cbox), "cluster-revealer", rev);
    gtk_box_pack_end(GTK_BOX(container), cbox, FALSE, FALSE, 0);

    // a drop is placed against the whole cluster, as one element: it lands
    // above its top member or below its bottom one. Its open member rows, and
    // the gaps between them, take a drop of their own (see _drop_at)
    g_object_set_data(G_OBJECT(cbox), "drop-item", GINT_TO_POINTER(1));
    g_object_set_data(G_OBJECT(cbox), "drop-row-top", rows[i + count - 1]);
    g_object_set_data(G_OBJECT(cbox), "drop-row-bottom", rows[i]);
    _set_drop_target(cbox);
    g_object_set_data(G_OBJECT(inner), "drop-list-owner", cbox);
    _set_drop_target(inner);
    i += count;
  }

  g_free(rows);
  g_free(kinds);
  g_free(fid_of);
}

// create a SINGLE-CHANNEL parametric form and open it for inline editing.
// channel_idx indexes the module's blend-colorspace channel[] array; in_out picks
// input(0)/output(1). Seeded NEUTRAL (no channel bit active, whole-range params) so
// it has no effect until the slider is dragged. Each parametric form edits exactly
// one channel; several can be combined with the usual operators, like shapes.
static void
_add_parametric_channel(dt_iop_module_t *self, const int channel_idx, const int in_out)
{
  dt_iop_gui_blend_data_t *bd = self->blend_data;
  if(!bd->blendif_support)
  {
    dt_control_log(_("this module does not support parametric masks"));
    return;
  }

  // the add-parametric controls are flexi-only, so the module is normally already
  // in flexi; only if it is in neither flexi nor drawn mode do we switch it into
  // flexi so the group (and the parametric form inside it) is evaluated. Forcing
  // drawn mode here would hide the flexi-only panel and the new row.
  if(!(self->blend_params->mask_mode & (DEVELOP_MASK_MASK | DEVELOP_MASK_FLEXI)))
    _blendop_mask_enable(self);
  dt_iop_request_focus(self);

  dt_masks_form_t *form = dt_masks_create(DT_MASKS_PARAMETRIC);
  dt_masks_point_parametric_t *p = calloc(1, sizeof(dt_masks_point_parametric_t));
  const dt_develop_blend_params_t *dp = self->default_blendop_params;
  // p->blendif stays 0, whatever the module's defaults hold: nothing in the
  // row edits the polarity bit (see _update_param_row_display), so one set
  // here would disagree with the element's invert and its handle icon
  memcpy(p->blendif_parameters, dp->blendif_parameters, sizeof(p->blendif_parameters));
  memcpy(p->blendif_boost_factors, dp->blendif_boost_factors,
         sizeof(p->blendif_boost_factors));
  p->colorspace = (uint32_t)self->blend_params->blend_cst;
  p->channel = (uint32_t)channel_idx;
  p->in_out = (uint32_t)in_out;
  // new parametric channel masks start collapsed -- a compact, input-only
  // slider (p->in_out defaults to 0/input-only above); see
  // _update_param_row_visibility.
  form->points = g_list_append(form->points, p);

  dt_print(DT_DEBUG_MASKS,
           "[masks] add single-channel parametric form to '%s' (ch=%d io=%d)", self->op,
           channel_idx, in_out);

  // register and add to the module's group (group creation, numbering,
  // history)
  dt_masks_gui_form_save_creation(darktable.develop, self, form, NULL);

  // build the list so the new form gets its own row -- its editor is always
  // visible/live, no separate "open for editing" step needed
  dt_masks_gui_build_list(self);
}

// one-click "add parametric" channel button (flexi row). Adds a single-channel
// form for the button's channel, on the input sub-channel.
static void _param_channel_clicked(GtkButton *button, gpointer user_data)
{
  if(DT_IN_GUI_UPDATE()) return;
  dt_iop_module_t *self = user_data;
  const int ch = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "param-channel"));
  _add_parametric_channel(self, ch, 0);
}

static void _param_channel_button_enter_cb(GtkEventControllerMotion *controller,
                                           double x, double y,
                                           dt_iop_module_t *module)
{
  DT_GUARD_GUI_UPDATE();

  GtkWidget *widget = dt_gui_get_widget(controller);
  dt_iop_gui_blend_data_t *bd = module->blend_data;

  // the button stands for one channel of the module's blend colorspace; the
  // row is rebuilt whenever that colorspace changes, so the index still fits
  dt_dev_pixelpipe_display_mask_t channel = DT_DEV_PIXELPIPE_DISPLAY_NONE;
  const int ch = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "param-channel"));
  const dt_iop_gui_blendif_channel_t *channels =
    dt_develop_blendif_channels_for_csp(bd->csp);
  if(ch >= 0 && ch < _channel_count(channels)) channel = channels[ch].display_channel;

  _preview_on_hover_enter(module, widget, channel);
}

static void _param_channel_button_leave_cb(GtkEventControllerMotion *controller,
                                           dt_iop_module_t *module)
{
  DT_GUARD_GUI_UPDATE();

  _preview_on_hover_leave(module, dt_gui_get_widget(controller));
}

// (re)build the flexi-only "add parametric" row: one flat, CSS-themeable button
// (styled like the add-shape buttons) per channel of the module's blend
// colorspace. Rebuilt only when the csp changes. The row's own visibility is
// toggled per mode by the mask-mode callbacks.
static void _rebuild_param_channel_buttons(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!bd->masks_param_channels_inner) return;
  if(bd->param_channels_csp == (int)bd->csp) return; // already built for this csp
  bd->param_channels_csp = (int)bd->csp;

  // dt_action_define_iop below keeps a per-instance referral to each button in
  // module->widget_list, with nothing to drop it again. Left in place, the old
  // buttons' referrals dangle, and the next dt_accel_connect_instance_iop (a
  // history step toggling the module, say) crashes on them. They are plain
  // buttons, so they sit ahead of the bauhaus tail at widget_list_bh
  GList *old = gtk_container_get_children(GTK_CONTAINER(bd->masks_param_channels_inner));
  for(GSList **link = &module->widget_list; *link && *link != module->widget_list_bh;)
  {
    dt_action_target_t *referral = (*link)->data;
    if(g_list_find(old, referral->target))
    {
      GSList *dead = *link;
      *link = dead->next;
      g_free(referral);
      g_slist_free_1(dead);
    }
    else
      link = &(*link)->next;
  }
  g_list_free(old);

  dt_gui_container_destroy_children(GTK_CONTAINER(bd->masks_param_channels_inner));

  const dt_iop_gui_blendif_channel_t *channels =
    dt_develop_blendif_channels_for_csp(bd->csp);
  if(!channels) return;

  int idx = 0;
  for(const dt_iop_gui_blendif_channel_t *ch = channels; ch->label; ch++, idx++)
  {
    GtkWidget *btn = gtk_button_new_with_label(_(ch->label));
    dt_gui_add_class(btn, "dt_transparent_background");
    dt_gui_add_class(btn, "dt_masks_channel_add");
    gtk_widget_set_tooltip_text(btn, _(ch->tooltip));
    _stash_base_tooltip(btn);
    g_object_set_data(G_OBJECT(btn), "param-channel", GINT_TO_POINTER(idx));
    g_signal_connect(G_OBJECT(btn), "clicked", G_CALLBACK(_param_channel_clicked),
                     module);
    dt_gui_connect_motion(btn, NULL, _param_channel_button_enter_cb,
                          _param_channel_button_leave_cb, module);
    gtk_widget_show(btn);
    dt_gui_box_add(bd->masks_param_channels_inner, btn);
    // makes each channel button individually shortcut-assignable, like the
    // add-shape buttons (dt_iop_togglebutton_new does this internally for
    // those; this is a plain gtk_button_new, rebuilt per csp, so it needs the
    // call explicitly every time it is (re)created)
    dt_action_define_iop(module, "blend`shapes", ch->label, btn, &dt_action_def_button);
  }
}

// add a raster mask element referencing the given upstream source module + mask
// id. Raster elements are first-class: several can coexist in a module's group,
// each referencing a different source -- exactly like shapes and parametric
// channels. The source->this-module
// dependency is wired at commit time by _reconcile_raster_form_users
// (imageop.c), which registers every raster FORM's source (and survives edit
// reload), so nothing here touches the single legacy blend_params raster sink.
static void _add_raster_mask(dt_iop_module_t *self,
                             dt_iop_module_t *src,
                             const dt_mask_id_t id)
{
  dt_iop_gui_blend_data_t *bd = self->blend_data;
  if(!bd->masks_support || !src) return;

  // as with the parametric add controls, make sure the module is in a mode where
  // the group (and the raster element inside it) is evaluated
  if(!(self->blend_params->mask_mode & (DEVELOP_MASK_MASK | DEVELOP_MASK_FLEXI)))
    _blendop_mask_enable(self);
  dt_iop_request_focus(self);

  // if the source is not already storing a raster mask for anyone, it must be
  // reprocessed so it starts storing one (its cache is otherwise valid and would
  // not recompute); the commit-time reconciliation registers us as a user first.
  const gboolean reprocess = !dt_iop_is_raster_mask_used(src, id);

  dt_masks_form_t *form = dt_masks_create(DT_MASKS_RASTER);
  dt_masks_point_raster_t *p = calloc(1, sizeof(dt_masks_point_raster_t));
  dt_strlcpy_to_fixed(p->source, src->op, sizeof(p->source));
  p->instance = src->multi_priority;
  p->id = id;
  form->points = g_list_append(form->points, p);

  dt_print(DT_DEBUG_MASKS, "[masks] add raster form to '%s' from '%s' id=%d", self->op,
           src->op, id);

  // registers the form + adds it to the module's group (records masks history,
  // which reprocesses -> commits -> reconciles the raster source registration)
  dt_masks_gui_form_save_creation(darktable.develop, self, form, NULL);

  // named by its type alone, the element shows its source's current name (see
  // dt_masks_gui_form_display_name). Set AFTER save_creation, whose de-dup numbering names
  // it "raster mask #N"
  dt_strlcpy_to_fixed(form->name, _("raster mask"), sizeof(form->name));
  dt_dev_add_masks_history_item(darktable.develop, self, TRUE);

  dt_masks_gui_build_list(self);
  // full reprocess so the (possibly newly-used) source recomputes and stores its
  // mask, and so this module's commit re-runs the source reconciliation
  if(reprocess) dt_dev_reprocess_all(self->dev);
}

// ---- shortcut actions on "whatever is currently selected in the panel" -----
// They act on the focused module's panel selection, so they have no widget of
// their own to bind to, and each calls the helper its click handler calls
// (_invert_element, _invert_group_members, _group_toggle_bypass,
// _build_flexi_op_menu, _stage_new_group). A command action carries no module,
// so they act on dt_dev_gui_module(), the one instance whose panel selection
// means anything

// the focused module's panel data, NULL without one
static dt_iop_gui_blend_data_t *_shortcut_target(void)
{
  dt_iop_module_t *module = dt_dev_gui_module();
  return module ? module->blend_data : NULL;
}

// the group selected in the focused module's panel, INVALID_MASKID for none
static dt_mask_id_t _shortcut_selected_group(void)
{
  const dt_iop_gui_blend_data_t *bd = _shortcut_target();
  return bd && dt_masks_gui_module_mask_group(bd->module) ? bd->panel_selected_group_cid
                                                          : INVALID_MASKID;
}

static void _shortcut_add_group_above_selected(dt_action_t *action)
{
  dt_iop_gui_blend_data_t *bd = _shortcut_target();
  if(bd && bd->masks_inited) _stage_new_group(bd->module, bd->masks_new_group_op);
}

static void _shortcut_invert_selected_group(dt_action_t *action)
{
  const dt_mask_id_t cid = _shortcut_selected_group();
  if(dt_is_valid_maskid(cid)) _invert_group_members(dt_dev_gui_module(), cid);
}

static void _shortcut_invert_selected_element(dt_action_t *action)
{
  dt_iop_gui_blend_data_t *bd = _shortcut_target();
  if(bd && dt_is_valid_maskid(bd->panel_selected_formid))
    _invert_element(bd->module, bd->panel_selected_formid);
}

static void _shortcut_toggle_soloedit(dt_action_t *action)
{
  dt_iop_gui_blend_data_t *bd = _shortcut_target();
  if(!bd || !bd->soloedit_mode) return;
  // drive the header toggle, not bd->soloedit_formid: solo edit follows the
  // selection, which would undo an isolation set by hand
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->soloedit_mode),
                               !_soloedit_mode_is_on());
}

static void _shortcut_change_group_mode(dt_action_t *action)
{
  const dt_mask_id_t cid = _shortcut_selected_group();
  if(!dt_is_valid_maskid(cid)) return;
  dt_iop_gui_blend_data_t *bd = _shortcut_target();
  GtkWidget *anchor = bd->masks_list_box ? GTK_WIDGET(bd->masks_list_box) : bd->module->widget;
  _build_flexi_op_menu(anchor, bd->module, cid);
}

// toggle "bypass" on the selected group: the keyboard counterpart of the
// "disable" entry in the group's actions menu (see _build_group_actions_menu).
// Worth its own shortcut because it is meant to be flipped back and forth while
// judging an edit.
static void _shortcut_toggle_group_bypass(dt_action_t *action)
{
  const dt_mask_id_t cid = _shortcut_selected_group();
  if(dt_is_valid_maskid(cid)) _group_toggle_bypass(dt_dev_gui_module(), cid);
}

// the panel options (see _add_masks_panel_options_box) are check buttons in a
// popover that is rebuilt from conf on every opening, so there is no persistent
// widget for dt_action_define_iop to bind to. These give each option a
// bindable shortcut anyway, flipping the same conf key the check button does.
static void _shortcut_toggle_preview_on_hover(dt_action_t *action)
{
  _preview_on_hover_set(!_preview_on_hover_is_on());
}

static void _shortcut_toggle_sticky_opacity(dt_action_t *action)
{
  dt_conf_set_bool("plugins/darkroom/masks/opacity_not_sticky",
                   !dt_conf_get_bool("plugins/darkroom/masks/opacity_not_sticky"));
}

static void _shortcut_toggle_auto_expand_selected(dt_action_t *action)
{
  const gboolean on = !_auto_expand_selected();
  dt_conf_set_bool("plugins/darkroom/masks/auto_expand_selected", on);
  // read at row-build time, not hashed by dt_masks_gui_list_signature -- same
  // invalidation (and same parametric-row kick) the menu item's own callback
  // does, see _masks_auto_expand_selected_toggled
  dt_iop_gui_blend_data_t *bd = _shortcut_target();
  if(bd)
  {
    if(on) _auto_expand_selected_row(bd->module, bd->panel_selected_formid);
    _masks_rebuild_for_option(bd->module);
  }
}

// a command action, not a widget action on bd->flexi_inline_collapse_btn: the
// button is hidden in the utility-lib position, and _process_action does not
// run a widget action whose target is invisible (dt_action_widget_invisible in
// gui/accelerators.c). The click handler dispatches on masks_panel_position
static void _shortcut_toggle_masks_panel(dt_action_t *action)
{
  dt_iop_gui_blend_masks_panel_toggle();
}

// the "blend mask" caption folds the panel like the arrow beside it. The
// sequence is claimed so that, in the canvas panel, the header around it
// (_flexi_header_pressed in gui/gtk.c) does not fold it a second time
static void _masks_caption_clicked(GtkGestureSingle *gesture,
                                   gint n_press,
                                   gdouble x,
                                   gdouble y,
                                   gpointer user_data)
{
  if(gtk_gesture_single_get_current_button(gesture) != GDK_BUTTON_PRIMARY) return;
  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  if(n_press == 1) dt_masks_gui_flexi_inline_collapse_clicked(NULL, user_data);
}

// register every panel-selection shortcut above under "<blending> / masks", the
// same shared tree the panel's own widget actions land in (dt_action_define_iop
// routes a "blend`masks" section to darktable.control->actions_blend). Not under
// module->so: none acts on a particular operation (each resolves its module
// through dt_dev_gui_module()), and the same entries would be listed again
// under every module that supports masking. Called once per instance init;
// repeated registration of a path that already exists is expected and harmless
// (dt_action_register only fills in a node still typed as a section).
static void _register_masks_action_shortcuts(void)
{
  dt_action_t *masks = dt_action_section(&darktable.control->actions_blend, N_("masks"));

  dt_action_register(masks, N_("show/hide mask panel"),
                     _shortcut_toggle_masks_panel, 0, 0);
  dt_action_register(masks, N_("add group above selected group"),
                     _shortcut_add_group_above_selected, 0, 0);
  dt_action_register(masks, N_("invert all elements of selected group"),
                     _shortcut_invert_selected_group, 0, 0);
  dt_action_register(masks, N_("invert selected element"),
                     _shortcut_invert_selected_element, 0, 0);
  dt_action_register(masks, N_("toggle solo edit"),
                     _shortcut_toggle_soloedit, 0, 0);
  dt_action_register(masks, N_("change operator of selected group"),
                     _shortcut_change_group_mode, 0, 0);
  dt_action_register(masks, N_("bypass/resume current group"),
                     _shortcut_toggle_group_bypass, 0, 0);
  dt_action_register(masks, N_("preview channel under cursor"),
                     _shortcut_toggle_preview_on_hover, 0, 0);
  dt_action_register(masks, N_("sticky opacity"),
                     _shortcut_toggle_sticky_opacity, 0, 0);
  dt_action_register(masks, N_("auto-expand selected"),
                     _shortcut_toggle_auto_expand_selected, 0, 0);
}

void dt_iop_gui_init_masks(GtkWidget *blendw, dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;

  /* create and add masks support if module supports it */
  if(bd->masks_support)
  {
    bd->masks_shown = DT_MASKS_EDIT_OFF;

    // flexi-only: opens the import menu (see _masks_import_btn_pressed)
    bd->masks_import_btn = dtgtk_button_new(dtgtk_cairo_paint_import, 0, NULL);
    gtk_widget_set_tooltip_text(bd->masks_import_btn,
                                _("link or copy shapes from other modules, copy their parametric\n"
                                  "channels, or add or use another module's whole mask\n"
                                  "(click to pick)"));
    _press_before_widget(dt_gui_connect_click(bd->masks_import_btn, _masks_import_btn_pressed,
                                              NULL, module));

    // default operator for a newly added group
    bd->masks_new_group_op = DT_MASKS_STATE_UNION;

    // ---- masks_toolbar's "add an element" actions (see its field comment in
    // blend.h): add group, shapes, parametric channels and link or copy. The
    // toolbar itself is built once they are all filled, further down

    // "add group": a plain "+" that opens the operator chooser (its icon is a
    // fixed add affordance, it never reflects the selection). It leads the
    // shapes: it adds to the mask as the shape buttons do
    bd->masks_new_op_box = _make_op_combo(&bd->masks_new_op, dtgtk_cairo_paint_plus,
                                          _new_shape_op_pressed);
    // the add-group button is a plain "+" icon, not a bordered chooser: drop the
    // "dt_masks_op_combo" border so there is no white outline around it
    dt_gui_remove_class(bd->masks_new_op_box, "dt_masks_op_combo");
    g_object_set_data(G_OBJECT(bd->masks_new_op), "module", module);
    _new_shape_op_update(bd->masks_new_op);
    gtk_widget_show(bd->masks_new_op_box);

    // solo edit sits on the panel header, next to "edit on canvas": it is used
    // interactively, and its state has to be visible while editing. The channel
    // preview is a set-once mode, so it lives in the panel options instead (see
    // _add_masks_panel_options_box).
    bd->soloedit_mode = dt_iop_togglebutton_new(
      module, "blend`tools", N_("solo edit the selection"), NULL,
      G_CALLBACK(_soloedit_mode_toggled), FALSE, 0, 0,
      dtgtk_cairo_paint_soloedit, NULL);
    gtk_widget_set_tooltip_text
      (bd->soloedit_mode,
       _("solo edit the selection\n"
         "while enabled, only the selected shape, or the shapes of the selected\n"
         "group, are shown and editable on canvas; the other elements still\n"
         "contribute to the mask"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->soloedit_mode),
                                 _soloedit_mode_is_on());
    gtk_widget_set_no_show_all(bd->soloedit_mode, TRUE);

    // "edit on canvas": toggles the on-canvas editing overlay (the shape
    // controls). On the panel header, with solo edit (see below)
    bd->masks_edit = dt_iop_togglebutton_new(
      module, "blend`tools", N_("edit on canvas"),
      N_("edit on canvas in restricted mode (no moving or resizing of shapes)"),
      G_CALLBACK(_blendop_masks_show_and_edit), FALSE, 0, 0, dtgtk_cairo_paint_masks_eye,
      NULL);
    gtk_widget_set_tooltip_text(
      bd->masks_edit,
      _("edit drawn mask elements on canvas\n"
        "ctrl+click for restricted mode (no moving or resizing of shapes)"));

    // the shape-add buttons, wrapped as one group so the toolbar can space
    // them as a unit
    static const struct
    {
      dt_masks_type_t type;
      const char *label, *ctrl_label;
      DTGTKCairoPaintIconFunc paint;
    } shape_buttons[DEVELOP_MASKS_NB_SHAPES] = {
      { DT_MASKS_PATH, N_("add path"), N_("add multiple paths"), dtgtk_cairo_paint_masks_path },
      { DT_MASKS_BRUSH, N_("add brush"), N_("add multiple brush strokes"),
        dtgtk_cairo_paint_masks_brush },
      { DT_MASKS_CIRCLE, N_("add circle"), N_("add multiple circles"),
        dtgtk_cairo_paint_masks_circle },
      { DT_MASKS_ELLIPSE, N_("add ellipse"), N_("add multiple ellipses"),
        dtgtk_cairo_paint_masks_ellipse },
      { DT_MASKS_GRADIENT, N_("add gradient"), N_("add multiple gradients"),
        dtgtk_cairo_paint_masks_gradient },
#ifdef HAVE_AI
      { DT_MASKS_OBJECT, N_("add AI object"), NULL, dtgtk_cairo_paint_masks_object },
#endif
    };
    GtkWidget *shapes_box = dt_gui_hbox();
    for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
    {
      bd->masks_type[n] = shape_buttons[n].type;
      bd->masks_shapes[n] = dt_iop_togglebutton_new(
        module, "blend`shapes", shape_buttons[n].label, shape_buttons[n].ctrl_label,
        G_CALLBACK(_blendop_masks_add_shape), FALSE, 0, 0, shape_buttons[n].paint, NULL);
      gtk_widget_show(bd->masks_shapes[n]);
      dt_gui_box_add(shapes_box, bd->masks_shapes[n]);
      _stash_base_tooltip(bd->masks_shapes[n]);
    }

    bd->panel_selected_formid = INVALID_MASKID;
    bd->panel_selected_group_cid = INVALID_MASKID;
    bd->solo_formid = INVALID_MASKID;
    bd->masks_row_click_entered = INVALID_MASKID;

    // ---- "add parametric" cluster (flexi-only, ahead of link or copy):
    // one flat button per channel of the module's blend colorspace,
    // populated lazily by _rebuild_param_channel_buttons once the csp is
    // known. Visibility is toggled per mode alongside the rest of the
    // flexi-only widgets.
    bd->masks_param_channels_box = dt_gui_hbox();
    bd->param_channels_csp = DEVELOP_BLEND_CS_NONE;
    gtk_widget_set_no_show_all(bd->masks_param_channels_box, TRUE);

    // the channel buttons live in an inner box (rebuilt per csp); it carries
    // no no_show_all of its own, so it stays realized -- the cluster's
    // visibility is driven by the outer box.
    bd->masks_param_channels_inner = dt_gui_hbox();
    dt_gui_box_add(bd->masks_param_channels_box, bd->masks_param_channels_inner);
    gtk_widget_show(bd->masks_param_channels_inner);
    gtk_widget_show(bd->masks_import_btn);
    gtk_widget_show(shapes_box);

    // the group layout presets build a whole set of groups at once, so they
    // sit apart from the runs, at the top right
    GtkWidget *presets_btn = dtgtk_button_new(dtgtk_cairo_paint_presets, 0, NULL);
    gtk_widget_set_tooltip_text(presets_btn, _("group layout presets, which build a whole"
                                               " set of groups at once"));
    _press_before_widget(dt_gui_connect_click(presets_btn, _masks_presets_pressed, NULL, module));
    gtk_widget_show(presets_btn);

    GtkWidget *toolbar_gap = dt_gui_hbox();
    dt_gui_add_class(toolbar_gap, "dt_masks_button_gap");
    gtk_widget_show(toolbar_gap);

    GtkWidget *toolbar = dt_masks_gui_toolbar_new(bd->masks_new_op_box, shapes_box,
                                                  bd->masks_param_channels_box,
                                                  bd->masks_import_btn, presets_btn,
                                                  toolbar_gap);
    gtk_widget_set_no_show_all(toolbar, TRUE);
    dt_gui_add_class(toolbar, "dt_masks_toolbar");
    bd->masks_toolbar = toolbar;

    // edit on canvas and solo edit, onto the panel header built before this
    _pack_header_edit_run(bd);

    // per-shape composition list (the groups), populated by dt_masks_gui_build_list()
    // whenever the module is in flexi-mask mode.
    bd->masks_list_box = GTK_BOX(dt_gui_vbox());
    gtk_widget_set_no_show_all(GTK_WIDGET(bd->masks_list_box), TRUE);
    // unique id for the panel's own top-level list container, alongside the
    // existing "dt_masks_list" class every nested list box in the panel shares
    gtk_widget_set_name(GTK_WIDGET(bd->masks_list_box), "masks-list-box");
    dt_gui_add_class(GTK_WIDGET(bd->masks_list_box), "dt_masks_list");
    // the list's one drop target (see _drop_target_at). Its motion handler
    // answers the drag status itself, so no default motion; and no default
    // drop, see _drop_drop
    gtk_drag_dest_set(GTK_WIDGET(bd->masks_list_box), 0, _mask_hdr_dnd,
                      G_N_ELEMENTS(_mask_hdr_dnd), GDK_ACTION_MOVE);
    g_signal_connect(G_OBJECT(bd->masks_list_box), "drag-motion", G_CALLBACK(_drop_motion),
                     module);
    g_signal_connect(G_OBJECT(bd->masks_list_box), "drag-leave", G_CALLBACK(_drop_leave),
                     module);
    g_signal_connect(G_OBJECT(bd->masks_list_box), "drag-drop", G_CALLBACK(_drop_drop),
                     NULL);
    g_signal_connect(G_OBJECT(bd->masks_list_box), "drag-data-received",
                     G_CALLBACK(_drop_received), module);

    // layout: toolbar -> element list, on one ground. The list opens on the
    // mask's own group, whose header carries the whole-mask actions
    bd->masks_list_area = dt_gui_vbox(toolbar, GTK_WIDGET(bd->masks_list_box));
    gtk_widget_set_name(bd->masks_list_area, "masks-list-area");
    gtk_widget_set_no_show_all(bd->masks_list_area, TRUE);
    bd->masks_box = GTK_BOX(dt_gui_vbox(bd->masks_list_area));
    _add_wrapped_box(blendw, bd->masks_box, "masks_drawn");

    bd->masks_inited = TRUE;
    _register_masks_action_shortcuts();
  }
}

void dt_iop_gui_cleanup_blending(dt_iop_module_t *module)
{
  if(!module->blend_data) return;
  dt_iop_gui_blend_data_t *bd = module->blend_data;

  // a last resort: dt_iop_gui_cleanup_module releases the panel before it
  // destroys the widgets (dt_iop_gui_blend_masks_panel_release). At quit the
  // host can go first, leaving bd->* pointing at dead widgets, so only the
  // host bookkeeping is done here
  if(darktable.develop->proxy.masks_flexi_host.hosted_module == module)
  {
    if(bd->relocatable_box && GTK_IS_WIDGET(bd->relocatable_box))
      dt_masks_gui_flexi_release(module);
    else
      darktable.develop->proxy.masks_flexi_host.hosted_module = NULL;
  }

  _preview_on_hover_cancel_dwell(bd);
  _row_hover_cancel(module);
  // not only on module teardown: the shortcut registration in imageop.c builds
  // and drops blending on a scratch instance
  DT_CONTROL_SIGNAL_DISCONNECT(_consumers_history_changed, module);

  dt_pthread_mutex_lock(&bd->lock);
  // a queued rebuild (_queue_masks_list_rebuild) would touch the widgets and
  // blend_data freed below
  if(bd->masks_rebuild_idle_id) g_source_remove(bd->masks_rebuild_idle_id);

  if(bd->masks_cluster_expanded) g_hash_table_destroy(bd->masks_cluster_expanded);
  if(bd->masks_props_expanded) g_hash_table_destroy(bd->masks_props_expanded);
  if(bd->masks_note_page) g_hash_table_destroy(bd->masks_note_page);
  if(bd->masks_note_open) g_hash_table_destroy(bd->masks_note_open);
  if(bd->masks_refine_bypassed) g_hash_table_destroy(bd->masks_refine_bypassed);
  if(bd->masks_row_map) g_hash_table_destroy(bd->masks_row_map);
  if(bd->group_ordinals) g_hash_table_destroy(bd->group_ordinals);
  dt_pthread_mutex_unlock(&bd->lock);
  dt_pthread_mutex_destroy(&bd->lock);

  g_free(module->blend_data);
  module->blend_data = NULL;
}


static gboolean _add_blendmode_combo(GtkWidget *combobox,
                                     const dt_develop_blend_mode_t start,
                                     const dt_develop_blend_mode_t end)
{
  return dt_bauhaus_combobox_add_introspection(combobox,
                                               NULL,
                                               dt_develop_blend_mode_names,
                                               start,
                                               end);
}

static GtkWidget *_combobox_new_from_list(dt_iop_module_t *module,
                                          const gchar *label,
                                          const dt_introspection_type_enum_tuple_t *list,
                                          uint32_t *field,
                                          const gchar *tooltip)
{
  GtkWidget *combo = dt_bauhaus_combobox_new(module);

  if(field)
    dt_bauhaus_widget_set_field(combo, field, DT_INTROSPECTION_TYPE_ENUM);
  dt_action_t *ac = dt_bauhaus_widget_set_label(combo, N_("blend"), label);
  gtk_widget_set_tooltip_text(combo, tooltip);
  dt_bauhaus_combobox_add_introspection(combo, ac, list, list[0].value, -1);

  return combo;
}

void dt_iop_gui_update_blending(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  dt_develop_blend_params_t *bp = module->blend_params;

  if(!(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
     || !bd
     || !bd->blend_inited)
    return;

  DT_ENTER_GUI_UPDATE();

  // update color space from parameters
  const dt_develop_blend_colorspace_t default_csp =
    dt_develop_blend_default_module_blend_colorspace(module);
  switch(default_csp)
  {
    case DEVELOP_BLEND_CS_RAW:
      bd->csp = DEVELOP_BLEND_CS_RAW;
      break;
    case DEVELOP_BLEND_CS_LAB:
    case DEVELOP_BLEND_CS_RGB_DISPLAY:
    case DEVELOP_BLEND_CS_RGB_SCENE:
      switch(bp->blend_cst)
      {
        case DEVELOP_BLEND_CS_LAB:
        case DEVELOP_BLEND_CS_RGB_DISPLAY:
        case DEVELOP_BLEND_CS_RGB_SCENE:
          bd->csp = bp->blend_cst;
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

  const gboolean is_mask_enabled = (bp->mask_mode != DEVELOP_MASK_DISABLED);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->mask_enable_toggle),
                               is_mask_enabled);
  _update_mask_enable_toggle_tooltip(bd->mask_enable_toggle, is_mask_enabled);
  _masks_panel_apply_shape_sensitivity(bd);
  if(bd->masks_blend_header)
  {
    if(is_mask_enabled)
      dt_gui_add_class(bd->masks_blend_header, "dt_masks_enabled");
    else
      dt_gui_remove_class(bd->masks_blend_header, "dt_masks_enabled");
  }
  if(darktable.develop->proxy.masks_flexi_host.hosted_module == module)
    dt_ui_flexi_panel_set_active(darktable.gui->ui, is_mask_enabled);

  const gboolean has_mask_display =
    (module->request_mask_display != DT_DEV_PIXELPIPE_DISPLAY_NONE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->showmask), has_mask_display);

  // the details refinement (bp->details) makes a real mask with no mask type
  // set (dt_develop_blend_process's `uniform` branch), so the show-mask and
  // suppress controls and the mask indicator count it too
  const gboolean valid_masking =
    (bp->mask_mode & ~DEVELOP_MASK_ENABLED) || bp->details != 0.0f;

  // (un)set the mask indicator
  dt_iop_add_remove_mask_indicator(module, valid_masking);
  _mask_lock_sync(module);

  // initialization of blending modes
  if(bd->csp != bd->blend_modes_csp)
  {
    dt_bauhaus_combobox_clear(bd->blend_modes_combo);

    if(bd->csp == DEVELOP_BLEND_CS_LAB
       || bd->csp == DEVELOP_BLEND_CS_RGB_DISPLAY
       || bd->csp == DEVELOP_BLEND_CS_RAW )
    {
      dt_bauhaus_combobox_add_section(bd->blend_modes_combo, _("normal & difference"));
      _add_blendmode_combo(bd->blend_modes_combo,
                           DEVELOP_BLEND_NORMAL2, DEVELOP_BLEND_DIFFERENCE2);
      _add_blendmode_combo(bd->blend_modes_combo,
                           DEVELOP_BLEND_BOUNDED, DEVELOP_BLEND_BOUNDED);
      dt_bauhaus_combobox_add_section(bd->blend_modes_combo, _("lighten"));
      _add_blendmode_combo(bd->blend_modes_combo,
                           DEVELOP_BLEND_LIGHTEN, DEVELOP_BLEND_LIGHTEN);
      _add_blendmode_combo(bd->blend_modes_combo,
                           DEVELOP_BLEND_ADD, DEVELOP_BLEND_ADD);
      _add_blendmode_combo(bd->blend_modes_combo,
                           DEVELOP_BLEND_SCREEN, DEVELOP_BLEND_SCREEN);
      dt_bauhaus_combobox_add_section(bd->blend_modes_combo, _("darken"));
      _add_blendmode_combo(bd->blend_modes_combo,
                           DEVELOP_BLEND_DARKEN, DEVELOP_BLEND_DARKEN);
      _add_blendmode_combo(bd->blend_modes_combo,
                           DEVELOP_BLEND_SUBTRACT, DEVELOP_BLEND_SUBTRACT);
      _add_blendmode_combo(bd->blend_modes_combo,
                           DEVELOP_BLEND_MULTIPLY, DEVELOP_BLEND_MULTIPLY);
      dt_bauhaus_combobox_add_section(bd->blend_modes_combo, _("contrast enhancing"));
      _add_blendmode_combo(bd->blend_modes_combo,
                           DEVELOP_BLEND_OVERLAY, DEVELOP_BLEND_PINLIGHT);

      if(bd->csp == DEVELOP_BLEND_CS_LAB
         || bd->csp == DEVELOP_BLEND_CS_RGB_DISPLAY)
      {
        dt_bauhaus_combobox_add_section(bd->blend_modes_combo, _("color channel"));
        if(bd->csp == DEVELOP_BLEND_CS_LAB)
          _add_blendmode_combo(bd->blend_modes_combo,
                               DEVELOP_BLEND_LAB_LIGHTNESS, DEVELOP_BLEND_LAB_COLOR);
        else
          _add_blendmode_combo(bd->blend_modes_combo,
                               DEVELOP_BLEND_RGB_R, DEVELOP_BLEND_HSV_COLOR);
        _add_blendmode_combo(bd->blend_modes_combo,
                             DEVELOP_BLEND_HUE, DEVELOP_BLEND_COLORADJUST);

        dt_bauhaus_combobox_add_section(bd->blend_modes_combo,
                                        _("chromaticity & lightness"));
        _add_blendmode_combo(bd->blend_modes_combo,
                             DEVELOP_BLEND_LIGHTNESS, DEVELOP_BLEND_CHROMATICITY);
      }
    }
    else if(bd->csp == DEVELOP_BLEND_CS_RGB_SCENE)
    {
      dt_bauhaus_combobox_add_section(bd->blend_modes_combo, _("normal & arithmetic"));
      _add_blendmode_combo(bd->blend_modes_combo,
                           DEVELOP_BLEND_NORMAL2, DEVELOP_BLEND_DIFFERENCE2);
      _add_blendmode_combo(bd->blend_modes_combo,
                           DEVELOP_BLEND_MULTIPLY, DEVELOP_BLEND_HARMONIC_MEAN);
      dt_bauhaus_combobox_add_section(bd->blend_modes_combo, _("color channel"));
      _add_blendmode_combo(bd->blend_modes_combo,
                           DEVELOP_BLEND_RGB_R, DEVELOP_BLEND_RGB_B);
      dt_bauhaus_combobox_add_section(bd->blend_modes_combo, _("chromaticity & lightness"));
      _add_blendmode_combo(bd->blend_modes_combo,
                           DEVELOP_BLEND_LIGHTNESS, DEVELOP_BLEND_CHROMATICITY);
    }
    bd->blend_modes_csp = bd->csp;
  }

  dt_develop_blend_mode_t blend_mode = bp->blend_mode & DEVELOP_BLEND_MODE_MASK;

  if(!dt_bauhaus_combobox_set_from_value(bd->blend_modes_combo, blend_mode))
  {
    // add deprecated blend mode
    dt_bauhaus_combobox_add_section(bd->blend_modes_combo, _("deprecated"));
    if(!_add_blendmode_combo(bd->blend_modes_combo, blend_mode, blend_mode))
    {
      // should never happen: unknown blend mode
      dt_control_log(_("unknown blend mode '%d' in module '%s'"), blend_mode, module->op);
      bp->blend_mode = DEVELOP_BLEND_NORMAL2;
      blend_mode = DEVELOP_BLEND_NORMAL2;
    }

    dt_bauhaus_combobox_set_from_value(bd->blend_modes_combo, blend_mode);
  }

  const gboolean blend_mode_reversed =
    (bp->blend_mode & DEVELOP_BLEND_REVERSE) == DEVELOP_BLEND_REVERSE;

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->blend_modes_blend_order),
                               blend_mode_reversed);

  dt_bauhaus_slider_set(bd->blend_mode_parameter_slider, bp->blend_parameter);
  gtk_widget_set_visible(bd->blend_mode_parameter_slider,
     _blendif_blend_parameter_enabled(bd->blend_modes_csp, bp->blend_mode));

  dt_bauhaus_combobox_set_from_value(bd->masks_combine_combo,
    bp->mask_combine & (DEVELOP_COMBINE_INV | DEVELOP_COMBINE_INCL));
  dt_bauhaus_slider_set(bd->opacity_slider, bp->opacity);
  if(bd->blend_opacity_lowop_badge)
    _update_blend_opacity_badge(bd->blend_opacity_lowop_badge, bp->opacity / 100.0f);
  dt_bauhaus_combobox_set_from_value(bd->masks_feathering_guide_combo, bp->feathering_guide);
  dt_bauhaus_slider_set(bd->feathering_radius_slider, bp->feathering_radius);
  dt_bauhaus_slider_set(bd->blur_radius_slider, bp->blur_radius);
  dt_bauhaus_slider_set(bd->brightness_slider, bp->brightness);
  dt_bauhaus_slider_set(bd->contrast_slider, bp->contrast);
  dt_bauhaus_slider_set(bd->details_slider, bp->details);
  _update_refine_sensitivity(module);

  // keep the flexi "add parametric" channel buttons in sync with the csp
  _rebuild_param_channel_buttons(module);

  dt_iop_gui_update_masks(module);

  /* now show hide controls as required, as _blendop_masks_mode_callback does */
  const dt_develop_mask_mode_t mask_mode = bp->mask_mode;
  const gboolean mask_enabled = mask_mode & DEVELOP_MASK_ENABLED;
  const gboolean mode_raster = mask_mode & DEVELOP_MASK_RASTER;
  const gboolean mode_drawn = mask_mode & DEVELOP_MASK_MASK;
  const gboolean mode_flexi = !mode_raster && (mask_enabled || (mask_mode & DEVELOP_MASK_FLEXI));
  const gboolean mode_parametric = mask_mode & DEVELOP_MASK_CONDITIONAL;

  _box_set_visible(bd->blend_box, TRUE);

  const dt_image_t img = module->dev->image_storage;
  gtk_widget_set_visible(bd->details_slider, dt_image_is_rawprepare_supported(&img));

  if(mask_enabled
     && ((bd->masks_inited && (mode_drawn || mode_flexi))
         || (bd->blendif_support && mode_parametric)))
  {
    gtk_widget_set_visible(GTK_WIDGET(bd->masks_combine_combo),
                           bd->blendif_support && mode_parametric);

    // the per-target refinement reset is flexi's own
    if(bd->masks_refine_reset_btn)
      gtk_widget_set_visible(bd->masks_refine_reset_btn, mode_flexi);

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
    }

    _box_set_visible(bd->refine_box, TRUE);
  }
  else
  {
    module->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->showmask), FALSE);
    // (re)set the header mask indicator too
    if(module->mask_indicator)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->mask_indicator), FALSE);

    // mask off: still shown, as the rest of the panel is
    _box_set_visible(bd->refine_box, !mask_enabled);
  }

  // mask off shows the flexi panel, live, rather than an empty panel
  if(bd->masks_inited && !mode_raster)
  {
    if(bd->masks_param_channels_box)
      gtk_widget_set_visible(bd->masks_param_channels_box, bd->blendif_support);
    gtk_widget_set_visible(bd->masks_list_area, TRUE);
    gtk_widget_set_visible(bd->masks_toolbar, TRUE);
    if(bd->soloedit_mode) gtk_widget_set_visible(bd->soloedit_mode, TRUE);
    gtk_widget_set_visible(GTK_WIDGET(bd->masks_list_box), TRUE);
    _box_set_visible(bd->masks_box, TRUE);
    _props_panel_show(bd);
    // (re)build the per-shape composition list for this module's group -- only
    // for a live mask; with the mask off the list keeps what it last held
    // unless the group was deleted or emptied, or it was never built. An off
    // mask shows the groups switching it on would show, so the toggle never
    // changes the structure the panel displays
    dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
    const gboolean has_group = grp && grp->points;
    const gboolean had_list = bd->masks_list_sig != DT_INVALID_HASH;
    if(mode_flexi || !has_group || !had_list) dt_masks_gui_build_list(module);
    // and nothing of an off mask belongs on canvas
    if(!mask_enabled) dt_masks_set_edit_mode(module, DT_MASKS_EDIT_OFF);
  }
  else
  {
    if(bd->masks_inited) dt_masks_set_edit_mode(module, DT_MASKS_EDIT_OFF);
    _box_set_visible(bd->masks_box, FALSE);
    _box_set_visible(bd->props_panel_box, FALSE);
  }

  _consumers_sync(module);

  // the parametric rows' pickers stand down with the list they sit in, shown
  // as above whether the mask is on or off
  if(bd->blendif_support && !(bd->masks_inited && !mode_raster))
    dt_iop_color_picker_reset(module, FALSE);

  // modules that can't be toggled on/off in the first place (see
  // module->hide_enable_button) don't get a blend-mask on/off control either
  gtk_widget_set_visible(bd->mask_enable_toggle, !module->hide_enable_button);
  gtk_widget_set_visible(bd->showmask, is_mask_enabled && !module->hide_enable_button);

  if(darktable.develop && darktable.develop->gui_module == module)
    dt_iop_gui_blend_masks_panel_relocate(module);

  DT_LEAVE_GUI_UPDATE();
}

// the mask overlay and "edit on canvas" mode the previously focused module had,
// handed over to the next focused module so masks of different modules can be
// compared and edited without switching them on each time
static dt_dev_pixelpipe_display_mask_t _focus_carried_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
static dt_masks_edit_mode_t _focus_carried_edit = DT_MASKS_EDIT_OFF;
static guint _focus_carry_drop_source = 0;

// focus left the module and went nowhere: nothing is handed on
static gboolean _focus_carry_drop(gpointer user_data)
{
  _focus_carry_drop_source = 0;
  _focus_carried_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
  _focus_carried_edit = DT_MASKS_EDIT_OFF;
  return G_SOURCE_REMOVE;
}

// a masked module without shapes has nothing to edit on canvas, so it holds the
// edit mode for the next module instead of dropping it (see
// dt_iop_gui_blending_lose_focus)
static dt_iop_module_t *_focus_edit_holder = NULL;

static void _carry_edit_to(dt_iop_module_t *module)
{
  const dt_masks_edit_mode_t carried = _focus_carried_edit;
  _focus_carried_edit = DT_MASKS_EDIT_OFF;
  _focus_edit_holder = NULL;

  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(carried == DT_MASKS_EDIT_OFF
     || !bd->masks_support
     || !bd->masks_edit
     || module->blend_params->mask_mode == DEVELOP_MASK_DISABLED
     || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(bd->masks_edit)))
    return;

  if(!_module_has_drawn_shapes(module))
  {
    _focus_carried_edit = carried;
    _focus_edit_holder = module;
    return;
  }

  bd->masks_shown = carried;
  DT_ENTER_GUI_UPDATE();
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), TRUE);
  DT_LEAVE_GUI_UPDATE();
  dt_masks_set_edit_mode(module, carried);
}

static void _carry_mask_display_to(dt_iop_module_t *module)
{
  const dt_dev_pixelpipe_display_mask_t carried = _focus_carried_mask_display;
  _focus_carried_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;

  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(!carried
     || !module->enabled
     || module->hide_enable_button
     || module->blend_params->mask_mode == DEVELOP_MASK_DISABLED
     || module->blend_colorspace(module, NULL, NULL) == IOP_CS_RAW
     || module->request_mask_display != DT_DEV_PIXELPIPE_DISPLAY_NONE)
    return;

  module->request_mask_display = carried;
  DT_ENTER_GUI_UPDATE();
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->showmask), TRUE);
  if(module->mask_indicator)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->mask_indicator), TRUE);
  DT_LEAVE_GUI_UPDATE();
  _refresh_mask_display(module);
}

void dt_iop_gui_blending_gain_focus(dt_iop_module_t *module)
{
  if(!module || !module->blend_data)
  {
    _focus_carried_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
    _focus_carried_edit = DT_MASKS_EDIT_OFF;
    return;
  }
  dt_iop_gui_blend_masks_panel_relocate(module);
  _carry_mask_display_to(module);
  _carry_edit_to(module);
  _consumers_sync(module);
}

void dt_iop_gui_blending_lose_focus(dt_iop_module_t *module)
{
  DT_GUARD_GUI_UPDATE();
  if(!module) return;

  // stepping into an AI object (see dt_masks_form_gui_t.entered_object) is
  // part of editing this module's mask on the canvas: it ends with it
  _step_object(module, INVALID_MASKID);

  const gboolean has_mask_display =
    module->request_mask_display
    & (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL);

  const gboolean suppress = module->suppress_mask;

  if((module->flags() & IOP_FLAGS_SUPPORTS_BLENDING) && module->blend_data)
  {
    dt_iop_gui_blend_data_t *bd = module->blend_data;

    // a running hover preview is not the overlay the user asked for;
    // save_for_leave holds that one. Channel displays are per-module, so
    // only the plain mask overlay travels. A focus on no module keeps it until
    // idle: expanding with single_module collapses the old module first, which
    // focuses NULL just before the new one (see _gui_set_single_expanded)
    dt_pthread_mutex_lock(&bd->lock);
    const dt_dev_pixelpipe_display_mask_t shown =
      bd->hover_preview_active ? bd->save_for_leave : module->request_mask_display;
    dt_pthread_mutex_unlock(&bd->lock);
    _focus_carried_mask_display = shown & DT_DEV_PIXELPIPE_DISPLAY_MASK;
    const gboolean editing =
      bd->masks_support && bd->masks_edit
      && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(bd->masks_edit));
    if(editing)
      _focus_carried_edit = bd->masks_shown;
    else if(module != _focus_edit_holder)
      _focus_carried_edit = DT_MASKS_EDIT_OFF;
    _focus_edit_holder = NULL;
    if(!darktable.develop->gui_module
       && (_focus_carried_mask_display || _focus_carried_edit)
       && !_focus_carry_drop_source)
      _focus_carry_drop_source = g_idle_add(_focus_carry_drop, NULL);

    // don't let the flexi masks panel content linger in a shared host once
    // its owning module loses focus
    if(darktable.develop->proxy.masks_flexi_host.hosted_module == module)
    {
      dt_masks_gui_flexi_release(module);
    }

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->showmask), FALSE);
    module->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
    module->suppress_mask = FALSE;

    // (re)set the header mask indicator too
    DT_ENTER_GUI_UPDATE();
    if(module->mask_indicator)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->mask_indicator), FALSE);
    DT_LEAVE_GUI_UPDATE();

    // unselect all tools
    if(bd->masks_support) _masks_canvas_off(bd);

    // request_mask_display was just cleared above, so a hover preview that was
    // running has nothing left to restore
    _preview_on_hover_cancel_dwell(bd);
    bd->hovered_channel_widget = NULL;
    dt_pthread_mutex_lock(&bd->lock);
    bd->hover_preview_active = FALSE;
    bd->save_for_leave = DT_DEV_PIXELPIPE_DISPLAY_NONE;
    dt_pthread_mutex_unlock(&bd->lock);

    // reprocess main center image if needed
    if(has_mask_display || suppress)
      _refresh_mask_display(module);
  }
}

// the blend opacity header stands in for its slider's hidden label and value:
// inset it by the slider's side margins, which the theme sets for every bauhaus
// widget, so that it lines up with the slider's bar and the blend mode combobox
static void _blend_opacity_header_follow_margins(GtkWidget *slider, GtkWidget *header)
{
  GtkBorder margin;
  gtk_style_context_get_margin(gtk_widget_get_style_context(slider),
                               gtk_widget_get_state_flags(slider), &margin);
  gtk_widget_set_margin_start(header, margin.left);
  gtk_widget_set_margin_end(header, margin.right);
}

void dt_iop_gui_init_blending(GtkWidget *iopw,
                              dt_iop_module_t *module)
{
  /* create and add blend mode if module supports it */
  if(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
  {
    DT_ENTER_GUI_UPDATE();
    --darktable.bauhaus->skip_accel;

    module->blend_data = g_malloc0(sizeof(dt_iop_gui_blend_data_t));
    dt_iop_gui_blend_data_t *bd = module->blend_data;
    dt_develop_blend_params_t *bp = module->blend_params;

    bd->iopw = iopw;
    bd->module = module;
    bd->csp = DEVELOP_BLEND_CS_NONE;
    bd->blend_modes_csp = DEVELOP_BLEND_CS_NONE;
    dt_iop_colorspace_type_t cst = module->blend_colorspace(module, NULL, NULL);
    bd->blendif_support = (cst == IOP_CS_LAB || cst == IOP_CS_RGB);
    bd->masks_support = !(module->flags() & IOP_FLAGS_NO_MASKS);

    dt_pthread_mutex_init(&bd->lock);

    // collapse control for the masking panel: in the separate flexi panel
    // (left/right) it folds the whole panel away to its canvas corner icon,
    // embedded in the module it folds the panel body away below this header
    // and doubles as the way back (see dt_masks_gui_flexi_inline_collapse_clicked, which
    // dispatches on the position, and sets the arrow direction and tooltip
    // to match). Hidden only in the utility-lib position, which collapses
    // via the lib's own expander header. A plain flat arrow with its own CSS
    // class, deliberately not styled like the on/off toggle next to it.
    bd->flexi_inline_collapse_btn =
      dtgtk_button_new(dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_LEFT, NULL);
    gtk_widget_set_name(bd->flexi_inline_collapse_btn, "masks-collapse");
    g_signal_connect(G_OBJECT(bd->flexi_inline_collapse_btn), "clicked",
                     G_CALLBACK(dt_masks_gui_flexi_inline_collapse_clicked), module);
    gtk_widget_set_no_show_all(bd->flexi_inline_collapse_btn, TRUE);
    gtk_widget_set_visible(bd->flexi_inline_collapse_btn, FALSE);
    gtk_widget_set_valign(bd->flexi_inline_collapse_btn, GTK_ALIGN_CENTER);
    // no dt_action_define_iop here: the matching shortcut is a command action
    // instead, so that it still works in the position that hides this button
    // (see _shortcut_toggle_masks_panel)

    // on/off toggle for the blend mask (DEVELOP_MASK_DISABLED or
    // DEVELOP_MASK_ENABLED | DEVELOP_MASK_FLEXI, see
    // _blendop_mask_enable_toggled)
    bd->mask_enable_toggle =
      dt_iop_togglebutton_new(module, "blend`masks", N_("mask enabled"), NULL,
                              G_CALLBACK(_blendop_mask_enable_toggled), FALSE, 0, 0,
                              dtgtk_cairo_paint_switch, NULL);
    _update_mask_enable_toggle_tooltip(bd->mask_enable_toggle, FALSE);
    // background always blends with the module's own background, on or off
    // -- only the glyph itself shows state
    dt_gui_add_class(bd->mask_enable_toggle, "dt_transparent_background");
    dt_gui_add_class(bd->mask_enable_toggle, "dt_masks_enable_toggle");
    gtk_widget_set_valign(bd->mask_enable_toggle, GTK_ALIGN_CENTER);

    // its own id rather than "iop-panel-label": embedded, this caption has to
    // line up with the module's name beside it, but the two headers are not
    // interchangeable and only the embedded position wants that metric (see
    // "#masks-header-caption" in darktable.css)
    GtkWidget *caption_label = dt_ui_label_new(_("blend mask"));
    gtk_widget_set_name(caption_label, "masks-header-caption");
    bd->masks_blend_header_label = caption_label;
    // expands over the gap up to the buttons, so all of it is the click target
    GtkWidget *caption_evb = dt_gui_expand(gtk_event_box_new());
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(caption_evb), FALSE);
    gtk_container_add(GTK_CONTAINER(caption_evb), caption_label);
    dt_gui_connect_click(caption_evb, _masks_caption_clicked, NULL, module);
    gtk_widget_set_tooltip_text(caption_evb, _("click to show or hide the mask panel"));

    // "blend mask" header, in one fixed reading order:
    //
    //   expander | title | <space> | lock | show_mask_overlay | <gap>
    //   | edit on canvas | solo edit | <gap> | on/off toggle
    //
    // The expander (the panel-collapse arrow, embedded position only) leads;
    // the caption follows; everything after the space closes on the right,
    // grouped into right_cluster below. The space in the middle is simply what
    // is left between the start-packed and end-packed halves. When docked in
    // the separate *right* panel, the expander moves to the far right -- see
    // _masks_header_apply_side. The blending options open on the on/off
    // toggle's right-click.
    GtkWidget *gbox =
      dt_gui_hbox(bd->flexi_inline_collapse_btn, caption_evb);
    dt_gui_add_class(gbox, "dt_section_label");
    dt_gui_add_help_link(gbox, "masks_blending");
    gtk_widget_set_name(gbox, "blending-tabs");
    // default to the embedded inset (see darktable.css's
    // "#blending-tabs.dt_masks_embedded"); dt_iop_gui_blend_masks_panel_relocate
    // toggles this off for the two hosted positions, which already provide
    // their own inset
    dt_gui_add_class(gbox, "dt_masks_embedded");
    // the blending tabs' own header; the panel can host it (see
    // dt_iop_gui_blend_masks_panel_relocate)
    bd->masks_blend_header = gbox;

    bd->showmask = dt_iop_togglebutton_new(
      module, "blend`tools", N_("display mask and/or color channel"), NULL,
      G_CALLBACK(_blendop_blendif_showmask_clicked), FALSE, 0, 0,
      dtgtk_cairo_paint_showmask, NULL);
    gtk_widget_set_valign(bd->showmask, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text
      (bd->showmask,
       _("display mask and/or color channel.\n"
         "ctrl+click to display mask,\n"
         "shift+click to display channel"));

    bd->mask_lock_btn = dt_iop_togglebutton_new(
      module, "blend`masks", N_("lock mask"), NULL,
      G_CALLBACK(_mask_lock_clicked), FALSE, 0, 0,
      dtgtk_cairo_paint_mask_lock, NULL);
    gtk_widget_set_valign(bd->mask_lock_btn, GTK_ALIGN_CENTER);
    // shown by _mask_lock_sync only, never by an ancestor's show_all
    gtk_widget_set_no_show_all(bd->mask_lock_btn, TRUE);

    // edit on canvas and solo edit, with a gap either side: they are built with
    // the rest of the mask controls, and packed in by _pack_header_edit_run.
    // Hidden until then, so a module without masks shows no stray gaps
    bd->masks_header_edit_box = dt_gui_hbox();
    dt_gui_add_class(bd->masks_header_edit_box, "dt_masks_button_row");
    gtk_widget_set_no_show_all(bd->masks_header_edit_box, TRUE);

    // right-hand cluster: lock, show_mask_overlay, edit run, on/off toggle
    GtkWidget *right_cluster = bd->masks_right_cluster =
      dt_gui_hbox(bd->mask_lock_btn, bd->showmask, bd->masks_header_edit_box,
                  bd->mask_enable_toggle);
    dt_gui_add_class(right_cluster, "dt_masks_button_row");
    gtk_widget_set_valign(right_cluster, GTK_ALIGN_CENTER);
    gtk_box_pack_end(GTK_BOX(gbox), right_cluster, FALSE, FALSE, 0);

    bd->blend_modes_combo = dt_bauhaus_combobox_new(module);
    dt_action_t * ac = dt_bauhaus_widget_set_label(bd->blend_modes_combo,
                                                   N_("blend"),
                                                   N_("mode"));
    // shown as "blend mode", against the mask elements' own modes and
    // operators; the action keeps its name (blend > mode), shortcuts with it
    dt_bauhaus_widget_set_label_text(bd->blend_modes_combo, _("blend mode"));
    dt_bauhaus_combobox_add_introspection(bd->blend_modes_combo, ac,
                                          dt_develop_blend_mode_names, -1, -1);
    gtk_widget_set_tooltip_text(bd->blend_modes_combo, _("choose blending mode"));

    g_signal_connect(G_OBJECT(bd->blend_modes_combo), "value-changed",
                     G_CALLBACK(_blendop_blend_mode_callback), bd);
    dt_gui_add_help_link(GTK_WIDGET(bd->blend_modes_combo),
                         "masks_blending_op");

    bd->blend_modes_blend_order = dt_iop_togglebutton_new
      (module, "blend`tools",
       N_("toggle blend order"), NULL,
       G_CALLBACK(_blendop_blend_order_clicked), FALSE,
       0, 0,
       dtgtk_cairo_paint_invert, NULL);
    gtk_widget_set_tooltip_text
      (bd->blend_modes_blend_order,
       _("toggle the blending order between the input and the output of the module,\n"
         "by default the output will be blended on top of the input,\n"
         "order can be reversed by clicking on the icon (input on top of output)"));

    bd->blend_mode_parameter_slider =
      dt_bauhaus_slider_new_with_range(module, -18.0f, 18.0f, 0, 0.0f, 3);
    dt_bauhaus_widget_set_field(bd->blend_mode_parameter_slider, &bp->blend_parameter, DT_INTROSPECTION_TYPE_FLOAT);
    dt_bauhaus_widget_set_label(bd->blend_mode_parameter_slider, N_("blend"), N_("fulcrum"));
    dt_bauhaus_slider_set_format(bd->blend_mode_parameter_slider, _(" EV"));
    dt_bauhaus_slider_set_soft_range(bd->blend_mode_parameter_slider, -3.0, 3.0);
    gtk_widget_set_tooltip_text(bd->blend_mode_parameter_slider,
                                _("adjust the fulcrum used by some blending"
                                  " operations"));
    gtk_widget_set_visible(bd->blend_mode_parameter_slider, FALSE);

    bd->opacity_slider = dt_bauhaus_slider_new_with_range(module, 0.0, 100.0, 0, 100.0, 0);
    dt_bauhaus_widget_set_field(bd->opacity_slider, &bp->opacity, DT_INTROSPECTION_TYPE_FLOAT);
    dt_bauhaus_widget_set_label(bd->opacity_slider, N_("blend"), N_("opacity"));
    dt_bauhaus_slider_set_format(bd->opacity_slider, "%");
    gtk_widget_set_tooltip_text(bd->opacity_slider,
                                _("set the opacity of the blending"));
    // no quad: its unused width would make the slider narrower
    dt_bauhaus_widget_set_quad_visibility(bd->opacity_slider, FALSE);
    dt_bauhaus_widget_hide_label(bd->opacity_slider);
    dt_gui_add_class(bd->opacity_slider, "dt_masks_blend_opacity_slider");
    module->fusion_slider = bd->opacity_slider;

    GtkWidget *opacity_header = dt_gui_hbox();
    // "blend opacity", against the mask elements' own opacity below; the
    // slider's action keeps its name (blend > opacity), shortcuts with it
    GtkWidget *opacity_lbl = gtk_label_new(_("blend opacity"));
    gtk_label_set_xalign(GTK_LABEL(opacity_lbl), 0.0f);
    dt_gui_box_add(opacity_header, dt_gui_expand(opacity_lbl));

    bd->blend_opacity_lowop_badge = _make_lowop_badge();
    // a text line, not a drawer: small enough not to make the line taller
    gtk_widget_set_size_request(bd->blend_opacity_lowop_badge, DT_PIXEL_APPLY_DPI(10),
                                DT_PIXEL_APPLY_DPI(10));
    GtkWidget *val_widget = _make_inline_opacity_value_widget(bd->opacity_slider, module);

    GtkWidget *val_box = dt_gui_hbox();
    dt_gui_box_add(val_box, bd->blend_opacity_lowop_badge, val_widget);

    gtk_box_pack_end(GTK_BOX(opacity_header), val_box, FALSE, FALSE, 0);

    g_signal_connect(G_OBJECT(bd->opacity_slider), "value-changed",
                     G_CALLBACK(_blend_opacity_slider_changed_cb), bd);

    GtkWidget *opacity_box = dt_gui_vbox();
    dt_gui_add_class(opacity_box, "dt_masks_blend_opacity_box");
    dt_gui_box_add(opacity_box, opacity_header, bd->opacity_slider);
    g_signal_connect_object(G_OBJECT(bd->opacity_slider), "style-updated",
                            G_CALLBACK(_blend_opacity_header_follow_margins),
                            opacity_header, 0);
    _blend_opacity_header_follow_margins(bd->opacity_slider, opacity_header);

    bd->masks_combine_combo = _combobox_new_from_list
      (module,
       N_("combine masks"),
       dt_develop_combine_masks_names, NULL,
       _("how to combine individual drawn mask and different channels of parametric mask"));
    g_signal_connect(G_OBJECT(bd->masks_combine_combo), "value-changed",
                     G_CALLBACK(_blendop_masks_combine_callback), bd);
    dt_gui_add_help_link(GTK_WIDGET(bd->masks_combine_combo),
                         "masks_combined");

    bd->details_slider = dt_bauhaus_slider_new_with_range(module, -1.0f, 1.0f, 0, 0.0f, 2);
    dt_bauhaus_widget_set_label(bd->details_slider, N_("blend"), N_("details threshold"));
    dt_bauhaus_slider_set_format(bd->details_slider, "%");
    gtk_widget_set_tooltip_text
      (bd->details_slider,
       _("adjust the threshold for the details mask (using raw data),\n"
         "positive values select areas with strong details,\n"
         "negative values select flat areas"));
    dt_bauhaus_widget_set_quad_visibility(bd->details_slider, FALSE);
    g_signal_connect(G_OBJECT(bd->details_slider), "value-changed",
                     G_CALLBACK(_refine_control_changed), bd);

    // the refinement controls are not bound to blend_params
    // (dt_bauhaus_widget_set_field): _refine_control_changed applies them to
    // the scope the selection implies, and only the global scope is in
    // blend_params
    bd->masks_feathering_guide_combo = _combobox_new_from_list(
      module, N_("feathering guide"), dt_develop_feathering_guide_names, NULL,
      _("choose to guide mask by input or output image and\n"
        "choose to apply feathering before or after mask blur"));
    g_signal_connect(G_OBJECT(bd->masks_feathering_guide_combo), "value-changed",
                     G_CALLBACK(_refine_control_changed), bd);

    bd->feathering_radius_slider =
      dt_bauhaus_slider_new_with_range(module, 0.0, 250.0, 0, 0.0, 1);
    dt_bauhaus_widget_set_label(bd->feathering_radius_slider,
                                N_("blend"), N_("feathering radius"));
    dt_bauhaus_slider_set_format(bd->feathering_radius_slider, _(" px"));
    gtk_widget_set_tooltip_text(bd->feathering_radius_slider,
                                _("spatial radius of feathering"));
    dt_bauhaus_widget_set_quad_visibility(bd->feathering_radius_slider, FALSE);
    g_signal_connect(G_OBJECT(bd->feathering_radius_slider), "value-changed",
                     G_CALLBACK(_refine_control_changed), bd);

    bd->blur_radius_slider =
      dt_bauhaus_slider_new_with_range(module, 0.0, 100.0, 0, 0.0, 1);
    dt_bauhaus_widget_set_label(bd->blur_radius_slider, N_("blend"), N_("blurring radius"));
    dt_bauhaus_slider_set_format(bd->blur_radius_slider, _(" px"));
    gtk_widget_set_tooltip_text(bd->blur_radius_slider,
                                _("radius for gaussian blur of blend mask"));
    dt_bauhaus_widget_set_quad_visibility(bd->blur_radius_slider, FALSE);
    g_signal_connect(G_OBJECT(bd->blur_radius_slider), "value-changed",
                     G_CALLBACK(_refine_control_changed), bd);

    bd->brightness_slider = dt_bauhaus_slider_new_with_range(module, -1.0, 1.0, 0, 0.0, 2);
    dt_bauhaus_widget_set_label(bd->brightness_slider, N_("blend"),
                                N_("mask brightness"));
    dt_bauhaus_slider_set_format(bd->brightness_slider, "%");
    gtk_widget_set_tooltip_text
      (bd->brightness_slider,
       _("shifts and tilts the tone curve of the blend mask to adjust its brightness\n"
         "without affecting fully transparent/fully opaque regions"));
    dt_bauhaus_widget_set_quad_visibility(bd->brightness_slider, FALSE);
    g_signal_connect(G_OBJECT(bd->brightness_slider), "value-changed",
                     G_CALLBACK(_refine_control_changed), bd);

    bd->contrast_slider = dt_bauhaus_slider_new_with_range(module, -1.0, 1.0, 0, 0.0, 2);
    dt_bauhaus_widget_set_label(bd->contrast_slider, N_("blend"), N_("mask contrast"));
    dt_bauhaus_slider_set_format(bd->contrast_slider, "%");
    gtk_widget_set_tooltip_text
      (bd->contrast_slider,
       _("gives the tone curve of the blend mask an s-like shape to "
         "adjust its contrast"));
    dt_bauhaus_widget_set_quad_visibility(bd->contrast_slider, FALSE);
    g_signal_connect(G_OBJECT(bd->contrast_slider), "value-changed",
                     G_CALLBACK(_refine_control_changed), bd);

    // the refinements: the target is named by the selection row above (see
    // _refine_update_header); on the bar, the disable button on the left and
    // the reset button by the arrow, so that the bar is about as heavy on
    // either side of its title. Both go insensitive while there is nothing to
    // reset or disable, which is what says there are no refinements
    bd->masks_refine_section_label = dt_ui_section_label_new(_("refinements"));
    gtk_widget_set_tooltip_text(bd->masks_refine_section_label,
                                _("refines the selected element or group, or the whole"
                                  " mask if nothing is selected\n"
                                  "click to expand or collapse"));
    _stash_base_tooltip(bd->masks_refine_section_label);

    bd->masks_refine_sliders_box = GTK_BOX(
      dt_gui_vbox(bd->details_slider, bd->masks_feathering_guide_combo,
                  bd->feathering_radius_slider, bd->blur_radius_slider,
                  bd->brightness_slider, bd->contrast_slider));
    gtk_widget_set_name(GTK_WIDGET(bd->masks_refine_sliders_box), "collapsible");
    dt_gui_add_class(GTK_WIDGET(bd->masks_refine_sliders_box), "dt_masks_selection_card");

    GtkWidget *refine_head = NULL;
    bd->masks_refine_expander =
      _section_new(DT_MASKS_SECTION_REFINE, bd->masks_refine_section_label,
                   _("toggle refinements section"), TRUE,
                   GTK_WIDGET(bd->masks_refine_sliders_box), &refine_head,
                   &bd->masks_refine_toggle_btn);

    bd->masks_refine_reset_btn = dtgtk_button_new(dtgtk_cairo_paint_reset, 0, NULL);
    gtk_widget_set_tooltip_text(bd->masks_refine_reset_btn,
                                _("reset the refinement of the current target"));
    g_signal_connect(G_OBJECT(bd->masks_refine_reset_btn), "clicked",
                     G_CALLBACK(_refine_reset_clicked), bd);
    gtk_widget_set_no_show_all(bd->masks_refine_reset_btn, TRUE);
    gtk_widget_set_visible(bd->masks_refine_reset_btn, FALSE);
    gtk_box_pack_end(GTK_BOX(refine_head), bd->masks_refine_reset_btn, FALSE, FALSE, 0);

    bd->masks_refine_bypass_btn =
      dtgtk_togglebutton_new(dtgtk_cairo_paint_eye_toggle, 0, NULL);
    dt_gui_add_class(bd->masks_refine_bypass_btn, "dt_masks_bypass");
    gtk_widget_set_tooltip_text(
      bd->masks_refine_bypass_btn,
      _("disable the refinement of this target, keeping its settings\n"
        "click again to enable it"));
    g_signal_connect(G_OBJECT(bd->masks_refine_bypass_btn), "toggled",
                     G_CALLBACK(_refine_bypass_toggled), module);
    gtk_box_pack_start(GTK_BOX(refine_head), bd->masks_refine_bypass_btn, FALSE, FALSE, 0);

    bd->masks_refine_scope_kind = REFINE_SCOPE_GLOBAL;
    bd->masks_refine_scope_formid = INVALID_MASKID;
    bd->canvas_hovered_formid = INVALID_MASKID;

    // relocatable_box holds the "blend mask" header (gbox) plus everything
    // below it, and is the unit that dt_iop_gui_blend_masks_panel_relocate() moves between
    // iopw (embedded, the default) and a flexi masks panel host (utility lib
    // or separate grid panel) -- the header travels together with the rest
    // of the content, not left behind. gbox is packed directly here (not
    // inside blend_box below) so it stays visible even while the mask is
    // off -- it's the only way back on.
    bd->relocatable_box = GTK_BOX(dt_gui_vbox());
    dt_gui_box_add(iopw, GTK_WIDGET(bd->relocatable_box));
    dt_gui_box_add(bd->relocatable_box, gbox);
    // ...and everything below the header goes into masks_panel_body, which
    // the embedded position folds as a unit, leaving the visibility of what
    // is inside alone (see its field comment). mask_panel, which the rest of
    // this function fills, is the body
    bd->masks_panel_body = GTK_BOX(dt_gui_vbox());
    // the one name to start a CSS tweak with (dev-doc/flexi_masks/styling.md)
    gtk_widget_set_name(GTK_WIDGET(bd->masks_panel_body), "masks-panel");
    dt_gui_box_add(bd->relocatable_box, GTK_WIDGET(bd->masks_panel_body));
    GtkWidget *mask_panel = GTK_WIDGET(bd->masks_panel_body);

    GtkWidget *box = dt_gui_vbox();
    bd->blend_box = GTK_BOX(dt_gui_vbox(
      dt_gui_hbox(dt_gui_expand(bd->blend_modes_combo), bd->blend_modes_blend_order),
      bd->blend_mode_parameter_slider, opacity_box));
    dt_gui_add_class(GTK_WIDGET(bd->blend_box), "dt_masks_blend_box");
    _add_wrapped_box(box, bd->blend_box, NULL);

    dt_gui_box_add(mask_panel, box);
    dt_iop_gui_init_masks(mask_panel, module);

    // "element properties in subpanel": a collapsible like the refinements',
    // its content following the selection (see _props_panel_sync), packed
    // above them below. Built always, shown only while the option is on (see
    // _props_panel_show)
    GtkWidget *props_label = dt_ui_section_label_new(_("properties"));
    gtk_widget_set_tooltip_text(
      props_label, _("the properties of the selected element or group, or the creation"
                     " controls of a shape being drawn\n"
                     "click to expand or collapse"));
    bd->props_panel_content = dt_gui_vbox();
    gtk_widget_set_name(bd->props_panel_content, "collapsible");
    dt_gui_add_class(bd->props_panel_content, "dt_masks_props_panel");
    dt_gui_add_class(bd->props_panel_content, "dt_masks_selection_card");
    bd->props_panel_expander =
      _section_new(DT_MASKS_SECTION_PROPS, props_label, _("toggle properties section"), TRUE,
                   bd->props_panel_content, NULL, &bd->props_panel_toggle_btn);
    bd->props_panel_formid = INVALID_MASKID;
    bd->props_panel_box = GTK_BOX(dt_gui_vbox(bd->props_panel_expander));

    // the selection panel: the selection's icon and name, then the
    // properties and the refinement acting on it, on a ground of its own as
    // the list has (#masks-selection-area in darktable.css). It is the one
    // child of a box, so that locking the mask and the mask modes show and
    // disable them together (see _mask_lock_sync). The properties keep their
    // own revealer inside it, since they show only while the option is on and
    // the selection has some (see _props_panel_show). Not named
    // #blending-box, whose padding would inset them once more than the
    // refinement beside them
    bd->masks_selection_area = dt_gui_vbox(
      _selection_row_new(&bd->masks_selection_icon_box, &bd->masks_selection_name_label));
    gtk_widget_set_name(bd->masks_selection_area, "masks-selection-area");
    _add_wrapped_box(bd->masks_selection_area, bd->props_panel_box, "masks_drawn");
    // renamed from the #blending-box _add_wrapped_box gives it, whose padding
    // would inset the properties once more than the refinement beside them
    gtk_widget_set_name(GTK_WIDGET(bd->props_panel_box), "masks-props-section");
    dt_gui_box_add(bd->masks_selection_area, bd->masks_refine_expander);
    bd->refine_box = GTK_BOX(dt_gui_vbox(bd->masks_selection_area));
    _add_wrapped_box(mask_panel, bd->refine_box, "masks_refinement");

    // "mask consumers": the modules reading this one's raster mask, shown only
    // while there are any (see _consumers_sync)
    GtkWidget *consumers_label = dt_ui_section_label_new(_("mask consumers"));
    gtk_widget_set_tooltip_text(
      consumers_label, _("the modules further down the pipe that use this module's mask as"
                         " a raster mask. click one of them to go to its mask panel\n"
                         "click here to expand or collapse"));
    bd->consumers_content = dt_gui_vbox();
    gtk_widget_set_name(bd->consumers_content, "collapsible");
    bd->consumers_expander =
      _section_new(DT_MASKS_SECTION_CONSUMERS, consumers_label,
                   _("toggle mask consumers section"), FALSE, bd->consumers_content, NULL,
                   &bd->consumers_toggle_btn);
    bd->consumers_sig = DT_INVALID_HASH;
    bd->consumers_box = GTK_BOX(dt_gui_vbox(bd->consumers_expander));
    _add_wrapped_box(mask_panel, bd->consumers_box, "masks_raster");
    // a consumer's own changes (a raster element added, its mask switched
    // off, a rename) reach this panel only through the history. Connected
    // with the module as user data, so dt_iop_gui_cleanup_module drops it
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_DEVELOP_HISTORY_CHANGE,
                              _consumers_history_changed, module);
    _sections_apply(bd);

    gtk_widget_set_name(GTK_WIDGET(iopw), "blending-wrapper");

    // masks_panel_body's visibility is the embedded fold, which the module
    // expander's show_all (dt_iop_gui_set_expander) must not reset: its
    // children are shown once, then it is set no_show_all
    gtk_widget_show_all(GTK_WIDGET(bd->masks_panel_body));
    gtk_widget_set_no_show_all(GTK_WIDGET(bd->masks_panel_body), TRUE);

    // the same for relocatable_box, whose visibility is focus: only the
    // focused module shows its panel, through
    // dt_iop_gui_blend_masks_panel_relocate and _release. It starts hidden
    gtk_widget_show_all(GTK_WIDGET(bd->relocatable_box));
    gtk_widget_hide(GTK_WIDGET(bd->relocatable_box));
    gtk_widget_set_no_show_all(GTK_WIDGET(bd->relocatable_box), TRUE);

    bd->blend_inited = TRUE;

    ++darktable.bauhaus->skip_accel;
    DT_LEAVE_GUI_UPDATE();
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
