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
#include "common/chromatic_adaptation.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/gamut_mapping.h"
#include "common/opencl.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/openmp_maths.h"
#include "develop/imageop_gui.h"
#include "dtgtk/drawingarea.h"
#include "dtgtk/gradientslider.h"
#include "gui/accelerators.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "gui/color_picker_proxy.h"
#include "iop/iop_api.h"

//#include <gtk/gtk.h>
#include <stdlib.h>
#define LUT_ELEM 360     // gamut LUT number of elements: resolution of 1°
#define STEPS 92         // so we test 92×92×92 combinations of RGB in [0; 1] to build the gamut LUT

// Filmlight Yrg puts red at 330°, while usual HSL wheels put it at 360/0°
// so shift in GUI only it to not confuse people. User params are always degrees,
// pixel params are always radians.
#define ANGLE_SHIFT -30.f
#define DEG_TO_RAD(x) ((x + ANGLE_SHIFT) * M_PI / 180.f)
#define RAD_TO_DEG(x) (x * 180.f / M_PI - ANGLE_SHIFT)

DT_MODULE_INTROSPECTION(5, dt_iop_colorbalancergb_params_t)

typedef enum dt_iop_colorbalancrgb_saturation_t
{
  DT_COLORBALANCE_SATURATION_JZAZBZ = 0, // $DESCRIPTION: "JzAzBz (2021)"
  DT_COLORBALANCE_SATURATION_DTUCS = 1   // $DESCRIPTION: "darktable UCS (2022)"
} dt_iop_colorbalancrgb_saturation_t;

typedef struct dt_iop_colorbalancergb_params_t
{
  /* params of v1 */
  float shadows_Y;             // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "luminance"
  float shadows_C;             // $MIN:  0.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma"
  float shadows_H;             // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
  float midtones_Y;            // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "luminance"
  float midtones_C;            // $MIN:  0.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma"
  float midtones_H;            // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
  float highlights_Y;          // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "luminance"
  float highlights_C;          // $MIN:  0.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma"
  float highlights_H;          // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
  float global_Y;              // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "luminance"
  float global_C;              // $MIN:  0.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma"
  float global_H;              // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
  float shadows_weight;        // $MIN:  0.0 $MAX:   3.0 $DEFAULT: 1.0 $DESCRIPTION: "shadows fall-off"
  float white_fulcrum;         // $MIN: -16.0 $MAX: 16.0 $DEFAULT: 0.0 $DESCRIPTION: "white fulcrum"
  float highlights_weight;     // $MIN:  0.0 $MAX:   3.0 $DEFAULT: 1.0 $DESCRIPTION: "highlights fall-off"
  float chroma_shadows;        // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "shadows"
  float chroma_highlights;     // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "highlights"
  float chroma_global;         // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "global chroma"
  float chroma_midtones;       // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "mid-tones"
  float saturation_global;     // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "global saturation"
  float saturation_highlights; // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "highlights"
  float saturation_midtones;   // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "mid-tones"
  float saturation_shadows;    // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "shadows"
  float hue_angle;             // $MIN: -180. $MAX: 180. $DEFAULT: 0.0 $DESCRIPTION: "hue shift"

  /* params of v2 */
  float brilliance_global;     // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "global brilliance"
  float brilliance_highlights; // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "highlights"
  float brilliance_midtones;   // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "mid-tones"
  float brilliance_shadows;    // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "shadows"

  /* params of v3 */
  float mask_grey_fulcrum;     // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.1845 $DESCRIPTION: "mask middle-gray fulcrum"

  /* params of v4 */
  float vibrance;         // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0 $DESCRIPTION: "global vibrance"
  float grey_fulcrum;     // $MIN:  0.0 $MAX: 1.0 $DEFAULT: 0.1845 $DESCRIPTION: "contrast gray fulcrum"
  float contrast;         // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0. $DESCRIPTION: "contrast"

  /* params of v5 */
  dt_iop_colorbalancrgb_saturation_t saturation_formula; // $DEFAULT: 1 $DESCRIPTION: "saturation formula"

  /* add future params after this so the legacy params import can use a blind memcpy */
} dt_iop_colorbalancergb_params_t;


typedef enum dt_iop_colorbalancergb_mask_data_t
{
  MASK_SHADOWS = 0,
  MASK_MIDTONES = 1,
  MASK_HIGHLIGHTS = 2,
  MASK_NONE
} dt_iop_colorbalancergb_mask_data_t;


typedef struct dt_iop_colorbalancergb_gui_data_t
{
  GtkWidget *shadows_H, *midtones_H, *highlights_H, *global_H;
  GtkWidget *shadows_C, *midtones_C, *highlights_C, *global_C;
  GtkWidget *shadows_Y, *midtones_Y, *highlights_Y, *global_Y;
  GtkWidget *shadows_weight, *mask_grey_fulcrum, *highlights_weight;
  GtkWidget *chroma_highlights, *chroma_global, *chroma_shadows, *chroma_midtones, *vibrance;
  GtkWidget *contrast, *grey_fulcrum, *white_fulcrum;
  GtkWidget *saturation_global, *saturation_highlights, *saturation_midtones, *saturation_shadows;
  GtkWidget *brilliance_global, *brilliance_highlights, *brilliance_midtones, *brilliance_shadows;
  GtkWidget *saturation_formula;
  GtkWidget *hue_angle;
  GtkDrawingArea *area;
  GtkNotebook *notebook;
  GtkWidget *checker_color_1_picker, *checker_color_2_picker, *checker_size;
  gboolean mask_display;
  dt_iop_colorbalancergb_mask_data_t mask_type;
  const dt_iop_order_iccprofile_info_t *sliders_output_profile;
} dt_iop_colorbalancergb_gui_data_t;


typedef struct dt_iop_colorbalancergb_data_t
{
  float global[4];
  float shadows[4];
  float highlights[4];
  float midtones[4];
  float midtones_Y;
  float chroma_global, chroma[4], vibrance, contrast;
  float saturation_global, saturation[4];
  float brilliance_global, brilliance[4];
  float hue_angle;
  float shadows_weight, highlights_weight, midtones_weight, mask_grey_fulcrum;
  float white_fulcrum, grey_fulcrum;
  float *gamut_LUT;
  float *chroma_LUT;
  float max_chroma;
  float checker_color_1[4], checker_color_2[4];
  dt_iop_colorbalancrgb_saturation_t saturation_formula;
  size_t checker_size;
  gboolean lut_inited;
  struct dt_iop_order_iccprofile_info_t *work_profile;
} dt_iop_colorbalancergb_data_t;

typedef struct dt_iop_colorbalance_global_data_t
{
  int kernel_colorbalance_rgb;
} dt_iop_colorbalancergb_global_data_t;


const char *name()
{
  return _("color balance rgb");
}

const char *aliases()
{
  return _("offset power slope|cdl|color grading|contrast|chroma_highlights|hue|vibrance|saturation");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("affect color, brightness and contrast"),
                                      _("corrective or creative"),
                                      _("linear, RGB, scene-referred"),
                                      _("non-linear, RGB"),
                                      _("non-linear, RGB, scene-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_group()
{
  return IOP_GROUP_COLOR | IOP_GROUP_GRADING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params,
                  const int new_version)
{
  if(old_version == 1 && new_version == 5)
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
      float white_fulcrum;       // $MIN: -6.0 $MAX:   6.0 $DEFAULT: 0.0 $DESCRIPTION: "fulcrum"
      float highlights_weight;     // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "tonal weight"
      float chroma_shadows;        // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "shadows"
      float chroma_highlights;     // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "highlights"
      float chroma_global;         // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "global"
      float chroma_midtones;       // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "mid-tones"
      float saturation_global;     // $MIN: -5.0 $MAX: 5.0 $DEFAULT: 0.0 $DESCRIPTION: "saturation global"
      float saturation_highlights; // $MIN: -0.2 $MAX: 0.2 $DEFAULT: 0.0 $DESCRIPTION: "highlights"
      float saturation_midtones;   // $MIN: -0.2 $MAX: 0.2 $DEFAULT: 0.0 $DESCRIPTION: "mid-tones"
      float saturation_shadows;    // $MIN: -0.2 $MAX: 0.2 $DEFAULT: 0.0 $DESCRIPTION: "shadows"
      float hue_angle;             // $MIN: -180. $MAX: 180. $DEFAULT: 0.0 $DESCRIPTION: "hue shift"
    } dt_iop_colorbalancergb_params_v1_t;

    // Init params with defaults
    memcpy(new_params, self->default_params, sizeof(dt_iop_colorbalancergb_params_t));

    // Copy the common part of the params struct
    memcpy(new_params, old_params, sizeof(dt_iop_colorbalancergb_params_v1_t));

    dt_iop_colorbalancergb_params_t *n = (dt_iop_colorbalancergb_params_t *)new_params;
    n->saturation_global /= 180.f / M_PI;
    n->mask_grey_fulcrum = 0.1845f;
    n->vibrance = 0.f;
    n->grey_fulcrum = 0.1845f;
    n->contrast = 0.f;
    n->saturation_formula = DT_COLORBALANCE_SATURATION_JZAZBZ;

    return 0;
  }

  if(old_version == 2 && new_version == 5)
  {
    typedef struct dt_iop_colorbalancergb_params_v2_t
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
      float shadows_weight;        // $MIN:  0.0 $MAX:   3.0 $DEFAULT: 1.0 $DESCRIPTION: "shadows fall-off"
      float white_fulcrum;         // $MIN: -6.0 $MAX:   6.0 $DEFAULT: 0.0 $DESCRIPTION: "white pivot"
      float highlights_weight;     // $MIN:  0.0 $MAX:   3.0 $DEFAULT: 1.0 $DESCRIPTION: "highlights fall-off"
      float chroma_shadows;        // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "shadows"
      float chroma_highlights;     // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "highlights"
      float chroma_global;         // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "global"
      float chroma_midtones;       // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "mid-tones"
      float saturation_global;     // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "global"
      float saturation_highlights; // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "highlights"
      float saturation_midtones;   // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "mid-tones"
      float saturation_shadows;    // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "shadows"
      float hue_angle;             // $MIN: -180. $MAX: 180. $DEFAULT: 0.0 $DESCRIPTION: "hue shift"

      /* params of v2 */
      float brilliance_global;     // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "global"
      float brilliance_highlights; // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "highlights"
      float brilliance_midtones;   // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "mid-tones"
      float brilliance_shadows;    // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "shadows"

    } dt_iop_colorbalancergb_params_v2_t;

    // Init params with defaults
    memcpy(new_params, self->default_params, sizeof(dt_iop_colorbalancergb_params_t));

    // Copy the common part of the params struct
    memcpy(new_params, old_params, sizeof(dt_iop_colorbalancergb_params_v2_t));

    dt_iop_colorbalancergb_params_t *n = (dt_iop_colorbalancergb_params_t *)new_params;
    n->mask_grey_fulcrum = 0.1845f;
    n->vibrance = 0.f;
    n->grey_fulcrum = 0.1845f;
    n->contrast = 0.f;
    n->saturation_formula = DT_COLORBALANCE_SATURATION_JZAZBZ;

    return 0;
  }
  if(old_version == 3 && new_version == 5)
  {
    typedef struct dt_iop_colorbalancergb_params_v3_t
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
      float shadows_weight;        // $MIN:  0.0 $MAX:   3.0 $DEFAULT: 1.0 $DESCRIPTION: "shadows fall-off"
      float white_fulcrum;         // $MIN: -16.0 $MAX:   16.0 $DEFAULT: 0.0 $DESCRIPTION: "white fulcrum"
      float highlights_weight;     // $MIN:  0.0 $MAX:   3.0 $DEFAULT: 1.0 $DESCRIPTION: "highlights fall-off"
      float chroma_shadows;        // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "shadows"
      float chroma_highlights;     // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "highlights"
      float chroma_global;         // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "global"
      float chroma_midtones;       // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "mid-tones"
      float saturation_global;     // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "global"
      float saturation_highlights; // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "highlights"
      float saturation_midtones;   // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "mid-tones"
      float saturation_shadows;    // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "shadows"
      float hue_angle;             // $MIN: -180. $MAX: 180. $DEFAULT: 0.0 $DESCRIPTION: "hue shift"

      /* params of v2 */
      float brilliance_global;     // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "global"
      float brilliance_highlights; // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "highlights"
      float brilliance_midtones;   // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "mid-tones"
      float brilliance_shadows;    // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "shadows"

      /* params of v3 */
      float mask_grey_fulcrum;     // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.1845 $DESCRIPTION: "middle-gray fulcrum"

    } dt_iop_colorbalancergb_params_v3_t;

    // Init params with defaults
    memcpy(new_params, self->default_params, sizeof(dt_iop_colorbalancergb_params_t));

    // Copy the common part of the params struct
    memcpy(new_params, old_params, sizeof(dt_iop_colorbalancergb_params_v3_t));

    dt_iop_colorbalancergb_params_t *n = (dt_iop_colorbalancergb_params_t *)new_params;
    n->vibrance = 0.f;
    n->grey_fulcrum = 0.1845f;
    n->contrast = 0.f;
    n->saturation_formula = DT_COLORBALANCE_SATURATION_JZAZBZ;

    return 0;
  }
  if(old_version == 4 && new_version == 5)
  {
    typedef struct dt_iop_colorbalancergb_params_v4_t
    {
      /* params of v1 */
      float shadows_Y;             // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "lift luminance"
      float shadows_C;             // $MIN:  0.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "lift chroma"
      float shadows_H;             // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "lift hue"
      float midtones_Y;            // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "power luminance"
      float midtones_C;            // $MIN:  0.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "power chroma"
      float midtones_H;            // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "power hue"
      float highlights_Y;          // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "gain luminance"
      float highlights_C;          // $MIN:  0.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "gain chroma"
      float highlights_H;          // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "gain hue"
      float global_Y;              // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "offset luminance"
      float global_C;              // $MIN:  0.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "offset chroma"
      float global_H;              // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "offset hue"
      float shadows_weight;        // $MIN:  0.0 $MAX:   3.0 $DEFAULT: 1.0 $DESCRIPTION: "shadows fall-off"
      float white_fulcrum;         // $MIN: -16.0 $MAX: 16.0 $DEFAULT: 0.0 $DESCRIPTION: "white fulcrum"
      float highlights_weight;     // $MIN:  0.0 $MAX:   3.0 $DEFAULT: 1.0 $DESCRIPTION: "highlights fall-off"
      float chroma_shadows;        // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma shadows"
      float chroma_highlights;     // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma highlights"
      float chroma_global;         // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma global"
      float chroma_midtones;       // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma mid-tones"
      float saturation_global;     // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "saturation global"
      float saturation_highlights; // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "saturation highlights"
      float saturation_midtones;   // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "saturation mid-tones"
      float saturation_shadows;    // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "saturation shadows"
      float hue_angle;             // $MIN: -180. $MAX: 180. $DEFAULT: 0.0 $DESCRIPTION: "hue shift"

      /* params of v2 */
      float brilliance_global;     // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "brilliance global"
      float brilliance_highlights; // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "brilliance highlights"
      float brilliance_midtones;   // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "brilliance mid-tones"
      float brilliance_shadows;    // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "brilliance shadows"

      /* params of v3 */
      float mask_grey_fulcrum;     // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.1845 $DESCRIPTION: "mask middle-gray fulcrum"

      /* params of v4 */
      float vibrance;         // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0 $DESCRIPTION: "global vibrance"
      float grey_fulcrum;     // $MIN:  0.0 $MAX: 1.0 $DEFAULT: 0.1845 $DESCRIPTION: "contrast gray fulcrum"
      float contrast;         // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0. $DESCRIPTION: "contrast"

    } dt_iop_colorbalancergb_params_v4_t;

    // Init params with defaults
    memcpy(new_params, self->default_params, sizeof(dt_iop_colorbalancergb_params_t));

    // Copy the common part of the params struct
    memcpy(new_params, old_params, sizeof(dt_iop_colorbalancergb_params_v4_t));

    dt_iop_colorbalancergb_params_t *n = (dt_iop_colorbalancergb_params_t *)new_params;
    n->saturation_formula = DT_COLORBALANCE_SATURATION_JZAZBZ;

    return 0;
  }

  return 1;
}

