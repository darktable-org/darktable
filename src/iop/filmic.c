/*
   This file is part of darktable,
   copyright (c) 2018 Aurélien Pierre, with guidance of Troy James Sobotka.

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
#include "common/colorspaces_inline_conversions.h"
#include "common/darktable.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop_math.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"
#include "common/iop_group.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE_INTROSPECTION(1, dt_iop_filmic_params_t)

/**
 * DOCUMENTATION
 *
 * This code ports :
 * 1. Troy Sobotka's filmic curves for Blender (and other softs)
 *      https://github.com/sobotka/OpenAgX/blob/master/lib/agx_colour.py
 * 2. ACES camera logarithmic encoding
 *        https://github.com/ampas/aces-dev/blob/master/transforms/ctl/utilities/ACESutil.Lin_to_Log2_param.ctl
 *
 * The ACES log implementation is taken from the profile_gamma.c IOP
 * where it works in camera RGB space. Here, it works on an arbitrary RGB
 * space. ProPhotoRGB has been choosen for its wide gamut coverage and
 * for conveniency because it's already in darktable's libs. Any other
 * RGB working space could work. This chouice could (should) also be
 * exposed to the user.
 *
 * The filmic curves are tonecurves intended to simulate the luminance
 * transfer function of film with "S" curves. These could be reproduced in
 * the tonecurve.c IOP, however what we offer here is a parametric
 * interface usefull to remap accurately and promptly the middle grey
 * to any arbitrary value choosen accordingly to the destination space.
 *
 * The combined use of both define a modern way to deal with large
 * dynamic range photographs by remapping the values with a comprehensive
 * interface avoiding many of the back and forth adjustements darktable
 * is prone to enforce.
 *
 * */

typedef enum dt_iop_filmic_pickcolor_type_t
{
  DT_PICKPROFLOG_NONE = 0,
  DT_PICKPROFLOG_GREY_POINT = 1,
  DT_PICKPROFLOG_BLACK_POINT = 2,
  DT_PICKPROFLOG_WHITE_POINT = 3
} dt_iop_filmic_pickcolor_type_t;

typedef struct dt_iop_filmic_params_t
{
  float grey_point_source;
  float black_point_source;
  float white_point_source;
  float security_factor;
  float grey_point_target;
  float black_point_target;
  float white_point_target;
  float output_power;
  float latitude_stops;
  float contrast;
  float saturation;
  float balance;
} dt_iop_filmic_params_t;

typedef struct dt_iop_filmic_gui_data_t
{
  int apply_picked_color;
  int which_colorpicker;
  GtkWidget *white_point_source;
  GtkWidget *grey_point_source;
  GtkWidget *black_point_source;
  GtkWidget *security_factor;
  GtkWidget *auto_button;
  GtkWidget *grey_point_target;
  GtkWidget *white_point_target;
  GtkWidget *black_point_target;
  GtkWidget *output_power;
  GtkWidget *latitude_stops;
  GtkWidget *contrast;
  GtkWidget *saturation;
  GtkWidget *balance;
} dt_iop_filmic_gui_data_t;

typedef struct dt_iop_filmic_data_t
{
  dt_draw_curve_t *curve;
  float table[0x10000];      // precomputed look-up table
  float unbounded_coeffs[3]; // approximation for extrapolation of curve
  float grey_source;
  float black_source;
  float dynamic_range;
  float saturation;
  float output_power;
} dt_iop_filmic_data_t;

typedef struct dt_iop_filmic_global_data_t
{
  int kernel_filmic;
  int kernel_filmic_log;
} dt_iop_filmic_global_data_t;


const char *name()
{
  return _("filmic");
}

