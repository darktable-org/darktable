/*
    This file is part of darktable,
    copyright (c) 2009--2012 johannes hanika.
    copyright (c) 2011 Henrik Andersson.
    copyright (c) 2012--2013 Ulrich Pegelow

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
#include "bauhaus/bauhaus.h"
#include "common/debug.h"
#include "common/dtpthread.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/tiling.h"
#include "dtgtk/button.h"
#include "dtgtk/gradientslider.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"

#include <assert.h>
#include <gmodule.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CLAMP_RANGE(x, y, z) (CLAMP(x, y, z))
#define NEUTRAL_GRAY 0.5

typedef enum _iop_gui_blendif_channel_t
{
  ch_L = 0,
  ch_a = 1,
  ch_b = 2,
  ch_gray = 0,
  ch_red = 1,
  ch_green = 2,
  ch_blue = 3,
  ch_max = 4
} _iop_gui_blendif_channel_t;



static const dt_iop_gui_blendif_colorstop_t _gradient_L[]
    = { { 0.0f, { 0, 0, 0, 1.0 } },
        { 0.5f, { NEUTRAL_GRAY / 2, NEUTRAL_GRAY / 2, NEUTRAL_GRAY / 2, 1.0 } },
        { 1.0f, { NEUTRAL_GRAY, NEUTRAL_GRAY, NEUTRAL_GRAY, 1.0 } } };

static const dt_iop_gui_blendif_colorstop_t _gradient_a[]
    = { { 0.0f, { 0, 0.34 * NEUTRAL_GRAY * 2, 0.27 * NEUTRAL_GRAY * 2, 1.0 } },
        { 0.5f, { NEUTRAL_GRAY, NEUTRAL_GRAY, NEUTRAL_GRAY, 1.0 } },
        { 1.0f, { 0.53 * NEUTRAL_GRAY * 2, 0.08 * NEUTRAL_GRAY * 2, 0.28 * NEUTRAL_GRAY * 2, 1.0 } } };

static const dt_iop_gui_blendif_colorstop_t _gradient_b[]
    = { { 0.0f, { 0, 0.27 * NEUTRAL_GRAY * 2, 0.58 * NEUTRAL_GRAY * 2, 1.0 } },
        { 0.5f, { NEUTRAL_GRAY, NEUTRAL_GRAY, NEUTRAL_GRAY, 1.0 } },
        { 1.0f, { 0.81 * NEUTRAL_GRAY * 2, 0.66 * NEUTRAL_GRAY * 2, 0, 1.0 } } };

static const dt_iop_gui_blendif_colorstop_t _gradient_gray[]
    = { { 0.0f, { 0, 0, 0, 1.0 } },
        { 0.5f, { NEUTRAL_GRAY / 2, NEUTRAL_GRAY / 2, NEUTRAL_GRAY / 2, 1.0 } },
        { 1.0f, { NEUTRAL_GRAY, NEUTRAL_GRAY, NEUTRAL_GRAY, 1.0 } } };

static const dt_iop_gui_blendif_colorstop_t _gradient_red[] = { { 0.0f, { 0, 0, 0, 1.0 } },
                                                                { 0.5f, { NEUTRAL_GRAY / 2, 0, 0, 1.0 } },
                                                                { 1.0f, { NEUTRAL_GRAY, 0, 0, 1.0 } } };

static const dt_iop_gui_blendif_colorstop_t _gradient_green[] = { { 0.0f, { 0, 0, 0, 1.0 } },
                                                                  { 0.5f, { 0, NEUTRAL_GRAY / 2, 0, 1.0 } },
                                                                  { 1.0f, { 0, NEUTRAL_GRAY, 0, 1.0 } } };

static const dt_iop_gui_blendif_colorstop_t _gradient_blue[] = { { 0.0f, { 0, 0, 0, 1.0 } },
                                                                 { 0.5f, { 0, 0, NEUTRAL_GRAY / 2, 1.0 } },
                                                                 { 1.0f, { 0, 0, NEUTRAL_GRAY, 1.0 } } };

static const dt_iop_gui_blendif_colorstop_t _gradient_chroma[]
    = { { 0.0f, { NEUTRAL_GRAY, NEUTRAL_GRAY, NEUTRAL_GRAY, 1.0 } },
        { 0.5f, { NEUTRAL_GRAY, NEUTRAL_GRAY / 2, NEUTRAL_GRAY, 1.0 } },
        { 1.0f, { NEUTRAL_GRAY, 0, NEUTRAL_GRAY, 1.0 } } };

static const dt_iop_gui_blendif_colorstop_t _gradient_hue[] = {
  { 0.0f, { 1.00f * 1.5f * NEUTRAL_GRAY, 0.68f * 1.5f * NEUTRAL_GRAY, 0.78f * 1.5f * NEUTRAL_GRAY, 1.0 } },
  { 0.166f, { 0.95f * 1.5f * NEUTRAL_GRAY, 0.73f * 1.5f * NEUTRAL_GRAY, 0.56f * 1.5f * NEUTRAL_GRAY, 1.0 } },
  { 0.333f, { 0.71f * 1.5f * NEUTRAL_GRAY, 0.81f * 1.5f * NEUTRAL_GRAY, 0.55f * 1.5f * NEUTRAL_GRAY, 1.0 } },
  { 0.500f, { 0.45f * 1.5f * NEUTRAL_GRAY, 0.85f * 1.5f * NEUTRAL_GRAY, 0.77f * 1.5f * NEUTRAL_GRAY, 1.0 } },
  { 0.666f, { 0.49f * 1.5f * NEUTRAL_GRAY, 0.82f * 1.5f * NEUTRAL_GRAY, 1.00f * 1.5f * NEUTRAL_GRAY, 1.0 } },
  { 0.833f, { 0.82f * 1.5f * NEUTRAL_GRAY, 0.74f * 1.5f * NEUTRAL_GRAY, 1.00f * 1.5f * NEUTRAL_GRAY, 1.0 } },
  { 1.0f, { 1.00f * 1.5f * NEUTRAL_GRAY, 0.68f * 1.5f * NEUTRAL_GRAY, 0.78f * 1.5f * NEUTRAL_GRAY, 1.0 } }
};

static const dt_iop_gui_blendif_colorstop_t _gradient_HUE[]
    = { { 0.0f, { NEUTRAL_GRAY, 0, 0, 1.0 } },
        { 0.166f, { NEUTRAL_GRAY, NEUTRAL_GRAY, 0, 1.0 } },
        { 0.332f, { 0, NEUTRAL_GRAY, 0, 1.0 } },
        { 0.498f, { 0, NEUTRAL_GRAY, NEUTRAL_GRAY, 1.0 } },
        { 0.664f, { 0, 0, NEUTRAL_GRAY, 1.0 } },
        { 0.830f, { NEUTRAL_GRAY, 0, NEUTRAL_GRAY, 1.0 } },
        { 1.0f, { NEUTRAL_GRAY, 0, 0, 1.0 } } };


static inline void _RGB_2_HSL(const float *RGB, float *HSL)
{
  float H, S, L;

  float R = RGB[0];
  float G = RGB[1];
  float B = RGB[2];

  float var_Min = fminf(R, fminf(G, B));
  float var_Max = fmaxf(R, fmaxf(G, B));
  float del_Max = var_Max - var_Min;

  L = (var_Max + var_Min) / 2.0f;

  if(del_Max == 0.0f)
  {
    H = 0.0f;
    S = 0.0f;
  }
  else
  {
    if(L < 0.5f)
      S = del_Max / (var_Max + var_Min);
    else
      S = del_Max / (2.0f - var_Max - var_Min);

    float del_R = (((var_Max - R) / 6.0f) + (del_Max / 2.0f)) / del_Max;
    float del_G = (((var_Max - G) / 6.0f) + (del_Max / 2.0f)) / del_Max;
    float del_B = (((var_Max - B) / 6.0f) + (del_Max / 2.0f)) / del_Max;

    if(R == var_Max)
      H = del_B - del_G;
    else if(G == var_Max)
      H = (1.0f / 3.0f) + del_R - del_B;
    else if(B == var_Max)
      H = (2.0f / 3.0f) + del_G - del_R;
    else
      H = 0.0f; // make GCC happy

    if(H < 0.0f) H += 1.0f;
    if(H > 1.0f) H -= 1.0f;
  }

  HSL[0] = H;
  HSL[1] = S;
  HSL[2] = L;
}


static inline void _Lab_2_LCH(const float *Lab, float *LCH)
{
  float var_H = atan2f(Lab[2], Lab[1]);

  if(var_H > 0.0f)
    var_H = var_H / (2.0f * M_PI);
  else
    var_H = 1.0f - fabs(var_H) / (2.0f * M_PI);

  LCH[0] = Lab[0];
  LCH[1] = sqrtf(Lab[1] * Lab[1] + Lab[2] * Lab[2]);
  LCH[2] = var_H;
}


static void _blendif_scale(dt_iop_colorspace_type_t cst, const float *in, float *out)
{
  float temp[4];

  switch(cst)
  {
    case iop_cs_Lab:
      _Lab_2_LCH(in, temp);
      out[0] = CLAMP_RANGE(in[0] / 100.0f, 0.0f, 1.0f);
      out[1] = CLAMP_RANGE((in[1] + 128.0f) / 256.0f, 0.0f, 1.0f);
      out[2] = CLAMP_RANGE((in[2] + 128.0f) / 256.0f, 0.0f, 1.0f);
      out[3] = CLAMP_RANGE(temp[1] / (128.0f * sqrtf(2.0f)), 0.0f, 1.0f);
      out[4] = CLAMP_RANGE(temp[2], 0.0f, 1.0f);
      out[5] = out[6] = out[7] = -1;
      break;
    case iop_cs_rgb:
      _RGB_2_HSL(in, temp);
      out[0] = CLAMP_RANGE(0.3f * in[0] + 0.59f * in[1] + 0.11f * in[2], 0.0f, 1.0f);
      out[1] = CLAMP_RANGE(in[0], 0.0f, 1.0f);
      out[2] = CLAMP_RANGE(in[1], 0.0f, 1.0f);
      out[3] = CLAMP_RANGE(in[2], 0.0f, 1.0f);
      out[4] = CLAMP_RANGE(temp[0], 0.0f, 1.0f);
      out[5] = CLAMP_RANGE(temp[1], 0.0f, 1.0f);
      out[6] = CLAMP_RANGE(temp[2], 0.0f, 1.0f);
      out[7] = -1;
      break;
    default:
      out[0] = out[1] = out[2] = out[3] = out[4] = out[5] = out[6] = out[7] = -1.0f;
  }
}

static void _blendif_cook(dt_iop_colorspace_type_t cst, const float *in, float *out)
{
  float temp[4];

  switch(cst)
  {
    case iop_cs_Lab:
      _Lab_2_LCH(in, temp);
      out[0] = in[0];
      out[1] = in[1];
      out[2] = in[2];
      out[3] = temp[1] / (128.0f * sqrtf(2.0f)) * 100.0f;
      out[4] = temp[2] * 360.0f;
      out[5] = out[6] = out[7] = -1;
      break;
    case iop_cs_rgb:
      _RGB_2_HSL(in, temp);
      out[0] = (0.3f * in[0] + 0.59f * in[1] + 0.11f * in[2]) * 255.0f;
      out[1] = in[0] * 255.0f;
      out[2] = in[1] * 255.0f;
      out[3] = in[2] * 255.0f;
      out[4] = temp[0] * 360.0f;
      out[5] = temp[1] * 100.0f;
      out[6] = temp[2] * 100.0f;
      out[7] = -1;
      break;
    default:
      out[0] = out[1] = out[2] = out[3] = out[4] = out[5] = out[6] = out[7] = -1.0f;
  }
}


static void _blendif_scale_print_L(float value, char *string, int n)
{
  snprintf(string, n, "%-4.0f", value * 100.0f);
}

static void _blendif_scale_print_ab(float value, char *string, int n)
{
  snprintf(string, n, "%-4.0f", value * 256.0f - 128.0f);
}

static void _blendif_scale_print_rgb(float value, char *string, int n)
{
  snprintf(string, n, "%-4.0f", value * 255.0f);
}

static void _blendif_scale_print_hue(float value, char *string, int n)
{
  snprintf(string, n, "%-4.0f", value * 360.0f);
}

static void _blendif_scale_print_default(float value, char *string, int n)
{
  snprintf(string, n, "%-4.0f", value * 100.0f);
}

static void _blendop_masks_mode_callback(GtkWidget *combo, dt_iop_gui_blend_data_t *data)
{
  const unsigned int mask_mode = GPOINTER_TO_UINT(
      g_list_nth_data(data->masks_modes, dt_bauhaus_combobox_get(data->masks_modes_combo)));

  data->module->blend_params->mask_mode = mask_mode;

  if(mask_mode & DEVELOP_MASK_ENABLED)
  {
    gtk_widget_show(GTK_WIDGET(data->top_box));
  }
  else
  {
    gtk_widget_hide(GTK_WIDGET(data->top_box));
  }

  if((mask_mode & DEVELOP_MASK_ENABLED)
     && ((data->masks_inited && (mask_mode & DEVELOP_MASK_MASK))
         || (data->blendif_inited && (mask_mode & DEVELOP_MASK_CONDITIONAL))))
  {
    if(data->blendif_inited && (mask_mode & DEVELOP_MASK_CONDITIONAL))
    {
      dt_bauhaus_combobox_set(data->masks_combine_combo,
                              g_list_index(data->masks_combine,
                                           GUINT_TO_POINTER(data->module->blend_params->mask_combine
                                                            & (DEVELOP_COMBINE_INV | DEVELOP_COMBINE_INCL))));
      gtk_widget_hide(GTK_WIDGET(data->masks_invert_combo));
      gtk_widget_show(GTK_WIDGET(data->masks_combine_combo));
    }
    else
    {
      dt_bauhaus_combobox_set(
          data->masks_invert_combo,
          g_list_index(data->masks_invert,
                       GUINT_TO_POINTER(data->module->blend_params->mask_combine & DEVELOP_COMBINE_INV)));
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
    if(dt_iop_module_colorspace(data->module) == iop_cs_RAW)
    {
      data->module->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
      dtgtk_button_set_active(DTGTK_BUTTON(data->showmask), 0);
      gtk_widget_hide(GTK_WIDGET(data->showmask));

      // disable also guided-filters on RAW based color space
      gtk_widget_set_sensitive(data->masks_feathering_guide_combo, 0);
      gtk_widget_hide(GTK_WIDGET(data->masks_feathering_guide_combo));
      gtk_widget_set_sensitive(data->feathering_radius_slider, 0);
      gtk_widget_hide(GTK_WIDGET(data->feathering_radius_slider));
      gtk_widget_set_sensitive(data->brightness_slider, 0);
      gtk_widget_hide(GTK_WIDGET(data->brightness_slider));
      gtk_widget_set_sensitive(data->contrast_slider, 0);
      gtk_widget_hide(GTK_WIDGET(data->contrast_slider));
    }
    else
    {
      gtk_widget_show(GTK_WIDGET(data->showmask));
    }

    gtk_widget_show(GTK_WIDGET(data->bottom_box));
  }
  else
  {
    data->module->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
    dtgtk_button_set_active(DTGTK_BUTTON(data->showmask), 0);
    data->module->suppress_mask = 0;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->suppress), 0);

    gtk_widget_hide(GTK_WIDGET(data->bottom_box));
  }

  if(data->masks_inited && (mask_mode & DEVELOP_MASK_MASK))
  {
    gtk_widget_show(GTK_WIDGET(data->masks_box));
  }
  else if(data->masks_inited)
  {
    dt_masks_set_edit_mode(data->module, DT_MASKS_EDIT_OFF);
    gtk_widget_hide(GTK_WIDGET(data->masks_box));
  }
  else
  {
    gtk_widget_hide(GTK_WIDGET(data->masks_box));
  }


  if(data->blendif_inited && (mask_mode & DEVELOP_MASK_CONDITIONAL))
  {
    gtk_widget_show(GTK_WIDGET(data->blendif_box));
  }
  else if(data->blendif_inited)
  {
    /* switch off color picker - only if it was requested by blendif */
    if(data->module->request_color_pick == DT_REQUEST_COLORPICK_BLEND)
    {
      data->module->request_color_pick = DT_REQUEST_COLORPICK_OFF;
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->colorpicker), 0);
    }

    gtk_widget_hide(GTK_WIDGET(data->blendif_box));
  }
  else
  {
    gtk_widget_hide(GTK_WIDGET(data->blendif_box));
  }

  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
}


