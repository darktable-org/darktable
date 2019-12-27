/*
   This file is part of darktable,
   copyright (c) 2018-2019 Aurélien Pierre, with guidance of Troy James Sobotka.

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
#include "iop/gaussian_elimination.h"


#include "develop/imageop.h"
#include "gui/draw.h"
#include "libs/colorpicker.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>


#define DT_GUI_CURVE_EDITOR_INSET DT_PIXEL_APPLY_DPI(1)


DT_MODULE_INTROSPECTION(1, dt_iop_filmicrgb_params_t)

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
 * space. ProPhotoRGB has been chosen for its wide gamut coverage and
 * for conveniency because it's already in darktable's libs. Any other
 * RGB working space could work. This chouice could (should) also be
 * exposed to the user.
 *
 * The filmic curves are tonecurves intended to simulate the luminance
 * transfer function of film with "S" curves. These could be reproduced in
 * the tonecurve.c IOP, however what we offer here is a parametric
 * interface useful to remap accurately and promptly the middle grey
 * to any arbitrary value chosen accordingly to the destination space.
 *
 * The combined use of both define a modern way to deal with large
 * dynamic range photographs by remapping the values with a comprehensive
 * interface avoiding many of the back and forth adjustments darktable
 * is prone to enforce.
 *
 * */


 /** Note :
 * we use finite-math-only and fast-math because divisions by zero are manually avoided in the code
 * fp-contract=fast enables hardware-accelerated Fused Multiply-Add
 * the rest is loop reorganization and vectorization optimization
 **/
#if defined(__GNUC__)
#pragma GCC optimize ("unroll-loops", "tree-loop-if-convert", \
                      "tree-loop-distribution", "no-strict-aliasing", \
                      "loop-interchange", "loop-nest-optimize", "tree-loop-im", \
                      "unswitch-loops", "tree-loop-ivcanon", "ira-loop-pressure", \
                      "split-ivs-in-unroller", "variable-expansion-in-unroller", \
                      "split-loops", "ivopts", "predictive-commoning",\
                      "tree-loop-linear", "loop-block", "loop-strip-mine", \
                      "finite-math-only", "fp-contract=fast", "fast-math")
#endif

typedef enum dt_iop_filmicrgb_pickcolor_type_t
{
  DT_PICKPROFLOG_NONE = 0,
  DT_PICKPROFLOG_GREY_POINT = 1,
  DT_PICKPROFLOG_BLACK_POINT = 2,
  DT_PICKPROFLOG_WHITE_POINT = 3,
  DT_PICKPROFLOG_AUTOTUNE = 4
} dt_iop_filmicrgb_pickcolor_type_t;


typedef enum dt_iop_filmicrgb_methods_type_t
{
  DT_FILMIC_METHOD_NONE = 0,
  DT_FILMIC_METHOD_MAX_RGB = 1,
  DT_FILMIC_METHOD_LUMINANCE = 2,
  DT_FILMIC_METHOD_POWER_NORM = 3
} dt_iop_filmicrgb_methods_type_t;


typedef struct dt_iop_filmic_rgb_spline_t
{
  float DT_ALIGNED_PIXEL M1[4], M2[4], M3[4], M4[4], M5[4]; // factors for the interpolation polynom
  float latitude_min, latitude_max;                         // bounds of the latitude == linear part by design
  float y[5];                                               // controls nodes
  float x[5];                                               // controls nodes
} dt_iop_filmic_rgb_spline_t;


typedef struct dt_iop_filmicrgb_params_t
{
  float grey_point_source;
  float black_point_source;
  float white_point_source;
  float security_factor;
  float grey_point_target;
  float black_point_target;
  float white_point_target;
  float output_power;
  float latitude;
  float contrast;
  float saturation;
  float balance;
  int preserve_color;
} dt_iop_filmicrgb_params_t;

typedef struct dt_iop_filmicrgb_gui_data_t
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
  GtkWidget *latitude;
  GtkWidget *contrast;
  GtkWidget *saturation;
  GtkWidget *balance;
  GtkWidget *preserve_color;
  GtkNotebook *notebook;
  dt_iop_color_picker_t color_picker;
  GtkDrawingArea *area;
  struct dt_iop_filmic_rgb_spline_t spline DT_ALIGNED_ARRAY;
} dt_iop_filmicrgb_gui_data_t;

typedef struct dt_iop_filmicrgb_data_t
{
  float max_grad;
  float grey_source;
  float black_source;
  float dynamic_range;
  float saturation;
  float output_power;
  float contrast;
  float sigma_toe, sigma_shoulder;
  int preserve_color;
  struct dt_iop_filmic_rgb_spline_t spline DT_ALIGNED_ARRAY;
} dt_iop_filmicrgb_data_t;


typedef struct dt_iop_filmicrgb_global_data_t
{
  int kernel_filmic_rgb_split;
  int kernel_filmic_rgb_chroma;
} dt_iop_filmicrgb_global_data_t;


const char *name()
{
  return _("filmic rgb");
}

