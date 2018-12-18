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
#include "dtgtk/expander.h"
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


DT_MODULE_INTROSPECTION(2, dt_iop_filmic_params_t)

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
  int preserve_color;
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
  GtkWidget *preserve_color;
  GtkWidget *extra_expander;
  GtkWidget *extra_toggle;
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
  int preserve_color;
  float latitude_min;
  float latitude_max;
} dt_iop_filmic_data_t;

typedef struct dt_iop_filmic_nodes_t
{
  int nodes;
  float y[5];
  float x[5];
} dt_iop_filmic_nodes_t;

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

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params,
                  const int new_version)
{
  if(old_version == 1 && new_version == 2)
  {
    typedef struct dt_iop_filmic_params_v1_t
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
    } dt_iop_filmic_params_v1_t;

    dt_iop_filmic_params_v1_t *o = (dt_iop_filmic_params_v1_t *)old_params;
    dt_iop_filmic_params_t *n = (dt_iop_filmic_params_t *)new_params;
    dt_iop_filmic_params_t *d = (dt_iop_filmic_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    n->grey_point_source = o->grey_point_source;
    n->white_point_source = o->white_point_source;
    n->black_point_source = o->black_point_source;
    n->security_factor = o->security_factor;
    n->grey_point_target = o->grey_point_target;
    n->black_point_target = o->black_point_target;
    n->white_point_target = o->white_point_target;
    n->output_power = o->output_power;
    n->latitude_stops = o->latitude_stops;
    n->contrast = o->contrast;
    n->saturation = o->saturation;
    n->balance = o->balance;
    n->interpolator = o->interpolator;
    n->preserve_color = 0;
    return 0;
  }
  return 1;
}