int groups()
{
  return dt_iop_get_group("filmic", IOP_GROUP_COLOR);
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

static inline float Log2(float x)
{
  if(x > 0.0f)
  {
    return logf(x) / logf(2.0f);
  }
  else
  {
    return x;
  }
}

static inline float Log2Thres(float x, float Thres)
{
  if(x > Thres)
  {
    return logf(x) / logf(2.f);
  }
  else
  {
    return logf(Thres) / logf(2.f);
  }
}


// From data/kernels/extended.cl
static inline float fastlog2(float x)
{
  union { float f; unsigned int i; } vx = { x };
  union { unsigned int i; float f; } mx = { (vx.i & 0x007FFFFF) | 0x3f000000 };

  float y = vx.i;

  y *= 1.1920928955078125e-7f;

  return y - 124.22551499f
    - 1.498030302f * mx.f
    - 1.72587999f / (0.3520887068f + mx.f);
}


static inline float average(const float *list, const size_t elements)
{
  float collect = 0;
  for (size_t i = 0; i < elements; ++i)
  {
    collect += list[i];
  }
  return collect / elements;
}


void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_filmic_data_t *data = (dt_iop_filmic_data_t *)piece->data;

  const int ch = piece->colors;

  /** The log2(x) -> -INF when x -> 0
  * thus very low values (noise) will get even lower, resulting in noise negative amplification,
  * which leads to pepper noise in shadows. To avoid that, we need to clip values that are noise for sure.
  * Using 16 bits RAW data, the black value (known by rawspeed for every manufacturer) could be used as a threshold.
  * However, at this point of the pixelpipe, the RAW levels have already been corrected and everything can happen with black levels
  * in the exposure module. So we define the threshold as the first non-null 16 bit integer
  */
  const float EPS = powf(2.0f, -16);

#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) shared(data) schedule(static)
#endif
  for(size_t j = 0; j < roi_out->height; j++)
  {
    const float *in = ((float *)ivoid) + (size_t)ch * roi_in->width * j;
    float *out = ((float *)ovoid) + (size_t)ch * roi_out->width * j;
    for(size_t i = 0; i < roi_out->width; i++)
    {
      // transform the pixel to sRGB:
      // Lab -> XYZ
      float XYZ[3] = { 0.0f };
      dt_Lab_to_XYZ(in, XYZ);

      // XYZ -> sRGB
      float rgb[3] = { 0.0f };
      dt_XYZ_to_prophotorgb(XYZ, rgb);

      for(size_t c = 0; c < 3; c++)
      {
        // Log tone-mapping
        rgb[c] = (rgb[c] > EPS) ? (Log2(rgb[c] / data->grey_source) - data->black_source) / data->dynamic_range : EPS;

        // Filmic S curve
        rgb[c] = data->table[CLAMP((int)(rgb[c] * 0x10000ul), 0, 0xffff)];

        // Linearize for display transfor function (gamma)
        // TODO: use the actual TRC from the ICC profile. That's just a quick fix.
        // That means dealing with the utter shite that darktable's color management is first
        rgb[c] = (rgb[c] <= 0.0f) ? rgb[c] : powf(rgb[c], data->output_power);
      }

      // Adjust the saturation
      dt_prophotorgb_to_XYZ(rgb, XYZ);
      const float Y = XYZ[1];

      for(size_t c = 0; c < 3; c++)
      {
        rgb[c] = Y + data->saturation * (rgb[c] - Y);
      }

      // transform the result back to Lab
      // sRGB -> XYZ
      dt_prophotorgb_to_XYZ(rgb, XYZ);

      // XYZ -> Lab
      dt_XYZ_to_Lab(XYZ, out);
      out[3] = in[3];

      in += ch;
      out += ch;
    }
  }

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
    dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

static void sanitize_latitude(dt_iop_filmic_params_t *p, dt_iop_filmic_gui_data_t *g)
{
  if (p->latitude_stops > (p->white_point_source - p->black_point_source) * 0.9f)
  {
    // The film latitude is its linear part
    // it can never be higher than the dynamic range
    p->latitude_stops =  (p->white_point_source - p->black_point_source) * 0.9f;
    darktable.gui->reset = 1;
    dt_bauhaus_slider_set(g->latitude_stops, p->latitude_stops);
    darktable.gui->reset = 0;
  }
  else if (p->latitude_stops <  (p->white_point_source - p->black_point_source) / 2.6f)
  {
    // Having a latitude < dynamic range / 2.5 breaks the spline interpolation
    p->latitude_stops =  (p->white_point_source - p->black_point_source) / 2.6f;
    darktable.gui->reset = 1;
    dt_bauhaus_slider_set(g->latitude_stops, p->latitude_stops);
    darktable.gui->reset = 0;
  }
}

static void apply_auto_grey(dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;

  float XYZ[3] = { 0.0f };
  dt_Lab_to_XYZ(self->picked_color, XYZ);

  float grey = XYZ[1];
  p->grey_point_source = 100.f * grey;

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->grey_point_source, p->grey_point_source);
  darktable.gui->reset = 0;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void apply_auto_black(dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;

  float noise = powf(2.0f, -16.0f);

  float XYZ[3] = { 0.0f };
  dt_Lab_to_XYZ(self->picked_color_min, XYZ);

  // Black
  float black = XYZ[1];
  float EVmin = Log2Thres(black / (p->grey_point_source / 100.0f), noise);
  EVmin *= (1.0f + p->security_factor / 100.0f);

  p->black_point_source = EVmin;

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->black_point_source, p->black_point_source);
  darktable.gui->reset = 0;

  sanitize_latitude(p, g);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void apply_auto_white_point_source(dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;

  float noise = powf(2.0f, -16.0f);

  float XYZ[3] = { 0.0f };
  dt_Lab_to_XYZ(self->picked_color_max, XYZ);

  // White
  float white = XYZ[1];
  float EVmax = Log2Thres(white / (p->grey_point_source / 100.0f), noise);
  EVmax *= (1.0f + p->security_factor / 100.0f);

  p->white_point_source = EVmax;

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->white_point_source, p->white_point_source);
  darktable.gui->reset = 0;

  sanitize_latitude(p, g);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void security_threshold_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;

  float previous = p->security_factor;
  p->security_factor = dt_bauhaus_slider_get(slider);
  float ratio = (p->security_factor - previous) / (previous + 100.0f);

  float EVmin = p->black_point_source;
  EVmin = EVmin + ratio * EVmin;

  float EVmax = p->white_point_source;
  EVmax = EVmax + ratio * EVmax;

  p->white_point_source = EVmax;
  p->black_point_source = EVmin;

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->white_point_source, p->white_point_source);
  dt_bauhaus_slider_set_soft(g->black_point_source, p->black_point_source);
  darktable.gui->reset = 0;

  sanitize_latitude(p, g);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void optimize_button_pressed_callback(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);

  dt_iop_request_focus(self);
  dt_lib_colorpicker_set_area(darktable.lib, 0.99);
  dt_control_queue_redraw();
  self->request_color_pick = DT_REQUEST_COLORPICK_MODULE;
  dt_dev_reprocess_all(self->dev);

  if(self->request_color_pick != DT_REQUEST_COLORPICK_MODULE || self->picked_color_max[0] < 0.0f)
  {
    dt_control_log(_("wait for the preview to be updated."));
    return;
  }

  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;

  float noise = powf(2.0f, -16.0f);
  float XYZ[3] = { 0.0f };

  // Grey
  dt_Lab_to_XYZ(self->picked_color, XYZ);
  float grey = XYZ[1];
  p->grey_point_source = 100.f * grey;

  // Black
  dt_Lab_to_XYZ(self->picked_color_min, XYZ);
  float black = XYZ[1];
  float EVmin = Log2Thres(black / (p->grey_point_source / 100.0f), noise);
  EVmin *= (1.0f + p->security_factor / 100.0f);

  // White
  dt_Lab_to_XYZ(self->picked_color_max, XYZ);
  float white = XYZ[1];
  float EVmax = Log2Thres(white / (p->grey_point_source / 100.0f), noise);
  EVmax *= (1.0f + p->security_factor / 100.0f);

  p->black_point_source = EVmin;
  p->white_point_source = EVmax;

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->grey_point_source, p->grey_point_source);
  dt_bauhaus_slider_set(g->black_point_source, p->black_point_source);
  dt_bauhaus_slider_set(g->white_point_source, p->white_point_source);
  darktable.gui->reset = 0;

  sanitize_latitude(p, g);

  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void grey_point_source_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  p->grey_point_source = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void white_point_source_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
  p->white_point_source = dt_bauhaus_slider_get(slider);

  sanitize_latitude(p, g);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void black_point_source_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
  p->black_point_source = dt_bauhaus_slider_get(slider);

  sanitize_latitude(p, g);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void grey_point_target_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  p->grey_point_target = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void latitude_stops_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;

  p->latitude_stops = dt_bauhaus_slider_get(slider);

  sanitize_latitude(p, g);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void contrast_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  p->contrast = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void saturation_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  p->saturation = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void white_point_target_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  p->white_point_target = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void black_point_target_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  p->black_point_target = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void output_power_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  p->output_power = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void balance_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  p->balance = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void disable_colorpick(struct dt_iop_module_t *self)
{
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  g->which_colorpicker = DT_PICKPROFLOG_NONE;
}