static void _blendop_blend_mode_callback(GtkWidget *combo, dt_iop_gui_blend_data_t *data)
{
  data->module->blend_params->blend_mode = GPOINTER_TO_UINT(
      g_list_nth_data(data->blend_modes, dt_bauhaus_combobox_get(data->blend_modes_combo)));
  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
}

static void _blendop_masks_combine_callback(GtkWidget *combo, dt_iop_gui_blend_data_t *data)
{
  const unsigned combine = GPOINTER_TO_UINT(
      g_list_nth_data(data->masks_combine, dt_bauhaus_combobox_get(data->masks_combine_combo)));
  data->module->blend_params->mask_combine &= ~(DEVELOP_COMBINE_INV | DEVELOP_COMBINE_INCL);
  data->module->blend_params->mask_combine |= combine;
  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
}

static void _blendop_masks_invert_callback(GtkWidget *combo, dt_iop_gui_blend_data_t *data)
{
  unsigned int invert = GPOINTER_TO_UINT(g_list_nth_data(data->masks_invert,
                                                         dt_bauhaus_combobox_get(data->masks_invert_combo)))
                        & DEVELOP_COMBINE_INV;
  if(invert)
    data->module->blend_params->mask_combine |= DEVELOP_COMBINE_INV;
  else
    data->module->blend_params->mask_combine &= ~DEVELOP_COMBINE_INV;
  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
}

static void _blendop_opacity_callback(GtkWidget *slider, dt_iop_gui_blend_data_t *data)
{
  data->module->blend_params->opacity = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
}

static void _blendop_blendif_feathering_radius_callback(GtkWidget *slider, dt_iop_gui_blend_data_t *data)
{
  data->module->blend_params->feathering_radius = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
}

static void _blendop_masks_feathering_guide_callback(GtkWidget *combo, dt_iop_gui_blend_data_t *data)
{
  data->module->blend_params->feathering_guide = GPOINTER_TO_UINT(
      g_list_nth_data(data->masks_feathering_guide, dt_bauhaus_combobox_get(data->masks_feathering_guide_combo)));
  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
}

static void _blendop_blendif_blur_radius_callback(GtkWidget *slider, dt_iop_gui_blend_data_t *data)
{
  data->module->blend_params->blur_radius = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
}

static void _blendop_blendif_brightness_callback(GtkWidget *slider, dt_iop_gui_blend_data_t *data)
{
  data->module->blend_params->brightness = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
}

static void _blendop_blendif_contrast_callback(GtkWidget *slider, dt_iop_gui_blend_data_t *data)
{
  data->module->blend_params->contrast = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
}

static void _blendop_blendif_upper_callback(GtkDarktableGradientSlider *slider, dt_iop_gui_blend_data_t *data)
{
  if(darktable.gui->reset) return;
  dt_develop_blend_params_t *bp = data->module->blend_params;

  int tab = data->tab;
  int ch = data->channels[tab][1];

  float *parameters = &(bp->blendif_parameters[4 * ch]);

  for(int k = 0; k < 4; k++) parameters[k] = dtgtk_gradient_slider_multivalue_get_value(slider, k);

  for(int k = 0; k < 4; k++)
  {
    char text[256];
    (data->scale_print[tab])(parameters[k], text, sizeof(text));
    gtk_label_set_text(data->upper_label[k], text);
  }

  /** de-activate processing of this channel if maximum span is selected */
  if(parameters[1] == 0.0f && parameters[2] == 1.0f)
    bp->blendif &= ~(1 << ch);
  else
    bp->blendif |= (1 << ch);

  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
}


static void _blendop_blendif_lower_callback(GtkDarktableGradientSlider *slider, dt_iop_gui_blend_data_t *data)
{
  if(darktable.gui->reset) return;
  dt_develop_blend_params_t *bp = data->module->blend_params;

  int tab = data->tab;
  int ch = data->channels[tab][0];

  float *parameters = &(bp->blendif_parameters[4 * ch]);

  for(int k = 0; k < 4; k++) parameters[k] = dtgtk_gradient_slider_multivalue_get_value(slider, k);

  for(int k = 0; k < 4; k++)
  {
    char text[256];
    (data->scale_print[tab])(parameters[k], text, sizeof(text));
    gtk_label_set_text(data->lower_label[k], text);
  }

  /** de-activate processing of this channel if maximum span is selected */
  if(parameters[1] == 0.0f && parameters[2] == 1.0f)
    bp->blendif &= ~(1 << ch);
  else
    bp->blendif |= (1 << ch);

  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
}


static void _blendop_blendif_polarity_callback(GtkToggleButton *togglebutton, dt_iop_gui_blend_data_t *data)
{
  int active = gtk_toggle_button_get_active(togglebutton);
  if(darktable.gui->reset) return;
  dt_develop_blend_params_t *bp = data->module->blend_params;

  int tab = data->tab;
  int ch = GTK_WIDGET(togglebutton) == data->lower_polarity ? data->channels[tab][0] : data->channels[tab][1];
  GtkDarktableGradientSlider *slider = GTK_WIDGET(togglebutton) == data->lower_polarity ? data->lower_slider
                                                                                        : data->upper_slider;

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
}


static void _blendop_blendif_tab_switch(GtkNotebook *notebook, GtkWidget *page, guint page_num,
                                        dt_iop_gui_blend_data_t *data)
{
  data->tab = page_num;
  dt_iop_gui_update_blendif(data->module);
}


static void _blendop_blendif_pick_toggled(GtkToggleButton *togglebutton, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return;

  /* if module itself already requested color pick don't tamper with it. A module color picker takes
   * precedence. */
  if(module->request_color_pick == DT_REQUEST_COLORPICK_MODULE)
  {
    gtk_toggle_button_set_active(togglebutton, 0);
    return;
  }

  module->request_color_pick
      = (gtk_toggle_button_get_active(togglebutton) ? DT_REQUEST_COLORPICK_BLEND : DT_REQUEST_COLORPICK_OFF);

  /* set the area sample size */
  if(module->request_color_pick != DT_REQUEST_COLORPICK_OFF)
  {
    dt_lib_colorpicker_set_point(darktable.lib, 0.5, 0.5);
    dt_dev_reprocess_all(module->dev);
  }
  else
    dt_control_queue_redraw();

  if(module->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), 1);
  dt_iop_request_focus(module);
}