/* UNCOMMENT AFTER DARKTABLE 2.6 IS RELEASED

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_filmic_params_t p;
  memset(&p, 0, sizeof(p));

  // Fine-tune settings, no use here
  p.interpolator = CUBIC_SPLINE;
  p.balance = 0.0f;

  // Output - standard display, gamma 2.2
  p.output_power = 2.2f;
  p.white_point_target = 100.0f;
  p.black_point_target = 0.0f;
  p.grey_point_target = 18.0f;

  // Input - standard raw picture
  p.security_factor = 0.0f;
  p.contrast = 1.5f;

  // Presets indoors
  p.grey_point_source = 18.0f;
  p.preserve_color = 0;
  p.latitude_stops = 2.0f;
  p.saturation = 100.0f;
  p.white_point_source = 2.45f;

  p.black_point_source = -7.55f;
  dt_gui_presets_add_generic(_("indoors, 10 EV"), self->op, self->version(), &p, sizeof(p), 1);

  p.black_point_source = -8.55f;
  dt_gui_presets_add_generic(_("indoors, 11 EV"), self->op, self->version(), &p, sizeof(p), 1);

  p.black_point_source = -9.55f;
  dt_gui_presets_add_generic(_("indoors, 12 EV"), self->op, self->version(), &p, sizeof(p), 1);

  // Presets outdoors
  p.grey_point_source = 9.0f;
  p.preserve_color = 1;
  p.latitude_stops = 3.0f;
  p.saturation = 50.0f;
  p.white_point_source = 3.45f;

  p.black_point_source = -6.55f;
  dt_gui_presets_add_generic(_("outdoors, 10 EV"), self->op, self->version(), &p, sizeof(p), 1);

  p.black_point_source = -7.55f;
  dt_gui_presets_add_generic(_("outdoors, 11 EV"), self->op, self->version(), &p, sizeof(p), 1);

  p.black_point_source = -8.55f;
  dt_gui_presets_add_generic(_("outdoors, 12 EV"), self->op, self->version(), &p, sizeof(p), 1);

  // Presets backlighting
  p.grey_point_source = 4.5f;
  p.preserve_color = 1;
  p.latitude_stops = 4.0f;
  p.saturation = 50.0f;
  p.white_point_source = 4.45f;

  p.black_point_source = -5.55f;
  dt_gui_presets_add_generic(_("backlighting, 10 EV"), self->op, self->version(), &p, sizeof(p), 1);

  p.black_point_source = -6.55f;
  dt_gui_presets_add_generic(_("backlighting, 11 EV"), self->op, self->version(), &p, sizeof(p), 1);

  p.black_point_source = -7.55f;
  dt_gui_presets_add_generic(_("backlighting, 12 EV"), self->op, self->version(), &p, sizeof(p), 1);

  // Presets sunset
  p.grey_point_source = 2.25f;
  p.preserve_color = 1;
  p.latitude_stops = 5.0f;
  p.saturation = 50.0f;
  p.white_point_source = 5.45f;

  p.black_point_source = -5.55f;
  dt_gui_presets_add_generic(_("sunset, 11 EV"), self->op, self->version(), &p, sizeof(p), 1);

  p.black_point_source = -6.55f;
  dt_gui_presets_add_generic(_("sunset, 12 EV"), self->op, self->version(), &p, sizeof(p), 1);

  p.black_point_source = -7.55f;
  dt_gui_presets_add_generic(_("sunset, 13 EV"), self->op, self->version(), &p, sizeof(p), 1);

  // Presets very dark subject
  p.grey_point_source = 1.125f;
  p.preserve_color = 1;
  p.latitude_stops = 6.0f;
  p.saturation = 50.0f;
  p.white_point_source = 6.45f;

  p.black_point_source = -7.55f;
  dt_gui_presets_add_generic(_("very dark subject, 12 EV"), self->op, self->version(), &p, sizeof(p), 1);

  p.black_point_source = -8.55f;
  dt_gui_presets_add_generic(_("very dark subject, 13 EV"), self->op, self->version(), &p, sizeof(p), 1);

  p.black_point_source = -9.55f;
  dt_gui_presets_add_generic(_("very dark subject, 14 EV"), self->op, self->version(), &p, sizeof(p), 1);
}
*/

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
  const int preserve_color = data->preserve_color;

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

      float concavity, luma;

      if (preserve_color)
      {
        int index;
        float ratios[4];
        float max = fmaxf(fmaxf(rgb[0], rgb[1]), rgb[2]);

        // Save the ratios
        for (size_t c = 0; c < 3; ++c) ratios[c] = rgb[c] / max;

        // Log tone-mapping
        max = max / data->grey_source;
        max = (max > EPS) ? (fastlog2(max) - data->black_source) / data->dynamic_range : EPS;
        max = CLAMP(max, 0.0f, 1.0f);

        // Filmic S curve on the max RGB
        index = CLAMP(max * 0x10000ul, 0, 0xffff);
        max = data->table[index];
        concavity = data->grad_2[index];

        // Re-apply ratios
        for (size_t c = 0; c < 3; ++c) rgb[c] = ratios[c] * max;

        luma = max;
      }
      else
      {
        int index[3];

        for(size_t c = 0; c < 3; c++)
        {
          // Log tone-mapping on RGB
          rgb[c] = rgb[c] / data->grey_source;
          rgb[c] = (rgb[c] > EPS) ? (fastlog2(rgb[c]) - data->black_source) / data->dynamic_range : EPS;
          rgb[c] = CLAMP(rgb[c], 0.0f, 1.0f);

          // Store the index of the LUT
          index[c] = CLAMP(rgb[c] * 0x10000ul, 0, 0xffff);
        }

        // Concavity
        dt_prophotorgb_to_XYZ(rgb, XYZ);
        concavity = data->grad_2[(int)CLAMP(XYZ[1] * 0x10000ul, 0, 0xffff)];

        // Filmic S curve
        for(size_t c = 0; c < 3; c++) rgb[c] = data->table[index[c]];

        dt_prophotorgb_to_XYZ(rgb, XYZ);
        luma = XYZ[1];
      }

      // Desaturate on the non-linear parts of the curve
      for(size_t c = 0; c < 3; c++)
      {
        // Desaturate on the non-linear parts of the curve
        rgb[c] = luma + concavity * (rgb[c] - luma);

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
  const int preserve_color = data->preserve_color;


  const __m128 grey = _mm_setr_ps(data->grey_source, data->grey_source, data->grey_source, 0.0f);
  const __m128 black = _mm_setr_ps(data->black_source, data->black_source, data->black_source, 0.0f);
  const __m128 dynamic_range = _mm_setr_ps(data->dynamic_range, data->dynamic_range, data->dynamic_range, .0f);
  const __m128 power = _mm_set1_ps(data->output_power);

  const float eps = powf(2.0f, -16);
  const __m128 EPS = _mm_setr_ps(eps, eps, eps, 0.0f);
  const __m128 zero = _mm_setzero_ps();
  const __m128 one = _mm_set1_ps(1.0f);

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

      float luma, concavity;

      if (preserve_color)
      {
        // Unpack SSE vector to regular array
        float rgb_unpack[4] = { _mm_vectorGetByIndex(rgb, 0),
                                _mm_vectorGetByIndex(rgb, 1),
                                _mm_vectorGetByIndex(rgb, 2),
                                _mm_vectorGetByIndex(rgb, 3) };
        int index;
        float ratios[4];
        float max = fmaxf(fmaxf(rgb_unpack[0], rgb_unpack[1]), rgb_unpack[2]);

        // Save the ratios
        for (size_t c = 0; c < 3; ++c) ratios[c] = rgb_unpack[c] / max;

        // Log tone-mapping
        max = max / data->grey_source;
        max = (max > eps) ? (fastlog2(max) - data->black_source) / data->dynamic_range : eps;
        max = CLAMP(max, 0.0f, 1.0f);

        // Filmic S curve on the max RGB
        index = CLAMP(max * 0x10000ul, 0, 0xffff);
        max = data->table[index];
        concavity = data->grad_2[index];

        // Re-apply ratios
        for (size_t c = 0; c < 3; ++c) rgb_unpack[c] = ratios[c] * max;

        rgb = _mm_load_ps(rgb_unpack);
        luma = max;
      }
      else
      {
        // Log tone-mapping
        rgb = rgb / grey;
        rgb = _mm_max_ps(rgb, EPS);
        rgb = _mm_log2_ps(rgb);
        rgb -= black;
        rgb /=  dynamic_range;
        rgb = _mm_max_ps(rgb, zero);
        rgb = _mm_min_ps(rgb, one);

        // Store the derivative at the pixel luminance
        XYZ = dt_prophotoRGB_to_XYZ_sse2(rgb);
        concavity = data->grad_2[(int)CLAMP(XYZ[1] * 0x10000ul, 0, 0xffff)];

        // Unpack SSE vector to regular array
        float rgb_unpack[4] = { _mm_vectorGetByIndex(rgb, 0),
                                _mm_vectorGetByIndex(rgb, 1),
                                _mm_vectorGetByIndex(rgb, 2),
                                _mm_vectorGetByIndex(rgb, 3) };

        // Filmic S curve
        for (size_t c = 0; c < 3; ++c)
        {
          rgb_unpack[c] = data->table[(int)CLAMP(rgb_unpack[c] * 0x10000ul, 0, 0xffff)];
        }

        rgb = _mm_load_ps(rgb_unpack);
        XYZ = dt_prophotoRGB_to_XYZ_sse2(rgb);
        luma = XYZ[1];

      }

      rgb = luma + _mm_set1_ps(concavity) * (rgb - luma);
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
  const float contrast = d->contrast;
  const float power = d->output_power;
  const int preserve_color = d->preserve_color;

  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 4, sizeof(float), (void *)&dynamic_range);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 5, sizeof(float), (void *)&shadows_range);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 6, sizeof(float), (void *)&grey);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 7, sizeof(cl_mem), (void *)&dev_table);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 8, sizeof(cl_mem), (void *)&diff_table);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 9, sizeof(float), (void *)&contrast);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 10, sizeof(float), (void *)&power);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic, 11, sizeof(int), (void *)&preserve_color);

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
    dt_bauhaus_slider_set_soft(g->latitude_stops, p->latitude_stops);
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

  const float grey = XYZ[1];
  const float prev_grey = p->grey_point_source;
  p->grey_point_source = 100.f * grey;
  const float grey_var = Log2(prev_grey / p->grey_point_source);
  p->black_point_source = p->black_point_source - grey_var;
  p->white_point_source = p->white_point_source + grey_var;

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->grey_point_source, p->grey_point_source);
  dt_bauhaus_slider_set_soft(g->black_point_source, p->black_point_source);
  dt_bauhaus_slider_set_soft(g->white_point_source, p->white_point_source);
  darktable.gui->reset = 0;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}