int default_group()
{
  return IOP_GROUP_TONE;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_rgb;
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_filmicrgb_params_t p;
  memset(&p, 0, sizeof(p));

  // Output
  p.output_power = 2.44f;
  p.white_point_target = 100.0f;
  p.black_point_target = 0.0f;
  p.grey_point_target = 18.45f;

  // Input - standard raw picture
  p.contrast = 1.4f;
  p.preserve_color = DT_FILMIC_METHOD_MAX_RGB;
  p.balance = 0.0f;
  p.saturation = 0.0f;
  p.latitude = 15.0f;
  p.security_factor = 22.4f;

  // Presets low-key
  p.grey_point_source = 18.45f;
  p.white_point_source = 3.5f;
  p.black_point_source = -3.5f;
  dt_gui_presets_add_generic(_("07 EV dynamic range"), self->op, self->version(), &p, sizeof(p), 1);

  // Presets indoors
  p.grey_point_source = 9.225f;
  p.white_point_source = 4.5f;
  p.black_point_source = -4.5f;
  dt_gui_presets_add_generic(_("09 EV dynamic range"), self->op, self->version(), &p, sizeof(p), 1);

  // Presets dim-outdoors
  p.grey_point_source = 4.6125f;
  p.white_point_source = 5.5f;
  p.black_point_source = -5.5f;
  dt_gui_presets_add_generic(_("11 EV dynamic range"), self->op, self->version(), &p, sizeof(p), 1);

  // Presets outdoors
  p.grey_point_source = 2.30625f;
  p.white_point_source = 6.5f;
  p.black_point_source = -6.5f;
  dt_gui_presets_add_generic(_("13 EV dynamic range"), self->op, self->version(), &p, sizeof(p), 1);

  // Presets backlighting
  p.grey_point_source = 1.153125f;
  p.white_point_source = 7.5f;
  p.black_point_source = -7.5f;
  dt_gui_presets_add_generic(_("15 EV (backlighting)"), self->op, self->version(), &p, sizeof(p), 1);

  // Presets HDR
  p.grey_point_source = 0.5765625f;
  p.white_point_source = 8.5f;
  p.black_point_source = -8.5f;
  dt_gui_presets_add_generic(_("17 EV (HDR)"), self->op, self->version(), &p, sizeof(p), 1);
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "white exposure"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "black exposure"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "middle grey luminance"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "dynamic range scaling"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "contrast"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "latitude"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "shadows highlights balance"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "extreme luminance saturation"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "target black luminance"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "target middle grey"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "target white luminance"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "target power transfer function"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  dt_accel_connect_slider_iop(self, "white exposure", GTK_WIDGET(g->white_point_source));
  dt_accel_connect_slider_iop(self, "black exposure", GTK_WIDGET(g->black_point_source));
  dt_accel_connect_slider_iop(self, "middle grey luminance", GTK_WIDGET(g->grey_point_source));
  dt_accel_connect_slider_iop(self, "dynamic range scaling", GTK_WIDGET(g->security_factor));
  dt_accel_connect_slider_iop(self, "contrast", GTK_WIDGET(g->contrast));
  dt_accel_connect_slider_iop(self, "latitude", GTK_WIDGET(g->latitude));
  dt_accel_connect_slider_iop(self, "shadows highlights balance", GTK_WIDGET(g->balance));
  dt_accel_connect_slider_iop(self, "extreme luminance saturation", GTK_WIDGET(g->saturation));
  dt_accel_connect_slider_iop(self, "target black luminance", GTK_WIDGET(g->black_point_target));
  dt_accel_connect_slider_iop(self, "target middle grey", GTK_WIDGET(g->grey_point_target));
  dt_accel_connect_slider_iop(self, "target white luminance", GTK_WIDGET(g->white_point_target));
  dt_accel_connect_slider_iop(self, "target power transfer function", GTK_WIDGET(g->output_power));
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float clamp_simd(const float x)
{
  return fminf(fmaxf(x, 0.0f), 1.0f);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(pixel:16)
#endif
static inline float pixel_rgb_norm_power(const float pixel[4])
{
  // weird norm sort of perceptual. This is black magic really, but it looks good.

  float numerator = 0.0f;
  float denominator = 0.0f;

#ifdef _OPENMP
#pragma omp simd aligned(pixel:16) reduction(+:numerator, denominator)
#endif
  for(int c = 0; c < 3; c++)
  {
    const float value = fabsf(pixel[c]);
    const float RGB_square = value * value;
    const float RGB_cubic = RGB_square * value;
    numerator += RGB_cubic;
    denominator += RGB_square;
  }

  return numerator / denominator;
}


#ifdef _OPENMP
#pragma omp declare simd aligned(pixel:16)
#endif
static inline float get_pixel_norm(const float pixel[4], const dt_iop_filmicrgb_methods_type_t variant,
                                   const dt_iop_order_iccprofile_info_t *const work_profile)
{
  switch(variant)
  {
    case(DT_FILMIC_METHOD_MAX_RGB):
      return fmaxf(fmaxf(pixel[0], pixel[1]), pixel[2]);

    case(DT_FILMIC_METHOD_LUMINANCE):
      return (work_profile) ? dt_ioppr_get_rgb_matrix_luminance(pixel,
                                                                work_profile->matrix_in,
                                                                work_profile->lut_in,
                                                                work_profile->unbounded_coeffs_in,
                                                                work_profile->lutsize,
                                                                work_profile->nonlinearlut)
                            : dt_camera_rgb_luminance(pixel);

    case(DT_FILMIC_METHOD_POWER_NORM):
      return pixel_rgb_norm_power(pixel);

    default:
      return (work_profile) ? dt_ioppr_get_rgb_matrix_luminance(pixel,
                                                                work_profile->matrix_in,
                                                                work_profile->lut_in,
                                                                work_profile->unbounded_coeffs_in,
                                                                work_profile->lutsize,
                                                                work_profile->nonlinearlut)
                            : dt_camera_rgb_luminance(pixel);
  }
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float log_tonemapping(const float x, const float grey, const float black, const float dynamic_range)
{
  const float temp = (log2f(x / grey) - black) / dynamic_range;
  return fmaxf(fminf(temp, 1.0f), 1.52587890625e-05f);
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float filmic_spline(const float x,
                                  const float M1[4], const float M2[4], const float M3[4], const float M4[4], const float M5[4],
                                  const float latitude_min, const float latitude_max)
{
#ifdef _OPENMP
  // use OpenMP vector reduction with mask
  const int toe = (x < latitude_min);
  const int shoulder = (x > latitude_max);
  const int latitude = (toe == shoulder); // == FALSE
  const int mask[4] DT_ALIGNED_PIXEL = { toe, shoulder, latitude, 0 };

  float sum = 0.0f;

#pragma omp simd reduction(+:sum) aligned(M1, M2, M3, M4, M5, mask:16)
  for(int i = 0; i < 3; ++i) sum += mask[i] * (M1[i] + x * (M2[i] + x * (M3[i] + x * (M4[i] + x * M5[i]))));
  return sum;

#else
  // use inline checks and hope for some clever stuff from the compiler
  return (x < latitude_min) ? M1[0] + x * (M2[0] + x * (M3[0] + x * (M4[0] + x * M5[0]))) : // toe
         (x > latitude_max) ? M1[1] + x * (M2[1] + x * (M3[1] + x * (M4[1] + x * M5[1]))) : // shoulder
                              M1[2] + x * (M2[2] + x * (M3[2] + x * (M4[2] + x * M5[2])));  // latitude
#endif
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float filmic_desaturate(const float x, const float sigma_toe, const float sigma_shoulder, const float saturation)
{
  const float radius_toe = x;
  const float radius_shoulder = 1.0f - x;

  const float key_toe = expf(-0.5f * radius_toe * radius_toe / sigma_toe);
  const float key_shoulder = expf(-0.5f * radius_shoulder * radius_shoulder / sigma_shoulder);

  return 1.0f - clamp_simd((key_toe + key_shoulder) / saturation) ;
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float linear_saturation(const float x, const float luminance, const float saturation)
{
  return luminance + saturation * (x - luminance);
}


void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const restrict ivoid, void *const restrict ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_filmicrgb_data_t *const data = (dt_iop_filmicrgb_data_t *)piece->data;
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);

  const int ch = piece->colors;

  /** The log2(x) -> -INF when x -> 0
  * thus very low values (noise) will get even lower, resulting in noise negative amplification,
  * which leads to pepper noise in shadows. To avoid that, we need to clip values that are noise for sure.
  * Using 16 bits RAW data, the black value (known by rawspeed for every manufacturer) could be used as a threshold.
  * However, at this point of the pixelpipe, the RAW levels have already been corrected and everything can happen with black levels
  * in the exposure module. So we define the threshold as the first non-null 16 bit integer
  */
 ;

  const float *const restrict in = (float *)ivoid;
  float *const restrict out = (float *)ovoid;

  const int variant = data->preserve_color;
  const dt_iop_filmic_rgb_spline_t spline = (dt_iop_filmic_rgb_spline_t)data->spline;

  if(variant == DT_FILMIC_METHOD_NONE) // no chroma preservation
  {
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(ch, data, in, out, roi_out, work_profile, spline) \
  schedule(simd:static) aligned(in, out:64)
#endif
    for(size_t k = 0; k < roi_out->height * roi_out->width * ch; k += ch)
    {
      const float *const pix_in = in + k;
      float *const pix_out = out + k;
      float DT_ALIGNED_PIXEL temp[4];

      // Log tone-mapping
      for(int c = 0; c < 3; c++)
        temp[c] = log_tonemapping((pix_in[c] < 1.52587890625e-05f) ? 1.52587890625e-05f : pix_in[c],
                                   data->grey_source, data->black_source, data->dynamic_range);

      // Get the desaturation coeff based on the log value
      const float lum = (work_profile) ? dt_ioppr_get_rgb_matrix_luminance(temp,
                                                                           work_profile->matrix_in,
                                                                           work_profile->lut_in,
                                                                           work_profile->unbounded_coeffs_in,
                                                                           work_profile->lutsize,
                                                                           work_profile->nonlinearlut)
                                        : dt_camera_rgb_luminance(temp);
      const float desaturation = filmic_desaturate(lum, data->sigma_toe, data->sigma_shoulder, data->saturation);

      // Desaturate on the non-linear parts of the curve
      // Filmic S curve on the max RGB
      // Apply the transfer function of the display
      for(int c = 0; c < 3; c++)
        pix_out[c] = powf(clamp_simd(filmic_spline(linear_saturation(temp[c], lum, desaturation), spline.M1, spline.M2, spline.M3, spline.M4, spline.M5, spline.latitude_min, spline.latitude_max)), data->output_power);

    }
  }
  else // chroma preservation
  {
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(ch, data, in, out, roi_out, work_profile, variant, spline) \
  schedule(simd:static) aligned(in, out:64)
#endif
    for(size_t k = 0; k < roi_out->height * roi_out->width * ch; k += ch)
    {
      const float *const pix_in = in + k;
      float *const pix_out = out + k;

      float DT_ALIGNED_PIXEL ratios[4];
      float norm = get_pixel_norm(pix_in, variant, work_profile);

      norm = (norm < 1.52587890625e-05f) ? 1.52587890625e-05f : norm; // norm can't be < to 2^(-16)

      // Save the ratios
      for(int c = 0; c < 3; c++) ratios[c] = pix_in[c] / norm;

      // Sanitize the ratios
      const float min_ratios = fminf(fminf(ratios[0], ratios[1]), ratios[2]);
      if(min_ratios < 0.0f) for(int c = 0; c < 3; c++) ratios[c] -= min_ratios;

      // Log tone-mapping
      norm = log_tonemapping(norm, data->grey_source, data->black_source, data->dynamic_range);

      // Get the desaturation value based on the log value
      const float desaturation = filmic_desaturate(norm, data->sigma_toe, data->sigma_shoulder, data->saturation);
       
      for(int c = 0; c < 3; c++) ratios[c] *= norm;

      const float lum = (work_profile) ? dt_ioppr_get_rgb_matrix_luminance(ratios,
                                                                           work_profile->matrix_in,
                                                                           work_profile->lut_in,
                                                                           work_profile->unbounded_coeffs_in,
                                                                           work_profile->lutsize,
                                                                           work_profile->nonlinearlut)
                                        : dt_camera_rgb_luminance(ratios);

      // Desaturate on the non-linear parts of the curve and save ratios
      for(int c = 0; c < 3; c++) ratios[c] = linear_saturation(ratios[c], lum, desaturation) / norm;

      // Filmic S curve on the max RGB
      // Apply the transfer function of the display
      norm = powf(clamp_simd(filmic_spline(norm, spline.M1, spline.M2, spline.M3, spline.M4, spline.M5, spline.latitude_min, spline.latitude_max)), data->output_power);

      // Re-apply ratios
      for(int c = 0; c < 3; c++) pix_out[c] = ratios[c] * norm;
    }
  }

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
    dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_filmicrgb_data_t *const d = (dt_iop_filmicrgb_data_t *)piece->data;
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);
  dt_iop_filmicrgb_global_data_t *const gd = (dt_iop_filmicrgb_global_data_t *)self->global_data;
  const dt_iop_filmic_rgb_spline_t spline = (dt_iop_filmic_rgb_spline_t)d->spline;

  cl_int err = -999;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const int use_work_profile = (work_profile == NULL) ? 0 : 1;

  cl_mem dev_profile_info = NULL;
  cl_mem dev_profile_lut = NULL;
  dt_colorspaces_iccprofile_info_cl_t *profile_info_cl;
  cl_float *profile_lut_cl = NULL;

  err = dt_ioppr_build_iccprofile_params_cl(work_profile, devid, &profile_info_cl, &profile_lut_cl,
                                            &dev_profile_info, &dev_profile_lut);
  if(err != CL_SUCCESS) goto error;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };

  if(d->preserve_color == DT_FILMIC_METHOD_NONE)
  {
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 4, sizeof(float), (void *)&d->dynamic_range);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 5, sizeof(float), (void *)&d->black_source);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 6, sizeof(float), (void *)&d->grey_source);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 7, sizeof(cl_mem), (void *)&dev_profile_info);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 8, sizeof(cl_mem), (void *)&dev_profile_lut);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 9, sizeof(int), (void *)&use_work_profile);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 10, sizeof(float), (void *)&d->sigma_toe);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 11, sizeof(float), (void *)&d->sigma_shoulder);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 12, sizeof(float), (void *)&d->saturation);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 13, 4 * sizeof(float), (void *)&spline.M1);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 14, 4 * sizeof(float), (void *)&spline.M2);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 15, 4 * sizeof(float), (void *)&spline.M3);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 16, 4 * sizeof(float), (void *)&spline.M4);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 17, 4 * sizeof(float), (void *)&spline.M5);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 18, sizeof(float), (void *)&spline.latitude_min);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 19, sizeof(float), (void *)&spline.latitude_max);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 20, sizeof(float), (void *)&d->output_power);

    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_rgb_split, sizes);
    dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl, &dev_profile_info, &dev_profile_lut);
    if(err != CL_SUCCESS) goto error;
    return TRUE;
  }
  else
  {
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 4, sizeof(float), (void *)&d->dynamic_range);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 5, sizeof(float), (void *)&d->black_source);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 6, sizeof(float), (void *)&d->grey_source);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 7, sizeof(cl_mem), (void *)&dev_profile_info);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 8, sizeof(cl_mem), (void *)&dev_profile_lut);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 9, sizeof(int), (void *)&use_work_profile);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 10, sizeof(float), (void *)&d->sigma_toe);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 11, sizeof(float), (void *)&d->sigma_shoulder);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 12, sizeof(float), (void *)&d->saturation);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 13, 4 * sizeof(float), (void *)&spline.M1);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 14, 4 * sizeof(float), (void *)&spline.M2);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 15, 4 * sizeof(float), (void *)&spline.M3);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 16, 4 * sizeof(float), (void *)&spline.M4);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 17, 4 * sizeof(float), (void *)&spline.M5);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 18, sizeof(float), (void *)&spline.latitude_min);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 19, sizeof(float), (void *)&spline.latitude_max);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 20, sizeof(float), (void *)&d->output_power);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 21, sizeof(int), (void *)&d->preserve_color);

    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_rgb_chroma, sizes);
    dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl, &dev_profile_info, &dev_profile_lut);
    if(err != CL_SUCCESS) goto error;
    return TRUE;
  }
