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
#include "dtgtk/drawingarea.h"
#include "dtgtk/paint.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "gui/color_picker_proxy.h"
#include "iop/iop_api.h"
#include "common/iop_group.h"


#include "develop/imageop.h"
#include "gui/draw.h"
#include "common/iop_group.h"
#include "libs/colorpicker.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef __SSE2__
#include "common/sse.h"
#endif

#define DT_GUI_CURVE_EDITOR_INSET DT_PIXEL_APPLY_DPI(1)


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
  DT_PICKPROFLOG_WHITE_POINT = 3,
  DT_PICKPROFLOG_AUTOTUNE = 4
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
  int interpolator;
} dt_iop_filmic_params_t;

typedef struct dt_iop_filmic_gui_data_t
{
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
  GtkWidget *interpolator;
  int which_colorpicker;
  dt_iop_color_picker_t color_picker;
  GtkDrawingArea *area;
  float table[256];      // precomputed look-up table
  float table_temp[256]; // precomputed look-up for the optimized interpolation
} dt_iop_filmic_gui_data_t;

typedef struct dt_iop_filmic_data_t
{
  float table[0x10000];      // precomputed look-up table
  float table_temp[0x10000]; // precomputed look-up for the optimized interpolation
  float grad_2[0x10000];
  float max_grad;
  float grey_source;
  float black_source;
  float dynamic_range;
  float saturation;
  float output_power;
  float contrast;
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
  return dt_iop_get_group("filmic", IOP_GROUP_TONE);
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

static inline float gaussian(float x, float std)
{
  return expf(- (x * x) / (2.0f * std * std)) / (std * powf(2.0f * M_PI, 0.5f));
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
  const float saturation = data->saturation / 100.0f;

#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) shared(data) schedule(static)
#endif
  for(size_t j = 0; j < roi_out->height; j++)
  {
    const float *in = ((float *)ivoid) + (size_t)ch * roi_in->width * j;
    float *out = ((float *)ovoid) + (size_t)ch * roi_out->width * j;
    for(size_t i = 0; i < roi_out->width; i++)
    {
      float XYZ[3];
      dt_Lab_to_XYZ(in, XYZ);

      float rgb[3] = { 0.0f };
      dt_XYZ_to_prophotorgb(XYZ, rgb);

      int index[3];

      for(size_t c = 0; c < 3; c++)
      {
        // Log tone-mapping on RGB
        rgb[c] = rgb[c] / data->grey_source;
        rgb[c] = (rgb[c] > EPS) ? (fastlog2(rgb[c]) - data->black_source) / data->dynamic_range : EPS;
        rgb[c] = CLAMP(rgb[c], 0.0f, 1.0f);

        // Store the index of the LUT
        index[c] = CLAMP(rgb[c] * 0x10000ul, 0, 0xffff);

        // Filmic S curve
        rgb[c] = data->table[index[c]];
      }

      dt_prophotorgb_to_XYZ(rgb, XYZ);

      const float luma = XYZ[1];
      const float mid_distance = 4.0f * (luma - 0.5f) * (luma - 0.5f);

      for(size_t c = 0; c < 3; c++)
      {
        // Desaturate on the non-linear parts of the curve
        const float concavity = data->grad_2[index[c]];
        const float bounds = 1.0f / (1.0f - luma);
        const float factor = CLAMP(1.0f - bounds * mid_distance * concavity / saturation, 0.0f, 1.0f);
        rgb[c] = luma + factor * (rgb[c] - luma);

        // Apply the transfer function of the display
        rgb[c] = powf(CLAMP(rgb[c], 0.0f, 1.0f), data->output_power);
      }

      // transform the result back to Lab
      // sRGB -> XYZ
      dt_prophotorgb_to_Lab(rgb, out);
      out[3] = in[3];

      in += ch;
      out += ch;
    }
  }

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
    dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}


#if defined(__SSE__)
void process_sse2(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_filmic_data_t *data = (dt_iop_filmic_data_t *)piece->data;

  const int ch = piece->colors;

  const float sat = data->saturation / 100.0f;
  const __m128 saturation = _mm_setr_ps(sat, sat, sat, 0.0f);

  const __m128 grey = _mm_setr_ps(data->grey_source, data->grey_source, data->grey_source, 0.0f);
  const __m128 black = _mm_setr_ps(data->black_source, data->black_source, data->black_source, 0.0f);
  const __m128 dynamic_range = _mm_setr_ps(data->dynamic_range, data->dynamic_range, data->dynamic_range, .0f);
  const __m128 power = _mm_set1_ps(data->output_power);

  const float eps = powf(2.0f, -16);
  const __m128 EPS = _mm_setr_ps(eps, eps, eps, 0.0f);
  const __m128 zero = _mm_setzero_ps();
  const __m128 one = _mm_set1_ps(1.0f);
  const __m128 four = _mm_set1_ps(4.0f);
  const __m128 half = _mm_set1_ps(0.5f);

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(data) schedule(static)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    float *in = ((float *)ivoid) + (size_t)ch * roi_in->width * j;
    float *out = ((float *)ovoid) + (size_t)ch * roi_out->width * j;
    for(int i = 0; i < roi_out->width; i++, in += ch, out += ch)
    {
      __m128 XYZ = dt_Lab_to_XYZ_sse2(_mm_load_ps(in));
      __m128 rgb = dt_XYZ_to_prophotoRGB_sse2(XYZ);

      // Log tone-mapping
      rgb = rgb / grey;
      rgb = _mm_max_ps(rgb, EPS);
      rgb = _mm_log2_ps(rgb);
      rgb -= black;
      rgb /=  dynamic_range;
      rgb = _mm_max_ps(rgb, zero);
      rgb = _mm_min_ps(rgb, one);

      // Unpack SSE vector to regular array
      float rgb_unpack[4] = { _mm_vectorGetByIndex(rgb, 0),
                              _mm_vectorGetByIndex(rgb, 1),
                              _mm_vectorGetByIndex(rgb, 2),
                              _mm_vectorGetByIndex(rgb, 3) };

      int index[4];
      float derivative[4];

      for (size_t c = 0; c < 3; ++c)
      {
        // Filmic S curve
        index[c] = CLAMP(rgb_unpack[c] * 0x10000ul, 0, 0xffff);
        rgb_unpack[c] = data->table[index[c]];
        derivative[c] = data->grad_2[index[c]];
      }

      rgb = _mm_load_ps(rgb_unpack);

      // Desaturate on the non-linear parts of the curve
      XYZ = dt_prophotoRGB_to_XYZ_sse2(rgb);
      const __m128 luma = _mm_set1_ps(_mm_vectorGetByIndex(XYZ, 1));
      const __m128 mid_distance = four * (luma - half) * (luma - half);
      const __m128 concavity = _mm_load_ps(derivative); // derivative
      const __m128 bounds = one / (one - luma);

      __m128 factor = one - (bounds * mid_distance * concavity)  / saturation;
      factor = _mm_max_ps(factor, zero);
      factor = _mm_min_ps(factor, one);

      rgb = luma + factor * (rgb - luma);
      rgb = _mm_max_ps(rgb, zero);
      rgb = _mm_min_ps(rgb, one);

      // Apply the transfer function of the display
      rgb = _mm_pow_ps(rgb, power);

      // transform the result back to Lab
      // sRGB -> XYZ
      XYZ = dt_prophotoRGB_to_XYZ_sse2(rgb);
      // XYZ -> Lab
      _mm_store_ps(out, dt_XYZ_to_Lab_sse2(XYZ));
    }
  }

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}
#endif