static void _blendop_blendif_showmask_clicked(GtkWidget *button, GdkEventButton *event, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return;

  // if blendif is bypassed don't allow to set this button on
  if(module->bypass_blendif)
  {
    dt_control_log(_("display mask is currently disabled by another module"));

    if(darktable.gui->reset) return;
    const int reset = darktable.gui->reset;
    darktable.gui->reset = 1;
    dtgtk_button_set_active(DTGTK_BUTTON(button), FALSE);
    darktable.gui->reset = reset;
    return;
  }

  if(event->button == 1)
  {
    const int has_mask_display = module->request_mask_display & (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL);

    module->request_mask_display &= ~(DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL | DT_DEV_PIXELPIPE_DISPLAY_ANY);

    GdkModifierType modifiers = gtk_accelerator_get_default_mod_mask();
    if((event->state & modifiers) == (GDK_CONTROL_MASK | GDK_SHIFT_MASK))
      module->request_mask_display |= (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL);
    else if((event->state & modifiers) == GDK_SHIFT_MASK)
      module->request_mask_display |= DT_DEV_PIXELPIPE_DISPLAY_CHANNEL;
    else if((event->state & modifiers) == GDK_CONTROL_MASK)
      module->request_mask_display |= DT_DEV_PIXELPIPE_DISPLAY_MASK;
    else
      module->request_mask_display |= (has_mask_display ? 0 : DT_DEV_PIXELPIPE_DISPLAY_MASK);

    if(module->request_mask_display & (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL))
      dtgtk_button_set_active(DTGTK_BUTTON(button), 1);
    else
      dtgtk_button_set_active(DTGTK_BUTTON(button), 0);


    if(module->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), 1);

    dt_iop_request_focus(module);
    dt_dev_reprocess_all(module->dev);
  }
}

static void _blendop_blendif_suppress_toggled(GtkToggleButton *togglebutton, dt_iop_module_t *module)
{
  module->suppress_mask = gtk_toggle_button_get_active(togglebutton);
  if(darktable.gui->reset) return;

  if(module->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), 1);
  dt_iop_request_focus(module);

  dt_dev_reprocess_all(module->dev);
}

static void _blendop_blendif_reset(GtkButton *button, dt_iop_module_t *module)
{
  module->blend_params->blendif = module->default_blendop_params->blendif;
  memcpy(module->blend_params->blendif_parameters, module->default_blendop_params->blendif_parameters,
         4 * DEVELOP_BLENDIF_SIZE * sizeof(float));

  dt_iop_gui_update_blendif(module);
  dt_dev_add_history_item(darktable.develop, module, TRUE);
}

static void _blendop_blendif_invert(GtkButton *button, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return;

  dt_iop_gui_blend_data_t *data = module->blend_data;

  unsigned int toggle_mask = 0;

  switch(data->csp)
  {
    case iop_cs_Lab:
      toggle_mask = DEVELOP_BLENDIF_Lab_MASK << 16;
      break;

    case iop_cs_rgb:
      toggle_mask = DEVELOP_BLENDIF_RGB_MASK << 16;
      break;

    case iop_cs_RAW:
      toggle_mask = 0;
      break;
  }

  module->blend_params->blendif ^= toggle_mask;
  module->blend_params->mask_combine ^= DEVELOP_COMBINE_MASKS_POS;
  module->blend_params->mask_combine ^= DEVELOP_COMBINE_INCL;
  dt_iop_gui_update_blending(module);
  dt_dev_add_history_item(darktable.develop, module, TRUE);
}

static int _blendop_masks_add_path(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return FALSE;
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)self->blend_data;

  if(event->button == 1)
  {
    // we want to be sure that the iop has focus
    dt_iop_request_focus(self);
    self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
    bd->masks_shown = DT_MASKS_EDIT_FULL;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), TRUE);
    // we create the new form
    dt_masks_form_t *form = dt_masks_create(DT_MASKS_PATH);
    dt_masks_change_form_gui(form);
    darktable.develop->form_gui->creation = TRUE;
    darktable.develop->form_gui->creation_module = self;
    dt_control_queue_redraw_center();
    return TRUE;
  }

  return FALSE;
}

static int _blendop_masks_add_circle(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return FALSE;
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)self->blend_data;

  if(event->button == 1)
  {
    // we want to be sure that the iop has focus
    dt_iop_request_focus(self);
    self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
    bd->masks_shown = DT_MASKS_EDIT_FULL;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), TRUE);
    // we create the new form
    dt_masks_form_t *spot = dt_masks_create(DT_MASKS_CIRCLE);
    dt_masks_change_form_gui(spot);
    darktable.develop->form_gui->creation = TRUE;
    darktable.develop->form_gui->creation_module = self;
    dt_control_queue_redraw_center();
    return TRUE;
  }

  return FALSE;
}

static int _blendop_masks_add_ellipse(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return FALSE;
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)self->blend_data;

  if(event->button == 1)
  {
    // we want to be sure that the iop has focus
    dt_iop_request_focus(self);
    self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
    bd->masks_shown = DT_MASKS_EDIT_FULL;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), TRUE);
    // we create the new form
    dt_masks_form_t *spot = dt_masks_create(DT_MASKS_ELLIPSE);
    dt_masks_change_form_gui(spot);
    darktable.develop->form_gui->creation = TRUE;
    darktable.develop->form_gui->creation_module = self;
    dt_control_queue_redraw_center();
    return TRUE;
  }

  return FALSE;
}

static int _blendop_masks_add_brush(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return FALSE;
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)self->blend_data;

  if(event->button == 1)
  {
    // we want to be sure that the iop has focus
    dt_iop_request_focus(self);
    self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
    bd->masks_shown = DT_MASKS_EDIT_FULL;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), TRUE);
    // we create the new form
    dt_masks_form_t *form = dt_masks_create(DT_MASKS_BRUSH);
    dt_masks_change_form_gui(form);
    darktable.develop->form_gui->creation = TRUE;
    darktable.develop->form_gui->creation_module = self;
    dt_control_queue_redraw_center();
    return TRUE;
  }

  return FALSE;
}

static int _blendop_masks_add_gradient(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return FALSE;
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)self->blend_data;

  if(event->button == 1)
  {
    // we want to be sure that the iop has focus
    dt_iop_request_focus(self);
    self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
    bd->masks_shown = DT_MASKS_EDIT_FULL;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), TRUE);
    // we create the new form
    dt_masks_form_t *spot = dt_masks_create(DT_MASKS_GRADIENT);
    dt_masks_change_form_gui(spot);
    darktable.develop->form_gui->creation = TRUE;
    darktable.develop->form_gui->creation_module = self;
    dt_control_queue_redraw_center();
    return TRUE;
  }

  return FALSE;
}


static int _blendop_masks_show_and_edit(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return FALSE;
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)self->blend_data;

  if(event->button == 1)
  {
    darktable.gui->reset = 1;

    dt_iop_request_focus(self);
    self->request_color_pick = DT_REQUEST_COLORPICK_OFF;

    dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, self->blend_params->mask_id);
    if(grp && (grp->type & DT_MASKS_GROUP) && g_list_length(grp->points) > 0)
    {
      const int control_button_pressed = event->state & GDK_CONTROL_MASK;

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
      bd->masks_shown = DT_MASKS_EDIT_OFF;

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), bd->masks_shown != DT_MASKS_EDIT_OFF);
    dt_masks_set_edit_mode(self, bd->masks_shown);

    darktable.gui->reset = 0;

    return TRUE;
  }

  return FALSE;
}

static void _blendop_masks_polarity_callback(GtkToggleButton *togglebutton, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  int active = gtk_toggle_button_get_active(togglebutton);
  dt_develop_blend_params_t *bp = (dt_develop_blend_params_t *)self->blend_params;

  if(active)
    bp->mask_combine |= DEVELOP_COMBINE_MASKS_POS;
  else
    bp->mask_combine &= ~DEVELOP_COMBINE_MASKS_POS;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static gboolean _blendop_blendif_draw(GtkWidget *widget, cairo_t *cr, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return FALSE;

  dt_iop_gui_blend_data_t *data = module->blend_data;

  float picker_mean[8], picker_min[8], picker_max[8];
  float cooked[8];
  float *raw_mean, *raw_min, *raw_max;
  char text[256];
  GtkLabel *label;

  if(widget == GTK_WIDGET(data->lower_slider))
  {
    raw_mean = module->picked_color;
    raw_min = module->picked_color_min;
    raw_max = module->picked_color_max;
    label = data->lower_picker_label;
  }
  else
  {
    raw_mean = module->picked_output_color;
    raw_min = module->picked_output_color_min;
    raw_max = module->picked_output_color_max;
    label = data->upper_picker_label;
  }

  darktable.gui->reset = 1;
  if((module->request_color_pick == DT_REQUEST_COLORPICK_BLEND) && (raw_min[0] != INFINITY))
  {
    _blendif_scale(data->csp, raw_mean, picker_mean);
    _blendif_scale(data->csp, raw_min, picker_min);
    _blendif_scale(data->csp, raw_max, picker_max);
    _blendif_cook(data->csp, raw_mean, cooked);

    if(data->channels[data->tab][0] >= 8) // min and max make no sense for HSL and LCh
      picker_min[data->tab] = picker_max[data->tab] = picker_mean[data->tab];

    snprintf(text, sizeof(text), "(%.1f)", cooked[data->tab]);

    dtgtk_gradient_slider_multivalue_set_picker_meanminmax(
        DTGTK_GRADIENT_SLIDER(widget), picker_mean[data->tab], picker_min[data->tab], picker_max[data->tab]);
    gtk_label_set_text(label, text);
  }
  else
  {
    dtgtk_gradient_slider_multivalue_set_picker(DTGTK_GRADIENT_SLIDER(widget), NAN);
    gtk_label_set_text(label, "");
  }

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->colorpicker),
                               (module->request_color_pick == DT_REQUEST_COLORPICK_BLEND ? 1 : 0));

  darktable.gui->reset = 0;

  return FALSE;
}

// magic mode: if mouse curser enters a gradient slider with shift and/or control pressed we
// enter channel display and/or mask display mode
static gboolean _blendop_blendif_enter(GtkWidget *widget, GdkEventCrossing *event, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return FALSE;
  dt_iop_gui_blend_data_t *data = module->blend_data;

  dt_pthread_mutex_lock(&data->lock);
  if(data->timeout_handle)
  {
    // purge any remaining timeout handlers
    g_source_remove(data->timeout_handle);
    data->timeout_handle = 0;
  }
  else
  {
    // save request_mask_display to restore later
    data->save_for_leave = module->request_mask_display;
  }
  dt_pthread_mutex_unlock(&data->lock);

  dt_dev_pixelpipe_display_mask_t new_request_mask_display = module->request_mask_display;

  // depending on shift modifiers we activate channel and/or mask display
  GdkModifierType modifiers = gtk_accelerator_get_default_mod_mask();
  if((event->state & modifiers) == (GDK_SHIFT_MASK | GDK_CONTROL_MASK))
  {
    new_request_mask_display |= (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL);
  }
  else if((event->state & modifiers) == GDK_SHIFT_MASK)
  {
    new_request_mask_display |= DT_DEV_PIXELPIPE_DISPLAY_CHANNEL;
  }
  else if((event->state & modifiers) == GDK_CONTROL_MASK)
  {
    new_request_mask_display |= DT_DEV_PIXELPIPE_DISPLAY_MASK;
  }

  // in case user requests channel display: get the cannel
  if(new_request_mask_display & DT_DEV_PIXELPIPE_DISPLAY_CHANNEL)
  {
    int tab = data->tab;
    int inout = (widget == GTK_WIDGET(data->lower_slider)) ? 0 : 1;
    dt_dev_pixelpipe_display_mask_t channel = data->display_channel[tab][inout];

    new_request_mask_display &= ~DT_DEV_PIXELPIPE_DISPLAY_ANY;
    new_request_mask_display |= channel;
  }

  // only if something has changed: reprocess center view
  if(new_request_mask_display != module->request_mask_display)
  {
    module->request_mask_display = new_request_mask_display;
    dt_dev_reprocess_all(module->dev);
  }

  return TRUE;
}