error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_filmicrgb] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif


static void apply_auto_grey(dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  const dt_iop_order_iccprofile_info_t *const work_profile
        = dt_ioppr_get_iop_work_profile_info(self, self->dev->iop);
  const float grey = get_pixel_norm(self->picked_color, p->preserve_color, work_profile) / 2.0f;

  const float prev_grey = p->grey_point_source;
  p->grey_point_source = CLAMP(100.f * grey, 0.001f, 100.0f);
  const float grey_var = log2f(prev_grey / p->grey_point_source);
  p->black_point_source = p->black_point_source - grey_var;
  p->white_point_source = p->white_point_source + grey_var;
  p->output_power =  logf(p->grey_point_target / 100.0f) / logf(-p->black_point_source / (p->white_point_source - p->black_point_source));

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->grey_point_source, p->grey_point_source);
  dt_bauhaus_slider_set_soft(g->black_point_source, p->black_point_source);
  dt_bauhaus_slider_set_soft(g->white_point_source, p->white_point_source);
  dt_bauhaus_slider_set_soft(g->output_power, p->output_power);
  darktable.gui->reset = reset;

  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void apply_auto_black(dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  // Black
  const dt_iop_order_iccprofile_info_t *const work_profile
        = dt_ioppr_get_iop_work_profile_info(self, self->dev->iop);
  const float black = get_pixel_norm(self->picked_color_min, p->preserve_color, work_profile);

  float EVmin = CLAMP(log2f(black / (p->grey_point_source / 100.0f)), -16.0f, -1.0f);
  EVmin *= (1.0f + p->security_factor / 100.0f);

  p->black_point_source = fmaxf(EVmin, -16.0f);
  p->output_power =  logf(p->grey_point_target / 100.0f) / logf(-p->black_point_source / (p->white_point_source - p->black_point_source));

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->black_point_source, p->black_point_source);
  dt_bauhaus_slider_set_soft(g->output_power, p->output_power);
  darktable.gui->reset = reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}


