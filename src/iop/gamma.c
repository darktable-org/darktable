/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "common/colorspaces_inline_conversions.h"
#include "control/control.h"
#include "develop/develop.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

DT_MODULE_INTROSPECTION(1, dt_iop_gamma_params_t)


typedef struct dt_iop_gamma_params_t
{
  float gamma, linear;
} dt_iop_gamma_params_t;

const char *name()
{
  return C_("modulename", "gamma");
}

int groups()
{
  return IOP_GROUP_COLOR;
}

int flags()
{
  return IOP_FLAGS_HIDDEN | IOP_FLAGS_ONE_INSTANCE;
}

static inline float Hue_2_RGB(float v1, float v2, float vH)
{
  if(vH < 0.0f) vH += 1.0f;
  if(vH > 1.0f) vH -= 1.0f;
  if((6.0f * vH) < 1.0f) return (v1 + (v2 - v1) * 6.0f * vH);
  if((2.0f * vH) < 1.0f) return (v2);
  if((3.0f * vH) < 2.0f) return (v1 + (v2 - v1) * ((2.0f / 3.0f) - vH) * 6.0f);
  return v1;
}

static inline void HSL_2_RGB(const float *HSL, float *RGB)
{
  float H = HSL[0];
  float S = HSL[1];
  float L = HSL[2];

  float var_1, var_2;

  if(S < 1e-6f)
  {
    RGB[0] = RGB[1] = RGB[2] = L;
  }
  else
  {
    if(L < 0.5f)
      var_2 = L * (1.0f + S);
    else
      var_2 = (L + S) - (S * L);

    var_1 = 2.0f * L - var_2;

    RGB[0] = Hue_2_RGB(var_1, var_2, H + (1.0f / 3.0f));
    RGB[1] = Hue_2_RGB(var_1, var_2, H);
    RGB[2] = Hue_2_RGB(var_1, var_2, H - (1.0f / 3.0f));
  }
}

static inline void LCH_2_Lab(const float *LCH, float *Lab)
{
  Lab[0] = LCH[0];
  Lab[1] = cosf(2.0f * M_PI * LCH[2]) * LCH[1];
  Lab[2] = sinf(2.0f * M_PI * LCH[2]) * LCH[1];
}

static inline void LCH_2_RGB(const float *LCH, float *RGB)
{
  float Lab[3], XYZ[3];
  LCH_2_Lab(LCH, Lab);
  dt_Lab_to_XYZ(Lab, XYZ);
  dt_XYZ_to_sRGB_clipped(XYZ, RGB);
}

static inline void Lab_2_RGB(const float *Lab, float *RGB)
{
  float XYZ[3];
  dt_Lab_to_XYZ(Lab, XYZ);
  dt_XYZ_to_sRGB_clipped(XYZ, RGB);
}