static void apply_auto_black(dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;

  const float noise = powf(2.0f, -16.0f);
  float XYZ[3] = { 0.0f };

  // Black
  dt_Lab_to_XYZ(self->picked_color_min, XYZ);
  const float black = XYZ[1];
  float EVmin = Log2Thres(black / (p->grey_point_source / 100.0f), noise);
  EVmin *= (1.0f + p->security_factor / 100.0f);

  p->black_point_source = EVmin;

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->black_point_source, p->black_point_source);
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

  const float noise = powf(2.0f, -16.0f);
  float XYZ[3] = { 0.0f };

  // White
  dt_Lab_to_XYZ(self->picked_color_max, XYZ);
  const float white = XYZ[1];
  float EVmax = Log2Thres(white / (p->grey_point_source / 100.0f), noise);
  EVmax *= (1.0f + p->security_factor / 100.0f);

  p->white_point_source = EVmax;

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->white_point_source, p->white_point_source);
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

  const float noise = powf(2.0f, -16.0f);
  float XYZ[3] = { 0.0f };

  // Grey
  dt_Lab_to_XYZ(self->picked_color, XYZ);
  const float grey = XYZ[1];
  p->grey_point_source = 100.f * grey;

  // Black
  dt_Lab_to_XYZ(self->picked_color_min, XYZ);
  const float black = XYZ[1];
  float EVmin = Log2Thres(black / (p->grey_point_source / 100.0f), noise);
  EVmin *= (1.0f + p->security_factor / 100.0f);

  // White
  dt_Lab_to_XYZ(self->picked_color_max, XYZ);
  const float white = XYZ[1];
  float EVmax = Log2Thres(white / (p->grey_point_source / 100.0f), noise);
  EVmax *= (1.0f + p->security_factor / 100.0f);

  p->black_point_source = EVmin;
  p->white_point_source = EVmax;

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->grey_point_source, p->grey_point_source);
  dt_bauhaus_slider_set_soft(g->black_point_source, p->black_point_source);
  dt_bauhaus_slider_set_soft(g->white_point_source, p->white_point_source);
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
  float prev_grey = p->grey_point_source;
  p->grey_point_source = dt_bauhaus_slider_get(slider);

  float grey_var = Log2(prev_grey / p->grey_point_source);
  p->black_point_source = p->black_point_source - grey_var;
  p->white_point_source = p->white_point_source + grey_var;

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->white_point_source, p->white_point_source);
  dt_bauhaus_slider_set_soft(g->black_point_source, p->black_point_source);
  darktable.gui->reset = 0;

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
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
  p->grey_point_target = dt_bauhaus_slider_get(slider);
  dt_iop_color_picker_reset(&g->color_picker, TRUE);
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

  dt_iop_color_picker_reset(&g->color_picker, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}