void init_presets(dt_iop_module_so_t *self)
{
  // Note : all the elements of the params structure are scalar floats,
  // so we can just init them all to 0.f in batch
  // Then, only 4 params have to be manually inited to non-zero values
  dt_iop_colorbalancergb_params_t p = { 0.f };
  p.shadows_weight = 1.f;        // DEFAULT: 1.0 DESCRIPTION: "shadows fall-off"
  p.highlights_weight = 1.f;     // DEFAULT: 1.0 DESCRIPTION: "highlights fall-off"
  p.mask_grey_fulcrum = 0.1845f; // DEFAULT: 0.1845 DESCRIPTION: "mask middle-gray fulcrum"
  p.grey_fulcrum = 0.1845f;      // DEFAULT: 0.1845 DESCRIPTION: "contrast gray fulcrum"
  p.saturation_formula = DT_COLORBALANCE_SATURATION_JZAZBZ;

  // preset
  p.chroma_global = 0.2f;
  p.saturation_shadows = 0.1f;
  p.saturation_midtones = 0.05f;
  p.saturation_highlights = -0.05f;

  dt_gui_presets_add_generic(_("add basic colorfulness (legacy)"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  p.saturation_formula = DT_COLORBALANCE_SATURATION_DTUCS;
  p.chroma_global = 0.f;

  p.saturation_global = 0.2f;
  p.saturation_shadows = 0.30f;
  p.saturation_midtones = 0.f;
  p.saturation_highlights = -0.5f;
  dt_gui_presets_add_generic(_("basic colorfulness: natural skin"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  p.saturation_global = 0.2f;
  p.saturation_shadows = 0.5f;
  p.saturation_midtones = 0.f;
  p.saturation_highlights = -0.25f;
  dt_gui_presets_add_generic(_("basic colorfulness: vibrant colors"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  p.saturation_global = 0.2f;
  p.saturation_shadows = 0.25f;
  p.saturation_midtones = 0.f;
  p.saturation_highlights = -0.25f;
  dt_gui_presets_add_generic(_("basic colorfulness: standard"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);
}



#ifdef _OPENMP
#pragma omp declare simd aligned(output, output_comp: 16) uniform(shadows_weight, midtones_weight, highlights_weight)
#endif
static inline void opacity_masks(const float x,
                                 const float shadows_weight, const float highlights_weight,
                                 const float midtones_weight, const float mask_grey_fulcrum,
                                 dt_aligned_pixel_t output, dt_aligned_pixel_t output_comp)
{
  const float x_offset = (x - mask_grey_fulcrum);
  const float x_offset_norm = x_offset / mask_grey_fulcrum;
  const float alpha = 1.f / (1.f + expf(x_offset_norm * shadows_weight));    // opacity of shadows
  const float beta = 1.f / (1.f + expf(-x_offset_norm * highlights_weight)); // opacity of highlights
  const float alpha_comp = 1.f - alpha;
  const float beta_comp = 1.f - beta;
  const float gamma = expf(-sqf(x_offset) * midtones_weight / 4.f) * sqf(alpha_comp) * sqf(beta_comp) * 8.f; // opacity of midtones
  const float gamma_comp = 1.f - gamma;

  output[0] = alpha;
  output[1] = gamma;
  output[2] = beta;
  output[3] = 0.f;

  if(output_comp)
  {
    output_comp[0] = alpha_comp;
    output_comp[1] = gamma_comp;
    output_comp[2] = beta_comp;
    output_comp[3] = 0.f;
  }
}


static inline float soft_clip(const float x, const float soft_threshold, const float hard_threshold)
{
  // use an exponential soft clipping above soft_threshold
  // hard threshold must be > soft threshold
  const float norm = hard_threshold - soft_threshold;
  return (x > soft_threshold) ? soft_threshold + (1.f - expf(-(x - soft_threshold) / norm)) * norm : x;
}


static inline float lookup_gamut(const float *const gamut_lut, const float x)
{
  // WARNING : x should be between [-pi ; pi ], which is the default output of atan2 anyway

  // convert in LUT coordinate
  const float x_test = (LUT_ELEM - 1) * (x + M_PI_F) / (2.f * M_PI_F);

  // find the 2 closest integer coordinates (next/previous)
  float x_prev = floorf(x_test);
  float x_next = ceilf(x_test);

  // get the 2 closest LUT elements at integer coordinates
  // cycle on the hue ring if out of bounds
  int xi = (int)x_prev;
  if(xi < 0) xi = LUT_ELEM - 1;
  else if(xi > LUT_ELEM - 1) xi = 0;

  int xii = (int)x_next;
  if(xii < 0) xii = LUT_ELEM - 1;
  else if(xii > LUT_ELEM - 1) xii = 0;

  // fetch the corresponding y values
  const float y_prev = gamut_lut[xi];
  const float y_next = gamut_lut[xii];

  // assume that we are exactly on an integer LUT element
  float out = y_prev;

  if(x_next != x_prev)
    // we are between 2 LUT elements : do linear interpolation
    // actually, we only add the slope term on the previous one
    out += (x_test - x_prev) * (y_next - y_prev) / (x_next - x_prev);

  return out;
}


void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_colorbalancergb_data_t *d = (dt_iop_colorbalancergb_data_t *)piece->data;
  dt_iop_colorbalancergb_gui_data_t *g = (dt_iop_colorbalancergb_gui_data_t *)self->gui_data;
  const struct dt_iop_order_iccprofile_info_t *const work_profile
      = dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  if(work_profile == NULL) return; // no point

  self->cache_next_important = TRUE; // The cpu code is pretty heavy stuff so an importance hint

  // work profile can't be fetched in commit_params since it is not yet initialised
  // work_profile->matrix_in === RGB_to_XYZ
  // work_profile->matrix_out === XYZ_to_RGB

  // Premultiply the input matrices

  /* What we do here is equivalent to :

    // go to CIE 1931 XYZ 2° D50
    dot_product(RGB, RGB_to_XYZ, XYZ_D50); // matrice product

    // chroma adapt D50 to D65
    XYZ_D50_to_65(XYZ_D50, XYZ_D65);       // matrice product

    // go to CIE 2006 LMS
    XYZ_to_LMS(XYZ_D65, LMS);              // matrice product

  * so we pre-multiply the 3 conversion matrices and operate only one matrix product
  */
  dt_colormatrix_t input_matrix;
  dt_colormatrix_t output_matrix;

  dt_colormatrix_mul(output_matrix, XYZ_D50_to_D65_CAT16, work_profile->matrix_in); // output_matrix used as temp buffer
  dt_colormatrix_mul(input_matrix, XYZ_D65_to_LMS_2006_D65, output_matrix);

  // Premultiply the output matrix

  /* What we do here is equivalent to :
    XYZ_D65_to_50(XYZ_D65, XYZ_D50);           // matrix product
    dot_product(XYZ_D50, XYZ_to_RGB, pix_out); // matrix product
  */

  dt_colormatrix_mul(output_matrix, work_profile->matrix_out, XYZ_D65_to_D50_CAT16);

  const float *const restrict in = __builtin_assume_aligned(((const float *const restrict)ivoid), 64);
  float *const restrict out = __builtin_assume_aligned(((float *const restrict)ovoid), 64);
  const float *const restrict gamut_LUT = __builtin_assume_aligned(((const float *const restrict)d->gamut_LUT), 64);

  const float *const restrict global = __builtin_assume_aligned((const float *const restrict)d->global, 16);
  const float *const restrict highlights = __builtin_assume_aligned((const float *const restrict)d->highlights, 16);
  const float *const restrict shadows = __builtin_assume_aligned((const float *const restrict)d->shadows, 16);
  const float *const restrict midtones = __builtin_assume_aligned((const float *const restrict)d->midtones, 16);

  const float *const restrict chroma = __builtin_assume_aligned((const float *const restrict)d->chroma, 16);
  const float *const restrict saturation = __builtin_assume_aligned((const float *const restrict)d->saturation, 16);
  const float *const restrict brilliance = __builtin_assume_aligned((const float *const restrict)d->brilliance, 16);

  const gint mask_display
      = ((piece->pipe->type & DT_DEV_PIXELPIPE_FULL) && self->dev->gui_attached
         && g && g->mask_display);

  // pixel size of the checker background
  const size_t checker_1 = (mask_display) ? DT_PIXEL_APPLY_DPI(d->checker_size) : 0;
  const size_t checker_2 = 2 * checker_1;

  const float L_white = Y_to_dt_UCS_L_star(d->white_fulcrum);

  const float DT_ALIGNED_ARRAY hue_rotation_matrix[2][2] = {
    { cosf(d->hue_angle), -sinf(d->hue_angle) },
    { sinf(d->hue_angle),  cosf(d->hue_angle) },
  };

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, out, roi_in, roi_out, d, g, mask_display, input_matrix, output_matrix, gamut_LUT, \
    global, highlights, shadows, midtones, chroma, saturation, brilliance, checker_1, checker_2, L_white, \
    hue_rotation_matrix) \
    schedule(static) collapse(2)
#endif
  for(size_t i = 0; i < roi_out->height; i++)
  for(size_t j = 0; j < roi_out->width; j++)
  {
    const size_t k = ((i * roi_out->width) + j) * 4;
    const float *const restrict pix_in = __builtin_assume_aligned(in + k, 16);
    float *const restrict pix_out = __builtin_assume_aligned(out + k, 16);

    dt_aligned_pixel_t XYZ_D65 = { 0.f };
    dt_aligned_pixel_t LMS = { 0.f };
    dt_aligned_pixel_t RGB = { 0.f };
    dt_aligned_pixel_t Yrg = { 0.f };
    dt_aligned_pixel_t Ych = { 0.f };

    // clip pipeline RGB
    for_four_channels(c, aligned(pix_in:16)) RGB[c] = fmaxf(pix_in[c], 0.0f);

    // go to CIE 2006 LMS D65
    dot_product(RGB, input_matrix, LMS);

    /* The previous line is equivalent to :
      // go to CIE 1931 XYZ 2° D50
      dot_product(RGB, RGB_to_XYZ, XYZ_D50); // matrice product

      // chroma adapt D50 to D65
      XYZ_D50_to_65(XYZ_D50, XYZ_D65); // matrice product

      // go to CIE 2006 LMS
      XYZ_to_LMS(XYZ_D65, LMS); // matrice product
    */

    // go to Filmlight Yrg
    LMS_to_Yrg(LMS, Yrg);

    // go to Ych
    Yrg_to_Ych(Yrg, Ych);

    // Sanitize input : no negative luminance
    Ych[0] = fmaxf(Ych[0], 0.f);

    // Opacities for luma masks
    dt_aligned_pixel_t opacities;
    dt_aligned_pixel_t opacities_comp;
    opacity_masks(powf(Ych[0], 0.4101205819200422f), // center middle grey in 50 %
                  d->shadows_weight, d->highlights_weight, d->midtones_weight, d->mask_grey_fulcrum, opacities, opacities_comp);

    // Hue shift - do it now because we need the gamut limit at output hue right after
    // The hue rotation is implemented as a matrix multiplication.
    const float cos_h = Ych[2];
    const float sin_h = Ych[3];
    Ych[2] = hue_rotation_matrix[0][0] * cos_h + hue_rotation_matrix[0][1] * sin_h;
    Ych[3] = hue_rotation_matrix[1][0] * cos_h + hue_rotation_matrix[1][1] * sin_h;

    // Linear chroma : distance to achromatic at constant luminance in scene-referred
    const float chroma_boost = d->chroma_global + scalar_product(opacities, chroma);
    const float vibrance = d->vibrance * (1.0f - powf(Ych[1], fabsf(d->vibrance)));
    const float chroma_factor = fmaxf(1.f + chroma_boost + vibrance, 0.f);
    Ych[1] *= chroma_factor;

    // clip chroma at constant hue and Y if needed
    gamut_check_Yrg(Ych);

    // go to Yrg for real
    Ych_to_Yrg(Ych, Yrg);

    // Go to LMS
    Yrg_to_LMS(Yrg, LMS);

    // Go to Filmlight RGB
    LMS_to_gradingRGB(LMS, RGB);

    // Color balance
    for_four_channels(c, aligned(RGB, opacities, opacities_comp, global, shadows, midtones, highlights:16))
    {
      // global : offset
      RGB[c] += global[c];

      //  highlights, shadows : 2 slopes with masking
      RGB[c] *= opacities_comp[2] * (opacities_comp[0] + opacities[0] * shadows[c]) + opacities[2] * highlights[c];
      // factorization of : (RGB[c] * (1.f - alpha) + RGB[c] * d->shadows[c] * alpha) * (1.f - beta)  + RGB[c] * d->highlights[c] * beta;

      // midtones : power with sign preservation
      const float sign = (RGB[c] < 0.f) ? -1.f : 1.f;
      RGB[c] = sign * powf(fabsf(RGB[c]) / d->white_fulcrum, midtones[c]) * d->white_fulcrum;
    }

    // for the non-linear ops we need to go in Yrg again because RGB doesn't preserve color
    gradingRGB_to_LMS(RGB, LMS);
    LMS_to_Yrg(LMS, Yrg);

    // Y midtones power (gamma)
    Yrg[0] = powf(fmaxf(Yrg[0] / d->white_fulcrum, 0.f), d->midtones_Y) * d->white_fulcrum;

    // Y fulcrumed contrast
    Yrg[0] = d->grey_fulcrum * powf(Yrg[0] / d->grey_fulcrum, d->contrast);

    Yrg_to_LMS(Yrg, LMS);
    LMS_to_XYZ(LMS, XYZ_D65);

    // Perceptual color adjustments
    if(d->saturation_formula == DT_COLORBALANCE_SATURATION_JZAZBZ)
    {
      dt_aligned_pixel_t Jab = { 0.f };
      dt_XYZ_2_JzAzBz(XYZ_D65, Jab);

      // Convert to JCh
      float JC[2] = { Jab[0], dt_fast_hypotf(Jab[1], Jab[2]) };   // brightness/chroma vector
      const float h = atan2f(Jab[2], Jab[1]);  // hue : (a, b) angle

      // Project JC onto S, the saturation eigenvector, with orthogonal vector O.
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

      // brilliance & Saturation : mix of chroma and luminance
      const float boosts[2] = { 1.f + d->brilliance_global + scalar_product(opacities, brilliance),     // move in S direction
                                d->saturation_global + scalar_product(opacities, saturation) }; // move in O direction

      SO[0] = JC[0] * M_rot_dir[0][0] + JC[1] * M_rot_dir[0][1];
      SO[1] = SO[0] * fminf(fmaxf(T * boosts[1], -T), DT_M_PI_F / 2.f - T);
      SO[0] = fmaxf(SO[0] * boosts[0], 0.f);

      // Project back to JCh, that is rotate back of -T angle
      JC[0] = fmaxf(SO[0] * M_rot_inv[0][0] + SO[1] * M_rot_inv[0][1], 0.f);
      JC[1] = fmaxf(SO[0] * M_rot_inv[1][0] + SO[1] * M_rot_inv[1][1], 0.f);

      // Gamut mapping
      const float out_max_sat_h = lookup_gamut(gamut_LUT, h);
      // if JC[0] == 0.f, the saturation / luminance ratio is infinite - assign the largest practical value we have
      const float sat = (JC[0] > 0.f) ? soft_clip(JC[1] / JC[0], 0.8f * out_max_sat_h, out_max_sat_h)
                                      : out_max_sat_h;
      const float max_C_at_sat = JC[0] * sat;
      // if sat == 0.f, the chroma is zero - assign the original luminance because there's no need to gamut map
      const float max_J_at_sat = (sat > 0.f) ? JC[1] / sat : JC[0];
      JC[0] = (JC[0] + max_J_at_sat) / 2.f;
      JC[1] = (JC[1] + max_C_at_sat) / 2.f;

      // Gamut-clip in Jch at constant hue and lightness,
      // e.g. find the max chroma available at current hue that doesn't
      // yield negative L'M'S' values, which will need to be clipped during conversion
      const float cos_H = cosf(h);
      const float sin_H = sinf(h);

      const float d0 = 1.6295499532821566e-11f;
      const float dd = -0.56f;
      float Iz = JC[0] + d0;
      Iz /= (1.f + dd - dd * Iz);
      Iz = fmaxf(Iz, 0.f);

      const dt_colormatrix_t AI
          = { {  1.0f,  0.1386050432715393f,  0.0580473161561189f, 0.0f },
              {  1.0f, -0.1386050432715393f, -0.0580473161561189f, 0.0f },
              {  1.0f, -0.0960192420263190f, -0.8118918960560390f, 0.0f } };

      // Do a test conversion to L'M'S'
      const dt_aligned_pixel_t IzAzBz = { Iz, JC[1] * cos_H, JC[1] * sin_H, 0.f };
      dot_product(IzAzBz, AI, LMS);

      // Clip chroma
      float max_C = JC[1];
      if(LMS[0] < 0.f)
        max_C = fmin(-Iz / (AI[0][1] * cos_H + AI[0][2] * sin_H), max_C);

      if(LMS[1] < 0.f)
        max_C = fmin(-Iz / (AI[1][1] * cos_H + AI[1][2] * sin_H), max_C);

      if(LMS[2] < 0.f)
        max_C = fmin(-Iz / (AI[2][1] * cos_H + AI[2][2] * sin_H), max_C);

      // Project back to JzAzBz for real
      Jab[0] = JC[0];
      Jab[1] = max_C * cos_H;
      Jab[2] = max_C * sin_H;

      dt_JzAzBz_2_XYZ(Jab, XYZ_D65);
    }
    else
    {
      dt_aligned_pixel_t xyY, JCH, HCB;
      dt_XYZ_to_xyY(XYZ_D65, xyY);
      xyY_to_dt_UCS_JCH(xyY, L_white, JCH);
      dt_UCS_JCH_to_HCB(JCH, HCB);

      const float radius = hypotf(HCB[1], HCB[2]);
      const float sin_T = (radius > 0.f) ? HCB[1] / radius : 0.f;
      const float cos_T = (radius > 0.f) ? HCB[2] / radius : 0.f;
      const float DT_ALIGNED_PIXEL M_rot_inv[2][2] = { { cos_T,  sin_T }, { -sin_T, cos_T } };
      // This would be the full matrice of direct rotation if we didn't need only its last row
      //const float DT_ALIGNED_PIXEL M_rot_dir[2][2] = { { cos_T, -sin_T }, {  sin_T, cos_T } };

      float P = HCB[1];
      float W = sin_T * HCB[1] + cos_T * HCB[2];

      float a = fmaxf(1.f + d->saturation_global + scalar_product(opacities, saturation), 0.f);
      const float b = fmaxf(1.f + d->brilliance_global + scalar_product(opacities, brilliance), 0.f);

      const float max_a = hypotf(P, W) / P;
      a = soft_clip(a, 0.5f * max_a, max_a);

      const float P_prime = (a - 1.f) * P;
      const float W_prime = sqrtf(sqf(P) * (1.f - sqf(a)) + sqf(W)) * b;

      HCB[1] = fmaxf(M_rot_inv[0][0] * P_prime + M_rot_inv[0][1] * W_prime, 0.f);
      HCB[2] = fmaxf(M_rot_inv[1][0] * P_prime + M_rot_inv[1][1] * W_prime, 0.f);

      dt_UCS_HCB_to_JCH(HCB, JCH);

      // Gamut mapping
      const float max_colorfulness = lookup_gamut(gamut_LUT, JCH[2]); // WARNING : this is M²
      const float max_chroma = 15.932993652962535f * powf(JCH[0] * L_white, 0.6523997524738018f) * powf(max_colorfulness, 0.6007557017508491f) / L_white;
      const dt_aligned_pixel_t JCH_gamut_boundary = { JCH[0], max_chroma, JCH[2], 0.f };
      dt_aligned_pixel_t HSB_gamut_boundary;
      dt_UCS_JCH_to_HSB(JCH_gamut_boundary, HSB_gamut_boundary);

      // Clip saturation at constant brightness
      dt_aligned_pixel_t HSB = { HCB[0], (HCB[2] > 0.f) ? HCB[1] / HCB[2] : 0.f, HCB[2], 0.f };
      HSB[1] = soft_clip(HSB[1], 0.8f * HSB_gamut_boundary[1], HSB_gamut_boundary[1]);

      dt_UCS_HSB_to_JCH(HSB, JCH);
      dt_UCS_JCH_to_xyY(JCH, L_white, xyY);
      dt_xyY_to_XYZ(xyY, XYZ_D65);
    }

    // Project back to D50 pipeline RGB
    dot_product(XYZ_D65, output_matrix, pix_out);

    /* The previous line is equivalent to :
      XYZ_D65_to_50(XYZ_D65, XYZ_D50);           // matrix product
      dot_product(XYZ_D50, XYZ_to_RGB, pix_out); // matrix product
    */

    if(mask_display)
    {
      // draw checkerboard
      dt_aligned_pixel_t color;
      if(i % checker_1 < i % checker_2)
      {
        if(j % checker_1 < j % checker_2) for_four_channels(c) color[c] = d->checker_color_2[c];
        else for_four_channels(c) color[c] = d->checker_color_1[c];
      }
      else
      {
        if(j % checker_1 < j % checker_2) for_four_channels(c) color[c] = d->checker_color_1[c];
        else for_four_channels(c) color[c] = d->checker_color_2[c];
      }

      float opacity = opacities[g->mask_type];
      const float opacity_comp = 1.0f - opacity;

      for_four_channels(c, aligned(pix_out, color:16)) pix_out[c] = opacity_comp * color[c] + opacity * fmaxf(pix_out[c], 0.f);
      pix_out[3] = 1.0f; // alpha is opaque, we need to preview it
    }
    else
    {
      for_four_channels(c, aligned(pix_out:16)) pix_out[c] = fmaxf(pix_out[c], 0.f);
      pix_out[3] = pix_in[3]; // alpha copy
    }
  }
}


#if HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorbalancergb_data_t *const d = (dt_iop_colorbalancergb_data_t *)piece->data;
  dt_iop_colorbalancergb_global_data_t *const gd = (dt_iop_colorbalancergb_global_data_t *)self->global_data;
  dt_iop_colorbalancergb_gui_data_t *g = (dt_iop_colorbalancergb_gui_data_t *)self->gui_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  if(piece->colors != 4)
  {
    dt_control_log(_("colorbalance works only on RGB input"));
    return err;
  }

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };

  // Get working color profile
  const struct dt_iop_order_iccprofile_info_t *const work_profile
      = dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  if(work_profile == NULL) return err; // no point

  cl_mem dev_profile_info = NULL;
  cl_mem dev_profile_lut = NULL;
  dt_colorspaces_iccprofile_info_cl_t *profile_info_cl;
  cl_float *profile_lut_cl = NULL;

  cl_mem input_matrix_cl = NULL;
  cl_mem output_matrix_cl = NULL;
  cl_mem gamut_LUT = NULL;
  cl_mem hue_rotation_matrix_cl = NULL;

  err = dt_ioppr_build_iccprofile_params_cl(work_profile, devid, &profile_info_cl, &profile_lut_cl,
                                            &dev_profile_info, &dev_profile_lut);
  if(err != CL_SUCCESS) goto error;

  // repack the matrices as flat AVX2-compliant matrice
  // work profile can't be fetched in commit_params since it is not yet initialised
  // work_profile->matrix_in === RGB_to_XYZ
  // work_profile->matrix_out === XYZ_to_RGB

  // Premultiply the input matrices

  /* What we do here is equivalent to :

    // go to CIE 1931 XYZ 2° D50
    dot_product(RGB, RGB_to_XYZ, XYZ_D50); // matrice product

    // chroma adapt D50 to D65
    XYZ_D50_to_65(XYZ_D50, XYZ_D65);       // matrice product

    // go to CIE 2006 LMS
    XYZ_to_LMS(XYZ_D65, LMS);              // matrice product

  * so we pre-multiply the 3 conversion matrices and operate only one matrix product
  */
  dt_colormatrix_t input_matrix;
  dt_colormatrix_t output_matrix;

  dt_colormatrix_mul(output_matrix, XYZ_D50_to_D65_CAT16, work_profile->matrix_in); // output_matrix used as temp buffer
  dt_colormatrix_mul(input_matrix, XYZ_D65_to_LMS_2006_D65, output_matrix);

  // Premultiply the output matrix

  /* What we do here is equivalent to :
    XYZ_D65_to_50(XYZ_D65, XYZ_D50);           // matrix product
    dot_product(XYZ_D50, XYZ_to_RGB, pix_out); // matrix product
  */

  dt_colormatrix_mul(output_matrix, work_profile->matrix_out, XYZ_D65_to_D50_CAT16);

  input_matrix_cl = dt_opencl_copy_host_to_device_constant(devid, 12 * sizeof(float), input_matrix);
  output_matrix_cl = dt_opencl_copy_host_to_device_constant(devid, 12 * sizeof(float), output_matrix);

  // Send gamut LUT to GPU
  gamut_LUT = dt_opencl_copy_host_to_device(devid, d->gamut_LUT, LUT_ELEM, 1, sizeof(float));

  // Size of the checker
  const gint mask_display
      = ((piece->pipe->type & DT_DEV_PIXELPIPE_FULL) && self->dev->gui_attached
         && g && g->mask_display);
  const int checker_1 = (mask_display) ? DT_PIXEL_APPLY_DPI(d->checker_size) : 0;
  const int checker_2 = 2 * checker_1;
  const int mask_type = (mask_display) ? g->mask_type : 0;

  const float L_white = Y_to_dt_UCS_L_star(d->white_fulcrum);

  float hue_rotation_matrix[4]
    = { cosf(d->hue_angle), -sinf(d->hue_angle), sinf(d->hue_angle), cosf(d->hue_angle) };
  hue_rotation_matrix_cl = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), hue_rotation_matrix);

  dt_opencl_set_kernel_args(devid, gd->kernel_colorbalance_rgb, 0, CLARG(dev_in), CLARG(dev_out),
    CLARG(width), CLARG(height), CLARG(dev_profile_info), CLARG(input_matrix_cl), CLARG(output_matrix_cl),
    CLARG(gamut_LUT), CLARG(d->shadows_weight), CLARG(d->highlights_weight), CLARG(d->midtones_weight),
    CLARG(d->mask_grey_fulcrum), CLARG(d->hue_angle), CLARG(d->chroma_global), CLARG(d->chroma), CLARG(d->vibrance),
    CLARG(d->global), CLARG(d->shadows), CLARG(d->highlights), CLARG(d->midtones), CLARG(d->white_fulcrum),
    CLARG(d->midtones_Y), CLARG(d->grey_fulcrum), CLARG(d->contrast), CLARG(d->brilliance_global),
    CLARG(d->brilliance), CLARG(d->saturation_global), CLARG(d->saturation), CLARG(mask_display), CLARG(mask_type),
    CLARG(checker_1), CLARG(checker_2), CLARG(d->checker_color_1), CLARG(d->checker_color_2), CLARG(L_white),
    CLARG(d->saturation_formula), CLARG(hue_rotation_matrix_cl));

  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_colorbalance_rgb, sizes);
  if(err != CL_SUCCESS) goto error;

  // cleanup and exit on success
  dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl, &dev_profile_info, &dev_profile_lut);
  dt_opencl_release_mem_object(input_matrix_cl);
  dt_opencl_release_mem_object(output_matrix_cl);
  dt_opencl_release_mem_object(gamut_LUT);
  dt_opencl_release_mem_object(hue_rotation_matrix_cl);
  return TRUE;