static int call_apply_picked_color(struct dt_iop_module_t *self, dt_iop_filmic_gui_data_t *g)
{
  int handled = 1;
  switch(g->which_colorpicker)
  {
     case DT_PICKPROFLOG_GREY_POINT:
       apply_auto_grey(self);
       break;
     case DT_PICKPROFLOG_BLACK_POINT:
       apply_auto_black(self);
       break;
     case DT_PICKPROFLOG_WHITE_POINT:
       apply_auto_white_point_source(self);
       break;
     default:
       handled = 0;
       break;
  }
  return handled;
}

static int get_colorpick_from_button(GtkWidget *button, dt_iop_filmic_gui_data_t *g)
{
  int which_colorpicker = DT_PICKPROFLOG_NONE;

  if(button == g->grey_point_source)
    which_colorpicker = DT_PICKPROFLOG_GREY_POINT;
  else if(button == g->black_point_source)
    which_colorpicker = DT_PICKPROFLOG_BLACK_POINT;
  else if(button == g->white_point_source)
    which_colorpicker = DT_PICKPROFLOG_WHITE_POINT;

  return which_colorpicker;
}

static void set_colorpick_state(dt_iop_filmic_gui_data_t *g, const int which_colorpicker)
{
  dt_bauhaus_widget_set_quad_active(g->grey_point_source, which_colorpicker == DT_PICKPROFLOG_GREY_POINT);
  dt_bauhaus_widget_set_quad_active(g->black_point_source, which_colorpicker == DT_PICKPROFLOG_BLACK_POINT);
  dt_bauhaus_widget_set_quad_active(g->white_point_source, which_colorpicker == DT_PICKPROFLOG_WHITE_POINT);
}