static void apply_auto_white_point_source(dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  // White
  const dt_iop_order_iccprofile_info_t *const work_profile
        = dt_ioppr_get_iop_work_profile_info(self, self->dev->iop);
  const float white = get_pixel_norm(self->picked_color_max, p->preserve_color, work_profile);

  float EVmax = CLAMP(log2f(white / (p->grey_point_source / 100.0f)), 1.0f, 16.0f);
  EVmax *= (1.0f + p->security_factor / 100.0f);

  p->white_point_source = EVmax;
  p->output_power =  logf(p->grey_point_target / 100.0f) / logf(-p->black_point_source / (p->white_point_source - p->black_point_source));

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->white_point_source, p->white_point_source);
  dt_bauhaus_slider_set_soft(g->output_power, p->output_power);
  darktable.gui->reset = reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}

static void apply_autotune(dt_iop_module_t *self)
{
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  const dt_iop_order_iccprofile_info_t *const work_profile
        = dt_ioppr_get_iop_work_profile_info(self, self->dev->iop);

  // Grey
  const float grey = get_pixel_norm(self->picked_color, p->preserve_color, work_profile) / 2.0f;
  p->grey_point_source = CLAMP(100.f * grey, 0.001f, 100.0f);

  // White
  const float white = get_pixel_norm(self->picked_color_max, p->preserve_color, work_profile);
  float EVmax = CLAMP(log2f(white / (p->grey_point_source / 100.0f)), 1.0f, 16.0f);
  EVmax *= (1.0f + p->security_factor / 100.0f);

  // Black
  const float black = get_pixel_norm(self->picked_color_min, p->preserve_color, work_profile);
  float EVmin = CLAMP(log2f(black / (p->grey_point_source / 100.0f)), -16.0f, -1.0f);
  EVmin *= (1.0f + p->security_factor / 100.0f);

  p->black_point_source = fmaxf(EVmin, -16.0f);
  p->white_point_source = EVmax;
  p->output_power =  logf(p->grey_point_target / 100.0f) / logf(-p->black_point_source / (p->white_point_source - p->black_point_source));

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->grey_point_source, p->grey_point_source);
  dt_bauhaus_slider_set_soft(g->black_point_source, p->black_point_source);
  dt_bauhaus_slider_set_soft(g->white_point_source, p->white_point_source);
  dt_bauhaus_slider_set_soft(g->output_power, p->output_power);
  darktable.gui->reset = reset;

  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static int _iop_color_picker_get_set(dt_iop_module_t *self, GtkWidget *button)
{
  dt_iop_filmicrgb_gui_data_t *g =  (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  const int current_picker = g->color_picker.current_picker;

  g->color_picker.current_picker = DT_PICKPROFLOG_NONE;

  if(button == g->grey_point_source)
    g->color_picker.current_picker = DT_PICKPROFLOG_GREY_POINT;
  else if(button == g->black_point_source)
    g->color_picker.current_picker = DT_PICKPROFLOG_BLACK_POINT;
  else if(button == g->white_point_source)
    g->color_picker.current_picker = DT_PICKPROFLOG_WHITE_POINT;
  else if(button == g->auto_button)
    g->color_picker.current_picker = DT_PICKPROFLOG_AUTOTUNE;

  if (current_picker == g->color_picker.current_picker)
    return DT_COLOR_PICKER_ALREADY_SELECTED;
  else
    return g->color_picker.current_picker;
}

static void _iop_color_picker_apply(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  switch(g->color_picker.current_picker)
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
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  const int which_colorpicker = g->color_picker.current_picker;
  dt_bauhaus_widget_set_quad_active(g->grey_point_source, which_colorpicker == DT_PICKPROFLOG_GREY_POINT);
  dt_bauhaus_widget_set_quad_active(g->black_point_source, which_colorpicker == DT_PICKPROFLOG_BLACK_POINT);
  dt_bauhaus_widget_set_quad_active(g->white_point_source, which_colorpicker == DT_PICKPROFLOG_WHITE_POINT);
  dt_bauhaus_widget_set_quad_active(g->auto_button, which_colorpicker == DT_PICKPROFLOG_AUTOTUNE);
}

static void grey_point_source_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  float prev_grey = p->grey_point_source;
  p->grey_point_source = dt_bauhaus_slider_get(slider);

  float grey_var = log2f(prev_grey / p->grey_point_source);
  p->black_point_source = p->black_point_source - grey_var;
  p->white_point_source = p->white_point_source + grey_var;
  p->output_power =  logf(p->grey_point_target / 100.0f) / logf(-p->black_point_source / (p->white_point_source - p->black_point_source));

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->white_point_source, p->white_point_source);
  dt_bauhaus_slider_set_soft(g->black_point_source, p->black_point_source);
  dt_bauhaus_slider_set_soft(g->output_power, p->output_power);
  darktable.gui->reset = reset;

  dt_iop_color_picker_reset(self, TRUE);

  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void white_point_source_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  p->white_point_source = dt_bauhaus_slider_get(slider);
  p->output_power =  logf(p->grey_point_target / 100.0f) / logf(-p->black_point_source / (p->white_point_source - p->black_point_source));

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->output_power, p->output_power);
  darktable.gui->reset = reset;

  dt_iop_color_picker_reset(self, TRUE);
  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void black_point_source_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  p->black_point_source = dt_bauhaus_slider_get(slider);
  p->output_power =  logf(p->grey_point_target / 100.0f) / logf(-p->black_point_source / (p->white_point_source - p->black_point_source));

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->output_power, p->output_power);
  darktable.gui->reset = reset;

  dt_iop_color_picker_reset(self, TRUE);
  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void security_threshold_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  float previous = p->security_factor;
  p->security_factor = dt_bauhaus_slider_get(slider);
  float ratio = (p->security_factor - previous) / (previous + 100.0f);

  float EVmin = p->black_point_source;
  EVmin = EVmin + ratio * EVmin;

  float EVmax = p->white_point_source;
  EVmax = EVmax + ratio * EVmax;

  p->white_point_source = EVmax;
  p->black_point_source = EVmin;
  p->output_power =  logf(p->grey_point_target / 100.0f) / logf(-p->black_point_source / (p->white_point_source - p->black_point_source));

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->white_point_source, p->white_point_source);
  dt_bauhaus_slider_set_soft(g->black_point_source, p->black_point_source);
  dt_bauhaus_slider_set_soft(g->output_power, p->output_power);
  darktable.gui->reset = reset;

  dt_iop_color_picker_reset(self, TRUE);
  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void grey_point_target_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  p->grey_point_target = dt_bauhaus_slider_get(slider);
  dt_iop_color_picker_reset(self, TRUE);
  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void latitude_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  p->latitude = dt_bauhaus_slider_get(slider);
  dt_iop_color_picker_reset(self, TRUE);
  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void contrast_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  p->contrast = dt_bauhaus_slider_get(slider);
  dt_iop_color_picker_reset(self, TRUE);
  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void saturation_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  p->saturation = dt_bauhaus_slider_get(slider);
  dt_iop_color_picker_reset(self, TRUE);
  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void white_point_target_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  p->white_point_target = dt_bauhaus_slider_get(slider);
  dt_iop_color_picker_reset(self, TRUE);
  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void black_point_target_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  p->black_point_target = dt_bauhaus_slider_get(slider);
  dt_iop_color_picker_reset(self, TRUE);
  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void output_power_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  p->output_power = dt_bauhaus_slider_get(slider);
  dt_iop_color_picker_reset(self, TRUE);
  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void balance_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  p->balance = dt_bauhaus_slider_get(slider);
  dt_iop_color_picker_reset(self, TRUE);
  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void preserve_color_callback(GtkWidget *combo, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  p->preserve_color = dt_bauhaus_combobox_get(combo);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  if(!in) dt_iop_color_picker_reset(self, TRUE);
}