error:
  dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl, &dev_profile_info, &dev_profile_lut);
  if(input_matrix_cl) dt_opencl_release_mem_object(input_matrix_cl);
  if(output_matrix_cl) dt_opencl_release_mem_object(output_matrix_cl);
  if(gamut_LUT) dt_opencl_release_mem_object(gamut_LUT);
  if(hue_rotation_matrix_cl) dt_opencl_release_mem_object(hue_rotation_matrix_cl);
  dt_print(DT_DEBUG_OPENCL, "[opencl_colorbalancergb] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 8; // extended.cl in programs.conf
  dt_iop_colorbalancergb_global_data_t *gd
      = (dt_iop_colorbalancergb_global_data_t *)malloc(sizeof(dt_iop_colorbalancergb_global_data_t));

  module->data = gd;
  gd->kernel_colorbalance_rgb = dt_opencl_create_kernel(program, "colorbalancergb");
}


void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_colorbalancergb_global_data_t *gd = (dt_iop_colorbalancergb_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_colorbalance_rgb);
  free(module->data);
  module->data = NULL;
}
#endif


static inline float Delta_H(const float h_1, const float h_2)
{
  // Compute the difference between 2 angles
  // and force the result in [-pi; pi] radians
  float diff = h_1 - h_2;
  diff += (diff < -M_PI_F) ? 2.f * M_PI_F : 0.f;
  diff -= (diff > M_PI_F) ? 2.f * M_PI_F : 0.f;
  return diff;
}