static void contrast_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
  p->contrast = dt_bauhaus_slider_get(slider);
  dt_iop_color_picker_reset(&g->color_picker, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}

static void saturation_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
  p->saturation = logf(9.0f * dt_bauhaus_slider_get(slider)/100.0 + 1.0f) / logf(10.0f) * 100.0f;
  dt_iop_color_picker_reset(&g->color_picker, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void white_point_target_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
  p->white_point_target = dt_bauhaus_slider_get(slider);
  dt_iop_color_picker_reset(&g->color_picker, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}

static void black_point_target_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
  p->black_point_target = dt_bauhaus_slider_get(slider);
  dt_iop_color_picker_reset(&g->color_picker, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}

static void output_power_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
  p->output_power = dt_bauhaus_slider_get(slider);
  dt_iop_color_picker_reset(&g->color_picker, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}

static void balance_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
  p->balance = dt_bauhaus_slider_get(slider);
  dt_iop_color_picker_reset(&g->color_picker, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}

static void interpolator_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
  dt_iop_color_picker_reset(&g->color_picker, TRUE);
  const int combo = dt_bauhaus_combobox_get(widget);

  switch (combo)
  {
    case CUBIC_SPLINE:
    {
      p->interpolator = CUBIC_SPLINE;
      break;
    }
    case CATMULL_ROM:
    {
      p->interpolator = CATMULL_ROM;
      break;
    }
    case MONOTONE_HERMITE:
    {
      p->interpolator = MONOTONE_HERMITE;
      break;
    }
    case 3:
    {
      p->interpolator = 3; // Optimized
      break;
    }
    default:
    {
      p->interpolator = CUBIC_SPLINE;
      break;
    }
  }

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}

static void preserve_color_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  p->preserve_color = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
  if(!in) dt_iop_color_picker_reset(&g->color_picker, TRUE);
}