inline static void dt_iop_filmic_rgb_compute_spline(const dt_iop_filmicrgb_params_t *const p, struct dt_iop_filmic_rgb_spline_t *const spline)
{
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

  const float latitude = CLAMP(p->latitude, 0.0f, 100.0f) / 100.0f * dynamic_range; // in % of dynamic range
  const float balance = CLAMP(p->balance, -50.0f, 50.0f) / 100.0f; // in %
  const float contrast = CLAMP(p->contrast, 0.1f, 2.0f);

  // nodes for mapping from log encoding to desired target luminance
  // X coordinates
  float toe_log = grey_log - latitude/dynamic_range * fabsf(black_source/dynamic_range);
  float shoulder_log = grey_log + latitude/dynamic_range * fabsf(white_source/dynamic_range);

  // interception
  float linear_intercept = grey_display - (contrast * grey_log);

  // y coordinates
  float toe_display = (toe_log * contrast + linear_intercept);
  float shoulder_display = (shoulder_log * contrast + linear_intercept);

  // Apply the highlights/shadows balance as a shift along the contrast slope
  const float norm = sqrtf(contrast * contrast + 1.0f);

  // negative values drag to the left and compress the shadows, on the UI negative is the inverse
  const float coeff = -((2.0f * latitude) / dynamic_range) * balance;

  toe_display += coeff * contrast / norm;
  shoulder_display += coeff * contrast / norm;
  toe_log += coeff / norm;
  shoulder_log += coeff / norm;

  /**
   * Now we have 3 segments :
   *  - x = [0.0 ; toe_log], curved part
   *  - x = [toe_log ; grey_log ; shoulder_log], linear part
   *  - x = [shoulder_log ; 1.0] curved part
   *
   * BUT : in case some nodes overlap, we need to remove them to avoid
   * degenerating of the curve
  **/

  // Build the curve from the nodes
  spline->x[0] = black_log;
  spline->x[1] = toe_log;
  spline->x[2] = grey_log;
  spline->x[3] = shoulder_log;
  spline->x[4] = white_log;

  spline->y[0] = black_display;
  spline->y[1] = toe_display;
  spline->y[2] = grey_display;
  spline->y[3] = shoulder_display;
  spline->y[4] = white_display;

  spline->latitude_min = spline->x[1];
  spline->latitude_max = spline->x[3];

  /**
   * For background and details, see :
   * https://eng.aurelienpierre.com/2018/11/30/filmic-darktable-and-the-quest-of-the-hdr-tone-mapping/#filmic_s_curve
   *
   **/

  // solve the linear central part - linear function
  const float f = contrast;
  const float g = spline->y[1] - f * spline->x[1];

  // solve the toe part - fourth order polynom
  const double Tl = spline->x[1];
  const double Tl2 = Tl * Tl;
  const double Tl3 = Tl2 * Tl;
  const double Tl4 = Tl3 * Tl;

  double A0[25] = {0.,         0.,       0.,      0., 1., // position in 0
                   0.,         0.,       0.,      1., 0., // first derivative in 0
                   Tl4,        Tl3,      Tl2,     Tl, 1., // position at toe node
                   4. * Tl3,   3. * Tl2, 2. * Tl, 1., 0., // first derivative at toe node
                   12. * Tl2,  6. * Tl,  2.,      0., 0. }; // second derivative at toe node

  double b0[5] = { black_display, 0., spline->y[1], f, 0. };

  gauss_solve(A0, b0, 5);
  const float a = b0[0];
  const float b = b0[1];
  const float c = b0[2];
  const float d = b0[3];
  const float e = b0[4];

  // solve the shoulder part - 3rd order polynom
  const double Sl = spline->x[3];
  const double Sl2 = Sl * Sl;
  const double Sl3 = Sl2 * Sl;

  double A1[16] = { 1.,        1.,        1.,      1., // position in 1
                    Sl3,       Sl2,       Sl,      1., // position at shoulder node
                    3. * Sl2,  2. * Sl,   1.,      0., // first derivative at shoulder node
                    6. * Sl,   2.,        0.,      0. }; // second derivative at shoulder node

  double b1[4] = { white_display, spline->y[3], f, 0. };

  gauss_solve(A1, b1, 4);
  const float h = 0.0f;
  const float i = b1[0];
  const float j = b1[1];
  const float k = b1[2];
  const float l = b1[3];

  // offsets
  spline->M1[0] = e;
  spline->M1[1] = l;
  spline->M1[2] = g;

  // factors of x
  spline->M2[0] = d;
  spline->M2[1] = k;
  spline->M2[2] = f;

  // factors of x²
  spline->M3[0] = c;
  spline->M3[1] = j;
  spline->M3[2] = 0.f;

  // factors of x³
  spline->M4[0] = b;
  spline->M4[1] = i;
  spline->M4[2] = 0.f;

  // factors of x⁴
  spline->M5[0] = a;
  spline->M5[1] = h;
  spline->M5[2] = 0.f;
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)p1;
  dt_iop_filmicrgb_data_t *d = (dt_iop_filmicrgb_data_t *)piece->data;

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

  // commit
  d->dynamic_range = dynamic_range;
  d->black_source = black_source;
  d->grey_source = grey_source;
  d->output_power = p->output_power;
  d->contrast = contrast;

  // compute the curves and their LUT
  dt_iop_filmic_rgb_compute_spline(p, &d->spline);

  d->saturation = (2.0f * p->saturation / 100.0f + 1.0f);
  d->sigma_toe = powf(d->spline.latitude_min / 3.0f, 2.0f);
  d->sigma_shoulder = powf((1.0f - d->spline.latitude_max) / 3.0f, 2.0f);
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_filmicrgb_data_t));
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
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)module->params;

  dt_iop_color_picker_reset(self, TRUE);

  self->color_picker_box[0] = self->color_picker_box[1] = .25f;
  self->color_picker_box[2] = self->color_picker_box[3] = .50f;
  self->color_picker_point[0] = self->color_picker_point[1] = 0.5f;

  dt_bauhaus_slider_set_soft(g->white_point_source, p->white_point_source);
  dt_bauhaus_slider_set_soft(g->grey_point_source, p->grey_point_source);
  dt_bauhaus_slider_set_soft(g->black_point_source, p->black_point_source);
  dt_bauhaus_slider_set_soft(g->security_factor, p->security_factor);
  dt_bauhaus_slider_set_soft(g->white_point_target, p->white_point_target);
  dt_bauhaus_slider_set_soft(g->grey_point_target, p->grey_point_target);
  dt_bauhaus_slider_set_soft(g->black_point_target, p->black_point_target);
  dt_bauhaus_slider_set_soft(g->output_power, p->output_power);
  dt_bauhaus_slider_set_soft(g->latitude, p->latitude);
  dt_bauhaus_slider_set(g->contrast, p->contrast);
  dt_bauhaus_slider_set(g->saturation, p->saturation);
  dt_bauhaus_slider_set(g->balance, p->balance);

  dt_bauhaus_combobox_set(g->preserve_color, p->preserve_color);

  gtk_widget_queue_draw(self->widget);

}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_filmicrgb_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_filmicrgb_params_t));
  module->default_enabled = 0;
  module->params_size = sizeof(dt_iop_filmicrgb_params_t);
  module->gui_data = NULL;

  dt_iop_filmicrgb_params_t tmp
    = (dt_iop_filmicrgb_params_t){
                                 .grey_point_source   = 9.225, // source grey
                                 .black_point_source  = -10.55f,  // source black
                                 .white_point_source  = 3.45f,  // source white
                                 .security_factor     = 0.0f,
                                 .grey_point_target   = 18.45f, // target grey
                                 .black_point_target  = 0.0,  // target black
                                 .white_point_target  = 100.0,  // target white
                                 .output_power        = 5.98f,  // target power (~ gamma)
                                 .latitude            = 45.0f,  // intent latitude
                                 .contrast            = 1.30f,  // intent contrast
                                 .saturation          = 5.0f,   // intent saturation
                                 .balance             = 12.0f, // balance shadows/highlights
                                 .preserve_color      = DT_FILMIC_METHOD_POWER_NORM // run the saturated variant
                              };
  memcpy(module->params, &tmp, sizeof(dt_iop_filmicrgb_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_filmicrgb_params_t));
}


