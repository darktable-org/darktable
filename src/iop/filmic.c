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
#include "common/sse.h"
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
  int interpolator;
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
  GtkWidget *interpolator;
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
  const int run_saturation = (saturation != 1.0f) ? TRUE : FALSE;

#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) shared(data) schedule(static)
#endif
  for(size_t j = 0; j < roi_out->height; j++)
  {
    const float *in = ((float *)ivoid) + (size_t)ch * roi_in->width * j;
    float *out = ((float *)ovoid) + (size_t)ch * roi_out->width * j;
    for(size_t i = 0; i < roi_out->width; i++)
    {
      float rgb[3] = { 0.0f };
      dt_Lab_to_prophotorgb(in, rgb);

      for(size_t c = 0; c < 3; c++)
      {
        // Log tone-mapping
        rgb[c] /= data->grey_source;
        rgb[c] = (rgb[c] > EPS) ? (fastlog2(rgb[c]) - data->black_source) / data->dynamic_range : EPS;
        rgb[c] = (rgb[c] < 0.0f) ? 0.0f : rgb[c];

        // Filmic S curve
        rgb[c] = (rgb[c] > 1.0f)
                    ? dt_iop_eval_exp(data->unbounded_coeffs, rgb[c])
                    : data->table[CLAMP((int)(rgb[c] * 0x10000ul), 0, 0xffff)];
      }

      // Adjust the saturation
      if (run_saturation)
      {
        // Run only when the saturation has a non-neutral value
        float XYZ[3];
        dt_prophotorgb_to_XYZ(rgb, XYZ);
        for(size_t c = 0; c < 3; c++)
        {
          rgb[c] = XYZ[1] + saturation * (rgb[c] - XYZ[1]);
        }
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
  const int run_saturation = (sat != 1.0f) ? TRUE : FALSE;
  const __m128 saturation = _mm_setr_ps(sat, sat, sat, 0.0f);

  const __m128 grey = _mm_setr_ps(data->grey_source, data->grey_source, data->grey_source, 0.0f);
  const __m128 black = _mm_setr_ps(data->black_source, data->black_source, data->black_source, 0.0f);
  const __m128 dynamic_range = _mm_setr_ps(data->dynamic_range, data->dynamic_range, data->dynamic_range, .0f);

  const float eps = powf(2.0f, -16);
  const __m128 EPS = _mm_setr_ps(eps, eps, eps, 0.0f);
  const __m128 zero = _mm_setzero_ps();

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(data) schedule(static)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    float *in = ((float *)ivoid) + (size_t)ch * roi_in->width * j;
    float *out = ((float *)ovoid) + (size_t)ch * roi_out->width * j;
    for(int i = 0; i < roi_out->width; i++, in += ch, out += ch)
    {
      __m128 XYZ;
      __m128 rgb = dt_XYZ_to_prophotoRGB_sse2(dt_Lab_to_XYZ_sse2(_mm_load_ps(in)));

      // Log tone-mapping
      rgb = rgb / grey;
      rgb = _mm_max_ps(rgb, EPS);
      rgb = _mm_log2_ps(rgb);
      rgb -= black;
      rgb /=  dynamic_range;
      rgb = _mm_max_ps(rgb, zero);

      float rgb_unpack[4] = { _mm_vectorGetByIndex(rgb, 0),
                              _mm_vectorGetByIndex(rgb, 1),
                              _mm_vectorGetByIndex(rgb, 2),
                              _mm_vectorGetByIndex(rgb, 3) };

      for (int c = 0; c < 3; ++c)
      {
        // Filmic S curve
        rgb_unpack[c] = (rgb_unpack[c] > 1.0f)
                      ? dt_iop_eval_exp(data->unbounded_coeffs, rgb_unpack[c])
                      : data->table[CLAMP((int)(rgb_unpack[c] * 0x10000ul), 0, 0xffff)];
      }

      rgb = _mm_load_ps(rgb_unpack);

      // adjust main saturation
      if (run_saturation)
      {
        XYZ = dt_prophotoRGB_to_XYZ_sse2(rgb);
        const __m128 luma = _mm_set1_ps(XYZ[1]); // the Y channel is the relative luminance
        rgb = luma + saturation * (rgb - luma);
      }

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
  cl_mem dev_coeffs = NULL;

  dev_table = dt_opencl_copy_host_to_device(devid, d->table, 256, 256, sizeof(float));
  if(dev_table == NULL) goto error;

  dev_coeffs = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3, d->unbounded_coeffs);
  if(dev_coeffs == NULL) goto error;

  const float dynamic_range = d->dynamic_range;
  const float shadows_range = d->black_source;
  const float grey = d->grey_source;
  const float saturation = d->saturation / 100.0f;

  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 4, sizeof(float), (void *)&dynamic_range);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 5, sizeof(float), (void *)&shadows_range);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 6, sizeof(float), (void *)&grey);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 7, sizeof(cl_mem), (void *)&dev_table);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 8, sizeof(cl_mem), (void *)&dev_coeffs);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 9, sizeof(float), (void *)&saturation);

  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic, sizes);
  if(err != CL_SUCCESS) goto error;
  dt_opencl_release_mem_object(dev_table);
  dt_opencl_release_mem_object(dev_coeffs);
  return TRUE;