#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_filmic_data_t *d = (dt_iop_filmic_data_t *)piece->data;
  dt_iop_filmic_global_data_t *gd = (dt_iop_filmic_global_data_t *)self->data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };

  cl_mem dev_table = NULL;
  cl_mem diff_table = NULL;

  dev_table = dt_opencl_copy_host_to_device(devid, d->table, 256, 256, sizeof(float));
  if(dev_table == NULL) goto error;

  diff_table = dt_opencl_copy_host_to_device(devid, d->grad_2, 256, 256, sizeof(float));
  if(diff_table == NULL) goto error;

  const float dynamic_range = d->dynamic_range;
  const float shadows_range = d->black_source;
  const float grey = d->grey_source;
  const float saturation = d->saturation / 100.0f;
  const float contrast = d->contrast;
  const float power = d->output_power;

  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 4, sizeof(float), (void *)&dynamic_range);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 5, sizeof(float), (void *)&shadows_range);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 6, sizeof(float), (void *)&grey);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 7, sizeof(cl_mem), (void *)&dev_table);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 8, sizeof(cl_mem), (void *)&diff_table);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 9, sizeof(float), (void *)&saturation);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 10, sizeof(float), (void *)&contrast);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 11, sizeof(float), (void *)&power);

  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic, sizes);
  if(err != CL_SUCCESS) goto error;
  dt_opencl_release_mem_object(dev_table);
  dt_opencl_release_mem_object(diff_table);
  return TRUE;