void compute_curve_lut(dt_iop_filmic_params_t *p, float *table, float *table_temp, int res,
  dt_iop_filmic_data_t *d, dt_iop_filmic_nodes_t *nodes_data)
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
  const float black_display = CLAMP(p->black_point_target, 0.0f, p->grey_point_target) / 100.0f; // in %
  const float grey_display = powf(CLAMP(p->grey_point_target, p->black_point_target, p->white_point_target) / 100.0f, 1.0f / (p->output_power));
  const float white_display = CLAMP(p->white_point_target, p->grey_point_target, 100.0f)  / 100.0f; // in %

  const float latitude = CLAMP(p->latitude_stops, 0.01f, dynamic_range * 0.99f);
  const float balance = CLAMP(p->balance, -50.0f, 50.0f) / 100.0f; // in %

  const float contrast = p->contrast;

  // nodes for mapping from log encoding to desired target luminance
  // X coordinates
  float toe_log = grey_log - latitude/dynamic_range * fabsf(black_source/dynamic_range);
  float shoulder_log = grey_log + latitude/dynamic_range * white_source/dynamic_range;


  // interception
  float linear_intercept = grey_display - (contrast * grey_log);

  // y coordinates
  float toe_display = (toe_log * contrast + linear_intercept);
  float shoulder_display = (shoulder_log * contrast + linear_intercept);

  // Apply the highlights/shadows balance as a shift along the contrast slope
  const float norm = powf(powf(contrast, 2.0f) + 1.0f, 0.5f);

  // negative values drag to the left and compress the shadows, on the UI negative is the inverse
  const float coeff = -(dynamic_range - latitude) / dynamic_range * balance;

  toe_display += coeff * contrast /norm;
  shoulder_display += coeff * contrast /norm;
  toe_log += coeff /norm;
  shoulder_log += coeff /norm;

  // Sanitize pass 1
  toe_log = CLAMP(toe_log, 0.0f, grey_log);
  shoulder_log = CLAMP(shoulder_log, grey_log, 1.0f);
  toe_display = CLAMP(toe_display, black_display, grey_display);
  shoulder_display = CLAMP(shoulder_display, grey_display, white_display);

  /**
   * Now we have 3 segments :
   *  - x = [0.0 ; toe_log], curved part
   *  - x = [toe_log ; grey_log ; shoulder_log], linear part
   *  - x = [shoulder_log ; 1.0] curved part
   *
   * BUT : in case some nodes overlap, we need to remove them to avoid
   * degenerating of the curve
  **/

  // sanitize pass 2
  int TOE_LOST = FALSE;
  int SHOULDER_LOST = FALSE;

  if ((toe_log == grey_log && toe_display == grey_display) || (toe_log == 0.0f && toe_display  == black_display))
  {
    TOE_LOST = TRUE;
  }
  if ((shoulder_log == grey_log && shoulder_display == grey_display) || (shoulder_log == 1.0f && shoulder_display == white_display))
  {
    SHOULDER_LOST = TRUE;
  }

  // Build the curve from the nodes

  if (SHOULDER_LOST && !TOE_LOST)
  {
    // shoulder only broke - we remove it
    nodes_data->nodes = 4;
    nodes_data->x[0] = black_log;
    nodes_data->x[1] = toe_log;
    nodes_data->x[2] = grey_log;
    nodes_data->x[3] = white_log;

    nodes_data->y[0] = black_display;
    nodes_data->y[1] = toe_display;
    nodes_data->y[2] = grey_display;
    nodes_data->y[3] = white_display;

    if(d)
    {
      d->latitude_min = toe_log;
      d->latitude_max = white_log;
    }

    //dt_control_log(_("filmic curve using 4 nodes - highlights lost"));

  }
  else if (TOE_LOST && !SHOULDER_LOST)
  {
    // toe only broke - we remove it
    nodes_data->nodes = 4;

    nodes_data->x[0] = black_log;
    nodes_data->x[1] = grey_log;
    nodes_data->x[2] = shoulder_log;
    nodes_data->x[3] = white_log;

    nodes_data->y[0] = black_display;
    nodes_data->y[1] = grey_display;
    nodes_data->y[2] = shoulder_display;
    nodes_data->y[3] = white_display;

    if(d)
    {
      d->latitude_min = black_log;
      d->latitude_max = shoulder_log;
    }

    //dt_control_log(_("filmic curve using 4 nodes - shadows lost"));

  }
  else if (TOE_LOST && SHOULDER_LOST)
  {
    // toe and shoulder both broke - we remove them
    nodes_data->nodes = 3;

    nodes_data->x[0] = black_log;
    nodes_data->x[1] = grey_log;
    nodes_data->x[2] = white_log;

    nodes_data->y[0] = black_display;
    nodes_data->y[1] = grey_display;
    nodes_data->y[2] = white_display;

    if(d)
    {
      d->latitude_min = black_log;
      d->latitude_max = white_log;
    }

    //dt_control_log(_("filmic curve using 3 nodes - highlights & shadows lost"));

  }
  else
  {
    // everything OK
    nodes_data->nodes = 4;

    nodes_data->x[0] = black_log;
    nodes_data->x[1] = toe_log;
    //nodes_data->x[2] = grey_log,
    nodes_data->x[2] = shoulder_log;
    nodes_data->x[3] = white_log;

    nodes_data->y[0] = black_display;
    nodes_data->y[1] = toe_display;
    //nodes_data->y[2] = grey_display,
    nodes_data->y[2] = shoulder_display;
    nodes_data->y[3] = white_display;

    if(d)
    {
      d->latitude_min = toe_log;
      d->latitude_max = shoulder_log;
    }

    //dt_control_log(_("filmic curve using 5 nodes - everything alright"));
  }

  if (p->interpolator != 3)
  {
    // Compute the interpolation

    // Catch bad interpolators exceptions (errors in saved params)
    int interpolator = CUBIC_SPLINE;
    if (p->interpolator > CUBIC_SPLINE && p->interpolator <= MONOTONE_HERMITE) interpolator = p->interpolator;

    curve = dt_draw_curve_new(0.0, 1.0, interpolator);
    for(int k = 0; k < nodes_data->nodes; k++) (void)dt_draw_curve_add_point(curve, nodes_data->x[k], nodes_data->y[k]);

    // Compute the LUT
    dt_draw_curve_calc_values(curve, 0.0f, 1.0f, res, NULL, table);
    dt_draw_curve_destroy(curve);

  }
  else
  {
    // Compute the monotonic interpolation
    curve = dt_draw_curve_new(0.0, 1.0, MONOTONE_HERMITE);
    for(int k = 0; k < nodes_data->nodes; k++) (void)dt_draw_curve_add_point(curve, nodes_data->x[k], nodes_data->y[k]);
    dt_draw_curve_calc_values(curve, 0.0f, 1.0f, res, NULL, table_temp);
    dt_draw_curve_destroy(curve);

    // Compute the cubic spline interpolation
    curve = dt_draw_curve_new(0.0, 1.0, CUBIC_SPLINE);
    for(int k = 0; k < nodes_data->nodes; k++) (void)dt_draw_curve_add_point(curve, nodes_data->x[k], nodes_data->y[k]);
    dt_draw_curve_calc_values(curve, 0.0f, 1.0f, res, NULL, table);
    dt_draw_curve_destroy(curve);

    // Average both LUT
#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) shared(table, table_temp, res) schedule(static)
#endif
    for(int k = 0; k < res; k++) table[k] = (table[k] + table_temp[k]) / 2.0f;
  }

}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)p1;
  dt_iop_filmic_data_t *d = (dt_iop_filmic_data_t *)piece->data;

  d->preserve_color = p->preserve_color;

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

  // commitproducts with no low-pass filter, you will increase the contrast of nois
  d->dynamic_range = dynamic_range;
  d->black_source = black_source;
  d->grey_source = grey_source;
  d->output_power = p->output_power;
  d->saturation = p->saturation;
  d->contrast = contrast;

  // compute the curves and their LUT
  dt_iop_filmic_nodes_t *nodes_data = (dt_iop_filmic_nodes_t *)malloc(sizeof(dt_iop_filmic_nodes_t));
  compute_curve_lut(p, d->table, d->table_temp, 0x10000, d, nodes_data);
  free(nodes_data);
  nodes_data = NULL;

  // Build a window function based on the log.
  // This will be used to selectively desaturate the non-linear parts
  // to avoid over-saturation in the toe and shoulder.

  const float latitude = d->latitude_max - d->latitude_min;
  const float center = (d->latitude_max + d->latitude_min)/2.0f;
  const float saturation = d->saturation / 100.0f;
  const float sigma = saturation * saturation * latitude * latitude;