// handler for delayed mask/channel display mode switch-off
static gboolean _blendop_blendif_leave_delayed(gpointer data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)data;
  dt_iop_gui_blend_data_t *bd = module->blend_data;

  dt_pthread_mutex_lock(&bd->lock);
  // restore saved request_mask_display and reprocess image
  if(bd->timeout_handle && (module->request_mask_display != bd->save_for_leave))
  {
    module->request_mask_display = bd->save_for_leave;
    dt_dev_reprocess_all(module->dev);
  }

  bd->timeout_handle = 0;
  dt_pthread_mutex_unlock(&bd->lock);

  // return FALSE and thereby terminate the handler
  return FALSE;
}

// de-activate magic mode when leaving the gradient slider
static gboolean _blendop_blendif_leave(GtkWidget *widget, GdkEventCrossing *event, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return FALSE;
  dt_iop_gui_blend_data_t *data = module->blend_data;

  if(module->request_mask_display & (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL))
  {
    // do not immediately switch-off mask/channel display in case user leaves gradient only briefly.
    // instead we activate a handler function that gets triggered after some timeout
    dt_pthread_mutex_lock(&data->lock);
    if(!data->timeout_handle && (module->request_mask_display != data->save_for_leave))
      data->timeout_handle = g_timeout_add(1000, _blendop_blendif_leave_delayed, module);
    dt_pthread_mutex_unlock(&data->lock);
  }

  return TRUE;
}


void dt_iop_gui_update_blendif(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *data = module->blend_data;
  dt_develop_blend_params_t *bp = module->blend_params;
  dt_develop_blend_params_t *dp = module->default_blendop_params;

  if(!data || !data->blendif_support || !data->blendif_inited) return;

  dt_pthread_mutex_lock(&data->lock);
  if(data->timeout_handle)
  {
    g_source_remove(data->timeout_handle);
    data->timeout_handle = 0;
    if(module->request_mask_display != data->save_for_leave)
    {
      module->request_mask_display = data->save_for_leave;
      dt_dev_reprocess_all(module->dev);
    }
  }
  dt_pthread_mutex_unlock(&data->lock);

  int tab = data->tab;
  int in_ch = data->channels[tab][0];
  int out_ch = data->channels[tab][1];

  float *iparameters = &(bp->blendif_parameters[4 * in_ch]);
  float *oparameters = &(bp->blendif_parameters[4 * out_ch]);
  float *idefaults = &(dp->blendif_parameters[4 * in_ch]);
  float *odefaults = &(dp->blendif_parameters[4 * out_ch]);

  int ipolarity = !(bp->blendif & (1 << (in_ch + 16)));
  int opolarity = !(bp->blendif & (1 << (out_ch + 16)));
  char text[256];

  int reset = darktable.gui->reset;
  darktable.gui->reset = 1;

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->lower_polarity), ipolarity);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->upper_polarity), opolarity);

  dtgtk_gradient_slider_multivalue_set_marker(
      data->lower_slider,
      ipolarity ? GRADIENT_SLIDER_MARKER_LOWER_OPEN_BIG : GRADIENT_SLIDER_MARKER_UPPER_OPEN_BIG, 0);
  dtgtk_gradient_slider_multivalue_set_marker(
      data->lower_slider,
      ipolarity ? GRADIENT_SLIDER_MARKER_UPPER_FILLED_BIG : GRADIENT_SLIDER_MARKER_LOWER_FILLED_BIG, 1);
  dtgtk_gradient_slider_multivalue_set_marker(
      data->lower_slider,
      ipolarity ? GRADIENT_SLIDER_MARKER_UPPER_FILLED_BIG : GRADIENT_SLIDER_MARKER_LOWER_FILLED_BIG, 2);
  dtgtk_gradient_slider_multivalue_set_marker(
      data->lower_slider,
      ipolarity ? GRADIENT_SLIDER_MARKER_LOWER_OPEN_BIG : GRADIENT_SLIDER_MARKER_UPPER_OPEN_BIG, 3);

  dtgtk_gradient_slider_multivalue_set_marker(
      data->upper_slider,
      opolarity ? GRADIENT_SLIDER_MARKER_LOWER_OPEN_BIG : GRADIENT_SLIDER_MARKER_UPPER_OPEN_BIG, 0);
  dtgtk_gradient_slider_multivalue_set_marker(
      data->upper_slider,
      opolarity ? GRADIENT_SLIDER_MARKER_UPPER_FILLED_BIG : GRADIENT_SLIDER_MARKER_LOWER_FILLED_BIG, 1);
  dtgtk_gradient_slider_multivalue_set_marker(
      data->upper_slider,
      opolarity ? GRADIENT_SLIDER_MARKER_UPPER_FILLED_BIG : GRADIENT_SLIDER_MARKER_LOWER_FILLED_BIG, 2);
  dtgtk_gradient_slider_multivalue_set_marker(
      data->upper_slider,
      opolarity ? GRADIENT_SLIDER_MARKER_LOWER_OPEN_BIG : GRADIENT_SLIDER_MARKER_UPPER_OPEN_BIG, 3);

  for(int k = 0; k < 4; k++)
  {
    dtgtk_gradient_slider_multivalue_set_value(data->lower_slider, iparameters[k], k);
    dtgtk_gradient_slider_multivalue_set_value(data->upper_slider, oparameters[k], k);
    dtgtk_gradient_slider_multivalue_set_resetvalue(data->lower_slider, idefaults[k], k);
    dtgtk_gradient_slider_multivalue_set_resetvalue(data->upper_slider, odefaults[k], k);
  }

  for(int k = 0; k < 4; k++)
  {
    (data->scale_print[tab])(iparameters[k], text, sizeof(text));
    gtk_label_set_text(data->lower_label[k], text);
    (data->scale_print[tab])(oparameters[k], text, sizeof(text));
    gtk_label_set_text(data->upper_label[k], text);
  }

  dtgtk_gradient_slider_multivalue_clear_stops(data->lower_slider);
  dtgtk_gradient_slider_multivalue_clear_stops(data->upper_slider);

  for(int k = 0; k < data->numberstops[tab]; k++)
  {
    dtgtk_gradient_slider_multivalue_set_stop(data->lower_slider, (data->colorstops[tab])[k].stoppoint,
                                              (data->colorstops[tab])[k].color);
    dtgtk_gradient_slider_multivalue_set_stop(data->upper_slider, (data->colorstops[tab])[k].stoppoint,
                                              (data->colorstops[tab])[k].color);
  }

  dtgtk_gradient_slider_multivalue_set_increment(data->lower_slider, data->increments[tab]);
  dtgtk_gradient_slider_multivalue_set_increment(data->upper_slider, data->increments[tab]);

  darktable.gui->reset = reset;
}