error:
  dt_opencl_release_mem_object(dev_table);
  dt_opencl_release_mem_object(diff_table);
  dt_print(DT_DEBUG_OPENCL, "[opencl_filmic] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

static void sanitize_latitude(dt_iop_filmic_params_t *p, dt_iop_filmic_gui_data_t *g)
{
  if (p->latitude_stops > (p->white_point_source - p->black_point_source) * 0.99f)
  {
    // The film latitude is its linear part
    // it can never be higher than the dynamic range
    p->latitude_stops =  (p->white_point_source - p->black_point_source) * 0.99f;
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
  gtk_widget_queue_draw(self->widget);
}

static void apply_auto_black(dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;

  float noise = powf(2.0f, -16.0f);
  float XYZ[3] = { 0.0f };

  // Black
  dt_Lab_to_prophotorgb(self->picked_color_min, XYZ);
  float black = (XYZ[0] + XYZ[1] + XYZ[2]) / 3.0f;
  float EVmin = Log2Thres(black / (p->grey_point_source / 100.0f), noise);
  EVmin *= (1.0f + p->security_factor / 100.0f);

  p->black_point_source = EVmin;

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->black_point_source, p->black_point_source);
  darktable.gui->reset = 0;

  sanitize_latitude(p, g);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}


static void apply_auto_white_point_source(dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;

  float noise = powf(2.0f, -16.0f);
  float XYZ[3] = { 0.0f };

  // White
  dt_Lab_to_prophotorgb(self->picked_color_max, XYZ);
  float white = fmaxf(fmaxf(XYZ[0], XYZ[1]), XYZ[2]);
  float EVmax = Log2Thres(white / (p->grey_point_source / 100.0f), noise);
  EVmax *= (1.0f + p->security_factor / 100.0f);

  p->white_point_source = EVmax;

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->white_point_source, p->white_point_source);
  darktable.gui->reset = 0;

  sanitize_latitude(p, g);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
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

  dt_iop_color_picker_reset(&g->color_picker, TRUE);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}

static void apply_autotune(dt_iop_module_t *self)
{
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;

  float noise = powf(2.0f, -16.0f);
  float XYZ[3] = { 0.0f };

  // Grey
  dt_Lab_to_XYZ(self->picked_color, XYZ);
  float grey = XYZ[1];
  p->grey_point_source = 100.f * grey;

  // Black
  dt_Lab_to_prophotorgb(self->picked_color_min, XYZ);
  float black = (XYZ[0] + XYZ[1] + XYZ[2]) / 3.0f;
  float EVmin = Log2Thres(black / (p->grey_point_source / 100.0f), noise);
  EVmin *= (1.0f + p->security_factor / 100.0f);

  // White
  dt_Lab_to_prophotorgb(self->picked_color_max, XYZ);
  float white = fmaxf(fmaxf(XYZ[0], XYZ[1]), XYZ[2]);
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

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}

static int _iop_color_picker_get_set(dt_iop_module_t *self, GtkWidget *button)
{
  dt_iop_filmic_gui_data_t *g =  (dt_iop_filmic_gui_data_t *)self->gui_data;

  const int current_picker = g->which_colorpicker;

  g->which_colorpicker = DT_PICKPROFLOG_NONE;

  if(button == g->grey_point_source)
    g->which_colorpicker = DT_PICKPROFLOG_GREY_POINT;
  else if(button == g->black_point_source)
    g->which_colorpicker = DT_PICKPROFLOG_BLACK_POINT;
  else if(button == g->white_point_source)
    g->which_colorpicker = DT_PICKPROFLOG_WHITE_POINT;
  else if(button == g->auto_button)
    g->which_colorpicker = DT_PICKPROFLOG_AUTOTUNE;

  if (current_picker == g->which_colorpicker)
    return ALREADY_SELECTED;
  else
    return g->which_colorpicker;
}

static void _iop_color_picker_apply(struct dt_iop_module_t *self)
{
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
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
     case DT_PICKPROFLOG_AUTOTUNE:
       apply_autotune(self);
       break;
     default:
       break;
  }
}

static void _iop_color_picker_update(dt_iop_module_t *self)
{
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
  const int which_colorpicker = g->which_colorpicker;
  dt_bauhaus_widget_set_quad_active(g->grey_point_source, which_colorpicker == DT_PICKPROFLOG_GREY_POINT);
  dt_bauhaus_widget_set_quad_active(g->black_point_source, which_colorpicker == DT_PICKPROFLOG_BLACK_POINT);
  dt_bauhaus_widget_set_quad_active(g->white_point_source, which_colorpicker == DT_PICKPROFLOG_WHITE_POINT);
  dt_bauhaus_widget_set_quad_active(g->auto_button, which_colorpicker == DT_PICKPROFLOG_AUTOTUNE);
}