#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) shared(d) schedule(static)
#endif
  for(int k = 0; k < 65536; k++)
  {
    const float x = ((float)k) / 65536.0f;
    if (sigma != 0.0f)
    {
      d->grad_2[k] = expf(-0.5f * (center - x) * (center - x) / sigma);
    }
    else
    {
      d->grad_2[k] = 0.0f;
    }
  }

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

  dt_bauhaus_slider_set_soft(g->white_point_source, p->white_point_source);
  dt_bauhaus_slider_set_soft(g->grey_point_source, p->grey_point_source);
  dt_bauhaus_slider_set_soft(g->black_point_source, p->black_point_source);
  dt_bauhaus_slider_set_soft(g->security_factor, p->security_factor);
  dt_bauhaus_slider_set_soft(g->white_point_target, p->white_point_target);
  dt_bauhaus_slider_set_soft(g->grey_point_target, p->grey_point_target);
  dt_bauhaus_slider_set_soft(g->black_point_target, p->black_point_target);
  dt_bauhaus_slider_set_soft(g->output_power, p->output_power);
  dt_bauhaus_slider_set_soft(g->latitude_stops, p->latitude_stops);
  dt_bauhaus_slider_set(g->contrast, p->contrast);
  dt_bauhaus_slider_set(g->saturation, (powf(10.0f, p->saturation/100.0f) - 1.0f) / 9.0f * 100.0f);
  dt_bauhaus_slider_set(g->balance, p->balance);

  dt_bauhaus_combobox_set(g->interpolator, p->interpolator);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->preserve_color), p->preserve_color);

  dtgtk_expander_set_expanded(DTGTK_EXPANDER(g->extra_expander),
                              gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->extra_toggle)));

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
                                 .black_point_source  = -8.65,  // source black
                                 .white_point_source  = 2.45,  // source white
                                 .security_factor     = 0.0,  // security factor
                                 .grey_point_target   = 18.0, // target grey
                                 .black_point_target  = 0.0,  // target black
                                 .white_point_target  = 100.0,  // target white
                                 .output_power        = 2.2,  // target power (~ gamma)
                                 .latitude_stops      = 2.0,  // intent latitude
                                 .contrast            = 1.5,  // intent contrast
                                 .saturation          = 100.0,   // intent saturation
                                 .balance             = 0.0, // balance shadows/highlights
                                 .interpolator        = CUBIC_SPLINE, //interpolator
                                 .preserve_color      = 0, // run the saturated variant
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
  dtgtk_expander_set_expanded(DTGTK_EXPANDER(g->extra_expander), FALSE);
  dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(g->extra_toggle), dtgtk_cairo_paint_solid_arrow,
                               CPF_DO_NOT_USE_BORDER | CPF_STYLE_BOX | CPF_DIRECTION_LEFT, NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->extra_toggle), FALSE);
}

static gboolean dt_iop_tonecurve_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_filmic_gui_data_t *c = (dt_iop_filmic_gui_data_t *)self->gui_data;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  dt_iop_filmic_nodes_t *nodes_data = (dt_iop_filmic_nodes_t *)malloc(sizeof(dt_iop_filmic_nodes_t));
  compute_curve_lut(p, c->table, c->table_temp, 256, NULL, nodes_data);

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

  // solve the equations for the rescaling parameters
  const float DR = (p->white_point_source - p->black_point_source);
  const float grey = -p->black_point_source / DR;
  int rescale = FALSE;

  float a, b, d;
  a = DR;
  b = Log2( 1.0f / (-1 + powf(2.0f, a)));
  d = - powf(2.0f, b);

  if (grey > powf(p->grey_point_target / 100.0f, p->output_power))
  {
    // The x-coordinate rescaling is valid only when the log grey value (dynamic range center)
    // is greater or equal to the destination grey value
    rescale = TRUE;

    for (int i = 0; i < 50; ++i)
    { // Optimization loop for the non-linear problem
      a = Log2((0.5f - d) / (1.0f - d)) / (grey - 1.0f);
      b = Log2( 1.0f / (-1 + powf(2.0f, a)));
      d = - powf(2.0f, b);
    }
  }

  const float gamma = (logf(p->grey_point_target / 100.0f) / logf(0.5f)) / p->output_power;

  // draw nodes
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);

  for(int k = 0; k < nodes_data->nodes; k++)
  {
    /*
     * Use double precision locally to avoid cancellation effect on
     * the "+ d" operation.
     */
    const float x = (rescale) ? pow(2.0, (double)a * nodes_data->x[k] + b) + d : nodes_data->x[k];
    const float y = powf(nodes_data->y[k], 1.0f / gamma);

    cairo_arc(cr, x * width, (1.0 - y) * (double)height, DT_PIXEL_APPLY_DPI(3), 0, 2. * M_PI);
    cairo_stroke_preserve(cr);
    cairo_fill(cr);
    cairo_stroke(cr);
  }
  free(nodes_data);
  nodes_data = NULL;

  // draw curve
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
  cairo_set_source_rgb(cr, .9, .9, .9);
  cairo_move_to(cr, 0, height * (1.0 - c->table[0]));

  for(int k = 1; k < 256; k++)
  {
    /*
     * Use double precision locally to avoid cancellation effect on
     * the "+ d" operation.
     */
    const float x = (rescale) ? pow(2.0, (double)a * k / 255.0 + b) + d : k / 255.0;
    const float y = powf(c->table[k], 1.0f / gamma);
    cairo_line_to(cr, x * width, (double)height * (1.0 - y));
  }
  cairo_stroke(cr);
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