error:
  dt_opencl_release_mem_object(dev_table);
  dt_opencl_release_mem_object(dev_coeffs);
  dt_print(DT_DEBUG_OPENCL, "[opencl_filmic] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif


static void sanitize_latitude(dt_iop_filmic_params_t *p, dt_iop_filmic_gui_data_t *g)
{
  if (p->latitude_stops > (p->white_point_source - p->black_point_source) * 0.95f)
  {
    // The film latitude is its linear part
    // it can never be higher than the dynamic range
    p->latitude_stops =  (p->white_point_source - p->black_point_source) * 0.95f;
    darktable.gui->reset = 1;
    dt_bauhaus_slider_set(g->latitude_stops, p->latitude_stops);
    darktable.gui->reset = 0;
  }
  else if (p->latitude_stops <  (p->white_point_source - p->black_point_source) / 4.0f)
  {
    // Having a latitude < dynamic range / 4 breaks the spline interpolation
    p->latitude_stops =  (p->white_point_source - p->black_point_source) / 4.0f;
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

static void interpolator_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  const int combo = dt_bauhaus_combobox_get(widget);
  if(combo == 0) p->interpolator = CUBIC_SPLINE;
  if(combo == 1) p->interpolator = CATMULL_ROM;
  if(combo == 2) p->interpolator = MONOTONE_HERMITE;
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

  // Initialize the LUT with Identity
#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) shared(d) schedule(static)
#endif
  for(int k = 0; k < 0x10000; k++) d->table[k] = k / 0x10000;

  // Reset the curve
  dt_draw_curve_destroy(d->curve);

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

  const float latitude = CLAMP(p->latitude_stops, dynamic_range/4.0f, dynamic_range * 0.95f);
  const float balance = p->balance / 100.0f; // in %

  float contrast = p->contrast;
  if (contrast < grey_display / grey_log)
  {
    // We need grey_display - (contrast * grey_log) <= 0.0
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
  float linear_intercept = grey_display - (contrast * grey_log);

  // X coordinates
  float toe_log = (toe_display - linear_intercept) / p->contrast;
  toe_log = CLAMP(toe_log, 0.001f, grey_log);

  float shoulder_log = (shoulder_display - linear_intercept) / p->contrast;
  shoulder_log = CLAMP(shoulder_log, grey_log, 0.999f);

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

  // Build the curve from the nodes
  if (SHOULDER_LOST && !TOE_LOST)
  {
    // shoulder only broke - we remove it
    const float x[4] = { black_log,
                         toe_log,
                         grey_log,
                         white_log };

    const float y[4] = { black_display,
                         toe_display,
                         grey_display,
                         white_display };

    d->curve = dt_draw_curve_new(0.0, 1.0, p->interpolator);
    for(int k = 0; k < 4; k++) (void)dt_draw_curve_add_point(d->curve, x[k], y[k]);

    //(_("filmic curve using 4 nodes - highlights lost"));

  }
  else if (TOE_LOST && !SHOULDER_LOST)
  {
    // toe only broke - we remove it
    const float x[4] = { black_log,
                         grey_log,
                         shoulder_log,
                         white_log };

    const float y[4] = { black_display,
                         grey_display,
                         shoulder_display,
                         white_display };

    d->curve = dt_draw_curve_new(0.0, 1.0, p->interpolator);
    for(int k = 0; k < 4; k++) (void)dt_draw_curve_add_point(d->curve, x[k], y[k]);

    //dt_control_log(_("filmic curve using 4 nodes - shadows lost"));

  }
  else if (TOE_LOST || SHOULDER_LOST)
  {
    // toe and shoulder both broke - we remove them
    const float x[3] = { black_log,
                         grey_log,
                         white_log };

    const float y[3] = { black_display,
                         grey_display,
                         white_display };

    d->curve = dt_draw_curve_new(0.0, 1.0, p->interpolator);
    for(int k = 0; k < 3; k++) (void)dt_draw_curve_add_point(d->curve, x[k], y[k]);

    //dt_control_log(_("filmic curve using 3 nodes - highlights & shadows lost"));

  }
  else
  {
    // everything OK
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

    d->curve = dt_draw_curve_new(0.0, 1.0, p->interpolator);
    for(int k = 0; k < 5; k++) (void)dt_draw_curve_add_point(d->curve, x[k], y[k]);
  }

  // Compute the LUT
  dt_draw_curve_calc_values(d->curve, 0.0f, 1.0f, 0x10000, NULL, d->table);

  // Linearize the LUT for display transfer function (gamma)
  // TODO: use the actual TRC from the ICC profile. That's just a quick fix.
  // That means dealing with the utter shite that darktable's color management is first
#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) shared(d) schedule(static)
#endif
  for(int k = 0; k < 0x10000; k++) d->table[k] = powf(d->table[k], d->output_power);

  // extrapolation for the LUT (right hand):
  const float x_r[4] = { 0.7f, 0.8f, 0.9f, 1.0f};
  const float y_r[4] = { d->table[CLAMP((int)(x_r[0] * 0x10000ul), 0, 0xffff)],
                         d->table[CLAMP((int)(x_r[1] * 0x10000ul), 0, 0xffff)],
                         d->table[CLAMP((int)(x_r[2] * 0x10000ul), 0, 0xffff)],
                         d->table[CLAMP((int)(x_r[3] * 0x10000ul), 0, 0xffff)] };
  dt_iop_estimate_exp(x_r, y_r, 4, d->unbounded_coeffs);

  // extrapolation for the LUT (left hand): NOT NEEDED HERE - is that useful anyway ?
  /*
  const float x_l[4] = { 0.3f, 0.2f, 0.1f, 0.0f };
  const float y_l[4] = { d->table[CLAMP((int)(x_l[0] * 0x10000ul), 0, 0xffff)],
                         d->table[CLAMP((int)(x_l[1] * 0x10000ul), 0, 0xffff)],
                         d->table[CLAMP((int)(x_l[2] * 0x10000ul), 0, 0xffff)],
                         d->table[CLAMP((int)(x_l[3] * 0x10000ul), 0, 0xffff)],};
  dt_iop_estimate_exp(x_l, y_l, 4, d->unbounded_coeffs + 3);
  */
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_filmic_data_t));
  dt_iop_filmic_data_t *d = (dt_iop_filmic_data_t *)piece->data;

  d->curve = dt_draw_curve_new(0.0, 1.0, CUBIC_SPLINE);
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

  dt_bauhaus_combobox_set(g->interpolator, p->interpolator);


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
                                 6.0,  // intent latitude
                                 2.0,  // intent contrast
                                 90.,   // intent saturation
                                 0.0, // balance shadows/highlights
                                 MONOTONE_HERMITE, //interpolator
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
  gtk_widget_set_tooltip_text(g->grey_point_source, _("adjust to match the average luminance of the subject.\n"
                                                      "except in back-lighting situations, this should be around 18%."));
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
  gtk_widget_set_tooltip_text(g->white_point_source, _("number of stops between middle grey and pure white\nthis is a reading a posemeter would give you on the scene"));
  g_signal_connect(G_OBJECT(g->white_point_source), "value-changed", G_CALLBACK(white_point_source_callback), self);
  dt_bauhaus_widget_set_quad_paint(g->white_point_source, dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->white_point_source, TRUE);
  g_signal_connect(G_OBJECT(g->white_point_source), "quad-pressed", G_CALLBACK(color_picker_callback), self);

  // Auto tune slider
  g->security_factor = dt_bauhaus_slider_new_with_range(self, -200., 200., 1.0, p->security_factor, 2);
  dt_bauhaus_widget_set_label(g->security_factor, NULL, _("auto tuning security factor"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->security_factor, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->security_factor, "%.2f %%");
  gtk_widget_set_tooltip_text(g->security_factor, _("enlarge or shrink the computed dynamic range"));
  g_signal_connect(G_OBJECT(g->security_factor), "value-changed", G_CALLBACK(security_threshold_callback), self);

  g->auto_button = gtk_button_new_with_label(_("auto tune source"));
  gtk_widget_set_tooltip_text(g->auto_button, _("make an optimization with some guessing"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->auto_button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->auto_button), "clicked", G_CALLBACK(optimize_button_pressed_callback), self);


  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("filmic S curve")), FALSE, FALSE, 5);

  // contrast slider
  g->contrast = dt_bauhaus_slider_new_with_range(self, 1., 5., 0.05, p->contrast, 2);
  dt_bauhaus_widget_set_label(g->contrast, NULL, _("contrast"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->contrast, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->contrast, _("slope of the linear part of the curve"));
  g_signal_connect(G_OBJECT(g->contrast), "value-changed", G_CALLBACK(contrast_callback), self);

  // latitude slider
  g->latitude_stops = dt_bauhaus_slider_new_with_range(self, 1.0, 16.0, 0.1, p->latitude_stops, 2);
  dt_bauhaus_widget_set_label(g->latitude_stops, NULL, _("latitude"));
  dt_bauhaus_slider_set_format(g->latitude_stops, "%.2f EV");
  gtk_box_pack_start(GTK_BOX(self->widget), g->latitude_stops, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->latitude_stops, _("linearity domain in the middle of the curve\n"
                                                   "increase to get more contrast at the extreme luminances"));
  g_signal_connect(G_OBJECT(g->latitude_stops), "value-changed", G_CALLBACK(latitude_stops_callback), self);

  // balance slider
  g->balance = dt_bauhaus_slider_new_with_range(self, -99., 99., 1.0, p->balance, 2);
  dt_bauhaus_widget_set_label(g->balance, NULL, _("balance shadows-highlights"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->balance, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->balance, "%.2f %%");
  gtk_widget_set_tooltip_text(g->balance, _("gives more room to shadows or highlights, to protect the details"));
  g_signal_connect(G_OBJECT(g->balance), "value-changed", G_CALLBACK(balance_callback), self);

  // saturation slider
  g->saturation = dt_bauhaus_slider_new_with_range(self, 0, 100., 1., p->saturation, 2);
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
  dt_bauhaus_widget_set_label(g->interpolator, NULL, _("interpolation method"));
  dt_bauhaus_combobox_add(g->interpolator, _("cubic spline"));
  dt_bauhaus_combobox_add(g->interpolator, _("centripetal spline"));
  dt_bauhaus_combobox_add(g->interpolator, _("monotonic spline"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->interpolator , TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->interpolator, _("change this method if you see oscillations or cusps in the curve\n"
                                                 "- cubic spline is better to produce smooth curves but oscillates when nodes are too close\n"
                                                 "- centripetal is better to avoids cusps and oscillations with close nodes but is less smooth\n"
                                                 "- monotonic is better for accuracy of pure analytical functions (log, gamma, exp)\n"));
  g_signal_connect(G_OBJECT(g->interpolator), "value-changed", G_CALLBACK(interpolator_callback), self);

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("destination/display")), FALSE, FALSE, 5);

  // Black slider
  g->black_point_target = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 1, p->black_point_target, 2);
  dt_bauhaus_widget_set_label(g->black_point_target, NULL, _("black luminance"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->black_point_target, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->black_point_target, "%.2f %%");
  gtk_widget_set_tooltip_text(g->black_point_target, _("luminance of output pure black.\n"
                                                        "this should be 0% except if you want a faded look"));
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
  gtk_widget_set_tooltip_text(g->white_point_target, _("luminance of output pure white\n"
                                                        "this should be 100% except if you want a faded look"));
  g_signal_connect(G_OBJECT(g->white_point_target), "value-changed", G_CALLBACK(white_point_target_callback), self);

  // power/gamma slider
  g->output_power = dt_bauhaus_slider_new_with_range(self, 1.0, 2.4, 0.1, p->output_power, 2);
  dt_bauhaus_widget_set_label(g->output_power, NULL, _("destination power factor"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->output_power, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->output_power, _("power or gamma of the transfer function of the display or color space.\n"
                                                  "you should never touch that unless you know what you are doing."));
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