static void color_picker_callback(GtkWidget *button, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;

  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);

  if(self->request_color_pick == DT_REQUEST_COLORPICK_OFF)
  {
    g->which_colorpicker = get_colorpick_from_button(button, g);

    if(g->which_colorpicker != DT_PICKPROFLOG_NONE)
    {
      dt_iop_request_focus(self);
      self->request_color_pick = DT_REQUEST_COLORPICK_MODULE;

      if(self->request_color_pick != DT_REQUEST_COLORPICK_OFF) dt_lib_colorpicker_set_area(darktable.lib, 0.99);

      g->apply_picked_color = 1;

      dt_dev_reprocess_all(self->dev);
    }
  }
  else
  {
    if(self->request_color_pick != DT_REQUEST_COLORPICK_MODULE || self->picked_color_max[0] < 0.0f)
    {
      dt_control_log(_("wait for the preview to be updated."));
      return;
    }
    self->request_color_pick = DT_REQUEST_COLORPICK_OFF;

    if(g->apply_picked_color)
    {
      call_apply_picked_color(self, g);
      g->apply_picked_color = 0;
    }

    const int which_colorpicker = get_colorpick_from_button(button, g);
    if(which_colorpicker != g->which_colorpicker && which_colorpicker != DT_PICKPROFLOG_NONE)
    {
      g->which_colorpicker = which_colorpicker;

      self->request_color_pick = DT_REQUEST_COLORPICK_MODULE;

      if(self->request_color_pick != DT_REQUEST_COLORPICK_OFF) dt_lib_colorpicker_set_area(darktable.lib, 0.99);

      g->apply_picked_color = 1;

      dt_dev_reprocess_all(self->dev);
    }
    else
    {
      g->which_colorpicker = DT_PICKPROFLOG_NONE;
    }
  }

  set_colorpick_state(g, g->which_colorpicker);
}