static void _iop_color_picker_reset(struct dt_iop_module_t *self)
{
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
  g->which_colorpicker = DT_PICKPROFLOG_NONE;
}

static void grey_point_source_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  p->grey_point_source = dt_bauhaus_slider_get(slider);

  dt_iop_color_picker_reset(&g->color_picker, TRUE);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}

static void white_point_source_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
  p->white_point_source = dt_bauhaus_slider_get(slider);

  sanitize_latitude(p, g);

  dt_iop_color_picker_reset(&g->color_picker, TRUE);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}

static void black_point_source_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
  p->black_point_source = dt_bauhaus_slider_get(slider);

  sanitize_latitude(p, g);

  dt_iop_color_picker_reset(&g->color_picker, TRUE);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}

static void grey_point_target_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  p->grey_point_target = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
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
  gtk_widget_queue_draw(self->widget);
}

static void contrast_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  p->contrast = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
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
  gtk_widget_queue_draw(self->widget);
}

static void black_point_target_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  p->black_point_target = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}

static void output_power_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  p->output_power = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}

static void balance_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  p->balance = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}

static void interpolator_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  const int combo = dt_bauhaus_combobox_get(widget);
  if(combo == 0) p->interpolator = CUBIC_SPLINE;
  if(combo == 1) p->interpolator = CATMULL_ROM;
  if(combo == 2) p->interpolator = MONOTONE_HERMITE;
  if(combo == 3) p->interpolator = 3; // Optimized
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}

void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
  if(!in) dt_iop_color_picker_reset(&g->color_picker, TRUE);
}