void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorbalancergb_data_t *d = (dt_iop_colorbalancergb_data_t *)(piece->data);
  dt_iop_colorbalancergb_params_t *p = (dt_iop_colorbalancergb_params_t *)p1;

  d->checker_color_1[0] = CLAMP(dt_conf_get_float("plugins/darkroom/colorbalancergb/checker1/red"), 0.f, 1.f);
  d->checker_color_1[1] = CLAMP(dt_conf_get_float("plugins/darkroom/colorbalancergb/checker1/green"), 0.f, 1.f);
  d->checker_color_1[2] = CLAMP(dt_conf_get_float("plugins/darkroom/colorbalancergb/checker1/blue"), 0.f, 1.f);
  d->checker_color_1[3] = 1.f;

  d->checker_color_2[0] = CLAMP(dt_conf_get_float("plugins/darkroom/colorbalancergb/checker2/red"), 0.f, 1.f);
  d->checker_color_2[1] = CLAMP(dt_conf_get_float("plugins/darkroom/colorbalancergb/checker2/green"), 0.f, 1.f);
  d->checker_color_2[2] = CLAMP(dt_conf_get_float("plugins/darkroom/colorbalancergb/checker2/blue"), 0.f, 1.f);
  d->checker_color_2[3] = 1.f;

  d->checker_size = MAX(dt_conf_get_int("plugins/darkroom/colorbalancergb/checker/size"), 2);

  d->vibrance = p->vibrance;
  d->contrast = 1.0f + p->contrast; // that limits the user param range to [-1, 1], but it seems enough
  d->grey_fulcrum = p->grey_fulcrum;

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

  d->brilliance_global = p->brilliance_global;
  d->brilliance[0] = p->brilliance_shadows;
  d->brilliance[1] = p->brilliance_midtones;
  d->brilliance[2] = p->brilliance_highlights;
  d->brilliance[3] = 0.f;

  d->hue_angle = M_PI * p->hue_angle / 180.f;

  // measure the grading RGB of a pure white
  const dt_aligned_pixel_t Ych_norm = { 1.f, 0.f, 1.f, 0.f };
  dt_aligned_pixel_t RGB_norm = { 0.f };
  Ych_to_gradingRGB(Ych_norm, RGB_norm);
  dt_aligned_pixel_t Ych;

  // global
  {
    make_Ych(1.f, p->global_C, DEG_TO_RAD(p->global_H), Ych);
    Ych_to_gradingRGB(Ych, d->global);
    for(size_t c = 0; c < 4; c++) d->global[c] = (d->global[c] - RGB_norm[c]) + RGB_norm[c] * p->global_Y;
  }

  // shadows
  {
    make_Ych(1.f, p->shadows_C, DEG_TO_RAD(p->shadows_H), Ych);
    Ych_to_gradingRGB(Ych, d->shadows);
    for(size_t c = 0; c < 4; c++) d->shadows[c] = 1.f + (d->shadows[c] - RGB_norm[c]) + p->shadows_Y;
    d->shadows_weight = 2.f + p->shadows_weight * 2.f;
  }

  // highlights
  {
    make_Ych(1.f, p->highlights_C, DEG_TO_RAD(p->highlights_H), Ych);
    Ych_to_gradingRGB(Ych, d->highlights);
    for(size_t c = 0; c < 4; c++) d->highlights[c] = 1.f + (d->highlights[c] - RGB_norm[c]) + p->highlights_Y;
    d->highlights_weight = 2.f + p->highlights_weight * 2.f;
  }

  // midtones
  {
    make_Ych(1.f, p->midtones_C, DEG_TO_RAD(p->midtones_H), Ych);
    Ych_to_gradingRGB(Ych, d->midtones);
    for(size_t c = 0; c < 4; c++) d->midtones[c] = 1.f / (1.f + (d->midtones[c] - RGB_norm[c]));
    d->midtones_Y = 1.f / (1.f + p->midtones_Y);
    d->white_fulcrum = exp2f(p->white_fulcrum);
    d->midtones_weight = sqf(d->shadows_weight) * sqf(d->highlights_weight) /
      (sqf(d->shadows_weight) + sqf(d->highlights_weight));
    d->mask_grey_fulcrum = powf(p->mask_grey_fulcrum, 0.4101205819200422f);
  }

  if(p->saturation_formula != d->saturation_formula) d->lut_inited = FALSE;
  d->saturation_formula = p->saturation_formula;

  // Check if the RGB working profile has changed in pipe
  // WARNING: this function is not triggered upon working profile change,
  // so the gamut boundaries are wrong until we change some param in this module
  struct dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  if(work_profile == NULL) return;
  if(work_profile != d->work_profile)
  {
    d->lut_inited = FALSE;
    d->work_profile = work_profile;
  }

  // find the maximum chroma allowed by the current working gamut in conjunction to hue
  // this will be used to prevent users to mess up their images by pushing chroma out of gamut
  if(!d->lut_inited)
  {
    float *const restrict LUT_saturation = dt_alloc_align_float(LUT_ELEM);

    // init the LUT between -pi and pi by increments of 1°
    for(size_t k = 0; k < LUT_ELEM; k++) LUT_saturation[k] = 0.f;

    // Premultiply both matrices to go from D50 pipeline RGB to D65 XYZ in a single matrix dot product
    // instead of D50 pipeline to D50 XYZ (work_profile->matrix_in) and then D50 XYZ to D65 XYZ
    dt_colormatrix_t input_matrix;
    dt_colormatrix_mul(input_matrix, XYZ_D50_to_D65_CAT16, work_profile->matrix_in);

    // make RGB values vary between [0; 1] in working space, convert to Ych and get the max(c(h)))
    if(p->saturation_formula == DT_COLORBALANCE_SATURATION_JZAZBZ)
    {
      #ifdef _OPENMP
      #pragma omp parallel for default(none) \
            dt_omp_firstprivate(input_matrix, p) schedule(static) dt_omp_sharedconst(LUT_saturation) \
            collapse(3)
      #endif
      for(size_t r = 0; r < STEPS; r++)
        for(size_t g = 0; g < STEPS; g++)
          for(size_t b = 0; b < STEPS; b++)
          {
            const dt_aligned_pixel_t rgb = { (float)r / (float)(STEPS - 1), (float)g / (float)(STEPS - 1),
                                            (float)b / (float)(STEPS - 1), 0.f };
            dt_aligned_pixel_t XYZ = { 0.f };
            float saturation = 0.f;
            float hue = 0.f;

            dot_product(rgb, input_matrix, XYZ); // Go from D50 pipeline RGB to D65 XYZ in one step

            dt_aligned_pixel_t Jab, Jch;
            dt_XYZ_2_JzAzBz(XYZ, Jab);           // this one expects D65 XYZ

            Jch[0] = Jab[0];
            Jch[1] = dt_fast_hypotf(Jab[2], Jab[1]);
            Jch[2] = atan2f(Jab[2], Jab[1]);

            saturation = (Jch[0] > 0.f) ? Jch[1] / Jch[0] : 0.f;
            hue = Jch[2];

            const size_t index = roundf((LUT_ELEM - 1) * (hue + M_PI_F) / (2.f * M_PI_F));
            LUT_saturation[index] = fmaxf(saturation, LUT_saturation[index]);
          }

      // anti-aliasing on the LUT (simple 5-taps 1D box average)
      for(size_t k = 2; k < LUT_ELEM - 2; k++)
        d->gamut_LUT[k] = (LUT_saturation[k - 2] + LUT_saturation[k - 1] + LUT_saturation[k] + LUT_saturation[k + 1] + LUT_saturation[k + 2]) / 5.f;

      // handle bounds
      d->gamut_LUT[0] = (LUT_saturation[LUT_ELEM - 2] + LUT_saturation[LUT_ELEM - 1] + LUT_saturation[0] + LUT_saturation[1] + LUT_saturation[2]) / 5.f;
      d->gamut_LUT[1] = (LUT_saturation[LUT_ELEM - 1] + LUT_saturation[0] + LUT_saturation[1] + LUT_saturation[2] + LUT_saturation[3]) / 5.f;
      d->gamut_LUT[LUT_ELEM - 1] = (LUT_saturation[LUT_ELEM - 3] + LUT_saturation[LUT_ELEM - 2] + LUT_saturation[LUT_ELEM - 1] + LUT_saturation[0] + LUT_saturation[1]) / 5.f;
      d->gamut_LUT[LUT_ELEM - 2] = (LUT_saturation[LUT_ELEM - 4] + LUT_saturation[LUT_ELEM - 3] + LUT_saturation[LUT_ELEM - 2] + LUT_saturation[LUT_ELEM - 1] + LUT_saturation[0]) / 5.f;
    }
    else if(p->saturation_formula == DT_COLORBALANCE_SATURATION_DTUCS)
    {
      dt_aligned_pixel_t D65_xyY = { 0.31269999999999992f,  0.32899999999999996f ,  1.f, 0.f };

      // Compute the RGB space primaries in xyY
      dt_aligned_pixel_t RGB_red   = { 1.f, 0.f, 0.f, 0.f };
      dt_aligned_pixel_t RGB_green = { 0.f, 1.f, 0.f, 0.f };
      dt_aligned_pixel_t RGB_blue =  { 0.f, 0.f, 1.f, 0.f };

      dt_aligned_pixel_t XYZ_red, XYZ_green, XYZ_blue;
      dot_product(RGB_red, input_matrix, XYZ_red);
      dot_product(RGB_green, input_matrix, XYZ_green);
      dot_product(RGB_blue, input_matrix, XYZ_blue);

      dt_aligned_pixel_t xyY_red, xyY_green, xyY_blue;
      dt_XYZ_to_xyY(XYZ_red, xyY_red);
      dt_XYZ_to_xyY(XYZ_green, xyY_green);
      dt_XYZ_to_xyY(XYZ_blue, xyY_blue);

      // Get the "hue" angles of the primaries in xy compared to D65
      const float h_red   = atan2f(xyY_red[1] - D65_xyY[1], xyY_red[0] - D65_xyY[0]);
      const float h_green = atan2f(xyY_green[1] - D65_xyY[1], xyY_green[0] - D65_xyY[0]);
      const float h_blue  = atan2f(xyY_blue[1] - D65_xyY[1], xyY_blue[0] - D65_xyY[0]);

       float *const restrict dt_UCS_LUT = d->gamut_LUT;

      // March the gamut boundary in CIE xyY 1931 by angular steps of 0.02°
      #ifdef _OPENMP
        #pragma omp parallel for default(none) \
              dt_omp_firstprivate(input_matrix, xyY_red, xyY_green, xyY_blue, h_red, h_green, h_blue, D65_xyY) \
              schedule(static) dt_omp_sharedconst(dt_UCS_LUT)
      #endif
      for(int i = 0; i < 50 * 360; i++)
      {
        const float angle = -M_PI_F + ((float)i) / (50.f * 360.f) * 2.f * M_PI_F;
        const float tan_angle = tanf(angle);

        const float t_1 = Delta_H(angle, h_blue)  / Delta_H(h_red, h_blue);
        const float t_2 = Delta_H(angle, h_red)   / Delta_H(h_green, h_red);
        const float t_3 = Delta_H(angle, h_green) / Delta_H(h_blue, h_green);

        float x_t = 0;
        float y_t = 0;

        if(t_1 == CLAMP(t_1, 0, 1))
        {
          const float t = (D65_xyY[1] - xyY_blue[1] + tan_angle * (xyY_blue[0] - D65_xyY[0]))
                    / (xyY_red[1] - xyY_blue[1] + tan_angle * (xyY_blue[0] - xyY_red[0]));
          x_t = xyY_blue[0] + t * (xyY_red[0] - xyY_blue[0]);
          y_t = xyY_blue[1] + t * (xyY_red[1] - xyY_blue[1]);
        }
        else if(t_2 == CLAMP(t_2, 0, 1))
        {
          const float t = (D65_xyY[1] - xyY_red[1] + tan_angle * (xyY_red[0] - D65_xyY[0]))
                    / (xyY_green[1] - xyY_red[1] + tan_angle * (xyY_red[0] - xyY_green[0]));
          x_t = xyY_red[0] + t * (xyY_green[0] - xyY_red[0]);
          y_t = xyY_red[1] + t * (xyY_green[1] - xyY_red[1]);
        }
        else if(t_3 == CLAMP(t_3, 0, 1))
        {
          const float t = (D65_xyY[1] - xyY_green[1] + tan_angle * (xyY_green[0] - D65_xyY[0]))
                        / (xyY_blue[1] - xyY_green[1] + tan_angle * (xyY_green[0] - xyY_blue[0]));
          x_t = xyY_green[0] + t * (xyY_blue[0] - xyY_green[0]);
          y_t = xyY_green[1] + t * (xyY_blue[1] - xyY_green[1]);
        }

        // Convert to darktable UCS
        dt_aligned_pixel_t xyY = { x_t, y_t, 1.f, 0.f };
        float UV_star_prime[2];
        xyY_to_dt_UCS_UV(xyY, UV_star_prime);

        // Get the hue angle in darktable UCS
        const float H = atan2f(UV_star_prime[1], UV_star_prime[0]) * 180.f / M_PI_F;
        const float H_round = roundf(H);
        if(fabsf(H - H_round) < 0.02f)
        {
          int index = (int)(H_round + 180);
          index += (index < 0) ? 360 : 0;
          index -= (index > 359) ? 360 : 0;
          // Warning: we store M², the square of the colorfulness
          dt_UCS_LUT[index] = UV_star_prime[0] * UV_star_prime[0] + UV_star_prime[1] * UV_star_prime[1];
        }
      }
    }

    dt_free_align(LUT_saturation);
    d->lut_inited = TRUE;
  }
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_colorbalancergb_data_t));
  dt_iop_colorbalancergb_data_t *d = (dt_iop_colorbalancergb_data_t *)(piece->data);
  d->gamut_LUT = dt_alloc_align_float(LUT_ELEM);
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