void dt_iop_gui_init_blendif(GtkBox *blendw, dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

  bd->blendif_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE));
  // add event box so that one can click into the area to get help for parametric masks
  GtkWidget* event_box = gtk_event_box_new();
  dt_gui_add_help_link(GTK_WIDGET(event_box), "parametric_mask.html");
  gtk_container_add(GTK_CONTAINER(blendw), event_box);

  /* create and add blendif support if module supports it */
  if(bd->blendif_support)
  {
    char *Lab_labels[] = { "  L  ", "  a  ", "  b  ", " C ", " h " };
    char *Lab_tooltips[]
        = { _("sliders for L channel"), _("sliders for a channel"), _("sliders for b channel"),
            _("sliders for chroma channel (of LCh)"), _("sliders for hue channel (of LCh)") };
    char *rgb_labels[] = { _(" g "), _(" R "), _(" G "), _(" B "), _(" H "), _(" S "), _(" L ") };
    char *rgb_tooltips[]
        = { _("sliders for gray value"), _("sliders for red channel"), _("sliders for green channel"),
            _("sliders for blue channel"), _("sliders for hue channel (of HSL)"),
            _("sliders for chroma channel (of HSL)"), _("sliders for value channel (of HSL)") };

    char *ttinput = _("adjustment based on input received by this module:\n* range defined by upper markers: "
                      "blend fully\n* range defined by lower markers: do not blend at all\n* range between "
                      "adjacent upper/lower markers: blend gradually");

    char *ttoutput = _("adjustment based on unblended output of this module:\n* range defined by upper "
                       "markers: blend fully\n* range defined by lower markers: do not blend at all\n* range "
                       "between adjacent upper/lower markers: blend gradually");

    bd->tab = 0;
    const int bs = DT_PIXEL_APPLY_DPI(14);

    int maxchannels = 0;
    char **labels = NULL;
    char **tooltips = NULL;

    switch(bd->csp)
    {
      case iop_cs_Lab:
        maxchannels = 5;
        labels = Lab_labels;
        tooltips = Lab_tooltips;
        bd->scale_print[0] = _blendif_scale_print_L;
        bd->scale_print[1] = _blendif_scale_print_ab;
        bd->scale_print[2] = _blendif_scale_print_ab;
        bd->scale_print[3] = _blendif_scale_print_default;
        bd->scale_print[4] = _blendif_scale_print_hue;
        bd->increments[0] = 1.0f / 100.0f;
        bd->increments[1] = 1.0f / 256.0f;
        bd->increments[2] = 1.0f / 256.0f;
        bd->increments[3] = 1.0f / 100.0f;
        bd->increments[4] = 1.0f / 360.0f;
        bd->channels[0][0] = DEVELOP_BLENDIF_L_in;
        bd->channels[0][1] = DEVELOP_BLENDIF_L_out;
        bd->channels[1][0] = DEVELOP_BLENDIF_A_in;
        bd->channels[1][1] = DEVELOP_BLENDIF_A_out;
        bd->channels[2][0] = DEVELOP_BLENDIF_B_in;
        bd->channels[2][1] = DEVELOP_BLENDIF_B_out;
        bd->channels[3][0] = DEVELOP_BLENDIF_C_in;
        bd->channels[3][1] = DEVELOP_BLENDIF_C_out;
        bd->channels[4][0] = DEVELOP_BLENDIF_h_in;
        bd->channels[4][1] = DEVELOP_BLENDIF_h_out;
        bd->display_channel[0][0] = DT_DEV_PIXELPIPE_DISPLAY_L;
        bd->display_channel[0][1] = DT_DEV_PIXELPIPE_DISPLAY_L | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT;
        bd->display_channel[1][0] = DT_DEV_PIXELPIPE_DISPLAY_a;
        bd->display_channel[1][1] = DT_DEV_PIXELPIPE_DISPLAY_a | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT;
        bd->display_channel[2][0] = DT_DEV_PIXELPIPE_DISPLAY_b;
        bd->display_channel[2][1] = DT_DEV_PIXELPIPE_DISPLAY_b | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT;
        bd->display_channel[3][0] = DT_DEV_PIXELPIPE_DISPLAY_LCH_C;
        bd->display_channel[3][1] = DT_DEV_PIXELPIPE_DISPLAY_LCH_C | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT;
        bd->display_channel[4][0] = DT_DEV_PIXELPIPE_DISPLAY_LCH_h;
        bd->display_channel[4][1] = DT_DEV_PIXELPIPE_DISPLAY_LCH_h | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT;
        bd->colorstops[0] = _gradient_L;
        bd->numberstops[0] = sizeof(_gradient_L) / sizeof(dt_iop_gui_blendif_colorstop_t);
        bd->colorstops[1] = _gradient_a;
        bd->numberstops[1] = sizeof(_gradient_a) / sizeof(dt_iop_gui_blendif_colorstop_t);
        bd->colorstops[2] = _gradient_b;
        bd->numberstops[2] = sizeof(_gradient_b) / sizeof(dt_iop_gui_blendif_colorstop_t);
        bd->colorstops[3] = _gradient_chroma;
        bd->numberstops[3] = sizeof(_gradient_chroma) / sizeof(dt_iop_gui_blendif_colorstop_t);
        bd->colorstops[4] = _gradient_hue;
        bd->numberstops[4] = sizeof(_gradient_hue) / sizeof(dt_iop_gui_blendif_colorstop_t);
        break;
      case iop_cs_rgb:
        maxchannels = 7;
        labels = rgb_labels;
        tooltips = rgb_tooltips;
        bd->scale_print[0] = _blendif_scale_print_rgb;
        bd->scale_print[1] = _blendif_scale_print_rgb;
        bd->scale_print[2] = _blendif_scale_print_rgb;
        bd->scale_print[3] = _blendif_scale_print_rgb;
        bd->scale_print[4] = _blendif_scale_print_hue;
        bd->scale_print[5] = _blendif_scale_print_default;
        bd->scale_print[6] = _blendif_scale_print_L;
        bd->increments[0] = 1.0f / 255.0f;
        bd->increments[1] = 1.0f / 255.0f;
        bd->increments[2] = 1.0f / 255.0f;
        bd->increments[3] = 1.0f / 255.0f;
        bd->increments[4] = 1.0f / 360.0f;
        bd->increments[5] = 1.0f / 100.0f;
        bd->increments[6] = 1.0f / 100.0f;
        bd->channels[0][0] = DEVELOP_BLENDIF_GRAY_in;
        bd->channels[0][1] = DEVELOP_BLENDIF_GRAY_out;
        bd->channels[1][0] = DEVELOP_BLENDIF_RED_in;
        bd->channels[1][1] = DEVELOP_BLENDIF_RED_out;
        bd->channels[2][0] = DEVELOP_BLENDIF_GREEN_in;
        bd->channels[2][1] = DEVELOP_BLENDIF_GREEN_out;
        bd->channels[3][0] = DEVELOP_BLENDIF_BLUE_in;
        bd->channels[3][1] = DEVELOP_BLENDIF_BLUE_out;
        bd->channels[4][0] = DEVELOP_BLENDIF_H_in;
        bd->channels[4][1] = DEVELOP_BLENDIF_H_out;
        bd->channels[5][0] = DEVELOP_BLENDIF_S_in;
        bd->channels[5][1] = DEVELOP_BLENDIF_S_out;
        bd->channels[6][0] = DEVELOP_BLENDIF_l_in;
        bd->channels[6][1] = DEVELOP_BLENDIF_l_out;
        bd->display_channel[0][0] = DT_DEV_PIXELPIPE_DISPLAY_GRAY;
        bd->display_channel[0][1] = DT_DEV_PIXELPIPE_DISPLAY_GRAY | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT;
        bd->display_channel[1][0] = DT_DEV_PIXELPIPE_DISPLAY_R;
        bd->display_channel[1][1] = DT_DEV_PIXELPIPE_DISPLAY_R | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT;
        bd->display_channel[2][0] = DT_DEV_PIXELPIPE_DISPLAY_G;
        bd->display_channel[2][1] = DT_DEV_PIXELPIPE_DISPLAY_G | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT;
        bd->display_channel[3][0] = DT_DEV_PIXELPIPE_DISPLAY_B;
        bd->display_channel[3][1] = DT_DEV_PIXELPIPE_DISPLAY_B | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT;
        bd->display_channel[4][0] = DT_DEV_PIXELPIPE_DISPLAY_HSL_H;
        bd->display_channel[4][1] = DT_DEV_PIXELPIPE_DISPLAY_HSL_H | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT;
        bd->display_channel[5][0] = DT_DEV_PIXELPIPE_DISPLAY_HSL_S;
        bd->display_channel[5][1] = DT_DEV_PIXELPIPE_DISPLAY_HSL_S | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT;
        bd->display_channel[6][0] = DT_DEV_PIXELPIPE_DISPLAY_HSL_l;
        bd->display_channel[6][1] = DT_DEV_PIXELPIPE_DISPLAY_HSL_l | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT;
        bd->colorstops[0] = _gradient_gray;
        bd->numberstops[0] = sizeof(_gradient_gray) / sizeof(dt_iop_gui_blendif_colorstop_t);
        bd->colorstops[1] = _gradient_red;
        bd->numberstops[1] = sizeof(_gradient_red) / sizeof(dt_iop_gui_blendif_colorstop_t);
        bd->colorstops[2] = _gradient_green;
        bd->numberstops[2] = sizeof(_gradient_green) / sizeof(dt_iop_gui_blendif_colorstop_t);
        bd->colorstops[3] = _gradient_blue;
        bd->numberstops[3] = sizeof(_gradient_blue) / sizeof(dt_iop_gui_blendif_colorstop_t);
        bd->colorstops[4] = _gradient_HUE;
        bd->numberstops[4] = sizeof(_gradient_HUE) / sizeof(dt_iop_gui_blendif_colorstop_t);
        bd->colorstops[5] = _gradient_chroma;
        bd->numberstops[5] = sizeof(_gradient_chroma) / sizeof(dt_iop_gui_blendif_colorstop_t);
        bd->colorstops[6] = _gradient_gray;
        bd->numberstops[6] = sizeof(_gradient_gray) / sizeof(dt_iop_gui_blendif_colorstop_t);
        break;
      default:
        assert(FALSE); // blendif not supported for RAW, which is already caught upstream; we should not get
                       // here
    }

    GtkWidget *uplabel = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *lowlabel = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *upslider = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(5));
    GtkWidget *lowslider = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(5));
    GtkWidget *notebook = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(5));

    bd->channel_tabs = GTK_NOTEBOOK(gtk_notebook_new());

    for(int ch = 0; ch < maxchannels; ch++)
    {
      gtk_notebook_append_page(GTK_NOTEBOOK(bd->channel_tabs),
                               GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0)),
                               gtk_label_new(labels[ch]));
      gtk_widget_set_tooltip_text(gtk_notebook_get_tab_label(bd->channel_tabs,
                                                             gtk_notebook_get_nth_page(bd->channel_tabs, -1)),
                                  tooltips[ch]);
    }

    gtk_widget_show_all(GTK_WIDGET(gtk_notebook_get_nth_page(bd->channel_tabs, bd->tab)));
    gtk_notebook_set_current_page(GTK_NOTEBOOK(bd->channel_tabs), bd->tab);
    gtk_notebook_set_scrollable(bd->channel_tabs, TRUE);

    gtk_box_pack_start(GTK_BOX(notebook), GTK_WIDGET(bd->channel_tabs), FALSE, FALSE, 0);

    bd->colorpicker
        = dtgtk_togglebutton_new(dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
    gtk_widget_set_tooltip_text(bd->colorpicker, _("pick GUI color from image"));
    gtk_widget_set_size_request(GTK_WIDGET(bd->colorpicker), bs, bs);

    GtkWidget *res = dtgtk_button_new(dtgtk_cairo_paint_reset, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
    gtk_widget_set_tooltip_text(res, _("reset blend mask settings"));
    gtk_widget_set_size_request(res, bs, bs);

    GtkWidget *inv = dtgtk_button_new(dtgtk_cairo_paint_invert, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
    gtk_widget_set_tooltip_text(inv, _("invert all channel's polarities"));
    gtk_widget_set_size_request(inv, bs, bs);

    gtk_box_pack_start(GTK_BOX(header), GTK_WIDGET(notebook), TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(header), GTK_WIDGET(res), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(header), GTK_WIDGET(inv), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(header), GTK_WIDGET(bd->colorpicker), FALSE, FALSE, 0);

    bd->lower_slider = DTGTK_GRADIENT_SLIDER_MULTIVALUE(dtgtk_gradient_slider_multivalue_new(4));
    bd->upper_slider = DTGTK_GRADIENT_SLIDER_MULTIVALUE(dtgtk_gradient_slider_multivalue_new(4));

    bd->lower_polarity
        = dtgtk_togglebutton_new(dtgtk_cairo_paint_plusminus, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
    gtk_widget_set_tooltip_text(bd->lower_polarity, _("toggle polarity. best seen by enabling 'display mask'"));

    bd->upper_polarity
        = dtgtk_togglebutton_new(dtgtk_cairo_paint_plusminus, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
    gtk_widget_set_tooltip_text(bd->upper_polarity, _("toggle polarity. best seen by enabling 'display mask'"));

    gtk_box_pack_start(GTK_BOX(upslider), GTK_WIDGET(bd->upper_slider), TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(upslider), GTK_WIDGET(bd->upper_polarity), FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(lowslider), GTK_WIDGET(bd->lower_slider), TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(lowslider), GTK_WIDGET(bd->lower_polarity), FALSE, FALSE, 0);


    GtkWidget *output = gtk_label_new(_("output"));
    bd->upper_picker_label = GTK_LABEL(gtk_label_new(""));
    gtk_box_pack_start(GTK_BOX(uplabel), GTK_WIDGET(output), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(uplabel), GTK_WIDGET(bd->upper_picker_label), TRUE, TRUE, 0);
    for(int k = 0; k < 4; k++)
    {
      bd->upper_label[k] = GTK_LABEL(gtk_label_new(NULL));
      gtk_label_set_width_chars(bd->upper_label[k], 5);
      gtk_box_pack_start(GTK_BOX(uplabel), GTK_WIDGET(bd->upper_label[k]), FALSE, FALSE, 0);
    }

    GtkWidget *input = gtk_label_new(_("input"));
    bd->lower_picker_label = GTK_LABEL(gtk_label_new(""));
    gtk_box_pack_start(GTK_BOX(lowlabel), GTK_WIDGET(input), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(lowlabel), GTK_WIDGET(bd->lower_picker_label), TRUE, TRUE, 0);
    for(int k = 0; k < 4; k++)
    {
      bd->lower_label[k] = GTK_LABEL(gtk_label_new(NULL));
      gtk_label_set_width_chars(bd->lower_label[k], 5);
      gtk_box_pack_start(GTK_BOX(lowlabel), GTK_WIDGET(bd->lower_label[k]), FALSE, FALSE, 0);
    }

    gtk_widget_set_tooltip_text(GTK_WIDGET(bd->lower_slider), _("double click to reset"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(bd->upper_slider), _("double click to reset"));
    gtk_widget_set_tooltip_text(output, ttoutput);
    gtk_widget_set_tooltip_text(input, ttinput);

    g_signal_connect(G_OBJECT(bd->lower_slider), "draw", G_CALLBACK(_blendop_blendif_draw), module);

    g_signal_connect(G_OBJECT(bd->upper_slider), "draw", G_CALLBACK(_blendop_blendif_draw), module);

    g_signal_connect(G_OBJECT(bd->channel_tabs), "switch_page", G_CALLBACK(_blendop_blendif_tab_switch), bd);

    g_signal_connect(G_OBJECT(bd->upper_slider), "value-changed", G_CALLBACK(_blendop_blendif_upper_callback),
                     bd);

    g_signal_connect(G_OBJECT(bd->lower_slider), "value-changed", G_CALLBACK(_blendop_blendif_lower_callback),
                     bd);

    g_signal_connect(G_OBJECT(bd->lower_slider), "leave-notify-event", G_CALLBACK(_blendop_blendif_leave), module);

    g_signal_connect(G_OBJECT(bd->upper_slider), "leave-notify-event", G_CALLBACK(_blendop_blendif_leave), module);

    g_signal_connect(G_OBJECT(bd->lower_slider), "enter-notify-event", G_CALLBACK(_blendop_blendif_enter), module);

    g_signal_connect(G_OBJECT(bd->upper_slider), "enter-notify-event", G_CALLBACK(_blendop_blendif_enter), module);

    g_signal_connect(G_OBJECT(bd->colorpicker), "toggled", G_CALLBACK(_blendop_blendif_pick_toggled), module);

    g_signal_connect(G_OBJECT(res), "clicked", G_CALLBACK(_blendop_blendif_reset), module);

    g_signal_connect(G_OBJECT(inv), "clicked", G_CALLBACK(_blendop_blendif_invert), module);

    g_signal_connect(G_OBJECT(bd->lower_polarity), "toggled", G_CALLBACK(_blendop_blendif_polarity_callback),
                     bd);

    g_signal_connect(G_OBJECT(bd->upper_polarity), "toggled", G_CALLBACK(_blendop_blendif_polarity_callback),
                     bd);

    gtk_box_pack_start(GTK_BOX(bd->blendif_box), dt_ui_section_label_new(_("parametric mask")), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->blendif_box), GTK_WIDGET(header), TRUE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bd->blendif_box), GTK_WIDGET(uplabel), TRUE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bd->blendif_box), GTK_WIDGET(upslider), TRUE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bd->blendif_box), GTK_WIDGET(lowlabel), TRUE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bd->blendif_box), GTK_WIDGET(lowslider), TRUE, FALSE, 0);

    bd->blendif_inited = 1;
  }

  gtk_container_add(GTK_CONTAINER(event_box), GTK_WIDGET(bd->blendif_box));
}