static inline void false_color(float val, dt_dev_pixelpipe_display_mask_t channel, float *out)
{
  float in[3];

  switch((channel & DT_DEV_PIXELPIPE_DISPLAY_ANY) & ~DT_DEV_PIXELPIPE_DISPLAY_OUTPUT)
  {
    case DT_DEV_PIXELPIPE_DISPLAY_L:
      in[0] = val * 100.0f;
      in[1] = 0.0f;
      in[2] = 0.0f;
      Lab_2_RGB(in, out);
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_a:
      in[0] = 80.0f;
      in[1] = val * 256.0f - 128.0f;
      in[2] = 0.0f;
      Lab_2_RGB(in, out);
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_b:
      in[0] = 80.0f;
      in[1] = 0.0f;
      in[2] = val * 256.0f - 128.0f;
      Lab_2_RGB(in, out);
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_R:
      out[0] = val;
      out[1] = out[2] = 0.0f;
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_G:
      out[1] = val;
      out[0] = out[2] = 0.0f;
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_B:
      out[2] = val;
      out[0] = out[1] = 0.0f;
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_LCH_C:
      in[0] = 80.0f;
      in[1] = val * 128.0f * sqrtf(2.0f);
      in[2] = 0.9111f;
      LCH_2_RGB(in, out);
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_LCH_h:
      in[0] = 50.0f;
      in[1] = 0.25f * 128.0f * sqrtf(2.0f);
      in[2] = val;
      LCH_2_RGB(in, out);
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_HSL_H:
      in[0] = val;
      in[1] = 1.0f;
      in[2] = 0.5f;
      HSL_2_RGB(in, out);
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_HSL_S:
      in[0] = 0.8333f;
      in[1] = val;
      in[2] = 0.5f;
      HSL_2_RGB(in, out);
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_HSL_l:
      in[0] = 0.0f;
      in[1] = 0.0f;
      in[2] = val;
      HSL_2_RGB(in, out);
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_GRAY:
    default:
      out[0] = out[1] = out[2] = val;
      break;
  }
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const i, void *const o,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const int ch = piece->colors;

  const dt_dev_pixelpipe_display_mask_t mask_display = piece->pipe->mask_display;
  char *str = dt_conf_get_string("channel_display");
  const int fcolor = !strcmp(str, "false color");
  g_free(str);

  if((mask_display & DT_DEV_PIXELPIPE_DISPLAY_CHANNEL) && (mask_display & DT_DEV_PIXELPIPE_DISPLAY_ANY) && fcolor)
  {
    const float yellow[3] = { 1.0f, 1.0f, 0.0f };
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
    for(int k = 0; k < roi_out->height; k++)
    {
      const float *in = ((float *)i) + (size_t)ch * k * roi_out->width;
      uint8_t *out = ((uint8_t *)o) + (size_t)ch * k * roi_out->width;
      for(int j = 0; j < roi_out->width; j++, in += ch, out += ch)
      {
        const float alpha = (mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) ? in[3] : 0.0f;
        float colors[3];
        false_color(in[1], mask_display, colors);
        for(int c = 0; c < 3; c++)
        {
          const float value = colors[c] * (1.0f - alpha) + yellow[c] * alpha;
          out[2 - c] = ((uint8_t)(CLAMP(((uint32_t)255.0f * value), 0x0, 0xff)));
        }
      }
    }
  }
  else if((mask_display & DT_DEV_PIXELPIPE_DISPLAY_CHANNEL) && (mask_display & DT_DEV_PIXELPIPE_DISPLAY_ANY))
  {
    const float yellow[3] = { 1.0f, 1.0f, 0.0f };
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
    for(int k = 0; k < roi_out->height; k++)
    {
      const float *in = ((float *)i) + (size_t)ch * k * roi_out->width;
      uint8_t *out = ((uint8_t *)o) + (size_t)ch * k * roi_out->width;
      for(int j = 0; j < roi_out->width; j++, in += ch, out += ch)
      {
        float alpha = (mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) ? in[3] : 0.0f;
        for(int c = 0; c < 3; c++)
        {
          const float value = in[1] * (1.0f - alpha) + yellow[c] * alpha;
          out[2 - c] = ((uint8_t)(CLAMP(((uint32_t)255.0f * value), 0x0, 0xff)));
        }
      }
    }
  }
  else if(mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
  {
    const float yellow[3] = { 1.0f, 1.0f, 0.0f };
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
    for(int k = 0; k < roi_out->height; k++)
    {
      const float *in = ((float *)i) + (size_t)ch * k * roi_out->width;
      uint8_t *out = ((uint8_t *)o) + (size_t)ch * k * roi_out->width;
      for(int j = 0; j < roi_out->width; j++, in += ch, out += ch)
      {
        const float gray = 0.3f * in[0] + 0.59f * in[1] + 0.11f * in[2];
        const float alpha = in[3];
        for(int c = 0; c < 3; c++)
        {
          const float value = gray * (1.0f - alpha) + yellow[c] * alpha;
          out[2 - c] = ((uint8_t)(CLAMP(((uint32_t)255.0f * value), 0x0, 0xff)));
        }
      }
    }
  }
  else
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
    for(int k = 0; k < roi_out->height; k++)
    {
      const float *in = ((float *)i) + (size_t)ch * k * roi_out->width;
      uint8_t *out = ((uint8_t *)o) + (size_t)ch * k * roi_out->width;
      for(int j = 0; j < roi_out->width; j++, in += ch, out += ch)
      {
        for(int c = 0; c < 3; c++) out[2 - c] = ((uint8_t)(CLAMP(((uint32_t)255.0f * in[c]), 0x0, 0xff)));
      }
    }
  }
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_gamma_data_t));
  module->params = calloc(1, sizeof(dt_iop_gamma_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_gamma_params_t));
  module->params_size = sizeof(dt_iop_gamma_params_t);
  module->gui_data = NULL;
  module->priority = 1000; // module order created by iop_dependencies.py, do not edit!
  module->hide_enable_button = 1;
  module->default_enabled = 1;
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