void pipe_RGB_to_Ych(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const dt_aligned_pixel_t RGB,
                     dt_aligned_pixel_t Ych)
{
  const struct dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  if(work_profile == NULL) return; // no point

  dt_aligned_pixel_t XYZ_D50 = { 0.f };
  dt_aligned_pixel_t XYZ_D65 = { 0.f };

  dt_ioppr_rgb_matrix_to_xyz(RGB, XYZ_D50, work_profile->matrix_in_transposed, work_profile->lut_in,
                             work_profile->unbounded_coeffs_in, work_profile->lutsize,
                             work_profile->nonlinearlut);
  XYZ_D50_to_D65(XYZ_D50, XYZ_D65);
  XYZ_to_Ych(XYZ_D65, Ych);
}


void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorbalancergb_gui_data_t *g = (dt_iop_colorbalancergb_gui_data_t *)self->gui_data;
  dt_iop_colorbalancergb_params_t *p = (dt_iop_colorbalancergb_params_t *)self->params;

  dt_aligned_pixel_t Ych = { 0.f };
  dt_aligned_pixel_t max_Ych = { 0.f };
  pipe_RGB_to_Ych(self, piece, (const float *)self->picked_color, Ych);
  pipe_RGB_to_Ych(self, piece, (const float *)self->picked_color_max, max_Ych);
  const float picked_hue = get_hue_angle_from_Ych(Ych);
  const float hue = RAD_TO_DEG(picked_hue) + 180.f;   // take the opponent color

  ++darktable.gui->reset;
  if(picker == g->global_H)
  {
    p->global_H = hue;
    p->global_C = Ych[1] * Ych[0];
    dt_bauhaus_slider_set(g->global_H, p->global_H);
    dt_bauhaus_slider_set(g->global_C, p->global_C);
  }
  else if(picker == g->shadows_H)
  {
    p->shadows_H = hue;
    p->shadows_C = Ych[1] * Ych[0];
    dt_bauhaus_slider_set(g->shadows_H, p->shadows_H);
    dt_bauhaus_slider_set(g->shadows_C, p->shadows_C);
  }
  else if(picker == g->midtones_H)
  {
    p->midtones_H = hue;
    p->midtones_C = Ych[1] * Ych[0];
    dt_bauhaus_slider_set(g->midtones_H, p->midtones_H);
    dt_bauhaus_slider_set(g->midtones_C, p->midtones_C);
  }
  else if(picker == g->highlights_H)
  {
    p->highlights_H = hue;
    p->highlights_C = Ych[1] * Ych[0];
    dt_bauhaus_slider_set(g->highlights_H, p->highlights_H);
    dt_bauhaus_slider_set(g->highlights_C, p->highlights_C);
  }
  else if(picker == g->white_fulcrum)
  {
    p->white_fulcrum = log2f(max_Ych[0]);
    dt_bauhaus_slider_set(g->white_fulcrum, p->white_fulcrum);
  }
  else if(picker == g->grey_fulcrum)
  {
    p->grey_fulcrum = Ych[0];
    dt_bauhaus_slider_set(g->grey_fulcrum, p->grey_fulcrum);
  }
  else
    fprintf(stderr, "[colorbalancergb] unknown color picker\n");
  --darktable.gui->reset;

  gui_changed(self, picker, NULL);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void _YchToRGB(dt_aligned_pixel_t *RGB_out, float chroma, float hue,
                      const dt_iop_order_iccprofile_info_t *output_profile,
                      const dt_colormatrix_t output_matrix_LMS_to_RGB)
{
  dt_aligned_pixel_t RGB_linear = { 0.f };
  dt_aligned_pixel_t Ych;
  make_Ych(0.75f, chroma, hue, Ych);
  dt_aligned_pixel_t XYZ_D65 = { 0.f };
  dt_aligned_pixel_t XYZ_D50 = { 0.f };
  Ych_to_XYZ(Ych, XYZ_D65);
  XYZ_D65_to_D50(XYZ_D65, XYZ_D50);
  dt_apply_transposed_color_matrix(XYZ_D50, output_profile->matrix_out_transposed, RGB_linear);
  // normalize to the brightest value available at this hue and chroma
  const float max_RGB = fmaxf(fmaxf(RGB_linear[0], RGB_linear[1]), RGB_linear[2]);
  for_each_channel(c) RGB_linear[c] = MAX(RGB_linear[c] / max_RGB, 0.f);
  // Apply nonlinear LUT if necessary
  if(output_profile->nonlinearlut)
    dt_ioppr_apply_trc(RGB_linear, *RGB_out, output_profile->lut_out, output_profile->unbounded_coeffs_out,
                       output_profile->lutsize);
  else
    memcpy(*RGB_out, RGB_linear, sizeof(RGB_linear));
}