void dt_iop_gui_update_masks(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  dt_develop_blend_params_t *bp = module->blend_params;

  if(!bd || !bd->masks_support || !bd->masks_inited) return;

  /* update masks state */
  dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, module->blend_params->mask_id);
  dt_bauhaus_combobox_clear(bd->masks_combo);
  if(grp && (grp->type & DT_MASKS_GROUP) && g_list_length(grp->points) > 0)
  {
    char txt[512];
    guint n = g_list_length(grp->points);
    snprintf(txt, sizeof(txt), ngettext("%d shape used", "%d shapes used", n), n);
    dt_bauhaus_combobox_add(bd->masks_combo, txt);
  }
  else
  {
    dt_bauhaus_combobox_add(bd->masks_combo, _("no mask used"));
    bd->masks_shown = DT_MASKS_EDIT_OFF;
    // reset the gui
    dt_masks_set_edit_mode(module, DT_MASKS_EDIT_OFF);
  }
  dt_bauhaus_combobox_set(bd->masks_combo, 0);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), bd->masks_shown != DT_MASKS_EDIT_OFF);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_polarity),
                               bp->mask_combine & DEVELOP_COMBINE_MASKS_POS);

  // update buttons status
  int b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0;
  if(module->dev->form_gui && module->dev->form_visible && module->dev->form_gui->creation
     && module->dev->form_gui->creation_module == module)
  {
    if(module->dev->form_visible->type & DT_MASKS_CIRCLE)
      b1 = 1;
    else if(module->dev->form_visible->type & DT_MASKS_PATH)
      b2 = 1;
    else if(module->dev->form_visible->type & DT_MASKS_GRADIENT)
      b3 = 1;
    else if(module->dev->form_visible->type & DT_MASKS_ELLIPSE)
      b4 = 1;
    else if(module->dev->form_visible->type & DT_MASKS_BRUSH)
      b5 = 1;
  }
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_circle), b1);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_path), b2);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_gradient), b3);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_ellipse), b4);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_brush), b5);
}

void dt_iop_gui_init_masks(GtkBox *blendw, dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

  bd->masks_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE));
  // add event box so that one can click into the area to get help for drawn masks
  GtkWidget* event_box = gtk_event_box_new();
  dt_gui_add_help_link(GTK_WIDGET(event_box), "drawn_mask.html");
  gtk_container_add(GTK_CONTAINER(blendw), event_box);

  /* create and add masks support if module supports it */
  if(bd->masks_support)
  {
    const int bs = DT_PIXEL_APPLY_DPI(14);

    bd->masks_combo_ids = NULL;
    bd->masks_shown = DT_MASKS_EDIT_OFF;

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *abox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    bd->masks_combo = dt_bauhaus_combobox_new(module);
    dt_bauhaus_widget_set_label(bd->masks_combo, _("blend"), _("drawn mask"));
    dt_bauhaus_combobox_add(bd->masks_combo, _("no mask used"));
    dt_bauhaus_combobox_set(bd->masks_combo, 0);
    g_signal_connect(G_OBJECT(bd->masks_combo), "value-changed",
                     G_CALLBACK(dt_masks_iop_value_changed_callback), module);
    dt_bauhaus_combobox_add_populate_fct(bd->masks_combo, dt_masks_iop_combo_populate);
    gtk_box_pack_start(GTK_BOX(hbox), bd->masks_combo, TRUE, TRUE, 0);

    bd->masks_edit
        = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_eye, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
    g_signal_connect(G_OBJECT(bd->masks_edit), "button-press-event", G_CALLBACK(_blendop_masks_show_and_edit),
                     module);
    gtk_widget_set_tooltip_text(bd->masks_edit, _("show and edit mask elements"));
    gtk_widget_set_size_request(GTK_WIDGET(bd->masks_edit), bs, bs);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), FALSE);
    gtk_box_pack_start(GTK_BOX(hbox), bd->masks_edit, FALSE, FALSE, 0);

    bd->masks_polarity
        = dtgtk_togglebutton_new(dtgtk_cairo_paint_plusminus, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
    gtk_widget_set_tooltip_text(bd->masks_polarity, _("toggle polarity of drawn mask"));
    g_signal_connect(G_OBJECT(bd->masks_polarity), "toggled", G_CALLBACK(_blendop_masks_polarity_callback),
                     module);
    gtk_widget_set_size_request(GTK_WIDGET(bd->masks_polarity), bs, bs);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_polarity), FALSE);
    gtk_box_pack_start(GTK_BOX(hbox), bd->masks_polarity, FALSE, FALSE, 0);

    bd->masks_gradient
        = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_gradient, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
    g_signal_connect(G_OBJECT(bd->masks_gradient), "button-press-event",
                     G_CALLBACK(_blendop_masks_add_gradient), module);
    gtk_widget_set_tooltip_text(bd->masks_gradient, _("add gradient"));
    gtk_widget_set_size_request(GTK_WIDGET(bd->masks_gradient), bs, bs);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_gradient), FALSE);
    gtk_box_pack_end(GTK_BOX(abox), bd->masks_gradient, FALSE, FALSE, 0);

    bd->masks_path
        = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_path, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
    g_signal_connect(G_OBJECT(bd->masks_path), "button-press-event", G_CALLBACK(_blendop_masks_add_path),
                     module);
    gtk_widget_set_tooltip_text(bd->masks_path, _("add path"));
    gtk_widget_set_size_request(GTK_WIDGET(bd->masks_path), bs, bs);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_path), FALSE);
    gtk_box_pack_end(GTK_BOX(abox), bd->masks_path, FALSE, FALSE, bs);

    bd->masks_ellipse
        = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_ellipse, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
    g_signal_connect(G_OBJECT(bd->masks_ellipse), "button-press-event",
                     G_CALLBACK(_blendop_masks_add_ellipse), module);
    gtk_widget_set_tooltip_text(bd->masks_ellipse, _("add ellipse"));
    gtk_widget_set_size_request(GTK_WIDGET(bd->masks_ellipse), bs, bs);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_ellipse), FALSE);
    gtk_box_pack_end(GTK_BOX(abox), bd->masks_ellipse, FALSE, FALSE, 0);

    bd->masks_circle
        = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_circle, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
    g_signal_connect(G_OBJECT(bd->masks_circle), "button-press-event", G_CALLBACK(_blendop_masks_add_circle),
                     module);
    gtk_widget_set_tooltip_text(bd->masks_circle, _("add circle"));
    gtk_widget_set_size_request(GTK_WIDGET(bd->masks_circle), bs, bs);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_circle), FALSE);
    gtk_box_pack_end(GTK_BOX(abox), bd->masks_circle, FALSE, FALSE, bs);

    bd->masks_brush
        = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_brush, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
    g_signal_connect(G_OBJECT(bd->masks_brush), "button-press-event", G_CALLBACK(_blendop_masks_add_brush),
                     module);
    gtk_widget_set_tooltip_text(bd->masks_brush, _("add brush"));
    gtk_widget_set_size_request(GTK_WIDGET(bd->masks_brush), bs, bs);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_brush), FALSE);
    gtk_box_pack_end(GTK_BOX(abox), bd->masks_brush, FALSE, FALSE, 0);


    gtk_box_pack_start(GTK_BOX(bd->masks_box), dt_ui_section_label_new(_("drawn mask")), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->masks_box), GTK_WIDGET(hbox), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->masks_box), GTK_WIDGET(abox), TRUE, TRUE, 0);

    bd->masks_inited = 1;
  }
  gtk_container_add(GTK_CONTAINER(event_box), GTK_WIDGET(bd->masks_box));
}

void dt_iop_gui_cleanup_blending(dt_iop_module_t *module)
{
  if(!module->blend_data) return;
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

  dt_pthread_mutex_lock(&bd->lock);
  if(bd->timeout_handle)
    g_source_remove(bd->timeout_handle);

  g_list_free(bd->blend_modes);
  g_list_free(bd->masks_modes);
  g_list_free(bd->masks_combine);
  g_list_free(bd->masks_invert);
  g_list_free(bd->masks_feathering_guide);
  g_list_free_full(bd->blend_modes_all, g_free);
  free(bd->masks_combo_ids);
  dt_pthread_mutex_unlock(&bd->lock);
  dt_pthread_mutex_destroy(&bd->lock);

  memset(module->blend_data, 0, sizeof(dt_iop_gui_blend_data_t));

  g_free(module->blend_data);
  module->blend_data = NULL;
}