void compute_curve_lut(dt_iop_filmic_params_t *p, float *table, float *table_temp, int res)
{
  dt_draw_curve_t *curve;

  const float white_source = p->white_point_source;
  const float black_source = p->black_point_source;
  const float dynamic_range = white_source - black_source;

  // luminance after log encoding
  const float black_log = 0.0f; // assumes user set log as in the autotuner
  const float grey_log = fabsf(p->black_point_source) / dynamic_range;
  const float white_log = 1.0f; // assumes user set log as in the autotuner

  // target luminance desired after filmic curve
  const float black_display = p->black_point_target / 100.0f; // in %
  const float grey_display = powf(p->grey_point_target / 100.0f, 1.0f / (p->output_power));
  const float white_display = p->white_point_target / 100.0f; // in %

  const float latitude = CLAMP(p->latitude_stops, 0.01f, dynamic_range * 0.99f);
  const float balance = p->balance / 100.0f; // in %

  float contrast = p->contrast;
  if (contrast < grey_display / grey_log)
  {
    // We need grey_display - (contrast * grey_log) <= 0.0
    contrast = 1.0001f * grey_display / grey_log;
  }

  // nodes for mapping from log encoding to desired target luminance
  // X coordinates
  float toe_log = 0.0f +
                        ((fabsf(black_source)) *
                          (dynamic_range - latitude)
                          / dynamic_range / dynamic_range);

  toe_log = CLAMP(toe_log, 0.0f, grey_log);

  float shoulder_log = 1.0f -
                        ((white_source) *
                          (dynamic_range - latitude)
                          / dynamic_range / dynamic_range);

  shoulder_log = CLAMP(shoulder_log, grey_log, 1.0f);

  // interception
  float linear_intercept = grey_display - (contrast * grey_log);
  if (linear_intercept > 0.0f) linear_intercept = -0.001f;

  // y coordinates
  float toe_display = (toe_log * contrast + linear_intercept);
  toe_display = CLAMP(toe_display, 0.0f, grey_display);

  float shoulder_display = (shoulder_log * contrast + linear_intercept);
  shoulder_display = CLAMP(shoulder_display, grey_display, 1.0f);

  // Apply the highlights/shadows balance as a shift along the contrast slope
  const float norm = powf(powf(contrast, 2.0f) + 1.0f, 0.5f);

  // negative values drag to the left and compress the shadows, on the UI negative is the inverse
  const float coeff = -(dynamic_range - latitude) / dynamic_range * balance;
  const float offset_y = coeff * contrast /norm;

  toe_display += offset_y;
  toe_display = CLAMP(toe_display, 0.0f, grey_display);

  shoulder_display += offset_y;
  shoulder_display = CLAMP(shoulder_display, grey_display, 1.0f);

  toe_log += offset_y / contrast;
  toe_log = CLAMP(toe_log, 0.0f, grey_log);

  shoulder_log += offset_y / contrast;
  shoulder_log = CLAMP(shoulder_log, grey_log, 1.0f);

  /**
   * Now we have 3 segments :
   *  - x = [0.0 ; toe_log], curved part
   *  - x = [toe_log ; grey_log ; shoulder_log], linear part
   *  - x = [shoulder_log ; 1.0] curved part
   *
   * BUT : in case some nodes overlap, we need to remove them to avoid
   * degenerating of the curve
  **/

  // sanitize the nodes
  int TOE_LOST = FALSE;
  int SHOULDER_LOST = FALSE;

  if (toe_log == grey_log || toe_log == 0.0f || toe_display  == 0.0f || toe_display == grey_display)
  {
    TOE_LOST = TRUE;
  }
  if (shoulder_log == grey_log || shoulder_log == 1.0f || shoulder_display == grey_display || shoulder_display == 1.0f)
  {
    SHOULDER_LOST = TRUE;
  }

  // the cubic spline is extra sensitive to close nodes : they will produce cusps
  // it's no better for other splines, although not as bad
  if (toe_log < 0.001f || toe_display < 0.001f) TOE_LOST = TRUE;
  if (shoulder_log > 0.999f || shoulder_display > 0.999f) SHOULDER_LOST = TRUE;

  // Build the curve from the nodes
  int nodes;
  float x[5], y[5];

  if (SHOULDER_LOST && !TOE_LOST)
  {
    // shoulder only broke - we remove it
    nodes = 4;
    x[0] = black_log;
    x[1] = toe_log;
    x[2] = grey_log;
    x[3] = white_log;

    y[0] = black_display;
    y[1] = toe_display;
    y[2] = grey_display;
    y[3] = white_display;

    //(_("filmic curve using 4 nodes - highlights lost"));

  }
  else if (TOE_LOST && !SHOULDER_LOST)
  {
    // toe only broke - we remove it
    nodes = 4;

    x[0] = black_log;
    x[1] = grey_log;
    x[2] = shoulder_log;
    x[3] = white_log;

    y[0] = black_display;
    y[1] = grey_display;
    y[2] = shoulder_display;
    y[3] = white_display;

    //dt_control_log(_("filmic curve using 4 nodes - shadows lost"));

  }
  else if (TOE_LOST && SHOULDER_LOST)
  {
    // toe and shoulder both broke - we remove them
    nodes = 3;

    x[0] = black_log;
    x[1] = grey_log;
    x[2] = white_log;

    y[0] = black_display;
    y[1] = grey_display;
    y[2] = white_display;

    //dt_control_log(_("filmic curve using 3 nodes - highlights & shadows lost"));

  }
  else
  {
    // everything OK
    nodes = 4;

    x[0] = black_log;
    x[1] = toe_log;
    //x[2] = grey_log,
    x[2] = shoulder_log;
    x[3] = white_log;

    y[0] = black_display;
    y[1] = toe_display;
    //y[2] = grey_display,
    y[2] = shoulder_display;
    y[3] = white_display;
  }

  if (p->interpolator != 3)
  {
    // Compute the interpolation
    curve = dt_draw_curve_new(0.0, 1.0, p->interpolator);
    for(int k = 0; k < nodes; k++) (void)dt_draw_curve_add_point(curve, x[k], y[k]);

    // Compute the LUT
    dt_draw_curve_calc_values(curve, 0.0f, 1.0f, res, NULL, table);
  }
  else
  {
    // Compute the monotonic interpolation
    curve = dt_draw_curve_new(0.0, 1.0, MONOTONE_HERMITE);
    for(int k = 0; k < nodes; k++) (void)dt_draw_curve_add_point(curve, x[k], y[k]);
    dt_draw_curve_calc_values(curve, 0.0f, 1.0f, res, NULL, table_temp);

    // Compute the cubic spline interpolation
    dt_draw_curve_destroy(curve);
    curve = dt_draw_curve_new(0.0, 1.0, CUBIC_SPLINE);
    for(int k = 0; k < nodes; k++) (void)dt_draw_curve_add_point(curve, x[k], y[k]);
    dt_draw_curve_calc_values(curve, 0.0f, 1.0f, res, NULL, table);

    // Average both LUT
#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) shared(table, table_temp, res) schedule(static)
#endif
    for(int k = 0; k < res; k++) table[k] = (table[k] + table_temp[k]) / 2.0f;
  }

  dt_draw_curve_destroy(curve);
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

  // luminance after log encoding
  const float grey_log = fabsf(p->black_point_source) / dynamic_range;

  // target luminance desired after filmic curve
  const float grey_display = powf(p->grey_point_target / 100.0f, 1.0f / (p->output_power));

  float contrast = p->contrast;
  if (contrast < grey_display / grey_log)
  {
    // We need grey_display - (contrast * grey_log) <= 0.0
    contrast = 1.0001f * grey_display / grey_log;
  }

  // commit
  d->dynamic_range = dynamic_range;
  d->black_source = black_source;
  d->grey_source = grey_source;
  d->output_power = p->output_power;
  d->saturation = p->saturation;
  d->contrast = contrast;

  // compute the curves and their LUT
  compute_curve_lut(p, d->table, d->table_temp, 0x10000);

  // Build a window function based on the log.
  // This will be used to selectively desaturate the non-linear parts
  // to avoid over-saturation in the toe and shoulder.

#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) shared(d) schedule(static)
#endif
  for(int k = 1; k < 65535; k++)
  {
    const float x = ((float)k) / 65536.0f;
    //d->grad_2[k] = fabsf(-d->table[k] * 2.0f + d->table[k-1] + d->table[k+1]) / 2.0f;
    d->grad_2[k] = powf(2.0f, (-dynamic_range * x));
  }
  d->grad_2[0] = 1.0f;
  d->grad_2[65535] = 1.0f;

}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_filmic_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)module->params;

  dt_iop_color_picker_reset(&g->color_picker, TRUE);

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

  dt_bauhaus_combobox_set(g->interpolator, p->interpolator);

  gtk_widget_queue_draw(self->widget);

}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_filmic_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_filmic_params_t));
  module->default_enabled = 0;
  module->priority = 642; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_filmic_params_t);
  module->gui_data = NULL;

  dt_iop_filmic_params_t tmp
    = (dt_iop_filmic_params_t){
                                 .grey_point_source   = 18, // source grey
                                 .black_point_source  = -7.0,  // source black
                                 .white_point_source  = 3.0,  // source white
                                 .security_factor     = 0.0,  // security factor
                                 .grey_point_target   = 18.0, // target grey
                                 .black_point_target  = 0.0,  // target black
                                 .white_point_target  = 100.0,  // target white
                                 .output_power        = 2.2,  // target power (~ gamma)
                                 .latitude_stops      = 2.0,  // intent latitude
                                 .contrast            = 1.333,  // intent contrast
                                 .saturation          = 20.0,   // intent saturation
                                 .balance             = 0.0, // balance shadows/highlights
                                 .interpolator        = CUBIC_SPLINE //interpolator
                              };
  memcpy(module->params, &tmp, sizeof(dt_iop_filmic_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_filmic_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 22; // filmic.cl, from programs.conf
  dt_iop_filmic_global_data_t *gd
      = (dt_iop_filmic_global_data_t *)malloc(sizeof(dt_iop_filmic_global_data_t));

  module->data = gd;
  gd->kernel_filmic = dt_opencl_create_kernel(program, "filmic");
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_filmic_global_data_t *gd = (dt_iop_filmic_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_filmic);
  free(module->data);
  module->data = NULL;
}

void gui_reset(dt_iop_module_t *self)
{
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
  dt_iop_color_picker_reset(&g->color_picker, TRUE);
}

static gboolean dt_iop_tonecurve_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_filmic_gui_data_t *c = (dt_iop_filmic_gui_data_t *)self->gui_data;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  compute_curve_lut(p, c->table, c->table_temp, 256);

  const int inset = DT_GUI_CURVE_EDITOR_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  // clear bg
  cairo_set_source_rgb(cr, .2, .2, .2);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2 * inset;
  height -= 2 * inset;

  cairo_set_source_rgb(cr, .3, .3, .3);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  // draw grid
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(.4));
  cairo_set_source_rgb(cr, .1, .1, .1);
  dt_draw_grid(cr, 4, 0, 0, width, height);

  // draw curve
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
  cairo_set_source_rgb(cr, .9, .9, .9);
  cairo_move_to(cr, 0, height * (1.0 - c->table[0]));

  for(int k = 1; k < 256; k++)
  {
    cairo_line_to(cr, k * width / 255.0, (double)height * (1.0 - c->table[k]));
  }
  cairo_stroke(cr);
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