static void paint_chroma_slider(const dt_iop_order_iccprofile_info_t *output_profile,
                                const dt_colormatrix_t output_matrix_LMS_to_RGB,
                                GtkWidget *w, const float hue)
{
  const float x_min = 0;
  const float x_max = 1;
  const float x_range = x_max - x_min;

  const float h = DEG_TO_RAD(hue);
  const float cos_h = cosf(h);
  const float sin_h = sinf(h);
  // Find max available chroma at this hue without negative RGB
  const float max_chroma = Ych_max_chroma_without_negatives(output_matrix_LMS_to_RGB, cos_h, sin_h);

  // Varies x in range around current y param
  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    const float x = MIN(x_min + stop * x_range, max_chroma);

    dt_aligned_pixel_t RGB;
    _YchToRGB(&RGB, x, h, output_profile, output_matrix_LMS_to_RGB);
    dt_bauhaus_slider_set_stop(w, stop, RGB[0], RGB[1], RGB[2]);
  }

  gtk_widget_queue_draw(w);
}


static void paint_hue_sliders(const dt_iop_order_iccprofile_info_t *output_profile,
                              const dt_colormatrix_t output_matrix_LMS_to_RGB,
                              const dt_iop_colorbalancergb_gui_data_t *const g)
{
  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    const float h = DEG_TO_RAD(stop * (360.f));
    const float max_chroma = Ych_max_chroma_without_negatives(output_matrix_LMS_to_RGB, cosf(h), sinf(h));
    dt_aligned_pixel_t RGB;
    _YchToRGB(&RGB, MIN(0.2f, max_chroma), h, output_profile, output_matrix_LMS_to_RGB);
    dt_bauhaus_slider_set_stop(g->global_H, stop, RGB[0], RGB[1], RGB[2]);
    dt_bauhaus_slider_set_stop(g->shadows_H, stop, RGB[0], RGB[1], RGB[2]);
    dt_bauhaus_slider_set_stop(g->highlights_H, stop, RGB[0], RGB[1], RGB[2]);
    dt_bauhaus_slider_set_stop(g->midtones_H, stop, RGB[0], RGB[1], RGB[2]);
  }
}


static void mask_callback(GtkWidget *togglebutton, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_request_focus(self);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), TRUE);

  dt_iop_colorbalancergb_gui_data_t *g = (dt_iop_colorbalancergb_gui_data_t *)self->gui_data;

  // if blend module is displaying mask do not display it here
  if(self->request_mask_display)
  {
    dt_control_log(_("cannot display masks when the blending mask is displayed"));
    g->mask_display = 0;
  }
  else
  {
    g->mask_display = dt_bauhaus_widget_get_quad_active(GTK_WIDGET(togglebutton));
  }

  if(g->mask_display)
  {
    if(togglebutton == g->shadows_weight) g->mask_type = MASK_SHADOWS;
    if(togglebutton == g->mask_grey_fulcrum) g->mask_type = MASK_MIDTONES;
    if(togglebutton == g->highlights_weight) g->mask_type = MASK_HIGHLIGHTS;
  }
  else
  {
    g->mask_type = MASK_NONE;
  }

  dt_bauhaus_widget_set_quad_active(GTK_WIDGET(g->shadows_weight), g->mask_type == MASK_SHADOWS);
  dt_bauhaus_widget_set_quad_active(GTK_WIDGET(g->mask_grey_fulcrum), g->mask_type == MASK_MIDTONES);
  dt_bauhaus_widget_set_quad_active(GTK_WIDGET(g->highlights_weight), g->mask_type == MASK_HIGHLIGHTS);

  dt_iop_refresh_center(self);
}


static gboolean dt_iop_tonecurve_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorbalancergb_params_t *p = (dt_iop_colorbalancergb_params_t *)self->params;
  const float shadows_weight = 2.f + p->shadows_weight * 2.f;
  const float highlights_weight = 2.f + p->highlights_weight * 2.f;

  // Cache the graph objects to avoid recomputing all the view at each redraw
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  GtkStyleContext *context = gtk_widget_get_style_context(widget);

  cairo_surface_t *cst =
    dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, allocation.width, allocation.height);
  PangoFontDescription *desc =
    pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
  cairo_t *cr = cairo_create(cst);
  PangoLayout *layout = pango_cairo_create_layout(cr);

  const gint font_size = pango_font_description_get_size(desc);
  pango_font_description_set_size(desc, 0.95 * font_size);
  pango_layout_set_font_description(layout, desc);
  pango_cairo_context_set_resolution(pango_layout_get_context(layout), darktable.gui->dpi);

  char text[256];

  // Get the text line height for spacing
  PangoRectangle ink;
  snprintf(text, sizeof(text), "X");
  pango_layout_set_text(layout, text, -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  const float line_height = ink.height;

  const float inset = DT_PIXEL_APPLY_DPI(4);
  const float margin_top = inset;
  const float margin_bottom = line_height + 2 * inset;
  const float margin_left = line_height + inset;
  const float margin_right = 0;

  const float graph_width = allocation.width - margin_right - margin_left;   // align the right border on sliders
  const float graph_height = allocation.height - margin_bottom - margin_top; // give room to nodes

  gtk_render_background(context, cr, 0, 0, allocation.width, allocation.height);

  // draw x gradient as axis legend
  cairo_pattern_t *grad;
  grad = cairo_pattern_create_linear(margin_left, 0.0, graph_width, 0.0);
  dt_cairo_perceptual_gradient(grad, 1.0);
  cairo_set_line_width(cr, 0.0);
  cairo_rectangle(cr, margin_left, graph_height + 2 * inset, graph_width, line_height);
  cairo_set_source(cr, grad);
  cairo_fill(cr);
  cairo_pattern_destroy(grad);

  // draw y gradient as axis legend
  const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, line_height);
  unsigned char *data = malloc(stride * graph_height);
  cairo_surface_t *surface = cairo_image_surface_create_for_data(data, CAIRO_FORMAT_ARGB32, (size_t)line_height, (size_t)graph_height, stride);

  const size_t checker_1 = DT_PIXEL_APPLY_DPI(6);
  const size_t checker_2 = 2 * checker_1;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(data, graph_height, line_height, checker_1, checker_2) \
  schedule(static) collapse(2)
#endif
  for(size_t i = 0; i < (size_t)graph_height; i++)
    for(size_t j = 0; j < (size_t)line_height; j++)
    {
      const size_t k = ((i * (size_t)line_height) + j) * 4;
      unsigned char color;
      const float alpha = (float)i / graph_height;
      if(i % checker_1 < i % checker_2)
      {
        if(j % checker_1 < j % checker_2) color = 150;
        else color = 100;
      }
      else
      {
        if(j % checker_1 < j % checker_2) color = 100;
        else color = 150;
      }

      for(size_t c = 0; c < 4; ++c) data[k + c] = color * alpha;
      data[k+3] = alpha * 255;
    }

  cairo_set_source_surface(cr, surface, 0, margin_top);
  cairo_paint(cr);
  free(data);
  cairo_surface_destroy(surface);

  // set the graph as the origin of the coordinates
  cairo_translate(cr, margin_left, margin_top);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

  set_color(cr, darktable.bauhaus->graph_bg);
  cairo_rectangle(cr, 0, 0, graph_width, graph_height);
  cairo_fill_preserve(cr);
  cairo_clip(cr);

  // from https://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.583.3007&rep=rep1&type=pdf
  const float midtones_weight
      = sqf(shadows_weight) * sqf(highlights_weight) / (sqf(shadows_weight) + sqf(highlights_weight));
  const float mask_grey_fulcrum = powf(p->mask_grey_fulcrum, 0.4101205819200422f);

  float *LUT[3];
  for(size_t c = 0; c < 3; c++) LUT[c] = dt_alloc_align_float(LUT_ELEM);

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(LUT, shadows_weight, midtones_weight, highlights_weight, mask_grey_fulcrum) \
  schedule(static)
#endif
  for(size_t k = 0 ; k < LUT_ELEM; k++)
  {
    const float Y = k / (float)(LUT_ELEM - 1);
    dt_aligned_pixel_t output;
    opacity_masks(Y, shadows_weight, highlights_weight, midtones_weight, mask_grey_fulcrum, output, NULL);
    for(size_t c = 0; c < 3; c++) LUT[c][k] = output[c];
  }

  GdkRGBA fg_color = darktable.bauhaus->graph_fg;
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));

  for(size_t c = 0; c < 3; c++)
  {
    GdkRGBA line_color = { fg_color.red * (1. - (2 - c) / 4.),
                           fg_color.green * (1. - (2 - c) / 4.),
                           fg_color.blue * (1. - (2 - c) / 4.),
                           fg_color.alpha };
    set_color(cr, line_color);

    cairo_move_to(cr, 0, (1.f - LUT[c][0]) * graph_height);
    for(size_t k = 0; k < LUT_ELEM; k++)
    {
      const float x = (float)k / (float)(LUT_ELEM - 1) * graph_width;
      const float y = (1.f - LUT[c][k]) * graph_height;
      cairo_line_to(cr, x, y);
    }
    cairo_stroke(cr);
  }

  for(size_t c = 0; c < 3; c++) dt_free_align(LUT[c]);

  cairo_restore(cr);

  // restore font size
  pango_font_description_set_size(desc, font_size);
  pango_layout_set_font_description(layout, desc);

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  g_object_unref(layout);
  pango_font_description_free(desc);
  return TRUE;
}


static void checker_1_picker_callback(GtkColorButton *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  GdkRGBA color;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), &color);
  dt_conf_set_float("plugins/darkroom/colorbalancergb/checker1/red", color.red);
  dt_conf_set_float("plugins/darkroom/colorbalancergb/checker1/green", color.green);
  dt_conf_set_float("plugins/darkroom/colorbalancergb/checker1/blue", color.blue);
  dt_iop_refresh_center(self);
}


static void checker_2_picker_callback(GtkColorButton *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  GdkRGBA color;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), &color);
  dt_conf_set_float("plugins/darkroom/colorbalancergb/checker2/red", color.red);
  dt_conf_set_float("plugins/darkroom/colorbalancergb/checker2/green", color.green);
  dt_conf_set_float("plugins/darkroom/colorbalancergb/checker2/blue", color.blue);
  dt_iop_refresh_center(self);
}


static void checker_size_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  const size_t size = dt_bauhaus_slider_get(widget);
  dt_conf_set_int("plugins/darkroom/colorbalancergb/checker/size", size);
  dt_iop_refresh_center(self);
}