void dt_iop_gui_update_blending(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

  if(!(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING) || !bd || !bd->blend_inited) return;

  int reset = darktable.gui->reset;
  darktable.gui->reset = 1;

  dt_bauhaus_combobox_set(bd->masks_modes_combo,
                          g_list_index(bd->masks_modes, GUINT_TO_POINTER(module->blend_params->mask_mode)));

  /* special handling of deprecated blend modes */
  int blend_mode_number = g_list_index(bd->blend_modes, GUINT_TO_POINTER(module->blend_params->blend_mode));
  if(blend_mode_number < 0)
  {
    GList *complete_list = bd->blend_modes_all;

    while(complete_list)
    {
      dt_iop_blend_mode_t *bm = (dt_iop_blend_mode_t *)complete_list->data;
      if(bm->mode == module->blend_params->blend_mode)
      {
        dt_bauhaus_combobox_add(bd->blend_modes_combo, bm->name);
        bd->blend_modes = g_list_append(bd->blend_modes, GUINT_TO_POINTER(bm->mode));
        break;
      }
      complete_list = g_list_next(complete_list);
    }

    if(complete_list)
    {
      /* found it and added it to combobox, now find entry number */
      blend_mode_number = g_list_index(bd->blend_modes, GUINT_TO_POINTER(module->blend_params->blend_mode));
    }
    else
    {
      /* should never happen: unknown blend mode */
      dt_control_log("unknown blend mode '%d' in module '%s'", module->blend_params->blend_mode, module->op);
      blend_mode_number = 0;
    }
  }

  dt_bauhaus_combobox_set(bd->blend_modes_combo, blend_mode_number);

  dt_bauhaus_combobox_set(
      bd->masks_combine_combo,
      g_list_index(bd->masks_combine, GUINT_TO_POINTER(module->blend_params->mask_combine
                                                       & (DEVELOP_COMBINE_INV | DEVELOP_COMBINE_INCL))));
  dt_bauhaus_combobox_set(bd->masks_invert_combo,
                          g_list_index(bd->masks_invert, GUINT_TO_POINTER(module->blend_params->mask_combine
                                                                          & DEVELOP_COMBINE_INV)));
  dt_bauhaus_slider_set(bd->opacity_slider, module->blend_params->opacity);
  dt_bauhaus_combobox_set(
      bd->masks_feathering_guide_combo,
      g_list_index(bd->masks_feathering_guide, GUINT_TO_POINTER(module->blend_params->feathering_guide)));
  dt_bauhaus_slider_set(bd->feathering_radius_slider, module->blend_params->feathering_radius);
  dt_bauhaus_slider_set(bd->blur_radius_slider, module->blend_params->blur_radius);
  dt_bauhaus_slider_set(bd->brightness_slider, module->blend_params->brightness);
  dt_bauhaus_slider_set(bd->contrast_slider, module->blend_params->contrast);

  dt_iop_gui_update_blendif(module);
  dt_iop_gui_update_masks(module);

  /* now show hide controls as required */
  const unsigned int mask_mode = module->blend_params->mask_mode;

  if(mask_mode & DEVELOP_MASK_ENABLED)
  {
    gtk_widget_show(GTK_WIDGET(bd->top_box));
  }
  else
  {
    gtk_widget_hide(GTK_WIDGET(bd->top_box));
  }

  if((mask_mode & DEVELOP_MASK_ENABLED) && ((bd->masks_inited && (mask_mode & DEVELOP_MASK_MASK))
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
    if(dt_iop_module_colorspace(module) == iop_cs_RAW)
    {
      module->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
      dtgtk_button_set_active(DTGTK_BUTTON(bd->showmask), 0);
      gtk_widget_hide(GTK_WIDGET(bd->showmask));
    }
    else
    {
      gtk_widget_show(GTK_WIDGET(bd->showmask));
    }

    gtk_widget_show(GTK_WIDGET(bd->bottom_box));
  }
  else
  {
    module->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
    dtgtk_button_set_active(DTGTK_BUTTON(bd->showmask), 0);
    module->suppress_mask = 0;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->suppress), 0);

    gtk_widget_hide(GTK_WIDGET(bd->bottom_box));
  }


  if(bd->masks_inited && (mask_mode & DEVELOP_MASK_MASK))
  {
    gtk_widget_show(GTK_WIDGET(bd->masks_box));
  }
  else if(bd->masks_inited)
  {
    dt_masks_set_edit_mode(module, DT_MASKS_EDIT_OFF);

    gtk_widget_hide(GTK_WIDGET(bd->masks_box));
  }
  else
  {
    gtk_widget_hide(GTK_WIDGET(bd->masks_box));
  }


  if(bd->blendif_inited && (mask_mode & DEVELOP_MASK_CONDITIONAL))
  {
    gtk_widget_show(GTK_WIDGET(bd->blendif_box));
  }
  else if(bd->blendif_inited)
  {
    /* switch off color picker if it was requested by blendif */
    if(module->request_color_pick == DT_REQUEST_COLORPICK_BLEND)
    {
      module->request_color_pick = DT_REQUEST_COLORPICK_OFF;
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->colorpicker), 0);
    }

    gtk_widget_hide(GTK_WIDGET(bd->blendif_box));
  }
  else
  {
    gtk_widget_hide(GTK_WIDGET(bd->blendif_box));
  }

  darktable.gui->reset = reset;
}

static void _collect_blend_modes(GList **list, const char *name, unsigned int mode)
{
  dt_iop_blend_mode_t *bm;
  bm = g_malloc(sizeof(dt_iop_blend_mode_t));
  g_strlcpy(bm->name, name, sizeof(bm->name));
  bm->mode = mode;
  *list = g_list_append(*list, bm);
}


static void _add_blendmode_combo(GList **list, GtkWidget *combobox, GList *complete, unsigned int mode)
{
  GList *all = complete;

  while(all)
  {
    dt_iop_blend_mode_t *bm = (dt_iop_blend_mode_t *)all->data;
    if(bm->mode == mode)
    {
      dt_bauhaus_combobox_add(combobox, bm->name);
      *list = g_list_append(*list, GUINT_TO_POINTER(bm->mode));
      break;
    }
    all = g_list_next(all);
  }
}