void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;

  if(!in)
  {
    disable_colorpick(self);
    g->apply_picked_color = 0;
    set_colorpick_state(g, g->which_colorpicker);
  }
}

int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state)
{
  int handled = 0;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;

  if(self->request_color_pick != DT_REQUEST_COLORPICK_OFF && which == 1)
  {
    handled = call_apply_picked_color(self, g);
    g->apply_picked_color = 0;
  }

  return handled;
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)p1;
  dt_iop_filmic_data_t *d = (dt_iop_filmic_data_t *)piece->data;

  // source luminance - Used only in the log encoding
  const float white_source = p->white_point_source;
  const float grey_source = p->grey_point_source / 100.0f; // in %
  const float black_source = p->black_point_source;
  const float dynamic_range = white_source - black_source;

  // commit
  d->dynamic_range = dynamic_range;
  d->black_source = black_source;
  d->grey_source = grey_source;
  d->output_power = p->output_power;
  d->saturation = p->saturation;

  // luminance after log encoding
  const float black_log = 0.0f; // assumes user set log as in the autotuner
  const float grey_log = fabsf(p->black_point_source) / dynamic_range;
  const float white_log = 1.0f; // assumes user set log as in the autotuner

  // target luminance desired after filmic curve
  const float black_display = p->black_point_target / 100.0f; // in %
  const float grey_display = powf(p->grey_point_target / 100.0f, 1.0f / (p->output_power));
  const float white_display = p->white_point_target / 100.0f; // in %

  const float latitude = CLAMP(p->latitude_stops, dynamic_range/2.6f, dynamic_range * 0.9f);
  const float balance = p->balance / 100.0f; // in %

  float contrast = p->contrast;
  if (contrast < grey_display / grey_log)
  {
    contrast = grey_display / grey_log;
  }

  // nodes for mapping from log encoding to desired target luminance
  // Y coordinates
  float toe_display = black_display +
                        ((fabsf(black_source)) *
                          (dynamic_range - latitude) *
                          (1.0f - balance) / dynamic_range / dynamic_range);

  toe_display = CLAMP(toe_display, 0.0f, grey_display);

  float shoulder_display = white_display -
                        ((white_source) *
                          (dynamic_range - latitude) *
                          (1.0f + balance) / dynamic_range / dynamic_range);

  shoulder_display = CLAMP(shoulder_display, grey_display, 1.0f);

  // interception
  float linear_intercept = grey_display - (p->contrast * grey_log);

  // X coordinates
  float toe_log = (toe_display - linear_intercept) / p->contrast;
  toe_log = CLAMP(toe_log, 0.0f, grey_log);

  float shoulder_log = (shoulder_display - linear_intercept) / p->contrast;
  shoulder_log = CLAMP(shoulder_log, grey_log, 1.0f);


  // sanitize values
  if (toe_log == grey_log)
  {
    toe_display = grey_display;
  }
  else if (toe_log == 0.0f)
  {
    toe_display = 0.0f;
  }
  if (toe_display == grey_display)
  {
    toe_log = grey_log;
  }
  else if (toe_display  == 0.0f)
  {
    toe_log = 0.0f;
  }
  if (shoulder_log == grey_log)
  {
    shoulder_display = grey_display;
  }
  else if (shoulder_log == 1.0f)
  {
    shoulder_display = 1.0f;
  }
  if (shoulder_display == grey_display)
  {
    shoulder_log = grey_log;
  }
  else if (shoulder_display == 1.0f)
  {
    shoulder_log = 1.0f;
  }

  /**
   * Now we have 3 segments :
   *  - x = [0.0 ; toe_log], curved part
   *  - x = [toe_log ; shoulder_log], linear part
   *  - x = [shoulder_log ; 1.0] curved part
  **/

  // Build the curve and LUT
  const float x[5] = { black_log,
                       toe_log,
                       grey_log,
                       shoulder_log,
                       white_log };

  const float y[5] = { black_display,
                       toe_display,
                       grey_display,
                       shoulder_display,
                       white_display };

  dt_draw_curve_destroy(d->curve);
  d->curve = dt_draw_curve_new(0.0, 1.0, MONOTONE_HERMITE);
  for(int k = 0; k < 5; k++) (void)dt_draw_curve_add_point(d->curve, x[k], y[k]);
  dt_draw_curve_calc_values(d->curve, 0.0f, 1.0f, 0x10000, NULL, d->table);
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_filmic_data_t));
  dt_iop_filmic_data_t *d = (dt_iop_filmic_data_t *)piece->data;

  d->curve = dt_draw_curve_new(0.0, 1.0, CUBIC_SPLINE);

  // Initialize the LUT with identity
  for(int k = 0; k < 0x10000; k++) d->table[k] = k / 0x10000;

  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_filmic_data_t *d = (dt_iop_filmic_data_t *)piece->data;
  dt_draw_curve_destroy(d->curve);
  free(piece->data);
  piece->data = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)module->params;

  disable_colorpick(self);

  self->color_picker_box[0] = self->color_picker_box[1] = .25f;
  self->color_picker_box[2] = self->color_picker_box[3] = .75f;
  self->color_picker_point[0] = self->color_picker_point[1] = 0.5f;

  dt_bauhaus_slider_set(g->white_point_source, p->white_point_source);
  dt_bauhaus_slider_set(g->grey_point_source, p->grey_point_source);
  dt_bauhaus_slider_set(g->black_point_source, p->black_point_source);
  dt_bauhaus_slider_set(g->security_factor, p->security_factor);
  dt_bauhaus_slider_set(g->white_point_target, p->white_point_target);
  dt_bauhaus_slider_set(g->grey_point_target, p->grey_point_target);
  dt_bauhaus_slider_set(g->black_point_target, p->black_point_target);
  dt_bauhaus_slider_set(g->output_power, p->output_power);
  dt_bauhaus_slider_set(g->latitude_stops, p->latitude_stops);
  dt_bauhaus_slider_set(g->contrast, p->contrast);
  dt_bauhaus_slider_set(g->saturation, p->saturation);
  dt_bauhaus_slider_set(g->balance, p->balance);

  set_colorpick_state(g, g->which_colorpicker);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_filmic_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_filmic_params_t));
  module->default_enabled = 0;
  module->priority = 642; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_filmic_params_t);
  module->gui_data = NULL;

  /** Param :
    float grey_point_source;
    float black_point_source;
    float white_point_source;
    float security_factor;
    float grey_point_target;
    float black_point_target;
    float white_point_target;
    float output_power;
    float latitude_stops;
    float contrast;
    float saturation;
    float balance;
  **/

  dt_iop_filmic_params_t tmp
    = (dt_iop_filmic_params_t){
                                 18, // source grey
                                -7.0,  // source black
                                 3.0,  // source white
                                 0.0,  // security factor
                                 18.0, // target grey
                                 0.0,  // target black
                                 100.0,  // target white
                                 2.2,  // target power (~ gamma)
                                 4.0,  // intent latitude
                                 2.0,  // intent contrast
                                 1.0,   // intent saturation
                                 0.0, // balance shadows/highlights
                              };
  memcpy(module->params, &tmp, sizeof(dt_iop_filmic_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_filmic_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  //const int program = 2; // basic.cl, from programs.conf
  dt_iop_filmic_global_data_t *gd
      = (dt_iop_filmic_global_data_t *)malloc(sizeof(dt_iop_filmic_global_data_t));

  module->data = gd;
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  //dt_iop_filmic_global_data_t *gd = (dt_iop_filmic_global_data_t *)module->data;
  free(module->data);
  module->data = NULL;
}


void gui_init(dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_filmic_gui_data_t));
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;

  disable_colorpick(self);
  g->apply_picked_color = 0;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("logarithmic tone-mapping")), FALSE, FALSE, 5);

  // grey_point_source slider
  g->grey_point_source = dt_bauhaus_slider_new_with_range(self, 0.1, 100., 0.5, p->grey_point_source, 2);
  dt_bauhaus_widget_set_label(g->grey_point_source, NULL, _("middle grey luminance"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->grey_point_source, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->grey_point_source, "%.2f %%");
  gtk_widget_set_tooltip_text(g->grey_point_source, _("adjust to match the average luma of the subject"));
  g_signal_connect(G_OBJECT(g->grey_point_source), "value-changed", G_CALLBACK(grey_point_source_callback), self);
  dt_bauhaus_widget_set_quad_paint(g->grey_point_source, dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->grey_point_source, TRUE);
  g_signal_connect(G_OBJECT(g->grey_point_source), "quad-pressed", G_CALLBACK(color_picker_callback), self);

  // Black slider
  g->black_point_source = dt_bauhaus_slider_new_with_range(self, -16.0, -0.5, 0.1, p->black_point_source, 2);
  dt_bauhaus_widget_set_label(g->black_point_source, NULL, _("black relative exposure"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->black_point_source, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->black_point_source, "%.2f EV");
  gtk_widget_set_tooltip_text(g->black_point_source, _("number of stops between middle grey and pure black\nthis is a reading a posemeter would give you on the scene"));
  g_signal_connect(G_OBJECT(g->black_point_source), "value-changed", G_CALLBACK(black_point_source_callback), self);
  dt_bauhaus_widget_set_quad_paint(g->black_point_source, dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->black_point_source, TRUE);
  g_signal_connect(G_OBJECT(g->black_point_source), "quad-pressed", G_CALLBACK(color_picker_callback), self);

  // White slider
  g->white_point_source = dt_bauhaus_slider_new_with_range(self, 0.5, 16.0, 0.1, p->white_point_source, 2);
  dt_bauhaus_widget_set_label(g->white_point_source, NULL, _("white relative exposure"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->white_point_source, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->white_point_source, "%.2f EV");
  gtk_widget_set_tooltip_text(g->white_point_source, _("number of stops between pure black and pure white\nthis is a reading a posemeter would give you on the scene"));
  g_signal_connect(G_OBJECT(g->white_point_source), "value-changed", G_CALLBACK(white_point_source_callback), self);
  dt_bauhaus_widget_set_quad_paint(g->white_point_source, dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->white_point_source, TRUE);
  g_signal_connect(G_OBJECT(g->white_point_source), "quad-pressed", G_CALLBACK(color_picker_callback), self);

  // Auto tune slider
  g->security_factor = dt_bauhaus_slider_new_with_range(self, -200., 200., 1.0, p->security_factor, 2);
  dt_bauhaus_widget_set_label(g->security_factor, NULL, _("auto tuning security factor"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->security_factor, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->security_factor, "%.2f %%");
  gtk_widget_set_tooltip_text(g->security_factor, _("enlarge or shrink the computed dynamic range\nthis is usefull when noise perturbates the measurements"));
  g_signal_connect(G_OBJECT(g->security_factor), "value-changed", G_CALLBACK(security_threshold_callback), self);

  g->auto_button = gtk_button_new_with_label(_("auto tune source"));
  gtk_widget_set_tooltip_text(g->auto_button, _("make an optimization with some guessing"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->auto_button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->auto_button), "clicked", G_CALLBACK(optimize_button_pressed_callback), self);


  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("filmic S curve")), FALSE, FALSE, 5);

  // Black slider
  g->black_point_target = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 1, p->black_point_target, 2);
  dt_bauhaus_widget_set_label(g->black_point_target, NULL, _("black absolute luminance"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->black_point_target, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->black_point_target, "%.2f %%");
  gtk_widget_set_tooltip_text(g->black_point_target, _("luminance of output pure black"));
  g_signal_connect(G_OBJECT(g->black_point_target), "value-changed", G_CALLBACK(black_point_target_callback), self);

  // latitude slider
  g->latitude_stops = dt_bauhaus_slider_new_with_range(self, 1.0, 16.0, 0.1, p->latitude_stops, 2);
  dt_bauhaus_widget_set_label(g->latitude_stops, NULL, _("latitude of the film"));
  dt_bauhaus_slider_set_format(g->latitude_stops, "%.2f EV");
  gtk_box_pack_start(GTK_BOX(self->widget), g->latitude_stops, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->latitude_stops, _("linearity domain in the middle of the curve"));
  g_signal_connect(G_OBJECT(g->latitude_stops), "value-changed", G_CALLBACK(latitude_stops_callback), self);

  // White slider
  g->white_point_target = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 1., p->white_point_target, 2);
  dt_bauhaus_widget_set_label(g->white_point_target, NULL, _("white absolute luminance"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->white_point_target, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->white_point_target, "%.2f %%");
  gtk_widget_set_tooltip_text(g->white_point_target, _("luminance of output pure white"));
  g_signal_connect(G_OBJECT(g->white_point_target), "value-changed", G_CALLBACK(white_point_target_callback), self);

  // contrast slider
  g->contrast = dt_bauhaus_slider_new_with_range(self, 1., 5., 0.1, p->contrast, 2);
  dt_bauhaus_widget_set_label(g->contrast, NULL, _("contrast"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->contrast, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->contrast, _("slope of the linear part of the curve"));
  g_signal_connect(G_OBJECT(g->contrast), "value-changed", G_CALLBACK(contrast_callback), self);

  // balance slider
  g->balance = dt_bauhaus_slider_new_with_range(self, -99., 99., 1.0, p->balance, 2);
  dt_bauhaus_widget_set_label(g->balance, NULL, _("balance shadows/highlights"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->balance, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->balance, "%.2f %%");
  gtk_widget_set_tooltip_text(g->balance, _("gives more room to shadows or highlights, to protect the details"));
  g_signal_connect(G_OBJECT(g->balance), "value-changed", G_CALLBACK(balance_callback), self);

  // saturation slider
  g->saturation = dt_bauhaus_slider_new_with_range(self, 0, 2., 0.1, p->saturation, 2);
  dt_bauhaus_widget_set_label(g->saturation, NULL, _("saturation"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->saturation, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->saturation, _("correction of the output saturation"));
  g_signal_connect(G_OBJECT(g->saturation), "value-changed", G_CALLBACK(saturation_callback), self);

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("destination/display")), FALSE, FALSE, 5);

  // grey_point_source slider
  g->grey_point_target = dt_bauhaus_slider_new_with_range(self, 0.1, 50., 0.5, p->grey_point_target, 2);
  dt_bauhaus_widget_set_label(g->grey_point_target, NULL, _("middle grey destination"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->grey_point_target, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->grey_point_target, "%.2f %%");
  gtk_widget_set_tooltip_text(g->grey_point_target, _("adjust to match the average luma of the subject"));
  g_signal_connect(G_OBJECT(g->grey_point_target), "value-changed", G_CALLBACK(grey_point_target_callback), self);

  // power/gamma slider
  g->output_power = dt_bauhaus_slider_new_with_range(self, 1.8, 2.4, 0.1, p->output_power, 2);
  dt_bauhaus_widget_set_label(g->output_power, NULL, _("destination power factor"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->output_power, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->output_power, _("number of stops between pure black and pure white\nthis is a reading a posemeter would give you on the scene"));
  g_signal_connect(G_OBJECT(g->output_power), "value-changed", G_CALLBACK(output_power_callback), self);
}


void gui_cleanup(dt_iop_module_t *self)
{
  disable_colorpick(self);
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