void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_colorbalancergb_gui_data_t *g = (dt_iop_colorbalancergb_gui_data_t *)self->gui_data;
  dt_iop_colorbalancergb_params_t *p = (dt_iop_colorbalancergb_params_t *)self->params;

  // Prepare data for gamut mapping slider backgrounds
  // Make our best effort to find display profile. If not found
  // or it is not a matrix profile, fall back to sRGB.
  const dt_iop_order_iccprofile_info_t *output_profile = NULL;

  if(self->dev && self->dev->pipe)
    output_profile = dt_ioppr_get_pipe_output_profile_info(self->dev->pipe);

  if(!output_profile || isnan(output_profile->matrix_out[0][0]))
  {
    output_profile = dt_ioppr_add_profile_info_to_list(self->dev, DT_COLORSPACE_SRGB, "",
                                                       DT_INTENT_RELATIVE_COLORIMETRIC);
  }

  // Prepare LMS 2006 -> RGB matrix for chroma clipping
  dt_colormatrix_t input_matrix = { { 0.f } };  // this is actually unused but is generated by below function anyway
  dt_colormatrix_t output_matrix = { { 0.f } };
  prepare_RGB_Yrg_matrices(output_profile, input_matrix, output_matrix);

  // Check if output profile is different than last time - will need to repaint
  // slider backgrounds.
  const gboolean output_profile_changed = output_profile != g->sliders_output_profile;

   ++darktable.gui->reset;

  if(output_profile_changed)
    paint_hue_sliders(output_profile, output_matrix, g);

  if(!w || w == g->global_H || output_profile_changed)
    paint_chroma_slider(output_profile, output_matrix, g->global_C, p->global_H);

  if(!w || w == g->shadows_H || output_profile_changed)
    paint_chroma_slider(output_profile, output_matrix, g->shadows_C, p->shadows_H);

  if(!w || w == g->midtones_H || output_profile_changed)
    paint_chroma_slider(output_profile, output_matrix, g->midtones_C, p->midtones_H);

  if(!w || w == g->highlights_H || output_profile_changed)
    paint_chroma_slider(output_profile, output_matrix, g->highlights_C, p->highlights_H);

  if(!w || w == g->shadows_weight || w == g->highlights_weight || w == g->mask_grey_fulcrum)
    gtk_widget_queue_draw(GTK_WIDGET(g->area));

  --darktable.gui->reset;

  g->sliders_output_profile = output_profile;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_colorbalancergb_gui_data_t *g = (dt_iop_colorbalancergb_gui_data_t *)self->gui_data;
  dt_iop_colorbalancergb_params_t *p = (dt_iop_colorbalancergb_params_t *)self->params;

  dt_bauhaus_slider_set(g->hue_angle, p->hue_angle);
  dt_bauhaus_slider_set(g->vibrance, p->vibrance);
  dt_bauhaus_slider_set(g->contrast, p->contrast);

  dt_bauhaus_slider_set(g->chroma_global, p->chroma_global);
  dt_bauhaus_slider_set(g->chroma_highlights, p->chroma_highlights);
  dt_bauhaus_slider_set(g->chroma_midtones, p->chroma_midtones);
  dt_bauhaus_slider_set(g->chroma_shadows, p->chroma_shadows);

  dt_bauhaus_slider_set(g->saturation_global, p->saturation_global);
  dt_bauhaus_slider_set(g->saturation_highlights, p->saturation_highlights);
  dt_bauhaus_slider_set(g->saturation_midtones, p->saturation_midtones);
  dt_bauhaus_slider_set(g->saturation_shadows, p->saturation_shadows);

  dt_bauhaus_slider_set(g->brilliance_global, p->brilliance_global);
  dt_bauhaus_slider_set(g->brilliance_highlights, p->brilliance_highlights);
  dt_bauhaus_slider_set(g->brilliance_midtones, p->brilliance_midtones);
  dt_bauhaus_slider_set(g->brilliance_shadows, p->brilliance_shadows);

  dt_bauhaus_slider_set(g->global_C, p->global_C);
  dt_bauhaus_slider_set(g->global_H, p->global_H);
  dt_bauhaus_slider_set(g->global_Y, p->global_Y);

  dt_bauhaus_slider_set(g->shadows_C, p->shadows_C);
  dt_bauhaus_slider_set(g->shadows_H, p->shadows_H);
  dt_bauhaus_slider_set(g->shadows_Y, p->shadows_Y);
  dt_bauhaus_slider_set(g->shadows_weight, p->shadows_weight);

  dt_bauhaus_slider_set(g->midtones_C, p->midtones_C);
  dt_bauhaus_slider_set(g->midtones_H, p->midtones_H);
  dt_bauhaus_slider_set(g->midtones_Y, p->midtones_Y);
  dt_bauhaus_slider_set(g->white_fulcrum, p->white_fulcrum);

  dt_bauhaus_slider_set(g->highlights_C, p->highlights_C);
  dt_bauhaus_slider_set(g->highlights_H, p->highlights_H);
  dt_bauhaus_slider_set(g->highlights_Y, p->highlights_Y);
  dt_bauhaus_slider_set(g->highlights_weight, p->highlights_weight);

  dt_bauhaus_slider_set(g->mask_grey_fulcrum, p->mask_grey_fulcrum);
  dt_bauhaus_slider_set(g->grey_fulcrum, p->grey_fulcrum);
  dt_bauhaus_combobox_set(g->saturation_formula, p->saturation_formula);

  gui_changed(self, NULL, NULL);
  dt_iop_color_picker_reset(self, TRUE);
  g->mask_display = FALSE;
  g->mask_type = MASK_NONE;

  dt_bauhaus_widget_set_quad_active(GTK_WIDGET(g->shadows_weight), FALSE);
  dt_bauhaus_widget_set_quad_active(GTK_WIDGET(g->mask_grey_fulcrum), FALSE);
  dt_bauhaus_widget_set_quad_active(GTK_WIDGET(g->highlights_weight), FALSE);

  // Checkerboard mask preview preferences
  GdkRGBA color;
  color.alpha = 1.0f;
  color.red = dt_conf_get_float("plugins/darkroom/colorbalancergb/checker1/red");
  color.green = dt_conf_get_float("plugins/darkroom/colorbalancergb/checker1/green");
  color.blue = dt_conf_get_float("plugins/darkroom/colorbalancergb/checker1/blue");

  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->checker_color_1_picker), &color);

  color.red = dt_conf_get_float("plugins/darkroom/colorbalancergb/checker2/red");
  color.green = dt_conf_get_float("plugins/darkroom/colorbalancergb/checker2/green");
  color.blue = dt_conf_get_float("plugins/darkroom/colorbalancergb/checker2/blue");

  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->checker_color_2_picker), &color);

  dt_bauhaus_slider_set(g->checker_size, dt_conf_get_int("plugins/darkroom/colorbalancergb/checker/size"));
}