void dt_iop_gui_init_blending(GtkWidget *iopw, dt_iop_module_t *module)
{
  /* create and add blend mode if module supports it */
  if(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
  {
    const int bs = DT_PIXEL_APPLY_DPI(14);

    module->blend_data = g_malloc0(sizeof(dt_iop_gui_blend_data_t));
    dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

    bd->iopw = iopw;
    bd->module = module;
    bd->csp = dt_iop_module_colorspace(module);
    bd->blendif_support = (bd->csp == iop_cs_Lab || bd->csp == iop_cs_rgb);
    bd->masks_support = !(module->flags() & IOP_FLAGS_NO_MASKS);

    bd->masks_modes = NULL;
    bd->blend_modes = NULL;
    bd->masks_combine = NULL;
    bd->masks_invert = NULL;
    bd->blend_modes_all = NULL;

    dt_pthread_mutex_init(&bd->lock, NULL);
    dt_pthread_mutex_lock(&bd->lock);
    bd->timeout_handle = 0;
    bd->save_for_leave = 0;
    dt_pthread_mutex_unlock(&bd->lock);


    /** generate a list of all available blend modes */
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "normal"), DEVELOP_BLEND_NORMAL2);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "normal bounded"), DEVELOP_BLEND_BOUNDED);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "lighten"), DEVELOP_BLEND_LIGHTEN);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "darken"), DEVELOP_BLEND_DARKEN);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "multiply"), DEVELOP_BLEND_MULTIPLY);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "average"), DEVELOP_BLEND_AVERAGE);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "addition"), DEVELOP_BLEND_ADD);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "subtract"), DEVELOP_BLEND_SUBSTRACT);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "difference"), DEVELOP_BLEND_DIFFERENCE2);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "screen"), DEVELOP_BLEND_SCREEN);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "overlay"), DEVELOP_BLEND_OVERLAY);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "softlight"), DEVELOP_BLEND_SOFTLIGHT);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "hardlight"), DEVELOP_BLEND_HARDLIGHT);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "vividlight"), DEVELOP_BLEND_VIVIDLIGHT);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "linearlight"), DEVELOP_BLEND_LINEARLIGHT);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "pinlight"), DEVELOP_BLEND_PINLIGHT);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "lightness"), DEVELOP_BLEND_LIGHTNESS);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "chroma"), DEVELOP_BLEND_CHROMA);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "hue"), DEVELOP_BLEND_HUE);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "color"), DEVELOP_BLEND_COLOR);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "coloradjustment"),
                         DEVELOP_BLEND_COLORADJUST);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "Lab lightness"),
                         DEVELOP_BLEND_LAB_LIGHTNESS);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "Lab color"), DEVELOP_BLEND_LAB_COLOR);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "Lab L-channel"),
                         DEVELOP_BLEND_LAB_L);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "Lab a-channel"),
                         DEVELOP_BLEND_LAB_A);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "Lab b-channel"),
                         DEVELOP_BLEND_LAB_B);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "HSV lightness"),
                         DEVELOP_BLEND_HSV_LIGHTNESS);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "HSV color"), DEVELOP_BLEND_HSV_COLOR);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "RGB red channel"),
                         DEVELOP_BLEND_RGB_R);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "RGB green channel"),
                         DEVELOP_BLEND_RGB_G);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "RGB blue channel"),
                         DEVELOP_BLEND_RGB_B);

    /** deprecated blend modes: make them available as legacy history stacks might want them */
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "difference (deprecated)"),
                         DEVELOP_BLEND_DIFFERENCE);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "inverse (deprecated)"),
                         DEVELOP_BLEND_INVERSE);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "normal (deprecated)"), DEVELOP_BLEND_NORMAL);
    _collect_blend_modes(&(bd->blend_modes_all), C_("blendmode", "unbounded (deprecated)"),
                         DEVELOP_BLEND_UNBOUNDED);


    bd->masks_modes_combo = dt_bauhaus_combobox_new(module);
    dt_bauhaus_widget_set_label(bd->masks_modes_combo, _("blend"), _("blend"));

    dt_bauhaus_combobox_add(bd->masks_modes_combo, _("off"));
    bd->masks_modes = g_list_append(bd->masks_modes, GUINT_TO_POINTER(DEVELOP_MASK_DISABLED));

    dt_bauhaus_combobox_add(bd->masks_modes_combo, _("uniformly"));
    bd->masks_modes = g_list_append(bd->masks_modes, GUINT_TO_POINTER(DEVELOP_MASK_ENABLED));

    if(bd->masks_support)
    {
      dt_bauhaus_combobox_add(bd->masks_modes_combo, _("drawn mask"));
      bd->masks_modes
          = g_list_append(bd->masks_modes, GUINT_TO_POINTER(DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK));
    }

    if(bd->blendif_support)
    {
      dt_bauhaus_combobox_add(bd->masks_modes_combo, _("parametric mask"));
      bd->masks_modes
          = g_list_append(bd->masks_modes, GUINT_TO_POINTER(DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL));
    }

    if(bd->blendif_support && bd->masks_support)
    {
      dt_bauhaus_combobox_add(bd->masks_modes_combo, _("drawn & parametric mask"));
      bd->masks_modes
          = g_list_append(bd->masks_modes, GUINT_TO_POINTER(DEVELOP_MASK_ENABLED | DEVELOP_MASK_BOTH));
    }

    dt_bauhaus_combobox_set(bd->masks_modes_combo, 0);
    gtk_widget_set_tooltip_text(bd->masks_modes_combo,_("activate blending: uniformly, with drawn mask, with "
                                                        "parametric mask, or combination of both"));
    g_signal_connect(G_OBJECT(bd->masks_modes_combo), "value-changed",
                     G_CALLBACK(_blendop_masks_mode_callback), bd);
    dt_gui_add_help_link(GTK_WIDGET(bd->masks_modes_combo), "blending.html");

    bd->blend_modes_combo = dt_bauhaus_combobox_new(module);
    dt_bauhaus_widget_set_label(bd->blend_modes_combo, _("blend"), _("blend mode"));
    gtk_widget_set_tooltip_text(bd->blend_modes_combo, _("choose blending mode"));

    /** populate combobox depending on the color space this module acts in */
    switch(bd->csp)
    {
      case iop_cs_Lab:
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_NORMAL2);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_BOUNDED);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_LIGHTEN);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_DARKEN);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_MULTIPLY);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_AVERAGE);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_ADD);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_SUBSTRACT);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_DIFFERENCE2);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_SCREEN);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_OVERLAY);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_SOFTLIGHT);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_HARDLIGHT);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_VIVIDLIGHT);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_LINEARLIGHT);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_PINLIGHT);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_LAB_LIGHTNESS);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_LAB_A);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_LAB_B);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_LAB_COLOR);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_LIGHTNESS);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_CHROMA);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_HUE);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_COLOR);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_COLORADJUST);
        break;

      case iop_cs_rgb:
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_NORMAL2);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_BOUNDED);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_LIGHTEN);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_DARKEN);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_MULTIPLY);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_AVERAGE);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_ADD);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_SUBSTRACT);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_DIFFERENCE2);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_SCREEN);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_OVERLAY);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_SOFTLIGHT);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_HARDLIGHT);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_VIVIDLIGHT);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_LINEARLIGHT);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_PINLIGHT);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_HSV_LIGHTNESS);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_HSV_COLOR);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_RGB_R);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_RGB_G);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_RGB_B);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_LIGHTNESS);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_CHROMA);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_HUE);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_COLOR);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_COLORADJUST);
        break;

      case iop_cs_RAW:
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_NORMAL2);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_BOUNDED);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_LIGHTEN);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_DARKEN);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_MULTIPLY);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_AVERAGE);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_ADD);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_SUBSTRACT);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_DIFFERENCE2);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_SCREEN);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_OVERLAY);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_SOFTLIGHT);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_HARDLIGHT);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_VIVIDLIGHT);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_LINEARLIGHT);
        _add_blendmode_combo(&(bd->blend_modes), bd->blend_modes_combo, bd->blend_modes_all,
                             DEVELOP_BLEND_PINLIGHT);
        break;
    }


    dt_bauhaus_combobox_set(bd->blend_modes_combo, 0);
    g_signal_connect(G_OBJECT(bd->blend_modes_combo), "value-changed",
                     G_CALLBACK(_blendop_blend_mode_callback), bd);
    dt_gui_add_help_link(GTK_WIDGET(bd->blend_modes_combo), "blending_operators.html");


    bd->opacity_slider = dt_bauhaus_slider_new_with_range(module, 0.0, 100.0, 1, 100.0, 0);
    dt_bauhaus_widget_set_label(bd->opacity_slider, _("blend"), _("opacity"));
    dt_bauhaus_slider_set_format(bd->opacity_slider, "%.0f%%");
    module->fusion_slider = bd->opacity_slider;
    gtk_widget_set_tooltip_text(bd->opacity_slider, _("set the opacity of the blending"));
    g_signal_connect(G_OBJECT(bd->opacity_slider), "value-changed", G_CALLBACK(_blendop_opacity_callback), bd);


    bd->masks_combine_combo = dt_bauhaus_combobox_new(module);
    dt_bauhaus_widget_set_label(bd->masks_combine_combo, _("blend"), _("combine masks"));

    dt_bauhaus_combobox_add(bd->masks_combine_combo, _("exclusive"));
    bd->masks_combine = g_list_append(bd->masks_combine, GUINT_TO_POINTER(DEVELOP_COMBINE_NORM_EXCL));

    dt_bauhaus_combobox_add(bd->masks_combine_combo, _("inclusive"));
    bd->masks_combine = g_list_append(bd->masks_combine, GUINT_TO_POINTER(DEVELOP_COMBINE_NORM_INCL));

    dt_bauhaus_combobox_add(bd->masks_combine_combo, _("exclusive & inverted"));
    bd->masks_combine = g_list_append(bd->masks_combine, GUINT_TO_POINTER(DEVELOP_COMBINE_INV_EXCL));

    dt_bauhaus_combobox_add(bd->masks_combine_combo, _("inclusive & inverted"));
    bd->masks_combine = g_list_append(bd->masks_combine, GUINT_TO_POINTER(DEVELOP_COMBINE_INV_INCL));

    dt_bauhaus_combobox_set(bd->masks_combine_combo, 0);
    gtk_widget_set_tooltip_text(bd->masks_combine_combo,
                                _("how to combine individual drawn mask and different channels of parametric mask"));
    g_signal_connect(G_OBJECT(bd->masks_combine_combo), "value-changed",
                     G_CALLBACK(_blendop_masks_combine_callback), bd);


    bd->masks_invert_combo = dt_bauhaus_combobox_new(module);
    dt_bauhaus_widget_set_label(bd->masks_invert_combo, _("blend"), _("invert mask"));

    dt_bauhaus_combobox_add(bd->masks_invert_combo, _("off"));
    bd->masks_invert = g_list_append(bd->masks_invert, GUINT_TO_POINTER(DEVELOP_COMBINE_NORM));

    dt_bauhaus_combobox_add(bd->masks_invert_combo, _("on"));
    bd->masks_invert = g_list_append(bd->masks_invert, GUINT_TO_POINTER(DEVELOP_COMBINE_INV));

    dt_bauhaus_combobox_set(bd->masks_invert_combo, 0);
    gtk_widget_set_tooltip_text(bd->masks_invert_combo, _("apply mask in normal or inverted mode"));
    g_signal_connect(G_OBJECT(bd->masks_invert_combo), "value-changed",
                     G_CALLBACK(_blendop_masks_invert_callback), bd);


    bd->masks_feathering_guide_combo = dt_bauhaus_combobox_new(module);
    dt_bauhaus_widget_set_label(bd->masks_feathering_guide_combo, _("feathering guide"), _("feathering guide"));

    dt_bauhaus_combobox_add(bd->masks_feathering_guide_combo, _("output image"));
    bd->masks_feathering_guide
        = g_list_append(bd->masks_feathering_guide, GUINT_TO_POINTER(DEVELOP_MASK_GUIDE_OUT));

    dt_bauhaus_combobox_add(bd->masks_feathering_guide_combo, _("input image"));
    bd->masks_feathering_guide
        = g_list_append(bd->masks_feathering_guide, GUINT_TO_POINTER(DEVELOP_MASK_GUIDE_IN));

    dt_bauhaus_combobox_set(bd->masks_feathering_guide_combo, 0);
    gtk_widget_set_tooltip_text(bd->masks_feathering_guide_combo,
                                _("choose to guide mask by input or output image"));
    g_signal_connect(G_OBJECT(bd->masks_feathering_guide_combo), "value-changed",
                     G_CALLBACK(_blendop_masks_feathering_guide_callback), bd);


    bd->feathering_radius_slider = dt_bauhaus_slider_new_with_range(module, 0.0, 250.0, 0.1, 0.0, 1);
    dt_bauhaus_widget_set_label(bd->feathering_radius_slider, _("blend"), _("feathering radius"));
    dt_bauhaus_slider_set_format(bd->feathering_radius_slider, "%.1f");
    gtk_widget_set_tooltip_text(bd->feathering_radius_slider, _("spatial radius of feathering"));
    g_signal_connect(G_OBJECT(bd->feathering_radius_slider), "value-changed",
                     G_CALLBACK(_blendop_blendif_feathering_radius_callback), bd);


    bd->blur_radius_slider = dt_bauhaus_slider_new_with_range(module, 0.0, 100.0, 0.1, 0.0, 1);
    dt_bauhaus_widget_set_label(bd->blur_radius_slider, _("blend"), _("mask blur"));
    dt_bauhaus_slider_set_format(bd->blur_radius_slider, "%.1f");
    gtk_widget_set_tooltip_text(bd->blur_radius_slider, _("radius for gaussian blur of blend mask"));
    g_signal_connect(G_OBJECT(bd->blur_radius_slider), "value-changed",
                     G_CALLBACK(_blendop_blendif_blur_radius_callback), bd);


    bd->brightness_slider = dt_bauhaus_slider_new_with_range(module, -1.0, 1.0, 0.01, 0.0, 2);
    dt_bauhaus_widget_set_label(bd->brightness_slider, _("blend"), _("mask opacity"));
    dt_bauhaus_slider_set_format(bd->brightness_slider, "%.2f");
    gtk_widget_set_tooltip_text(bd->brightness_slider, _("shifts and tilts the tone curve of the blend mask to adjust its "
                                                         "brightness without affecting fully transparent/fully opaque "
                                                         "regions"));
    g_signal_connect(G_OBJECT(bd->brightness_slider), "value-changed",
                     G_CALLBACK(_blendop_blendif_brightness_callback), bd);


    bd->contrast_slider = dt_bauhaus_slider_new_with_range(module, -1.0, 1.0, 0.01, 0.0, 2);
    dt_bauhaus_widget_set_label(bd->contrast_slider, _("blend"), _("mask contrast"));
    dt_bauhaus_slider_set_format(bd->contrast_slider, "%.2f");
    gtk_widget_set_tooltip_text(bd->contrast_slider, _("gives the tone curve of the blend mask an s-like shape to "
                                                       "adjust its contrast"));
    g_signal_connect(G_OBJECT(bd->contrast_slider), "value-changed",
                     G_CALLBACK(_blendop_blendif_contrast_callback), bd);


    bd->showmask = dtgtk_button_new(dtgtk_cairo_paint_showmask, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
    gtk_widget_set_tooltip_text(bd->showmask, _("display mask and/or color channel. ctrl-click to display mask, "
                                                "shift-click to display channel. hover over parametric mask slider to "
                                                "select channel for display"));
    gtk_widget_set_size_request(GTK_WIDGET(bd->showmask), bs, bs);
    g_signal_connect(G_OBJECT(bd->showmask), "button-press-event", G_CALLBACK(_blendop_blendif_showmask_clicked), module);
    gtk_widget_set_name(bd->showmask, "show_mask_button");

    bd->suppress
        = dtgtk_togglebutton_new(dtgtk_cairo_paint_eye_toggle, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
    gtk_widget_set_tooltip_text(bd->suppress, _("temporarily switch off blend mask. only for module in focus"));
    gtk_widget_set_size_request(GTK_WIDGET(bd->suppress), bs, bs);
    g_signal_connect(G_OBJECT(bd->suppress), "toggled", G_CALLBACK(_blendop_blendif_suppress_toggled), module);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
    gtk_box_pack_start(GTK_BOX(iopw), GTK_WIDGET(box), TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(box), dt_ui_section_label_new(_("blend options")), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(bd->masks_modes_combo), TRUE, TRUE, 0);

    bd->top_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE));
    gtk_box_pack_start(GTK_BOX(bd->top_box), bd->blend_modes_combo, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->top_box), bd->opacity_slider, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(bd->top_box), TRUE, TRUE, 0);

    dt_iop_gui_init_masks(GTK_BOX(iopw), module);
    dt_iop_gui_init_blendif(GTK_BOX(iopw), module);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4*DT_BAUHAUS_SPACE);
    gtk_box_pack_end(GTK_BOX(hbox), GTK_WIDGET(bd->showmask), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hbox), GTK_WIDGET(bd->suppress), FALSE, FALSE, 0);
    bd->bottom_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE));
    gtk_box_pack_start(GTK_BOX(bd->bottom_box), GTK_WIDGET(bd->masks_combine_combo), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->bottom_box), GTK_WIDGET(bd->masks_invert_combo), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->bottom_box), dt_ui_section_label_new(_("mask refinement")), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->bottom_box), bd->masks_feathering_guide_combo, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->bottom_box), bd->feathering_radius_slider, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->bottom_box), bd->blur_radius_slider, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->bottom_box), bd->brightness_slider, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->bottom_box), bd->contrast_slider, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->bottom_box), hbox, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(iopw), GTK_WIDGET(bd->bottom_box), TRUE, TRUE, 0);
    dt_gui_add_help_link(GTK_WIDGET(bd->bottom_box), "combined_masks.html");

    bd->blend_inited = 1;
    gtk_widget_queue_draw(GTK_WIDGET(iopw));
    dt_iop_gui_update_blending(module);
  }
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