static void _extra_options_button_changed(GtkDarktableToggleButton *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
  const gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->extra_toggle));
  dtgtk_expander_set_expanded(DTGTK_EXPANDER(g->extra_expander), active);
  dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(g->extra_toggle), dtgtk_cairo_paint_solid_arrow,
                               CPF_DO_NOT_USE_BORDER | CPF_STYLE_BOX | (active?CPF_DIRECTION_DOWN:CPF_DIRECTION_LEFT), NULL);
}

void gui_init(dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_filmic_gui_data_t));
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  const int margin_width = (int)dt_conf_get_int("panel_width") / 10.0;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));

  g->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(1.0));
  // Add after darktable 2.6
  // dt_bauhaus_widget_set_label(GTK_WIDGET(g->area), _("read-only graph, use the parameters below to set the nodes"));
  gtk_widget_set_margin_start(GTK_WIDGET(g->area), margin_width);
  gtk_widget_set_margin_end(GTK_WIDGET(g->area), margin_width);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->area), FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->area), "draw", G_CALLBACK(dt_iop_tonecurve_draw), self);

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("logarithmic shaper")), FALSE, FALSE, 5);

  // grey_point_source slider
  g->grey_point_source = dt_bauhaus_slider_new_with_range(self, 0.1, 36., 0.1, p->grey_point_source, 2);
  dt_bauhaus_slider_enable_soft_boundaries(g->grey_point_source, 0.0, 100.0);
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
  g->white_point_source = dt_bauhaus_slider_new_with_range(self, 2.0, 8.0, 0.1, p->white_point_source, 2);
  dt_bauhaus_slider_enable_soft_boundaries(g->white_point_source, 0.0, 16.0);
  dt_bauhaus_widget_set_label(g->white_point_source, NULL, _("white relative exposure"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->white_point_source, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->white_point_source, "%.2f EV");
  gtk_widget_set_tooltip_text(g->white_point_source, _("number of stops between middle grey and pure white\nthis is a reading a posemeter would give you on the scene"));
  g_signal_connect(G_OBJECT(g->white_point_source), "value-changed", G_CALLBACK(white_point_source_callback), self);
  dt_bauhaus_widget_set_quad_paint(g->white_point_source, dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->white_point_source, TRUE);
  g_signal_connect(G_OBJECT(g->white_point_source), "quad-pressed", G_CALLBACK(dt_iop_color_picker_callback), &g->color_picker);

  // Black slider
  g->black_point_source = dt_bauhaus_slider_new_with_range(self, -14.0, -3.0, 0.1, p->black_point_source, 2);
  dt_bauhaus_slider_enable_soft_boundaries(g->black_point_source, -16.0, -0.1);
  dt_bauhaus_widget_set_label(g->black_point_source, NULL, _("black relative exposure"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->black_point_source, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->black_point_source, "%.2f EV");
  gtk_widget_set_tooltip_text(g->black_point_source, _("number of stops between middle grey and pure black\nthis is a reading a posemeter would give you on the scene"));
  g_signal_connect(G_OBJECT(g->black_point_source), "value-changed", G_CALLBACK(black_point_source_callback), self);
  dt_bauhaus_widget_set_quad_paint(g->black_point_source, dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->black_point_source, TRUE);
  g_signal_connect(G_OBJECT(g->black_point_source), "quad-pressed", G_CALLBACK(dt_iop_color_picker_callback), &g->color_picker);

  // Security factor
  g->security_factor = dt_bauhaus_slider_new_with_range(self, -50., 50., 1.0, p->security_factor, 2);
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
  g->contrast = dt_bauhaus_slider_new_with_range(self, 1., 2., 0.01, p->contrast, 3);
  dt_bauhaus_slider_enable_soft_boundaries(g->contrast, 0.0, 5.0);
  dt_bauhaus_widget_set_label(g->contrast, NULL, _("contrast"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->contrast, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->contrast, _("slope of the linear part of the curve"));
  g_signal_connect(G_OBJECT(g->contrast), "value-changed", G_CALLBACK(contrast_callback), self);

  // latitude slider
  g->latitude_stops = dt_bauhaus_slider_new_with_range(self, 2., 8.0, 0.05, p->latitude_stops, 3);
  dt_bauhaus_slider_enable_soft_boundaries(g->latitude_stops, 0.01, 16.0);
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
  g->saturation = dt_bauhaus_slider_new_with_range(self, 0., 200., 0.5, (powf(10.0f, p->saturation/100.0f) - 1.0f) / 9.0f *100.0f, 2);
  dt_bauhaus_widget_set_label(g->saturation, NULL, _("saturation"));
  dt_bauhaus_slider_enable_soft_boundaries(g->saturation, 0.0, 1000.0);
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

  // Preserve color
  g->preserve_color = gtk_check_button_new_with_label(_("preserve the chrominance"));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON(g->preserve_color), p->preserve_color);
  gtk_widget_set_tooltip_text(g->preserve_color, _("ensure the original color are preserved\n"
                                                   "may produce over-saturated colors and reinforce lens chromatic aberrations"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->preserve_color , TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->preserve_color), "toggled", G_CALLBACK(preserve_color_callback), self);


  // add collapsable section for those extra options that are generally not to be used

  GtkWidget *destdisp_head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_BAUHAUS_SPACE);
  GtkWidget *destdisp = dt_ui_section_label_new(_("destination/display"));
  g->extra_toggle = dtgtk_togglebutton_new(dtgtk_cairo_paint_solid_arrow, CPF_DO_NOT_USE_BORDER | CPF_STYLE_BOX | CPF_DIRECTION_LEFT, NULL);
  gtk_widget_set_size_request(g->extra_toggle,  DT_PIXEL_APPLY_DPI(15), DT_PIXEL_APPLY_DPI(15));
  GtkWidget *extra_options = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(destdisp_head), destdisp, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(destdisp_head), g->extra_toggle, FALSE, FALSE, 0);
  gtk_widget_set_visible(extra_options, FALSE);
  g->extra_expander = dtgtk_expander_new(destdisp_head, extra_options);
  dtgtk_expander_set_expanded(DTGTK_EXPANDER(g->extra_expander), TRUE);
  gtk_box_pack_start(GTK_BOX(self->widget), g->extra_expander, FALSE, FALSE, 0);

  g_signal_connect(G_OBJECT(g->extra_toggle), "toggled", G_CALLBACK(_extra_options_button_changed),  (gpointer)self);

  // Black slider
  g->black_point_target = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 1, p->black_point_target, 2);
  dt_bauhaus_widget_set_label(g->black_point_target, NULL, _("black luminance"));
  gtk_box_pack_start(GTK_BOX(extra_options), g->black_point_target, FALSE, FALSE, 0);
  dt_bauhaus_slider_set_format(g->black_point_target, "%.2f %%");
  gtk_widget_set_tooltip_text(g->black_point_target, _("luminance of output pure black, "
                                                        "this should be 0%\nexcept if you want a faded look"));
  g_signal_connect(G_OBJECT(g->black_point_target), "value-changed", G_CALLBACK(black_point_target_callback), self);

  // grey_point_source slider
  g->grey_point_target = dt_bauhaus_slider_new_with_range(self, 0.1, 50., 0.5, p->grey_point_target, 2);
  dt_bauhaus_widget_set_label(g->grey_point_target, NULL, _("middle grey destination"));
  gtk_box_pack_start(GTK_BOX(extra_options), g->grey_point_target, FALSE, FALSE, 0);
  dt_bauhaus_slider_set_format(g->grey_point_target, "%.2f %%");
  gtk_widget_set_tooltip_text(g->grey_point_target, _("midde grey value of the target display or color space.\n"
                                                      "you should never touch that unless you know what you are doing."));
  g_signal_connect(G_OBJECT(g->grey_point_target), "value-changed", G_CALLBACK(grey_point_target_callback), self);

  // White slider
  g->white_point_target = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 1., p->white_point_target, 2);
  dt_bauhaus_widget_set_label(g->white_point_target, NULL, _("white luminance"));
  gtk_box_pack_start(GTK_BOX(extra_options), g->white_point_target, FALSE, FALSE, 0);
  dt_bauhaus_slider_set_format(g->white_point_target, "%.2f %%");
  gtk_widget_set_tooltip_text(g->white_point_target, _("luminance of output pure white, "
                                                        "this should be 100%\nexcept if you want a faded look"));
  g_signal_connect(G_OBJECT(g->white_point_target), "value-changed", G_CALLBACK(white_point_target_callback), self);

  // power/gamma slider
  g->output_power = dt_bauhaus_slider_new_with_range(self, 1.0, 2.4, 0.1, p->output_power, 2);
  dt_bauhaus_widget_set_label(g->output_power, NULL, _("destination power factor"));
  gtk_box_pack_start(GTK_BOX(extra_options), g->output_power, FALSE, FALSE, 0);
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