void gui_reset(dt_iop_module_t *self)
{
  //dt_iop_colorbalancergb_gui_data_t *g = (dt_iop_colorbalancergb_gui_data_t *)self->gui_data;
  dt_iop_color_picker_reset(self, TRUE);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_colorbalancergb_gui_data_t *g = IOP_GUI_ALLOC(colorbalancergb);
  g->mask_display = FALSE;
  dt_iop_module_t *sect = NULL;

  // start building top level widget
  static dt_action_def_t notebook_def = { };
  g->notebook = dt_ui_notebook_new(&notebook_def);
  dt_action_define_iop(self, NULL, N_("page"), GTK_WIDGET(g->notebook), &notebook_def);

  // Page master
  self->widget = dt_ui_notebook_page(g->notebook, N_("master"), _("global grading"));

  g->hue_angle = dt_bauhaus_slider_from_params(self, "hue_angle");
  dt_bauhaus_slider_set_format(g->hue_angle, "°");
  gtk_widget_set_tooltip_text(g->hue_angle, _("rotate all hues by an angle, at the same luminance"));

  g->vibrance = dt_bauhaus_slider_from_params(self, "vibrance");
  dt_bauhaus_slider_set_soft_range(g->vibrance, -0.5, 0.5);
  dt_bauhaus_slider_set_digits(g->vibrance, 4);
  dt_bauhaus_slider_set_format(g->vibrance, "%");
  gtk_widget_set_tooltip_text(g->vibrance, _("increase colorfulness mostly on low-chroma colors"));

  g->contrast = dt_bauhaus_slider_from_params(self, "contrast");
  dt_bauhaus_slider_set_soft_range(g->contrast, -0.5, 0.5);
  dt_bauhaus_slider_set_digits(g->contrast, 4);
  dt_bauhaus_slider_set_format(g->contrast, "%");
  gtk_widget_set_tooltip_text(g->contrast, _("increase the contrast at constant chromaticity"));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(C_("section", "linear chroma grading")), FALSE, FALSE, 0);
  sect = DT_IOP_SECTION_FOR_PARAMS(self, N_("chroma"));

  g->chroma_global = dt_bauhaus_slider_from_params(self, "chroma_global");
  dt_bauhaus_slider_set_soft_range(g->chroma_global, -0.5, 0.5);
  dt_bauhaus_slider_set_digits(g->chroma_global, 4);
  dt_bauhaus_slider_set_format(g->chroma_global, "%");
  gtk_widget_set_tooltip_text(g->chroma_global, _("increase colorfulness at same luminance globally"));

  g->chroma_shadows = dt_bauhaus_slider_from_params(sect, "chroma_shadows");
  dt_bauhaus_slider_set_digits(g->chroma_shadows, 4);
  dt_bauhaus_slider_set_format(g->chroma_shadows, "%");
  gtk_widget_set_tooltip_text(g->chroma_shadows, _("increase colorfulness at same luminance mostly in shadows"));

  g->chroma_midtones = dt_bauhaus_slider_from_params(sect, "chroma_midtones");
  dt_bauhaus_slider_set_digits(g->chroma_midtones, 4);
  dt_bauhaus_slider_set_format(g->chroma_midtones, "%");
  gtk_widget_set_tooltip_text(g->chroma_midtones, _("increase colorfulness at same luminance mostly in mid-tones"));

  g->chroma_highlights = dt_bauhaus_slider_from_params(sect, "chroma_highlights");
  dt_bauhaus_slider_set_digits(g->chroma_highlights, 4);
  dt_bauhaus_slider_set_format(g->chroma_highlights, "%");
  gtk_widget_set_tooltip_text(g->chroma_highlights, _("increase colorfulness at same luminance mostly in highlights"));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(C_("section", "perceptual saturation grading")), FALSE, FALSE, 0);
  sect = DT_IOP_SECTION_FOR_PARAMS(self, N_("saturation"));

  g->saturation_global = dt_bauhaus_slider_from_params(self, "saturation_global");
  dt_bauhaus_slider_set_digits(g->saturation_global, 4);
  dt_bauhaus_slider_set_format(g->saturation_global, "%");
  gtk_widget_set_tooltip_text(g->saturation_global, _("add or remove saturation by an absolute amount"));

  g->saturation_shadows = dt_bauhaus_slider_from_params(sect, "saturation_shadows");
  dt_bauhaus_slider_set_digits(g->saturation_shadows, 4);
  dt_bauhaus_slider_set_format(g->saturation_shadows, "%");
  gtk_widget_set_tooltip_text(g->saturation_shadows, _("increase or decrease saturation proportionally to the original pixel saturation"));

  g->saturation_midtones= dt_bauhaus_slider_from_params(sect, "saturation_midtones");
  dt_bauhaus_slider_set_digits(g->saturation_midtones, 4);
  dt_bauhaus_slider_set_format(g->saturation_midtones, "%");
  gtk_widget_set_tooltip_text(g->saturation_midtones, _("increase or decrease saturation proportionally to the original pixel saturation"));

  g->saturation_highlights = dt_bauhaus_slider_from_params(sect, "saturation_highlights");
  dt_bauhaus_slider_set_digits(g->saturation_highlights, 4);
  dt_bauhaus_slider_set_format(g->saturation_highlights, "%");
  gtk_widget_set_tooltip_text(g->saturation_highlights, _("increase or decrease saturation proportionally to the original pixel saturation"));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(C_("section", "perceptual brilliance grading")), FALSE, FALSE, 0);
  sect = DT_IOP_SECTION_FOR_PARAMS(self, N_("brilliance"));

  g->brilliance_global = dt_bauhaus_slider_from_params(self, "brilliance_global");
  dt_bauhaus_slider_set_digits(g->brilliance_global, 4);
  dt_bauhaus_slider_set_format(g->brilliance_global, "%");
  gtk_widget_set_tooltip_text(g->brilliance_global, _("add or remove brilliance by an absolute amount"));

  g->brilliance_shadows = dt_bauhaus_slider_from_params(sect, "brilliance_shadows");
  dt_bauhaus_slider_set_digits(g->brilliance_shadows, 4);
  dt_bauhaus_slider_set_format(g->brilliance_shadows, "%");
  gtk_widget_set_tooltip_text(g->brilliance_shadows, _("increase or decrease brilliance proportionally to the original pixel brilliance"));

  g->brilliance_midtones= dt_bauhaus_slider_from_params(sect, "brilliance_midtones");
  dt_bauhaus_slider_set_digits(g->brilliance_midtones, 4);
  dt_bauhaus_slider_set_format(g->brilliance_midtones, "%");
  gtk_widget_set_tooltip_text(g->brilliance_midtones, _("increase or decrease brilliance proportionally to the original pixel brilliance"));

  g->brilliance_highlights = dt_bauhaus_slider_from_params(sect, "brilliance_highlights");
  dt_bauhaus_slider_set_digits(g->brilliance_highlights, 4);
  dt_bauhaus_slider_set_format(g->brilliance_highlights, "%");
  gtk_widget_set_tooltip_text(g->brilliance_highlights, _("increase or decrease brilliance proportionally to the original pixel brilliance"));

  // Page 4-ways
  self->widget = dt_ui_notebook_page(g->notebook, N_("4 ways"), _("selective color grading"));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(C_("section", "global offset")), FALSE, FALSE, 0);
  sect = DT_IOP_SECTION_FOR_PARAMS(self, N_("offset"));

  g->global_Y = dt_bauhaus_slider_from_params(sect, "global_Y");
  dt_bauhaus_slider_set_soft_range(g->global_Y, -0.05, 0.05);
  dt_bauhaus_slider_set_digits(g->global_Y, 4);
  dt_bauhaus_slider_set_format(g->global_Y, "%");
  gtk_widget_set_tooltip_text(g->global_Y, _("global luminance offset"));

  g->global_H = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(sect, "global_H"));
  dt_bauhaus_slider_set_feedback(g->global_H, 0);
  dt_bauhaus_slider_set_format(g->global_H, "°");
  gtk_widget_set_tooltip_text(g->global_H, _("hue of the global color offset"));

  g->global_C = dt_bauhaus_slider_from_params(sect, "global_C");
  dt_bauhaus_slider_set_soft_range(g->global_C, 0., 0.01);
  dt_bauhaus_slider_set_digits(g->global_C, 4);
  dt_bauhaus_slider_set_format(g->global_C, "%");
  gtk_widget_set_tooltip_text(g->global_C, _("chroma of the global color offset"));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(C_("section", "shadows lift")), FALSE, FALSE, 0);
  sect = DT_IOP_SECTION_FOR_PARAMS(self, N_("lift"));

  g->shadows_Y = dt_bauhaus_slider_from_params(sect, "shadows_Y");
  dt_bauhaus_slider_set_soft_range(g->shadows_Y, -1.0, 1.0);
  dt_bauhaus_slider_set_digits(g->shadows_Y, 4);
  dt_bauhaus_slider_set_format(g->shadows_Y, "%");
  gtk_widget_set_tooltip_text(g->shadows_Y, _("luminance gain in shadows"));

  g->shadows_H = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(sect, "shadows_H"));
  dt_bauhaus_slider_set_feedback(g->shadows_H, 0);
  dt_bauhaus_slider_set_format(g->shadows_H, "°");
  gtk_widget_set_tooltip_text(g->shadows_H, _("hue of the color gain in shadows"));

  g->shadows_C = dt_bauhaus_slider_from_params(sect, "shadows_C");
  dt_bauhaus_slider_set_soft_range(g->shadows_C, 0., 0.5);
  dt_bauhaus_slider_set_digits(g->shadows_C, 4);
  dt_bauhaus_slider_set_format(g->shadows_C, "%");
  gtk_widget_set_tooltip_text(g->shadows_C, _("chroma of the color gain in shadows"));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(C_("section", "highlights gain")), FALSE, FALSE, 0);
  sect = DT_IOP_SECTION_FOR_PARAMS(self, N_("gain"));

  g->highlights_Y = dt_bauhaus_slider_from_params(sect, "highlights_Y");
  dt_bauhaus_slider_set_soft_range(g->highlights_Y, -0.5, 0.5);
  dt_bauhaus_slider_set_digits(g->highlights_Y, 4);
  dt_bauhaus_slider_set_format(g->highlights_Y, "%");
  gtk_widget_set_tooltip_text(g->highlights_Y, _("luminance gain in highlights"));

  g->highlights_H = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(sect, "highlights_H"));
  dt_bauhaus_slider_set_feedback(g->highlights_H, 0);
  dt_bauhaus_slider_set_format(g->highlights_H, "°");
  gtk_widget_set_tooltip_text(g->highlights_H, _("hue of the color gain in highlights"));

  g->highlights_C = dt_bauhaus_slider_from_params(sect, "highlights_C");
  dt_bauhaus_slider_set_soft_range(g->highlights_C, 0., 0.2);
  dt_bauhaus_slider_set_digits(g->highlights_C, 4);
  dt_bauhaus_slider_set_format(g->highlights_C, "%");
  gtk_widget_set_tooltip_text(g->highlights_C, _("chroma of the color gain in highlights"));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(C_("section", "power")), FALSE, FALSE, 0);
  sect = DT_IOP_SECTION_FOR_PARAMS(self, N_("power"));

  g->midtones_Y = dt_bauhaus_slider_from_params(sect, "midtones_Y");
  dt_bauhaus_slider_set_soft_range(g->midtones_Y, -0.25, 0.25);
  dt_bauhaus_slider_set_digits(g->midtones_Y, 4);
  dt_bauhaus_slider_set_format(g->midtones_Y, "%");
  gtk_widget_set_tooltip_text(g->midtones_Y, _("luminance exponent in mid-tones"));

  g->midtones_H = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(sect, "midtones_H"));
  dt_bauhaus_slider_set_feedback(g->midtones_H, 0);
  dt_bauhaus_slider_set_format(g->midtones_H, "°");
  gtk_widget_set_tooltip_text(g->midtones_H, _("hue of the color exponent in mid-tones"));

  g->midtones_C = dt_bauhaus_slider_from_params(sect, "midtones_C");
  dt_bauhaus_slider_set_soft_range(g->midtones_C, 0., 0.1);
  dt_bauhaus_slider_set_digits(g->midtones_C, 4);
  dt_bauhaus_slider_set_format(g->midtones_C, "%");
  gtk_widget_set_tooltip_text(g->midtones_C, _("chroma of the color exponent in mid-tones"));

  // Page masks
  self->widget = dt_ui_notebook_page(g->notebook, N_("masks"), _("isolate luminances"));

  g->saturation_formula = dt_bauhaus_combobox_from_params(self, "saturation_formula");
  gtk_widget_set_tooltip_text(g->saturation_formula,
                              _("choose in which uniform color space the saturation is computed"));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(C_("section", "luminance ranges")), FALSE, FALSE, 0);

  g->area = GTK_DRAWING_AREA(dt_ui_resize_wrap(NULL, 0, "plugins/darkroom/colorbalancergb/aspect_percent"));
  g_object_set_data(G_OBJECT(g->area), "iop-instance", self);
  dt_action_define_iop(self, NULL, N_("graph"), GTK_WIDGET(g->area), NULL);
  g_signal_connect(G_OBJECT(g->area), "draw", G_CALLBACK(dt_iop_tonecurve_draw), self);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->area), FALSE, FALSE, 0);

  g->shadows_weight = dt_bauhaus_slider_from_params(self, "shadows_weight");
  dt_bauhaus_slider_set_digits(g->shadows_weight, 4);
  dt_bauhaus_slider_set_format(g->shadows_weight, "%");
  gtk_widget_set_tooltip_text(g->shadows_weight, _("weight of the shadows over the whole tonal range"));
  dt_bauhaus_widget_set_quad_paint(g->shadows_weight, dtgtk_cairo_paint_showmask, 0, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->shadows_weight, TRUE);
  g_signal_connect(G_OBJECT(g->shadows_weight), "quad-pressed", G_CALLBACK(mask_callback), self);

  g->mask_grey_fulcrum = dt_bauhaus_slider_from_params(self, "mask_grey_fulcrum");
  dt_bauhaus_slider_set_digits(g->mask_grey_fulcrum, 4);
  dt_bauhaus_slider_set_format(g->mask_grey_fulcrum, "%");
  gtk_widget_set_tooltip_text(g->mask_grey_fulcrum, _("position of the middle-gray reference for masking"));
  dt_bauhaus_widget_set_quad_paint(g->mask_grey_fulcrum, dtgtk_cairo_paint_showmask, 0, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->mask_grey_fulcrum, TRUE);
  g_signal_connect(G_OBJECT(g->mask_grey_fulcrum), "quad-pressed", G_CALLBACK(mask_callback), self);

  g->highlights_weight = dt_bauhaus_slider_from_params(self, "highlights_weight");
  dt_bauhaus_slider_set_digits(g->highlights_weight, 4);
  dt_bauhaus_slider_set_format(g->highlights_weight, "%");
  gtk_widget_set_tooltip_text(g->highlights_weight, _("weights of highlights over the whole tonal range"));
  dt_bauhaus_widget_set_quad_paint(g->highlights_weight, dtgtk_cairo_paint_showmask, 0, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->highlights_weight, TRUE);
  g_signal_connect(G_OBJECT(g->highlights_weight), "quad-pressed", G_CALLBACK(mask_callback), self);

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(C_("section", "threshold")), FALSE, FALSE, 0);

  g->white_fulcrum = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, "white_fulcrum"));
  dt_bauhaus_slider_set_soft_range(g->white_fulcrum, -2., +2.);
  dt_bauhaus_slider_set_format(g->white_fulcrum, _(" EV"));
  gtk_widget_set_tooltip_text(g->white_fulcrum, _("peak white luminance value used to normalize the power function"));

  g->grey_fulcrum = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, "grey_fulcrum"));
  dt_bauhaus_slider_set_soft_range(g->grey_fulcrum, 0.1, 0.5);
  dt_bauhaus_slider_set_digits(g->grey_fulcrum, 4);
  dt_bauhaus_slider_set_format(g->grey_fulcrum, "%");
  gtk_widget_set_tooltip_text(g->grey_fulcrum, _("peak gray luminance value used to normalize the power function"));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(C_("section", "mask preview settings")), FALSE, FALSE, 0);

  GtkWidget *row1 = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  gtk_box_pack_start(GTK_BOX(row1), dt_ui_label_new(_("checkerboard color 1")), TRUE, TRUE, 0);
  g->checker_color_1_picker = gtk_color_button_new();
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(g->checker_color_1_picker), FALSE);
  gtk_color_button_set_title(GTK_COLOR_BUTTON(g->checker_color_1_picker), _("select color of the checkerboard from a swatch"));
  gtk_box_pack_start(GTK_BOX(row1), GTK_WIDGET(g->checker_color_1_picker), FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->checker_color_1_picker), "color-set", G_CALLBACK(checker_1_picker_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(row1), FALSE, FALSE, 0);

  GtkWidget *row2 = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  gtk_box_pack_start(GTK_BOX(row2), dt_ui_label_new(_("checkerboard color 2")), TRUE, TRUE, 0);
  g->checker_color_2_picker = gtk_color_button_new();
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(g->checker_color_2_picker), FALSE);
  gtk_color_button_set_title(GTK_COLOR_BUTTON(g->checker_color_2_picker), _("select color of the checkerboard from a swatch"));
  gtk_box_pack_start(GTK_BOX(row2), GTK_WIDGET(g->checker_color_2_picker), FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->checker_color_2_picker), "color-set", G_CALLBACK(checker_2_picker_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(row2), FALSE, FALSE, 0);

  g->checker_size = dt_bauhaus_slider_new_with_range(self, 2., 32., 0, 8., 0);
  dt_bauhaus_slider_set_format(g->checker_size, " px");
  dt_bauhaus_widget_set_label(g->checker_size,  NULL, _("checkerboard size"));
  g_signal_connect(G_OBJECT(g->checker_size), "value-changed", G_CALLBACK(checker_size_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->checker_size), FALSE, FALSE, 0);

  // Init the conf keys if they don't exist
  if(!dt_conf_key_exists("plugins/darkroom/colorbalancergb/checker1/red"))
    dt_conf_set_float("plugins/darkroom/colorbalancergb/checker1/red", 1.0f);
  if(!dt_conf_key_exists("plugins/darkroom/colorbalancergb/checker1/green"))
    dt_conf_set_float("plugins/darkroom/colorbalancergb/checker1/green", 1.0f);
  if(!dt_conf_key_exists("plugins/darkroom/colorbalancergb/checker1/blue"))
    dt_conf_set_float("plugins/darkroom/colorbalancergb/checker1/blue", 1.0f);

  if(!dt_conf_key_exists("plugins/darkroom/colorbalancergb/checker2/red"))
    dt_conf_set_float("plugins/darkroom/colorbalancergb/checker2/red", 0.18f);
  if(!dt_conf_key_exists("plugins/darkroom/colorbalancergb/checker2/green"))
    dt_conf_set_float("plugins/darkroom/colorbalancergb/checker2/green", 0.18f);
  if(!dt_conf_key_exists("plugins/darkroom/colorbalancergb/checker2/blue"))
    dt_conf_set_float("plugins/darkroom/colorbalancergb/checker2/blue", 0.18f);

  if(!dt_conf_key_exists("plugins/darkroom/colorbalancergb/checker/size"))
    dt_conf_set_int("plugins/darkroom/colorbalancergb/checker/size", 8);

  // paint backgrounds
  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
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

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