void init_global(dt_iop_module_so_t *module)
{
  const int program = 22; // filmic.cl, from programs.conf
  dt_iop_filmicrgb_global_data_t *gd
      = (dt_iop_filmicrgb_global_data_t *)malloc(sizeof(dt_iop_filmicrgb_global_data_t));

  module->data = gd;
  gd->kernel_filmic_rgb_split = dt_opencl_create_kernel(program, "filmicrgb_split");
  gd->kernel_filmic_rgb_chroma = dt_opencl_create_kernel(program, "filmicrgb_chroma");
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
  free(module->default_params);
  module->default_params = NULL;
}


void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_filmicrgb_global_data_t *gd = (dt_iop_filmicrgb_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_filmic_rgb_split);
  dt_opencl_free_kernel(gd->kernel_filmic_rgb_chroma);
  free(module->data);
  module->data = NULL;
}


void gui_reset(dt_iop_module_t *self)
{
  dt_iop_color_picker_reset(self, TRUE);
}

static gboolean dt_iop_tonecurve_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  dt_iop_filmic_rgb_compute_spline(p, &g->spline);

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

  // draw identity line
  cairo_move_to(cr, 0, height);
  cairo_line_to(cr, width, 0);
  cairo_stroke(cr);

  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
  cairo_set_line_cap  (cr, CAIRO_LINE_CAP_ROUND);

  // Draw the saturation curve
  const float saturation = (2.0f * p->saturation / 100.0f + 1.0f);
  const float sigma_toe = powf(g->spline.latitude_min / 3.0f, 2.0f);
  const float sigma_shoulder = powf((1.0f - g->spline.latitude_max) / 3.0f, 2.0f);

  cairo_set_source_rgb(cr, .5, .5, .5);
  cairo_move_to(cr, 0, height * (1.0 - filmic_desaturate(0.0f, sigma_toe, sigma_shoulder, saturation)));
  for(int k = 1; k < 256; k++)
  {
    const float x = k / 255.0;
    float y = filmic_desaturate(x, sigma_toe, sigma_shoulder, saturation);
    cairo_line_to(cr, x * width, height * (1.0 - y));
  }
  cairo_stroke(cr);

  // draw the tone curve
  cairo_move_to(cr, 0, height * (1.0 - filmic_spline(0.0f, g->spline.M1, g->spline.M2, g->spline.M3, g->spline.M4, g->spline.M5, g->spline.latitude_min, g->spline.latitude_max)));

  for(int k = 1; k < 256; k++)
  {
    const float x = k / 255.0;
    float y = filmic_spline(x, g->spline.M1, g->spline.M2, g->spline.M3, g->spline.M4, g->spline.M5, g->spline.latitude_min, g->spline.latitude_max);

    if(y > 1.0f)
    {
      y = 1.0f;
      cairo_set_source_rgb(cr, 0.75, .5, 0.);
    }
    else if(y < 0.0f)
    {
      y = 0.0f;
      cairo_set_source_rgb(cr, 0.75, .5, 0.);
    }
    else
    {
      cairo_set_source_rgb(cr, .9, .9, .9);
    }

    cairo_line_to(cr, x * width, height * (1.0 - y));
    cairo_stroke(cr);
    cairo_move_to(cr, x * width, height * (1.0 - y));
  }

  // draw nodes

  // special case for the grey node
  cairo_set_source_rgb(cr, 0.75, 0.5, 0.0);
  cairo_arc(cr, g->spline.x[2] * width, (1.0 - g->spline.y[2]) * height, DT_PIXEL_APPLY_DPI(6), 0, 2. * M_PI);
  cairo_fill(cr);
  cairo_stroke(cr);

  // latitude nodes
  cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
  for(int k = 0; k < 5; k++)
  {
    if(k != 2)
    {
      const float x = g->spline.x[k];
      const float y = g->spline.y[k];
      cairo_arc(cr, x * width, (1.0 - y) * height, DT_PIXEL_APPLY_DPI(4), 0, 2. * M_PI);
      cairo_fill(cr);
      cairo_stroke(cr);
    }
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
  self->gui_data = malloc(sizeof(dt_iop_filmicrgb_gui_data_t));
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));

  // don't make the area square to safe some vertical space -- it's not interactive anyway
  g->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(0.618));
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->area), _("read-only graph, use the parameters below to set the nodes\n"
                                                     "the bright curve is the filmic tone mapping curve\n"
                                                     "the dark curve is the desaturation curve\n"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->area), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->area), "draw", G_CALLBACK(dt_iop_tonecurve_draw), self);

  // Init GTK notebook
  g->notebook = GTK_NOTEBOOK(gtk_notebook_new());
  GtkWidget *page1 = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  GtkWidget *page2 = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  GtkWidget *page3 = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));

  gtk_notebook_append_page(GTK_NOTEBOOK(g->notebook), page1, gtk_label_new(_("scene")));
  gtk_notebook_append_page(GTK_NOTEBOOK(g->notebook), page2, gtk_label_new(_("look")));
  gtk_notebook_append_page(GTK_NOTEBOOK(g->notebook), page3, gtk_label_new(_("display")));
  gtk_widget_show_all(GTK_WIDGET(gtk_notebook_get_nth_page(g->notebook, 0)));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->notebook), FALSE, FALSE, 0);

  dtgtk_justify_notebook_tabs(g->notebook);

  // grey_point_source slider
  g->grey_point_source = dt_bauhaus_slider_new_with_range(self, 0.1, 36., 0.1, p->grey_point_source, 2);
  dt_bauhaus_slider_enable_soft_boundaries(g->grey_point_source, 0.0, 100.0);
  dt_bauhaus_widget_set_label(g->grey_point_source, NULL, _("middle grey luminance"));
  gtk_box_pack_start(GTK_BOX(page1), g->grey_point_source, FALSE, FALSE, 0);
  dt_bauhaus_slider_set_format(g->grey_point_source, "%.2f %%");
  gtk_widget_set_tooltip_text(g->grey_point_source, _("adjust to match the average luminance of the image's subject.\n"
                                                      "the value entered here will then be remapped to 18.45%.\n"
                                                      "decrease the value to increase the overall brightness."));
  g_signal_connect(G_OBJECT(g->grey_point_source), "value-changed", G_CALLBACK(grey_point_source_callback), self);
  dt_bauhaus_widget_set_quad_paint(g->grey_point_source, dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->grey_point_source, TRUE);
  g_signal_connect(G_OBJECT(g->grey_point_source), "quad-pressed", G_CALLBACK(dt_iop_color_picker_callback), &g->color_picker);

  // White slider
  g->white_point_source = dt_bauhaus_slider_new_with_range(self, 2.0, 8.0, 0.1, p->white_point_source, 2);
  dt_bauhaus_slider_enable_soft_boundaries(g->white_point_source, 0.0, 16.0);
  dt_bauhaus_widget_set_label(g->white_point_source, NULL, _("white relative exposure"));
  gtk_box_pack_start(GTK_BOX(page1), g->white_point_source, FALSE, FALSE, 0);
  dt_bauhaus_slider_set_format(g->white_point_source, _("%+.2f EV"));
  gtk_widget_set_tooltip_text(g->white_point_source, _("number of stops between middle grey and pure white.\n"
                                                       "this is a reading a lightmeter would give you on the scene.\n"
                                                       "adjust so highlights clipping is avoided"));
  g_signal_connect(G_OBJECT(g->white_point_source), "value-changed", G_CALLBACK(white_point_source_callback), self);
  dt_bauhaus_widget_set_quad_paint(g->white_point_source, dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->white_point_source, TRUE);
  g_signal_connect(G_OBJECT(g->white_point_source), "quad-pressed", G_CALLBACK(dt_iop_color_picker_callback), &g->color_picker);

  // Black slider
  g->black_point_source = dt_bauhaus_slider_new_with_range(self, -14.0, -3.0, 0.1, p->black_point_source, 2);
  dt_bauhaus_slider_enable_soft_boundaries(g->black_point_source, -16.0, -0.1);
  dt_bauhaus_widget_set_label(g->black_point_source, NULL, _("black relative exposure"));
  gtk_box_pack_start(GTK_BOX(page1), g->black_point_source, FALSE, FALSE, 0);
  dt_bauhaus_slider_set_format(g->black_point_source, _("%+.2f EV"));
  gtk_widget_set_tooltip_text(g->black_point_source, _("number of stops between middle grey and pure black.\n"
                                                       "this is a reading a lightmeter would give you on the scene.\n"
                                                       "increase to get more contrast.\ndecrease to recover more details in low-lights."));
  g_signal_connect(G_OBJECT(g->black_point_source), "value-changed", G_CALLBACK(black_point_source_callback), self);
  dt_bauhaus_widget_set_quad_paint(g->black_point_source, dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->black_point_source, TRUE);
  g_signal_connect(G_OBJECT(g->black_point_source), "quad-pressed", G_CALLBACK(dt_iop_color_picker_callback), &g->color_picker);

  // Security factor
  g->security_factor = dt_bauhaus_slider_new_with_range(self, -50., 50., 1.0, p->security_factor, 2);
  dt_bauhaus_slider_enable_soft_boundaries(g->security_factor, -50, 200);
  dt_bauhaus_widget_set_label(g->security_factor, NULL, _("dynamic range scaling"));
  gtk_box_pack_start(GTK_BOX(page1), g->security_factor, FALSE, FALSE, 0);
  dt_bauhaus_slider_set_format(g->security_factor, "%+.2f %%");
  gtk_widget_set_tooltip_text(g->security_factor, _("symmetrically enlarge or shrink the computed dynamic range.\n"
                                                    "useful to give a safety margin to extreme luminances."));
  g_signal_connect(G_OBJECT(g->security_factor), "value-changed", G_CALLBACK(security_threshold_callback), self);

  // Auto tune slider
  g->auto_button = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->auto_button, NULL, _("auto tune levels"));
  dt_bauhaus_widget_set_quad_paint(g->auto_button, dtgtk_cairo_paint_colorpicker,
                                   CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->auto_button, TRUE);
  g_signal_connect(G_OBJECT(g->auto_button), "quad-pressed", G_CALLBACK(dt_iop_color_picker_callback), &g->color_picker);
  gtk_widget_set_tooltip_text(g->auto_button, _("try to optimize the settings with some statistical assumptions.\n"
                                                "this will fit the luminance range inside the histogram bounds.\n"
                                                "works better for landscapes and evenly-lit pictures\n"
                                                "but fails for high-keys, low-keys and high-ISO pictures.\n"
                                                "this is not an artificial intelligence, but a simple guess.\n"
                                                "ensure you understand its assumptions before using it."));
  gtk_box_pack_start(GTK_BOX(page1), g->auto_button, FALSE, FALSE, 0);


  // contrast slider
  g->contrast = dt_bauhaus_slider_new_with_range(self, 1., 2., 0.01, p->contrast, 3);
  dt_bauhaus_slider_enable_soft_boundaries(g->contrast, 0.0, 5.0);
  dt_bauhaus_widget_set_label(g->contrast, NULL, _("contrast"));
  gtk_box_pack_start(GTK_BOX(page2), g->contrast, FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(g->contrast, _("slope of the linear part of the curve\n"
                                             "affects mostly the mid-tones"));
  g_signal_connect(G_OBJECT(g->contrast), "value-changed", G_CALLBACK(contrast_callback), self);

  // latitude slider
  g->latitude = dt_bauhaus_slider_new_with_range(self, 5.0, 45.0, 1.0, p->latitude, 2);
  dt_bauhaus_slider_enable_soft_boundaries(g->latitude, 0.01, 100.0);
  dt_bauhaus_widget_set_label(g->latitude, NULL, _("latitude"));
  dt_bauhaus_slider_set_format(g->latitude, "%.2f %%");
  gtk_box_pack_start(GTK_BOX(page2), g->latitude, FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(g->latitude, _("width of the linear domain in the middle of the curve,\n"
                                             "in percent of the dynamic range (white exposure - black exposure).\n"
                                             "increase to get more contrast and less desaturation at extreme luminances,\n"
                                             "decrease otherwise. no desaturation happens in the latitude range.\n"
                                             "this has no effect on mid-tones."));
  g_signal_connect(G_OBJECT(g->latitude), "value-changed", G_CALLBACK(latitude_callback), self);

  // balance slider
  g->balance = dt_bauhaus_slider_new_with_range(self, -50., 50., 1.0, p->balance, 2);
  dt_bauhaus_widget_set_label(g->balance, NULL, _("shadows/highlights balance"));
  gtk_box_pack_start(GTK_BOX(page2), g->balance, FALSE, FALSE, 0);
  dt_bauhaus_slider_set_format(g->balance, "%.2f %%");
  gtk_widget_set_tooltip_text(g->balance, _("slides the latitude along the slope\n"
                                            "to give more room to shadows or highlights.\n"
                                            "use it if you need to protect the details\n"
                                            "at one extremity of the histogram."));
  g_signal_connect(G_OBJECT(g->balance), "value-changed", G_CALLBACK(balance_callback), self);

  // saturation slider
  g->saturation = dt_bauhaus_slider_new_with_range(self, -50., 50., 0.5, p->saturation, 2);
  dt_bauhaus_widget_set_label(g->saturation, NULL, _("extreme luminance saturation"));
  dt_bauhaus_slider_enable_soft_boundaries(g->saturation, -50, 200.0);
  dt_bauhaus_slider_set_format(g->saturation, "%.2f %%");
  gtk_box_pack_start(GTK_BOX(page2), g->saturation, FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(g->saturation, _("desaturates the output of the module\n"
                                               "specifically at extreme luminances.\n"
                                               "increase if shadows and/or highlights are under-saturated."));
  g_signal_connect(G_OBJECT(g->saturation), "value-changed", G_CALLBACK(saturation_callback), self);


  // Preserve color
  g->preserve_color = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->preserve_color, NULL, _("preserve chrominance"));
  dt_bauhaus_combobox_add(g->preserve_color, _("no"));
  dt_bauhaus_combobox_add(g->preserve_color, _("max RGB"));
  dt_bauhaus_combobox_add(g->preserve_color, _("luminance Y"));
  dt_bauhaus_combobox_add(g->preserve_color, _("RGB power norm"));
  gtk_widget_set_tooltip_text(g->preserve_color, _("ensure the original color are preserved.\n"
                                                   "may reinforce chromatic aberrations and chroma noise,\n"
                                                   "so ensure they are properly corrected elsewhere.\n"));
  gtk_box_pack_start(GTK_BOX(page2), g->preserve_color , FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->preserve_color), "value-changed", G_CALLBACK(preserve_color_callback), self);

  // Black slider
  g->black_point_target = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 1, p->black_point_target, 2);
  dt_bauhaus_widget_set_label(g->black_point_target, NULL, _("target black luminance"));
  gtk_box_pack_start(GTK_BOX(page3), g->black_point_target, FALSE, FALSE, 0);
  dt_bauhaus_slider_set_format(g->black_point_target, "%.2f %%");
  gtk_widget_set_tooltip_text(g->black_point_target, _("luminance of output pure black, "
                                                        "this should be 0%\nexcept if you want a faded look"));
  g_signal_connect(G_OBJECT(g->black_point_target), "value-changed", G_CALLBACK(black_point_target_callback), self);

  // grey_point_source slider
  g->grey_point_target = dt_bauhaus_slider_new_with_range(self, 0.1, 50., 0.5, p->grey_point_target, 2);
  dt_bauhaus_widget_set_label(g->grey_point_target, NULL, _("target middle grey"));
  gtk_box_pack_start(GTK_BOX(page3), g->grey_point_target, FALSE, FALSE, 0);
  dt_bauhaus_slider_set_format(g->grey_point_target, "%.2f %%");
  gtk_widget_set_tooltip_text(g->grey_point_target, _("midde grey value of the target display or color space.\n"
                                                      "you should never touch that unless you know what you are doing."));
  g_signal_connect(G_OBJECT(g->grey_point_target), "value-changed", G_CALLBACK(grey_point_target_callback), self);

  // White slider
  g->white_point_target = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 1., p->white_point_target, 2);
  dt_bauhaus_widget_set_label(g->white_point_target, NULL, _("target white luminance"));
  gtk_box_pack_start(GTK_BOX(page3), g->white_point_target, FALSE, FALSE, 0);
  dt_bauhaus_slider_set_format(g->white_point_target, "%.2f %%");
  gtk_widget_set_tooltip_text(g->white_point_target, _("luminance of output pure white, "
                                                        "this should be 100%\nexcept if you want a faded look"));
  g_signal_connect(G_OBJECT(g->white_point_target), "value-changed", G_CALLBACK(white_point_target_callback), self);

  // power/gamma slider
  g->output_power = dt_bauhaus_slider_new_with_range(self, 1.0, 10.0, 0.1, p->output_power, 2);
  dt_bauhaus_widget_set_label(g->output_power, NULL, _("target power transfer function"));
  gtk_box_pack_start(GTK_BOX(page3), g->output_power, FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(g->output_power, _("power or gamma of the transfer function\nof the display or color space.\n"
                                                 "you should never touch that unless you know what you are doing."));
  g_signal_connect(G_OBJECT(g->output_power), "value-changed", G_CALLBACK(output_power_callback), self);

  dt_iop_init_picker(&g->color_picker,
              self,
              DT_COLOR_PICKER_AREA,
              _iop_color_picker_get_set,
              _iop_color_picker_apply,
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