void gui_init(dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_filmic_gui_data_t));
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));

  g->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(1.0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->area), TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->area), _("double click to reset curve"));
  g_signal_connect(G_OBJECT(g->area), "draw", G_CALLBACK(dt_iop_tonecurve_draw), self);

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("logarithmic shaper")), FALSE, FALSE, 5);

  // grey_point_source slider
  g->grey_point_source = dt_bauhaus_slider_new_with_range(self, 0.1, 100., 0.1, p->grey_point_source, 2);
  dt_bauhaus_widget_set_label(g->grey_point_source, NULL, _("middle grey luminance"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->grey_point_source, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->grey_point_source, "%.2f %%");
  gtk_widget_set_tooltip_text(g->grey_point_source, _("adjust to match the average luminance of the subject.\n"
                                                      "except in back-lighting situations, this should be around 18%."));
  g_signal_connect(G_OBJECT(g->grey_point_source), "value-changed", G_CALLBACK(grey_point_source_callback), self);
  dt_bauhaus_widget_set_quad_paint(g->grey_point_source, dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->grey_point_source, TRUE);
  g_signal_connect(G_OBJECT(g->grey_point_source), "quad-pressed", G_CALLBACK(dt_iop_color_picker_callback), &g->color_picker);

  // White slider
  g->white_point_source = dt_bauhaus_slider_new_with_range(self, 0.5, 16.0, 0.1, p->white_point_source, 2);
  dt_bauhaus_widget_set_label(g->white_point_source, NULL, _("white relative exposure"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->white_point_source, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->white_point_source, "%.2f EV");
  gtk_widget_set_tooltip_text(g->white_point_source, _("number of stops between middle grey and pure white\nthis is a reading a posemeter would give you on the scene"));
  g_signal_connect(G_OBJECT(g->white_point_source), "value-changed", G_CALLBACK(white_point_source_callback), self);
  dt_bauhaus_widget_set_quad_paint(g->white_point_source, dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->white_point_source, TRUE);
  g_signal_connect(G_OBJECT(g->white_point_source), "quad-pressed", G_CALLBACK(dt_iop_color_picker_callback), &g->color_picker);

  // Black slider
  g->black_point_source = dt_bauhaus_slider_new_with_range(self, -16.0, -0.5, 0.1, p->black_point_source, 2);
  dt_bauhaus_widget_set_label(g->black_point_source, NULL, _("black relative exposure"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->black_point_source, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->black_point_source, "%.2f EV");
  gtk_widget_set_tooltip_text(g->black_point_source, _("number of stops between middle grey and pure black\nthis is a reading a posemeter would give you on the scene"));
  g_signal_connect(G_OBJECT(g->black_point_source), "value-changed", G_CALLBACK(black_point_source_callback), self);
  dt_bauhaus_widget_set_quad_paint(g->black_point_source, dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->black_point_source, TRUE);
  g_signal_connect(G_OBJECT(g->black_point_source), "quad-pressed", G_CALLBACK(dt_iop_color_picker_callback), &g->color_picker);

  // Security factor
  g->security_factor = dt_bauhaus_slider_new_with_range(self, -200., 200., 1.0, p->security_factor, 2);
  dt_bauhaus_widget_set_label(g->security_factor, NULL, _("auto tuning safety factor"));
  dt_bauhaus_widget_set_label(g->security_factor, NULL, _("safety factor"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->security_factor, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->security_factor, "%.2f %%");
  gtk_widget_set_tooltip_text(g->security_factor, _("enlarge or shrink the computed dynamic range\n"
                                                    "useful in conjunction with \"auto tune levels\""));
  g_signal_connect(G_OBJECT(g->security_factor), "value-changed", G_CALLBACK(security_threshold_callback), self);

  // Auto tune slider
  g->auto_button = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->auto_button, NULL, _("auto tune levels"));
  dt_bauhaus_widget_set_quad_paint(g->auto_button, dtgtk_cairo_paint_colorpicker,
                                   CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->auto_button, TRUE);
  g_signal_connect(G_OBJECT(g->auto_button), "quad-pressed", G_CALLBACK(dt_iop_color_picker_callback), &g->color_picker);
  gtk_widget_set_tooltip_text(g->auto_button, _("make an optimization with some guessing"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->auto_button, TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("filmic S curve")), FALSE, FALSE, 5);

  // contrast slider
  g->contrast = dt_bauhaus_slider_new_with_range(self, 1., 4., 0.01, p->contrast, 3);
  dt_bauhaus_widget_set_label(g->contrast, NULL, _("contrast"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->contrast, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->contrast, _("slope of the linear part of the curve"));
  g_signal_connect(G_OBJECT(g->contrast), "value-changed", G_CALLBACK(contrast_callback), self);

  // latitude slider
  g->latitude_stops = dt_bauhaus_slider_new_with_range(self, 1.0, 16.0, 0.05, p->latitude_stops, 3);
  dt_bauhaus_widget_set_label(g->latitude_stops, NULL, _("latitude"));
  dt_bauhaus_slider_set_format(g->latitude_stops, "%.2f EV");
  gtk_box_pack_start(GTK_BOX(self->widget), g->latitude_stops, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->latitude_stops, _("linearity domain in the middle of the curve\n"
                                                   "increase to get more contrast at the extreme luminances"));
  g_signal_connect(G_OBJECT(g->latitude_stops), "value-changed", G_CALLBACK(latitude_stops_callback), self);

  // balance slider
  g->balance = dt_bauhaus_slider_new_with_range(self, -50., 50., 1.0, p->balance, 2);
  dt_bauhaus_widget_set_label(g->balance, NULL, _("balance shadows-highlights"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->balance, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->balance, "%.2f %%");
  gtk_widget_set_tooltip_text(g->balance, _("gives more room to shadows or highlights, to protect the details"));
  g_signal_connect(G_OBJECT(g->balance), "value-changed", G_CALLBACK(balance_callback), self);

  // saturation slider
  g->saturation = dt_bauhaus_slider_new_with_range(self, 0.01, 100., 0.05, p->saturation, 2);
  dt_bauhaus_widget_set_label(g->saturation, NULL, _("saturation"));
  dt_bauhaus_slider_set_format(g->saturation, "%.2f %%");
  gtk_box_pack_start(GTK_BOX(self->widget), g->saturation, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->saturation, _("desaturates the output, if the contrast adjustment\n"
                                               "produces over-saturation in shadows"));
  g_signal_connect(G_OBJECT(g->saturation), "value-changed", G_CALLBACK(saturation_callback), self);

    /* From src/common/curve_tools.h :
    #define CUBIC_SPLINE 0
    #define CATMULL_ROM 1
    #define MONOTONE_HERMITE 2
  */
  g->interpolator = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->interpolator, NULL, _("intent"));
  dt_bauhaus_combobox_add(g->interpolator, _("contrasted")); // cubic spline
  dt_bauhaus_combobox_add(g->interpolator, _("faded")); // centripetal spline
  dt_bauhaus_combobox_add(g->interpolator, _("linear")); // monotonic spline
  dt_bauhaus_combobox_add(g->interpolator, _("optimized")); // monotonic spline
  gtk_box_pack_start(GTK_BOX(self->widget), g->interpolator , TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->interpolator, _("change this method if you see reversed contrast or faded blacks"));
  g_signal_connect(G_OBJECT(g->interpolator), "value-changed", G_CALLBACK(interpolator_callback), self);

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("destination/display")), FALSE, FALSE, 5);

  // Black slider
  g->black_point_target = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 1, p->black_point_target, 2);
  dt_bauhaus_widget_set_label(g->black_point_target, NULL, _("black luminance"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->black_point_target, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->black_point_target, "%.2f %%");
  gtk_widget_set_tooltip_text(g->black_point_target, _("luminance of output pure black, "
                                                        "this should be 0%\nexcept if you want a faded look"));
  g_signal_connect(G_OBJECT(g->black_point_target), "value-changed", G_CALLBACK(black_point_target_callback), self);

  // grey_point_source slider
  g->grey_point_target = dt_bauhaus_slider_new_with_range(self, 0.1, 50., 0.5, p->grey_point_target, 2);
  dt_bauhaus_widget_set_label(g->grey_point_target, NULL, _("middle grey destination"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->grey_point_target, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->grey_point_target, "%.2f %%");
  gtk_widget_set_tooltip_text(g->grey_point_target, _("midde grey value of the target display or color space.\n"
                                                      "you should never touch that unless you know what you are doing."));
  g_signal_connect(G_OBJECT(g->grey_point_target), "value-changed", G_CALLBACK(grey_point_target_callback), self);

  // White slider
  g->white_point_target = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 1., p->white_point_target, 2);
  dt_bauhaus_widget_set_label(g->white_point_target, NULL, _("white luminance"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->white_point_target, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->white_point_target, "%.2f %%");
  gtk_widget_set_tooltip_text(g->white_point_target, _("luminance of output pure white, "
                                                        "this should be 100%\nexcept if you want a faded look"));
  g_signal_connect(G_OBJECT(g->white_point_target), "value-changed", G_CALLBACK(white_point_target_callback), self);

  // power/gamma slider
  g->output_power = dt_bauhaus_slider_new_with_range(self, 1.0, 2.4, 0.1, p->output_power, 2);
  dt_bauhaus_widget_set_label(g->output_power, NULL, _("destination power factor"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->output_power, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->output_power, _("power or gamma of the transfer function of the display or color space.\n"
                                                  "you should never touch that unless you know what you are doing."));
  g_signal_connect(G_OBJECT(g->output_power), "value-changed", G_CALLBACK(output_power_callback), self);

  init_picker(&g->color_picker,
              self,
              _iop_color_picker_get_set,
              _iop_color_picker_apply,
              _iop_color_picker_reset,
              _iop_color_picker_update);
}


void gui_cleanup(dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
